package gateway

import (
	"bufio"
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"testing"
	"time"
)

func TestCombatRequestsUseSessionIdentityAndKeepView(t *testing.T) {
	key := strings.Repeat("c", 32)
	combat := json.RawMessage(`{"hp":71,"max_hp":180,"mp":37,"max_mp":100,"dead":false}`)
	role := Role{ID: "actor", Name: "combat", Scene: "scene-1", Owner: 3, Session: 4, Coins: 9, Combat: combat}
	called := make(chan string, 8)
	scene := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var in map[string]any
		if err := json.NewDecoder(r.Body).Decode(&in); err != nil {
			t.Error(err)
		}
		if in["role_id"] != "actor" || in["owner_epoch"] != float64(3) || in["session_epoch"] != float64(4) || r.Header.Get("X-Internal-Token") != key {
			t.Errorf("无效可信Ref: %v", in)
		}
		if r.URL.Path == "/v1/view" {
			json.NewEncoder(w).Encode(View{Self: role, Players: []Role{role}, Scene: "scene-1", Mobs: json.RawMessage(`[{"id":"npc-1"}]`), Skills: json.RawMessage(`[{"id":"strike"}]`), Events: json.RawMessage(`[{"id":1,"type":"damage"}]`)})
			return
		}
		if r.URL.Path == "/v1/cast" && (in["seq"] != float64(8) || in["skill_id"] != "strike" || in["target_id"] != "npc-1") {
			t.Errorf("施法字段丢失: %v", in)
		}
		called <- r.URL.Path
		json.NewEncoder(w).Encode(map[string]any{"accepted": true, "role": role})
	}))
	defer scene.Close()
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/v1/checkpoint" {
			json.NewEncoder(w).Encode(map[string]bool{"ok": true})
			return
		}
		json.NewEncoder(w).Encode(Route{Role: role, URL: scene.URL})
	}))
	defer backend.Close()
	s, err := New(Config{Token: key, BackendURL: backend.URL})
	if err != nil {
		t.Fatal(err)
	}
	server, peer := net.Pipe()
	defer peer.Close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan struct{})
	go func() { defer close(done); s.serveConn(ctx, server) }()
	scanner := bufio.NewScanner(peer)
	exchange := func(request Request) Response {
		t.Helper()
		peer.SetDeadline(time.Now().Add(3 * time.Second))
		if err := json.NewEncoder(peer).Encode(request); err != nil {
			t.Fatal(err)
		}
		for scanner.Scan() {
			var reply Response
			if err := json.Unmarshal(scanner.Bytes(), &reply); err != nil {
				t.Fatal(err)
			}
			if reply.ID == request.ID {
				return reply
			}
		}
		t.Fatalf("响应连接关闭: %v", scanner.Err())
		return Response{}
	}
	if exchange(Request{ID: "pre", Op: "cast", Seq: 1, SkillID: "strike", TargetID: "npc-1"}).OK {
		t.Fatal("未登录施法被接受")
	}
	if !exchange(Request{ID: "login", Op: "login", Token: "test"}).OK {
		t.Fatal("登录失败")
	}
	if exchange(Request{ID: "invalid", Op: "cast", SkillID: "strike"}).OK {
		t.Fatal("缺少序号的施法被接受")
	}
	if !exchange(Request{ID: "cast", Op: "cast", Seq: 8, SkillID: "strike", TargetID: "npc-1"}).OK {
		t.Fatal("施法失败")
	}
	if !exchange(Request{ID: "revive", Op: "revive", Seq: 9}).OK {
		t.Fatal("复活失败")
	}
	view := exchange(Request{ID: "view", Op: "view"})
	if !view.OK {
		t.Fatal("视图失败")
	}
	data, _ := json.Marshal(view.Data)
	var actual View
	if err := json.Unmarshal(data, &actual); err != nil {
		t.Fatal(err)
	}
	var actualCombat, expectedCombat map[string]any
	json.Unmarshal(actual.Self.Combat, &actualCombat)
	json.Unmarshal(combat, &expectedCombat)
	if len(actual.Mobs) == 0 || len(actual.Skills) == 0 || len(actual.Events) == 0 || !reflect.DeepEqual(actualCombat, expectedCombat) {
		t.Fatalf("战斗视图丢失: %s", data)
	}
	if a, b := <-called, <-called; a != "/v1/cast" || b != "/v1/revive" {
		t.Fatal(a, b)
	}
	peer.Close()
	cancel()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("连接未结束")
	}
}
