package backend

import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"
)

func validIdentityHex(value string, size int) bool {
	if len(value) != size {
		return false
	}
	_, err := hex.DecodeString(value)
	return err == nil
}

func validIdentityName(value string) bool {
	if value != strings.TrimSpace(value) || !utf8.ValidString(value) || utf8.RuneCountInString(value) < 1 || utf8.RuneCountInString(value) > 24 {
		return false
	}
	for _, letter := range value {
		if unicode.IsControl(letter) {
			return false
		}
	}
	return true
}

func newAuthClient(config Config) (*http.Client, error) {
	if config.AuthURL == "" {
		return nil, nil
	}
	u, err := url.Parse(config.AuthURL)
	if err != nil || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") || u.User != nil || u.Path != "" || u.RawQuery != "" || u.Fragment != "" {
		return nil, errors.New("AUTH_URL 必须是无凭据、路径或查询参数的HTTP(S)服务地址")
	}
	client := &http.Client{Timeout: 5 * time.Second}
	if config.AuthClient != nil {
		copy := *config.AuthClient
		client = &copy
		if client.Timeout <= 0 {
			client.Timeout = 5 * time.Second
		}
	}
	client.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	return client, nil
}

func (s *Server) callAuth(ctx context.Context, operation string, in, out any) error {
	body, err := json.Marshal(in)
	if err != nil {
		return bad("无效鉴权请求")
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.config.AuthURL+"/v1/"+operation, bytes.NewReader(body))
	if err != nil {
		return unavailable("鉴权地址无效")
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Internal-Token", s.config.Token)
	response, err := s.authClient.Do(req)
	if err != nil {
		return unavailable("登录鉴权服务不可用，请稍后重试")
	}
	defer response.Body.Close()
	data, err := io.ReadAll(io.LimitReader(response.Body, 64*1024+1))
	if err != nil || len(data) > 64*1024 {
		return unavailable("鉴权响应读取失败")
	}
	if response.StatusCode != http.StatusOK {
		// 不转发上游正文，避免错误响应意外包含凭据。
		switch response.StatusCode {
		case 400:
			return bad("账号请求格式无效")
		case 401:
			return &apiError{401, "登录 token 无效"}
		case 409:
			return conflict("名字已被注册；请使用原 token 登录")
		default:
			return unavailable("登录鉴权服务暂不可用")
		}
	}
	if json.Unmarshal(data, out) != nil {
		return unavailable("鉴权响应格式无效")
	}
	return nil
}

func (s *Server) identityRequest(ctx context.Context, operation string, raw json.RawMessage) (any, error) {
	if operation == "register" {
		var request struct {
			Name string `json:"name"`
		}
		if err := decode(raw, &request); err != nil {
			return nil, err
		}
		var result struct {
			Token  string `json:"token"`
			RoleID string `json:"role_id"`
		}
		if err := s.callAuth(ctx, "register", request, &result); err != nil {
			return nil, err
		}
		if !validIdentityHex(result.Token, 64) || !validIdentityHex(result.RoleID, 32) {
			return nil, unavailable("鉴权返回无效账号")
		}
		// 先把凭据交给客户端保存；游戏角色在首次成功登录时幂等创建。
		return result, nil
	}
	var request struct {
		Token string `json:"token"`
	}
	if err := decode(raw, &request); err != nil {
		return nil, err
	}
	if len(request.Token) != 64 {
		return nil, &apiError{401, "登录 token 无效"}
	}
	var identity struct {
		RoleID string `json:"role_id"`
		Name   string `json:"name"`
	}
	if err := s.callAuth(ctx, "authenticate", request, &identity); err != nil {
		return nil, err
	}
	if !validIdentityHex(identity.RoleID, 32) || !validIdentityName(identity.Name) {
		return nil, unavailable("鉴权返回无效身份")
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	role, err := s.role(ctx, identity.RoleID)
	var missing *apiError
	if errors.As(err, &missing) && missing.status == 404 {
		// token_hash 是旧结构的兼容字段；分离模式不使用它作鉴权决定。
		_, err = s.db.ExecContext(ctx, `INSERT INTO roles(role_id,name,token_hash,scene_id,owner_epoch,epoch_counter,session_epoch,x,y,coins) VALUES(?,?,?,?,1,1,0,0,0,0)`, identity.RoleID, identity.Name, tokenHash(request.Token), s.config.DefaultScene)
		if err != nil {
			return nil, unavailable("游戏角色初始化失败；请保留凭据稍后重新登录")
		}
	} else if err != nil {
		return nil, err
	} else if role.RoleID != identity.RoleID || role.Name != identity.Name {
		return nil, unavailable("角色身份不匹配")
	}
	return s.loginRole(ctx, identity.RoleID)
}
