package gateway

import (
	"bufio"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestGatewayRoutesAndRejectsUnauthenticatedInput(t *testing.T) {
	key := strings.Repeat("a", 32)
	role := Role{ID: "role-a", Name: "alice", Scene: "scene-1", Owner: 1, Session: 2, Coins: 100}
	var sceneCalls atomic.Int64
	scene := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Internal-Token") != key {
			t.Error("missing internal credential")
		}
		var p map[string]any
		json.NewDecoder(r.Body).Decode(&p)
		if p["role_id"] != "role-a" || p["session_epoch"] != float64(2) {
			t.Errorf("wrong routed identity: %v", p)
		}
		sceneCalls.Add(1)
		if r.URL.Path == "/v1/view" {
			json.NewEncoder(w).Encode(View{Self: role, Players: []Role{role}, Scene: "scene-1", Tick: 1})
		} else {
			json.NewEncoder(w).Encode(map[string]any{"accepted": true})
		}
	}))
	defer scene.Close()
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Internal-Token") != key {
			t.Error("missing internal credential")
		}
		switch r.URL.Path {
		case "/v1/login", "/v1/session":
			json.NewEncoder(w).Encode(Route{Role: role, URL: scene.URL})
		case "/v1/checkpoint":
			json.NewEncoder(w).Encode(map[string]any{"ok": true})
		default:
			http.Error(w, `{"error":"unexpected"}`, 400)
		}
	}))
	defer backend.Close()
	s, err := New(Config{Token: key, BackendURL: backend.URL})
	if err != nil {
		t.Fatal(err)
	}
	serverConn, clientConn := net.Pipe()
	defer clientConn.Close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan struct{})
	go func() { defer close(done); s.serveConn(ctx, serverConn) }()
	scanner := bufio.NewScanner(clientConn)
	exchange := func(req Request) Response {
		t.Helper()
		clientConn.SetDeadline(time.Now().Add(3 * time.Second))
		if err := json.NewEncoder(clientConn).Encode(req); err != nil {
			t.Fatal(err)
		}
		for scanner.Scan() {
			var r Response
			if err := json.Unmarshal(scanner.Bytes(), &r); err != nil {
				t.Fatal(err)
			}
			if r.ID == req.ID {
				return r
			}
		}
		t.Fatalf("connection closed: %v", scanner.Err())
		return Response{}
	}
	if r := exchange(Request{ID: "1", Op: "move", Seq: 1, DX: 1}); r.OK || r.Error != "login required" {
		t.Fatalf("unauthenticated move: %+v", r)
	}
	if r := exchange(Request{ID: "2", Op: "login", Token: "test"}); !r.OK {
		t.Fatalf("login: %+v", r)
	}
	if r := exchange(Request{ID: "3", Op: "move", Seq: 1, DX: 1}); !r.OK {
		t.Fatalf("move: %+v", r)
	}
	if r := exchange(Request{ID: "4", Op: "transfer", Target: "scene-2"}); r.OK {
		t.Fatal("transfer without stable request id accepted")
	}
	clientConn.Close()
	cancel()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("connection did not stop")
	}
	if sceneCalls.Load() == 0 {
		t.Fatal("scene not reached")
	}
}

func TestRejectsInternalRedirect(t *testing.T) {
	leaked := false
	destination := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { leaked = true }))
	defer destination.Close()
	redirect := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, destination.URL, http.StatusTemporaryRedirect)
	}))
	defer redirect.Close()
	s, err := New(Config{Token: strings.Repeat("a", 32), BackendURL: redirect.URL})
	if err != nil {
		t.Fatal(err)
	}
	if err = s.call(context.Background(), redirect.URL, "/", map[string]any{}, nil); err == nil {
		t.Fatal("redirect accepted")
	}
	if leaked {
		t.Fatal("internal credential forwarded to redirect target")
	}
}

func TestBalanceOverlay(t *testing.T) {
	v := View{Self: Role{ID: "a"}, Players: []Role{{ID: "a"}, {ID: "b", Coins: 7}}}
	overlayBalance(&v, Role{ID: "a", Coins: 100})
	if v.Self.Coins != 100 || v.Players[0].Coins != 100 || v.Players[1].Coins != 7 {
		t.Fatal(v)
	}
}

func TestTLSListenerRequiresTrustedCertificate(t *testing.T) {
	// 使用测试证书验证实际 TCP 入口，未信任证书的客户端必须失败。
	fixture := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {}))
	defer fixture.Close()
	dir := t.TempDir()
	certFile, keyFile := filepath.Join(dir, "cert.pem"), filepath.Join(dir, "key.pem")
	key, err := x509.MarshalPKCS8PrivateKey(fixture.TLS.Certificates[0].PrivateKey)
	if err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(certFile, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: fixture.TLS.Certificates[0].Certificate[0]}), 0600); err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(keyFile, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: key}), 0600); err != nil {
		t.Fatal(err)
	}
	address := func() string {
		l, e := net.Listen("tcp", "127.0.0.1:0")
		if e != nil {
			t.Fatal(e)
		}
		a := l.Addr().String()
		l.Close()
		return a
	}
	listen, health := address(), address()
	s, err := New(Config{Listen: listen, HealthListen: health, BackendURL: fixture.URL, Token: strings.Repeat("x", 32), CertFile: certFile, KeyFile: keyFile})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- s.Run(ctx) }()
	defer func() {
		cancel()
		select {
		case e := <-done:
			if e != nil {
				t.Error(e)
			}
		case <-time.After(5 * time.Second):
			t.Error("TLS gateway failed to shut down")
		}
	}()
	ready := false
	for deadline := time.Now().Add(3 * time.Second); time.Now().Before(deadline); {
		client := http.Client{Timeout: 100 * time.Millisecond}
		r, e := client.Get("http://" + health + "/healthz")
		if e == nil {
			r.Body.Close()
			ready = true
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if !ready {
		t.Fatal("TLS gateway did not start")
	}
	dialer := &net.Dialer{Timeout: 2 * time.Second}
	if c, e := tls.DialWithDialer(dialer, "tcp", listen, &tls.Config{MinVersion: tls.VersionTLS12}); e == nil {
		c.Close()
		t.Fatal("untrusted certificate accepted")
	}
	pool := x509.NewCertPool()
	pool.AddCert(fixture.Certificate())
	c, err := tls.DialWithDialer(dialer, "tcp", listen, &tls.Config{RootCAs: pool, MinVersion: tls.VersionTLS12})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(2 * time.Second))
	if err = json.NewEncoder(c).Encode(Request{ID: "tls-1", Op: "ping"}); err != nil {
		t.Fatal(err)
	}
	var r Response
	if err = json.NewDecoder(c).Decode(&r); err != nil {
		t.Fatal(err)
	}
	if !r.OK || r.ID != "tls-1" {
		t.Fatalf("TLS ping failed: %+v", r)
	}
}
