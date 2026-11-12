package gateway

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math"
)

func elementalRejected(reason string) map[string]any {
	return map[string]any{"accepted": false, "reason": reason}
}

// 所有身份字段仍来自连接路由；客户端归属只用于拒绝迟到输入。
func (s *Server) elementalOperation(ctx context.Context, req Request, current *Route) (any, error) {
	if req.SceneID == "" || len(req.SceneID) > 64 || req.OwnerEpoch == nil || *req.OwnerEpoch <= 0 {
		return nil, errors.New("scene_id and positive owner_epoch are required")
	}
	if req.SceneID != current.Role.Scene || *req.OwnerEpoch != current.Role.Owner {
		return elementalRejected("scene_changed"), nil
	}
	if req.Op == "cast" {
		if req.X == nil || req.Y == nil || math.Trunc(*req.X) != *req.X || math.Trunc(*req.Y) != *req.Y || len(req.Element) > 32 {
			return nil, errors.New("element and integer target cells are required")
		}
		if current.Role.Scene != s.cfg.ArenaScene {
			return elementalRejected("outside_arena"), nil
		}
		p := ref(current.Role)
		p["scene_id"] = current.Role.Scene
		p["request_id"] = req.ID
		p["element"] = req.Element
		p["x"], p["y"] = *req.X, *req.Y
		var result map[string]any
		err := s.sceneCall(ctx, *current, "/v1/elemental_cast", p, &result)
		return result, err
	}
	target := s.cfg.ArenaScene
	if req.Op == "enter_arena" && current.Role.Scene != s.cfg.LoginScene {
		return elementalRejected("already_in_arena"), nil
	}
	if req.Op != "enter_arena" && current.Role.Scene != s.cfg.ArenaScene {
		return elementalRejected("outside_arena"), nil
	}
	if req.Op == "leave_arena" {
		target = s.cfg.LoginScene
	}
	hash := sha256.Sum256([]byte(req.ID))
	requestID := fmt.Sprintf("elemental-%d-%d-%s-%s", current.Role.Session, current.Role.Owner, req.Op, hex.EncodeToString(hash[:12]))
	payload := ref(current.Role)
	payload["target_scene_id"] = target
	payload["request_id"] = requestID
	if req.Op == "restart_battle" {
		payload["restart"] = true
	}
	var next Route
	if err := s.call(ctx, s.cfg.BackendURL, "/v1/transfer", payload, &next); err != nil {
		return nil, err
	}
	if next.Role.ID != current.Role.ID || next.Role.Session != current.Role.Session || next.Role.Scene != target || next.Role.Owner <= current.Role.Owner {
		return nil, errors.New("invalid elemental ownership transition")
	}
	return next, nil
}

func elementalClientView(v View) View {
	// 自由区位置/旧战斗存档在内部保留，公开快照投影为这场试炼的角色。
	v.Self.Combat = nil
	v.Players = append([]Role(nil), v.Players...)
	for i := range v.Players {
		v.Players[i].Combat = nil
	}
	if len(v.Battle) == 0 || string(v.Battle) == "null" {
		return v
	}
	var battle struct {
		Player struct {
			ID    string `json:"id"`
			X, Y  float64
			HP    float64 `json:"hp"`
			MaxHP float64 `json:"max_hp"`
		} `json:"player"`
	}
	if json.Unmarshal(v.Battle, &battle) != nil || battle.Player.ID != v.Self.ID {
		return v
	}
	p := battle.Player
	v.Self.X, v.Self.Y = p.X, p.Y
	v.Self.Combat, _ = json.Marshal(map[string]any{"dead": p.HP <= 0})
	for i := range v.Players {
		if v.Players[i].ID == v.Self.ID {
			v.Players[i] = v.Self
		}
	}
	return v
}
