package authsvc

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode"
	"unicode/utf8"

	"github.com/go-sql-driver/mysql"
	_ "modernc.org/sqlite"
)

type Config struct {
	Token, Driver, Path                       string
	Host, Port, User, Password, Database, TLS string
}

type Identity struct {
	RoleID string `json:"role_id"`
	Name   string `json:"name"`
}

// 账号凭据由 Auth 单独写入；角色、会话、战斗与经济仍由游戏服务管理。
type Server struct {
	mu       sync.Mutex
	pool     *sql.DB
	conn     *sql.Conn
	config   Config
	imported int
}

func New(c Config) (*Server, error) {
	if len(c.Token) < 32 {
		return nil, errors.New("INTERNAL_TOKEN 至少需要32字符")
	}
	c.Driver = strings.ToLower(strings.TrimSpace(c.Driver))
	c.Host = strings.TrimSpace(c.Host)
	if c.Driver == "" {
		c.Driver = "mysql"
	}
	var pool *sql.DB
	var err error
	switch c.Driver {
	case "sqlite":
		if c.Path == "" {
			return nil, errors.New("AUTH_DB 不能为空")
		}
		if err = os.MkdirAll(filepath.Dir(c.Path), 0700); err != nil {
			return nil, err
		}
		pool, err = sql.Open("sqlite", c.Path)
	case "mysql":
		if c.Host == "" || c.User == "" || c.Database == "" || strings.ContainsAny(c.Host, "/@?#\\ \t\r\n") || (strings.Contains(c.Host, ":") && net.ParseIP(c.Host) == nil) {
			return nil, errors.New("MySQL 地址、账号或数据库配置无效")
		}
		if c.Port == "" {
			c.Port = "3306"
		}
		port, e := strconv.Atoi(c.Port)
		if e != nil || port < 1 || port > 65535 {
			return nil, errors.New("MYSQL_PORT 必须为1..65535")
		}
		if c.TLS == "" {
			c.TLS = "false"
		}
		if c.TLS != "false" && c.TLS != "true" {
			return nil, errors.New("MYSQL_TLS 只支持false或true")
		}
		cfg := mysql.NewConfig()
		cfg.Net, cfg.Addr, cfg.User, cfg.Passwd, cfg.DBName = "tcp", net.JoinHostPort(c.Host, c.Port), c.User, c.Password, c.Database
		cfg.TLSConfig, cfg.RejectReadOnly = c.TLS, true
		cfg.Timeout, cfg.ReadTimeout, cfg.WriteTimeout = 5*time.Second, 5*time.Second, 5*time.Second
		if err = cfg.Apply(mysql.Charset("utf8mb4", "")); err != nil {
			return nil, err
		}
		connector, e := mysql.NewConnector(cfg)
		if e != nil {
			return nil, e
		}
		pool = sql.OpenDB(connector)
	default:
		return nil, errors.New("AUTH_DB_DRIVER 只支持mysql或sqlite")
	}
	if err != nil {
		return nil, err
	}
	pool.SetMaxOpenConns(1)
	pool.SetMaxIdleConns(0)
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	conn, err := pool.Conn(ctx)
	if err != nil {
		pool.Close()
		return nil, err
	}
	s := &Server{pool: pool, conn: conn, config: c}
	fail := func(e error) (*Server, error) { s.Close(); return nil, e }
	if c.Driver == "mysql" {
		// 与 Game Backend 的锁名不同，两个服务只写各自拥有的表。
		digest := sha256.Sum256([]byte(strings.ToLower(c.Database)))
		var locked sql.NullInt64
		if err = conn.QueryRowContext(ctx, "SELECT GET_LOCK(?,0)", fmt.Sprintf("mmo-auth:%x", digest[:24])).Scan(&locked); err != nil {
			return fail(err)
		}
		if !locked.Valid || locked.Int64 != 1 {
			return fail(errors.New("Auth 数据库已被其他鉴权进程占用"))
		}
		_, err = conn.ExecContext(ctx, `CREATE TABLE IF NOT EXISTS auth_accounts (
		 role_id VARBINARY(128) PRIMARY KEY, name VARBINARY(96) NOT NULL UNIQUE,
		 token_hash BINARY(64) NOT NULL UNIQUE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4`)
	} else {
		for _, statement := range []string{"PRAGMA journal_mode=WAL", "PRAGMA synchronous=FULL", "PRAGMA busy_timeout=5000", `CREATE TABLE IF NOT EXISTS auth_accounts (role_id TEXT PRIMARY KEY,name TEXT NOT NULL UNIQUE,token_hash TEXT NOT NULL UNIQUE)`} {
			if _, err = conn.ExecContext(ctx, statement); err != nil {
				break
			}
		}
	}
	if err != nil {
		return fail(err)
	}
	if c.Driver == "mysql" {
		var engine string
		if err = conn.QueryRowContext(ctx, "SELECT ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='auth_accounts'").Scan(&engine); err != nil {
			return fail(err)
		}
		if !strings.EqualFold(engine, "InnoDB") {
			return fail(errors.New("auth_accounts 必须使用 InnoDB"))
		}
	}
	if err = s.importLegacy(ctx); err != nil {
		return fail(err)
	}
	return s, nil
}

func (s *Server) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return errors.Join(s.conn.Close(), s.pool.Close())
}

func (s *Server) Health() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	return s.conn.PingContext(ctx)
}

func (s *Server) importLegacy(ctx context.Context) error {
	var exists int
	query := "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='roles'"
	if s.config.Driver == "mysql" {
		query = "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='roles'"
	}
	if err := s.conn.QueryRowContext(ctx, query).Scan(&exists); err != nil || exists == 0 {
		return err
	}
	rows, err := s.conn.QueryContext(ctx, "SELECT role_id,name,token_hash FROM roles")
	if err != nil {
		return err
	}
	type account struct{ id, name, hash string }
	var accounts []account
	for rows.Next() {
		var a account
		if err = rows.Scan(&a.id, &a.name, &a.hash); err != nil {
			rows.Close()
			return err
		}
		accounts = append(accounts, a)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return err
	}
	// 先关闭结果流，再在同一会话内迁移；遇到身份冲突整批回滚。
	tx, err := s.conn.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	for _, a := range accounts {
		var exact, conflict int
		if err = tx.QueryRowContext(ctx, "SELECT COUNT(*) FROM auth_accounts WHERE role_id=? AND name=? AND token_hash=?", a.id, a.name, a.hash).Scan(&exact); err != nil {
			return err
		}
		if exact == 1 {
			continue
		}
		if err = tx.QueryRowContext(ctx, "SELECT COUNT(*) FROM auth_accounts WHERE role_id=? OR name=? OR token_hash=?", a.id, a.name, a.hash).Scan(&conflict); err != nil {
			return err
		}
		if conflict != 0 {
			return errors.New("旧账号导入存在身份冲突；未覆盖现有账号")
		}
		if _, err = tx.ExecContext(ctx, "INSERT INTO auth_accounts(role_id,name,token_hash) VALUES(?,?,?)", a.id, a.name, a.hash); err != nil {
			return err
		}
		s.imported++
	}
	return tx.Commit()
}

type failure struct {
	status  int
	message string
}

func (e *failure) Error() string { return e.message }
func response(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}
func parse(raw []byte, target any) error {
	if len(raw) == 0 || bytes.Equal(bytes.TrimSpace(raw), []byte("null")) {
		return &failure{400, "请求必须是JSON对象"}
	}
	d := json.NewDecoder(bytes.NewReader(raw))
	d.DisallowUnknownFields()
	if d.Decode(target) != nil || d.Decode(new(any)) != io.EOF {
		return &failure{400, "请求JSON或字段无效"}
	}
	return nil
}
func hash(token string) string {
	value := sha256.Sum256([]byte(token))
	return hex.EncodeToString(value[:])
}
func randomHex(size int) (string, error) {
	value := make([]byte, size)
	_, err := rand.Read(value)
	return hex.EncodeToString(value), err
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			response(w, 405, map[string]string{"error": "只允许GET"})
			return
		}
		if s.Health() != nil {
			response(w, 503, map[string]string{"error": "鉴权数据库不可用"})
			return
		}
		response(w, 200, map[string]any{"ok": true, "service": "auth"})
	})
	for _, operation := range []string{"register", "authenticate", "status"} {
		op := operation
		mux.HandleFunc("/v1/"+op, func(w http.ResponseWriter, r *http.Request) {
			if r.Method != http.MethodPost {
				response(w, 405, map[string]string{"error": "只允许POST"})
				return
			}
			if subtle.ConstantTimeCompare([]byte(r.Header.Get("X-Internal-Token")), []byte(s.config.Token)) != 1 {
				response(w, 401, map[string]string{"error": "内部服务凭据无效"})
				return
			}
			body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 64*1024))
			if err != nil {
				response(w, 400, map[string]string{"error": "请求过大或读取失败"})
				return
			}
			ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
			defer cancel()
			s.mu.Lock()
			data, err := s.execute(ctx, op, body)
			s.mu.Unlock()
			if err != nil {
				var f *failure
				if errors.As(err, &f) {
					response(w, f.status, map[string]string{"error": f.message})
				} else {
					response(w, 503, map[string]string{"error": "鉴权服务暂不可用"})
				}
				return
			}
			response(w, 200, data)
		})
	}
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		response(w, 404, map[string]string{"error": "接口不存在"})
	})
	return mux
}

func (s *Server) execute(ctx context.Context, operation string, raw []byte) (any, error) {
	if operation == "status" {
		var request struct{}
		if err := parse(raw, &request); err != nil {
			return nil, err
		}
		var count int
		err := s.conn.QueryRowContext(ctx, "SELECT COUNT(*) FROM auth_accounts").Scan(&count)
		return map[string]any{"ok": true, "accounts": count, "imported_legacy": s.imported}, err
	}
	if operation == "authenticate" {
		var request struct {
			Token string `json:"token"`
		}
		if err := parse(raw, &request); err != nil {
			return nil, err
		}
		if len(request.Token) != 64 {
			return nil, &failure{401, "登录token无效"}
		}
		var identity Identity
		err := s.conn.QueryRowContext(ctx, "SELECT role_id,name FROM auth_accounts WHERE token_hash=?", hash(request.Token)).Scan(&identity.RoleID, &identity.Name)
		if errors.Is(err, sql.ErrNoRows) {
			return nil, &failure{401, "登录token无效"}
		}
		return identity, err
	}
	var request struct {
		Name string `json:"name"`
	}
	if err := parse(raw, &request); err != nil {
		return nil, err
	}
	name := strings.TrimSpace(request.Name)
	if !utf8.ValidString(name) || utf8.RuneCountInString(name) < 1 || utf8.RuneCountInString(name) > 24 {
		return nil, &failure{400, "名字需要1到24个字符"}
	}
	for _, letter := range name {
		if unicode.IsControl(letter) {
			return nil, &failure{400, "名字不能含控制字符"}
		}
	}
	var count int
	if err := s.conn.QueryRowContext(ctx, "SELECT COUNT(*) FROM auth_accounts WHERE name=?", name).Scan(&count); err != nil {
		return nil, err
	}
	if count != 0 {
		return nil, &failure{409, "名字已被注册；请使用原token登录"}
	}
	token, err := randomHex(32)
	if err != nil {
		return nil, err
	}
	id, err := randomHex(16)
	if err != nil {
		return nil, err
	}
	if _, err = s.conn.ExecContext(ctx, "INSERT INTO auth_accounts(role_id,name,token_hash) VALUES(?,?,?)", id, name, hash(token)); err != nil {
		return nil, err
	}
	return map[string]string{"role_id": id, "token": token}, nil
}
