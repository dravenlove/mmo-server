package backend

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math"
	"net/http"
	"strings"
	"sync"
	"time"
	"unicode"
	"unicode/utf8"
)

type Server struct {
	mu         sync.Mutex
	db         *store
	config     Config
	client     *http.Client
	authClient *http.Client
}

func New(config Config) (*Server, error) {
	if len(config.Token) < 32 {
		return nil, fmt.Errorf("INTERNAL_TOKEN 至少需要 32 字符")
	}
	config.DBDriver = strings.ToLower(strings.TrimSpace(config.DBDriver))
	if config.DBDriver == "" {
		config.DBDriver = "sqlite"
	}
	if config.DBDriver != "sqlite" && config.DBDriver != "mysql" {
		return nil, fmt.Errorf("BACKEND_DB_DRIVER 只支持 sqlite 或 mysql")
	}
	if config.DBDriver == "sqlite" && config.DBPath == "" {
		return nil, fmt.Errorf("BACKEND_DB 不能为空")
	}
	if len(config.Scenes) == 0 || config.Scenes[config.DefaultScene] == "" {
		return nil, fmt.Errorf("必须配置默认场景")
	}
	// 配置也可能由测试或嵌入式调用构造，统一校验地址，禁止 HTTP 重定向泄露凭据。
	for id, address := range config.Scenes {
		if _, _, err := ParseScenes(id + "=" + address); err != nil {
			return nil, err
		}
	}
	authClient, err := newAuthClient(config)
	if err != nil {
		return nil, err
	}
	db, err := openStore(config)
	if err != nil {
		return nil, err
	}
	client := config.HTTPClient
	if client == nil {
		client = defaultClient()
	} else {
		copy := *client
		client = &copy
		client.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
		if client.Timeout <= 0 {
			client.Timeout = defaultClient().Timeout
		}
	}
	return &Server{db: db, config: config, client: client, authClient: authClient}, nil
}

func (s *Server) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.db.Close()
}

// Health 与业务操作串行，避免在同一 MySQL 连接的事务/结果流中插入探活。
// 超时从获得锁后计算，不把排队时间误判为数据库断线。
func (s *Server) Health() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	return s.db.PingContext(ctx)
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			writeError(w, &apiError{http.StatusMethodNotAllowed, "只允许 GET"})
			return
		}
		if err := s.Health(); err != nil {
			writeError(w, unavailable("数据库不可用"))
			return
		}
		writeJSON(w, map[string]any{"ok": true, "service": "backend"})
	})
	methods := map[string]func(context.Context, json.RawMessage) (any, error){
		"register": s.register, "login": s.login, "session": s.session,
		"checkpoint": s.checkpoint, "transfer": s.transfer, "claim": s.claim,
	}
	for name, method := range methods {
		handler := method
		operation := name
		mux.HandleFunc("/v1/"+name, func(w http.ResponseWriter, r *http.Request) {
			if r.Method != http.MethodPost {
				writeError(w, &apiError{http.StatusMethodNotAllowed, "只允许 POST"})
				return
			}
			if subtle.ConstantTimeCompare([]byte(r.Header.Get("X-Internal-Token")), []byte(s.config.Token)) != 1 {
				writeError(w, &apiError{http.StatusUnauthorized, "内部服务凭据无效"})
				return
			}
			r.Body = http.MaxBytesReader(w, r.Body, 64*1024)
			body, err := io.ReadAll(r.Body)
			if err != nil {
				writeError(w, bad("请求过大或读取失败"))
				return
			}
			var data any
			if s.config.AuthURL != "" && (operation == "register" || operation == "login") {
				// 跨地域鉴权 I/O 不持有游戏状态锁，不能阻塞在线会话和保存。
				data, err = s.identityRequest(r.Context(), operation, body)
			} else {
				s.mu.Lock()
				data, err = handler(r.Context(), body)
				s.mu.Unlock()
			}
			if err != nil {
				writeError(w, err)
				return
			}
			writeJSON(w, data)
		})
	}
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		writeError(w, &apiError{http.StatusNotFound, "接口不存在"})
	})
	return mux
}

func writeJSON(w http.ResponseWriter, data any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	_ = json.NewEncoder(w).Encode(data)
}

func writeError(w http.ResponseWriter, err error) {
	status, message := http.StatusInternalServerError, "服务内部错误"
	var api *apiError
	if errors.As(err, &api) {
		status, message = api.status, api.message
	} else {
		log.Printf("backend operation failed: %v", err)
	}
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": message})
}

func decode(raw []byte, target any) error {
	if len(raw) == 0 || bytes.Equal(bytes.TrimSpace(raw), []byte("null")) {
		return bad("请求必须是 JSON 对象")
	}
	d := json.NewDecoder(bytes.NewReader(raw))
	d.DisallowUnknownFields()
	if err := d.Decode(target); err != nil {
		return bad("请求 JSON 或字段无效")
	}
	var extra any
	if err := d.Decode(&extra); err != io.EOF {
		return bad("请求只能包含一个 JSON 对象")
	}
	return nil
}

func randomHex(count int) (string, error) {
	b := make([]byte, count)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}

func tokenHash(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}

func (s *Server) register(ctx context.Context, raw json.RawMessage) (any, error) {
	var req struct {
		Name string `json:"name"`
	}
	if err := decode(raw, &req); err != nil {
		return nil, err
	}
	name := strings.TrimSpace(req.Name)
	if !utf8.ValidString(name) || utf8.RuneCountInString(name) < 1 || utf8.RuneCountInString(name) > 24 {
		return nil, bad("名字需要 1 到 24 个字符")
	}
	for _, r := range name {
		if unicode.IsControl(r) {
			return nil, bad("名字不能含控制字符")
		}
	}
	var count int
	if err := s.db.QueryRowContext(ctx, "SELECT COUNT(*) FROM roles WHERE name=?", name).Scan(&count); err != nil {
		return nil, err
	}
	if count != 0 {
		return nil, conflict("名字已被注册；已有角色请使用原 token 登录")
	}
	token, err := randomHex(32)
	if err != nil {
		return nil, err
	}
	id, err := randomHex(16)
	if err != nil {
		return nil, err
	}
	_, err = s.db.ExecContext(ctx, `INSERT INTO roles(role_id,name,token_hash,scene_id,owner_epoch,epoch_counter,session_epoch,x,y,coins) VALUES(?,?,?,?,1,1,0,0,0,0)`, id, name, tokenHash(token), s.config.DefaultScene)
	if err != nil {
		return nil, err
	}
	return map[string]string{"token": token, "role_id": id}, nil
}

func (s *Server) login(ctx context.Context, raw json.RawMessage) (any, error) {
	var req struct {
		Token string `json:"token"`
	}
	if err := decode(raw, &req); err != nil {
		return nil, err
	}
	if len(req.Token) != 64 {
		return nil, &apiError{401, "登录 token 无效"}
	}
	r, err := scanRole(s.db.QueryRowContext(ctx, "SELECT "+columns+" FROM roles WHERE token_hash=?", tokenHash(req.Token)))
	if errors.Is(err, sql.ErrNoRows) {
		return nil, &apiError{401, "登录 token 无效"}
	}
	if err != nil {
		return nil, err
	}
	return s.loginRole(ctx, r.RoleID)
}

// 调用方持有游戏状态锁；会话代次和场景归属始终由游戏侧管理。
func (s *Server) loginRole(ctx context.Context, id string) (any, error) {
	if err := s.recoverRole(ctx, id); err != nil {
		return nil, err
	}
	if _, err := s.db.ExecContext(ctx, "UPDATE roles SET session_epoch=session_epoch+1 WHERE role_id=?", id); err != nil {
		return nil, err
	}
	r, err := s.role(ctx, id)
	if err != nil {
		return nil, err
	}
	var result roleResult
	if err = s.callScene(ctx, r.SceneID, "enter", map[string]any{"role": r}, &result); err != nil {
		return nil, unavailable("当前场景无法入场，请稍后重新登录")
	}
	if !validSnapshot(result.Role, r) {
		return nil, unavailable("场景返回了无效角色快照")
	}
	r.X, r.Y = result.Role.X, result.Role.Y
	if hasCombat(result.Role.Combat) {
		r.Combat = result.Role.Combat
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	if _, err = tx.ExecContext(ctx, "UPDATE roles SET x=?,y=? WHERE role_id=? AND owner_epoch=? AND session_epoch=?", r.X, r.Y, r.RoleID, r.OwnerEpoch, r.SessionEpoch); err != nil {
		return nil, err
	}
	if err = saveCombat(ctx, tx, r); err != nil {
		return nil, err
	}
	if err = tx.Commit(); err != nil {
		return nil, err
	}
	return s.route(r), nil
}

func (s *Server) session(ctx context.Context, raw json.RawMessage) (any, error) {
	var req sessionRequest
	if err := decode(raw, &req); err != nil {
		return nil, err
	}
	if _, err := s.validateSession(ctx, req); err != nil {
		return nil, err
	}
	if err := s.recoverRole(ctx, req.RoleID); err != nil {
		return nil, err
	}
	r, err := s.role(ctx, req.RoleID)
	if err != nil {
		return nil, err
	}
	return s.route(r), nil
}

func validPosition(x, y float64) bool {
	return !math.IsNaN(x) && !math.IsNaN(y) && !math.IsInf(x, 0) && !math.IsInf(y, 0) && x >= -100 && x <= 100 && y >= -100 && y <= 100
}
func validSnapshot(actual, expected Role) bool {
	_, err := combatRevision(actual.Combat)
	return err == nil && actual.Ref() == expected.Ref() && actual.SceneID == expected.SceneID && validPosition(actual.X, actual.Y)
}

func (s *Server) checkpoint(ctx context.Context, raw json.RawMessage) (any, error) {
	var req roleResult
	if err := decode(raw, &req); err != nil {
		return nil, err
	}
	r := req.Role
	if r.RoleID == "" || r.OwnerEpoch <= 0 || r.SessionEpoch <= 0 || !validPosition(r.X, r.Y) {
		return nil, bad("无效角色快照")
	}
	if _, err := combatRevision(r.Combat); err != nil {
		return nil, err
	}
	// 单 Backend + 全局串行锁使归属提交与检查点更新共享同一串行化边界。
	// SQLite 文件锁 / MySQL 会话锁共同保证只有一个 Backend；本实现不支持多副本。
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	result, err := tx.ExecContext(ctx, "UPDATE roles SET x=?,y=? WHERE role_id=? AND scene_id=? AND owner_epoch=? AND session_epoch=? AND NOT EXISTS(SELECT 1 FROM migrations WHERE role_id=? AND status NOT IN ('DONE','ABORTED'))", r.X, r.Y, r.RoleID, r.SceneID, r.OwnerEpoch, r.SessionEpoch, r.RoleID)
	if err != nil {
		return nil, err
	}
	n, err := result.RowsAffected()
	if err != nil {
		return nil, err
	}
	if n != 1 {
		return nil, conflict("快照归属或会话已失效，或角色正在迁移")
	}
	if err = saveCombat(ctx, tx, r); err != nil {
		return nil, err
	}
	if err = tx.Commit(); err != nil {
		return nil, err
	}
	return map[string]bool{"ok": true}, nil
}

func (s *Server) claim(ctx context.Context, raw json.RawMessage) (any, error) {
	var req sessionRequest
	if err := decode(raw, &req); err != nil {
		return nil, err
	}
	if _, err := s.validateSession(ctx, req); err != nil {
		return nil, err
	}
	if err := s.recoverRole(ctx, req.RoleID); err != nil {
		return nil, err
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	_, err = tx.ExecContext(ctx, "INSERT INTO claims(operation_id,role_id) VALUES(?,?)", "welcome:"+req.RoleID, req.RoleID)
	granted := err == nil
	if err != nil && !uniqueViolation(err) {
		return nil, err
	}
	if granted {
		if _, err = tx.ExecContext(ctx, "UPDATE roles SET coins=coins+100 WHERE role_id=?", req.RoleID); err != nil {
			return nil, err
		}
	}
	var coins int64
	if err = tx.QueryRowContext(ctx, "SELECT coins FROM roles WHERE role_id=?", req.RoleID).Scan(&coins); err != nil {
		return nil, err
	}
	if err = tx.Commit(); err != nil {
		return nil, err
	}
	return map[string]any{"coins": coins, "granted": granted}, nil
}

func (s *Server) route(r Role) Route { return Route{Role: r, SceneURL: s.config.Scenes[r.SceneID]} }

type sceneError struct {
	Status    int
	Operation string
}

func (e *sceneError) Error() string {
	return fmt.Sprintf("scene %s returned HTTP %d", e.Operation, e.Status)
}

func (s *Server) callScene(ctx context.Context, scene, operation string, data, out any) error {
	address := s.config.Scenes[scene]
	if address == "" {
		return bad("未知场景")
	}
	payload, err := json.Marshal(data)
	if err != nil {
		return err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, address+"/v1/"+operation, bytes.NewReader(payload))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Internal-Token", s.config.Token)
	resp, err := s.client.Do(req)
	if err != nil {
		return fmt.Errorf("scene %s unavailable", operation)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 65536))
		return &sceneError{resp.StatusCode, operation}
	}
	if out == nil {
		_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 65536))
		return nil
	}
	if err = json.NewDecoder(io.LimitReader(resp.Body, 65537)).Decode(out); err != nil {
		return fmt.Errorf("scene %s invalid response", operation)
	}
	return nil
}
