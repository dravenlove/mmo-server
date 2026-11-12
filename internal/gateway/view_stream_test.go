package gateway

import (
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestSlowAutomaticViewDoesNotBlockActionOrPublishAnOlderCombatRevision(t *testing.T) {
	role := Role{ID: "snapshot-role", Scene: "world", Owner: 1, Session: 1, Combat: json.RawMessage(`{"hp":180,"revision":1}`)}
	latest := role
	latest.Combat = json.RawMessage(`{"hp":170,"revision":2}`)
	started, release := make(chan struct{}), make(chan struct{})
	var releaseOnce sync.Once
	var count atomic.Int64
	scene := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/v1/view" {
			current := latest
			tick := count.Add(1)
			if tick == 1 {
				current = role
				close(started)
				select {
				case <-release:
				case <-r.Context().Done():
					return
				}
			}
			json.NewEncoder(w).Encode(View{Self: current, Scene: role.Scene, Players: []Role{current}, Tick: uint64(tick)})
			return
		}
		json.NewEncoder(w).Encode(map[string]any{"accepted": true, "role": latest})
	}))
	defer scene.Close()
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		json.NewEncoder(w).Encode(Route{Role: role, URL: scene.URL})
	}))
	defer backend.Close()
	server, err := New(Config{Token: strings.Repeat("v", 32), BackendURL: backend.URL})
	if err != nil {
		t.Fatal(err)
	}
	left, right := net.Pipe()
	ctx, cancel := context.WithCancel(context.Background())
	ticks := make(chan time.Time)
	done := make(chan struct{})
	go func() { defer close(done); server.serveConnWithTicks(ctx, left, ticks) }()
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
	ticks <- time.Now()
	select {
	case <-started:
	case <-time.After(time.Second):
		t.Fatal("automatic view did not start")
	}
	exchange(Request{ID: "action", Op: "cast", Seq: 1, SkillID: "strike", TargetID: "npc-1"})
	exchange(Request{ID: "ping", Op: "ping"})
	releaseOnce.Do(func() { close(release) })
	ticks <- time.Now()
	var response struct {
		Op   string `json:"op"`
		Data View   `json:"data"`
	}
	if err := reader.Decode(&response); err != nil {
		t.Fatal(err)
	}
	if response.Op != "state" || string(response.Data.Self.Combat) != string(latest.Combat) {
		t.Fatalf("older pending snapshot replaced successful action: %+v", response)
	}
}

func TestFreshViewAllowsWorldResetButRejectsOlderTickInSameWorld(t *testing.T) {
	role := Role{ID: "a", Scene: "scene", Owner: 1, Session: 1, Combat: json.RawMessage(`{"revision":99}`)}
	previous := View{Self: role, WorldEpoch: "old", Tick: 100}
	next := previous
	next.WorldEpoch = "new"
	next.Tick = 1
	next.Self.Combat = json.RawMessage(`{"revision":1}`)
	if !freshView(&previous, next) {
		t.Fatal("new world cannot reset simulation tick")
	}
	next.WorldEpoch = "old"
	if freshView(&previous, next) {
		t.Fatal("old tick was accepted inside same world")
	}
	previous.WorldEpoch = ""
	next.WorldEpoch = "first-world"
	next.Tick = 101
	if freshView(&previous, next) {
		t.Fatal("initial delayed view regressed a successful action")
	}
}
