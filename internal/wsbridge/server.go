// Package wsbridge exposes the existing TCP JSONL gateway to WebSocket clients.
// It never computes positions, AOI, identity or game rules.
package wsbridge

import (
	"bufio"
	"bytes"
	"compress/flate"
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unicode/utf8"

	"github.com/gorilla/websocket"
)

const MaxFrame = 64 * 1024

type Config struct {
	Upstream        string
	UpstreamTLS     bool
	AllowedOrigins  []string
	MaxConnections  int
	Logger          *log.Logger
	ProtocolVersion string
}

type Server struct {
	cfg     Config
	mu      sync.Mutex
	active  int
	closed  bool
	clients map[*websocket.Conn]net.Conn
	origins map[string]bool
	nextID  atomic.Uint64
}

func New(cfg Config) (*Server, error) {
	if _, _, err := net.SplitHostPort(cfg.Upstream); err != nil {
		return nil, errors.New("upstream must be host:port")
	}
	if cfg.MaxConnections <= 0 {
		cfg.MaxConnections = 128
	}
	if cfg.Logger == nil {
		cfg.Logger = log.Default()
	}
	if cfg.ProtocolVersion == "" {
		cfg.ProtocolVersion = "webgl-movement-v1"
	}
	if cfg.ProtocolVersion != "webgl-movement-v1" && cfg.ProtocolVersion != "webgl-combat-v1" && cfg.ProtocolVersion != "webgl-elemental-v1" && cfg.ProtocolVersion != "webgl-skill-lab-v1" {
		return nil, errors.New("unsupported WebSocket protocol version")
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
		json.NewEncoder(w).Encode(map[string]any{"ok": true, "service": "websocket-bridge", "protocol_version": s.cfg.ProtocolVersion})
		return
	}
	if r.URL.Path != "/ws" || r.Method != http.MethodGet {
		http.NotFound(w, r)
		return
	}
	// Requiring an allowed Origin also makes browser/account boundary mistakes visible.
	if !s.origins[r.Header.Get("Origin")] {
		s.reject(r, http.StatusForbidden, "origin_not_allowed")
		http.Error(w, "origin is not allowed", http.StatusForbidden)
		return
	}
	if r.URL.RawQuery != "" || r.URL.ForceQuery {
		s.reject(r, http.StatusBadRequest, "query_not_supported")
		http.Error(w, "query parameters are not supported", http.StatusBadRequest)
		return
	}
	s.mu.Lock()
	if s.closed || s.active >= s.cfg.MaxConnections {
		s.mu.Unlock()
		s.reject(r, http.StatusServiceUnavailable, "bridge_unavailable")
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
		s.reject(r, http.StatusBadGateway, "gateway_unavailable")
		http.Error(w, "gateway unavailable", http.StatusBadGateway)
		return
	}
	defer upstream.Close()
	upgrade := websocket.Upgrader{
		HandshakeTimeout:  5 * time.Second,
		EnableCompression: true,
		CheckOrigin:       func(req *http.Request) bool { return s.origins[req.Header.Get("Origin")] },
		Error: func(w http.ResponseWriter, req *http.Request, status int, _ error) {
			s.reject(req, status, "invalid_upgrade")
			http.Error(w, http.StatusText(status), status)
		},
	}
	ws, err := upgrade.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	defer ws.Close()
	if err := ws.SetCompressionLevel(flate.BestSpeed); err != nil {
		return
	}
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return
	}
	s.clients[ws] = upstream
	s.mu.Unlock()
	defer func() { s.mu.Lock(); delete(s.clients, ws); s.mu.Unlock() }()
	id, started := s.nextID.Add(1), time.Now()
	s.cfg.Logger.Printf("wsbridge event=connected connection=%d origin=%q", id, diagnosticOrigin(r.Header.Get("Origin")))
	// 只记录阶段、固定原因和关闭码；不记录消息、凭据、查询串或客户端关闭说明。
	var finished sync.Once
	finish := func(reason string, code, peerCode int) {
		finished.Do(func() {
			s.cfg.Logger.Printf("wsbridge event=closed connection=%d reason=%s close_code=%d peer_code=%d duration_ms=%d", id, reason, code, peerCode, time.Since(started).Milliseconds())
			if code != 0 {
				ws.WriteControl(websocket.CloseMessage, websocket.FormatCloseMessage(code, reason), time.Now().Add(time.Second))
			}
			ws.Close()
			upstream.Close()
		})
	}
	defer finish("handler_returned", websocket.CloseInternalServerErr, 0)
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
					finish("ping_failed", 0, 0)
					return
				}
			case <-done:
				return
			}
		}
	}()
	// One TCP reader and one WebSocket data writer. Ping uses the concurrency-safe control API.
	go func() {
		scanner := bufio.NewScanner(upstream)
		scanner.Buffer(make([]byte, 4096), MaxFrame)
		for scanner.Scan() {
			line := scanner.Bytes()
			if len(line) > MaxFrame-1 || !utf8.Valid(line) || len(line) == 0 || line[0] != '{' || !json.Valid(line) {
				finish("gateway_invalid_json", websocket.CloseInternalServerErr, 0)
				return
			}
			ws.SetWriteDeadline(time.Now().Add(5 * time.Second))
			if ws.WriteMessage(websocket.TextMessage, line) != nil {
				finish("client_write_failed", 0, 0)
				return
			}
		}
		if scanner.Err() != nil {
			finish("gateway_read_failed", websocket.CloseInternalServerErr, 0)
		} else {
			finish("gateway_closed", websocket.CloseInternalServerErr, 0)
		}
	}()
	for {
		kind, reader, readErr := ws.NextReader()
		var message []byte
		if readErr == nil {
			message, readErr = io.ReadAll(io.LimitReader(reader, MaxFrame+1))
			if len(message) > MaxFrame-1 {
				finish("message_too_large", websocket.CloseMessageTooBig, 0)
				return
			}
		}
		if readErr != nil {
			reason, code, peerCode := "client_transport_closed", 0, 0
			var closeErr *websocket.CloseError
			var netErr net.Error
			if errors.As(readErr, &closeErr) {
				peerCode = closeErr.Code
				if peerCode != websocket.CloseAbnormalClosure {
					reason = "peer_closed"
				}
			} else if errors.Is(readErr, websocket.ErrReadLimit) {
				reason, code = "message_too_large", websocket.CloseMessageTooBig
			} else if errors.As(readErr, &netErr) && netErr.Timeout() {
				reason, code = "client_read_timeout", websocket.CloseGoingAway
			}
			finish(reason, code, peerCode)
			return
		}
		if kind != websocket.TextMessage {
			finish("text_json_required", websocket.CloseUnsupportedData, 0)
			return
		}
		message = bytes.TrimSpace(message)
		if len(message) == 0 || !utf8.Valid(message) || message[0] != '{' || !json.Valid(message) {
			finish("invalid_text_json", websocket.CloseInvalidFramePayloadData, 0)
			return
		}
		var compact bytes.Buffer
		if json.Compact(&compact, message) != nil || compact.Len()+1 > MaxFrame {
			finish("message_too_large", websocket.CloseMessageTooBig, 0)
			return
		}
		compact.WriteByte('\n')
		upstream.SetWriteDeadline(time.Now().Add(5 * time.Second))
		if _, err = io.Copy(upstream, &compact); err != nil {
			finish("gateway_write_failed", websocket.CloseInternalServerErr, 0)
			return
		}
	}
}

func (s *Server) reject(r *http.Request, status int, reason string) {
	s.cfg.Logger.Printf("wsbridge event=handshake_rejected reason=%s status=%d origin=%q", reason, status, diagnosticOrigin(r.Header.Get("Origin")))
}

// 非法 Origin 可能夹带凭据或查询参数，日志只保留合法的来源三元组。
func diagnosticOrigin(origin string) string {
	if origin == "" {
		return "(missing)"
	}
	u, err := url.Parse(origin)
	if len(origin) > 256 || err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" || u.User != nil || u.Path != "" || u.RawQuery != "" || u.ForceQuery || u.Fragment != "" {
		return "(invalid)"
	}
	return u.Scheme + "://" + u.Host
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
