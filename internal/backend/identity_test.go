package backend

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"mmo/internal/authsvc"
)

func fixtureAuth(t *testing.T, f *fixture) *authsvc.Server {
	t.Helper()
	c := authsvc.Config{Token: testToken, Driver: "sqlite", Path: f.cfg.DBPath}
	if f.cfg.DBDriver == "mysql" {
		m := f.cfg.MySQL
		c = authsvc.Config{Token: testToken, Driver: "mysql", Host: m.Host, Port: m.Port, User: m.User, Password: m.Password, Database: m.Database, TLS: m.TLS}
	}
	s, err := authsvc.New(c)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = s.Close() })
	return s
}

func TestExternalAuthPreservesLegacyPlayersAndKeepsGameplayLocal(t *testing.T) {
	f := newFixture(t)
	token, original := registerLogin(t, f, "LegacyAuth")
	wantPost(t, f.s, "claim", sessionRequest{original.Role.RoleID, original.Role.SessionEpoch}, 200, nil)
	identity := fixtureAuth(t, f)
	if f.cfg.DBDriver == "mysql" {
		m := f.cfg.MySQL
		if duplicate, err := authsvc.New(authsvc.Config{Token: testToken, Driver: "mysql", Host: m.Host, Port: m.Port, User: m.User, Password: m.Password, Database: m.Database, TLS: m.TLS}); err == nil {
			duplicate.Close()
			t.Fatal("第二个鉴权写入者获得了同一数据库")
		}
	}
	var calls atomic.Int64
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { calls.Add(1); identity.Handler().ServeHTTP(w, r) }))
	defer upstream.Close()
	f.cfg.AuthURL = upstream.URL
	f.restart(t)
	var current Route
	wantPost(t, f.s, "login", map[string]string{"token": token}, 200, &current)
	if current.Role.RoleID != original.Role.RoleID || current.Role.Coins != 100 || current.Role.SessionEpoch != original.Role.SessionEpoch+1 {
		t.Fatal("旧角色或余额在分离鉴权后改变")
	}
	var registered map[string]string
	wantPost(t, f.s, "register", map[string]string{"name": "NewAuth"}, 200, &registered)
	var newRole Route
	wantPost(t, f.s, "login", map[string]string{"token": registered["token"]}, 200, &newRole)
	if newRole.Role.RoleID != registered["role_id"] || newRole.Role.Coins != 0 {
		t.Fatal("首次登录未按身份创建游戏角色")
	}
	wantPost(t, f.s, "login", map[string]string{"token": registered["token"]}, 200, &newRole)
	var count int
	if err := f.s.db.QueryRow("SELECT COUNT(*) FROM roles").Scan(&count); err != nil || count != 2 {
		t.Fatal("重复登录创建了重复角色")
	}
	upstream.Close()
	before := calls.Load()
	wantPost(t, f.s, "session", sessionRequest{current.Role.RoleID, current.Role.SessionEpoch}, 200, nil)
	wantPost(t, f.s, "claim", sessionRequest{current.Role.RoleID, current.Role.SessionEpoch}, 200, nil)
	if calls.Load() != before {
		t.Fatal("在线游戏请求仍调用跨地域鉴权")
	}
	wantPost(t, f.s, "login", map[string]string{"token": token}, 503, nil)
	wantPost(t, f.s, "session", sessionRequest{current.Role.RoleID, current.Role.SessionEpoch}, 200, nil)
}

func TestSlowAuthenticationDoesNotHoldGameStateLock(t *testing.T) {
	f := newFixture(t)
	token, route := registerLogin(t, f, "AlreadyOnline")
	entered, release := make(chan struct{}), make(chan struct{})
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = io.Copy(io.Discard, r.Body)
		close(entered)
		select {
		case <-release:
		case <-r.Context().Done():
		}
		w.WriteHeader(503)
	}))
	defer upstream.Close()
	f.cfg.AuthURL = upstream.URL
	f.restart(t)
	loginDone := make(chan int, 1)
	go func() { status, _ := post(f.s, "login", map[string]string{"token": token}); loginDone <- status }()
	<-entered
	sessionDone := make(chan int, 1)
	go func() {
		status, _ := post(f.s, "session", sessionRequest{route.Role.RoleID, route.Role.SessionEpoch})
		sessionDone <- status
	}()
	var code int
	select {
	case code = <-sessionDone:
	case <-time.After(time.Second):
		close(release)
		<-loginDone
		t.Fatal("慢鉴权阻塞了在线会话检查")
	}
	close(release)
	if code != 200 || <-loginDone != 503 {
		t.Fatal("鉴权故障隔离结果错误")
	}
}

func TestExternalAuthRejectsRedirectAndInvalidIdentity(t *testing.T) {
	f := newFixture(t)
	var leaked atomic.Bool
	destination := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { leaked.Store(true) }))
	defer destination.Close()
	redirect := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, destination.URL, http.StatusTemporaryRedirect)
	}))
	defer redirect.Close()
	f.cfg.AuthURL = redirect.URL
	f.restart(t)
	wantPost(t, f.s, "login", map[string]string{"token": strings.Repeat("a", 64)}, 503, nil)
	if leaked.Load() {
		t.Fatal("鉴权凭据被重定向到其他地址")
	}
	invalid := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		json.NewEncoder(w).Encode(map[string]string{"role_id": "bad", "name": "bad"})
	}))
	defer invalid.Close()
	f.cfg.AuthURL = invalid.URL
	f.restart(t)
	wantPost(t, f.s, "login", map[string]string{"token": strings.Repeat("a", 64)}, 503, nil)
	var count int
	f.s.db.QueryRow("SELECT COUNT(*) FROM roles").Scan(&count)
	if count != 0 {
		t.Fatal("无效鉴权响应创建了角色")
	}
}
