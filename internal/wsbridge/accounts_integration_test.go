package wsbridge

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// 仅使用隔离 Compose 提供的地址，默认不访问线上或创建线上账号。
func TestAccountLoginIntegration(t *testing.T) {
	upstream := os.Getenv("MMO_TEST_ACCOUNT_GATEWAY")
	if upstream == "" {
		t.Skip("isolated external-auth gateway not configured")
	}
	bridge, err := New(Config{Upstream: upstream, AllowedOrigins: []string{"https://game.example"}})
	if err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(bridge)
	defer server.Close()
	defer bridge.Close()
	connect := func() *movementPeer {
		t.Helper()
		c, _, e := websocket.DefaultDialer.Dial("ws"+strings.TrimPrefix(server.URL, "http")+"/ws", http.Header{"Origin": {"https://game.example"}})
		if e != nil {
			t.Fatal(e)
		}
		t.Cleanup(func() { c.Close() })
		return &movementPeer{t: t, connection: c}
	}
	name := "pw-" + strconv.FormatInt(time.Now().UnixNano(), 16)
	password := "test-password-only-123!"
	p := connect()
	created := p.call("account_register", map[string]any{"name": name, "password": password})
	var token, id string
	json.Unmarshal(created["token"], &token)
	json.Unmarshal(created["role_id"], &id)
	if len(token) != 64 || len(id) != 32 {
		t.Fatal("registration ticket missing")
	}
	p.receive(p.send("move", map[string]any{"seq": 1, "dx": 1, "dy": 0}), false)
	entered := p.call("login", map[string]any{"token": token})
	if string(entered["role_id"]) != string(created["role_id"]) {
		t.Fatal("registration role changed")
	}
	p.connection.Close()
	q := connect()
	q.receive(q.send("account_login", map[string]any{"name": name, "password": "wrong-password"}), false)
	logged := q.call("account_login", map[string]any{"name": name, "password": password})
	var next string
	json.Unmarshal(logged["token"], &next)
	if next == token || string(logged["role_id"]) != string(created["role_id"]) {
		t.Fatal("password login did not preserve account")
	}
	q.call("login", map[string]any{"token": next})
	q.call("view", nil)
	q.receive(q.send("account_login", map[string]any{"name": name, "password": password}), false)
	legacy := connect()
	oldName := "old-" + strconv.FormatInt(time.Now().UnixNano(), 16)
	old := legacy.call("register", map[string]any{"name": oldName})
	var original string
	json.Unmarshal(old["token"], &original)
	bound := legacy.call("account_bind", map[string]any{"token": original, "password": password})
	if string(bound["role_id"]) != string(old["role_id"]) {
		t.Fatal("binding changed identity")
	}
	legacy.receive(legacy.send("account_bind", map[string]any{"token": original, "password": "overwrite-password"}), false)
	oldLogin := legacy.call("account_login", map[string]any{"name": oldName, "password": password})
	if string(oldLogin["role_id"]) != string(old["role_id"]) {
		t.Fatal("bound password changed role")
	}
	legacy.call("login", map[string]any{"token": original})
}
