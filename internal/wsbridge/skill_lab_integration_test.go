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

func TestSkillLabIntegration(t *testing.T) {
	upstream := os.Getenv("MMO_TEST_SKILL_LAB_GATEWAY")
	if upstream == "" {
		t.Skip("isolated skill lab not configured")
	}
	bridge, err := New(Config{Upstream: upstream, AllowedOrigins: []string{"https://lab.example"}, ProtocolVersion: "webgl-skill-lab-v1"})
	if err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(bridge)
	defer server.Close()
	defer bridge.Close()
	peer := func() *movementPeer {
		dialer := websocket.Dialer{EnableCompression: true}
		c, response, err := dialer.Dial("ws"+strings.TrimPrefix(server.URL, "http")+"/ws", http.Header{"Origin": {"https://lab.example"}})
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(response.Header.Get("Sec-WebSocket-Extensions"), "permessage-deflate") {
			t.Fatal("compression missing")
		}
		t.Cleanup(func() { c.Close() })
		return &movementPeer{t: t, connection: c}
	}
	decode := func(value any, target any) {
		t.Helper()
		raw, _ := json.Marshal(value)
		if err := json.Unmarshal(raw, target); err != nil {
			t.Fatal(err)
		}
	}
	type object struct {
		ID, SkillID                      string
		Speed, Radius, Length, Thickness float64
	}
	type world struct {
		Epoch    string `json:"world_epoch"`
		Features []string
		Self     struct {
			ID     string `json:"role_id"`
			X, Y   float64
			Combat struct {
				HP        int
				Cooldowns map[string]int
			}
		}
		Skills             []json.RawMessage
		Cooldowns          map[string]int `json:"skill_cooldowns"`
		Projectiles, Walls []json.RawMessage
		Events             []struct {
			Type   string
			Skill  string `json:"skill_id"`
			Object string `json:"object_id"`
			Value  int
			Target string `json:"target_id"`
		}
	}
	view := func(p *movementPeer) world {
		var w world
		decode(p.call("view", nil), &w)
		if len(w.Features) != 1 || w.Features[0] != "parameterized_skills_v1" {
			t.Fatal("capability missing")
		}
		return w
	}
	a, b := peer(), peer()
	var credentials [2]map[string]json.RawMessage
	for i, p := range []*movementPeer{a, b} {
		credentials[i] = p.call("register", map[string]any{"name": fmt.Sprintf("lab-%d-%d", time.Now().UnixNano()%10000000, i)})
		var token string
		decode(credentials[i]["token"], &token)
		p.call("login", map[string]any{"token": token})
	}
	var protocol string
	decode(a.call("ping", nil)["protocol_version"], &protocol)
	if protocol != "webgl-skill-lab-v1" {
		t.Fatal(protocol)
	}
	if len(view(a).Skills) != 0 {
		t.Fatal("server invented client skills before initialization")
	}
	var moveSeq int
	for deadline := time.Now().Add(5 * time.Second); ; {
		w := view(b)
		delta := 6 - w.Self.X
		if math.Abs(delta) < .4 {
			moveSeq++
			b.call("move", map[string]any{"seq": moveSeq, "dx": 0, "dy": 0})
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("movement setup timed out")
		}
		moveSeq++
		b.call("move", map[string]any{"seq": moveSeq, "dx": math.Copysign(1, delta), "dy": 0})
		time.Sleep(60 * time.Millisecond)
	}
	epoch := view(a).Epoch
	definition := map[string]any{"type": "projectile", "damage": 17, "mp_cost": 0, "cooldown_ms": 150, "range": 30, "speed": 12, "radius": .15}
	init := func(p *movementPeer, id string, parameters map[string]any) uint64 {
		var r struct {
			Accepted   bool
			Reason     string
			Definition struct {
				Version uint64 `json:"definition_version"`
			}
		}
		decode(p.call("skill_init", map[string]any{"skill_id": id, "definition": parameters, "world_epoch": epoch}), &r)
		if !r.Accepted || r.Definition.Version == 0 {
			t.Fatalf("init rejected: %+v", r)
		}
		return r.Definition.Version
	}
	version := init(a, "client_bolt_7", definition)
	if init(a, "client_bolt_7", definition) != version {
		t.Fatal("same init changed version")
	}
	other := map[string]any{"type": "projectile", "damage": 2, "mp_cost": 0, "cooldown_ms": 150, "range": 30, "speed": 8, "radius": .1}
	init(b, "client_bolt_7", other)
	cast := func(seq int, id string, v uint64, extra map[string]any) map[string]json.RawMessage {
		fields := map[string]any{"seq": seq, "skill_id": id, "definition_version": v, "dx": 1, "dy": 0, "world_epoch": epoch}
		for k, value := range extra {
			fields[k] = value
		}
		return a.call("cast", fields)
	}
	first := cast(1, "client_bolt_7", version, nil)
	var accepted bool
	decode(first["accepted"], &accepted)
	if !accepted {
		t.Fatal("cast rejected")
	}
	var projectile struct {
		ID            string
		Speed, Radius float64
		Skill         string `json:"skill_id"`
	}
	decode(first["projectile"], &projectile)
	if projectile.Skill != "client_bolt_7" || projectile.Speed != 12 || projectile.Radius != .15 {
		t.Fatal("instance ignored initialized values")
	}
	var retry struct{ ID string }
	decode(cast(1, "client_bolt_7", version, nil)["projectile"], &retry)
	if retry.ID != projectile.ID {
		t.Fatal("retry duplicated projectile")
	}
	observed := false
	for _, raw := range view(b).Projectiles {
		var p struct {
			ID    string
			Speed float64
		}
		decode(raw, &p)
		if p.ID == projectile.ID && p.Speed == 12 {
			observed = true
		}
	}
	if !observed {
		t.Fatal("observer rebuilt projectile from its own different definition")
	}
	wait := func(predicate func(world) bool) {
		t.Helper()
		for deadline := time.Now().Add(3 * time.Second); time.Now().Before(deadline); {
			if predicate(view(b)) {
				return
			}
			time.Sleep(30 * time.Millisecond)
		}
		t.Fatal("expected authoritative effect not observed")
	}
	target := view(b).Self.ID
	wait(func(w world) bool {
		for _, e := range w.Events {
			if e.Type == "damage" && e.Object == projectile.ID && e.Target == target && e.Value == 17 {
				return true
			}
		}
		return false
	})
	barrier := map[string]any{"type": "wall", "damage": 0, "mp_cost": 0, "cooldown_ms": 5000, "range": 8, "length": 4, "thickness": 1, "duration_ms": 5000}
	wallVersion := init(a, "client_barrier", barrier)
	wall := cast(2, "client_barrier", wallVersion, map[string]any{"x": 3, "y": 0})
	decode(wall["accepted"], &accepted)
	if !accepted {
		t.Fatal("wall rejected")
	}
	var geometry struct {
		ID                string
		Length, Thickness float64
	}
	decode(wall["wall"], &geometry)
	if geometry.Length != 4 || geometry.Thickness != 1 {
		t.Fatal("wrong server wall dimensions")
	}
	blocked := cast(3, "client_bolt_7", version, nil)
	var blockedObject struct{ ID string }
	decode(blocked["projectile"], &blockedObject)
	wait(func(w world) bool {
		for _, e := range w.Events {
			if e.Type == "projectile_blocked" && e.Object == blockedObject.ID {
				return true
			}
		}
		return false
	})
	var reason string
	decode(cast(4, "client_bolt_7", version+999, nil)["reason"], &reason)
	if reason != "definition_changed" {
		t.Fatal("stale definition accepted")
	}
	bad := map[string]any{"type": "arbitrary_code"}
	decode(a.call("skill_init", map[string]any{"skill_id": "invalid", "definition": bad, "world_epoch": epoch})["reason"], &reason)
	if reason != "unknown_behavior" {
		t.Fatal(reason)
	}
	a.call("ping", nil)
	replacement := peer()
	var token string
	decode(credentials[0]["token"], &token)
	replacement.call("login", map[string]any{"token": token})
	a.connection.Close()
	latest := view(replacement)
	if len(latest.Walls) != 0 || len(latest.Skills) != 2 || latest.Cooldowns["client_barrier"] <= 0 || len(latest.Self.Combat.Cooldowns) != 6 {
		t.Fatal("reconnect reset custom definitions/cooldown or corrupted persistent schema")
	}
	t.Log("PASS client-defined IDs/parameters, shared instance observation, configured PvP damage, wall blocking, retry/version fencing, reconnect and compression")
}
