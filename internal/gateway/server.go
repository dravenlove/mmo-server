package gateway

import (
	"bufio"
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

type Role struct {
	ID      string          `json:"role_id"`
	Name    string          `json:"name"`
	Scene   string          `json:"scene_id"`
	Owner   int64           `json:"owner_epoch"`
	Session int64           `json:"session_epoch"`
	X       float64         `json:"x"`
	Y       float64         `json:"y"`
	Coins   int64           `json:"coins"`
	Combat  json.RawMessage `json:"combat,omitempty"`
}
type Route struct {
	Role Role   `json:"role"`
	URL  string `json:"scene_url"`
}
type View struct {
	Self               Role            `json:"self"`
	Players            []Role          `json:"players"`
	Tick               uint64          `json:"tick"`
	Scene              string          `json:"scene_id"`
	Mobs               json.RawMessage `json:"mobs,omitempty"`
	Skills             json.RawMessage `json:"skills,omitempty"`
	Events             json.RawMessage `json:"events,omitempty"`
	PlayersTruncated   bool            `json:"players_truncated,omitempty"`
	VisiblePlayerCount int             `json:"visible_player_count,omitempty"`
	ConfigVersion      string          `json:"config_version,omitempty"`
	ConfigGeneration   uint64          `json:"config_generation,omitempty"`
	ConfigAppliedTick  uint64          `json:"config_applied_tick,omitempty"`
}
type Request struct {
	ID              string  `json:"id"`
	Op              string  `json:"op"`
	Name            string  `json:"name,omitempty"`
	Token           string  `json:"token,omitempty"`
	Seq             int64   `json:"seq,omitempty"`
	DX              float64 `json:"dx,omitempty"`
	DY              float64 `json:"dy,omitempty"`
	Target          string  `json:"target_scene_id,omitempty"`
	RequestID       string  `json:"request_id,omitempty"`
	SkillID         string  `json:"skill_id,omitempty"`
	TargetID        string  `json:"target_id,omitempty"`
	receivedAt      time.Time
	validationError string
}
type Response struct {
	ID    string `json:"id,omitempty"`
	Op    string `json:"op"`
	OK    bool   `json:"ok"`
	Data  any    `json:"data,omitempty"`
	Error string `json:"error,omitempty"`
}
type Config struct {
	Listen, HealthListen, BackendURL, Token, CertFile, KeyFile string
	Profile, LoginScene                                        string
	TraceMovement                                              bool
}
type Server struct {
	cfg         Config
	client      *http.Client
	active      atomic.Int64
	mu          sync.Mutex
	connections map[net.Conn]struct{}
	wg          sync.WaitGroup
}
type upstreamError struct {
	status  int
	message string
}

func (e *upstreamError) Error() string { return e.message }

func env(key, fallback string) string {
	if s := os.Getenv(key); s != "" {
		return s
	}
	return fallback
}
func FromEnv() (*Server, error) {
	return New(Config{Listen: env("GATEWAY_LISTEN", "127.0.0.1:7000"), HealthListen: env("GATEWAY_HEALTH_LISTEN", "127.0.0.1:8080"), BackendURL: env("BACKEND_URL", "http://127.0.0.1:8081"), Token: os.Getenv("INTERNAL_TOKEN"), CertFile: os.Getenv("TLS_CERT_FILE"), KeyFile: os.Getenv("TLS_KEY_FILE"), Profile: env("GATEWAY_PROFILE", "full"), LoginScene: os.Getenv("GATEWAY_LOGIN_SCENE"), TraceMovement: os.Getenv("TRACE_MOVEMENT") == "true"})
}
func New(c Config) (*Server, error) {
	if c.Profile == "" {
		c.Profile = "full"
	}
	if c.Profile != "full" && c.Profile != "movement" {
		return nil, errors.New("GATEWAY_PROFILE 必须为 full 或 movement")
	}
	if len(c.LoginScene) > 64 {
		return nil, errors.New("GATEWAY_LOGIN_SCENE 过长")
	}
	if len(c.Token) < 32 {
		return nil, errors.New("INTERNAL_TOKEN 至少需要 32 字符")
	}
	u, err := url.Parse(c.BackendURL)
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" || u.User != nil || u.RawQuery != "" || u.Fragment != "" {
		return nil, errors.New("BACKEND_URL 必须是有效的 HTTP(S) 服务地址")
	}
	if (c.CertFile == "") != (c.KeyFile == "") {
		return nil, errors.New("TLS_CERT_FILE 和 TLS_KEY_FILE 必须同时配置")
	}
	return &Server{cfg: c, connections: make(map[net.Conn]struct{}), client: &http.Client{Timeout: 20 * time.Second, Transport: &http.Transport{MaxIdleConns: 128, MaxIdleConnsPerHost: 32, IdleConnTimeout: 60 * time.Second, ResponseHeaderTimeout: 19 * time.Second}, CheckRedirect: func(*http.Request, []*http.Request) error { return errors.New("禁止内部服务重定向") }}}, nil
}

func (s *Server) Run(ctx context.Context) error {
	l, err := net.Listen("tcp", s.cfg.Listen)
	if err != nil {
		return err
	}
	defer l.Close()
	if s.cfg.CertFile != "" {
		cert, e := tls.LoadX509KeyPair(s.cfg.CertFile, s.cfg.KeyFile)
		if e != nil {
			return e
		}
		l = tls.NewListener(l, &tls.Config{Certificates: []tls.Certificate{cert}, MinVersion: tls.VersionTLS12})
	}
	hl, err := net.Listen("tcp", s.cfg.HealthListen)
	if err != nil {
		return err
	}
	defer hl.Close()
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{"ok": true, "service": "gateway", "connections": s.active.Load(), "profile": s.cfg.Profile})
	})
	mux.HandleFunc("GET /readyz", func(w http.ResponseWriter, r *http.Request) {
		c, cancel := context.WithTimeout(r.Context(), 2*time.Second)
		defer cancel()
		req, _ := http.NewRequestWithContext(c, http.MethodGet, strings.TrimRight(s.cfg.BackendURL, "/")+"/healthz", nil)
		resp, e := s.client.Do(req)
		if e != nil {
			http.Error(w, "backend unavailable", 503)
			return
		}
		defer resp.Body.Close()
		if resp.StatusCode != 200 {
			http.Error(w, "backend unavailable", 503)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		io.WriteString(w, `{"ok":true}`)
	})
	hs := &http.Server{Handler: mux, ReadHeaderTimeout: 3 * time.Second, WriteTimeout: 3 * time.Second, IdleTimeout: 30 * time.Second}
	go hs.Serve(hl)
	defer hs.Close()
	shutdownDone := make(chan struct{})
	defer close(shutdownDone)
	go func() {
		select {
		case <-ctx.Done():
			l.Close()
			s.mu.Lock()
			for c := range s.connections {
				c.Close()
			}
			s.mu.Unlock()
		case <-shutdownDone:
		}
	}()
	log.Printf("gateway listening=%s health=%s tls=%t", l.Addr(), hl.Addr(), s.cfg.CertFile != "")
	defer func() {
		s.mu.Lock()
		for c := range s.connections {
			c.Close()
		}
		s.mu.Unlock()
		s.wg.Wait()
		s.client.CloseIdleConnections()
	}()
	for {
		c, e := l.Accept()
		if e != nil {
			if ctx.Err() != nil {
				return nil
			}
			return e
		}
		if s.active.Add(1) > 256 {
			s.active.Add(-1)
			c.Close()
			continue
		}
		s.mu.Lock()
		s.connections[c] = struct{}{}
		s.mu.Unlock()
		s.wg.Add(1)
		go func() {
			defer s.wg.Done()
			defer s.active.Add(-1)
			defer func() { s.mu.Lock(); delete(s.connections, c); s.mu.Unlock() }()
			s.serveConn(ctx, c)
		}()
	}
}

func (s *Server) call(ctx context.Context, base, path string, in, out any) error {
	started := time.Now()
	body, err := json.Marshal(in)
	if err != nil {
		return errors.New("invalid request")
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, strings.TrimRight(base, "/")+path, bytes.NewReader(body))
	if err != nil {
		return errors.New("invalid service address")
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Internal-Token", s.cfg.Token)
	resp, err := s.client.Do(req)
	if err != nil {
		s.logUpstreamFailure(base, req, "request", started, err)
		return &upstreamError{503, "service unavailable or timed out; retry with the same operation id"}
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		s.logUpstreamFailure(base, req, "read_body", started, err)
		return errors.New("service response interrupted")
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		var v struct {
			Error string `json:"error"`
		}
		json.Unmarshal(data, &v)
		if v.Error == "" {
			v.Error = fmt.Sprintf("service returned status %d", resp.StatusCode)
		}
		return &upstreamError{resp.StatusCode, v.Error}
	}
	if out != nil {
		if err = json.Unmarshal(data, out); err != nil {
			s.logUpstreamFailure(base, req, "decode", started, err)
			return errors.New("invalid service response")
		}
	}
	return nil
}

// 只记录服务地址、固定接口与传输错误，不记录登录/动作正文或内部凭据。
// 客户端错误保持稳定，运维日志则区分尚未收到响应和正文读取中断。
func (s *Server) logUpstreamFailure(base string, req *http.Request, phase string, started time.Time, err error) {
	var transportError *url.Error
	if errors.As(err, &transportError) {
		err = transportError.Err
	}
	service := "scene"
	if strings.TrimRight(s.cfg.BackendURL, "/") == strings.TrimRight(base, "/") {
		service = "backend"
	}
	log.Printf("upstream request failed service=%s endpoint=%s path=%s phase=%s elapsed_ms=%d error=%q",
		service, req.URL.Host, req.URL.Path, phase, time.Since(started).Milliseconds(), err)
}
func ref(r Role) map[string]any {
	return map[string]any{"role_id": r.ID, "owner_epoch": r.Owner, "session_epoch": r.Session}
}

func overlayBalance(v *View, role Role) {
	v.Self.Coins = role.Coins
	for i := range v.Players {
		if v.Players[i].ID == role.ID {
			v.Players[i].Coins = role.Coins
		}
	}
}

// 成功动作的回执比轮询视图更新；立即断开时也要保存该回执中的权威快照。
// Scene 不管理业务金币，因此先覆盖为 Backend 路由中的余额再交给客户端。
func actionSnapshot(data map[string]json.RawMessage, route Route, previous *View) (*View, bool) {
	var role Role
	if err := json.Unmarshal(data["role"], &role); err != nil || role.ID != route.Role.ID ||
		role.Owner != route.Role.Owner || role.Session != route.Role.Session || role.Scene != route.Role.Scene {
		return previous, false
	}
	role.Coins = route.Role.Coins
	data["role"], _ = json.Marshal(role)
	view := &View{Self: role, Scene: role.Scene}
	if previous != nil && previous.Self.ID == role.ID && previous.Self.Owner == role.Owner && previous.Self.Session == role.Session && previous.Self.Scene == role.Scene {
		var before, after struct {
			Revision int64 `json:"revision"`
		}
		if json.Unmarshal(previous.Self.Combat, &before) == nil && json.Unmarshal(role.Combat, &after) == nil && before.Revision > after.Revision {
			// 幂等重试可返回历史结算回执，不能覆盖此后已经观察到的战斗状态。
			return previous, true
		}
		copy := *previous
		view = &copy
		view.Self = role
		view.Players = append([]Role(nil), previous.Players...)
		for i := range view.Players {
			if view.Players[i].ID == role.ID {
				view.Players[i] = role
			}
		}
	}
	return view, true
}
func (s *Server) sceneCall(ctx context.Context, r Route, path string, in, out any) error {
	c, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	return s.call(c, r.URL, path, in, out)
}
func (s *Server) checkpoint(ctx context.Context, r Route, v *View) {
	if v == nil || v.Self.ID != r.Role.ID || v.Self.Owner != r.Role.Owner || v.Self.Session != r.Role.Session {
		return
	}
	c, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	if err := s.call(c, s.cfg.BackendURL, "/v1/checkpoint", map[string]any{"role": v.Self}, nil); err != nil {
		log.Printf("checkpoint role=%s failed: %v", r.Role.ID, err)
	}
}

func (s *Server) serveConn(ctx context.Context, c net.Conn) {
	defer c.Close()
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	done := make(chan struct{})
	defer close(done)
	requests := make(chan Request, 32)
	moves := make(chan Request, 4)
	var published atomic.Pointer[connectionRoute]
	var moveBarrier sync.Mutex
	var moveWG sync.WaitGroup
	readErrors := make(chan error, 1)
	go func() {
		scanner := bufio.NewScanner(c)
		scanner.Buffer(make([]byte, 4096), MaxMessageBytes+2)
		window := time.Now()
		count := 0
		for {
			c.SetReadDeadline(time.Now().Add(60 * time.Second))
			if !scanner.Scan() {
				cancel()
				readErrors <- scanner.Err()
				return
			}
			if time.Since(window) >= time.Second {
				window = time.Now()
				count = 0
			}
			count++
			if count > 40 {
				readErrors <- errors.New("message rate limit exceeded")
				return
			}
			if len(scanner.Bytes()) > MaxMessageBytes {
				readErrors <- errors.New("message exceeds 65535 bytes")
				cancel()
				return
			}
			var r Request
			dec := json.NewDecoder(bytes.NewReader(scanner.Bytes()))
			dec.DisallowUnknownFields()
			if err := dec.Decode(&r); err != nil {
				var header struct {
					ID string `json:"id"`
					Op string `json:"op"`
				}
				if json.Unmarshal(scanner.Bytes(), &header) == nil && header.ID != "" && len(header.ID) <= 64 && header.Op == "move" {
					select {
					case requests <- Request{ID: header.ID, Op: header.Op, validationError: "invalid movement fields"}:
					case <-ctx.Done():
						return
					}
					continue
				}
				readErrors <- errors.New("invalid JSON request")
				return
			}
			if dec.Decode(new(any)) != io.EOF || len(r.ID) == 0 || len(r.Op) == 0 || len(r.ID) > 64 || len(r.Op) > 24 {
				readErrors <- errors.New("invalid request")
				return
			}
			r.receivedAt = time.Now()
			if r.Op == "move" {
				var fields map[string]json.RawMessage
				if json.Unmarshal(scanner.Bytes(), &fields) != nil {
					readErrors <- errors.New("invalid move")
					cancel()
					return
				}
				valid := true
				for _, key := range []string{"seq", "dx", "dy"} {
					value := bytes.TrimSpace(fields[key])
					if (len(value) == 0 && s.cfg.Profile == "movement") || bytes.Equal(value, []byte("null")) {
						valid = false
					}
				}
				if !valid {
					r.validationError = "seq, dx and dy must be numbers"
					select {
					case requests <- r:
					case <-ctx.Done():
						return
					}
					continue
				}
				select {
				case moves <- r:
				case <-ctx.Done():
					return
				case <-done:
					return
				}
				continue
			}
			select {
			case requests <- r:
			case <-done:
				return
			default:
				readErrors <- errors.New("input queue full")
				return
			}
		}
	}()
	var writeMu sync.Mutex
	write := func(r Response) bool {
		writeMu.Lock()
		defer writeMu.Unlock()
		var data bytes.Buffer
		encoder := json.NewEncoder(&data)
		encoder.SetEscapeHTML(false)
		if encoder.Encode(r) != nil {
			return false
		}
		if data.Len()-1 > MaxMessageBytes {
			return false
		}
		c.SetWriteDeadline(time.Now().Add(3 * time.Second))
		_, err := io.Copy(c, &data)
		return err == nil
	}
	moveWG.Add(1)
	go func() {
		defer moveWG.Done()
		s.moveWorker(ctx, moves, &published, &moveBarrier, write)
		cancel()
		c.Close()
	}()
	publish := func(route *Route) {
		published.Store(&connectionRoute{Route: *route, ValidUntil: time.Now().Add(4 * time.Second)})
	}
	var route *Route
	var lastView *View
	defer func() {
		cancel()
		c.Close()
		moveWG.Wait()
		published.Store(nil)
		if route != nil {
			// 先按精确会话停止并隐藏角色，再保存该回执中的最终权威位置。
			var final struct {
				Role Role `json:"role"`
			}
			if s.sceneCall(context.Background(), *route, "/v1/disconnect", ref(route.Role), &final) == nil && sameRole(final.Role, route.Role) {
				final.Role.Coins = route.Role.Coins
				lastView = &View{Self: final.Role, Scene: final.Role.Scene}
			}
			s.checkpoint(context.Background(), *route, lastView)
		}
	}()
	ticker := time.NewTicker(200 * time.Millisecond)
	defer ticker.Stop()
	nextSession := time.Now()
	nextSave := time.Now().Add(2 * time.Second)
	nextView := time.Now()
	for {
		select {
		case <-ctx.Done():
			return
		case err := <-readErrors:
			if err != nil {
				write(Response{Op: "error", Error: err.Error()})
			}
			return
		case req := <-requests:
			resp := Response{ID: req.ID, Op: req.Op, OK: true}
			if req.validationError != "" {
				if !write(Response{ID: req.ID, Op: req.Op, Error: req.validationError}) {
					return
				}
				continue
			}
			if s.cfg.Profile == "movement" && req.Op != "register" && req.Op != "login" && req.Op != "view" && req.Op != "ping" {
				if !write(Response{ID: req.ID, Op: req.Op, Error: "operation is not available in movement profile"}) {
					return
				}
				continue
			}
			var err error
			switch req.Op {
			case "ping":
				data := map[string]any{"unix_ms": time.Now().UnixMilli()}
				if s.cfg.Profile == "movement" {
					data["protocol_version"] = "webgl-movement-v1"
				}
				resp.Data = data
			case "register":
				var out map[string]any
				err = s.call(ctx, s.cfg.BackendURL, "/v1/register", map[string]any{"name": req.Name}, &out)
				resp.Data = out
			case "login":
				if route != nil {
					err = errors.New("already logged in; reconnect to log in again")
					break
				}
				var out Route
				err = s.call(ctx, s.cfg.BackendURL, "/v1/login", map[string]any{"token": req.Token}, &out)
				if err == nil && s.cfg.LoginScene != "" && out.Role.Scene != s.cfg.LoginScene {
					payload := ref(out.Role)
					payload["target_scene_id"] = s.cfg.LoginScene
					payload["request_id"] = fmt.Sprintf("entry-%s-%d", out.Role.ID, out.Role.Session)
					err = s.call(ctx, s.cfg.BackendURL, "/v1/transfer", payload, &out)
				}
				if err == nil {
					route = &out
					lastView = nil
					nextSession = time.Now().Add(time.Second)
					publish(route)
					resp.Data = s.clientRole(out.Role)
				}
			default:
				if route == nil {
					err = errors.New("login required")
					break
				}
				switch req.Op {
				case "cast", "revive":
					if req.Seq <= 0 {
						err = errors.New("positive action seq is required")
						break
					}
					p := ref(route.Role)
					p["seq"] = req.Seq
					if req.Op == "cast" {
						if len(req.SkillID) == 0 || len(req.SkillID) > 32 || len(req.TargetID) > 128 {
							err = errors.New("invalid skill or target")
							break
						}
						p["skill_id"], p["target_id"] = req.SkillID, req.TargetID
					}
					var out map[string]json.RawMessage
					err = s.sceneCall(ctx, *route, "/v1/"+req.Op, p, &out)
					if err == nil {
						if snapshot, valid := actionSnapshot(out, *route, lastView); valid {
							lastView = snapshot
							// 持久化失败只记录日志，不能把已成功的结算伪装成未执行。
							s.checkpoint(ctx, *route, lastView)
						}
					}
					resp.Data = out
				case "view":
					var v View
					err = s.sceneCall(ctx, *route, "/v1/view", ref(route.Role), &v)
					if err == nil {
						overlayBalance(&v, route.Role)
						lastView = &v
						resp.Data = s.clientView(v)
					}
				case "transfer":
					if req.RequestID == "" || len(req.RequestID) > 128 {
						err = errors.New("request_id is required (1..128 characters)")
						break
					}
					p := ref(route.Role)
					p["target_scene_id"] = req.Target
					p["request_id"] = req.RequestID
					var out Route
					moveBarrier.Lock()
					err = s.call(ctx, s.cfg.BackendURL, "/v1/transfer", p, &out)
					if err == nil {
						route = &out
						lastView = nil
						publish(route)
						resp.Data = s.clientRole(out.Role)
					}
					moveBarrier.Unlock()
				case "claim":
					var out struct {
						Coins   int64 `json:"coins"`
						Granted bool  `json:"granted"`
					}
					err = s.call(ctx, s.cfg.BackendURL, "/v1/claim", map[string]any{"role_id": route.Role.ID, "session_epoch": route.Role.Session}, &out)
					if err == nil {
						route.Role.Coins = out.Coins
						resp.Data = out
					}
				default:
					err = errors.New("unknown operation")
				}
			}
			if err != nil {
				resp.OK = false
				resp.Data = nil
				resp.Error = err.Error()
			}
			if !write(resp) {
				return
			}
		case now := <-ticker.C:
			if route == nil {
				continue
			}
			if !now.Before(nextSession) {
				nextSession = now.Add(time.Second)
				var current Route
				cctx, ccancel := context.WithTimeout(ctx, 3*time.Second)
				err := s.call(cctx, s.cfg.BackendURL, "/v1/session", map[string]any{"role_id": route.Role.ID, "session_epoch": route.Role.Session}, &current)
				ccancel()
				if err != nil {
					if !write(Response{Op: "session", Error: err.Error()}) {
						return
					}
					var ue *upstreamError
					if errors.As(err, &ue) && (ue.status == 401 || ue.status == 403 || ue.status == 409) {
						return
					}
					// 无法核实会话时暂停本连接，避免继续提交旧输入。
					return
				}
				if current.Role.Owner != route.Role.Owner || current.Role.Scene != route.Role.Scene {
					lastView = nil
				}
				route = &current
				publish(route)
			}
			if !now.Before(nextView) {
				var v View
				err := s.sceneCall(ctx, *route, "/v1/view", ref(route.Role), &v)
				if err != nil {
					nextView = now.Add(time.Second)
					if !write(Response{Op: "state", Error: err.Error()}) {
						return
					}
				} else {
					nextView = now.Add(200 * time.Millisecond)
					overlayBalance(&v, route.Role)
					lastView = &v
					if !write(Response{Op: "state", OK: true, Data: s.clientView(v)}) {
						return
					}
				}
			}
			if !now.Before(nextSave) {
				nextSave = now.Add(2 * time.Second)
				s.checkpoint(ctx, *route, lastView)
			}
		}
	}
}
