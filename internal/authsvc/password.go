package authsvc

import (
	"context"
	"database/sql"
	"errors"
	"strings"
	"sync"
	"time"
	"unicode"
	"unicode/utf8"

	"golang.org/x/crypto/bcrypt"
)

const accountTokenTTL = 24 * time.Hour
const accountTokenLimit = 5

// 不存在的账号也执行同等成本的密码比较，不直接暴露账号存在性。
const dummyPasswordHash = "$2a$10$N9qo8uLOickgx2ZMRZoMyeIjZAgcfl7p92ldGxad68LJZdL17lhWy"

type accountTicket struct {
	RoleID    string `json:"role_id"`
	Name      string `json:"name"`
	Token     string `json:"token"`
	ExpiresAt int64  `json:"expires_at_unix_ms"`
}

type accountBucket struct {
	count int
	until time.Time
}
type accountLimiter struct {
	mu     sync.Mutex
	global accountBucket
	names  map[string]accountBucket
}

func (l *accountLimiter) allow(name string, now time.Time) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	if !now.Before(l.global.until) {
		l.global = accountBucket{until: now.Add(time.Minute)}
	}
	if l.names == nil {
		l.names = make(map[string]accountBucket)
	}
	if len(l.names) >= 1024 {
		for key, bucket := range l.names {
			if !now.Before(bucket.until) {
				delete(l.names, key)
			}
		}
	}
	key := hash(name)
	bucket, found := l.names[key]
	if !now.Before(bucket.until) {
		bucket = accountBucket{until: now.Add(time.Minute)}
	}
	if l.global.count >= 60 || bucket.count >= 6 || (!found && len(l.names) >= 1024) {
		return false
	}
	l.global.count++
	bucket.count++
	l.names[key] = bucket
	return true
}

func (s *Server) createPasswordTables(ctx context.Context) error {
	statements := []string{
		`CREATE TABLE IF NOT EXISTS auth_passwords (role_id TEXT PRIMARY KEY, password_hash TEXT NOT NULL)`,
		`CREATE TABLE IF NOT EXISTS auth_login_tokens (token_hash TEXT PRIMARY KEY, role_id TEXT NOT NULL, created_ms INTEGER NOT NULL, expires_ms INTEGER NOT NULL)`,
		`CREATE INDEX IF NOT EXISTS auth_login_tokens_role ON auth_login_tokens(role_id,created_ms)`,
		`CREATE INDEX IF NOT EXISTS auth_login_tokens_expiry ON auth_login_tokens(expires_ms)`,
	}
	if s.config.Driver == "mysql" {
		statements = []string{
			`CREATE TABLE IF NOT EXISTS auth_passwords (role_id VARBINARY(128) PRIMARY KEY, password_hash VARBINARY(128) NOT NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4`,
			`CREATE TABLE IF NOT EXISTS auth_login_tokens (token_hash BINARY(64) PRIMARY KEY, role_id VARBINARY(128) NOT NULL, created_ms BIGINT NOT NULL, expires_ms BIGINT NOT NULL, INDEX auth_login_tokens_role(role_id,created_ms), INDEX auth_login_tokens_expiry(expires_ms)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4`,
		}
	}
	for _, statement := range statements {
		if _, err := s.conn.ExecContext(ctx, statement); err != nil {
			return err
		}
	}
	if s.config.Driver == "mysql" {
		for _, table := range []string{"auth_passwords", "auth_login_tokens"} {
			var engine string
			if err := s.conn.QueryRowContext(ctx, "SELECT ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME=?", table).Scan(&engine); err != nil {
				return err
			}
			if !strings.EqualFold(engine, "InnoDB") {
				return errors.New("账号密码表必须使用 InnoDB")
			}
		}
	}
	return nil
}

// 调用方持有 s.mu。长期旧凭据保持不变，新密码登录使用独立、可过期的票据。
func (s *Server) authenticate(ctx context.Context, token string) (Identity, error) {
	var identity Identity
	if len(token) != 64 {
		return identity, &failure{401, "登录token无效"}
	}
	err := s.conn.QueryRowContext(ctx, "SELECT role_id,name,token_hash FROM auth_accounts WHERE token_hash=?", hash(token)).Scan(&identity.RoleID, &identity.Name, &identity.LegacyTokenHash)
	if errors.Is(err, sql.ErrNoRows) {
		err = s.conn.QueryRowContext(ctx, `SELECT a.role_id,a.name,a.token_hash FROM auth_login_tokens t JOIN auth_accounts a ON a.role_id=t.role_id WHERE t.token_hash=? AND t.expires_ms>?`, hash(token), time.Now().UnixMilli()).Scan(&identity.RoleID, &identity.Name, &identity.LegacyTokenHash)
	}
	if errors.Is(err, sql.ErrNoRows) {
		return identity, &failure{401, "登录token无效或已过期"}
	}
	return identity, err
}

func validatePassword(password string) error {
	if !utf8.ValidString(password) || len(password) < 8 || len(password) > 72 {
		return &failure{400, "密码需要8到72字节的UTF-8文本"}
	}
	for _, r := range password {
		if unicode.IsControl(r) {
			return &failure{400, "密码不能包含控制字符"}
		}
	}
	return nil
}

func (s *Server) passwordRequest(ctx context.Context, op string, raw []byte) (any, error) {
	var request struct {
		Name     string `json:"name,omitempty"`
		Password string `json:"password"`
		Token    string `json:"token,omitempty"`
	}
	if err := parse(raw, &request); err != nil {
		return nil, err
	}
	if err := validatePassword(request.Password); err != nil {
		return nil, err
	}
	name := request.Name
	if op == "account_bind" {
		if name != "" || len(request.Token) != 64 {
			return nil, &failure{400, "绑定密码需要原登录token，不能提交name"}
		}
	} else {
		if request.Token != "" || name != strings.TrimSpace(name) || !utf8.ValidString(name) || utf8.RuneCountInString(name) < 1 || utf8.RuneCountInString(name) > 24 {
			return nil, &failure{400, "账号名需要1到24个字符，且不能包含首尾空格"}
		}
		for _, r := range name {
			if unicode.IsControl(r) {
				return nil, &failure{400, "账号名不能包含控制字符"}
			}
		}
	}
	limitKey := name
	if op == "account_bind" {
		limitKey = "bind:" + hash(request.Token)
	}
	if !s.loginLimits.allow(limitKey, time.Now()) {
		return nil, &failure{429, "账号请求过于频繁，请稍后重试"}
	}
	select {
	case s.passwordSlots <- struct{}{}:
		defer func() { <-s.passwordSlots }()
	default:
		return nil, &failure{429, "登录服务繁忙，请稍后重试"}
	}
	if ctx.Err() != nil {
		return nil, ctx.Err()
	}
	if op == "account_login" {
		return s.passwordLogin(ctx, name, request.Password)
	}
	encoded, err := bcrypt.GenerateFromPassword([]byte(request.Password), bcrypt.DefaultCost)
	if err != nil {
		return nil, err
	}
	if ctx.Err() != nil {
		return nil, ctx.Err()
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if op == "account_bind" {
		identity, err := s.authenticate(ctx, request.Token)
		if err != nil {
			return nil, err
		}
		var count int
		if err = s.conn.QueryRowContext(ctx, "SELECT COUNT(*) FROM auth_passwords WHERE role_id=?", identity.RoleID).Scan(&count); err != nil {
			return nil, err
		}
		if count != 0 {
			return nil, &failure{409, "账号已设置密码，本接口不能覆盖已有密码"}
		}
		_, err = s.conn.ExecContext(ctx, "INSERT INTO auth_passwords(role_id,password_hash) VALUES(?,?)", identity.RoleID, string(encoded))
		return map[string]any{"bound": true, "role_id": identity.RoleID, "name": identity.Name}, err
	}
	tx, err := s.conn.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	var count int
	if err = tx.QueryRowContext(ctx, "SELECT COUNT(*) FROM auth_accounts WHERE name=?", name).Scan(&count); err != nil {
		return nil, err
	}
	if count != 0 {
		return nil, &failure{409, "账号名已被注册"}
	}
	id, err := randomHex(16)
	if err != nil {
		return nil, err
	}
	unusedLegacySecret, err := randomHex(32)
	if err != nil {
		return nil, err
	}
	if _, err = tx.ExecContext(ctx, "INSERT INTO auth_accounts(role_id,name,token_hash) VALUES(?,?,?)", id, name, hash(unusedLegacySecret)); err != nil {
		return nil, err
	}
	if _, err = tx.ExecContext(ctx, "INSERT INTO auth_passwords(role_id,password_hash) VALUES(?,?)", id, string(encoded)); err != nil {
		return nil, err
	}
	ticket, err := issueAccountTicket(ctx, tx, id, name)
	if err != nil {
		return nil, err
	}
	return ticket, tx.Commit()
}

func (s *Server) passwordLogin(ctx context.Context, name, password string) (any, error) {
	var id, encoded string
	s.mu.Lock()
	err := s.conn.QueryRowContext(ctx, `SELECT a.role_id,p.password_hash FROM auth_accounts a JOIN auth_passwords p ON p.role_id=a.role_id WHERE a.name=?`, name).Scan(&id, &encoded)
	s.mu.Unlock()
	if err != nil && !errors.Is(err, sql.ErrNoRows) {
		return nil, err
	}
	found := err == nil
	if !found {
		encoded = dummyPasswordHash
	}
	if bcrypt.CompareHashAndPassword([]byte(encoded), []byte(password)) != nil || !found {
		return nil, &failure{401, "账号或密码错误"}
	}
	if ctx.Err() != nil {
		return nil, ctx.Err()
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.conn.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	ticket, err := issueAccountTicket(ctx, tx, id, name)
	if err != nil {
		return nil, err
	}
	return ticket, tx.Commit()
}

func issueAccountTicket(ctx context.Context, tx *sql.Tx, id, name string) (accountTicket, error) {
	now := time.Now().UnixMilli()
	ticket := accountTicket{RoleID: id, Name: name, ExpiresAt: now + accountTokenTTL.Milliseconds()}
	var err error
	ticket.Token, err = randomHex(32)
	if err != nil {
		return ticket, err
	}
	rows, err := tx.QueryContext(ctx, "SELECT token_hash FROM auth_login_tokens WHERE role_id=? ORDER BY created_ms ASC,token_hash ASC", id)
	if err != nil {
		return ticket, err
	}
	var older []string
	for rows.Next() {
		var h string
		if err = rows.Scan(&h); err != nil {
			rows.Close()
			return ticket, err
		}
		older = append(older, h)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return ticket, err
	}
	if len(older) >= accountTokenLimit {
		// 每账号固定最多五条记录，复用最早记录，兼容没有 DELETE 权限的部署账号。
		// 主键和时间一并替换；旧票据立即失效，新票据仍只有哈希落库。
		_, err = tx.ExecContext(ctx, "UPDATE auth_login_tokens SET token_hash=?,created_ms=?,expires_ms=? WHERE token_hash=? AND role_id=?", hash(ticket.Token), now, ticket.ExpiresAt, older[0], id)
		return ticket, err
	}
	_, err = tx.ExecContext(ctx, "INSERT INTO auth_login_tokens(token_hash,role_id,created_ms,expires_ms) VALUES(?,?,?,?)", hash(ticket.Token), id, now, ticket.ExpiresAt)
	return ticket, err
}
