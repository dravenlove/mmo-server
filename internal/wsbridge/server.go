// Package wsbridge exposes the existing TCP JSONL gateway to WebSocket clients.
// It never computes positions, AOI, identity or game rules.
package wsbridge

import (
	"bufio"
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/gorilla/websocket"
)

const MaxFrame = 64 * 1024

type Config struct {
	Upstream       string
	UpstreamTLS    bool
	AllowedOrigins []string
	MaxConnections int
}

type Server struct {
	cfg     Config
	mu      sync.Mutex
	active  int
	closed  bool
	clients map[*websocket.Conn]net.Conn
	origins map[string]bool
}

func New(cfg Config) (*Server, error) {
	if _, _, err := net.SplitHostPort(cfg.Upstream); err != nil {
		return nil, errors.New("upstream must be host:port")
	}
	if cfg.MaxConnections <= 0 {
		cfg.MaxConnections = 128
	}
	s := &Server{cfg: cfg, clients: make(map[*websocket.Conn]net.Conn), origins: make(map[string]bool)}
	for _, origin := range cfg.AllowedOrigins {
		u, err := url.Parse(strings.TrimSpace(origin))
		if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" || u.User != nil || u.Path != "" || u.RawQuery != "" || u.Fragment != "" {
			return nil, errors.New("allowed origins must be exact http(s) origins without a path")
		}
		s.origins[u.Scheme+"://"+u.Host] = true
	}
	if len(s.origins) == 0 {
		return nil, errors.New("at least one allowed browser origin is required")
	}
	return s, nil
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path == "/healthz" && r.Method == http.MethodGet {
		w.Header().Set("Content-Type", "application/json")
		io.WriteString(w, `{"ok":true,"service":"websocket-bridge","protocol_version":"webgl-movement-v1"}`+"\n")
		return
	}
	if r.URL.Path != "/ws" || r.Method != http.MethodGet {
		http.NotFound(w, r)
		return
	}
	// Requiring an allowed Origin also makes browser/account boundary mistakes visible.
	if !s.origins[r.Header.Get("Origin")] {
		http.Error(w, "origin is not allowed", http.StatusForbidden)
		return
	}
	if r.URL.RawQuery != "" || r.URL.ForceQuery {
		http.Error(w, "query parameters are not supported", http.StatusBadRequest)
		return
	}
	s.mu.Lock()
	if s.closed || s.active >= s.cfg.MaxConnections {
		s.mu.Unlock()
		http.Error(w, "bridge unavailable", http.StatusServiceUnavailable)
		return
	}
	s.active++
	s.mu.Unlock()
	defer func() { s.mu.Lock(); s.active--; s.mu.Unlock() }()
	ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
	defer cancel()
	dialer := &net.Dialer{Timeout: 5 * time.Second, KeepAlive: 30 * time.Second}
	var upstream net.Conn
	var err error
	if s.cfg.UpstreamTLS {
		host, _, _ := net.SplitHostPort(s.cfg.Upstream)
		td := &tls.Dialer{NetDialer: dialer, Config: &tls.Config{ServerName: host, MinVersion: tls.VersionTLS12}}
		upstream, err = td.DialContext(ctx, "tcp", s.cfg.Upstream)
	} else {
		upstream, err = dialer.DialContext(ctx, "tcp", s.cfg.Upstream)
	}
	if err != nil {
		http.Error(w, "gateway unavailable", http.StatusBadGateway)
		return
	}
	defer upstream.Close()
	upgrade := websocket.Upgrader{HandshakeTimeout: 5 * time.Second, CheckOrigin: func(req *http.Request) bool { return s.origins[req.Header.Get("Origin")] }}
	ws, err := upgrade.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	defer ws.Close()
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return
	}
	s.clients[ws] = upstream
	s.mu.Unlock()
	defer func() { s.mu.Lock(); delete(s.clients, ws); s.mu.Unlock() }()
	ws.SetReadLimit(MaxFrame - 1)
	ws.SetReadDeadline(time.Now().Add(60 * time.Second))
	ws.SetPongHandler(func(string) error { return ws.SetReadDeadline(time.Now().Add(60 * time.Second)) })
	done := make(chan struct{})
	defer close(done)
	go func() {
		ticker := time.NewTicker(20 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				if ws.WriteControl(websocket.PingMessage, nil, time.Now().Add(3*time.Second)) != nil {
					ws.Close()
					upstream.Close()
					return
				}
			case <-done:
				return
			}
		}
	}()
	// One TCP reader and one WebSocket data writer. Ping uses the concurrency-safe control API.
	go func() {
		defer ws.Close()
		defer upstream.Close()
		scanner := bufio.NewScanner(upstream)
		scanner.Buffer(make([]byte, 4096), MaxFrame)
		for scanner.Scan() {
			line := scanner.Bytes()
			if len(line) > MaxFrame-1 || !utf8.Valid(line) || len(line) == 0 || line[0] != '{' || !json.Valid(line) {
				return
			}
			ws.SetWriteDeadline(time.Now().Add(5 * time.Second))
			if ws.WriteMessage(websocket.TextMessage, line) != nil {
				return
			}
		}
	}()
	for {
		kind, message, readErr := ws.ReadMessage()
		if readErr != nil {
			return
		}
		if kind != websocket.TextMessage {
			ws.WriteControl(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseUnsupportedData, "text JSON required"), time.Now().Add(time.Second))
			return
		}
		message = bytes.TrimSpace(message)
		if len(message) == 0 || !utf8.Valid(message) || message[0] != '{' || !json.Valid(message) {
			return
		}
		var compact bytes.Buffer
		if json.Compact(&compact, message) != nil || compact.Len()+1 > MaxFrame {
			return
		}
		compact.WriteByte('\n')
		upstream.SetWriteDeadline(time.Now().Add(5 * time.Second))
		if _, err = io.Copy(upstream, &compact); err != nil {
			return
		}
	}
}

// HTTP Shutdown alone does not close hijacked WebSocket connections.
func (s *Server) Close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.closed = true
	for ws, tcp := range s.clients {
		ws.Close()
		tcp.Close()
	}
}
