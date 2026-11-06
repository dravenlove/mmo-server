package backend

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// Role 的资产字段只能由 Backend 的数据库事务修改。
type Role struct {
	RoleID       string          `json:"role_id"`
	Name         string          `json:"name"`
	SceneID      string          `json:"scene_id"`
	OwnerEpoch   int64           `json:"owner_epoch"`
	SessionEpoch int64           `json:"session_epoch"`
	X            float64         `json:"x"`
	Y            float64         `json:"y"`
	Coins        int64           `json:"coins"`
	Combat       json.RawMessage `json:"combat,omitempty"`
}

type Ref struct {
	RoleID       string `json:"role_id"`
	OwnerEpoch   int64  `json:"owner_epoch"`
	SessionEpoch int64  `json:"session_epoch"`
}

func (r Role) Ref() Ref { return Ref{r.RoleID, r.OwnerEpoch, r.SessionEpoch} }

type Route struct {
	Role     Role   `json:"role"`
	SceneURL string `json:"scene_url"`
}

type Config struct {
	Token        string
	DBDriver     string
	DBPath       string
	MySQL        MySQLConfig
	Scenes       map[string]string
	DefaultScene string
	HTTPClient   *http.Client
}

// MySQLConfig 使用独立字段，密码中的 @、:、/ 不会被当作连接串语法。
type MySQLConfig struct {
	Host, Port, User, Password, Database, TLS string
}

// ParseScenes 保留配置中第一项作为新角色默认场景。
func ParseScenes(raw string) (map[string]string, string, error) {
	scenes := make(map[string]string)
	first := ""
	for _, entry := range strings.Split(raw, ",") {
		parts := strings.SplitN(strings.TrimSpace(entry), "=", 2)
		if len(parts) != 2 || strings.TrimSpace(parts[0]) == "" {
			return nil, "", fmt.Errorf("SCENES 必须是 scene-1=http://host:port,scene-2=http://host:port")
		}
		id, address := strings.TrimSpace(parts[0]), strings.TrimRight(strings.TrimSpace(parts[1]), "/")
		u, err := url.Parse(address)
		if err != nil || u == nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" || u.User != nil || u.Path != "" || u.RawQuery != "" || u.Fragment != "" {
			return nil, "", fmt.Errorf("场景 %s 地址必须为不含凭据、路径的 HTTP(S) 服务地址", id)
		}
		if _, exists := scenes[id]; exists {
			return nil, "", fmt.Errorf("重复的场景标识 %s", id)
		}
		if len(id) > 64 {
			return nil, "", fmt.Errorf("场景标识过长")
		}
		scenes[id] = address
		if first == "" {
			first = id
		}
	}
	return scenes, first, nil
}

func defaultClient() *http.Client {
	return &http.Client{Timeout: 2 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
}

type apiError struct {
	status  int
	message string
}

func (e *apiError) Error() string      { return e.message }
func bad(message string) error         { return &apiError{http.StatusBadRequest, message} }
func conflict(message string) error    { return &apiError{http.StatusConflict, message} }
func unavailable(message string) error { return &apiError{http.StatusServiceUnavailable, message} }

type sessionRequest struct {
	RoleID       string `json:"role_id"`
	SessionEpoch int64  `json:"session_epoch"`
}

type transferRequest struct {
	Ref
	TargetSceneID string `json:"target_scene_id"`
	RequestID     string `json:"request_id"`
}

type transferRef struct {
	Ref
	TransferID string `json:"transfer_id"`
}

type roleResult struct {
	Role Role `json:"role"`
}

type migration struct {
	RoleID, RequestID, TransferID, TargetSceneID, Status string
	Source, Target                                       Role
}
