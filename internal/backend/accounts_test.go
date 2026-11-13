package backend

import (
	"net/http/httptest"
	"strings"
	"testing"

	"mmo/internal/authsvc"
)

func TestPasswordLoginPreservesRoleAndAuthRestartImport(t *testing.T) {
	f := newFixture(t)
	legacyToken, old := registerLogin(t, f, "KeptAccount")
	wantPost(t, f.s, "claim", sessionRequest{old.Role.RoleID, old.Role.SessionEpoch}, 200, nil)
	identity := fixtureAuth(t, f)
	upstream := httptest.NewServer(identity.Handler())
	defer upstream.Close()
	f.cfg.AuthURL = upstream.URL
	f.restart(t)
	password := "test-password-123!"
	wantPost(t, f.s, "account_bind", map[string]string{"token": legacyToken, "password": password}, 200, nil)
	var ticket struct {
		Token     string `json:"token"`
		RoleID    string `json:"role_id"`
		ExpiresAt int64  `json:"expires_at_unix_ms"`
	}
	wantPost(t, f.s, "account_login", map[string]string{"name": "KeptAccount", "password": password}, 200, &ticket)
	if ticket.RoleID != old.Role.RoleID || ticket.ExpiresAt == 0 {
		t.Fatal("password changed identity")
	}
	var current Route
	wantPost(t, f.s, "login", map[string]string{"token": ticket.Token}, 200, &current)
	if current.Role.RoleID != old.Role.RoleID || current.Role.Coins != 100 {
		t.Fatal("original role or balance changed")
	}
	wantPost(t, f.s, "login", map[string]string{"token": legacyToken}, 200, nil)
	wantPost(t, f.s, "account_register", map[string]string{"name": "NewPasswordAccount", "password": password}, 200, &ticket)
	wantPost(t, f.s, "login", map[string]string{"token": ticket.Token}, 200, &current)
	if current.Role.RoleID != ticket.RoleID {
		t.Fatal("wrong new role")
	}
	var accountHash, roleHash string
	if err := f.s.db.QueryRow("SELECT token_hash FROM auth_accounts WHERE role_id=?", ticket.RoleID).Scan(&accountHash); err != nil {
		t.Fatal(err)
	}
	if err := f.s.db.QueryRow("SELECT token_hash FROM roles WHERE role_id=?", ticket.RoleID).Scan(&roleHash); err != nil {
		t.Fatal(err)
	}
	if roleHash != accountHash || roleHash == tokenHash(ticket.Token) {
		t.Fatal("temporary token corrupted stable legacy import")
	}
	// Auth 重启会重新核对 roles；密码登录后的账号也必须能通过旧账号导入校验。
	if err := identity.Close(); err != nil {
		t.Fatal(err)
	}
	c := authsvc.Config{Token: testToken, Driver: "sqlite", Path: f.cfg.DBPath}
	if f.cfg.DBDriver == "mysql" {
		m := f.cfg.MySQL
		c = authsvc.Config{Token: testToken, Driver: "mysql", Host: m.Host, Port: m.Port, User: m.User, Password: m.Password, Database: m.Database, TLS: m.TLS}
	}
	restarted, err := authsvc.New(c)
	if err != nil {
		t.Fatal("auth restart identity conflict:", err)
	}
	defer restarted.Close()
	second := httptest.NewServer(restarted.Handler())
	defer second.Close()
	f.cfg.AuthURL = second.URL
	f.restart(t)
	wantPost(t, f.s, "account_login", map[string]string{"name": "NewPasswordAccount", "password": password}, 200, &ticket)
	wantPost(t, f.s, "login", map[string]string{"token": ticket.Token}, 200, nil)
	wantPost(t, f.s, "account_login", map[string]string{"name": "NewPasswordAccount", "password": "wrong-password"}, 401, nil)
	var count int
	if err := f.s.db.QueryRow("SELECT COUNT(*) FROM roles").Scan(&count); err != nil || count != 2 {
		t.Fatal("extra roles created")
	}
}

func TestPasswordAccountRequiresExternalAuthAndRejectsExtraFields(t *testing.T) {
	f := newFixture(t)
	wantPost(t, f.s, "account_login", map[string]string{"name": "name", "password": "password-123"}, 503, nil)
	identity := fixtureAuth(t, f)
	upstream := httptest.NewServer(identity.Handler())
	defer upstream.Close()
	f.cfg.AuthURL = upstream.URL
	f.restart(t)
	wantPost(t, f.s, "account_register", map[string]string{"name": "name", "password": "password-123", "extra": "not-forwarded"}, 400, nil)
	wantPost(t, f.s, "account_bind", map[string]string{"token": strings.Repeat("a", 64), "password": "password-123"}, 401, nil)
}
