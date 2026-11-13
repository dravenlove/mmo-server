// 构造原Go Backend真实数据库，供C++跨语言接管测试；只接受新建的临时输出目录。
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"

	"mmo/internal/backend"
)

const token = "native-legacy-test-internal-token-at-least-32"

type fakeScene struct {
	mu           sync.Mutex
	players      map[string]json.RawMessage
	failActivate bool
}

func (f *fakeScene) serve(w http.ResponseWriter, r *http.Request) {
	f.mu.Lock()
	defer f.mu.Unlock()
	var req struct {
		Role   json.RawMessage `json:"role"`
		RoleID string          `json:"role_id"`
	}
	if json.NewDecoder(r.Body).Decode(&req) != nil {
		w.WriteHeader(400)
		return
	}
	op := strings.TrimPrefix(r.URL.Path, "/v1/")
	if op == "enter" || op == "prepare" {
		var role struct {
			RoleID string `json:"role_id"`
		}
		json.Unmarshal(req.Role, &role)
		f.players[role.RoleID] = req.Role
		json.NewEncoder(w).Encode(map[string]any{"role": req.Role})
		return
	}
	if op == "remove" {
		delete(f.players, req.RoleID)
		json.NewEncoder(w).Encode(map[string]bool{"accepted": true})
		return
	}
	role, exists := f.players[req.RoleID]
	if !exists {
		w.WriteHeader(404)
		return
	}
	if op == "activate" && f.failActivate {
		f.failActivate = false
		w.WriteHeader(503)
		return
	}
	json.NewEncoder(w).Encode(map[string]any{"role": role})
}
func main() {
	out := flag.String("output-dir", "", "must be a new temporary directory")
	flag.Parse()
	if *out == "" || !strings.Contains(filepath.Base(*out), "mmo-native-legacy-") {
		panic("requires named temporary fixture directory")
	}
	if err := os.Mkdir(*out, 0700); err != nil {
		panic("fixture directory must not exist")
	}
	first, second := &fakeScene{players: map[string]json.RawMessage{}}, &fakeScene{players: map[string]json.RawMessage{}, failActivate: true}
	firstHTTP, secondHTTP := httptest.NewServer(http.HandlerFunc(first.serve)), httptest.NewServer(http.HandlerFunc(second.serve))
	defer firstHTTP.Close()
	defer secondHTTP.Close()
	path := filepath.Join(*out, "legacy.db")
	service, err := backend.New(backend.Config{Token: token, DBDriver: "sqlite", DBPath: path, Scenes: map[string]string{"scene-1": firstHTTP.URL, "scene-2": secondHTTP.URL}, DefaultScene: "scene-1"})
	if err != nil {
		panic(err)
	}
	call := func(op string, request any, expected int) json.RawMessage {
		body, _ := json.Marshal(request)
		r := httptest.NewRequest(http.MethodPost, "/v1/"+op, bytes.NewReader(body))
		r.Header.Set("X-Internal-Token", token)
		w := httptest.NewRecorder()
		service.Handler().ServeHTTP(w, r)
		if w.Code != expected {
			panic(fmt.Sprintf("legacy fixture %s status=%d", op, w.Code))
		}
		return w.Body.Bytes()
	}
	var identity struct {
		Token  string `json:"token"`
		RoleID string `json:"role_id"`
	}
	if json.Unmarshal(call("register", map[string]string{"name": "legacy-native-player"}, 200), &identity) != nil {
		panic("identity")
	}
	var route backend.Route
	json.Unmarshal(call("login", map[string]string{"token": identity.Token}, 200), &route)
	call("claim", map[string]any{"role_id": route.Role.RoleID, "session_epoch": route.Role.SessionEpoch}, 200)
	role := route.Role
	role.X = 7
	role.Y = 9
	role.Coins = 100
	role.Combat = json.RawMessage(`{"hp":71,"max_hp":180,"mp":37,"max_mp":100,"dead":false,"revive_remaining_ms":0,"revision":8,"last_action_seq":4,"action_session":1,"cooldowns":{"strike":500,"whirlwind":2500,"charge":6000,"mend":11000}}`)
	call("checkpoint", map[string]any{"role": role}, 200)
	first.mu.Lock()
	raw, _ := json.Marshal(role)
	first.players[role.RoleID] = raw
	first.mu.Unlock()
	call("transfer", map[string]any{"role_id": role.RoleID, "owner_epoch": role.OwnerEpoch, "session_epoch": role.SessionEpoch, "target_scene_id": "scene-2", "request_id": "legacy-pending-commit"}, 503)
	if err := service.Close(); err != nil {
		panic(err)
	}
	// 数据库必须先释放独占锁，再让C++进程接管。
	data, _ := json.Marshal(map[string]any{"database": path, "token": identity.Token, "role_id": role.RoleID})
	if err := os.WriteFile(filepath.Join(*out, "fixture.json"), data, 0600); err != nil {
		panic(err)
	}
}
