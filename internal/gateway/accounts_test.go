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

func TestAccountOperationsAcrossProfiles(t *testing.T) {
	for _, profile := range []string{"", "movement", "abilities", "elemental", "skill_lab"} {
		t.Run(profile, func(t *testing.T) {
			upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				var body map[string]string
				if json.NewDecoder(r.Body).Decode(&body) != nil || body["password"] != "password-123" {
					t.Error("password not forwarded")
				}
				switch r.URL.Path {
				case "/v1/account_register", "/v1/account_login":
					if body["name"] != "player" || body["token"] != "" {
						t.Error("invalid account payload")
					}
					json.NewEncoder(w).Encode(map[string]any{"token": strings.Repeat("a", 64), "role_id": strings.Repeat("b", 32), "expires_at_unix_ms": time.Now().Add(time.Hour).UnixMilli()})
				case "/v1/account_bind":
					if body["token"] != "existing-token" || body["name"] != "" {
						t.Error("invalid binding payload")
					}
					w.WriteHeader(409)
					json.NewEncoder(w).Encode(map[string]string{"error": "already bound"})
				default:
					t.Error("unexpected route")
					w.WriteHeader(404)
				}
			}))
			defer upstream.Close()
			cfg := Config{Token: strings.Repeat("k", 32), BackendURL: upstream.URL, Profile: profile}
			if profile == "elemental" {
				cfg.LoginScene = "lobby"
				cfg.ArenaScene = "arena"
			}
			s, err := New(cfg)
			if err != nil {
				t.Fatal(err)
			}
			a, b := net.Pipe()
			defer b.Close()
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			done := make(chan struct{})
			go func() { defer close(done); s.serveConn(ctx, a) }()
			scanner := bufio.NewScanner(b)
			for _, op := range []string{"account_register", "account_login", "account_bind"} {
				b.SetDeadline(time.Now().Add(3 * time.Second))
				req := Request{ID: op, Op: op, Name: "player", Password: "password-123"}
				if op == "account_bind" {
					req.Name = ""
					req.Token = "existing-token"
				}
				if err := json.NewEncoder(b).Encode(req); err != nil {
					t.Fatal(err)
				}
				if !scanner.Scan() {
					t.Fatal("no account response")
				}
				var reply Response
				if json.Unmarshal(scanner.Bytes(), &reply) != nil {
					t.Fatal("invalid response")
				}
				if op == "account_bind" {
					if reply.OK || reply.Status != 409 {
						t.Fatal("conflict status lost")
					}
				} else if !reply.OK {
					t.Fatal("account operation rejected")
				}
			}
			b.Close()
			cancel()
			select {
			case <-done:
			case <-time.After(3 * time.Second):
				t.Fatal("connection did not close")
			}
		})
	}
}
