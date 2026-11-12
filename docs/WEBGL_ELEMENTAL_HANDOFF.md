# 元素试炼场：服务端接入交付 v1

协议扩展能力：`elemental_arena_v1`；入口协议标识：`webgl-elemental-v1`。日期：2026-11-12（Asia/Shanghai）。实现依据客户端 [AXIOMANCY `45cde240`](https://github.com/dravenlove/AXIOMANCY/tree/45cde2405bfbed3b2bd814518775159d43f5f636) 的元素需求、`CombatSimulation.cs` 与 `LocalElementalAuthority.cs`。本次只实现服务端；没有修改 Unity 项目或启动云构建。

交付镜像 `elemental-20261112-v1`、运行源码 `12ed3a8aaf44bd93093bd014b036327bc9f343de` 已发布并部署。两账号公网协议、实际客户端解析器及原移动/弹道入口回归均通过；浏览器真实画面和输入操作由客户端侧继续验收。具体证据与数据保留检查见 [VALIDATION.md](VALIDATION.md)。

## 入口与能力

下列 IP 为占位地址，交付时填写实际游戏主机 IP。

| 用途 | 地址 / 值 |
| --- | --- |
| 元素 WSS | `wss://203.0.113.10/elemental/ws` |
| 健康检查 | `https://203.0.113.10/elemental/healthz` |
| 自由区 | `scene-elemental-world` |
| 个人试炼场 | `scene-elemental-arena` |
| ping | `data.protocol_version:"webgl-elemental-v1"` |
| 精确网页 Origin | 同机 HTTPS IP、`https://gate.draven.world`、`https://cloud.unity.com`、`https://developer.unity3dusercontent.com` |

每个 `view.data` 与 `state.data` 都声明 `features:["elemental_arena_v1"]`，自由区也持续声明。进入后追加 `battle`；退出后不发送 `battle`。客户端按能力显示进入按钮，按实际快照填写场景和归属，不硬编码客户端本地演示的场景名。

一帧一个 UTF-8 JSON 对象，最大 65,535 字节；继续校验证书，不在连接 URL 中传 token 或查询参数。`register/login/view/ping/move` 的基础约定沿用 [移动协议](WEBGL_MOVEMENT_HANDOFF.md)。角色仍复用原账号，一账号只有一个有效会话；双账号验收使用两个不同凭据。

原 `/ws` 移动入口与 `/combat/ws` 弹道/墙体入口继续运行。该入口的试炼内容是**单人 13×9、六敌人**，每个角色各有一份实例；两账号即使看到相同场景 ID，也不共享地形、生命、敌人或效果。

## 消息与归属

```json
{"id":"1","op":"enter_arena","scene_id":"scene-elemental-world","owner_epoch":10}
{"id":"2","op":"cast","scene_id":"scene-elemental-arena","owner_epoch":11,"element":"lightning","x":4,"y":4}
{"id":"3","op":"restart_battle","scene_id":"scene-elemental-arena","owner_epoch":11}
{"id":"4","op":"leave_arena","scene_id":"scene-elemental-arena","owner_epoch":12}
```

以上是独立示例，归属从最近快照读取。四个操作都要求 `scene_id` 与正整数 `owner_epoch`；施法不需要旧弹道协议的 `seq/skill_id/target_id/dx/dy`。`element` 为 `water/fire/lightning`，目标 `x/y` 为整数格，分别在 `[0,12]`、`[0,8]`。服务器从已登录连接取得角色和登录会话，客户端不能指定伤害、生命、角色 ID 或最终角色位置。

```json
{"id":"2","op":"cast","ok":true,"data":{"accepted":true,"reason":""}}
{"id":"2","op":"cast","ok":true,"data":{"accepted":false,"reason":"cooldown"}}
```

正常拒绝使用 `ok:true` 和布尔 `accepted:false`；客户端保持连接。原因包括 `cooldown/out_of_range/outside_arena/unknown_element/battle_finished/scene_changed`；重复进入返回 `already_in_arena`，同 ID 改施法参数返回 `request_conflict`。格式、认证或内部服务故障使用 `ok:false,error`，由客户端按基础会话错误处理。

每次进入、退出、重开都通过 Backend 持久迁移事务取得更大的实际 `owner_epoch`；`session_epoch` 保持不变。同一连接的移动 `seq` 连续递增，不因重开归零。进入/重开会停止旧方向，已排队的旧归属移动和战斗请求不会应用到新实例。成功确认不替代快照：客户端等待新的完整状态后更新生命、地图和位置。

最近 64 个施法请求 ID 在当前实例/会话内去重；同 ID 相同参数返回原确认，不重复反应。重开后旧归属先被拒绝，不能借重试打到新战斗。此缓存不是跨进程的可靠操作日志，网络故障重连后应读快照再操作。

## 快照与坐标

完整状态保留 `self/players/scene_id/tick/players_truncated/visible_player_count`，每份快照重复能力声明。角色列表只包含玩家；试炼场只显示自己，六敌人始终放在 `battle.enemies`，包含死亡敌人。

```json
{
  "features":["elemental_arena_v1"],
  "scene_id":"scene-elemental-arena",
  "tick":100,
  "self":{"role_id":"role-a","name":"A","scene_id":"scene-elemental-arena","owner_epoch":11,"session_epoch":3,"x":1.5,"y":4.5,"coins":100,"combat":{"dead":false}},
  "players":[{"role_id":"role-a","name":"A","scene_id":"scene-elemental-arena","owner_epoch":11,"session_epoch":3,"x":1.5,"y":4.5,"coins":100,"combat":{"dead":false}}],
  "players_truncated":false,
  "visible_player_count":1,
  "battle":{"version":1,"battle_id":"opaque-battle-id","width":13,"height":9,"phase":"playing","player":{"id":"role-a","x":1.5,"y":4.5,"hp":100.0,"max_hp":100.0},"enemies":[],"tiles":[],"cooldown":0.0,"range":7.0,"elapsed":0.0,"living_enemies":6,"conduction":0,"ignitions":0,"steam":0,"last_chain":0,"message":"Prepare the ground. Defeat all six pursuers.","event_sequence":0,"effects":[]}
}
```

示例中的 `enemies/tiles` 为空仅用于压缩展示结构；实际始终为 **6 个敌人、117 个格子**，不是可省略数组。`battle.player.id/x/y` 与公开 `self` 完全一致，死亡使用 `self.combat.dead`；元素试炼血量只在 `battle` 中发布，不混用旧技能栏的完整战斗结构。

- 坐标 X 向右、Y 向上，格中心 `(x+0.5,y+0.5)`；`tiles[y*13+x]` 从左下格开始。角色移动坐标与施法整数格分开处理。
- `phase` 为 `playing/won/lost`；胜利要求玩家存活、敌人数为零，玩家死亡时失败优先。
- 全局场景 Tick 在当前进程内持续递增。进入、退出与重开改变归属；重开生成新 `battle_id`，清空效果序列和反应计数。
- 效果最多 64 条，`id` 严格递增且不超过 `event_sequence`，字段为 `id/type/actor_id/element/x/y/amount`。类型为 `cast/ignite/steam/conduction/damage/death/win/loss`。
- 按 `battle_id + effect.id` 去重，首次接收该战斗时跳过历史效果；生命和地形直接取完整快照，不靠事件累减。
- 结束后继续约 5Hz 发终局快照，模拟规则、生命、地形和战斗 elapsed 冻结；可以退出或重开。

## 已实现的参考规则

20Hz 固定推进。玩家 100 HP、移速 3.2；六敌人各 56 HP、移速 1.05，距离 0.65 内每 0.9 秒近战 8 伤害。施法距离 7，共享冷却 0.28 秒。自由区继续使用 6 单位/秒、250ms 输入失效和原 AOI。

| 元素 / 地形 | 结算 |
| --- | --- |
| 水 | 五格十字，边界裁剪；湿地 9 秒并灭火，灭火产生 3.5 秒蒸汽 |
| 火遇湿地 | 去除湿地、停止燃烧并生成蒸汽；该格不结算干地直接伤害 |
| 火遇干地 | 对格内敌人即时 16 伤害；草地可燃烧 4 秒，每 0.6 秒向四邻草地扩散，湿格不蔓延 |
| 燃烧地面 | 每秒 14 伤害，可伤玩家与敌人；自然烧尽变焦地，水灭火保留原地形 |
| 蒸汽 | 敌人在其中降到 40% 速度 |
| 干地雷 | 格内玩家与敌人受 28 伤害，充电显示 0.24 秒 |
| 湿地雷 | 沿四邻湿地连通格导电，每个相关角色只受一次 32 伤害，充电显示 0.4 秒 |

干地火的即时伤害只针对敌人；玩家可受燃烧地面和雷伤害。这与参考核心实际行为一致。角色、地形、冷却、敌人 AI、胜负全部由服务端计算，远程客户端不运行本地结算。

## 数据与断线边界

进入时冻结并保存权威自由区坐标，退出恢复。试炼的 100 HP、敌人、地形与效果是独立临时状态，不写回原角色 180 HP 系统的战斗快照，不刷新其法力/冷却，也不修改金币。

普通断线停止方向、隐藏角色并暂停该试炼；同进程重登可以恢复原实例，新登录仍增加 `session_epoch`。场景进程重启后临时试炼重建为新 `battle_id`，保留账号、持久归属和自由区返回位置；本版不承诺跨进程恢复战斗进度，也没有试炼掉落或通关奖励。

正常进入/退出/重开共用同一个 C++ 进程，Backend 事务提交归属后才替换候选实例。迁移中断按原持久迁移日志恢复，冻结源在提交前保留。旧场景未声明归属轮换能力时，Backend 在冻结前拒绝重开请求。

## 客户端联调清单

1. 使用新入口登录，检查每个完整视图都有能力，再显示进入试炼按钮。
2. 在自由区移动后进入；确认新归属、同一登录会话、玩家 `(1.5,4.5)`、六敌人与 117 格。
3. 试 `lightning(4,4)`：初始湿地连通格为 6，两个走廊敌人各受到一次 32 伤害。重复请求不重复结算。
4. 验证水火蒸汽、草地燃烧/蔓延、水灭火和射程/冷却拒绝；角色实际血量只读快照。
5. 在自身格连续落雷触发失败；失败后方向输入不能改变试炼位置，终局快照持续到达。
6. 重开后 `owner_epoch` 增大、登录会话不变、`battle_id` 改变、事件从零重建；旧上下文施法得到 `scene_changed`。
7. 离开后无 `battle`，自由区位置恢复；继续发送更大的移动 `seq`，不归零。
8. 两个账号分别进入，确认战斗 ID、反应、血量和敌人互不影响；验证断线、新会话与场景重启边界。

客户端真实操作、Unity 编译、WebGL 云构建和服务端协议验收分别记录。当前服务端证据见 [VALIDATION.md](VALIDATION.md)，部署步骤见 [ELEMENTAL_DEPLOYMENT.md](ELEMENTAL_DEPLOYMENT.md)。
