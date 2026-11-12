package gateway

import (
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestSlowDatabaseDoesNotBlockCastReplyOrPingAndFinalSaveDrains(t *testing.T) {
	role := Role{ID: "async-role", Scene: "scene-1", Owner: 1, Session: 1, Coins: 9, Combat: json.RawMessage(`{"hp":170,"revision":2}`)}
	finalRole := role
	finalRole.Combat = json.RawMessage(`{"hp":169,"revision":3}`)
	scene := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/v1/disconnect" {
			json.NewEncoder(w).Encode(map[string]any{"role": finalRole})
			return
		}
		if r.URL.Path == "/v1/view" {
			json.NewEncoder(w).Encode(View{Self: role, Scene: role.Scene, Players: []Role{role}})
			return
		}
		json.NewEncoder(w).Encode(map[string]any{"accepted": true, "role": role})
	}))
	defer scene.Close()
	started, release := make(chan struct{}), make(chan struct{})
	var once, releaseOnce sync.Once
	defer releaseOnce.Do(func() { close(release) })
	saved := make(chan Role, 16)
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/v1/checkpoint" {
			once.Do(func() { close(started) })
			select {
			case <-release:
			case <-r.Context().Done():
				return
			}
			var body struct {
				Role Role `json:"role"`
			}
			json.NewDecoder(r.Body).Decode(&body)
			saved <- body.Role
			json.NewEncoder(w).Encode(map[string]bool{"ok": true})
			return
		}
		json.NewEncoder(w).Encode(Route{Role: role, URL: scene.URL})
	}))
	defer backend.Close()
	server, err := New(Config{Token: strings.Repeat("w", 32), BackendURL: backend.URL})
	if err != nil {
		t.Fatal(err)
	}
	left, right := net.Pipe()
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { defer close(done); server.serveConn(ctx, left) }()
	defer func() { releaseOnce.Do(func() { close(release) }); cancel(); right.Close(); <-done }()
	reader := json.NewDecoder(right)
	exchange := func(request Request) {
		t.Helper()
		right.SetDeadline(time.Now().Add(time.Second))
		if err := json.NewEncoder(right).Encode(request); err != nil {
			t.Fatal(err)
		}
		for {
			var reply Response
			if err := reader.Decode(&reply); err != nil {
				t.Fatal(err)
			}
			if reply.ID == request.ID {
				if !reply.OK {
					t.Fatal(reply.Error)
				}
				return
			}
		}
	}
	exchange(Request{ID: "login", Op: "login", Token: "controlled"})
	exchange(Request{ID: "cast", Op: "cast", Seq: 1, SkillID: "strike", TargetID: "npc-1"})
	select {
	case <-started:
	case <-time.After(time.Second):
		t.Fatal("checkpoint was not scheduled")
	}
	// 数据库仍未释放，后续请求必须能完成。
	exchange(Request{ID: "ping", Op: "ping"})
	for i := int64(2); i <= 6; i++ {
		exchange(Request{ID: strings.Repeat("a", int(i)), Op: "cast", Seq: i, SkillID: "strike", TargetID: "npc-1"})
	}
	releaseOnce.Do(func() { close(release) })
	right.Close()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("final checkpoint did not drain")
	}
	if len(saved) == 0 {
		t.Fatal("no checkpoint persisted")
	}
	var last Role
	for len(saved) > 0 {
		last = <-saved
		if got := last; got.ID != role.ID || got.Coins != 9 || (string(got.Combat) != string(role.Combat) && string(got.Combat) != string(finalRole.Combat)) {
			t.Fatalf("snapshot was corrupted: %+v", got)
		}
	}
	if string(last.Combat) != string(finalRole.Combat) {
		t.Fatal("final authoritative role was not the last persisted checkpoint")
	}
}

func TestEventCursorDoesNotLoseUint64Precision(t *testing.T) {
	raw := json.RawMessage(`[{"id":9007199254740992,"type":"old"},{"id":9007199254740993,"type":"new"}]`)
	filtered := eventsAfter(raw, 9007199254740992)
	if string(filtered) != `[{"id":9007199254740993,"type":"new"}]` {
		t.Fatalf("wrong event filter: %s", filtered)
	}
}
