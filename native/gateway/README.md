# C++ 玩家网关

`native-gateway` 直接处理玩家 TCP JSONL 和 WebSocket 消息，向 Backend 与 Scene 调用既有内部 HTTP 接口，不启动或转发至 Go Gateway/Bridge。既有 Go Backend/Auth 可以在迁移期间继续作为独立上游，随后由兼容接口的 C++ 服务替换。

## 环境变量

| 配置 | 默认值 | 用途 |
| --- | --- | --- |
| `GATEWAY_LISTEN` | `127.0.0.1:7000` | TCP JSONL；空字符串关闭此监听 |
| `WEBSOCKET_LISTEN` | 空 | 显式启用 WebSocket HTTP，例如 `127.0.0.1:7001`；空字符串关闭此监听 |
| `GATEWAY_HEALTH_LISTEN` | `127.0.0.1:8080` | 健康接口 |
| `BACKEND_URL` | `http://127.0.0.1:8081` | Go 或 C++ Backend |
| `INTERNAL_TOKEN` | 必填，至少 32 字节 | 内部服务凭据，不是玩家票据 |
| `GATEWAY_PROFILE` | `full` | `full/movement/abilities/elemental/skill_lab` |
| `GATEWAY_LOGIN_SCENE` | 空 | 登录后进入的场景 |
| `GATEWAY_ARENA_SCENE` | 空 | elemental 的独立试炼场 |
| `GATEWAY_STATE_HZ` | movement/skill_lab 为 20，其余 5 | 允许 1～20 |
| `WEBSOCKET_ALLOWED_ORIGINS` | 无 | 逗号分隔的精确浏览器来源；启用 WS 时必填 |
| `GATEWAY_MAX_CONNECTIONS` | 128 | TCP/WS 合计上限，允许 1～256 |

`WS_LISTEN/WS_ALLOWED_ORIGINS` 是前两项的兼容别名。Caddy 继续负责公网 TLS、证书和四个旧路径路由；每个 profile 网关的内部路径都是 `/ws`。HTTP `/healthz` 返回实现、profile、连接数、协议与状态频率，`/readyz` 检查 Backend 的健康接口。

此版本明确不支持 `TLS_CERT_FILE/TLS_KEY_FILE` 在 TCP 监听器上终止 TLS，配置这些变量会拒绝启动，避免误以为数据已经加密。公网入口继续使用受信任 Caddy WSS，容器内部 TCP/HTTP 不直接发布公网。原生 TCP TLS 的使用者需要独立的受信任 TLS 代理。

## 执行与边界

每连接使用一个 Asio strand 串行维护会话状态；网络线程不执行密码运算、数据库调用或场景等待。全局分开的命令、移动、状态、会话及存档工作池处理内部 HTTP，结果回到连接的 strand 后再次检查归属和版本。

- 输入上限 65,535 字节、40 条/秒；命令队列 32、移动在途与队列合计最多 4，待发送回执/状态最多 64。超限不会无限分配内存。
- 移动保序，积累的输入最多 4 条合并为 Scene 的 `/v1/move_batch`；状态查询最多一份在途，慢查询不会排出长队。
- 定时 session 验证独立于移动和状态；过期或验证失败停止连接，旧会话不能继续输入。
- 迁移等待在途移动完成；冻结/切换阶段暂停自动 view/session，旧查询（含旧会话超时）不作用于新 owner。自动及显式 view 均经过 generation、owner/session、tick 与战斗 revision 筛选；慢显式查询被新状态超过时返回最新已知视图。
- 每连接存档串行执行，等待中的快照只保留最新一份。断开时先排完已开始的动作，再精确调用 `/v1/disconnect` 并保存最终权威角色。
- 登录入场已成功但初始试炼场视图失败时，仍保留精确路由用于断开清理；不能把已入场角色当作未登录连接再次创建会话。
- WS 拒绝缺失/未知 Origin、query、非文本数据、无效 JSON、过大或无效 UTF-8 帧。压缩解码后的消息仍受帧上限约束。Ping/Pong 与读写超时由 Beast 管理。
- 内部 HTTP 禁止重定向、验证 HTTPS 证书，响应上限 1MiB，Scene/存档时限 2 秒，session 3 秒，账号/迁移最多 20 秒。关闭 HTTP keep-alive，避免空闲连接占满旧 cpp-httplib 的有限线程。
- 旧 JSON 数值保持 `uint64_t` 精度；同一连接已交付的事件按 world/cursor 去重，完整对象快照继续发送。

网关不是房间、物理或资产的权威来源；技能 ID/参数继续交给 Scene 校验及执行。密码仅由预登录的 `account_*` 入口传往独立账号服务，不进入技能或移动请求。

## 验证

`native-gateway-protocol-tests` 验证字段存在性、身份注入、重复键、帧长、事件/战斗版本、profile 投影等规则。`tests/integration.py` 启动实际 C++ 进程及内存假上游，直接通过 TCP 和 WebSocket 验证五个 profile、Origin/query/关闭码、账号错误码、慢 view 与移动隔离、断开最终存档、精确 uint64 事件与会话失效，并覆盖被冻结源的轮询隔离、慢显式 view 后的断开失败备份、初始视图失败后的入场清理。

这两类测试不替代真实 Scene/Backend 联调、隔离 MySQL、VPS TLS 和浏览器画面验收；通过情况见仓库验证文档。
