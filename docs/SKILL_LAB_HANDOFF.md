# 参数化多人技能：客户端联调协议 v1

日期：2026-11-12（Asia/Shanghai）。能力 `parameterized_skills_v1`，入口协议 `webgl-skill-lab-v1`。这是客户端提交完整技能定义、服务端注册并运行实例的联调模式；技能 ID 不再固定为两个示例名称。首版行为组件为 `projectile` 和 `wall`。

首次技能交付：`skill-lab-20261112-v1` 已发布并部署，源码 `0c48fbe15d6548fce6411591911b51f5311c68ce`。单客户端移动并施放、双账号观察同一实例、不同本机定义、墙体过期和参数/代次校验均通过公网协议验收。旧入口回归和数据核对见 [VALIDATION.md](VALIDATION.md)；Unity 页面需按本协议适配后另做画面验收。

2026-11-13 后续已增加账号密码登录：技能 Scene 与 bridge 保留上述版本，Gateway、Backend 和 Auth 分别使用账号发布版本。当前分组件镜像与验证时间见 [项目协作记忆](AGENT_MEMORY.md)，不能把首次发布标签当成整套服务的统一当前版本。

## 连接与职责

WSS：`wss://203.0.113.10/skills/ws`，健康检查：`https://203.0.113.10/skills/healthz`。IP 为占位地址。精确网页 Origin 继续使用现有允许列表；校验受信任 TLS。最大 UTF-8 消息 65,535 字节，单帧单个 JSON；客户端每秒最多 40 条请求。

沿用 `register/login/ping/view/move`，复用已有账号。`ping.data.protocol_version` 为 `webgl-skill-lab-v1`；场景 `scene-skills` 是共享多人场景。它不使用 13×9 单人元素试炼的地图和胜负逻辑。

客户端负责编辑定义、输入方向/目标、表现和资源选择；服务器负责校验定义、分配实例 ID、决定起点、冷却、资源消耗、推进飞行、碰撞、伤害、墙体阻挡和销毁。客户端不提交命中结果或最终生命。

旧 `/ws`、`/combat/ws`、`/elemental/ws` 保留。新协议需要客户端适配：旧战斗解析器把弹道和墙体 ID 固定为两个示例名称，不能直接解析新目录。此次只修改服务端，未构建 Unity 或验收浏览器画面。

## 1. 读取完整状态

登录后读取首份 `view/state`：

```json
{"world_epoch":"opaque-scene-process-id","scene_id":"scene-skills","features":["parameterized_skills_v1"],"ability_rules_version":"parameterized-skills-v1","skills":[],"skill_cooldowns":{},"projectiles":[],"walls":[],"event_cursor":0,"events":[]}
```

以上只展示扩展字段；实际保留完整 `self/players/tick/mobs` 等字段。首次没有注册技能时，`skills` 可以为空。坐标是世界 XY，X 向右、Y 向上，范围 `[-100,100]`；不使用整数格。

`world_epoch` 是场景进程代次，不得硬编码。每份完整状态持续包含本机技能目录及冷却，客户端按最新世界代次维护缓存。场景、会话或世界代次改变后，清理旧表现和待重放的动作。

## 2. 注册技能

`skill_init` 注册定义，不立即生成飞行物。技能 ID 为 1～32 字节 ASCII 字母、数字、`_ . -`；每角色最多 16 个。`name` 可省略，最多 64 UTF-8 字节。定义上限 2KiB，未知字段和越界值直接拒绝，不会静默替换为固定数值。

```json
{"id":"init-1","op":"skill_init","world_epoch":"从最新状态读取","skill_id":"my_rocket_07","expected_definition_version":0,"definition":{"type":"projectile","name":"测试弹道","damage":17,"mp_cost":3,"cooldown_ms":300,"range":30,"speed":12,"radius":0.15}}
```

```json
{"id":"init-1","op":"skill_init","ok":true,"data":{"accepted":true,"reason":"","unchanged":false,"definition":{"skill_id":"my_rocket_07","name":"测试弹道","type":"projectile","definition_version":1,"damage":17,"mp_cost":3,"cooldown_ms":300,"range":30,"speed":12,"radius":0.15}}}
```

以响应中的定义和版本为准。相同角色、相同 ID、相同参数重复初始化，返回 `unchanged:true` 和原版本，不刷新冷却。修改已有定义时，`expected_definition_version` 填当前版本；过期编辑返回 `definition_conflict`。仍有该技能实例或冷却时，不允许替换定义。

初始化用于首次接入、配置改变或定义丢失；不在每帧重复注册。日常施放只发送技能 ID、确认的版本和当前方向/目标。同一技能的初始化最多一个请求在途，确认后再进行下一次编辑。

墙体示例：

```json
{"id":"init-2","op":"skill_init","world_epoch":"从最新状态读取","skill_id":"my_barrier","expected_definition_version":0,"definition":{"type":"wall","damage":0,"mp_cost":5,"cooldown_ms":2000,"range":8,"length":4,"thickness":1,"duration_ms":5000}}
```

| 参数 | 范围 / 单位 |
| --- | --- |
| `type` | `projectile` 或 `wall` |
| `damage` | 整数 0～1000；墙体必须为 0 |
| `mp_cost` | 整数 0～100 |
| `cooldown_ms` | 整数 50～60000ms |
| `range` | 弹道最大飞行距离 0.5～50；墙体放置距离 0.5～20 |
| `speed` | 弹道 1～100 世界单位/秒 |
| `radius` | 弹道半径 0.05～2 世界单位 |
| `length` / `thickness` | 墙体长度 0.5～12、厚度 0.1～3 世界单位 |
| `duration_ms` | 墙体持续时间，整数 100～20000ms |

同 ID 在不同角色下可以有不同参数，互不覆盖。`type` 表示服务端实现的行为组件，任意技能 ID 都可以选择这两种组件。增加全新的行为算法需要增加对应服务端组件，调整已有组件数值不需要重新构建。

## 3. 施放并生成实际实例

```json
{"id":"cast-1","op":"cast","seq":1,"world_epoch":"从最新状态读取","skill_id":"my_rocket_07","definition_version":1,"dx":1,"dy":0}
```

`seq` 是当前登录会话的战斗动作序号，与移动序号分开。正整数、递增、上限 `9007199254740991`；重登从 1 开始，`cast/revive` 共用。重试成功动作时保持原序号和全部参数，不能换参数。缓存只保证当前进程/会话内最近成功动作去重，不承诺跨进程重放。

`world_epoch/definition_version` 都取服务器最新确认值。方向必须显式提供，为 `[-1,1]` 内有限数，非零，服务端再归一化。弹道起点取角色权威位置，不能传 `x/y` 伪造发射点。生成响应包含独立实例：

```json
{"accepted":true,"seq":1,"skill_id":"my_rocket_07","projectile":{"id":"p-opaque-world-1","owner_id":"role-a","skill_id":"my_rocket_07","definition_version":1,"x":0,"y":0,"dx":1,"dy":0,"speed":12,"radius":0.15,"remaining_range":30}}
```

响应另有权威 `role/tick/world_epoch` 等字段。`accepted:true` 表示已生成，不代表命中。后续 Tick 扫掠碰撞，首个玩家、怪物或墙体决定结果；弹道不穿透、不伤施法者。伤害按注册值结算，再通过生命快照与事件显示。

本机表现可在首个有效成功回执到达时显示这个已由服务器确认的实例，不必再等下一份 `state`。按 `world_epoch + id` 与后续快照/事件去重；旧世界、旧 Tick 或历史重试回执不能复活已经结束的对象。此处只提前显示已确认实例，不提前扣血或判定命中。

墙体施放增加中心坐标：

```json
{"id":"cast-2","op":"cast","seq":2,"world_epoch":"从最新状态读取","skill_id":"my_barrier","definition_version":2,"x":3,"y":0,"dx":1,"dy":0}
```

方向表示墙面法线，长边为 `(-dy,dx)`。响应的 `wall` 带 `id/owner_id/skill_id/definition_version/x/y/nx/ny/length/thickness/remaining_ms`，均由服务端生成。拒绝覆盖玩家、怪物、复活点、已有墙或地图边界。墙体按真实注册尺寸阻挡角色与弹道。

**观察者必须按实例字段绘制。** 他人的 `skill_id` 不一定在本机目录中；即使 ID 同名，也不能拿自己的速度、半径、墙尺寸或持续时间重建别人的实例。实例由服务端 `id` 唯一标识，以 `world_epoch + id` 管理生命周期。

## 4. 状态、事件与冷却

`skills` 是本角色注册的完整定义数组，项目使用 `skill_id`、`type`、`definition_version`；`skill_cooldowns` 是技能 ID 到剩余毫秒的完整映射。不要从旧 `self.combat.cooldowns` 推断新技能，它保留旧存档的六个兼容键。`self.combat` 的 HP/MP/死亡/修订号仍是权威角色状态。

`projectiles/walls/players/mobs` 是 AOI 内完整集合；根据每份完整集合移除缺失对象，不能只靠销毁事件。默认 20Hz 模拟和 20Hz 下发；网络拥堵时不追发历史状态。

`events` 是表现事件。首次完整视图可含近期窗口；同一可靠连接后续 `state` 只带尚未下发的事件。客户端按 `world_epoch + event.id` 去重，`event_cursor` 是全局事件游标，不能把 ID 空缺推断为消息丢失。显式 `view` 可以重取近期窗口，正常运行消费 `state`，避免额外高频轮询。`projectile_spawn/wall_spawn` 带实际对象数据；伤害和销毁继续按权威快照确认。

全场最多 32 条弹道、8 面墙；每角色最多 4 条弹道、2 面墙。死亡、断线、迁出或更换登录会话会清理本人实例。普通重登后，同一进程里的本角色定义和自定义冷却保留；场景进程重启或角色迁出后，它们不作为数据库存档恢复，需要重新初始化。HP/MP 和原存档继续保留。

## 5. 拒绝与持久化边界

正常拒绝使用 `ok:true,data.accepted:false,reason`。初始化原因包括 `world_changed/invalid_skill_id/invalid_definition/invalid_parameter/unknown_parameter/unknown_behavior/definition_conflict/cooldown_active/skill_in_use/skill_limit`；数值不合法时可附 `field` 指明字段。施放原因包括 `world_changed/skill_not_initialized/definition_changed/cooldown/not_enough_mana/dead/object_limit/out_of_range/out_of_bounds/occupied/wall_overlap/spawn_blocked/stale_action_seq`。

格式、鉴权和服务故障仍使用 `ok:false,error`。初始化或施放普通拒绝不应关闭连接。`world_changed` 后读新状态并初始化；`definition_changed/conflict` 后刷新当前目录，不能自动补放旧施法。

施法不等待 MySQL 保存完成才回执。后台按连接串行保存，合并尚未开始的旧快照；正常断开时排空最终快照。进程或主机突然崩溃仍可能丢失尚未写入的最近状态，不能将此演示当作付费交易或跨进程可靠战斗日志。

## 6. 联调顺序

1. `ping/login`，确认新协议、能力、世界代次和空目录。
2. 初始化两个任意 ID，核对返回的全部数值、版本和幂等初始化。
3. 施放弹道，核对回执实例、自己和另一账号收到的实例 ID 与参数一致。
4. 修改伤害、速度、半径后等待旧实例/冷却结束，再用当前版本重新初始化；验证实际结算改变。
5. 生成不同尺寸/时长的墙，验证服务器碰撞、过期和多人观察。
6. 重试同一成功动作，确认不重复生成、不重复扣资源；越界参数正常拒绝。
7. 两账号使用同一 ID、不同参数，确认各自配置独立，观察者按实例数据绘制。
8. 重登、迁出、场景重启，核对定义/冷却/实例的上述生命周期边界。

服务端原生与真实 WebSocket 验证见 [VALIDATION.md](VALIDATION.md)；部署步骤见 [SKILL_LAB_DEPLOYMENT.md](SKILL_LAB_DEPLOYMENT.md)。服务端协议通过与 Unity 真实画面验收分别记录。

## 7. 旧页面适配和卡顿定位

`/ws` 始终是纯移动/AOI，不声明战斗能力，也不执行技能。此次排查读取的本机 AXIOMANCY `293c47b` 源码为移动和战斗保存两份独立地址：`gatewayUrl` 默认 `/ws`，`combatGatewayUrl` 默认 `/combat/ws`，分别存在 PlayerPrefs。尚未核对浏览器实际构建和该页面保存值，不能仅凭其中一个输入框判定实际战斗连接。

接入新模块时新增或明确选择 `/skills/ws`，识别新协议；技能目录按任意 `skill_id` 建表，允许空目录，初始化成功后再显示可施放技能。读取新 `skill_cooldowns`，实例按收到的数值绘制，移除旧解析器对两个固定技能 ID 的限制。移动继续使用独立序号与最多四条在途输入。

客户端应分别记录输入时刻、消息发出、回执、实例状态收到和首次画面显示。公网单客户端本次测得施法回执中位约 49ms、实例状态约 99ms；该结果不包含客户端输入节流、表现平滑、帧率和浏览器后台挂起，不能当作实际按键到首帧结果。
