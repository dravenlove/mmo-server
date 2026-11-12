package backend

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"unicode"
)

// Transfer 的提交点只存在于数据库事务中，网络超时不改变已提交归属。
func (s *Server) transfer(ctx context.Context, raw json.RawMessage) (any, error) {
	var req transferRequest
	if err := decode(raw, &req); err != nil {
		return nil, err
	}
	if len(req.RequestID) < 1 || len(req.RequestID) > 128 || req.OwnerEpoch <= 0 {
		return nil, bad("无效迁移请求")
	}
	for _, r := range req.RequestID {
		if unicode.IsControl(r) {
			return nil, bad("无效迁移请求标识")
		}
	}
	if s.config.Scenes[req.TargetSceneID] == "" {
		return nil, bad("目标场景不存在")
	}
	r, err := s.validateSession(ctx, sessionRequest{req.RoleID, req.SessionEpoch})
	if err != nil {
		return nil, err
	}
	previous, err := s.migration(ctx, req.RoleID, req.RequestID)
	if err == nil {
		if previous.TargetSceneID != req.TargetSceneID {
			return nil, conflict("相同 request_id 不能用于不同目标")
		}
		if previous.Status != "DONE" && previous.Status != "ABORTED" {
			if err = s.recoverMigration(ctx, &previous); err != nil {
				return nil, err
			}
		}
		if previous.Status == "ABORTED" {
			return nil, conflict("这次迁移已经取消，请使用新的 request_id")
		}
		r, err = s.role(ctx, r.RoleID)
		if err != nil {
			return nil, err
		}
		return s.route(r), nil
	}
	if !errors.Is(err, sql.ErrNoRows) {
		return nil, err
	}
	if err = s.recoverRole(ctx, req.RoleID); err != nil {
		return nil, err
	}
	r, err = s.role(ctx, req.RoleID)
	if err != nil {
		return nil, err
	}
	if req.Ref != r.Ref() {
		return nil, conflict("角色归属已变化，请刷新会话路由")
	}
	if r.SceneID == req.TargetSceneID && !req.Restart {
		return nil, bad("角色已在目标场景")
	}
	if req.Restart {
		if r.SceneID != req.TargetSceneID {
			return nil, bad("归属轮换必须使用当前场景")
		}
		var capabilities struct {
			OwnerRotation bool `json:"owner_rotation"`
		}
		if err := s.callScene(ctx, r.SceneID, "capabilities", map[string]any{}, &capabilities); err != nil || !capabilities.OwnerRotation {
			return nil, bad("当前场景不支持同场景归属轮换")
		}
	}
	m, err := s.beginMigration(ctx, r, req)
	if err != nil {
		return nil, err
	}
	var frozen roleResult
	if err = s.callScene(ctx, m.Source.SceneID, "freeze", transferRef{m.Source.Ref(), m.TransferID}, &frozen); err != nil {
		return nil, s.cancelAfterFailure(ctx, &m, "源场景冻结失败，迁移未提交")
	}
	if !validSnapshot(frozen.Role, m.Source) {
		return nil, s.cancelAfterFailure(ctx, &m, "源场景返回了无效快照")
	}
	m.Source.X, m.Source.Y = frozen.Role.X, frozen.Role.Y
	m.Target.X, m.Target.Y = frozen.Role.X, frozen.Role.Y
	if hasCombat(frozen.Role.Combat) {
		m.Source.Combat = append(json.RawMessage(nil), frozen.Role.Combat...)
		m.Target.Combat = append(json.RawMessage(nil), frozen.Role.Combat...)
	}
	sourceJSON, _ := json.Marshal(m.Source)
	targetJSON, _ := json.Marshal(m.Target)
	if _, err = s.db.ExecContext(ctx, "UPDATE migrations SET source_json=?,target_json=? WHERE role_id=? AND request_id=? AND status='PREPARING'", string(sourceJSON), string(targetJSON), m.RoleID, m.RequestID); err != nil {
		return nil, unavailable("保存迁移快照失败，角色暂时暂停；重试或重新登录恢复")
	}
	var prepared roleResult
	if err = s.callScene(ctx, m.Target.SceneID, "prepare", map[string]any{"role": m.Target, "transfer_id": m.TransferID}, &prepared); err != nil {
		return nil, s.cancelAfterFailure(ctx, &m, "目标场景准备失败，迁移未提交")
	}
	if !validSnapshot(prepared.Role, m.Target) {
		return nil, s.cancelAfterFailure(ctx, &m, "目标场景返回了无效快照")
	}
	if err = s.commitMigration(ctx, &m); err != nil {
		// COMMIT 返回错误时结果可能不明，后续以持久记录判定，绝不盲目解冻源。
		return nil, unavailable("归属提交结果待核实，请重试相同 request_id 或重新登录")
	}
	if err = s.finishCommitted(ctx, &m); err != nil {
		return nil, err
	}
	r, err = s.role(ctx, m.RoleID)
	if err != nil {
		return nil, err
	}
	return s.route(r), nil
}

func (s *Server) beginMigration(ctx context.Context, r Role, req transferRequest) (migration, error) {
	id, err := randomHex(16)
	if err != nil {
		return migration{}, err
	}
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return migration{}, err
	}
	defer tx.Rollback()
	// 候选代次被取消后仍不复用，防止与场景中的删除墓碑冲突。
	if _, err = tx.ExecContext(ctx, "UPDATE roles SET epoch_counter=epoch_counter+1 WHERE role_id=?", r.RoleID); err != nil {
		return migration{}, err
	}
	var epoch int64
	if err = tx.QueryRowContext(ctx, "SELECT epoch_counter FROM roles WHERE role_id=?", r.RoleID).Scan(&epoch); err != nil {
		return migration{}, err
	}
	if epoch <= r.OwnerEpoch {
		return migration{}, fmt.Errorf("owner epoch counter exhausted or invalid")
	}
	target := r
	target.SceneID = req.TargetSceneID
	target.OwnerEpoch = epoch
	m := migration{RoleID: r.RoleID, RequestID: req.RequestID, TransferID: id, TargetSceneID: req.TargetSceneID, Status: "PREPARING", Source: r, Target: target}
	sourceJSON, _ := json.Marshal(m.Source)
	targetJSON, _ := json.Marshal(m.Target)
	if _, err = tx.ExecContext(ctx, "INSERT INTO migrations("+migrationColumns+") VALUES(?,?,?,?,?,?,?)", m.RoleID, m.RequestID, m.TransferID, m.TargetSceneID, m.Status, string(sourceJSON), string(targetJSON)); err != nil {
		return migration{}, err
	}
	if err = tx.Commit(); err != nil {
		return migration{}, err
	}
	return m, nil
}

func (s *Server) commitMigration(ctx context.Context, m *migration) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	result, err := tx.ExecContext(ctx, "UPDATE roles SET scene_id=?,owner_epoch=?,x=?,y=? WHERE role_id=? AND scene_id=? AND owner_epoch=? AND session_epoch=?", m.Target.SceneID, m.Target.OwnerEpoch, m.Target.X, m.Target.Y, m.Source.RoleID, m.Source.SceneID, m.Source.OwnerEpoch, m.Source.SessionEpoch)
	if err != nil {
		return err
	}
	n, err := result.RowsAffected()
	if err != nil {
		return err
	}
	if n != 1 {
		return conflict("迁移提交时源归属已变化")
	}
	if err = saveCombat(ctx, tx, m.Target); err != nil {
		return err
	}
	result, err = tx.ExecContext(ctx, "UPDATE migrations SET status='COMMITTED' WHERE role_id=? AND request_id=? AND status='PREPARING'", m.RoleID, m.RequestID)
	if err != nil {
		return err
	}
	n, err = result.RowsAffected()
	if err != nil {
		return err
	}
	if n != 1 {
		return conflict("迁移状态已变化")
	}
	if err = tx.Commit(); err != nil {
		return err
	}
	m.Status = "COMMITTED"
	return nil
}

func (s *Server) cancelAfterFailure(ctx context.Context, m *migration, message string) error {
	if err := s.setStatus(ctx, m, "ABORTING"); err != nil {
		return unavailable("迁移取消状态待核实，请稍后重试")
	}
	if err := s.abortMigration(ctx, m); err != nil {
		return err
	}
	return unavailable(message + "；已安全取消，使用新的 request_id 可重试")
}

func (s *Server) abortMigration(ctx context.Context, m *migration) error {
	r, err := s.role(ctx, m.RoleID)
	if err != nil {
		return err
	}
	if r.Ref() != m.Source.Ref() || r.SceneID != m.Source.SceneID {
		return unavailable("迁移撤销归属校验失败，角色保持暂停")
	}
	if m.Status != "ABORTING" {
		if err = s.setStatus(ctx, m, "ABORTING"); err != nil {
			return err
		}
	}
	// 先作废目标候选，再恢复源。remove 即使目标未创建，也留下代次墓碑。
	if err = s.callScene(ctx, m.Target.SceneID, "remove", transferRef{m.Target.Ref(), m.TransferID}, nil); err != nil {
		return unavailable("迁移撤销等待目标场景恢复，角色暂时暂停")
	}
	err = s.callScene(ctx, m.Source.SceneID, "unfreeze", transferRef{m.Source.Ref(), m.TransferID}, nil)
	var sceneErr *sceneError
	if err == nil || (errors.As(err, &sceneErr) && sceneErr.Status == 404) {
		// enter 对仍活跃的相同会话不覆写实时数据，同时取得解冻后的权威快照。
		// 源进程重启时则按仍有效的源归属恢复到已保存的冻结快照。
		var result roleResult
		err = s.callScene(ctx, m.Source.SceneID, "enter", map[string]any{"role": m.Source}, &result)
		if err == nil && !validSnapshot(result.Role, m.Source) {
			err = fmt.Errorf("invalid restored role")
		}
		if err == nil {
			m.Source.X, m.Source.Y = result.Role.X, result.Role.Y
			if hasCombat(result.Role.Combat) {
				m.Source.Combat = result.Role.Combat
			}
		}
	}
	if err != nil {
		return unavailable("迁移撤销等待源场景恢复，角色暂时暂停")
	}
	// 取消恢复到冻结坐标；资产仍以数据库为准。
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err = tx.ExecContext(ctx, "UPDATE roles SET x=?,y=? WHERE role_id=? AND owner_epoch=? AND session_epoch=?", m.Source.X, m.Source.Y, m.RoleID, m.Source.OwnerEpoch, m.Source.SessionEpoch); err != nil {
		return err
	}
	if err = saveCombat(ctx, tx, m.Source); err != nil {
		return err
	}
	if _, err = tx.ExecContext(ctx, "UPDATE migrations SET status='ABORTED' WHERE role_id=? AND request_id=?", m.RoleID, m.RequestID); err != nil {
		return err
	}
	if err = tx.Commit(); err != nil {
		return err
	}
	m.Status = "ABORTED"
	return nil
}

func (s *Server) finishCommitted(ctx context.Context, m *migration) error {
	r, err := s.role(ctx, m.RoleID)
	if err != nil {
		return err
	}
	if r.Ref() != m.Target.Ref() || r.SceneID != m.Target.SceneID {
		return unavailable("已提交迁移的目标归属校验失败")
	}
	// 重启后的目标可能没有准备对象，重做 prepare 必须幂等且不能覆盖已活跃角色。
	var result roleResult
	if err = s.callScene(ctx, m.Target.SceneID, "prepare", map[string]any{"role": m.Target, "transfer_id": m.TransferID}, &result); err != nil {
		return unavailable("迁移已提交，等待目标场景准备恢复")
	}
	if !validSnapshot(result.Role, m.Target) {
		return unavailable("目标场景准备快照无效")
	}
	if err = s.callScene(ctx, m.Target.SceneID, "activate", transferRef{m.Target.Ref(), m.TransferID}, &result); err != nil {
		return unavailable("迁移已提交，等待目标场景激活；请重试相同请求")
	}
	if !validSnapshot(result.Role, m.Target) {
		return unavailable("目标场景激活快照无效")
	}
	if err = s.callScene(ctx, m.Source.SceneID, "remove", transferRef{m.Source.Ref(), m.TransferID}, nil); err != nil {
		return unavailable("目标已激活，等待源场景清理；请稍后重试")
	}
	// 完成标记与目标最新快照一起提交，避免恢复重试把目标实时战斗进度回退。
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err = tx.ExecContext(ctx, "UPDATE roles SET x=?,y=? WHERE role_id=? AND owner_epoch=? AND session_epoch=?", result.Role.X, result.Role.Y, m.RoleID, m.Target.OwnerEpoch, m.Target.SessionEpoch); err != nil {
		return err
	}
	if err = saveCombat(ctx, tx, result.Role); err != nil {
		return err
	}
	if _, err = tx.ExecContext(ctx, "UPDATE migrations SET status='DONE' WHERE role_id=? AND request_id=?", m.RoleID, m.RequestID); err != nil {
		return err
	}
	if err = tx.Commit(); err != nil {
		return err
	}
	m.Status = "DONE"
	return nil
}

func (s *Server) recoverMigration(ctx context.Context, m *migration) error {
	switch m.Status {
	case "PREPARING", "ABORTING":
		return s.abortMigration(ctx, m)
	case "COMMITTED":
		return s.finishCommitted(ctx, m)
	case "DONE", "ABORTED":
		return nil
	default:
		return unavailable("未知迁移状态，角色保持暂停")
	}
}

func (s *Server) recoverRole(ctx context.Context, id string) error {
	m, err := s.pending(ctx, id)
	if errors.Is(err, sql.ErrNoRows) {
		return nil
	}
	if err != nil {
		return err
	}
	return s.recoverMigration(ctx, &m)
}

// Recover 在启动时尽力恢复；未能完成的角色会在 login/session/transfer 时再次恢复。
func (s *Server) Recover(ctx context.Context) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.QueryContext(ctx, "SELECT "+migrationColumns+" FROM migrations WHERE status NOT IN ('DONE','ABORTED')")
	if err != nil {
		return err
	}
	var migrations []migration
	for rows.Next() {
		m, e := scanMigration(rows)
		if e != nil {
			rows.Close()
			return e
		}
		migrations = append(migrations, m)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return err
	}
	var failures []error
	for i := range migrations {
		if e := s.recoverMigration(ctx, &migrations[i]); e != nil {
			failures = append(failures, e)
		}
	}
	return errors.Join(failures...)
}
