package backend

import (
	"context"
	"encoding/json"
	"time"
)

// 账号密码只经过游戏后台转发，不写入游戏表，也不持有在线游戏状态锁。
func (s *Server) accountRequest(ctx context.Context, operation string, raw json.RawMessage) (any, error) {
	if s.config.AuthURL == "" {
		return nil, unavailable("账号密码登录需要配置独立 AUTH_URL")
	}
	var request struct {
		Name     string `json:"name,omitempty"`
		Password string `json:"password"`
		Token    string `json:"token,omitempty"`
	}
	if err := decode(raw, &request); err != nil {
		return nil, err
	}
	if operation == "account_bind" {
		var result struct {
			Bound  bool   `json:"bound"`
			RoleID string `json:"role_id"`
			Name   string `json:"name"`
		}
		if err := s.callAuth(ctx, operation, request, &result); err != nil {
			return nil, err
		}
		if !result.Bound || !validIdentityHex(result.RoleID, 32) || !validIdentityName(result.Name) {
			return nil, unavailable("鉴权返回无效绑定结果")
		}
		return result, nil
	}
	var result struct {
		RoleID    string `json:"role_id"`
		Name      string `json:"name"`
		Token     string `json:"token"`
		ExpiresAt int64  `json:"expires_at_unix_ms"`
	}
	if err := s.callAuth(ctx, operation, request, &result); err != nil {
		return nil, err
	}
	if !validIdentityHex(result.RoleID, 32) || !validIdentityName(result.Name) || !validIdentityHex(result.Token, 64) || result.ExpiresAt <= time.Now().UnixMilli() || result.ExpiresAt > time.Now().Add(25*time.Hour).UnixMilli() {
		return nil, unavailable("鉴权返回无效登录票据")
	}
	return result, nil
}
