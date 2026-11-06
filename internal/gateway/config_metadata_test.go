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

func TestConfigMetadataPassesThroughGateway(t *testing.T) {
	for _, version := range []string{"", "balance-20261106"} {
		name := version
		if name == "" {
			name = "legacy-scene"
		}
		t.Run(name, func(t *testing.T) {
			role := Role{ID: "actor", Name: "config", Scene: "scene-1", Owner: 3, Session: 4, Coins: 9}
			// 使用完整 uint64 范围，确保网关不会经 float64 丢失版本代次数值。
			generation, appliedTick := ^uint64(0)-1, ^uint64(0)-2
			scene := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				out := map[string]any{"self": role, "players": []Role{role}, "scene_id": role.Scene, "tick": 10}
				if r.URL.Path == "/v1/cast" {
					out = map[string]any{"accepted": true, "seq": 1, "skill_id": "strike", "role": role}
				}
				if version != "" {
					out["config_version"] = version
					if r.URL.Path == "/v1/view" {
						out["config_generation"], out["config_applied_tick"] = generation, appliedTick
					}
				}
				json.NewEncoder(w).Encode(out)
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
			s, err := New(Config{Token: strings.Repeat("c", 32), BackendURL: backend.URL})
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
			exchange := func(req Request) map[string]json.RawMessage {
				t.Helper()
				peer.SetDeadline(time.Now().Add(3 * time.Second))
				if err := json.NewEncoder(peer).Encode(req); err != nil {
					t.Fatal(err)
				}
				for scanner.Scan() {
					var reply struct {
						ID    string                     `json:"id"`
						OK    bool                       `json:"ok"`
						Data  map[string]json.RawMessage `json:"data"`
						Error string                     `json:"error"`
					}
					if err := json.Unmarshal(scanner.Bytes(), &reply); err != nil {
						t.Fatal(err)
					}
					if reply.ID == req.ID {
						if !reply.OK {
							t.Fatalf("%s 失败: %s", req.Op, reply.Error)
						}
						return reply.Data
					}
				}
				t.Fatalf("响应连接关闭: %v", scanner.Err())
				return nil
			}
			exchange(Request{ID: "login", Op: "login", Token: "test"})
			view := exchange(Request{ID: "view", Op: "view"})
			cast := exchange(Request{ID: "cast", Op: "cast", Seq: 1, SkillID: "strike", TargetID: "npc-1"})
			if version == "" {
				for _, field := range []string{"config_version", "config_generation", "config_applied_tick"} {
					if _, ok := view[field]; ok {
						t.Fatalf("旧场景出现多余字段 %s: %s", field, view[field])
					}
				}
				if _, ok := cast["config_version"]; ok {
					t.Fatal("旧施法回执出现多余配置版本")
				}
			} else {
				for _, data := range []map[string]json.RawMessage{view, cast} {
					var actual string
					if err := json.Unmarshal(data["config_version"], &actual); err != nil || actual != version {
						t.Fatalf("配置版本丢失: %s (%v)", data["config_version"], err)
					}
				}
				for field, want := range map[string]uint64{"config_generation": generation, "config_applied_tick": appliedTick} {
					var actual uint64
					if err := json.Unmarshal(view[field], &actual); err != nil || actual != want {
						t.Fatalf("%s 丢失精度: %s (%v)", field, view[field], err)
					}
				}
			}
			peer.Close()
			cancel()
			select {
			case <-done:
			case <-time.After(3 * time.Second):
				t.Fatal("连接未结束")
			}
		})
	}
}
