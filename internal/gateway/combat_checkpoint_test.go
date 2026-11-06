package gateway

import (
	"bufio"
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestCombatActionImmediatelyCheckpointsAndKeepsCoins(t *testing.T) {
	for _, operation := range []string{"cast", "revive"} {
		t.Run(operation, func(t *testing.T) {
			key := strings.Repeat("d", 32)
			role := Role{ID: "actor", Name: "action", Scene: "scene-1", Owner: 2, Session: 3, Coins: 100, Combat: json.RawMessage(`{"hp":180,"revision":1}`)}
			latest := role
			latest.Coins = 0
			latest.Combat = json.RawMessage(`{"hp":81,"revision":9,"cooldowns":{"strike":600}}`)
			checkpoints := make(chan Role, 8)
			scene := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path == "/v1/view" {
					json.NewEncoder(w).Encode(View{Self: role, Players: []Role{role}, Scene: role.Scene})
					return
				}
				json.NewEncoder(w).Encode(map[string]any{"accepted": true, "role": latest})
			}))
			defer scene.Close()
			backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path == "/v1/checkpoint" {
					var payload struct {
						Role Role `json:"role"`
					}
					if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
						t.Error(err)
					}
					checkpoints <- payload.Role
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
				t.Fatal("连接在响应前中断")
				return Response{}
			}
			if !exchange(Request{ID: "login", Op: "login", Token: "test"}).OK {
				t.Fatal("登录失败")
			}
			reply := exchange(Request{ID: "action", Op: operation, Seq: 8, SkillID: "strike", TargetID: "npc-1"})
			if !reply.OK {
				t.Fatalf("动作失败: %+v", reply)
			}
			encoded, _ := json.Marshal(reply.Data)
			var payload struct {
				Role Role `json:"role"`
			}
			if err := json.Unmarshal(encoded, &payload); err != nil {
				t.Fatal(err)
			}
			if payload.Role.Coins != 100 {
				t.Fatalf("动作回执回退了金币: %s", encoded)
			}
			peer.Close()
			cancel()
			select {
			case <-done:
			case <-time.After(3 * time.Second):
				t.Fatal("断开后未保存快照")
			}
			if len(checkpoints) == 0 {
				t.Fatal("成功动作没有保存快照")
			}
			for len(checkpoints) > 0 {
				checkpoint := <-checkpoints
				if string(checkpoint.Combat) != string(latest.Combat) || checkpoint.Coins != 100 {
					t.Fatalf("正常断开保存了旧动作状态: %+v", checkpoint)
				}
			}
		})
	}
}

func TestActionRetryDoesNotReplaceNewerView(t *testing.T) {
	role := Role{ID: "a", Scene: "scene-1", Owner: 1, Session: 1, Coins: 100, Combat: json.RawMessage(`{"hp":62,"revision":20}`)}
	previous := &View{Self: role, Scene: role.Scene}
	old := role
	old.Combat = json.RawMessage(`{"hp":180,"revision":4}`)
	old.Coins = 0
	data := map[string]json.RawMessage{}
	data["role"], _ = json.Marshal(old)
	actual, ok := actionSnapshot(data, Route{Role: role}, previous)
	if !ok || actual != previous {
		t.Fatal("历史幂等回执替换了更新视图")
	}
	var displayed Role
	json.Unmarshal(data["role"], &displayed)
	if displayed.Coins != 100 {
		t.Fatal("历史回执的金币没有覆盖为Backend余额")
	}
}
