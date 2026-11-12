package gateway

import (
	"context"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestElementalQueuedMovementCannotCrossOwner(t *testing.T) {
	s, err := New(Config{Token: strings.Repeat("e", 32), BackendURL: "http://127.0.0.1:1", Profile: "elemental", LoginScene: "world", ArenaScene: "arena"})
	if err != nil {
		t.Fatal(err)
	}
	old := &connectionRoute{Route: Route{Role: Role{ID: "actor", Scene: "world", Owner: 1, Session: 1}}, ValidUntil: time.Now().Add(time.Second)}
	next := &connectionRoute{Route: Route{Role: Role{ID: "actor", Scene: "arena", Owner: 2, Session: 1}}, ValidUntil: time.Now().Add(time.Second)}
	var published atomic.Pointer[connectionRoute]
	published.Store(next)
	queue := make(chan Request, 4)
	queue <- Request{ID: "old-move", Op: "move", Seq: 10, DX: 1, receivedRoute: old}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	responses := make(chan Response, 1)
	var barrier sync.Mutex
	go s.moveWorker(ctx, queue, &published, &barrier, func(r Response) bool { responses <- r; return false })
	select {
	case r := <-responses:
		if !r.OK || r.ID != "old-move" || r.Data.(map[string]any)["reason"] != "scene_changed" {
			t.Fatalf("unexpected old-owner result: %+v", r)
		}
	case <-time.After(time.Second):
		t.Fatal("old-owner movement was sent to a scene")
	}
}
