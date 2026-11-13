package authsvc

import (
	"context"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"golang.org/x/crypto/bcrypt"
)

func TestPasswordAccountsRestartExpiryAndLegacyBinding(t *testing.T) {
	c := Config{Token: key, Driver: "sqlite", Path: filepath.Join(t.TempDir(), "accounts.db")}
	s, err := New(c)
	if err != nil {
		t.Fatal(err)
	}
	defer func() { s.Close() }()
	password := "联调-password-123!"
	code, created := post(t, s, "account_register", map[string]string{"name": "密码角色", "password": password}, true)
	if code != 200 {
		t.Fatalf("register status %d", code)
	}
	id, ticket := created["role_id"].(string), created["token"].(string)
	if created["expires_at_unix_ms"].(float64) <= float64(time.Now().UnixMilli()) {
		t.Fatal("missing expiry")
	}
	var encoded, stableHash string
	if err = s.conn.QueryRowContext(context.Background(), "SELECT password_hash FROM auth_passwords WHERE role_id=?", id).Scan(&encoded); err != nil {
		t.Fatal(err)
	}
	if encoded == password || bcrypt.CompareHashAndPassword([]byte(encoded), []byte(password)) != nil {
		t.Fatal("password not hashed")
	}
	if err = s.conn.QueryRowContext(context.Background(), "SELECT token_hash FROM auth_accounts WHERE role_id=?", id).Scan(&stableHash); err != nil {
		t.Fatal(err)
	}
	if stableHash == hash(ticket) {
		t.Fatal("temporary ticket replaced stable identity")
	}
	if code, _ = post(t, s, "authenticate", map[string]string{"token": ticket}, true); code != 200 {
		t.Fatal("ticket rejected")
	}
	if code, _ = post(t, s, "account_login", map[string]string{"name": "密码角色", "password": "wrong-password"}, true); code != 401 {
		t.Fatal("wrong password accepted")
	}
	code, missing := post(t, s, "account_login", map[string]string{"name": "不存在", "password": "wrong-password"}, true)
	_, wrong := post(t, s, "account_login", map[string]string{"name": "密码角色", "password": "wrong-password"}, true)
	if code != 401 || missing["error"] != wrong["error"] {
		t.Fatal("missing user disclosed")
	}
	if code, _ = post(t, s, "account_register", map[string]string{"name": "密码角色", "password": password}, true); code != 409 {
		t.Fatal("duplicate overwrote account")
	}
	s.Close()
	s, err = New(c)
	if err != nil {
		t.Fatal(err)
	}
	code, logged := post(t, s, "account_login", map[string]string{"name": "密码角色", "password": password}, true)
	if code != 200 || logged["role_id"] != id || logged["token"] == ticket {
		t.Fatal("password restart login failed")
	}
	if _, err = s.conn.ExecContext(context.Background(), "UPDATE auth_login_tokens SET expires_ms=0 WHERE token_hash=?", hash(ticket)); err != nil {
		t.Fatal(err)
	}
	if code, _ = post(t, s, "authenticate", map[string]string{"token": ticket}, true); code != 401 {
		t.Fatal("expired ticket accepted")
	}
	if code, _ = post(t, s, "authenticate", map[string]any{"token": logged["token"]}, true); code != 200 {
		t.Fatal("other ticket expired")
	}
	_, legacy := post(t, s, "register", map[string]string{"name": "原角色"}, true)
	code, bound := post(t, s, "account_bind", map[string]any{"token": legacy["token"], "password": password}, true)
	if code != 200 || bound["role_id"] != legacy["role_id"] {
		t.Fatal("legacy bind failed")
	}
	if code, _ = post(t, s, "account_bind", map[string]any{"token": legacy["token"], "password": "overwrite-password"}, true); code != 409 {
		t.Fatal("existing password overwritten")
	}
	if code, _ = post(t, s, "account_login", map[string]string{"name": "原角色", "password": password}, true); code != 200 {
		t.Fatal("bound login failed")
	}
	if code, _ = post(t, s, "authenticate", map[string]any{"token": legacy["token"]}, true); code != 200 {
		t.Fatal("legacy credential invalidated")
	}
}

func TestPasswordBoundsLimitsAndTicketCap(t *testing.T) {
	s, err := New(Config{Token: key, Driver: "sqlite", Path: filepath.Join(t.TempDir(), "accounts.db")})
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	for _, password := range []string{"short", strings.Repeat("x", 73), "12345678\n"} {
		if code, _ := post(t, s, "account_register", map[string]string{"name": "invalid", "password": password}, true); code != 400 {
			t.Fatal("invalid password accepted")
		}
	}
	if code, _ := post(t, s, "account_register", map[string]string{"name": "private", "password": "password-123"}, false); code != 401 {
		t.Fatal("internal key not enforced")
	}
	_, created := post(t, s, "account_register", map[string]string{"name": "limited", "password": "password-123"}, true)
	id := created["role_id"].(string)
	s.mu.Lock()
	for i := 0; i < 9; i++ {
		tx, e := s.conn.BeginTx(context.Background(), nil)
		if e != nil {
			t.Fatal(e)
		}
		if _, e = issueAccountTicket(context.Background(), tx, id, "limited"); e != nil {
			tx.Rollback()
			t.Fatal(e)
		}
		if e = tx.Commit(); e != nil {
			t.Fatal(e)
		}
	}
	var count int
	err = s.conn.QueryRowContext(context.Background(), "SELECT COUNT(*) FROM auth_login_tokens WHERE role_id=?", id).Scan(&count)
	s.mu.Unlock()
	if err != nil || count != accountTokenLimit {
		t.Fatal("ticket cap not enforced")
	}
	for i := 0; i < 6; i++ {
		code, _ := post(t, s, "account_login", map[string]string{"name": "throttle", "password": "wrong-password"}, true)
		if code != 401 {
			t.Fatalf("attempt %d status %d", i, code)
		}
	}
	if code, _ := post(t, s, "account_login", map[string]string{"name": "throttle", "password": "wrong-password"}, true); code != 429 {
		t.Fatal("account throttling absent")
	}
	s.passwordSlots <- struct{}{}
	s.passwordSlots <- struct{}{}
	if code, _ := post(t, s, "account_login", map[string]string{"name": "busy", "password": "wrong-password"}, true); code != 429 {
		t.Fatal("password CPU queue unbounded")
	}
	if err := s.Health(); err != nil {
		t.Fatal("busy password work blocked health")
	}
	<-s.passwordSlots
	<-s.passwordSlots
}
