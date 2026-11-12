# 《万象法则》核心战斗 v1：客户端联调文档

版本：`webgl-combat-v1`；规则版本：`projectile-wall-v1`；日期：2026-11-12（Asia/Shanghai）。本文为最终服务端接口，供客户端另行适配；本仓库没有修改 Unity 项目或发起 Unity 云构建。

## 1. 接入与兼容

`203.0.113.10` 为占位 IP，交付时替换为实际游戏主机公网 IP。

| 项目 | 值 |
| --- | --- |
| 战斗 WSS | `wss://203.0.113.10/combat/ws` |
| 战斗入口健康检查 | `https://203.0.113.10/combat/healthz` |
| 场景 ID | `scene-combat` |
| 协议探测 | `ping` 的 `data.protocol_version` 为 `webgl-combat-v1` |
| 网页 Origin | 同机 IP 的 HTTPS 来源；`https://gate.draven.world`、`https://cloud.unity.com`、`https://developer.unity3dusercontent.com`，精确匹配 |
| 原移动入口 | `/ws`，协议继续为 `webgl-movement-v1` |

保持受信任 TLS 校验。Origin 取实际运行游戏 iframe 的请求头，不取 Unity Dashboard 地址栏。每条 WebSocket 文本消息是一个 JSON 对象，UTF-8 上限 65,535 字节；不传 URL 查询参数、二进制帧或 URL token。

继续使用 `register` / `login` / `view` / `ping` / `move`。登录战斗入口时服务端将既有角色迁入战斗场景，角色 ID、存档、血蓝及冷却保留；不重复注册。一个角色只允许一个有效登录会话；两窗口联调用两个独立账号，不能共用 token。返回移动入口会迁回移动场景，不刷新血蓝或冷却。

请求仍带字符串 `id` / `op`，响应原样关联，不依赖响应顺序；异步 `state` 没有 `id`。普通移动参数、方向坐标、会话代次和 AOI 规则继承 [移动协议](WEBGL_MOVEMENT_HANDOFF.md)。

## 2. 第一版规则

服务器以 20Hz 推进模拟；早期版本约 5Hz 推送，`skill-lab-20261112-v1` 的游戏主机配置通过 `COMBAT_STATE_HZ=20` 提高到 20Hz。世界坐标范围 `[-100,100]`，`+x` 向右、`+y` 向上，移动速度 6 单位/秒，输入失效 250ms，AOI 圆半径 30。角色碰撞半径 0.45，怪物 0.6。角色之间暂不阻挡移动；墙体阻挡所有角色和怪物。

| 技能 | `skill_id` | 资源与行为 |
| --- | --- | --- |
| 奥术弹 | `arc_bolt` | 消耗 10 MP，冷却 1,200ms，速度 18 单位/秒，半径 0.25，最大飞行距离 24；首个目标承受 28 伤害，弹道随即消失 |
| 石墙 | `stone_wall` | 消耗 20 MP，冷却 8,000ms，施法距离 ≤8，墙长 6、厚 0.6，持续 6,000ms；阻挡移动、怪物近战视线和所有弹道 |

弹道不追踪、不穿透，不命中施法者；其他在线存活玩家均可受伤，无队伍或阵营豁免。怪物也可被命中。服务器比较整段飞行路径的首次交点，防止高速穿墙；同距离优先墙体，其他平局按稳定目标 ID 顺序处理。角色、怪物与墙的碰撞使用扩张矩形近似，墙角也按该保守边界阻挡。

玩家初始 180 HP、100 MP；存活时每秒恢复 6 MP。HP 为 0 时不能移动或施法，5 秒后可以主动 `revive`，回到 `(0,0)`、恢复血蓝，已有冷却保留。石墙不能覆盖复活点、在线存活角色、存活怪物或其他墙，也不能越出地图；墙体没有 HP，暂不支持被攻击摧毁。

全场最多 32 条弹道、8 面墙；每人最多 4 条弹道、2 面墙。AOI 仅影响显示，不阻止视野外的真实碰撞。施法者死亡、断线、租约失效、更换会话或离开场景时，自己的弹道与墙被清理。场景重启也清空临时对象，HP/MP/冷却继续按持久快照恢复。

上述两个新技能的数值在本版本固定，`ability_rules_version` 表示这套规则；现有四技能数值热更接口不调整它们。

## 3. 连接与初始化

```json
{"id":"1","op":"ping"}
{"id":"1","op":"ping","ok":true,"data":{"unix_ms":1790000000000,"protocol_version":"webgl-combat-v1"}}
{"id":"2","op":"login","token":"example-token-not-a-real-credential"}
{"id":"3","op":"view"}
```

登录响应的角色增加 `combat`。不要根据登录响应单独生成世界；第一次 `view` 或 `state` 提供 `world_epoch`、技能目录和完整对象集合。`skills` 当前只有这两个新技能；`combat.cooldowns` 为了存档兼容还保留四个旧技能键，客户端只按 `skills` 渲染技能栏。

## 4. 施放弹道

```json
{"id":"cast-1","op":"cast","seq":1,"skill_id":"arc_bolt","dx":1,"dy":0}
```

`dx/dy` 必须都提供，为 `[-1,1]` 内有限数值，向量长度至少 `0.000001`，服务端再次归一化。起点取可信角色当前坐标，不接受客户端指定起点、速度、伤害、命中对象或模拟时间。不要传 `target_id`。客户端将鼠标位置投影到服务器使用的 XY 平面，减去本机角色位置后先归一化，再发送方向。

成功响应的 `data` 核心字段如下，另含当前完整 `role`、`tick`、`world_epoch`、`ability_rules_version` 与旧战斗配置标识 `config_version`：

```json
{"accepted":true,"seq":1,"skill_id":"arc_bolt","projectile":{"id":"p-world-1","owner_id":"role-a","skill_id":"arc_bolt","x":0,"y":0,"dx":1,"dy":0,"speed":18,"radius":0.25,"remaining_range":24}}
```

回执只表示服务器生成了弹道，不表示已经命中。实际扣血发生在后续模拟 Tick，通过权威角色/怪物快照与事件显示。不要用回执在客户端预先扣血。

## 5. 生成墙体

```json
{"id":"cast-2","op":"cast","seq":2,"skill_id":"stone_wall","x":3,"y":0,"dx":1,"dy":0}
```

`x/y` 是墙中心，`dx/dy` 是墙面法线方向，要求同弹道方向；服务器归一化后回传 `nx/ny`。墙体长边方向为 `(-ny,nx)`。上例生成中心 `(3,0)`、沿 Y 轴延伸的竖墙。客户端可先显示放置预览，但是否生成、最终位置和尺寸以回执为准。

```json
{"accepted":true,"seq":2,"skill_id":"stone_wall","wall":{"id":"w-world-2","owner_id":"role-a","skill_id":"stone_wall","x":3,"y":0,"nx":1,"ny":0,"length":6,"thickness":0.6,"remaining_ms":6000}}
```

服务端移动被墙阻挡时，移动请求仍可能 `accepted:true`，权威位置停止在碰撞边界；回执表示接收了方向，不保证实际位移。当前撞墙时停止该段移动，不做自动寻路或贴墙滑动。

## 6. 序号、重试与业务拒绝

移动 `seq` 与战斗动作 `seq` 使用两套独立计数器。`cast` 和 `revive` 共用战斗计数器，在当前新登录会话中从 1 递增，客户端范围为 `1..9007199254740991`。不要让两个技能各自从 1 开始。

服务端仅缓存最近一次成功动作的结果。同一会话重发最近成功的相同 `seq`、相同技能与参数时，返回原结果，不重复消耗或生成对象。更老序号或同序号改参数返回 `accepted:false`。重试必须保持原参数；新操作使用新序号。拒绝不推进成功动作计数，但客户端仍可为下一操作递增。一次成功动作回执丢失且进程已重启时，不能靠该内存缓存保证跨重启的请求去重；客户端应重新登录并读取快照，不自动补放旧技能。

重试回执里的 `role/tick` 和生成对象可能是历史值。用它完成请求关联即可，不能覆盖更新的角色快照或重新生成已消失对象；同一世界、同一角色会话内用 `combat.revision` 比较战斗状态新旧，因为同一 Tick 内也可能发生多个动作。对象 ID 去重与权威完整快照共同决定显示状态。

战斗中的正常业务拒绝使用 **`ok:true` + `data.accepted:false`**，避免沿用旧客户端的 `ok:false` 断线策略：

```json
{"id":"cast-3","op":"cast","ok":true,"data":{"accepted":false,"seq":3,"skill_id":"arc_bolt","reason":"cooldown","retry_after_ms":850}}
```

实际拒绝还可能附带当前 `role`、`tick` 和 `world_epoch`。客户端显示提示并保持连接，不能把它当作施法成功或播放扣蓝动画。

| `reason` | 客户端处理 |
| --- | --- |
| `cooldown` | 按权威冷却更新 UI，等待后允许新的操作 |
| `not_enough_mana` | 提示法力不足 |
| `out_of_range` / `out_of_bounds` | 调整墙位置 |
| `occupied` / `wall_overlap` / `spawn_blocked` | 放置区域不可用 |
| `object_limit` | 等待已有对象结束 |
| `dead` | 停止移动与施法输入，等待复活 |
| `stale_action_seq` | 对照当前会话与 `last_action_seq` 更新计数，不重放旧动作 |
| `not_dead` / `revive_not_ready` | 保持连接，更新复活按钮或倒计时 |

格式错误、未知技能、未登录、身份过期和服务不可用继续是 `ok:false`。不得将这些错误全部改成忽略，否则会隐藏失效会话或协议不一致。

复活请求：`{"id":"revive-1","op":"revive","seq":4}`。死亡移动拒绝也使用 `ok:true,data.accepted:false,reason:"dead"`。

## 7. 完整状态与事件

`view` 回应和异步 `state.data` 使用同一结构：

```json
{
  "self":{"role_id":"role-a","name":"A","scene_id":"scene-combat","owner_epoch":2,"session_epoch":3,"x":0,"y":0,"coins":100,"combat":{"hp":180,"max_hp":180,"mp":70,"max_mp":100,"dead":false,"revive_remaining_ms":0,"revision":10,"last_action_seq":2,"action_session":3,"cooldowns":{"strike":0,"whirlwind":0,"charge":0,"mend":0,"arc_bolt":900,"stone_wall":8000}}},
  "players":[],
  "mobs":[],
  "skills":[],
  "projectiles":[],
  "walls":[],
  "events":[],
  "tick":100,
  "scene_id":"scene-combat",
  "world_epoch":"boot-identity",
  "ability_rules_version":"projectile-wall-v1",
  "pvp_enabled":true,
  "event_cursor":25,
  "players_truncated":false,
  "visible_player_count":1,
  "projectiles_truncated":false,
  "events_truncated":false
}
```

示例数组为结构占位，实际 `players` 包含自己，`skills` 为上述两个技能目录。其他玩家 `combat` 是摘要，不含完整冷却；完整冷却只在 `self.combat`。`mobs` 包含 `id/name/kind/x/y/hp/max_hp/alive/respawn_remaining_ms/target_id/config_version`，由服务端怪物 AI 更新。

客户端维护角色、怪物、弹道和墙四份 ID→显示对象映射。普通快照里缺失对象即移除；`projectiles_truncated:true` 时缺失不一定代表真实消失，应结合上次对象的剩余距离或后续完整快照清理。墙体集合在当前容量限制内完整发送。玩家裁剪沿用移动协议约定。

只有相同 `scene_id + world_epoch + self.owner_epoch + self.session_epoch` 下才比较 Tick。`world_epoch` 改变时清空临时对象、事件去重和 Tick 基线，再加载新快照，避免重启后误弃新世界。角色新会话同样重置本地移动序号和战斗序号。

事件公共字段为 `id/type/source_id/target_id/skill_id/value/x/y/tick`；技能对象事件另有 `object_id`：

| 类型 | 用途 |
| --- | --- |
| `projectile_spawn` | 附 `projectile` 对象，可立即显示其他玩家弹道 |
| `projectile_hit` / `projectile_blocked` | 命中目标 / 被墙拦截，播放一次效果并移除该弹道 |
| `projectile_expire` | 达到射程、边界或被清理，移除对象 |
| `wall_spawn` | 附 `wall` 对象，显示墙体 |
| `wall_expire` | 过期或归属失效，移除墙体 |
| `damage` / `death` / `revive` | 飘字、死亡和复活表现；真实 HP 仍读快照 |

显式 `view` 返回最近最多 32 条事件窗口；新版本同一可靠连接的后续 `state` 只带尚未下发的事件，避免重复传输历史对象。按同一世界内的 `id` 去重；这不是可靠消息队列，可能因 AOI 或帧预算遗漏。`event_cursor` 是全场最新事件 ID，不能把所有 ID 间隙都视为丢包。`events_truncated:true` 表示此次因字节预算裁剪；不补算伤害，不因缺事件保持过期碰撞体。短弹道可能在两份快照之间生成并命中，此时通过事件播放短暂命中效果，不强行保留已经结束的飞行物。

新 Gateway 的状态查询和存档不再占住施法处理循环。角色状态后台尽快保存，合并未开始的旧快照，正常断开时排空最终状态；施法回执不表示数据库事务已完成，崩溃可能丢失未写入的最近变化。自定义 ID/完整参数初始化使用独立 [参数化技能协议](SKILL_LAB_HANDOFF.md)，旧入口的两个示例技能保持兼容。

## 8. 建议客户端第一轮适配

1. 增加战斗连接模式，地址使用 `/combat/ws`，先确认 ping 协议为 `webgl-combat-v1`。
2. 复用现有角色、移动、会话和 AOI 显示；增加 HP/MP、两格技能栏和权威冷却。
3. 实现鼠标瞄准方向、墙体中心/朝向预览及 `cast`；合理限频，继续遵守全连接 40 请求/秒限制。
4. 识别 `ok:true,accepted:false`，提示失败并保持连接；死亡时停发方向，5 秒后允许复活。
5. 实现弹道和墙体对象、插值与事件去重；最终位置、碰撞、血量和资源始终按服务器纠正。

可以建议左键或 Q 发弹道、E 放墙、R 复活，但按键属于客户端选择，不是网络协议要求。客户端若使用 Unity XZ 地面，应明确映射服务器 `(x,y)` 到 Unity `(x,0,z)`；不要把服务器 `dy` 当作重力方向。

## 9. 双客户端验收清单

两账号同进战斗场景，B 移到 A 前方约 6 单位：

1. A 发弹道：先见飞行，再由 B 的 HP 从 180 降到 152；一次只伤害一个目标。
2. A 连按技能：冷却拒绝不会断线、重复扣蓝或生成额外弹道。复发相同成功动作不重复伤害。
3. A 在两人间放墙：墙生成后，两端看到相同位置和方向；双方移动不能穿过，弹道命中墙后消失，B 不扣血。
4. 测试斜向墙、贴墙、超距离、压住玩家或重叠墙；拒绝时不扣蓝、不开始新冷却。
5. 等约 6 秒，墙自然消失，可以通过；施法者断线或重登也清理自己的临时对象，但保留冷却。
6. 向怪物发弹道：怪物受伤后追击反击，墙能阻挡追击路径与近战视线。
7. 连续命中玩家直到死亡；死亡停止移动/施法，5 秒后主动复活，血蓝恢复且旧冷却保留。
8. 重连、新登录、场景重启后，不显示旧世界弹道、不重播历史事件；两个客户端的最终 HP、墙体与位置一致。

服务端检查与 Unity 检查分别记录：源码/协议自动验收不代表 Unity 编译、WebGL 发布或真实鼠标键盘已经验收。具体已执行记录见 [VALIDATION.md](VALIDATION.md)。
