package gateway

import (
	"bufio"
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

func TestSlowViewDoesNotBlockFourMovementInputsAndDisconnectSavesFinalPosition(t *testing.T) {
	role := Role{ID: "movement-test", Name: "tester", Scene: "scene-1", Owner: 1, Session: 1}
	viewStarted, release := make(chan struct{}), make(chan struct{})
	var begin, releaseOnce sync.Once
	defer releaseOnce.Do(func() { close(release) })
	scene := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/v1/view":
			begin.Do(func() { close(viewStarted) })
			select {
			case <-release:
			case <-r.Context().Done():
				return
			}
			json.NewEncoder(w).Encode(View{Self: role, Players: []Role{role}, Scene: role.Scene, Tick: 1})
		case "/v1/move":
			json.NewEncoder(w).Encode(map[string]any{"accepted": true})
		case "/v1/move_batch":
			var body struct {
				Inputs []json.RawMessage `json:"inputs"`
			}
			json.NewDecoder(r.Body).Decode(&body)
			results := make([]Response, len(body.Inputs))
			for i := range results {
				results[i] = Response{OK: true, Data: map[string]any{"accepted": true}}
			}
			json.NewEncoder(w).Encode(map[string]any{"results": results})
		case "/v1/disconnect":
			latest := role
			latest.X = 7.5
			json.NewEncoder(w).Encode(map[string]any{"role": latest})
		default:
			http.Error(w, `{"error":"unexpected"}`, 400)
		}
	}))
	defer scene.Close()
	checkpoints := make(chan Role, 8)
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/v1/checkpoint" {
			var body struct {
				Role Role `json:"role"`
			}
			json.NewDecoder(r.Body).Decode(&body)
			checkpoints <- body.Role
			json.NewEncoder(w).Encode(map[string]bool{"ok": true})
			return
		}
		json.NewEncoder(w).Encode(Route{Role: role, URL: scene.URL})
	}))
	defer backend.Close()
	server, err := New(Config{Token: strings.Repeat("m", 32), BackendURL: backend.URL})
	if err != nil {
		t.Fatal(err)
	}
	left, right := net.Pipe()
	defer right.Close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan struct{})
	go func() { defer close(done); server.serveConn(ctx, left) }()
	right.SetDeadline(time.Now().Add(3 * time.Second))
	reader := bufio.NewReader(right)
	send := func(value Request) {
		t.Helper()
		if err := json.NewEncoder(right).Encode(value); err != nil {
			t.Fatal(err)
		}
	}
	read := func() Response {
		t.Helper()
		var value Response
		if err := json.NewDecoder(reader).Decode(&value); err != nil {
			t.Fatal(err)
		}
		return value
	}
	send(Request{ID: "login", Op: "login", Token: "private-not-logged"})
	if !read().OK {
		t.Fatal("login failed")
	}
	send(Request{ID: "slow-view", Op: "view"})
	select {
	case <-viewStarted:
	case <-time.After(time.Second):
		t.Fatal("view did not start")
	}
	sent := make(chan error, 1)
	go func() {
		for i := int64(1); i <= 4; i++ {
			err := json.NewEncoder(right).Encode(Request{ID: strings.Repeat("m", int(i)), Op: "move", Seq: i, DX: 1})
			if err != nil {
				sent <- err
				return
			}
		}
		sent <- nil
	}()
	for i := 0; i < 4; i++ {
		response := read()
		if response.Op != "move" || !response.OK {
			t.Fatalf("movement waited behind blocked view: %+v", response)
		}
	}
	if err := <-sent; err != nil {
		t.Fatal(err)
	}
	select {
	case <-release:
		t.Fatal("test accidentally released the slow view")
	default:
	}
	releaseOnce.Do(func() { close(release) })
	right.Close()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("connection did not stop")
	}
	select {
	case saved := <-checkpoints:
		if saved.X != 7.5 {
			t.Fatalf("saved stale view instead of final disconnect position: %v", saved.X)
		}
	case <-time.After(time.Second):
		t.Fatal("final checkpoint missing")
	}
}
