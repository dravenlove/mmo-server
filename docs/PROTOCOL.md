# MVP 协议约定

账号密码注册、登录、旧 token 绑定与到期票据见 [账号登录协议](ACCOUNT_LOGIN.md)。新增 `account_*` 不改变原 `login(token)`、角色会话和战斗协议。

> Unity WebGL 的 WSS、移动专用场景、在线 AOI 和公网待备案状态见 [WEBGL_MOVEMENT_HANDOFF.md](WEBGL_MOVEMENT_HANDOFF.md)。本文继续说明完整 TCP 游戏协议。


本文件约束当前实现。内部 HTTP/JSON，POST 必须带 `X-Internal-Token`，值来自所有服务一致的 `INTERNAL_TOKEN`（至少 32 字符）。仅 GET `/healthz` 不鉴权。错误为非 2xx 加 `{"error":"说明"}`。请求 JSON 上限 64 KiB。数值必须有限。

## 公共结构

Role: `{role_id:string,name:string,scene_id:string,owner_epoch:int64,session_epoch:int64,x:double,y:double,coins:int64,combat?:object}`。
Ref: `{role_id:string,owner_epoch:int64,session_epoch:int64}`。
Route: `{role:Role,scene_url:string}`。
View: `{self:Role,players:Role[],tick:uint64,scene_id:string}`；战斗版本追加 mobs、skills、events 及 players_truncated/visible_player_count；数值热更版再追加 `config_version:string,config_generation:uint64,config_applied_tick:uint64`。旧字段保留。

## C++ SceneHost

环境：`INTERNAL_TOKEN` 必填；`SCENE_ID=scene-1`；`SCENE_HOST=127.0.0.1`；`SCENE_PORT=9001`；`TICK_HZ=20`；`AOI_RADIUS=30`；`MOVE_SPEED=6`；可选 `COMBAT_CONFIG_FILE` 为固定数值配置文件路径。不设置使用内置默认，指定文件但读取或校验失败则启动退出。
一个实例一个场景，位置限定 [-100,100]。网络线程通过有界命令队列把修改投递到 Tick 线程；tick 负责移动、战斗与怪物状态。网络等待响应有超时，超时不撤销已入队命令。每个场景最多保留 128 个已加载角色，包括离线、冻结和待激活角色；拒绝超限。

- GET `/healthz`: `{ok:true,scene_id:string,tick:uint64,players:int}`。
- POST `/v1/enter`: `{role:Role}` → `{role:Role}`。同 role、同 owner_epoch 新 session 更新会话、重置输入序号/速度、保留当前坐标；同 session 重试保留状态；旧代次拒绝。初次创建即激活，本调用仅由 Backend 在验证归属后发出。冻结/待激活角色不可通过 enter 绕过迁移。
- POST `/v1/move`: Ref 加 `{seq:int64,dx:double,dy:double}` → `{accepted:true}`。seq 必须递增，方向分量 [-1,1]，对角归一化；新输入更新速度，250ms 无新输入自动停下。冻结/待激活或旧 Ref 拒绝。
- POST `/v1/cast`: Ref 加 `{seq:int64,skill_id:string,target_id?:string}` → 技能结算结果。斩击和突进须指定存活怪物 ID；旋风斩和战意恢复的目标可省略、为空或填写自身角色 ID。技能、目标、距离、法力、冷却和存活状态由场景检查；当前会话最近一次成功动作的相同序号和内容可返回缓存结果，更旧或内容冲突的序号拒绝且不重复结算。
- POST `/v1/revive`: Ref 加 `{seq:int64}` → 复活结果。死亡等待约 5 秒后由服务器将角色移至原点，恢复 180 生命和 100 法力，保留剩余技能冷却；重复成功动作不会再结算。
- POST `/v1/view`: Ref → View。返回 AOI 半径内处于 Active 状态的角色，按 role_id 稳定排序；极端帧预算下可裁剪其他角色，自身始终保留。
- POST `/v1/freeze`: Ref 加 `{transfer_id:string}` → `{role:Role}`。冻结并停止运动；同 transfer_id 重试返回同一状态。
- POST `/v1/unfreeze`: Ref 加 `{transfer_id:string}` → `{accepted:true}`。仅匹配迁移可恢复；已解冻的相同请求可幂等成功。
- POST `/v1/prepare`: `{role:Role,transfer_id:string}` → `{role:Role}`。目标使用 Backend 持久预留的更大 owner_epoch 创建待激活对象；失败迁移也消耗代次，不能假定恰好 +1。相同 Ref/transfer 重试幂等；冲突拒绝。准备对象不得 view/move/enter 或影响其他角色。
- POST `/v1/activate`: Ref 加 `{transfer_id:string}` → `{role:Role}`。仅匹配准备记录可激活，相同请求重试幂等。
- POST `/v1/remove`: Ref（可附 transfer_id）→ `{accepted:true}`。精确匹配才删除；不存在视为成功，不得删除更高代次。删除后保留代次墓碑以拒绝延迟 enter/prepare；进程重启后墓碑丢失的边界需说明。

### 数值配置管理

以下接口同样需要 `X-Internal-Token`，不通过 Gateway 玩家 TCP 暴露。配置全量校验与切换都在原有 Tick 命令队列执行；请求超时不撤销已入队命令。完整 schema 与部署工具见 [数值热更](CONFIG_HOT_RELOAD.md)。

- POST `/v1/config/status`: `{}` → `{config:object,version:string,generation:uint64,applied_tick:uint64}`。返回当前生效完整配置；不读取或修改文件。
- POST `/v1/config/validate`: `{config:object,expected_generation?:int64}` → `{valid:true,config:object,version:string,current_version:string,generation:uint64,applied_tick:uint64,would_change:bool}`。验证完整配置、同名版本冲突、64 版本历史容量及可选 CAS，不占用版本名、不修改玩家或配置状态。
- POST `/v1/config/reload`: `{expected_generation?:int64}` → 当前配置状态加 `{changed:bool}`。HTTP 线程限量读取启动时固定的 `COMBAT_CONFIG_FILE`，随后交给 Tick 校验和切换；请求不能传配置或路径。未设置文件返回 409。
- POST `/v1/config/apply`: `{config:object,expected_generation?:int64}` → 当前配置状态加 `{changed:bool}`。仅内存调试，不写文件；重启重新读取启动文件。正式部署使用固定文件加 reload。

配置固定 `schema_version=1`，完整包含 `version,skills,monsters`，仅允许四个既有技能 ID 与 wolf/elite。文件和请求上限 64 KiB，拒绝重复/未知/缺失键、非法数值及 NaN/Infinity。技能行为、名称、目标类型和玩家 180/100 上限保持固定。技能数值在新施法时生效；现有 HP/MP/CD、动作高水位和缓存不重置。NPC 在下次刷新或脱战重置时整体采用新参数；当前死亡刷新等待沿用该 NPC 持有版本。

每次实际切换 `generation` 增 1，并记录 `applied_tick`；当前同版本同内容幂等不增。同版本不同内容或过期 `expected_generation` 返回 409，历史容量耗尽拒绝新版本并返回 503。进程最多记住 64 个版本名，不淘汰历史；历史同内容回切允许，属于一次新切换。历史、代次和应用 Tick 在重启后重建，代次从 1 开始。单节点切换原子，但管理工具对两场景依次发布，不保证它们同时生效；失败恢复规则见 [发布一致性](CONFIG_HOT_RELOAD.md#发布一致性与故障处理)。

## Go Backend

环境：`INTERNAL_TOKEN`；`BACKEND_LISTEN=127.0.0.1:8081`；`BACKEND_DB_DRIVER=sqlite`（默认）及 `BACKEND_DB=data/mmo.db`；`SCENES=scene-1=http://127.0.0.1:9001,scene-2=http://127.0.0.1:9002`。连接指定 MySQL 时改为 `BACKEND_DB_DRIVER=mysql` 并填写 [MySQL 环境变量](MYSQL.md)。两种模式均只运行一个 Backend；SQLite 目录需持久卷，MySQL 数据存于指定库。

- GET `/healthz`: `{ok:true,service:"backend"}`，探测数据库可用。
- POST `/v1/register`: `{name:string}` → `{token:string,role_id:string}`。名字 1..24 字符、唯一；服务端生成 32 字节随机 token，数据库仅保存 token 哈希；默认第一场景、epoch=1、session=0、位置0、金币0。
- POST `/v1/login`: `{token:string}` → Route。可靠增加 session_epoch，处理该角色未完迁移，再调用 Scene enter。已在线同场景保留当前坐标。新 session 在场景生效后旧 Ref 不能移动。失败重试允许再次增长 session。
- POST `/v1/session`: `{role_id:string,session_epoch:int64}` → Route。验证会话，完成/阻止未完迁移；供 Gateway 周期校验旧连接与恢复路由。
- POST `/v1/checkpoint`: `{role:Role}` → `{ok:true}`。只在数据库当前 Ref 与 scene_id 一致时更新 x/y，不能用场景 coins 覆盖业务资产。
- POST `/v1/transfer`: Ref 加 `{target_scene_id:string,request_id:string}` → Route。每角色串行；request_id 作为角色内幂等键，保存迁移记录。记录 PREPARING → freeze source → prepare target → 数据库原子更新归属与 COMMITTED → activate target → remove source → DONE。提交前失败走可恢复撤销（只恢复仍归属源的角色）；提交后失败保留记录，之后 login/session/同请求重试继续目标激活，不回滚。启动后/调用时恢复持久记录，不确定时暂停该角色。新目标位置可沿用源位置。重复相同 request_id 不得再次迁移，不同目标同 request_id 拒绝。
- POST `/v1/claim`: `{role_id:string,session_epoch:int64}` → `{coins:int64,granted:bool}`。仅一次领取固定 100 金币的 demo 欢迎奖励，唯一键 welcome:role_id 与余额在同事务提交，重复调用返回 granted=false。不提供任意金额入账接口。

所有场景调用设置超时。拒绝未注册的 scene_id 和无效 Ref。Backend 全局串行控制可用于 MVP，但需明确吞吐与单点边界。SQLite 使用 WAL、busy timeout、事务与文件独占锁；MySQL 使用 InnoDB 事务、迁移唯一约束和固定连接上的会话独占锁。防止两个 Backend 同时写同一库，MySQL 模式也不开放多副本。

## Go Gateway 外部 TCP

环境：`INTERNAL_TOKEN`；`BACKEND_URL=http://127.0.0.1:8081`；`GATEWAY_LISTEN=127.0.0.1:7000`；`GATEWAY_HEALTH_LISTEN=127.0.0.1:8080`。TLS 可通过 `TLS_CERT_FILE/TLS_KEY_FILE` 开启；明文只用于私网和本机。

每行一个 JSON，最多 64KiB。请求 `{id:string,op:string,...}`；响应 `{id:string,op:string,ok:bool,data:object,error:string}`；异步推送 `{op:"state",ok:true,data:View}`。错误不含凭据。

- `register` + name：注册并返回 token；不会隐式登录。
- `login` + token：绑定 Route，返回 Role；seq 从1开始。
- `move` + seq/dx/dy：转发场景输入，返回 accepted。
- `view`：返回当前 View。
- `transfer` + target_scene_id/request_id：请求 Backend 迁移，更新 Route。
- `claim`：领取一次 demo 奖励。
- `cast` + seq/skill_id/target_id：Gateway 使用已登录角色的可信 Ref 调用当前场景，客户端不提供伤害或 HP。
- `revive` + seq：请求当前场景执行死亡等待后的复活。
- `ping`：返回时间。

Gateway 每 200ms 读取一次 View 并推送，每 1 秒验证 Backend session，每 2 秒保存一次位置和战斗快照，成功施法/复活后及正常断开也会尽力保存。视图轮询是明确的 MVP 简化，尚非 gRPC 场景批量推送。每连接单写入者、读写超时、连接与消息速率上限；慢客户端关闭。旧会话失效必须关闭/停止输入。

场景不可用时返回明确错误，不能把路由切到另一个场景。场景重启后通过重新 login 从最近快照入场；位置和战斗状态可能回退检查点之后的变化，已提交金币持久。当前包含 PvE 怪物和技能战斗，尚未提供自动故障迁移及生产账号系统。

## 战斗状态与兼容

Role 可追加 `combat` 对象：`hp,max_hp,mp,max_mp,dead,revive_remaining_ms,revision,last_action_seq,action_session,cooldowns`；cooldowns 为 strike/whirlwind/charge/mend 四项剩余毫秒。新版允许每项剩余 CD 为 0～600000，不再以当前技能表为恢复上限，因此缩短数值配置 CD 后，旧剩余 CD 仍可重连、迁移和恢复；不会被主动截短。旧 combat 二进制可能拒绝新版产生的更长 CD，数值回滚不等于应用二进制回退。自身角色和迁移/检查点使用完整状态，其他可见玩家可仅带血量摘要。处于 Active 状态且存活的角色每秒恢复 6 法力，最多 100。新会话只清动作序号，血蓝/冷却保留；冻结或待激活角色不参与战斗，法力恢复、技能冷却和复活倒计时均暂停。角色 `revision` 是战斗状态版本，与配置 `generation` 不同。

施法和复活共享同一会话的单调动作序号高水位。动作结果缓存仅留在当前进程的角色对象中，不随迁移快照保存；目标场景或重启进程恢复后，旧序号可被拒绝，但不会重新结算。网络超时重试同一次技能时应复用原序号，不得自动换新序号重复发送。

View.mobs 含 `id,name,kind,x,y,hp,max_hp,alive,respawn_remaining_ms,target_id,config_version`；怪物 `config_version` 表示其所持参数版本，可能暂时不同于场景当前版本。skills 含 `id,name,description,range,cooldown_ms,mp_cost,damage,heal,target`，来自当前生效配置；events 含最近最多32项 `id,type,source_id,target_id,skill_id,value,x,y,tick`。事件类型包含 cast/damage/heal/death/respawn/revive/reset，客户端应按场景和事件id去重，初次加入不回放历史伤害。

`cast` 返回 `{accepted,seq,skill_id,target_id,hits:[{target_id,damage,hp,max_hp,dead}],healed,role,tick,config_version}`；`config_version` 为这次施法采用的配置版本，重复缓存结果保留原版本。`revive` 返回 `{accepted,seq,role,tick}`。目标只允许当前可交互的怪物；本版未开放 PvP。

为遵守64KiB外部帧限制，极端满员/长名字的AOI视图可裁剪其他players；`players_truncated=true` 时，`visible_player_count` 表示未裁剪总数，自身始终保留。

Backend 的 `combat_snapshots` 表新增保存当前角色的归属、会话、版本和完整JSON状态。快照写入与位置/归属修改使用相同事务校验，旧会话、旧归属及低版本不能回退当前快照。战斗技能不是逐次持久事务；进程故障会恢复最近检查点，边界与玩法见 [战斗说明](COMBAT.md)。
