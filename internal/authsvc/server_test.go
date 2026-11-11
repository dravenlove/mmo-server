package authsvc

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

const key = "test-auth-internal-key-at-least-32-characters"

func post(t *testing.T, s *Server, operation string, payload any, authorized bool) (int, map[string]any) {
	t.Helper()
	body, _ := json.Marshal(payload)
	req := httptest.NewRequest(http.MethodPost, "/v1/"+operation, bytes.NewReader(body))
	if authorized {
		req.Header.Set("X-Internal-Token", key)
	}
	w := httptest.NewRecorder()
	s.Handler().ServeHTTP(w, req)
	var result map[string]any
	if json.Unmarshal(w.Body.Bytes(), &result) != nil {
		t.Fatal("无效响应")
	}
	return w.Code, result
}

func TestLegacyImportRegistrationAndRestart(t *testing.T) {
	path := filepath.Join(t.TempDir(), "accounts.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	_, err = db.Exec(`CREATE TABLE roles(role_id TEXT,name TEXT,token_hash TEXT,coins INTEGER); INSERT INTO roles VALUES(?,?,?,77)`, strings.Repeat("1", 32), "原角色", hash(strings.Repeat("a", 64)))
	if err != nil {
		t.Fatal(err)
	}
	c := Config{Token: key, Driver: "sqlite", Path: path}
	s, err := New(c)
	if err != nil {
		t.Fatal(err)
	}
	code, old := post(t, s, "authenticate", map[string]string{"token": strings.Repeat("a", 64)}, true)
	if code != 200 || old["role_id"] != strings.Repeat("1", 32) {
		t.Fatal("旧凭据导入失败")
	}
	if code, _ = post(t, s, "authenticate", map[string]string{"token": strings.Repeat("b", 64)}, true); code != 401 {
		t.Fatal("错误token被接受")
	}
	code, registered := post(t, s, "register", map[string]string{"name": "新角色"}, true)
	if code != 200 || len(registered["token"].(string)) != 64 {
		t.Fatal("注册失败")
	}
	var roles, coins int
	if err = db.QueryRow("SELECT COUNT(*),MAX(coins) FROM roles").Scan(&roles, &coins); err != nil || roles != 1 || coins != 77 {
		t.Fatal("鉴权服务修改了游戏角色数据")
	}
	if err = s.Close(); err != nil {
		t.Fatal(err)
	}
	s, err = New(c)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	code, old = post(t, s, "authenticate", map[string]string{"token": registered["token"].(string)}, true)
	if code != 200 || old["role_id"] != registered["role_id"] {
		t.Fatal("重启后凭据未保留")
	}
	_, status := post(t, s, "status", map[string]any{}, true)
	if status["accounts"] != float64(2) || status["imported_legacy"] != float64(0) {
		t.Fatal("导入不是幂等操作")
	}
}

func TestImportConflictDoesNotOverwriteAccounts(t *testing.T) {
	path := filepath.Join(t.TempDir(), "conflict.db")
	db, _ := sql.Open("sqlite", path)
	defer db.Close()
	_, err := db.Exec(`CREATE TABLE roles(role_id TEXT,name TEXT,token_hash TEXT); CREATE TABLE auth_accounts(role_id TEXT PRIMARY KEY,name TEXT UNIQUE,token_hash TEXT UNIQUE);
	 INSERT INTO roles VALUES('legacy','same','legacy-hash'); INSERT INTO auth_accounts VALUES('other','same','original-hash')`)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = New(Config{Token: key, Driver: "sqlite", Path: path}); err == nil {
		t.Fatal("冲突身份被静默导入")
	}
	var value string
	if err = db.QueryRow("SELECT token_hash FROM auth_accounts WHERE role_id='other'").Scan(&value); err != nil || value != "original-hash" {
		t.Fatal("原账号被覆盖")
	}
}

func TestConcurrentRegistrationAndAuthenticationGuards(t *testing.T) {
	s, err := New(Config{Token: key, Driver: "sqlite", Path: filepath.Join(t.TempDir(), "new.db")})
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if code, _ := post(t, s, "register", map[string]string{"name": "denied"}, false); code != 401 {
		t.Fatal("缺失内部凭据仍可注册")
	}
	var group sync.WaitGroup
	codes := make(chan int, 20)
	for n := 0; n < 20; n++ {
		group.Add(1)
		go func() {
			defer group.Done()
			code, _ := post(t, s, "register", map[string]string{"name": "unique"}, true)
			codes <- code
		}()
	}
	group.Wait()
	close(codes)
	success, conflicts := 0, 0
	for code := range codes {
		if code == 200 {
			success++
		} else if code == 409 {
			conflicts++
		}
	}
	if success != 1 || conflicts != 19 {
		t.Fatalf("并发注册结果错误: %d/%d", success, conflicts)
	}
	for _, body := range []any{nil, []string{"bad"}, map[string]any{"name": "bad", "extra": 1}, map[string]string{"name": "\n"}, map[string]string{"name": strings.Repeat("x", 25)}} {
		if code, _ := post(t, s, "register", body, true); code != 400 {
			t.Fatal("无效注册数据被接受")
		}
	}
	if err = s.Health(); err != nil {
		t.Fatal(err)
	}
	if _, err = s.conn.ExecContext(context.Background(), "SELECT COUNT(*) FROM auth_accounts"); err != nil {
		t.Fatal(err)
	}
}
