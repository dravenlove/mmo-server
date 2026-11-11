package wsbridge

import (
	"encoding/json"
	"fmt"
	"math"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

type movementPeer struct {
	t          *testing.T
	connection *websocket.Conn
	next       int
}

func (p *movementPeer) send(operation string, fields map[string]any) string {
	p.t.Helper()
	p.next++
	id := fmt.Sprint(p.next)
	if fields == nil {
		fields = map[string]any{}
	}
	fields["id"], fields["op"] = id, operation
	if err := p.connection.WriteJSON(fields); err != nil {
		p.t.Fatal(err)
	}
	return id
}
func (p *movementPeer) receive(id string, success bool) map[string]json.RawMessage {
	p.t.Helper()
	p.connection.SetReadDeadline(time.Now().Add(5 * time.Second))
	for {
		kind, body, err := p.connection.ReadMessage()
		if err != nil {
			p.t.Fatal(err)
		}
		if kind != websocket.TextMessage || len(body) > 65535 {
			p.t.Fatal("invalid frame")
		}
		var response struct {
			ID    string                     `json:"id"`
			Op    string                     `json:"op"`
			OK    bool                       `json:"ok"`
			Data  map[string]json.RawMessage `json:"data"`
			Error string                     `json:"error"`
		}
		if json.Unmarshal(body, &response) != nil {
			p.t.Fatal("invalid response JSON")
		}
		if response.ID != id {
			if !response.OK {
				p.t.Fatalf("asynchronous error: %s", response.Error)
			}
			continue
		}
		if response.OK != success {
			p.t.Fatalf("unexpected %s outcome: %s", response.Op, response.Error)
		}
		return response.Data
	}
}
func (p *movementPeer) call(op string, fields map[string]any) map[string]json.RawMessage {
	return p.receive(p.send(op, fields), true)
}

type movementRole struct {
	ID      string  `json:"role_id"`
	Scene   string  `json:"scene_id"`
	Owner   int64   `json:"owner_epoch"`
	Session int64   `json:"session_epoch"`
	X       float64 `json:"x"`
	Y       float64 `json:"y"`
}
type movementView struct {
	Self    movementRole   `json:"self"`
	Players []movementRole `json:"players"`
	Scene   string         `json:"scene_id"`
	Tick    int64          `json:"tick"`
}

func (p *movementPeer) view() movementView {
	p.t.Helper()
	raw, _ := json.Marshal(p.call("view", nil))
	var view movementView
	if json.Unmarshal(raw, &view) != nil || view.Self.ID == "" || view.Scene != "scene-webgl" || view.Self.Scene != view.Scene || len(view.Players) > 128 {
		p.t.Fatal("invalid movement view")
	}
	ids := map[string]bool{}
	for _, role := range view.Players {
		if role.Scene != view.Scene || ids[role.ID] || role.Owner <= 0 || role.Session <= 0 || math.Abs(role.X) > 100 || math.Abs(role.Y) > 100 {
			p.t.Fatal("invalid AOI role")
		}
		ids[role.ID] = true
	}
	return view
}

// 仅连接验收脚本新建的隔离 Docker 项目，不默认访问线上。
func TestMovementWorldIntegration(t *testing.T) {
	upstream := os.Getenv("MMO_TEST_MOVEMENT_GATEWAY")
	if upstream == "" {
		t.Skip("isolated movement gateway not configured")
	}
	bridge, err := New(Config{Upstream: upstream, AllowedOrigins: []string{"https://game.example"}})
	if err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(bridge)
	defer server.Close()
	defer bridge.Close()
	endpoint := "ws" + strings.TrimPrefix(server.URL, "http") + "/ws"
	connect := func() *movementPeer {
		t.Helper()
		c, _, err := websocket.DefaultDialer.Dial(endpoint, http.Header{"Origin": {"https://game.example"}})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { c.Close() })
		return &movementPeer{t: t, connection: c}
	}
	register := func(p *movementPeer, name string) (string, movementRole) {
		raw := p.call("register", map[string]any{"name": name})
		var token string
		json.Unmarshal(raw["token"], &token)
		if token == "" {
			t.Fatal("missing token")
		}
		raw = p.call("login", map[string]any{"token": token})
		body, _ := json.Marshal(raw)
		var role movementRole
		json.Unmarshal(body, &role)
		if role.Scene != "scene-webgl" {
			t.Fatal("login did not select movement scene")
		}
		return token, role
	}
	alice, bob := connect(), connect()
	token, original := register(alice, "ws-alice")
	_, other := register(bob, "ws-bob")
	find := func(view movementView, id string) bool {
		for _, role := range view.Players {
			if role.ID == id {
				return true
			}
		}
		return false
	}
	if !find(alice.view(), other.ID) {
		t.Fatal("nearby player missing")
	}
	// 方向输入有界在途，逐个独立回执；与视图/推送同时存在。
	ids := []string{}
	for seq := 1; seq <= 4; seq++ {
		ids = append(ids, alice.send("move", map[string]any{"seq": seq, "dx": 1, "dy": 1}))
	}
	for _, id := range ids {
		alice.receive(id, true)
	}
	time.Sleep(100 * time.Millisecond)
	alice.call("move", map[string]any{"seq": 5, "dx": 0, "dy": 0})
	stopped := alice.view()
	time.Sleep(300 * time.Millisecond)
	stable := alice.view()
	if math.Hypot(stable.Self.X-stopped.Self.X, stable.Self.Y-stopped.Self.Y) > .02 {
		t.Fatal("zero input did not stop")
	}
	alice.call("move", map[string]any{"seq": 6, "dx": 1, "dy": 0})
	start := alice.view()
	time.Sleep(500 * time.Millisecond)
	expired := alice.view()
	if expired.Self.X-start.Self.X > 1.51 {
		t.Fatal("no-input timeout exceeded speed budget")
	}
	// 持续向右离开 30 单位半径；另一个角色持续取快照保活。
	seq := 7
	deadline := time.Now().Add(8 * time.Second)
	for alice.view().Self.X < 32 && time.Now().Before(deadline) {
		alice.call("move", map[string]any{"seq": seq, "dx": 1, "dy": 0})
		seq++
		bob.view()
		time.Sleep(100 * time.Millisecond)
	}
	alice.call("move", map[string]any{"seq": seq, "dx": 0, "dy": 0})
	seq++
	if find(bob.view(), original.ID) {
		t.Fatal("player remained in AOI after leaving radius")
	}
	// 不改账号：新连接登录必须取得更高的会话代次，旧会话的移动失败。
	newAlice := connect()
	login := newAlice.call("login", map[string]any{"token": token})
	var newSession int64
	json.Unmarshal(login["session_epoch"], &newSession)
	if newSession <= original.Session {
		t.Fatal("session epoch did not advance")
	}
	if state := newAlice.view(); state.Self.ID != original.ID {
		t.Fatal("reconnect changed identity")
	}
	newAlice.call("move", map[string]any{"seq": 1, "dx": 0, "dy": 0})
	// 回到 AOI 并检验断线消失；新会话持续刷新，旧连接不能复活旧状态。
	seq = 2
	deadline = time.Now().Add(8 * time.Second)
	for newAlice.view().Self.X > 20 && time.Now().Before(deadline) {
		newAlice.call("move", map[string]any{"seq": seq, "dx": -1, "dy": 0})
		seq++
		bob.view()
		time.Sleep(100 * time.Millisecond)
	}
	newAlice.call("move", map[string]any{"seq": seq, "dx": 0, "dy": 0})
	if !find(bob.view(), original.ID) {
		t.Fatal("AOI re-entry missing")
	}
	newAlice.connection.Close()
	deadline = time.Now().Add(2 * time.Second)
	for find(bob.view(), original.ID) && time.Now().Before(deadline) {
		time.Sleep(100 * time.Millisecond)
	}
	if find(bob.view(), original.ID) {
		t.Fatal("disconnected player remained visible")
	}
	t.Log("PASS WSS framing, two identities, four in-flight inputs, stop, input expiry, AOI leave/re-enter, new session and disconnect presence")
}
