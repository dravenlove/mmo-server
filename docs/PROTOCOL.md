# MVP 协议约定

本文件约束当前实现。内部 HTTP/JSON，POST 必须带 `X-Internal-Token`，值来自所有服务一致的 `INTERNAL_TOKEN`（至少 32 字符）。仅 GET `/healthz` 不鉴权。错误为非 2xx 加 `{"error":"说明"}`。请求 JSON 上限 64 KiB。数值必须有限。

## 公共结构

Role: `{role_id:string,name:string,scene_id:string,owner_epoch:int64,session_epoch:int64,x:double,y:double,coins:int64}`。
Ref: `{role_id:string,owner_epoch:int64,session_epoch:int64}`。
Route: `{role:Role,scene_url:string}`。
View: `{self:Role,players:Role[],tick:uint64,scene_id:string}`。

## C++ SceneHost

环境：`INTERNAL_TOKEN` 必填；`SCENE_ID=scene-1`；`SCENE_HOST=127.0.0.1`；`SCENE_PORT=9001`；`TICK_HZ=20`；`AOI_RADIUS=30`；`MOVE_SPEED=6`。
一个实例一个场景，位置限定 [-100,100]。网络线程通过有界命令队列把修改投递到 Tick 线程；tick 负责移动与角色状态。网络等待响应有超时，超时不撤销已入队命令。每个角色最多 128 个；拒绝超限。

- GET `/healthz`: `{ok:true,scene_id:string,tick:uint64,players:int}`。
- POST `/v1/enter`: `{role:Role}` → `{role:Role}`。同 role、同 owner_epoch 新 session 更新会话、重置输入序号/速度、保留当前坐标；同 session 重试保留状态；旧代次拒绝。初次创建即激活，本调用仅由 Backend 在验证归属后发出。冻结/待激活角色不可通过 enter 绕过迁移。
- POST `/v1/move`: Ref 加 `{seq:int64,dx:double,dy:double}` → `{accepted:true}`。seq 必须递增，方向分量 [-1,1]，对角归一化；新输入更新速度，250ms 无新输入自动停下。冻结/待激活或旧 Ref 拒绝。
- POST `/v1/view`: Ref → View。只返回 AOI 半径内活跃玩家（含 self），排序稳定。
- POST `/v1/freeze`: Ref 加 `{transfer_id:string}` → `{role:Role}`。冻结并停止运动；同 transfer_id 重试返回同一状态。
- POST `/v1/unfreeze`: Ref 加 `{transfer_id:string}` → `{accepted:true}`。仅匹配迁移可恢复；已解冻的相同请求可幂等成功。
- POST `/v1/prepare`: `{role:Role,transfer_id:string}` → `{role:Role}`。目标使用 Backend 持久预留的更大 owner_epoch 创建待激活对象；失败迁移也消耗代次，不能假定恰好 +1。相同 Ref/transfer 重试幂等；冲突拒绝。准备对象不得 view/move/enter 或影响其他角色。
- POST `/v1/activate`: Ref 加 `{transfer_id:string}` → `{role:Role}`。仅匹配准备记录可激活，相同请求重试幂等。
- POST `/v1/remove`: Ref（可附 transfer_id）→ `{accepted:true}`。精确匹配才删除；不存在视为成功，不得删除更高代次。删除后保留代次墓碑以拒绝延迟 enter/prepare；进程重启后墓碑丢失的边界需说明。

## Go Backend

环境：`INTERNAL_TOKEN`；`BACKEND_LISTEN=127.0.0.1:8081`；`BACKEND_DB=data/mmo.db`；`SCENES=scene-1=http://127.0.0.1:9001,scene-2=http://127.0.0.1:9002`。只运行一个 Backend 实例，数据库目录需要持久卷。

- GET `/healthz`: `{ok:true,service:"backend"}`，探测数据库可用。
- POST `/v1/register`: `{name:string}` → `{token:string,role_id:string}`。名字 1..24 字符、唯一；服务端生成 32 字节随机 token，数据库仅保存 token 哈希；默认第一场景、epoch=1、session=0、位置0、金币0。
- POST `/v1/login`: `{token:string}` → Route。可靠增加 session_epoch，处理该角色未完迁移，再调用 Scene enter。已在线同场景保留当前坐标。新 session 在场景生效后旧 Ref 不能移动。失败重试允许再次增长 session。
- POST `/v1/session`: `{role_id:string,session_epoch:int64}` → Route。验证会话，完成/阻止未完迁移；供 Gateway 周期校验旧连接与恢复路由。
- POST `/v1/checkpoint`: `{role:Role}` → `{ok:true}`。只在数据库当前 Ref 与 scene_id 一致时更新 x/y，不能用场景 coins 覆盖业务资产。
- POST `/v1/transfer`: Ref 加 `{target_scene_id:string,request_id:string}` → Route。每角色串行；request_id 作为角色内幂等键，保存迁移记录。记录 PREPARING → freeze source → prepare target → 数据库原子更新归属与 COMMITTED → activate target → remove source → DONE。提交前失败走可恢复撤销（只恢复仍归属源的角色）；提交后失败保留记录，之后 login/session/同请求重试继续目标激活，不回滚。启动后/调用时恢复持久记录，不确定时暂停该角色。新目标位置可沿用源位置。重复相同 request_id 不得再次迁移，不同目标同 request_id 拒绝。
- POST `/v1/claim`: `{role_id:string,session_epoch:int64}` → `{coins:int64,granted:bool}`。仅一次领取固定 100 金币的 demo 欢迎奖励，唯一键 welcome:role_id 与余额在同事务提交，重复调用返回 granted=false。不提供任意金额入账接口。

所有场景调用设置超时。拒绝未注册的 scene_id 和无效 Ref。Backend 全局串行控制可用于 MVP，但需明确吞吐与单点边界。SQLite 使用 WAL、busy timeout 和事务。数据库必须有进程级独占保护，防止两 Backend 用同一数据库。

## Go Gateway 外部 TCP

环境：`INTERNAL_TOKEN`；`BACKEND_URL=http://127.0.0.1:8081`；`GATEWAY_LISTEN=127.0.0.1:7000`；`GATEWAY_HEALTH_LISTEN=127.0.0.1:8080`。TLS 可通过 `TLS_CERT_FILE/TLS_KEY_FILE` 开启；明文只用于私网和本机。

每行一个 JSON，最多 64KiB。请求 `{id:string,op:string,...}`；响应 `{id:string,op:string,ok:bool,data:object,error:string}`；异步推送 `{op:"state",ok:true,data:View}`。错误不含凭据。

- `register` + name：注册并返回 token；不会隐式登录。
- `login` + token：绑定 Route，返回 Role；seq 从1开始。
- `move` + seq/dx/dy：转发场景输入，返回 accepted。
- `view`：返回当前 View。
- `transfer` + target_scene_id/request_id：请求 Backend 迁移，更新 Route。
- `claim`：领取一次 demo 奖励。
- `ping`：返回时间。

Gateway 每 200ms 读取一次 View 并推送，每 1 秒验证 Backend session，每 2 秒保存一次当前位置，正常断开尽力保存。视图轮询是明确的 MVP 简化，尚非 gRPC 场景批量推送。每连接单写入者、读写超时、连接与消息速率上限；慢客户端关闭。旧会话失效必须关闭/停止输入。

场景不可用时返回明确错误，不能把路由切到另一个场景。场景重启后通过重新 login 从最近快照入场；位置允许回退，已提交金币持久。演示没有 NPC、技能战斗、自动故障迁移和生产账号系统。
