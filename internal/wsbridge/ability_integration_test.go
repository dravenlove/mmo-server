package wsbridge

import (
	"encoding/json"
	"fmt"
	"github.com/gorilla/websocket"
	"math"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"
)

// 由隔离原生/Docker fixture 显式提供端口；从不默认连接线上。
func TestAbilityWorldIntegration(t *testing.T) {
	upstream := os.Getenv("MMO_TEST_COMBAT_GATEWAY")
	if upstream == "" {
		t.Skip("isolated ability gateway not configured")
	}
	bridge, err := New(Config{Upstream: upstream, AllowedOrigins: []string{"https://game.example"}, ProtocolVersion: "webgl-combat-v1"})
	if err != nil {
		t.Fatal(err)
	}
	h := httptest.NewServer(bridge)
	defer h.Close()
	defer bridge.Close()
	peer := func() *movementPeer {
		c, _, err := websocket.DefaultDialer.Dial("ws"+strings.TrimPrefix(h.URL, "http")+"/ws", http.Header{"Origin": {"https://game.example"}})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { c.Close() })
		return &movementPeer{t: t, connection: c}
	}
	decode := func(v any, out any) {
		b, _ := json.Marshal(v)
		if err := json.Unmarshal(b, out); err != nil {
			t.Fatal(err)
		}
	}
	type combatRole struct {
		ID      string `json:"role_id"`
		X, Y    float64
		Session int64 `json:"session_epoch"`
		Combat  struct {
			HP, MP    int
			Cooldowns map[string]int
		} `json:"combat"`
	}
	type combatView struct {
		Self                       combatRole
		Players                    []combatRole
		Projectiles, Walls, Skills []json.RawMessage
		World                      string `json:"world_epoch"`
		PVP                        bool   `json:"pvp_enabled"`
	}
	view := func(p *movementPeer) combatView {
		var v combatView
		decode(p.call("view", nil), &v)
		if v.World == "" || !v.PVP || len(v.Skills) != 2 {
			t.Fatal("incomplete combat view")
		}
		return v
	}
	a, b := peer(), peer()
	var credentials [2]map[string]json.RawMessage
	for i, p := range []*movementPeer{a, b} {
		credentials[i] = p.call("register", map[string]any{"name": fmt.Sprintf("ability-%d-%d", time.Now().UnixNano()%10000000, i)})
		var token string
		decode(credentials[i]["token"], &token)
		p.call("login", map[string]any{"token": token})
	}
	var ping struct {
		Version string `json:"protocol_version"`
	}
	decode(a.call("ping", nil), &ping)
	if ping.Version != "webgl-combat-v1" {
		t.Fatal("wrong protocol")
	}
	var bseq int
	for deadline := time.Now().Add(5 * time.Second); ; {
		v := view(b)
		view(a)
		bseq++
		dx := 1.
		if v.Self.X >= 5.8 {
			dx = 0
		}
		b.call("move", map[string]any{"seq": bseq, "dx": dx, "dy": 0})
		if dx == 0 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("movement setup timeout")
		}
		time.Sleep(90 * time.Millisecond)
	}
	cast := func(p *movementPeer, seq int, skill string, extra map[string]any) map[string]json.RawMessage {
		fields := map[string]any{"seq": seq, "skill_id": skill, "dx": 1, "dy": 0}
		for k, v := range extra {
			fields[k] = v
		}
		return p.call("cast", fields)
	}
	shot := cast(a, 1, "arc_bolt", nil)
	if string(shot["accepted"]) != "true" || len(shot["projectile"]) == 0 {
		t.Fatal("projectile spawn response missing")
	}
	retry := cast(a, 1, "arc_bolt", nil)
	if string(retry["projectile"]) != string(shot["projectile"]) {
		t.Fatal("retry duplicated projectile")
	}
	if string(cast(a, 2, "arc_bolt", nil)["accepted"]) != "false" {
		t.Fatal("cooldown not a recoverable rejection")
	}
	deadline := time.Now().Add(3 * time.Second)
	for view(b).Self.Combat.HP == 180 {
		view(a)
		if time.Now().After(deadline) {
			t.Fatal("PvP projectile did not hit")
		}
		time.Sleep(50 * time.Millisecond)
	}
	if view(b).Self.Combat.HP != 152 {
		t.Fatal("wrong PvP damage")
	}
	placement := map[string]any{"x": 3, "y": 0}
	wall := cast(a, 2, "stone_wall", placement)
	if string(wall["accepted"]) != "true" {
		t.Fatal("wall rejected")
	}
	if len(view(a).Walls) != 1 {
		t.Fatal("wall lost at gateway serialization")
	}
	for view(a).Self.Combat.Cooldowns["arc_bolt"] > 0 {
		view(b)
		time.Sleep(50 * time.Millisecond)
	}
	cast(a, 3, "arc_bolt", nil)
	time.Sleep(500 * time.Millisecond)
	if view(b).Self.Combat.HP != 152 || len(view(a).Projectiles) != 0 {
		t.Fatal("wall did not block projectile")
	}
	for i := 1; i <= 8; i++ {
		a.call("move", map[string]any{"seq": i, "dx": 1, "dy": 0})
		view(b)
		time.Sleep(90 * time.Millisecond)
	}
	a.call("move", map[string]any{"seq": 9, "dx": 0, "dy": 0})
	if math.Abs(view(a).Self.X-2.25) > .1 {
		t.Fatal("wall did not block movement")
	}
	// 新会话清理旧对象，并保留冷却；旧连接关闭不能移除新会话。
	c := peer()
	var token string
	decode(credentials[0]["token"], &token)
	c.call("login", map[string]any{"token": token})
	if len(view(c).Walls) != 0 || view(c).Self.Combat.Cooldowns["stone_wall"] <= 0 {
		t.Fatal("relogin did not clean objects or preserve cooldown")
	}
	a.connection.Close()
	if view(c).Self.ID == "" {
		t.Fatal("old close damaged new role")
	}
	// 非法消息返回相关请求错误，不执行技能；正常连接仍能继续调试。
	bad := c.send("cast", map[string]any{"seq": 1, "skill_id": "arc_bolt", "dx": nil, "dy": 1})
	c.receive(bad, false)
	c.call("ping", nil)
	t.Log("real WebSocket login, movement, projectile PvP, recoverable cooldown, wall collision, retry and reconnect passed")
}
