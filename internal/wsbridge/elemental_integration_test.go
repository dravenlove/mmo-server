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

// 必须由隔离 fixture 提供端口，不访问默认线上地址。
func TestElementalWorldIntegration(t *testing.T) {
	upstream := os.Getenv("MMO_TEST_ELEMENTAL_GATEWAY")
	if upstream == "" {
		t.Skip("isolated elemental gateway not configured")
	}
	bridge, err := New(Config{Upstream: upstream, AllowedOrigins: []string{"https://game.example"}, ProtocolVersion: "webgl-elemental-v1"})
	if err != nil {
		t.Fatal(err)
	}
	h := httptest.NewServer(bridge)
	defer h.Close()
	defer bridge.Close()
	peer := func() *movementPeer {
		c, _, e := websocket.DefaultDialer.Dial("ws"+strings.TrimPrefix(h.URL, "http")+"/ws", http.Header{"Origin": {"https://game.example"}})
		if e != nil {
			t.Fatal(e)
		}
		t.Cleanup(func() { c.Close() })
		return &movementPeer{t: t, connection: c}
	}
	decode := func(raw any, out any) {
		body, _ := json.Marshal(raw)
		if err := json.Unmarshal(body, out); err != nil {
			t.Fatal(err)
		}
	}
	type actor struct {
		ID        string `json:"id"`
		X, Y      float64
		HP, MaxHP float64
	}
	type battle struct {
		ID                     string `json:"battle_id"`
		Version, Width, Height int
		Phase                  string
		Player                 actor
		Enemies                []actor
		Tiles, Effects         []json.RawMessage
		Cooldown, Elapsed      float64
		Living                 int `json:"living_enemies"`
		Conduction, Steam      int
		Sequence               int64 `json:"event_sequence"`
	}
	type viewValue struct {
		Self struct {
			ID      string `json:"role_id"`
			Scene   string `json:"scene_id"`
			Owner   int64  `json:"owner_epoch"`
			Session int64  `json:"session_epoch"`
			X, Y    float64
			Combat  struct{ Dead bool } `json:"combat"`
		}
		Players  []json.RawMessage
		Features []string
		Battle   *battle
		Scene    string `json:"scene_id"`
		Tick     uint64
	}
	var frames *os.File
	if path := os.Getenv("MMO_TEST_ELEMENTAL_FRAMES"); path != "" {
		frames, err = os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			t.Fatal(err)
		}
		defer frames.Close()
	}
	view := func(p *movementPeer) viewValue {
		raw := p.call("view", nil)
		body, _ := json.Marshal(map[string]any{"op": "state", "ok": true, "data": raw})
		if len(body) > 65535 {
			t.Fatal("oversized elemental frame")
		}
		if frames != nil {
			frames.Write(append(body, '\n'))
		}
		var v viewValue
		decode(raw, &v)
		if len(v.Features) != 1 || v.Features[0] != "elemental_arena_v1" || v.Scene != v.Self.Scene {
			t.Fatal("feature/scene mismatch")
		}
		if b := v.Battle; b != nil {
			if b.Version != 1 || b.Width != 13 || b.Height != 9 || len(b.Enemies) != 6 || len(b.Tiles) != 117 || len(b.Effects) > 64 || b.Player.ID != v.Self.ID || math.Abs(b.Player.X-v.Self.X) > 1e-6 || math.Abs(b.Player.Y-v.Self.Y) > 1e-6 || v.Self.Combat.Dead != (b.Player.HP == 0) || len(v.Players) != 1 {
				t.Fatal("invalid elemental snapshot")
			}
		}
		return v
	}
	contextFields := func(v viewValue) map[string]any {
		return map[string]any{"scene_id": v.Scene, "owner_epoch": v.Self.Owner}
	}
	operation := func(p *movementPeer, op string, v viewValue, extra map[string]any) map[string]json.RawMessage {
		fields := contextFields(v)
		for k, x := range extra {
			fields[k] = x
		}
		return p.call(op, fields)
	}
	accepted := func(data map[string]json.RawMessage) bool { return string(data["accepted"]) == "true" }
	a, b := peer(), peer()
	var tokens [2]string
	for i, p := range []*movementPeer{a, b} {
		auth := p.call("register", map[string]any{"name": fmt.Sprintf("elem-%d-%d", time.Now().UnixNano()%10000000, i)})
		decode(auth["token"], &tokens[i])
		p.call("login", map[string]any{"token": tokens[i]})
	}
	a.call("move", map[string]any{"seq": 1, "dx": 1, "dy": 0})
	time.Sleep(100 * time.Millisecond)
	a.call("move", map[string]any{"seq": 2, "dx": 0, "dy": 0})
	world := view(a)
	if world.Battle != nil {
		t.Fatal("battle before entry")
	}
	if !accepted(operation(a, "enter_arena", world, nil)) {
		t.Fatal("entry rejected")
	}
	arena := view(a)
	if arena.Battle == nil || arena.Self.Owner <= world.Self.Owner || arena.Self.Session != world.Self.Session {
		t.Fatal("entry ownership invalid")
	}
	if view(b).Battle != nil {
		t.Fatal("other actor entered arena")
	}
	fields := contextFields(arena)
	fields["element"], fields["x"], fields["y"] = "lightning", 4, 4
	id := a.send("cast", fields)
	cooldownFields := contextFields(arena)
	cooldownFields["element"], cooldownFields["x"], cooldownFields["y"] = "water", 1, 4
	cooldownID := a.send("cast", cooldownFields)
	if !accepted(a.receive(id, true)) {
		t.Fatal("conduction rejected")
	}
	if accepted(a.receive(cooldownID, true)) {
		t.Fatal("immediate second cast bypassed cooldown")
	}
	chain := view(a)
	if chain.Battle.Conduction != 1 {
		t.Fatal("conduction state lost")
	}
	a.connection.WriteJSON(fields)
	if !accepted(a.receive(id, true)) {
		t.Fatal("duplicate ack rejected")
	}
	if view(a).Battle.Sequence != chain.Battle.Sequence {
		t.Fatal("duplicate cast changed effects")
	}
	time.Sleep(320 * time.Millisecond)
	if accepted(operation(a, "cast", arena, map[string]any{"element": "water", "x": 12, "y": 8})) {
		t.Fatal("out of range accepted")
	}
	if !accepted(operation(a, "cast", arena, map[string]any{"element": "water", "x": 1, "y": 4})) {
		t.Fatal("water rejected")
	}
	time.Sleep(320 * time.Millisecond)
	if !accepted(operation(a, "cast", arena, map[string]any{"element": "fire", "x": 1, "y": 4})) {
		t.Fatal("fire rejected")
	}
	if view(a).Battle.Steam < 1 {
		t.Fatal("steam missing")
	}
	bw := view(b)
	operation(b, "enter_arena", bw, nil)
	bv := view(b)
	if bv.Battle == nil || bv.Battle.ID == arena.Battle.ID {
		t.Fatal("arenas not isolated")
	}
	for i := 0; i < 4; i++ {
		if view(a).Battle.Phase != "playing" {
			break
		}
		time.Sleep(320 * time.Millisecond)
		result := operation(a, "cast", arena, map[string]any{"element": "lightning", "x": 1, "y": 4})
		if !accepted(result) && string(result["reason"]) != `"battle_finished"` {
			t.Fatal("self strike rejected")
		}
	}
	lost := view(a)
	if lost.Battle.Phase != "lost" || lost.Battle.Player.HP != 0 || !lost.Self.Combat.Dead {
		t.Fatal("loss inconsistent")
	}
	if view(b).Battle.Player.HP != 100 {
		t.Fatal("private arenas shared damage")
	}
	a.call("move", map[string]any{"seq": 3, "dx": 1, "dy": 0})
	time.Sleep(250 * time.Millisecond)
	terminal := view(a)
	if terminal.Battle.Elapsed != lost.Battle.Elapsed || terminal.Self.X != lost.Self.X {
		t.Fatal("terminal rules continued")
	}
	operation(a, "restart_battle", lost, nil)
	restarted := view(a)
	if restarted.Self.Owner <= lost.Self.Owner || restarted.Self.Session != lost.Self.Session || restarted.Battle.ID == lost.Battle.ID || restarted.Battle.Sequence != 0 || restarted.Battle.Player.HP != 100 {
		t.Fatal("restart contract invalid")
	}
	if accepted(operation(a, "cast", lost, map[string]any{"element": "fire", "x": 1, "y": 4})) {
		t.Fatal("old owner affected new battle")
	}
	if view(a).Battle.Sequence != 0 {
		t.Fatal("stale request mutated new battle")
	}
	operation(a, "leave_arena", restarted, nil)
	restored := view(a)
	if restored.Battle != nil || restored.Scene != world.Scene || restored.Self.Owner <= restarted.Self.Owner || restored.Self.Session != world.Self.Session || math.Abs(restored.Self.X-world.Self.X) > 1e-6 || math.Abs(restored.Self.Y-world.Self.Y) > 1e-6 {
		t.Fatal("leave did not restore world")
	}
	a.call("move", map[string]any{"seq": 4, "dx": 0, "dy": 1})
	if view(a).Self.Owner != restored.Self.Owner {
		t.Fatal("movement reset ownership")
	}
	t.Log("elemental feature, single-player isolation, reactions, terminal snapshots, restart fencing, stable login session and return location passed")
}
