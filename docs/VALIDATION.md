# 验证记录

日期：2026-11-04 基线；2026-11-05 增加 MySQL 和桌面客户端；2026-11-06 增加 PvE 战斗和数值热更验证；2026-11-11 增加真实跨地域链路排查与隧道验收；2026-11-12 增加弹道/墙体及元素试炼场。以下结果不等于容量或生产可用性承诺。




## 2026-11-13：Lua / ECS / 房间 Worker（隔离验证，未切现网）

在 C++ 核心替换上新增 `native-worker`、`native-script`、EnTT 权威组件存储、GameService 房间目录与新 Gateway 房间命令。设计、客户端增量接口及部署/排空流程见 [ROOMS_LUA_ECS.md](ROOMS_LUA_ECS.md)。

本机独立 Ubuntu 24.04 WSL / GCC 13.3 / Lua 5.4.6 完成：

- 统一 Debug 构建与 **20/20 CTest**：原生数据库/账号/旧库接管、网关竞态、Lua 沙箱预算、ECS 与旧 Scene 算法、Room 生命周期及真实 HTTP。新世界会话旋转、迟到的旧控制请求、旧检查点被拒绝、版本及 SHA 固定、存档后回收、离线排空均有回归。
- `test_room_stack.py`：三隔离密码账号、两个并行房间、准备/房主权限、客户端参数化弹道、跨房隔离；新脚本只影响新房，Lua deny 不扣 MP/序号或生成半套对象；Worker/Backend/Gateway 重启后保留身份、100 金币、房间路由与归档版本；结束后的活房/空房角色安全回原大厅。
- `test_native_legacy.py --scene .../native-worker`：原 15 项基础流程、PvP 弹道/墙体、元素个人试炼和迁移丢响应恢复、技能实验室参数/版本/重连通过。兼容默认场景实际换成 Lua/ECS Worker；部分旧 WebSocket 用例仍通过旧 Go bridge 对照，直接 C++ WS 由网关 CTest 和下一项覆盖。
- 四 profile 直接 C++ WebSocket 账号/移动/失效会话/重启通过；C++ bcrypt 产物由 Go 验证。原 Go `go test ./...` 通过。
- 独立 MySQL 8.0.46 通过全部跨语言接管与 Backend 不变量，包括新增房间表和恢复测试。GameService 与 Auth 均使用无 DELETE 的受限测试账号；角色、旧 token、100 金币和战斗快照保持，原命名锁与固定连接失联停止写入不变。没有连接真实业务库。
- 实际离线合并四份部署组合：新第三层只切核心镜像与 scene-skills Worker，保留 Auth、其他五个 Scene、原数据卷、TLS 和端口。测试工具补齐 WebSocket continuation 解析，能读取超过单帧大小的真实房间视图。

镜像的 Release 构建、普通用户容器/新卷和匿名拉取由独立 Linux CI 验证，发布证据随后记录。上述进程验证不等于 VPS 切换、真实 Unity 画面、全国网络或容量验收。Lua 首版只编排已注册弹道/墙体的生成计划；Protobuf、安全 KCP、Redis/集群及完整对局无损恢复尚未实现。

## 2026-11-13：C++ 核心服务迁移（隔离验证，未切现网）

新增 `native-gateway`、`native-account`、`native-game-service` 和共享 `native-db`。在本机独立 Ubuntu 24.04 WSL 环境以 GCC 13.3 / C++17 构建；未使用两台 VPS 编译，也未修改 Unity 工程。构建、镜像和切换入口见 [NATIVE_CPP_RUNBOOK.md](NATIVE_CPP_RUNBOOK.md)。

- 7 项原生 CTest 通过：数据库事务/锁、账号 UTF-8/bcrypt 与 HTTP、Backend 不变量、Go 旧库接管、网关协议及真实 TCP/WebSocket 集成。原 Scene 的 9 项 CTest 通过。
- `test_native_stack.py` 四个 profile 通过：直接连接 C++ WebSocket，账号注册/密码取票据、登录、连续移动、参数化弹道、旧会话失效与 Gateway 重启。此链路中 Gateway/Auth/Backend/Scene 全为 C++。
- `test_native_legacy.py` 在临时源码副本中复用原验收：15 项基础检查、弹道/PvP/墙体/重连、元素试炼与 prepare/unfreeze/activate 丢响应恢复、客户端参数化技能/版本/双角色观察通过。此处部分旧 Go 用例自建 Go bridge 作为协议对照；新 WebSocket 实现另由上一项及网关 CTest 直接验证。
- 独立 MySQL **8.0.46** 通过：原 Go `$2a$` 密码和票据由 C++ 接管，C++ `$2b$10$` 哈希由 Go 验证，永久 token 与稳定身份保持，最小权限账号没有 DELETE 也能轮换五票据，双方竞争同一原命名锁。静止 UPDATE 匹配行数、KILL 原固定连接后旧 writer 永久停止写入、新 writer 接锁及完整 Backend MySQL 不变量通过；`localhost` 使用配置的 TCP 端口，不误连默认 Unix socket。
- Go 创建的完整旧 SQLite 库由 C++ 接管，恢复未决 COMMITTED 迁移，保留角色 ID、永久 token、100 金币、战斗快照及幂等奖励。没有重新注册或重新散列已有身份。
- 跨模块审查发现并修复三类异步问题：迁移中自动查询冻结源；慢显式 view 覆盖较新自动状态并导致最终存档回退；Elemental 登录已入场但初始 view 失败时漏 disconnect。分别增加竞态与失败响应夹具，包含旧 revision 1 与新 revision 9 交错后最终仍保存 9 的验证。
- 原 Go 全套 `go test ./...` 通过。三份新增 Compose（本地与 game/auth override）使用合成配置完成实际合并检查：新 image/entrypoint、清除旧 Go build、保留原场景/数据卷/TLS、四个回环 WS 端口及完整 tmpfs 选项；本机 env 初始化不覆盖已有配置。另以 UID/GID 10001、清空附加组完成四 profile 原生进程联调。

[完整 Linux CI 43407338371](https://github.com/dravenlove/mmo-server/actions/runs/43407338371) 对源码 `20b17dbaf7937cf935fb1dac130ed3691dd90af2` 全部通过：上述回归、Release 运行镜像、普通用户容器与新 SQLite 卷、真实 C++ WS→GameService→Auth→Scene 的账号/移动/参数化施法，以及匿名 Docker 拉取。已发布 `ghcr.io/dravenlove/mmo-server/backend:cpp-core-20b17dbaf793`，digest `sha256:3bfea5e02734378867fbcf5e80c25b170f361f2245b6909745fec405efd62915`；Windows 客户机再以匿名 registry token 读取 manifest/config，确认 linux/amd64、原生 Gateway 入口及相同 revision。发布复用既有公开 backend 包，使用独立 C++ 标签，旧 Go 标签未覆盖。

这些是阶段 2 的隔离进程/数据库结果，不是 VPS 更新、真实 Unity 画面、全国网络或容量验收。Lua/ECS 房间在上述后续阶段加入；Protobuf 和安全 KCP 尚未实现。原生 TCP 直连 TLS 暂需受信任代理，不能清空证书变量降级。现网仍按下面已有运行快照管理。

## 2026-11-13：账号密码登录部署与公网验收

账号密码协议与游戏侧镜像为 `account-20261113-v1` / `674a5502f18671434f1162afa86a448990517510`，鉴权修正镜像为 `account-auth-20261113-v2` / `4ab0058a478b4c6a66f707b6a66acde7eda0a615`。[完整 Linux 发行 43349657010](https://github.com/dravenlove/mmo-server/actions/runs/43349657010) 通过 Go/race/vet、9 项 CTest、原生技能、SQLite/MySQL Docker、独立鉴权、真实 WebSocket 账号和旧账号绑定验证；[鉴权补丁发行 43352755405](https://github.com/dravenlove/mmo-server/actions/runs/43352755405) 通过 Go/race/vet、MySQL 回归和匿名镜像拉取。

真实部署账号没有 DELETE 权限，首版 Auth 的过期票据 DELETE 使注册事务回滚。修正为每账号最多五条记录循环复用，保留原 MySQL 权限。新增隔离 MySQL 用例明确证明测试账号不能 DELETE，再完成注册、登录、六张票据轮换、最早票据拒绝及最新票据可用；未依赖 root 权限掩盖部署问题。SQLite 用例覆盖加盐密码哈希、错误密码、用户名存在性统一失败、到期、重启、绑定不覆盖旧密码、原 token 保留与限流；Backend 验证临时票据不会污染旧角色身份哈希，Auth 重启导入仍成功。

最终部署检查通过：12 个游戏服务及独立 Auth 健康，Auth 修正版重启计数 0；六个 C++ 场景容器 ID、原 Native bridge、隧道和数据卷保持不变。更新时原角色与战斗快照逐项一致；公网验收后原 11 个角色的名字、凭据哈希、金币仍一致，新增 1 个受控测试账号，角色总数 12，未完成迁移 0。配置与数据库私有备份位于 `backups/account-20261113-v1/`，Auth 补丁备份位于鉴权机 `backups/account-auth-20261113-v2/`，未恢复旧数据库覆盖新数据。

Windows 客户机通过可信 IP TLS 和实际 Unity iframe Origin，完成新账号注册、错误密码 401，以及四个公网入口的密码取票据、原 login 入场、权威 view；已入场后账号操作正常拒绝。受控凭据仅存在本机受限目录，不写入 Git。四个 HTTPS 健康入口通过。该次注册约 1.07 秒，正确密码取票据约 635～644ms，随后入场 login 约 646～662ms；这是一次跨地域鉴权/数据库链路的协议观测，不是容量、游戏动作响应或浏览器画面测量。在线移动与技能继续绕过 Auth。

交付 [账号登录协议](ACCOUNT_LOGIN.md) 与 [网络协议选择](NETWORK_TRANSPORT.md)；没有修改或构建 Unity 客户端，登录 UI 仍需按协议接入。

## 2026-11-13：参数化技能交付复查

09:39（Asia/Shanghai）只读复查通过：12 个游戏服务健康，五个原场景容器 ID 保持不变；五个 Gateway、新技能 Scene 和 Native bridge 仍为 `0c48fbe15d6548fce6411591911b51f5311c68ce`。四个入口健康检查与独立 Auth 正常，Auth 未重启。未重跑会改变账号状态的业务探针；下面的功能和延迟数字仍是 2026-11-12 的历史实测，不是今天重新测得的浏览器指标。

## 2026-11-12：客户端参数化技能与战斗链路改造

用户选择联调模式：客户端提交任意技能 ID 和完整参数，服务器注册后生成实际弹道/墙体实例。实现源码 `0c48fbe15d6548fce6411591911b51f5311c68ce`，镜像 `skill-lab-20261112-v1`。[Linux 发行运行 43223589834](https://github.com/dravenlove/mmo-server/actions/runs/43223589834) 全部成功，覆盖 Go/race/vet、9 项 CTest、SQLite 原生四协议、MySQL/独立鉴权 Docker 四协议、持久化恢复与匿名拉取。

新 C++ 用例验证自定义 ID、参数边界、两个角色同名不同定义、初始化幂等与版本比较、世界代次隔离、配置更新、冷却和实例占用、定义上限、实际 17 点 PvP 伤害、动态墙尺寸/生命周期、重连和旧六键存档兼容。新 WebSocket 用例在真实 C++/Backend/Gateway 上验证另一账号收到同一实例、使用施法者的实际参数、按配置伤害、挡弹、动作重试、重登与压缩。Go 故障用例把数据库保存或自动状态查询刻意阻塞，确认施法回执和后续 ping 仍完成；旧异步状态不会覆盖更新的战斗修订，最终断开快照最后保存。原 15 项 smoke、旧弹道和元素三种迁移故障恢复回归通过。

Gateway 现将状态查询限制为每连接一个在途，并合并等待中的定时需求；存档逐连接串行异步执行，最多保留一份尚未执行的最新快照。旧代次/显式视图之前启动的异步结果被丢弃。完整对象持续发送，可靠连接中已经送出的历史表现事件不反复加入 `state`；显式 `view` 保留近期窗口。Native bridge 协商 `permessage-deflate`，解压后仍限制消息大小；旧无压缩客户端保持可用。

线上新增共享 `scene-skills` / `gateway-skills` 和 `/skills/ws`，协议 `webgl-skill-lab-v1`；12 个游戏容器健康。五个旧 Scene 容器 ID 未变，Backend 继续使用原镜像并重启载入新场景映射，五个 Gateway、新 Scene 与 Native bridge 为上述源码。Native SHA-256 为 `b96c2e5e9b8bff20e8b040ff093dbf4e7a6f884051e8cbb5d3e17e5a1912d7d7`。独立 Auth 原镜像保持健康、重启次数为 0，鉴权隧道和原入口保留。

首次切换的桥接端口检查早于监听就绪，自动恢复旧运行版本，未恢复数据库旧数据；以新备份和有界就绪重试完成第二次切换。开放新路由前，10 个角色、9 份战斗快照逐项一致。验收后原 10 个角色的名字、凭据哈希与余额仍一致，角色总数为 11；验证脚本只复用两个既有受控账号，没有发送注册请求。没有未完成迁移。私有材料在 `backups/skill-lab-20261112-v1/` 和成功重试的 `backups/skill-lab-20261112-v1-retry1/`。

Windows 客户机通过可信 IP TLS 和实际 Unity iframe Origin 测得新入口：

| 场景（各六次施法） | 回执中位 / 最大 | 自己收到实例状态中位 / 最大 | 观察者收到实例状态中位 / 最大 |
| --- | --- | --- | --- |
| 一个客户端，移动并施法 | 48.55 / 49.50ms | 99.41 / 101.84ms | — |
| 两个账号，移动并施法 | 49.03 / 49.87ms | 99.19 / 100.31ms | 99.66 / 100.63ms |

使用单调高精度计时，实例确认来自自动状态中的对象或生成事件，不把施法回执当作已经收到世界状态。公开接口测得任意 ID 注册、重复初始化、同 ID 不同角色参数、实际相同实例 ID/速度/半径、共享墙尺寸与过期、更新定义、旧版本和旧初始化拒绝、越界参数提示字段、连接继续可用。公网探针使用零伤害配置，实际 17 点伤害由 C++、原生和 MySQL 容器联调覆盖。没有新建测试账号。

同一套静止连接采样对比旧战斗入口：

| 指标 | 更新前 | 更新后 |
| --- | --- | --- |
| 单连接状态中位大小 | 11,206 字节 | 2,056 字节 |
| 两连接状态中位大小 | 11,966 字节 | 2,341 字节 |
| 单连接应用消息量 | 53.88KiB/s，5Hz | 40.48KiB/s，20Hz |
| 两连接应用消息量 | 117.49KiB/s，5Hz | 93.52KiB/s，20Hz |
| WebSocket 压缩协商 | 无 | 已协商 |

大小和速率均是解码后的应用消息，不是实际压缩后的公网出口字节，也不是带宽上限/容量测试。更新前单份消息约 9KB 为重复历史事件。原 `/ws` 的 10 项移动/AOI/会话回归通过，118 次主动视图回执中位 50.87ms、最大 98.77ms；63 次移动回执中位 50.06ms、最大 110.64ms。

用户描述的卡顿在一个客户端时出现，提供地址为 `/ws`。该入口确认是纯移动而非技能服务。排查读取的本机客户端 `293c47b` 为两个模式分别保存 `/ws` 与 `/combat/ws` 地址；尚未核对浏览器实际构建、保存值、帧率或输入到画面时间，因此不能宣称用户当前网页卡顿根因已完全确认或 UI 已修复。当前完成的是服务端改造和协议验收；新页面需按 [SKILL_LAB_HANDOFF.md](SKILL_LAB_HANDOFF.md) 适配。

证据为 `output/skill-lab-smoke-20261112-210045/report.json`、`output/ability-smoke-20261112-210052/report.json`、`output/elemental-smoke-20261112-205303/report.json`、`output/smoke-20261112-210101/report.json`、`output/skill-lab-public-report.json`、`output/skill-lab-public-frames.jsonl`、`output/multiplayer-wire-before.json`、`output/multiplayer-wire-after.json`、`output/unity-iframe-business-report.json` 和服务器私有 `deployment.json/final-audit.json`。未修改 Unity 项目或启动其云构建；未做大规模并发或浏览器画面验收。

## 2026-11-12：移动状态漏推送修复与公网延迟验收

Gateway 源码 `27d3b89507b0b255409b574f8597743bd3688bc0`，镜像 `latency-20261112-v1`。[Linux 发布运行 43197058722](https://github.com/dravenlove/mmo-server/actions/runs/43197058722) 全部成功，包含 Go/race/vet、8 项 CTest、原生与 Docker 验证、MySQL 恢复、独立鉴权、三套 WebSocket 协议与匿名镜像下载。新增真实连接回归用可控定时事件验证一纳秒提前或延迟不会跳过状态，并验证同步频率配置和非法值拒绝。

旧代码用上一份 Ticker 时间加 200ms 判断下一次状态是否到期，会因时间戳微小偏差跳过整次推送。修复为每个定时事件下发一份最新状态；`movement` 默认 20Hz，其余 Profile 保持 5Hz，故障退避保留。游戏 Compose 增加独立 Gateway 镜像标签，配置与回退步骤见 [MOVEMENT_LATENCY.md](MOVEMENT_LATENCY.md)。

已仅更新四个 Gateway。Backend、五个 C++ Scene 共六个容器的 ID 保持不变，继续运行 `elemental-20261112-v1`；Native bridge 二进制与 Caddy 未更新，独立 Auth 仍健康且没有重启。切换时短暂停止三个 WSS bridge 和四个旧 Gateway，完成最终 checkpoint，再启动新 Gateway 和原 bridge。重新开放入口前，9 个角色、8 份战斗快照逐项一致；验收后再次确认原账号、名字、凭据哈希与余额保留，两个受控账号的六键冷却仍在，没有新建账号或未完成迁移。10 个游戏服务健康；私有回退配置和数据核对材料为 `backups/latency-20261112-v1/`。

Windows 客户机、可信 IP TLS、实际 Unity iframe Origin 的公网结果：

| 指标 | 更新前 | 更新后 |
| --- | --- | --- |
| 自动状态频率 | 22 帧约 3.59Hz | 81 帧约 20.00Hz |
| 状态间隔中位 / 最大 | 200.41 / 401.46ms | 49.97 / 52.58ms |
| 移动输入到首份坐标变化状态中位 | 192.08ms | 115.34ms |
| 移动输入到首份坐标变化状态最大 | 452.04ms | 140.40ms |
| 同轮移动回执中位 | 65.88ms | 66.10ms |

移动对比各为八次受控方向输入，消费自动 `state`，没有用主动 `view` 请求催出状态。另以两个账号同时连接测试，输入到首份变化状态中位 99.53ms、最大 137.84ms；另一账号接收的 51 份状态间隔中位 50.18ms、最大 60.81ms。回执快慢和首份变化状态分开统计，不把回执当作坐标已应用的确认。

原 `/ws` 的 10 项 AOI、四输入在途、停止、输入失效、会话隔离回归通过。`/combat/ws` 和 `/elemental/ws` 继续声明原协议，状态间隔中位分别约 200.07ms、200.22ms；三条 HTTPS 健康检查通过。主动查询另有边界：回归脚本频繁发送 `view` 时，回执中位 146ms、最大 447ms；该同步查询与自动推送共用会话循环，本次没有优化此路径。客户端应正常消费 `state`，避免额外高频轮询。

证据为 `output/latency-state-ws-before-20261112.json`、`output/latency-state-ws-after-20261112.json`、`output/latency-input-before-20261112.json`、`output/latency-input-after-20261112.json`、`output/latency-input-after-two-clients-20261112.json`、`output/latency-two-client-state-20261112.json`、`output/unity-iframe-business-report.json` 及服务器私有 `deployment.json` / `final-audit.json`。这些是短时协议采样，未包含浏览器输入节流、渲染或位置平滑，不是大规模并发、浏览器按键到首帧指标。没有修改 Unity 项目或启动 Unity 云构建。

## 2026-11-12：元素试炼场发布与公网验收

源码 `12ed3a8aaf44bd93093bd014b036327bc9f343de`，镜像 `elemental-20261112-v1`。[Linux 发行运行 43181861421](https://github.com/dravenlove/mmo-server/actions/runs/43181861421)全部成功，覆盖 Go/race/vet、8 项 CTest、原生元素 WebSocket 与三个迁移故障恢复场景、SQLite/MySQL 容器持久化和重启恢复、独立鉴权，以及移动/弹道/元素三套协议和四镜像匿名拉取。

游戏主机已升级为 10 个健康容器，均为上述源码版本且重启次数为 0；新增独立 `scene-elemental` / `gateway-elemental`，原有场景继续运行。Native bridge 为同一 SHA，二进制 SHA-256 为 `dbff9fdab31ada4bb48eb6f6416a19eaf01cdf45653541cfd9e83199462c77bc`。三个 WSS 桥接进程、Caddy 与鉴权隧道健康；独立 Auth 保持原镜像，健康且没有重启。

维护时停止入口和 Gateway，保存两个已加载角色的最终权威状态，并备份五张表。恢复入口前逐项核对原 8 个角色、7 份战斗快照完全一致。公网验收后再次核对原角色、名字、凭据哈希与余额一致，账号仍为 8 个、没有未完成迁移；受控角色返回移动场景后保留六键冷却。私有回退材料为 `backups/elemental-20261112-v1/`，含一致 SQL 与配置/程序备份。VPS 下载链路较慢，本次从另一台主机导出已发布镜像，经 SSH 传输并核对归档 SHA-256、平台和镜像源码标签后加载；下载期间原服务持续运行。

Windows 客户机通过可信 IP TLS、实际 Unity iframe Origin，复用两个已有受控账号，在 `/elemental/ws` 完成：

- 握手、登录、协议 ping、每份快照重复能力声明；未知 Origin 返回 403。
- 进入/离开/重开均增加实际归属，登录会话不变；返回自由区恢复位置，同连接移动序号连续。
- 初始六格湿地导电只结算一次；两个走廊敌人各减 32 HP，重复请求不重复伤害，连续施法被冷却拒绝。
- 射程、格界和未知元素正常拒绝且保持连接；水火蒸汽、草地燃烧、水灭火均反映在完整快照。
- 两个个人实例互不伤害；自伤触发失败，终局保持冻结并继续发送状态；重开清零事件、新建战斗，旧归属施法被拒绝。

15 次施法回执中位 47ms、最大 109ms；4 次移动均约 47ms，进入/离开约 313ms，重开约 360ms。场景切换含持久归属事务，与持续移动/施法回执分别统计。此次 50 份公网完整视图（39 份试炼视图、两个角色）通过实际客户端 `GatewayProtocol/PlayerSnapshot/AoiWorld/ElementalBattleSnapshot` 解析器。

同版本原 `/combat/ws` 的弹道 PvP/怪物伤害、重复操作、冷却、墙体挡弹/挡人和重连回归全部通过；原 `/ws` 的 10 项公网移动/AOI/会话隔离回归全部通过。报告分别为 `output/elemental-public-report.json`、`output/elemental-legacy-combat-report.json`、`output/unity-iframe-business-report.json`，服务器留有私有 `final-audit.json`。

本次没有修改 Unity 客户端或触发云构建；以上是服务端、实际 C# 解析器和 Windows 公网协议验证，不代表 WebGL 画面、浏览器按键到首帧或容量已验收。客户端可按 [WEBGL_ELEMENTAL_HANDOFF.md](WEBGL_ELEMENTAL_HANDOFF.md) 切换新入口并继续真实操作联调。

## 2026-11-12：元素试炼场本机接入验证

按 AXIOMANCY `45cde2405bfbed3b2bd814518775159d43f5f636` 元素协议实现独立单人试炼。C++ 元素核心与该客户端 `CombatSimulation.cs` / `LocalElementalAuthority.cs` 生成的 22 组参考快照逐字段比对通过，浮点绝对容差 0.0005；原始参考保存在 `scene/tests/elemental_reference.json`，两份参考源文件与该客户端提交一致。范围包含轴向转换、水火蒸汽、燃烧蔓延、四邻导电、冷却、自伤和终局冻结，另验证合法胜利。

8 项 CTest、Go 全包测试与 vet 通过。新增同进程候选归属替换与墓碑幂等删除，Backend 在场景声明支持后才允许同场景归属轮换；移动队列拒绝跨归属的旧请求，原有 15 项跨进程 smoke 与弹道/墙体 WebSocket 回归通过。

`scripts/smoke_elemental.py` 在临时 SQLite、随机端口、真实 C++/Backend/Gateway 和 WebSocket 中验证能力声明、双账号个人实例隔离、正常拒绝、元素反应、死亡、重开新 ID/新归属、登录会话保持、移动序号连续与退出位置恢复。故障代理另外验证准备响应丢失后的安全撤销、解冻响应丢失加客户端断线后的重新登录恢复，以及归属提交后激活响应丢失的恢复。报告为 `output/elemental-smoke-20261112-160839/report.json`。

服务端生成的 19 份完整视图（14 份试炼视图、两个角色）通过当前客户端独立 `GatewayProtocol/PlayerSnapshot/AoiWorld/ElementalBattleSnapshot` 解析器；没有修改客户端源码或运行 Unity 构建。该结果不代表浏览器真实操作、云构建或线上部署已经验收；发布与公网结果另行记录。联调与持久化边界见 [WEBGL_ELEMENTAL_HANDOFF.md](WEBGL_ELEMENTAL_HANDOFF.md)。

## 2026-11-12：核心战斗 v1 发布与公网验收

源码 `82ec8743e470063a3986eee66879ceec4f296fff`，镜像 `combat-20261112-v1`。[Linux 发行运行 43149692964](https://github.com/dravenlove/mmo-server/actions/runs/43149692964)全部成功，覆盖 Go/race/vet、7 项 CTest、SQLite 与 MySQL 容器持久化/重启恢复、独立 Auth、原移动 WebSocket 与新战斗 WebSocket 双角色集成，以及四镜像匿名拉取。

真实游戏主机已从 6 个服务升级为 8 个，新增独立 `scene-combat` / `gateway-combat`，八容器健康且重启次数为 0，镜像源码标签均为上述完整 SHA。Native bridge 同步该 SHA，二进制 SHA-256 为 `003932275ca69c8c9a13e8ab8a7f28a4dbea3213d2ac6b97755483b7043f52a5`。原 `/ws` 保持 `webgl-movement-v1`；新增 `/combat/ws` 和 `/combat/healthz` 对应 `webgl-combat-v1`，两个桥接进程及 Caddy 均健康。原 Auth 镜像与链路保持运行。

维护前停止入口及 Gateway、保存已加载角色的最终权威状态并制作五表一致 MySQL 备份。首次导出未成功，加入 `--set-gtid-purged=OFF` 后完成备份；未扩大数据库账号权限。重新开放入口前，原 8 个角色和 7 份战斗快照逐项一致。公网验收后再次核对原有角色均存在、名字/凭据哈希/余额一致，账号数没有增加；两个既有受控测试角色在回到移动场景后仍保留六键冷却。私有回退材料在 `backups/combat-20261112-v1/`。

Windows 客户机使用实际 Unity iframe Origin 与系统默认 TLS 校验，公网验收通过：

- 战斗 WSS 握手、协议 ping、既有角色登录、未授权 Origin 403。
- 弹道向其他玩家延迟命中且仅扣 28 HP；相同成功动作重试不重复生成；冷却拒绝不关闭连接。
- 墙体的权威状态、挡弹不扣目标 HP、阻挡角色移动。
- 新会话清理旧对象、战斗序号重置、技能冷却保留；同一弹道对怪物扣 28 HP。
- 原移动入口的 10 项公网协议回归全部通过，包括 AOI、4 条输入在途、停止、输入失效及新旧会话隔离。

本次战斗 6 次施法回执中位 55ms、最大 109ms，15 次移动回执中位 47ms、最大 62ms；原移动入口 54 次移动中位约 49.68ms、最大约 100.91ms。只代表本机到当前服务的短时协议结果，不是浏览器输入到首帧或容量指标。报告为 `output/combat-v1-public-report.json`、`output/unity-iframe-business-report.json` 与服务器私有 `final-audit.json`。测试使用两个既有受控账号，没有读取玩家客户端凭据。

没有修改 Unity 客户端、触发 Unity 云构建或宣称实际 WebGL 战斗 UI 已验收；客户端需按 [WEBGL_COMBAT_HANDOFF.md](WEBGL_COMBAT_HANDOFF.md)适配新入口、技能对象与业务拒绝语义。

## 2026-11-12：核心战斗 v1 本机回归

新增独立 `abilities` 场景与 Gateway Profile，使用 `webgl-combat-v1`、`projectile-wall-v1`。弹道连续碰撞覆盖玩家与怪物，墙体为有向矩形，阻挡移动和弹道；原无战斗移动场景及原四技能场景维持原模式。Backend 接受旧四键和新六键冷却快照，场景跨模式保留新冷却，避免切换入口刷新消耗。

本机 Go 全包测试与 vet 通过；C++ 7 项 CTest 全部通过，新增用例覆盖延迟命中、首目标不穿透、高速扫掠、旋转墙、墙体过期、资源与冷却、幂等重试、会话隔离、断线/租约清理、PvP 死亡复活、关闭 PvP、对象容量和极端快照字节预算。SQLite Backend 回归验证新增冷却经过持久化重启保留，并拒绝不完整或未知冷却键。

`python scripts/smoke_abilities.py` 在随机回环端口、独立 SQLite 和临时进程上运行真实 WebSocket：双角色登录、移动、28 点 PvP 伤害、正常冷却拒绝不关闭连接、墙体挡弹与挡人、重复请求去重、重登清理对象并保留冷却、非法瞄准字段拒绝均通过。报告在 `output/ability-smoke-20261112-113619/`；原 `python scripts/smoke.py` 15 项检查通过，报告在 `output/smoke-20261112-113629/`。

本机 Docker Desktop 引擎不可用，未用该引擎完成本轮容器验收；发行工作流的 Linux 7 项 CTest、Go race、MySQL 及容器回归仍需以本次发布运行结果为准。源码检查不代表线上已升级，也不代表 Unity 已编译或实际 WebGL 操作已验证。客户端接口见 [WEBGL_COMBAT_HANDOFF.md](WEBGL_COMBAT_HANDOFF.md)。

## 2026-11-12：Unity 预览 Origin 与断线诊断

用户从实际 WS 请求头确认来源为 `https://developer.unity3dusercontent.com`，与地址栏的 `https://cloud.unity.com` 不同。修改前，Windows 对该实际来源的公网 WSS 握手明确返回 403；精确增加来源后返回 101，并收到 `webgl-movement-v1` ping 回执。原 IP、原域名与 Unity Dashboard 三个来源继续通过，未授权来源仍返回 403。

Native 转接程序源码 `7adafb5` 增加不含消息、凭据、查询串或客户端关闭说明的连接阶段日志；不符合协议的文本返回 1007，二进制返回 1003，超限返回 1009，Gateway 断开返回 1011。本机 `go test ./...`、`go vet ./...` 通过；新增日志脱敏与关闭码回归，交叉编译的 Linux amd64 转接测试在实际游戏主机连续 5 轮通过。备份旧程序、build.json 和环境后，仅替换 Native 转接程序并更新 Origin；六个游戏容器 ID、启动时间、重启次数与健康状态保持一致。回退材料保存在私有 `backups/ws-diagnostics-20261112-031253/`。

Windows 使用实际预览 Origin、受信任 IP TLS 入口和两个既有受控测试角色完成 10 项协议检查，包括登录、双角色 AOI、4 条移动在途、停止与失联、AOI 离开/返回、新旧会话隔离、断线隐藏和未登录拒绝。59 次移动回执中位约 49.82ms、最大约 97.1ms；110 次视图中位约 54.36ms、最大约 104.9ms。没有创建账号或读取用户客户端凭据。多来源初次复测中的一次握手等待达到 6 秒，未在转接日志中出现对应连接；随后所有来源复测通过，不能据此保证客户端网络无偶发问题。报告为 `output/unity-iframe-origin-check.json` 和 `output/unity-iframe-business-report.json`。

这些是 Windows 协议探针结果；未修改 Unity 客户端或触发云构建，实际 Unity 浏览器的 1006 是否消失仍待用户刷新验证。

## 2026-11-12：IP WSS 与 Windows 公网验收

用户选择先按 IP 联调。游戏主机新增 Caddy IP 站点，显式使用 Let's Encrypt `shortlived` ACME 签发，并配置 NAT 下无 SNI 连接的公网默认名称；WebSocket 允许列表增加该 IP 的精确 HTTPS Origin。游戏程序和镜像保持 `801b88f` / `webgl-20261111-v1`，没有重建游戏容器或修改数据库配置。原域名、原战斗入口和账号存档保留。

- Windows 客户机通过真实公网 IP、禁用测试进程的 HTTP 代理，HTTPS 健康检查 200。TLS 1.3 证书由 Let's Encrypt 签发，SAN 为该公网 IP，系统默认链和地址校验通过；没有关闭验证或安装本地 CA。Caddy 已获取自动续期信息并保持开机启动，尚未经历完整六日续期周期。
- 复用两个已存在的受控验收角色，没有新增账号或读取既有玩家凭据。10 项现场检查通过：错误/缺失 Origin 与查询拒绝、登录到移动场景、双角色 AOI、4 条移动在途和停止、250ms 输入失联、AOI 离开/返回、新会话 fencing、旧连接关闭不影响新连接、正常断线隐藏和未登录移动拒绝。
- 62 次移动回执中位约 49.75ms、最大约 51.61ms；114 次视图中位约 50ms、最大约 101.18ms；三次登录中位约 285.84ms。只代表本机网络的短时协议数据，不是输入到显示首帧或容量结果。
- IP 状态页按当前主机显示 WSS 地址，仍明确等待 Unity 构建产物；没有修改或构建 Unity 客户端，也没有宣称实际 WebGL 按键验收通过。

同日按用户提供的 `https://cloud.unity.com` 增加精确 Origin，保留原 IP 和域名。Windows 对三个允许 Origin 的受信任 IP WSS 握手和 ping 均通过，未授权 Origin 仍返回 403；没有创建账号。该检查模拟请求头，不代替 Unity Cloud 内实际 iframe 的浏览器验收。

私有配置备份位于游戏仓库 `backups/ip-wss-20261112/`；本机记录为 `output/ip-wss-public-report.json` 与 `output/ip-wss-tls-report.json`。实际 IP 在私有交付清单中，公开模板使用占位地址。当前可使用 IP 入口联调，原域名的备案拦截状态仍单独保留。操作与续期要求见 [IP_WSS_DEBUG.md](IP_WSS_DEBUG.md)。

## 2026-11-11：WebGL 移动、AOI 与公网入口限制

服务端源码 `801b88f`，游戏镜像 `webgl-20261111-v1`。[GitHub Linux 发行验收](https://github.com/dravenlove/mmo-server/actions/runs/43029183920)成功，包括 Go/race/vet、6 项 CTest、配置管理工具、SQLite/MySQL 既有战斗与热更恢复回归，以及独立鉴权加 WebSocket 双角色集成；四镜像匿名拉取验证通过。本机原生进程的独立移动世界与 WebSocket 双角色检查也通过。

实际游戏主机运行 6 个新镜像容器：原 Backend、原完整 Gateway、两个战斗场景，以及新增的 movement Gateway / `scene-webgl`；鉴权主机保持原 Auth 镜像。六个游戏容器均健康、无自发重启。新 Gateway 只开放回环 7002，Native WebSocket bridge 仍为回环 7001，Caddy 原域名配置和静态目录保留。转接程序校验和与 build.json 一致。

维护前停止入口、保存两个已加载角色的最终快照，其余三个角色保留已有存档；保存五表静止 MySQL 备份。新核心服务启动后、重新开放入口前，原 5 个角色及 4 个战斗快照逐项对比一致。公网地址验收新增两个独立角色；验收后再次核对原 5 个角色、原战斗快照、余额与凭据哈希仍一致，未完成迁移为 0。没有读取既有客户端凭据。

从广州云主机通过公网地址连接 `wss://gate.draven.world/ws`，证书验证与精确 Origin 检查成功，10 项现场检查通过：错误/缺失 Origin 与查询拒绝、注册登录到独立场景、双角色 AOI、4 条移动在途、零方向停止、250ms 输入失联、AOI 离开/返回、新会话屏蔽旧连接、旧连接关闭不影响新会话、正常断线隐藏及未登录输入拒绝。52 次移动回执中位约 49.78ms、最大约 53.04ms；94 次视图请求中位约 50.56ms、最大约 101.73ms。仅代表这个云主机路径和当前短时负载。

**Windows 普通公网未通过，已经定位到平台备案拦截。** DNS 解析正确，TLS 握手被重置；HTTP GET 返回 302，目标为腾讯云 `dnspod.qcloud.com/static/webblock.html?d=gate.draven.world`，页面指向备案控制台。用户确认域名尚未备案；腾讯云说明未备案/未接入域名会被阻断，HTTPS 也会受影响。[官方说明](https://cloud.tencent.com/document/api/243/18907)。云主机来源的成功不能作为普通公网可用证明。未变更端口或关闭证书验证来绕开拦截。

原完整 TCP 入口在 Windows 公网协议 ping 10/10，中位约 38.02ms，最大约 38.31ms。客户端源码、Unity 引擎构建和 WebGL 画面没有修改或验证；不宣称浏览器接入或按键到首帧已通过，也不将这些短时数据作为容量承诺。

实际配置、原转接程序、Compose 和 MySQL 保存在游戏主机 `backups/webgl-20261111-v1/`；独立验收凭据保存在鉴权主机对应私有测试目录。角色数据不能通过覆盖旧 SQL 盲目回退；回退旧两场景路由前，须用当前 Backend 正常迁移仍归属 `scene-webgl` 的角色。交付协议与备案放行后的复测步骤见 [WEBGL_MOVEMENT_HANDOFF.md](WEBGL_MOVEMENT_HANDOFF.md)。

## 2026-11-11：游戏同机与独立鉴权部署

当前拓扑已经切换为 game（Gateway + Game Backend + 两个 C++ Scene）和 auth（账号注册与凭据验证）。游戏请求通过同机 Docker 网络调用场景，鉴权 HTTP 与 Auth 数据库连接使用受限 SSH 加密链路。客户端源码与程序未修改。

- 发行镜像 `game-auth-20261111-v1`，程序源码 `9fc4941`；实际部署 Compose 包含后续原卷映射修正。GitHub Linux [发行验收](https://github.com/dravenlove/mmo-server/actions/runs/42998286025)成功，包含既有 SQLite/MySQL 回归和分离鉴权 MySQL 的 28 项容器检查，以及四镜像匿名拉取验证。慢鉴权不持有游戏状态锁、旧身份导入、冲突拒绝与账号唯一性有独立测试。
- 迁移前完成 3 个已加载角色的最终权威快照；1 个未加载角色保留原存档。停止原 Backend 与源场景后保存 5 表 MySQL 备份。启动新核心服务、开放玩家入口之前，逐项核对原 4 个角色的身份、余额、位置、会话/归属代次，以及 3 个战斗快照原文，全部一致；Auth 导入匹配 4/4。
- 五个新容器健康，使用非 root 用户和只读根目录；游戏 Backend 保留原 `mmo-two-host-backend-data` 卷并继续连接原 MySQL。原两机的旧业务容器已停止，旧场景隧道已停用。原规则 `default-v1` 文件校验和保持一致，两场景状态一致，新项目管理 CLI 查询通过。
- 新 Auth/数据库链路启用开机启动。正式切换之前重启该链路，Auth 自动检测丢失的数据库会话、重启并恢复健康。该验证不依赖开发机常驻。
- Windows 客户机公网未登录 ping 10/10，中位数 38.34ms，最大 40.77ms。独立验收账号通过公开协议完成注册、分离鉴权登录、权威移动、旋风斩冷却、场景切换/同 ID 重试与重连后的身份、位置和法力保存。成功复核轮次中登录约 259ms，移动回执约 49ms，技能回执约 56ms，迁移约 258ms；20 次视图请求中位数约 50ms、P95 约 101ms。账号凭据仅保存在游戏主机私有备份目录，没有读取既有玩家凭据。
- 同机完整视图在主机和 Gateway 网络命名空间各读取 10 次，20/20 成功，当前测试响应 2320 字节，约 4.93～50.08ms。与旧隧道 5816 字节视图的对象/事件内容不同，不能把两组数字当作等负载对照。约 50ms 的连续请求等待符合当前 20Hz 场景命令批处理周期。

部署参数、镜像和静止数据库备份位于两台 `backups/game-auth-20261111-v1/` 私有目录。验收新增 1 个独立测试账号，后续的角色/快照和迁移记录增量来自该账号；原玩家数据未清空。现场探测记录在忽略的 `output/game-auth-*` 中。

这些是短时真实部署与单个 Windows 公网测点的协议验收，不是全国网络、图形客户端输入首帧、长期可用性或容量承诺。全国线路和加速评估见 [NATIONWIDE_LATENCY.md](NATIONWIDE_LATENCY.md)，两机操作见 [GAME_AUTH_DEPLOYMENT.md](GAME_AUTH_DEPLOYMENT.md)。

## 2026-11-11：跨地域带宽限制与公网 SSH 隧道

两个实际实例的元数据表明它们位于不同地域，用户确认仅使用云联网免费测试带宽。只读查询既有角色的视图，得到 5816 字节响应：原私网从主机读取约 1.94～4.74 秒，从 Gateway 网络命名空间读取约 4.80～5.70 秒。双端 TCP 摘要显示 Scene 已发送的片段在对端丢失并重传；缩小 MSS 到 1200 没有解决。健康、配置等小响应通过，不能代表真实游戏视图及时返回。

腾讯云说明预付费云联网未购买地域间带宽时仅有 10Kbps 测试通信；5.8KB 响应理想传输时间就约 4.7 秒，超过程序的 2 秒 Scene 调用期限。[配置带宽说明](https://cloud.tencent.com/document/product/877/49859)

用户选择保留两台并使用现有公网套餐。我在 gateway 部署专用 SSH 转发服务，严格验证 scene 主机公钥，专用授权限制来源和本地转发目标并禁止交互命令；隧道只绑定 gateway 私网地址，scene 的接口仍绑定私网地址。Gateway/Backend 的路由改为本机转发端口，数据库、内部密钥、卷和数值规则保留；两个 Scene 没有重启，镜像仍为 `transport-20261106-v1`、源码 `71e6d66`。

- 切换后同一 5816 字节视图连续读取 40 次全部成功，最小约 98.48ms、中位数约 149.92ms、最大约 204.82ms；包含主机与 Gateway 网络命名空间。使用最终 systemd 模板后再复核 6 次，约 107.63～155.27ms，均完整读取。
- Backend/Gateway 到两个 Scene 的配置状态读取 12/12，通过；MySQL 只读小响应与 8KiB 响应均约 8ms。
- 四个容器健康，原参数和挂载保持；Gateway、Backend 重建后就绪。公网未登录协议 ping 10/10，单次样本约 40ms。没有创建玩家或读取客户端凭据；视图探测只读，不代替真人移动/技能手感验收。
- 专用服务已启用开机启动；在切换游戏路由前，终止它自己的 SSH 主进程，验证 systemd 自动重新建立连接。专用密钥尝试执行交互命令返回拒绝；原管理登录不受影响。
- 配置、授权文件与停止写入后的 MySQL 快照保存在两台 `backups/tunnel-20261111-6c921e/` 私有目录，原数据库保留。

本机记录包括 `output/view-mss-results-20261111.jsonl`、`tunnel-stability-20261111.jsonl`、`tunnel-final-20261111.jsonl`、`tunnel-runtime-20261111.json`。临时 Windows 中继已停止，正式服务不依赖开发机在线。操作说明和服务模板见[公网 SSH 隧道](SCENE_SSH_TUNNEL.md)。这是短时连通与完整响应验收，不是长期公网质量或容量保证。

## 2026-11-06：内部连接等待与中断诊断

现场客户端报告登录后 `service response interrupted`，随后移动超时。代码确认前者发生在内部 HTTP 响应正文读取阶段，后者发生在取得完整响应头之前；这两条旧客户端消息不足以判定现场根因。

另外独立复现了 Scene 的连接容量缺陷：8 条已完成响应的空闲 HTTP 保活连接占满 8 个工作线程，第 9 条请求超过 2 秒期限。修复让每个内部响应完成后关闭连接并释放工作线程，保留原有线程/队列上限、2 秒 Scene 调用期限及归属、动作序号校验；代价是内部 TCP 建连增加。没有为不确定结果的动作增加自动重放。

- 原生 Release CTest 5/5。新增 HTTP 回归在旧程序第 9 个请求处超时；新程序持有 12 个请求客户端后仍可读取鉴权视图，单次本机样本约 47ms。
- `go test ./...`、`go vet ./...` 通过。新增上游故障检查覆盖响应头超时、正文超时、正文截断、JSON 无效、正常响应，以及 Backend/Scene 分类、日志不含凭据/动作正文、失败 POST 不自动重放。
- 原生基础业务 smoke 15/15，报告为 `output/smoke-20261106-204023/report.json`；独立进程与测试数据库已清理。
- 原生战斗与热更端到端 17/17，报告为 `output/combat-check-e2e87c55b6/report.json`；覆盖同一会话发布、回滚、迁移与重启恢复。
- 隔离原生稳定性样本：6 个 TCP 客户端各运行 30 秒，完成 900 次移动、900 次 ping、900 次显式视图请求并读取 570 次推送；没有请求错误、断连或上游失败日志。合并业务请求 p95 约 57.12ms、最大约 95.49ms。报告为 `output/connection-stability-20261106-204507-8a1c59/report.json`；本次自建的三个服务进程均已停止，未访问实际 VPS。这是本机短时样本，不是公网或容量结论。

Gateway 新增服务、接口、请求/正文/解析阶段、耗时和底层错误日志；客户端兼容原提示。测试工具最初因未消费 POST 正文而无法在断开时结束测试 handler，随后修正为先读完正文并设置有界等待，才计入上述通过结果。真实 VPS 是否命中这个已修复缺陷，仍须现场更新和日志核对；没有把本机复现写成公网根因确认。排错命令和本次完整更新顺序见[连接排错](CONNECTION_TROUBLESHOOTING.md)。

镜像 `transport-20261106-v1` 的[发布运行 42219654315](https://github.com/dravenlove/mmo-server/actions/runs/42219654315)已成功，源码提交 `71e6d66005b8902302564f5eb1194580267dc93f`。GitHub Ubuntu 重新通过 Go 测试与 vet、CTest 5 项、配置工具 24 项、SQLite 容器 26 项、MySQL 集成及 MySQL 容器 27 项，再发布四个 Linux 镜像；使用空认证配置逐个 `docker pull` 并核对源码标签，4/4 成功。匿名检查时本地镜像层已有缓存，验证的是匿名访问及版本一致性，没有把它写成四次重新下载所有镜像层。完整日志副本为 `output/transport-publish-42219654315.log`。本机 Docker Desktop 的版本 API 本轮返回 500，未在本机重新构建或运行 Linux 容器；Linux 运行结果来自上述 GitHub 环境。

## 2026-11-06：桌面移动连续显示

针对低 ping 下移动跳动，确认旧客户端虽用 50ms 定时器重绘，角色坐标与相机仍直接采用低频权威快照。改为按 WPF 组合帧绘制，对玩家、NPC及相机使用独立显示轨迹；重复坐标不重新启动插值，断流不无限外推，场景/身份变化、死亡复活与突进等按权威位置校正。输入变化立即尝试发送，等待移动回执期间合并最新方向；保留原输入心跳。冻结画刷缓存和有界文字缓存减少重复绘制分配。服务器、通信协议、持久化与镜像未改动。

- Release 构建零警告、零错误。
- 核心检查 24/24：原 TCP/TLS、凭据、移动、迁移、奖励等 16 项，加 8 项连续显示检查；覆盖 5Hz 快照在 60Hz 采样下形成连续位置、权威状态不被修改、重复视图、停流、场景/会话/重启、死亡/复活、突进事件去重和 AOI 轨迹清理。
- 最终单文件 exe 的 GUI 检查 16/16：原有业务、战斗、规则版本、连续移动以及同一会话动态热更均通过。
- 实际移动采样约 1603ms、65 次采样中，权威位置变化 5 次，显示位置变化 47 次，采样窗口组合帧统计约 60.0fps；该窗口视图接收约 4.0Hz。最大相邻采样位置变化约 0.38 世界单位；这是本机一次样本，不是硬件帧率保证或公网延迟测试。

报告为 `output/desktop-check-20261106-200920-68b138/report.json` 和 `packaged-smooth-report.json`，截图已复核。另一轮开发 exe 检查为 `output/desktop-check-20261106-200548-3805f6/smooth-ui-report.json`。所有服务使用回环地址、随机测试密钥和独立数据库，测试后已停止；没有注册或修改真实公网玩家。初次测试工具因无交互 stdin 提前清理并记录 EOF，随后使用交互 fixture 完整通过；未把那次 harness 错误当作客户端检查成功。

显示平滑不改变技能命中、碰撞和血蓝，也不会降低真实网络或数据库处理延迟；约一个快照周期的显示滞后是这次插值的边界。服务端仍使用原有推送与存储节奏，本次无需重新构建或部署 VPS 镜像。

## 2026-11-06：数值热更

新增固定文件配置、预检、Tick 边界整体切换、配置版本透传和桌面实时显示。数值发布保留玩家血蓝、剩余冷却、动作序号与缓存；怪物在刷新/脱战重置时整体采用新参数。管理工具通过两个容器内部接口进行预检、CAS、文件原子替换、结果核对及失败补偿，不增加玩家配置管理协议。数据库和四技能快照结构未改动，快照剩余 CD 改用统一 600000ms 上限，兼容缩短技能时长后保存的旧 CD。

| 检查 | 结果 | 覆盖 |
| --- | --- | --- |
| 原生 Release 构建、Go 测试与 vet | 通过 | 新旧场景版本字段、施法回执、uint64 透传及原有业务 |
| CTest | 5/5 | 原场景/战斗/HTTP，加配置状态与固定文件 HTTP；含严格 schema、重复键、NaN、文件损坏/丢失、鉴权、CAS、同名冲突、64 版本边界、旧 CD 恢复与怪物延迟生效 |
| 管理工具隔离故障检查 | 24/24 | 发布、幂等、回滚、节点失败/响应丢失后的补偿、并发 CAS、未知文件/版本不覆盖、env 与秘密保留、原子写及锁；另含旧服务 JSON/纯文本 404 的明确升级提示 |
| 原生战斗 + 热更端到端 | 17/17 | 同一 TCP 会话更新数值，旧技能回执不重复结算，长旧 CD 经重连、跨场景、全服务重启后保留，怪物刷新换参及在线回滚 |
| Linux 镜像构建和导出 | 4/4 | `linux/amd64`，本机 Docker Desktop Linux Engine，标签 `hotreload-20261106-v1`；本地构建时工作区包含未提交修改，manifest 如实记录 |
| SQLite 容器业务 + 热更 | 26/26 | 真实容器和管理 CLI 发布/回滚、目录绑定下原子文件替换可见、重启从已发布文件恢复 |
| MySQL 容器业务 + 热更 | 27/27 | 同样的真实热更/回滚和长旧 CD 恢复，并验证 MySQL 重启后的 Backend 自动恢复 |
| Windows 桌面协议 | 16/16 | 兼容原有连接、凭据、业务、帧边界和证书验证 |
| 最终桌面 exe | 15/15 | 基础业务、战斗/死亡复活、版本与数值提示；保持同一连接/角色/会话接收 demo 更新，核对斩击 36 与恢复技能 5 秒提示 |

原生报告为 `output/combat-check-3234dfe4e3/report.json`，容器报告为 `output/mmo-check-37e307ae/report.json`（SQLite）、`output/mmo-check-ee1db5c0/report.json`（MySQL），桌面报告为 `output/desktop-check-20261106-160223-0ccf75/report.json` 和 `packaged-hot-reload-report.json`。最终 exe 截图已复核，两个临时场景在动态界面检查后恢复原配置；本轮自建的进程、容器和测试卷已清理。早先一轮 GUI 检查在第 12 项之后发生连接中断，原始失败报告保留为 `live-rules-ui-report.json`；随后独立复跑及最终打包 exe 均通过 15 项。现有日志不足确定该次中断原因，没有将旧会话 checkpoint 告警或资源负载直接当作根因；这不是长期稳定性结论。

最终 exe 对实际公网 gateway 的未登录协议 ping 再次通过，单次样本约 64ms，没有创建公网账号；地址只保存在被忽略的交付配置与报告。gateway/scene 两种角色、SQLite/MySQL 两种数据库模式、启用/禁用固定文件的 8 组 Compose 展开通过，核对配置目录只读挂载、数据库卷名不变、Backend 不发布端口及数据库密码仅进入 Backend；89 处相对文件链接通过，报告在 `output/hot-reload-static-check/report.json`。此配置检查没有启动真实部署。

上述检查使用隔离端口、测试库与随机密钥，不访问真实 VPS 的玩家账号或数据库。真实公网热更仍需两台升级后按 [配置热更 Demo](CONFIG_HOT_RELOAD.md)验收。首次安装支持热更的程序需要维护重启；后续数值发布不重启。两场景依次切换，不承诺跨节点同一时刻生效，配置回滚也不撤销已造成的伤害或持久业务效果。

数值热更版 [GitHub 发布运行 42187148036](https://github.com/dravenlove/mmo-server/actions/runs/42187148036) 已成功，镜像源码提交为 `a1023d9`。GitHub 的 Linux 环境重新通过 CTest 5 项、管理工具 23 项、SQLite 容器 26 项、MySQL Backend 集成与 MySQL 容器 27 项检查，再发布四个 `linux/amd64` 镜像。发布后以空 Docker 认证配置逐个匿名拉取并核对源码标签，4/4 通过。完整阶段日志保留于该 Actions 运行；本机副本为 `output/hotreload-publish-42187148036.log`。两台主机模板和当前部署示例随后更新为该已发布标签，已有实际 env 不会被自动改写。

本机额外匿名下载尝试在 Docker Desktop 的镜像创建 API 返回 500 时中止，未计为本机拉取通过；匿名可见性结论使用上述 GitHub Linux 环境实际完成的四次空认证拉取。本机此前的镜像构建、26/27 项容器业务验收及离线包导出均已完成，不能用此后的一次引擎错误替代这些不同范围的结果。

后续现场排错：用户贴出的两个 scene 容器均仍运行 `mvp-20261105-mysql`、源码标签 `72c1717`，该版本没有配置管理接口。管理脚本补充了 HTTP 404 的明确升级提示，JSON 或纯文本响应均可识别，其他 HTTP 错误继续保持原策略；24 项隔离测试通过。此次仅修改宿主机 Python 工具和文档，不需要重建应用镜像；运行时仍应安装已发布的 `hotreload-20261106-v1`。

## 2026-11-06：战斗与技能

新增四种技能、权威血蓝与冷却、被动反击的狼与精英、怪物刷新、角色死亡和复活。战斗快照随原有会话、归属和迁移协议保存；SQLite/MySQL 通过新增 `combat_snapshots` 表兼容原有数据。

| 检查 | 结果 | 覆盖 |
| --- | --- | --- |
| Go 测试与 vet | 通过 | 快照格式、过期归属/会话/版本拒绝、迁移与恢复、成功技能后立即断线的检查点、旧动作回执不覆盖新视图 |
| Release CTest | 3/3 | 原有场景状态与 HTTP 生命周期，新增技能数值、AI、死亡/复活、动作去重和有界视图 |
| 原生基础业务 smoke | 15/15 | 旧登录、移动、奖励、迁移与恢复继续可用 |
| 原生战斗 smoke | 9/9 | 技能/距离/消耗、冷却和重复序号、怪物反击/死亡/刷新、重连/迁移/全服务恢复、角色死亡与幂等复活 |
| MySQL Backend 集成 | 18/18 | MySQL 8.0.46，包含新增战斗快照持久化与所有权隔离 |
| Linux 镜像 | 4/4 | `linux/amd64`，本机 Docker Desktop Linux Engine；战斗标签 `combat-20261106-v1` |
| 容器基础 + 战斗 smoke | SQLite 18/18；MySQL 19/19 | 四服务、真实 CLI 施法、技能完整闭环、检查点和全容器重启恢复；MySQL 另含数据库重启后的 Backend 恢复 |
| Windows 桌面协议核心 | 16/16 | 原协议、加密凭据、分包/帧限制、证书拒绝与基础业务 |
| Windows 桌面最终 exe | 13/13 | 登录/移动/奖励/迁移、选怪与目标面板、四技能、血条和伤害事件、重连保留战斗状态、死亡禁用操作、复活后按钮恢复和实际移动 |
| 发布工作流 | actionlint 通过 | 工作流在推送前执行 SQLite/MySQL 两套容器战斗检查；发布状态以具体 Actions 运行结果为准 |

报告位于 `output/combat-check-6a6a940f2d/report.json`、`output/mmo-mysql-test-c006705de2/report.json`、`output/mmo-check-cdc72861/report.json`（SQLite）、`output/mmo-check-59d177f9/report.json`（MySQL）、`output/desktop-check-20261106-094427-a7fde1/report.json` 和 `packaged-combat-report.json`。所有运行使用隔离端口、测试库、随机密钥和测试角色；自建服务、容器及测试卷已清理。最终 exe 已渲染截图复核，但这不代替用户用物理键鼠进行完整游玩。

最终战斗 exe 另对用户提供的真实公网 gateway 完成未登录协议 ping，样本约 58ms，未注册账号、登录或修改真实玩家数据。实际地址仅保存在被忽略的本机配置和报告中。

战斗版 [GitHub 发布运行 42177116107](https://github.com/dravenlove/mmo-server/actions/runs/42177116107) 已成功，镜像源码提交为 `a49eb62`。工作流重新构建四个镜像，完成 SQLite 18 项和 MySQL 19 项容器检查、MySQL Backend 集成检查后推送到 `ghcr.io/dravenlove/mmo-server/*:combat-20261106-v1`。随后以全新空认证 Docker 配置完成四个镜像的匿名拉取，并核对 `linux/amd64` 与源码提交标签，4/4 通过；报告为 `output/anonymous-pull-combat-20261106/report.json`。两台主机模板随后改为新标签，默认/GHCR、本地与自定义仓库配合 SQLite/MySQL，共 12 组 Compose 展开通过，保留原持久卷名，数据库密码仅进入 Backend。此配置检查未启动真实部署；报告为 `output/combat-release-config-check/report.json`。

这些战斗结果来自本机独立进程和同一 Docker 引擎，不代表真实两台 VPS 已更新或公网战斗已验收。未做并发容量、长时间稳定性或真实跨机故障演练。场景每约 2 秒保存检查点，成功施法/复活与正常断开也尽力保存；硬故障仍可能回退最近检查点之后的变化。技能没有新增持久金币或经验结算，不能将这套快照当成逐技能的持久事务。升级步骤见 [战斗说明](COMBAT.md)。

## 既有基线记录

部署角色命名统一为 `gateway` / `scene`：两个主机模板分别在本地、GHCR、自定义镜像前缀以及 SQLite / MySQL 模式下展开，共 12 组通过；使用相同项目名对比改名前后的服务配置完全一致，数据库卷名保持原值。另以随机项目和独立测试卷运行实际 Docker 项目改名检查，旧项目停止后，新项目继续挂载原命名卷并读取到原有数据。仅清理该测试项目及其测试卷；报告位于 `output/role-config-check-*/report.json`、`output/project-rename-check-*/report.json`。镜像包采用 `mmo-gateway.tar.gz`、`mmo-scene.tar.gz`；历史归档仅调整交付文件名，镜像内容和 SHA-256 保持一致。

2026-11-05 部署文档整合另完成静态验收：README、架构文档和 docs 中的 71 处相对文件/锚点链接通过；本次修改文档里的 73 段 shell 命令仅做语法检查，未执行安装、停机或建库操作。主指南的 scene、gateway 完整 env，以及自定义 MySQL 地址/端口变体，共 3 组 Compose 展开通过，核对镜像标签、私网绑定、健康依赖、数据库卷名和密码只进入 Backend。没有启动服务，也没有连接真实 VPS/MySQL；应用与部署模板未改动，继续使用现有已发布镜像。报告位于 `output/deploy-doc-check-d2230b22/report.json`。

两台 VPS 模板现默认 `ghcr.io/dravenlove/mmo-server/*:mvp-20261105-mysql`，避免复制模板后误拉 Docker Hub 的开发镜像。两份模板在发布、本地 `mmo:dev` 和自定义仓库/标签下共 6 组 Compose 展开通过；数据库默认驱动、卷名和主机角色保持原值。主指南补充镜像名检查，自行构建文档明确本地标签配置；shell 语法通过，未启动容器或访问实际 VPS。报告位于 `output/vps-template-defaults-d50dce74/report.json`。

2026-11-05 新增 Windows WPF 桌面客户端，使用本机 .NET SDK 10.0.403 / Windows Desktop Runtime 10.0.14，Release 编译零警告、零错误。协议核心对隔离的 Go Gateway、Backend 和两个 C++ 场景完成 16/16 检查，覆盖凭据加密与重连、请求/推送分发、移动、奖励幂等、迁移重试、断线、UTF-8 拆包、64KiB 边界与不可信 TLS 证书拒绝；TLS 用例先通过对照握手，避免把故障测试服务误判为安全检查通过。最终单文件 exe 在同一隔离环境完成 6/6 界面业务检查，并渲染截图检查按钮、文字和场景面板；这不是物理鼠标/键盘的完整自动化验收。报告在 `output/desktop-check-20261105-215648-a69e7b/report.json` 和 `ui-packaged-report.json`，测试创建的四个进程已停止。

最终桌面 exe 还从本机对用户提供的真实公网 gateway 完成 TCP 和未登录协议 ping 检查，成功返回游戏响应，桌面程序的协议 ping 样本约 57～104ms；未注册公网账号、未登录或修改真实玩家数据。实际地址只保存于被 Git 忽略的本机交付配置和测试报告。用户另反馈已完成真实 VPS 的容器客户端业务验收；公网桌面角色业务仍由用户用 GUI 验收，公网 TLS、容量、持续稳定性及故障恢复不在此次公网探测范围。

| 检查 | 结果 | 范围 |
| --- | --- | --- |
| `scripts/build.ps1` | 通过 | Windows 本机生成 Backend、Gateway、CLI 和 C++ SceneHost |
| `go test ./...` | 通过 | 数据持久化、并发奖励、会话与归属隔离、迁移故障恢复、网关与 TLS |
| `go vet ./...` | 通过 | Go 静态检查 |
| CTest | 2/2 通过 | 场景状态不变量、真实 HTTP 生命周期与同端口第二实例拒绝 |
| `python scripts/smoke.py` | 15/15 通过 | 四个独立原生进程的端到端测试 |
| Go Linux amd64 / CGO=0 构建 | 三个程序均通过 | Backend、Gateway、CLI 交叉编译；不等于 Linux 运行验证 |
| Compose 配置展开 | 7/7 通过 | 根配置，Backend、两个 Scene 环境、Gateway 分机配置，以及两机 `gateway`/`scene` 配置 |
| Linux Docker 镜像构建 | 4/4 通过 | Docker Desktop WSL2 / Engine 26.1.4 构建 Backend、Gateway、CLI、SceneHost；平台 `linux/amd64`，标签 `mvp-20261104` |
| C++ Linux 编译与运行 | 通过 | Debian Bookworm 容器内 GCC 12.2.0 / CMake 3.25.1 Release 构建，两个场景容器参与实际协议验证 |
| 镜像导出与重新加载 | `gateway`/`scene` 两包通过 | gzip 归档标签、架构、SHA-256、`docker load` 后的镜像 ID 与 UID 10001 均核对；不包含运行密钥 |
| Docker 容器端到端 | SQLite 8/8、MySQL 9/9 通过 | 四服务健康、内部认证、登录/AOI、移动、奖励幂等、迁移与旧归属隔离、容器 CLI、持久化恢复；MySQL 额外覆盖数据库重启后的 Backend 自动恢复 |
| MySQL Backend 集成测试 | 13/13 通过 | MySQL 8.0.46，复用迁移故障恢复与并发测试，额外覆盖相同坐标、Unicode 名字、迁移唯一约束和失去会话后的旧写入者隔离 |
| 跨真实 Linux 主机的网络、容器内 TLS 与恢复演练 | 待目标环境验收 | 部署文档提供具体步骤 |
| Windows 本机 Debug 构建 | 通过 | `prepare-debug.ps1` 生成禁用优化的 Go 程序、C++ Debug 程序及 PDB；不覆盖 Release 输出 |
| Debug 二进制验证 | CTest 2/2、smoke 15/15 通过 | `ctest -C Debug` 与 `smoke.py --bin-dir bin/debug` |
| Go 源码断点 | 通过 | Delve 1.25.2 对 Go 1.22.5 的 Backend 命中 `main.main`，显示源码行和调用栈 |
| VS Code 启动配置 | 字段与运行配置均通过 | 核对 Go 0.58.0 / C/C++ 1.34.4 的 launch 字段；按配置启动四个原生进程，健康、登录、视图与迁移通过（验证数据库单独放在 output）；尚未通过编辑器界面完整点击 F5 验收 |

本机 Debug 的网关健康检查使用 `127.0.0.1:18080`，避开已被其他应用占用的 `8080`。重复运行准备脚本已确认会保留原密钥和 Release 程序，不修改已有数据库。

构建网络选项另有静态与 shell 验证：根 Compose、两机 `gateway`/`scene` Compose 在默认源和自定义源下共 6 组展开通过，确认 `DEBIAN_MIRROR`/`GOPROXY` 传给相应构建阶段，内部服务密钥未传入构建参数。三个 apt 阶段的实际 shell 前缀在隔离目录测试，默认/中科大/清华域名替换、签名与发行版字段保留、超时设置和无效域名拒绝均通过。随后本机 Docker 实际使用中科大源完成镜像构建；这不证明任何 VPS 的网络提速效果。

本次镜像包对应源码提交 `c3aa61a`，构建开始时工作区干净。`gateway` 包为 43,494,175 字节，`scene` 包为 34,261,648 字节；详细镜像 ID、平台及 SHA-256 见交付目录的 `manifest.json`。原始构建日志位于 `output/image-build-*/`。

容器运行验证在 Windows 的 Docker Desktop Linux 引擎中进行，使用隔离 Compose 项目、随机密钥、动态回环端口和独立命名卷，保留配置中的只读根文件系统、非 root 用户及能力限制。真实 CLI 镜像通过 UID 10001 将凭据写入新命名卷；停止并重新启动全部四个容器后，继续使用原凭据登录，场景、坐标、100 金币和奖励领取记录均恢复。验收生成 `output/mmo-check-*/report.json`，完成后仅清理该随机测试项目及其新建卷。此结果不覆盖两台真实 VPS 之间的私网路由、云防火墙或实际证书挂载，也不是 2 核 2 GB 环境的容量测试。

Windows 构建环境为 Go 1.22.5、CMake 3.30.0、Visual Studio 2022 C++ 工具和 Python 3.12。测试没有改动已有业务数据；使用独立临时数据库、随机内部密钥和动态端口。脚本退出时停止自己启动的进程。

两机配置额外核对了 `gateway` 内部 Backend 地址、Backend 不发布宿主端口、`scene` 的两个私网端口、场景 ID、健康依赖、数据卷、构建目录以及空密钥拒绝。该结果是静态配置验证，尚未在两台真实 Linux 主机启动容器。

镜像仓库配置另验证了默认 `mmo`、GHCR、自定义仓库前缀下的全部 7 份 Compose 配置，共 21/21 通过；原来的本地镜像名保持兼容。可重复执行的 `python scripts/smoke_docker.py --tag mvp-20261104` 通过 8/8 项，并清理自己的临时容器和卷。发布工作流通过 actionlint 1.7.12 检查，在 GitHub 构建并完成相同容器验收后才推送镜像；具体发布是否成功以 Actions 实际运行结果为准。

2026-11-05 的 MySQL 支持对应源码提交 `3667839`，本机构建的标签为 `mvp-20261105-mysql`，四个镜像及 `gateway`/`scene` 归档均检查为 `linux/amd64`。Go 测试、`go vet ./...` 与原生 SQLite 四进程 smoke 15/15 通过。默认/自定义地址与端口、两种驱动、三个 Backend Compose 入口共 12 组配置展开通过，确认 MySQL 密码仅传入 Backend 运行环境，不传给场景、网关或镜像构建参数。Compose 输出会将字面 `$` 转义成 `$$`；运行验收使用包含 `$`、`@`、`:`、`/` 的临时密码，验证实际连接成功。

`python scripts/test_mysql.py` 启动本机隔离的 MySQL 8.0.46 并通过 13 项测试，结束后清理自己创建的测试库、容器和匿名卷。`smoke_docker.py --tag mvp-20261105-mysql --database mysql` 通过 9 项真实容器检查：除完整客户端链路外，还验证 MySQL 进程重启释放旧连接后，Backend 在固定会话失效时停止，由容器重启策略启动新进程并重新取得独占锁；玩家身份、场景、金币与领取记录保持不变。另一个 SQLite 容器项目通过 8 项兼容检查。报告位于 `output/mmo-mysql-test-*/report.json` 和 `output/mmo-check-*/report.json`，不包含在源码包或镜像上下文中。

未连接或修改用户指定的 MySQL 实例；其实际版本、私网路由、账号权限、库名和证书仍需目标环境验收。此变更不自动迁移 SQLite 中的旧玩家数据，也不开放 Backend 多副本。

MySQL 版本的 [GitHub 发布运行 41954041853](https://github.com/dravenlove/mmo-server/actions/runs/41954041853) 已成功完成，构建源码提交 `72c1717`。工作流通过 SQLite 容器检查、MySQL Backend 集成测试和 MySQL 容器重启恢复检查后，将四个镜像发布到 `ghcr.io/dravenlove/mmo-server/*:mvp-20261105-mysql`。随后在本机使用全新、空认证的 Docker 配置，匿名拉取四个镜像，逐一核对 `linux/amd64` 平台与源码标签，4/4 通过；报告在 `output/anonymous-pull-mysql-*/report.json`。该网络验证使用本机构建环境，仍不代替目标 VPS 的下载及连库验收。

15 项端到端检查：

1. 四个独立进程健康检查。
2. 内部服务拒绝缺失认证。
3. 注册、名字唯一、token 登录、未登录输入拒绝。
4. 两个客户端的邻近玩家视图。
5. 权威移动、旧序号拒绝、输入范围和自动停止。
6. 顺序与并发领取奖励均只入账一次。
7. 跨场景传送、同 ID 重试、旧归属输入拒绝。
8. 返回原场景，以及同 ID 更换目标被拒绝。
9. 新登录使旧会话失效。
10. 目标不可用时安全失败，目标恢复后可进行新迁移。
11. 网关重启后身份和路由恢复。
12. Backend 与场景重启后保存的位置、金币和领取记录恢复。
13. 第二个 Backend 进程不能打开同一 SQLite 数据库。
14. 真实 CLI 显式大输入序号后可继续自动编号，传送重试 ID 可见。
15. 凭据路径不可写时不提前注册并占用名字。

Backend 单元测试还使用故障注入覆盖“目标准备成功但响应丢失”和“归属提交、目标激活后响应丢失”的恢复。Gateway TLS 测试启动真实 TLS TCP 监听器，验证可信证书可连接、未信任证书被拒绝；该结果不替代实际证书目录权限和容器挂载验证。

重新执行：

```powershell
./scripts/build.ps1
go test ./...
go vet ./...
ctest --test-dir build/scene -C Release --output-on-failure
python scripts/smoke.py
```

`output/smoke-*/report.json` 保存本机测试摘要，同目录有各服务日志。该目录可能含临时账号凭据和测试数据库，已从 Git 和 Docker 上下文排除，不进入交付源码包。测试创建的新账号会消耗所在场景容量，因此这里采用每次新建的独立进程和数据库，不应对有真实玩家的部署直接运行此原生启动脚本。

目标服务器可按 [预构建镜像部署](PREBUILT_IMAGES.md) 加载 `gateway`/`scene` 包，或按 [部署文档](DEPLOYMENT.md) 自行构建。先检查健康与完整客户端流程，再演练停机备份、恢复和场景重启；不能仅用配置展开成功代替实际部署验收。
