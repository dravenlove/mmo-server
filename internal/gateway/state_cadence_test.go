package gateway

import (
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestSnapshotCadenceDoesNotSkipAnEarlyOrDelayedTick(t *testing.T) {
	role := Role{ID: "cadence-role", Scene: "scene-webgl", Owner: 1, Session: 1}
	var views atomic.Uint64
	scene := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/v1/disconnect" {
			json.NewEncoder(w).Encode(map[string]any{"role": role})
			return
		}
		json.NewEncoder(w).Encode(View{Self: role, Players: []Role{role}, Scene: role.Scene, Tick: views.Add(1), WorldEpoch: "world-a", EventCursor: 17, Events: json.RawMessage(`[{"id":17,"type":"damage"}]`)})
	}))
	defer scene.Close()
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		json.NewEncoder(w).Encode(Route{Role: role, URL: scene.URL})
	}))
	defer backend.Close()
	server, err := New(Config{Token: strings.Repeat("s", 32), BackendURL: backend.URL, Profile: "abilities", LoginScene: role.Scene})
	if err != nil {
		t.Fatal(err)
	}
	left, right := net.Pipe()
	ctx, cancel := context.WithCancel(context.Background())
	ticks, done := make(chan time.Time), make(chan struct{})
	go func() { defer close(done); server.serveConnWithTicks(ctx, left, ticks) }()
	defer func() {
		cancel()
		right.Close()
		select {
		case <-done:
		case <-time.After(time.Second):
			t.Error("connection did not stop")
		}
	}()
	right.SetDeadline(time.Now().Add(3 * time.Second))
	if err := json.NewEncoder(right).Encode(Request{ID: "login", Op: "login", Token: "controlled-test"}); err != nil {
		t.Fatal(err)
	}
	reader := json.NewDecoder(right)
	var response Response
	if err := reader.Decode(&response); err != nil || !response.OK {
		t.Fatalf("login: %v %+v", err, response)
	}
	start := time.Now()
	// 相邻周期差一纳秒也必须发送；积压的旧时间戳仍只触发一份最新状态。
	for i, offset := range []time.Duration{0, 200*time.Millisecond - 1, 400*time.Millisecond - 2, 600*time.Millisecond + 1, -time.Second} {
		ticks <- start.Add(offset)
		var state struct {
			Op   string `json:"op"`
			OK   bool   `json:"ok"`
			Data View   `json:"data"`
		}
		if err := reader.Decode(&state); err != nil {
			t.Fatalf("tick %d did not produce state: %v", i, err)
		}
		if !state.OK || state.Op != "state" || state.Data.Tick != uint64(i+1) {
			t.Fatalf("tick %d produced invalid state: %+v", i, state)
		}
		var events []json.RawMessage
		if err := json.Unmarshal(state.Data.Events, &events); err != nil {
			t.Fatal(err)
		}
		if (i == 0 && len(events) != 1) || (i > 0 && len(events) != 0) {
			t.Fatalf("tick %d replayed event history: %s", i, state.Data.Events)
		}
	}
}

func TestSnapshotRateDefaultsAndValidation(t *testing.T) {
	for _, test := range []struct {
		profile     string
		input, want int
	}{{"movement", 0, 20}, {"skill_lab", 0, 20}, {"abilities", 0, 5}, {"full", 0, 5}, {"movement", 10, 10}} {
		server, err := New(Config{Token: strings.Repeat("r", 32), BackendURL: "http://localhost:8081", Profile: test.profile, StateHz: test.input})
		if err != nil || server.cfg.StateHz != test.want {
			t.Fatalf("profile %s rate %d: %v", test.profile, test.input, err)
		}
	}
	for _, hz := range []int{-1, 21} {
		if _, err := New(Config{Token: strings.Repeat("r", 32), BackendURL: "http://localhost:8081", StateHz: hz}); err == nil {
			t.Fatalf("accepted invalid rate %d", hz)
		}
	}
	t.Setenv("INTERNAL_TOKEN", strings.Repeat("r", 32))
	t.Setenv("GATEWAY_PROFILE", "movement")
	for _, value := range []string{"0", "21", "fast", "5.5"} {
		t.Setenv("GATEWAY_STATE_HZ", value)
		if _, err := FromEnv(); err == nil {
			t.Fatalf("accepted invalid environment rate %q", value)
		}
	}
	t.Setenv("GATEWAY_STATE_HZ", "10")
	if server, err := FromEnv(); err != nil || server.cfg.StateHz != 10 {
		t.Fatalf("valid environment rate failed: %v", err)
	}
}
