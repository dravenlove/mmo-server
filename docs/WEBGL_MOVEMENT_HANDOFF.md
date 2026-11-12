# 《万象法则》服务端交接：移动与 AOI

协议：`webgl-movement-v1` · 更新 2026-11-12（Asia/Shanghai）。对应客户端需求：AXIOMANCY `1bcd265`。本次只实现服务端，不修改或构建 Unity 客户端。

**状态：IP WSS 已部署，并完成 Windows 客户机公网协议验收。** 游戏程序仍为 `801b88f`、镜像 `webgl-20261111-v1`。2026-11-12 增加公网 IP 的受信任 TLS 入口，保留原账号、场景和协议；接入和自动续期说明见 [IP 联调文档](IP_WSS_DEBUG.md)。原域名仍受其未备案状态影响，IP 联调不表示原域名已经放行。Unity 引擎构建与浏览器实际操作仍由客户端方验收。

## 客户端接入信息

| 项目 | 最终约定 |
| --- | --- |
| WSS | `wss://<游戏主机公网IP>/ws`；使用部署人员交付的实际 IP，证书由 Caddy 管理 |
| 允许的网页 Origin | `https://<游戏主机公网IP>`；另有 `https://gate.draven.world` 与 `https://cloud.unity.com`，精确匹配 |
| 健康检查 | `https://<游戏主机公网IP>/healthz`；只证明转接进程响应，业务以双角色验收为准 |
| 认证 | 连接后使用 JSON `register` / `login`；URL 不接受查询参数或凭据 |
| 注册规则 | 去除首尾空白；1～24 个 Unicode 码点；禁止控制字符、重名；区分大小写。C# 的 24 UTF-16 单位限制可以继续保留 |
| 身份 | token 为服务端生成的 64 位十六进制字符串；角色 ID 稳定，重连使 `session_epoch` 增加 |
| 场景 | `scene-webgl`，独立无战斗移动场景；注册账号和游戏角色库与现有服务共享 |
| 坐标 | `+x` 向右，`+y` 向上，范围 `[-100,100]`，边界钳制 |
| 移动 | 默认 6 世界单位/秒，斜向归一化，按模拟时间推进，不按发包次数位移 |
| 模拟 / 推送 | 默认 20Hz；完整状态约 200ms / 5Hz，停止后也继续推送 |
| AOI | 同场景、在线角色的二维圆形范围，默认半径 30，边界包含在内 |
| 输入失联 | 250ms 模拟时间后不再继续按旧方向移动；健康运行时再考虑最多一 Tick 调度间隔 |
| 离线可见性 | 正常断线在场景处理 disconnect 后隐藏；异常网关失联以 3 秒场景在线租约作为兜底，再反映到观察者的后续快照 |
| 容量 / 消息 | 每份 players 最多 128 项，包含自己；单条完整 WebSocket UTF-8 文本消息最多 65,535 字节；场景在线名额与性能容量不同，尚未做满载压测 |
| 帧 / 保活 | 一帧文本一个完整 JSON 对象，不接受二进制；客户端业务 ping 约 15 秒可沿用；入口另有 WebSocket Ping/Pong |

IP 入口已允许同一 IP 的 HTTPS 网页，并按用户提供的 Origin 授权 `https://cloud.unity.com`。预览游戏若来自其他来源的 iframe，须以该游戏实际 WebSocket 请求头中的 Origin 为准。WSS URL 与网页 Origin 是两个配置；若用其他页面托管客户端，先提供其实际网页 Origin。IP 根目录仍是等待构建产物的状态页，页面会按当前主机显示 WSS；当前没有发布 Unity 游戏产物。

## 协议兼容与行为差异

沿用需求中的 `register`、`login`、`view`、`ping`、`move`，响应原样返回字符串 `id` 和 `op`，`ok` 为布尔值，成功 `data` 为对象。状态推送为无 `id` 的 `op:state`，完整 View 包含 `self`、`players`、`tick`、`scene_id`；所有代次均为 JSON 整数。

1. 登录后连接绑定可信角色。服务端内部将角色迁移到 `scene-webgl`，完成后才回复登录成功。客户端不需要传递场景、角色 ID 或迁移请求。现有凭据可继续登录。
2. 本入口只提供本轮五个操作。无战斗场景不推进怪物、伤害或战斗消耗，客户端 View 不提供战斗字段；既有战斗快照仍保存在内部，用于返回原战斗场景，不能借进入移动场景刷新血蓝或冷却。原 TCP 桌面入口继续提供战斗功能。
3. `move` 必须包含整数 `seq` 和有限数值 `dx` / `dy`。方向分量范围 `[-1,1]`。序号在当前登录会话内递增；重复或旧序号明确失败，不返回成功来掩盖拒绝。
4. 支持 4 条移动请求在途。网关移动通道独立于慢视图和数据库 I/O；已积压的最多 4 个方向会作为一个内部批次逐条校验并各自回复。相同模拟 Tick 前，较新已接受方向可以覆盖较早方向；每个方向不额外产生位移。`accepted:true` 表示方向已接收，不能当成某个坐标已包含该输入的确认。
5. 零方向停止；断线先按角色、owner/session epoch 停止并隐藏，再保存该回执中的最终权威位置。迟到旧会话的断线/移动不能影响新的会话。无法复核会话时暂停并结束连接。
6. `players` 是完整显示集合，默认包含自己；`self` 始终独立提供。`visible_player_count` 包含自己，统计裁剪前在线可见人数；发生人数或字节裁剪时设置 `players_truncated:true`。裁剪时未出现的 ID 仅说明不在本次显示子集中，不代表可靠离开事件。
7. 状态的 tick 在同一上下文中单调；场景重启导致旧角色上下文不可用时，旧连接收到错误后重新登录，新的 session epoch 区分重置。客户端继续按需求中的过期 tick 处理规则运行。
8. 错误沿用 `ok:false`。未引入可恢复警告或“合并成功但丢弃”类新语义。ping 成功 data 额外提供 `protocol_version:"webgl-movement-v1"`；这是可忽略的扩展字段。

原 JSONL 协议仍见 [PROTOCOL.md](PROTOCOL.md)。当前入口默认业务消息限速 40 条/秒；健康发送频率与在途限制应共同遵守。当前实现采用精确圆形 AOI 与有界快照，不包含障碍物遮挡或增量事件流。

## 部署与运维

游戏主机的 `deploy/game-host.compose.yaml` 新增 `scene-webgl`、`gateway-webgl`，复用原 scene/gateway 镜像，无需新增数据库或更换原 Auth 服务。场景与网关仍在同机 Docker 网络；专用 Gateway 只发布回环 `7002`，其健康端口为回环 `8083`。

调用链：浏览器 WSS → Caddy → 回环 7001 WebSocket bridge → 回环 7002 WebGL Gateway → 同机 `scene-webgl:9003`。注册和登录继续经既有 Game Backend / Auth，在线操作不经过跨地域鉴权链路。

首次准备新的转接服务时，使用 [systemd 模板](../deploy/mmo-websocket.service.example)与 [环境模板](../deploy/websocket.env.example)；实际环境文件为 `/etc/mmo-websocket.env`，只由运维填写。旧转接服务已经存在的机器先备份 unit、程序与 build.json，再升级，不重复覆盖 Caddy 或其他站点。

游戏发行镜像标签：`webgl-20261111-v1`。保留原 MySQL 参数、内部密钥、数据卷及战斗规则。目标主机只拉镜像；bridge 使用构建机交叉编译后的 Linux amd64 程序。原有 [game/auth 部署文档](GAME_AUTH_DEPLOYMENT.md)继续适用，升级时先下载，再在维护窗口保存快照并重建服务。

```bash
# 游戏主机，仓库根目录；使用当前私有环境文件
sudo docker compose --env-file deploy/game-host.env -f deploy/game-host.compose.yaml logs --since 10m --tail 100 gateway-webgl scene-webgl
sudo systemctl status mmo-websocket --no-pager
```

`WEBGL_TICK_HZ`、`WEBGL_AOI_RADIUS`、`WEBGL_MOVE_SPEED` 控制专用场景参数；改变这些参数应更新交接约定。`WEBGL_TRACE_MOVEMENT=true` 开启诊断：网关记录请求 ID、seq、排队/调用/发送耗时；场景记录方向接收、模拟应用、快照生成的 Tick 和进程内排队耗时。内部批次保留每条输入 ID/seq，日志不包含 token 或请求全文。使用同一 ID/seq 对照各阶段，不直接相减未校准的跨进程/客户端绝对时钟。Docker 日志已设置轮转。

## 验收与客户端交接边界

新增场景测试覆盖斜向速度、按时间位移、批内拒绝与停止、250ms 输入失联、离线隐藏、迟到旧会话、重连、AOI 边界、128 项裁剪和战斗快照保留。网关测试单独阻塞视图，证明 4 条移动可以在它结束前收到回执；断线保存使用最终角色快照。

GitHub Linux 隔离验收使用：

```bash
python3 scripts/smoke_docker.py --tag webgl-20261111-v1 --database mysql --external-auth --hot-reload --webgl
```

其中 WebSocket 双角色检查使用独立临时项目和数据库，覆盖 WSS 文本桥接核心、注册登录、4 条在途输入、停止、无输入超时、AOI 离开/返回、新会话和断线消失。线上 WSS 使用独立受控验收角色，不读取用户凭据。

服务端协议检查、HTTPS/WSS 入口检查、Unity 编译和真实 WebGL 操作是不同验证范围。最终现场结果记录在 [VALIDATION.md](VALIDATION.md)；客户端方收到接入完成通知后，按本文件适配并完成双角色、松键/失焦和浏览器真实操作验收。

## 原域名恢复后的补充复核

当前直接按 IP 联调即可。以后恢复原域名时，在 [腾讯云备案控制台](https://console.cloud.tencent.com/beian)按实际主体情况完成首次/新增备案及接入，等待平台解除拦截。随后保持原 WSS 地址，按下面顺序复核：

1. 从实际客户端网络 GET `https://gate.draven.world/healthz`，确认返回 JSON 和 `webgl-movement-v1`，而非跳转页。当前 HEAD 曾到达 Caddy，而 GET 被平台拦截，不能只用 `curl -I` 下结论。
2. 用实际网页 Origin 建立 WSS，发送 ping、注册两个不同名字并登录，确认场景为 `scene-webgl`。
3. 客户端按本文同步后，验证键盘方向、斜向速度、松键/失焦停止、AOI 进出、关闭网页和同凭据重连。Unity 编译与浏览器真实操作由客户端方另行验收。

当前服务已部署并保持运行；无需为备案放行重新清库、生成密钥或注册原有账号。现场测试与待办边界见 [VALIDATION.md](VALIDATION.md)。
