package backend

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

const testToken = "test-internal-token-at-least-32-characters"

type fakePlayer struct {
	Role              Role
	State, TransferID string
}
type fakeScene struct {
	mu         sync.Mutex
	server     *httptest.Server
	players    map[string]fakePlayer
	tombstones map[string]int64
	failBefore map[string]int
	failAfter  map[string]int
}

func newFakeScene(t *testing.T) *fakeScene {
	t.Helper()
	f := &fakeScene{players: map[string]fakePlayer{}, tombstones: map[string]int64{}, failBefore: map[string]int{}, failAfter: map[string]int{}}
	f.server = httptest.NewServer(http.HandlerFunc(f.serve))
	t.Cleanup(f.server.Close)
	return f
}

func (f *fakeScene) serve(w http.ResponseWriter, r *http.Request) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if r.Header.Get("X-Internal-Token") != testToken {
		w.WriteHeader(401)
		return
	}
	op := strings.TrimPrefix(r.URL.Path, "/v1/")
	if f.failBefore[op] > 0 {
		f.failBefore[op]--
		w.WriteHeader(503)
		return
	}
	var req struct {
		Ref
		Role       Role   `json:"role"`
		TransferID string `json:"transfer_id"`
	}
	if json.NewDecoder(r.Body).Decode(&req) != nil {
		w.WriteHeader(400)
		return
	}
	if op == "enter" || op == "prepare" {
		req.Ref = req.Role.Ref()
	}
	p, exists := f.players[req.RoleID]
	response := any(map[string]bool{"accepted": true})
	reject := func() { w.WriteHeader(409) }
	switch op {
	case "enter":
		if exists {
			if p.State != "active" || p.Role.OwnerEpoch != req.Role.OwnerEpoch || p.Role.SessionEpoch > req.Role.SessionEpoch {
				reject()
				return
			}
			p.Role.SessionEpoch = req.Role.SessionEpoch
			p.Role.Coins = req.Role.Coins
		} else {
			if f.tombstones[req.RoleID] >= req.OwnerEpoch {
				reject()
				return
			}
			p = fakePlayer{Role: req.Role, State: "active"}
		}
		f.players[req.RoleID] = p
		response = roleResult{p.Role}
	case "prepare":
		if exists {
			if p.Role.Ref() != req.Ref || p.TransferID != req.TransferID {
				reject()
				return
			}
		} else {
			if f.tombstones[req.RoleID] >= req.OwnerEpoch {
				reject()
				return
			}
			p = fakePlayer{Role: req.Role, State: "prepared", TransferID: req.TransferID}
			f.players[req.RoleID] = p
		}
		response = roleResult{p.Role}
	case "freeze":
		if !exists {
			w.WriteHeader(404)
			return
		}
		if p.Role.Ref() != req.Ref || (p.State == "frozen" && p.TransferID != req.TransferID) {
			reject()
			return
		}
		p.State = "frozen"
		p.TransferID = req.TransferID
		f.players[req.RoleID] = p
		response = roleResult{p.Role}
	case "unfreeze":
		if !exists {
			w.WriteHeader(404)
			return
		}
		if p.Role.Ref() != req.Ref || (p.State == "frozen" && p.TransferID != req.TransferID) {
			reject()
			return
		}
		p.State = "active"
		f.players[req.RoleID] = p
	case "activate":
		if !exists {
			w.WriteHeader(404)
			return
		}
		if p.Role.Ref() != req.Ref || p.TransferID != req.TransferID {
			reject()
			return
		}
		p.State = "active"
		f.players[req.RoleID] = p
		response = roleResult{p.Role}
	case "remove":
		if exists && p.Role.Ref() != req.Ref {
			reject()
			return
		}
		delete(f.players, req.RoleID)
		if f.tombstones[req.RoleID] < req.OwnerEpoch {
			f.tombstones[req.RoleID] = req.OwnerEpoch
		}
	default:
		w.WriteHeader(404)
		return
	}
	if f.failAfter[op] > 0 {
		f.failAfter[op]--
		w.WriteHeader(503)
		return
	}
	_ = json.NewEncoder(w).Encode(response)
}

func (f *fakeScene) fail(op string, after bool, count int) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if after {
		f.failAfter[op] = count
	} else {
		f.failBefore[op] = count
	}
}
func (f *fakeScene) player(id string) (fakePlayer, bool) {
	f.mu.Lock()
	defer f.mu.Unlock()
	p, ok := f.players[id]
	return p, ok
}
func (f *fakeScene) reset() {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.players = map[string]fakePlayer{}
	f.tombstones = map[string]int64{}
}

type fixture struct {
	s              *Server
	cfg            Config
	source, target *fakeScene
}

func newFixture(t *testing.T) *fixture {
	t.Helper()
	source, target := newFakeScene(t), newFakeScene(t)
	cfg := Config{Token: testToken, DBPath: filepath.Join(t.TempDir(), "state.db"), Scenes: map[string]string{"scene-1": source.server.URL, "scene-2": target.server.URL}, DefaultScene: "scene-1"}
	s, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}
	f := &fixture{s: s, cfg: cfg, source: source, target: target}
	t.Cleanup(func() {
		if f.s != nil {
			f.s.Close()
		}
	})
	return f
}

func (f *fixture) restart(t *testing.T) {
	t.Helper()
	if err := f.s.Close(); err != nil {
		t.Fatal(err)
	}
	f.s = nil
	s, err := New(f.cfg)
	if err != nil {
		t.Fatal(err)
	}
	f.s = s
}

func post(s *Server, path string, data any) (int, []byte) {
	body, _ := json.Marshal(data)
	req := httptest.NewRequest(http.MethodPost, "/v1/"+path, bytes.NewReader(body))
	req.Header.Set("X-Internal-Token", testToken)
	w := httptest.NewRecorder()
	s.Handler().ServeHTTP(w, req)
	return w.Code, w.Body.Bytes()
}

func wantPost(t *testing.T, s *Server, path string, data any, status int, out any) {
	t.Helper()
	code, body := post(s, path, data)
	if code != status {
		t.Fatalf("%s: status=%d want=%d body=%s", path, code, status, body)
	}
	if out != nil {
		if err := json.Unmarshal(body, out); err != nil {
			t.Fatal(err)
		}
	}
}

func registerLogin(t *testing.T, f *fixture, name string) (string, Route) {
	t.Helper()
	var registered struct{ Token, RoleID string }
	wantPost(t, f.s, "register", map[string]string{"name": name}, 200, &registered)
	var route Route
	wantPost(t, f.s, "login", map[string]string{"token": registered.Token}, 200, &route)
	if route.Role.RoleID == "" || route.Role.SessionEpoch != 1 || route.Role.OwnerEpoch != 1 {
		t.Fatalf("bad initial route: %+v", route)
	}
	return registered.Token, route
}

func TestCredentialsSessionAndDurability(t *testing.T) {
	f := newFixture(t)
	token, route := registerLogin(t, f, "Alice")
	wantPost(t, f.s, "register", map[string]string{"name": "Alice"}, 409, nil)
	wantPost(t, f.s, "login", map[string]string{"token": strings.Repeat("x", 64)}, 401, nil)
	wantPost(t, f.s, "register", map[string]string{"name": "Bob", "extra": "bad"}, 400, nil)
	var claim struct {
		Coins   int64
		Granted bool
	}
	wantPost(t, f.s, "claim", sessionRequest{route.Role.RoleID, route.Role.SessionEpoch}, 200, &claim)
	if claim.Coins != 100 || !claim.Granted {
		t.Fatalf("bad claim: %+v", claim)
	}
	wantPost(t, f.s, "claim", sessionRequest{route.Role.RoleID, route.Role.SessionEpoch}, 200, &claim)
	if claim.Coins != 100 || claim.Granted {
		t.Fatalf("duplicate claim changed assets: %+v", claim)
	}
	snapshot := route.Role
	snapshot.X = 12
	snapshot.Coins = 999999
	wantPost(t, f.s, "checkpoint", roleResult{snapshot}, 200, nil)
	stored, err := f.s.role(context.Background(), snapshot.RoleID)
	if err != nil {
		t.Fatal(err)
	}
	if stored.X != 12 || stored.Coins != 100 {
		t.Fatalf("checkpoint overwrote assets: %+v", stored)
	}
	// 模拟场景进程丢失内存，重新登录从已持久快照恢复。
	f.source.reset()
	f.restart(t)
	var next Route
	wantPost(t, f.s, "login", map[string]string{"token": token}, 200, &next)
	if next.Role.Coins != 100 || next.Role.X != 12 || next.Role.SessionEpoch != 2 {
		t.Fatalf("durability failed: %+v", next)
	}
	wantPost(t, f.s, "claim", sessionRequest{route.Role.RoleID, route.Role.SessionEpoch}, 409, nil)
	wantPost(t, f.s, "checkpoint", roleResult{snapshot}, 409, nil)
	wantPost(t, f.s, "claim", sessionRequest{next.Role.RoleID, next.Role.SessionEpoch}, 200, &claim)
	if claim.Granted || claim.Coins != 100 {
		t.Fatal("restart duplicated reward")
	}
	var storedHash string
	if err = f.s.db.QueryRow("SELECT token_hash FROM roles WHERE role_id=?", next.Role.RoleID).Scan(&storedHash); err != nil {
		t.Fatal(err)
	}
	if storedHash == token || storedHash != tokenHash(token) {
		t.Fatal("raw token persisted")
	}
}

func TestConcurrentClaimOnlyOnce(t *testing.T) {
	f := newFixture(t)
	_, route := registerLogin(t, f, "Alice")
	var wg sync.WaitGroup
	var mu sync.Mutex
	granted := 0
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			code, body := post(f.s, "claim", sessionRequest{route.Role.RoleID, route.Role.SessionEpoch})
			if code != 200 {
				t.Errorf("claim status %d: %s", code, body)
				return
			}
			var result struct {
				Coins   int64
				Granted bool
			}
			if err := json.Unmarshal(body, &result); err != nil {
				t.Error(err)
				return
			}
			if result.Coins != 100 {
				t.Errorf("coins=%d", result.Coins)
			}
			if result.Granted {
				mu.Lock()
				granted++
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	if granted != 1 {
		t.Fatalf("grant count=%d", granted)
	}
}

func TestMigrationIdempotencyAndStaleCheckpoint(t *testing.T) {
	f := newFixture(t)
	_, route := registerLogin(t, f, "Alice")
	f.source.mu.Lock()
	player := f.source.players[route.Role.RoleID]
	player.Role.X = 8
	f.source.players[route.Role.RoleID] = player
	f.source.mu.Unlock()
	req := transferRequest{Ref: route.Role.Ref(), TargetSceneID: "scene-2", RequestID: "first"}
	var moved Route
	wantPost(t, f.s, "transfer", req, 200, &moved)
	if moved.Role.SceneID != "scene-2" || moved.Role.OwnerEpoch != 2 || moved.Role.X != 8 {
		t.Fatalf("bad migration: %+v", moved)
	}
	if _, exists := f.source.player(route.Role.RoleID); exists {
		t.Fatal("source not removed")
	}
	if p, _ := f.target.player(route.Role.RoleID); p.State != "active" {
		t.Fatal("target not active")
	}
	wantPost(t, f.s, "transfer", req, 200, &moved)
	if moved.Role.OwnerEpoch != 2 {
		t.Fatal("duplicate migration incremented epoch")
	}
	wantPost(t, f.s, "checkpoint", roleResult{route.Role}, 409, nil)
	wrong := req
	wrong.TargetSceneID = "scene-1"
	wantPost(t, f.s, "transfer", wrong, 409, nil)
	back := transferRequest{Ref: moved.Role.Ref(), TargetSceneID: "scene-1", RequestID: "back"}
	wantPost(t, f.s, "transfer", back, 200, &moved)
	if moved.Role.OwnerEpoch != 3 || moved.Role.SceneID != "scene-1" {
		t.Fatal("return migration failed")
	}
	wantPost(t, f.s, "transfer", req, 200, &moved)
	if moved.Role.OwnerEpoch != 3 || moved.Role.SceneID != "scene-1" {
		t.Fatal("old replay migrated player again")
	}
}

func TestPrepareFailureConsumesEpochAndCancels(t *testing.T) {
	f := newFixture(t)
	_, route := registerLogin(t, f, "Alice")
	f.target.fail("prepare", true, 1)
	req := transferRequest{Ref: route.Role.Ref(), TargetSceneID: "scene-2", RequestID: "fail-prepare"}
	wantPost(t, f.s, "transfer", req, 503, nil)
	stored, err := f.s.role(context.Background(), route.Role.RoleID)
	if err != nil {
		t.Fatal(err)
	}
	if stored.SceneID != "scene-1" || stored.OwnerEpoch != 1 {
		t.Fatal("precommit failure changed ownership")
	}
	if p, _ := f.source.player(stored.RoleID); p.State != "active" {
		t.Fatal("source not unfrozen")
	}
	if _, exists := f.target.player(stored.RoleID); exists {
		t.Fatal("target candidate not deleted")
	}
	wantPost(t, f.s, "transfer", req, 409, nil)
	req.RequestID = "retry-new"
	var moved Route
	wantPost(t, f.s, "transfer", req, 200, &moved)
	if moved.Role.OwnerEpoch != 3 {
		t.Fatalf("failed epoch reused: %d", moved.Role.OwnerEpoch)
	}
}

func TestCommittedResponseLossRecoveredAfterRestart(t *testing.T) {
	f := newFixture(t)
	token, route := registerLogin(t, f, "Alice")
	f.target.fail("activate", true, 1)
	req := transferRequest{Ref: route.Role.Ref(), TargetSceneID: "scene-2", RequestID: "lost-response"}
	wantPost(t, f.s, "transfer", req, 503, nil)
	m, err := f.s.migration(context.Background(), route.Role.RoleID, req.RequestID)
	if err != nil {
		t.Fatal(err)
	}
	if m.Status != "COMMITTED" {
		t.Fatalf("state=%s", m.Status)
	}
	stored, err := f.s.role(context.Background(), route.Role.RoleID)
	if err != nil {
		t.Fatal(err)
	}
	if stored.SceneID != "scene-2" || stored.OwnerEpoch != 2 {
		t.Fatal("committed ownership rolled back")
	}
	// Backend 与目标场景同时重启，持久记录足够重建待激活对象。
	f.target.reset()
	f.restart(t)
	if err = f.s.Recover(context.Background()); err != nil {
		t.Fatal(err)
	}
	var current Route
	wantPost(t, f.s, "login", map[string]string{"token": token}, 200, &current)
	if current.Role.SceneID != "scene-2" || current.Role.OwnerEpoch != 2 || current.Role.SessionEpoch != 2 {
		t.Fatalf("recovery wrong route: %+v", current)
	}
	if _, exists := f.source.player(route.Role.RoleID); exists {
		t.Fatal("stale source remains")
	}
	if p, _ := f.target.player(route.Role.RoleID); p.State != "active" {
		t.Fatal("target not active")
	}
}

func TestAbortingRecoveryWaitsForScene(t *testing.T) {
	f := newFixture(t)
	_, route := registerLogin(t, f, "Alice")
	f.target.fail("prepare", true, 1)
	f.target.fail("remove", false, 1)
	req := transferRequest{Ref: route.Role.Ref(), TargetSceneID: "scene-2", RequestID: "rollback-pending"}
	wantPost(t, f.s, "transfer", req, 503, nil)
	m, err := f.s.pending(context.Background(), route.Role.RoleID)
	if err != nil {
		t.Fatal(err)
	}
	if m.Status != "ABORTING" {
		t.Fatalf("state=%s", m.Status)
	}
	if p, _ := f.source.player(route.Role.RoleID); p.State != "frozen" {
		t.Fatal("source prematurely resumed")
	}
	f.restart(t)
	var recovered Route
	wantPost(t, f.s, "session", sessionRequest{route.Role.RoleID, route.Role.SessionEpoch}, 200, &recovered)
	if recovered.Role.SceneID != "scene-1" || recovered.Role.OwnerEpoch != 1 {
		t.Fatal("rollback recovered wrong owner")
	}
	if p, _ := f.source.player(route.Role.RoleID); p.State != "active" {
		t.Fatal("source not resumed")
	}
	if _, exists := f.target.player(route.Role.RoleID); exists {
		t.Fatal("prepared target remains")
	}
}

func TestCrashBeforeCommitRollsBackPreparedCandidate(t *testing.T) {
	f := newFixture(t)
	_, route := registerLogin(t, f, "Alice")
	req := transferRequest{Ref: route.Role.Ref(), TargetSceneID: "scene-2", RequestID: "crash-before-commit"}
	m, err := f.s.beginMigration(context.Background(), route.Role, req)
	if err != nil {
		t.Fatal(err)
	}
	if err = f.s.callScene(context.Background(), "scene-1", "freeze", transferRef{m.Source.Ref(), m.TransferID}, nil); err != nil {
		t.Fatal(err)
	}
	if err = f.s.callScene(context.Background(), "scene-2", "prepare", map[string]any{"role": m.Target, "transfer_id": m.TransferID}, nil); err != nil {
		t.Fatal(err)
	}
	f.restart(t)
	if err = f.s.Recover(context.Background()); err != nil {
		t.Fatal(err)
	}
	m, err = f.s.migration(context.Background(), route.Role.RoleID, req.RequestID)
	if err != nil {
		t.Fatal(err)
	}
	if m.Status != "ABORTED" {
		t.Fatalf("state=%s", m.Status)
	}
	if _, exists := f.target.player(route.Role.RoleID); exists {
		t.Fatal("prepared candidate survived rollback")
	}
}

func TestExclusiveStoreLock(t *testing.T) {
	f := newFixture(t)
	other, err := New(f.cfg)
	if err == nil {
		other.Close()
		t.Fatal("second backend opened same database")
	}
	f.restart(t)
	if err = f.s.db.Ping(); err != nil {
		t.Fatal(err)
	}
}

func TestRequestBoundaries(t *testing.T) {
	f := newFixture(t)
	unauthorized := httptest.NewRequest(http.MethodPost, "/v1/register", strings.NewReader(`{"name":"Alice"}`))
	w := httptest.NewRecorder()
	f.s.Handler().ServeHTTP(w, unauthorized)
	if w.Code != 401 {
		t.Fatalf("auth status=%d", w.Code)
	}
	for _, body := range []string{"null", `{"name":"Alice"} {}`, `{"name":"` + strings.Repeat("x", 65536) + `"}`} {
		req := httptest.NewRequest(http.MethodPost, "/v1/register", strings.NewReader(body))
		req.Header.Set("X-Internal-Token", testToken)
		w = httptest.NewRecorder()
		f.s.Handler().ServeHTTP(w, req)
		if w.Code != 400 {
			t.Fatalf("boundary status=%d body=%s", w.Code, w.Body)
		}
	}
	for _, raw := range []string{"", "scene-1=http://user:pass@localhost:9001", "scene-1=file:///tmp/a", "scene-1=http://localhost:9001/path", "scene-1=http://localhost:9001,scene-1=http://localhost:9002"} {
		if _, _, err := ParseScenes(raw); err == nil {
			t.Fatalf("accepted invalid scene config %q", raw)
		}
	}
}
