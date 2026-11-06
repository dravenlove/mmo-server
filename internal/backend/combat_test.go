package backend

import (
	"context"
	"encoding/json"
	"testing"
)

func testCombat(hp, revision, session int64) json.RawMessage {
	data, _ := json.Marshal(map[string]any{
		"hp": hp, "max_hp": 180, "mp": 37, "max_mp": 100, "dead": hp == 0,
		"revive_remaining_ms": 0, "revision": revision, "last_action_seq": 4, "action_session": session,
		"cooldowns": map[string]int64{"strike": 500, "whirlwind": 2500, "charge": 6000, "mend": 11000},
	})
	return data
}

func setCombat(f *fakeScene, id string, state json.RawMessage) {
	f.mu.Lock()
	defer f.mu.Unlock()
	p := f.players[id]
	p.Role.Combat = append(json.RawMessage(nil), state...)
	f.players[id] = p
}

func assertCombat(t *testing.T, state json.RawMessage, hp, revision int64) {
	t.Helper()
	var parsed struct {
		HP, Revision int64
		Cooldowns    map[string]int64
	}
	if err := json.Unmarshal(state, &parsed); err != nil {
		t.Fatal(err)
	}
	if parsed.HP != hp || parsed.Revision != revision || parsed.Cooldowns["mend"] != 11000 {
		t.Fatalf("战斗快照不匹配: %s", state)
	}
}

func TestCombatCheckpointDurabilityAndFence(t *testing.T) {
	f := newFixture(t)
	_, route := registerLogin(t, f, "combat-checkpoint")
	role := route.Role
	role.X = 4
	role.Combat = testCombat(71, 8, role.SessionEpoch)
	wantPost(t, f.s, "checkpoint", roleResult{role}, 200, nil)
	// 重复写入合法；旧revision同时不能回退位置或战斗状态。
	wantPost(t, f.s, "checkpoint", roleResult{role}, 200, nil)
	older := role
	older.X = 99
	older.Combat = testCombat(180, 7, role.SessionEpoch)
	wantPost(t, f.s, "checkpoint", roleResult{older}, 409, nil)
	stored, err := f.s.role(context.Background(), role.RoleID)
	if err != nil {
		t.Fatal(err)
	}
	if stored.X != 4 {
		t.Fatal("旧战斗快照回退了位置")
	}
	assertCombat(t, stored.Combat, 71, 8)
	stale := role
	stale.SessionEpoch++
	wantPost(t, f.s, "checkpoint", roleResult{stale}, 409, nil)
	invalid := role
	invalid.Combat = json.RawMessage(`{"hp":999999999}`)
	wantPost(t, f.s, "checkpoint", roleResult{invalid}, 400, nil)
	f.restart(t)
	stored, err = f.s.role(context.Background(), role.RoleID)
	if err != nil {
		t.Fatal(err)
	}
	assertCombat(t, stored.Combat, 71, 8)
}

func TestCombatLoginKeepsLiveSceneSnapshot(t *testing.T) {
	f := newFixture(t)
	token, route := registerLogin(t, f, "combat-live-login")
	checkpoint := route.Role
	checkpoint.Combat = testCombat(150, 2, 1)
	wantPost(t, f.s, "checkpoint", roleResult{checkpoint}, 200, nil)
	setCombat(f.source, route.Role.RoleID, testCombat(41, 9, 1))
	var relogged Route
	wantPost(t, f.s, "login", map[string]string{"token": token}, 200, &relogged)
	if relogged.Role.SessionEpoch != 2 {
		t.Fatal("新登录没有更新会话")
	}
	assertCombat(t, relogged.Role.Combat, 41, 9)
	stored, err := f.s.role(context.Background(), route.Role.RoleID)
	if err != nil {
		t.Fatal(err)
	}
	assertCombat(t, stored.Combat, 41, 9)
	// 旧会话即使携带更高revision，也不得恢复已经损失的生命。
	checkpoint.Combat = testCombat(180, 999, 1)
	wantPost(t, f.s, "checkpoint", roleResult{checkpoint}, 409, nil)
}

func TestCombatMigrationPreservesFreezeAndRecovery(t *testing.T) {
	f := newFixture(t)
	token, route := registerLogin(t, f, "combat-transfer")
	checkpoint := route.Role
	checkpoint.Combat = testCombat(150, 2, 1)
	wantPost(t, f.s, "checkpoint", roleResult{checkpoint}, 200, nil)
	setCombat(f.source, route.Role.RoleID, testCombat(63, 7, 1))
	request := transferRequest{Ref: route.Role.Ref(), TargetSceneID: "scene-2", RequestID: "combat-transfer-1"}
	var transferred Route
	wantPost(t, f.s, "transfer", request, 200, &transferred)
	assertCombat(t, transferred.Role.Combat, 63, 7)
	player, ok := f.target.player(route.Role.RoleID)
	if !ok {
		t.Fatal("目标角色不存在")
	}
	assertCombat(t, player.Role.Combat, 63, 7)
	checkpoint.Combat = testCombat(180, 999, 1)
	wantPost(t, f.s, "checkpoint", roleResult{checkpoint}, 409, nil)
	f.restart(t)
	f.target.reset()
	var restored Route
	wantPost(t, f.s, "login", map[string]string{"token": token}, 200, &restored)
	assertCombat(t, restored.Role.Combat, 63, 7)
}

func TestCombatAbortedMigrationKeepsLatestSource(t *testing.T) {
	f := newFixture(t)
	_, route := registerLogin(t, f, "combat-abort")
	setCombat(f.source, route.Role.RoleID, testCombat(47, 12, 1))
	f.target.fail("prepare", false, 1)
	request := transferRequest{Ref: route.Role.Ref(), TargetSceneID: "scene-2", RequestID: "combat-abort-1"}
	wantPost(t, f.s, "transfer", request, 503, nil)
	stored, err := f.s.role(context.Background(), route.Role.RoleID)
	if err != nil {
		t.Fatal(err)
	}
	if stored.SceneID != "scene-1" {
		t.Fatal("已取消迁移改变了归属")
	}
	assertCombat(t, stored.Combat, 47, 12)
}

func TestCombatCommittedRecoveryDoesNotResetActiveTarget(t *testing.T) {
	f := newFixture(t)
	_, route := registerLogin(t, f, "combat-commit-retry")
	setCombat(f.source, route.Role.RoleID, testCombat(100, 6, 1))
	f.source.fail("remove", false, 1)
	request := transferRequest{Ref: route.Role.Ref(), TargetSceneID: "scene-2", RequestID: "combat-commit-1"}
	wantPost(t, f.s, "transfer", request, 503, nil)
	setCombat(f.target, route.Role.RoleID, testCombat(19, 11, 1))
	var recovered Route
	wantPost(t, f.s, "transfer", request, 200, &recovered)
	assertCombat(t, recovered.Role.Combat, 19, 11)
	wantPost(t, f.s, "transfer", request, 200, &recovered)
	assertCombat(t, recovered.Role.Combat, 19, 11)
}
