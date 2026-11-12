package backend

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
)

// 战斗状态由场景计算；Backend 只验证快照边界并按归属代次保存，不计算伤害。
func combatRevision(raw json.RawMessage) (int64, error) {
	if len(raw) == 0 || bytes.Equal(bytes.TrimSpace(raw), []byte("null")) {
		return 0, nil
	}
	if len(raw) > 4096 {
		return 0, bad("战斗快照过大")
	}
	var v struct {
		HP            *int64           `json:"hp"`
		MaxHP         *int64           `json:"max_hp"`
		MP            *int64           `json:"mp"`
		MaxMP         *int64           `json:"max_mp"`
		Dead          *bool            `json:"dead"`
		Revive        *int64           `json:"revive_remaining_ms"`
		Revision      *int64           `json:"revision"`
		LastAction    *int64           `json:"last_action_seq"`
		ActionSession *int64           `json:"action_session"`
		Cooldowns     map[string]int64 `json:"cooldowns"`
	}
	if trimmed := bytes.TrimSpace(raw); len(trimmed) == 0 || trimmed[0] != '{' {
		return 0, bad("战斗快照必须为对象")
	}
	if err := json.Unmarshal(raw, &v); err != nil {
		return 0, bad("战斗快照字段无效")
	}
	if v.HP == nil || v.MaxHP == nil || v.MP == nil || v.MaxMP == nil || v.Dead == nil || v.Revive == nil || v.Revision == nil || v.LastAction == nil || v.ActionSession == nil ||
		*v.MaxHP < 1 || *v.MaxHP > 1000000 || *v.HP < 0 || *v.HP > *v.MaxHP || *v.MaxMP < 1 || *v.MaxMP > 1000000 || *v.MP < 0 || *v.MP > *v.MaxMP ||
		*v.Revive < 0 || *v.Revive > 600000 || *v.Revision < 0 || *v.LastAction < 0 || *v.ActionSession < 1 || *v.Dead != (*v.HP == 0) || (len(v.Cooldowns) != 4 && len(v.Cooldowns) != 6) {
		return 0, bad("战斗快照数值无效")
	}
	for skill, remaining := range v.Cooldowns {
		if (skill != "strike" && skill != "whirlwind" && skill != "charge" && skill != "mend" && skill != "arc_bolt" && skill != "stone_wall") || remaining < 0 || remaining > 600000 {
			return 0, bad("战斗冷却快照无效")
		}
	}
	for _, skill := range []string{"strike", "whirlwind", "charge", "mend"} {
		if _, ok := v.Cooldowns[skill]; !ok {
			return 0, bad("战斗冷却快照缺少基础技能")
		}
	}
	_, bolt := v.Cooldowns["arc_bolt"]
	_, wall := v.Cooldowns["stone_wall"]
	if bolt != wall {
		return 0, bad("战斗冷却快照不完整")
	}
	return *v.Revision, nil
}

func hasCombat(raw json.RawMessage) bool {
	return len(raw) != 0 && !bytes.Equal(bytes.TrimSpace(raw), []byte("null"))
}

func (s *Server) loadCombat(ctx context.Context, role *Role) error {
	var owner, session int64
	var state string
	err := s.db.QueryRowContext(ctx, "SELECT owner_epoch,session_epoch,state_json FROM combat_snapshots WHERE role_id=?", role.RoleID).Scan(&owner, &session, &state)
	if errors.Is(err, sql.ErrNoRows) {
		return nil
	}
	if err != nil {
		return err
	}
	if owner != role.OwnerEpoch || session > role.SessionEpoch {
		return unavailable("战斗快照归属不匹配")
	}
	role.Combat = json.RawMessage(state)
	if _, err = combatRevision(role.Combat); err != nil {
		return fmt.Errorf("invalid stored combat snapshot: %w", err)
	}
	return nil
}

func saveCombat(ctx context.Context, tx *sql.Tx, role Role) error {
	if !hasCombat(role.Combat) {
		return nil
	}
	revision, err := combatRevision(role.Combat)
	if err != nil {
		return err
	}
	var owner, session, previous int64
	err = tx.QueryRowContext(ctx, "SELECT owner_epoch,session_epoch,revision FROM combat_snapshots WHERE role_id=?", role.RoleID).Scan(&owner, &session, &previous)
	if errors.Is(err, sql.ErrNoRows) {
		_, err = tx.ExecContext(ctx, "INSERT INTO combat_snapshots(role_id,owner_epoch,session_epoch,revision,state_json) VALUES(?,?,?,?,?)", role.RoleID, role.OwnerEpoch, role.SessionEpoch, revision, string(role.Combat))
		return err
	}
	if err != nil {
		return err
	}
	if owner == role.OwnerEpoch && session == role.SessionEpoch && previous > revision {
		return conflict("战斗快照已过期")
	}
	_, err = tx.ExecContext(ctx, "UPDATE combat_snapshots SET owner_epoch=?,session_epoch=?,revision=?,state_json=? WHERE role_id=?", role.OwnerEpoch, role.SessionEpoch, revision, string(role.Combat), role.RoleID)
	return err
}
