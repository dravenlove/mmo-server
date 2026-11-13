# 2026-11-13 服务端与真实 C# 客户端联调

本轮按 [参数化技能协议](SKILL_LAB_HANDOFF.md)、[房间接口](ROOMS_LUA_ECS.md) 和客户端 `docs/SKILL_LAB.md` 执行。测试分开记录公网旧部署、新 C++ 隔离服务和真实 Unity 浏览器三个边界；未切换 VPS、未修改 Unity 源码、未启动云构建。

## 版本与环境

| 验证对象 | 实际版本 |
| --- | --- |
| 公网 Gateway / Backend | `account-20261113-v1`，`674a5502f18671434f1162afa86a448990517510` |
| 公网技能 Scene | `skill-lab-20261112-v1`，`0c48fbe15d6548fce6411591911b51f5311c68ce` |
| 公网 Auth | `account-auth-20261113-v2`，健康检查通过 |
| 新 C++ 服务 | 源码 `91f4800c8a13d0be3a08e262238334f54869467c`，本机独立 Ubuntu 24.04 / Debug 二进制；临时 SQLite、端口和账号 |
| 真实客户端网络层 | `dravenlove/AXIOMANCY` / `36681c40edc6d67c339e7427a78b413d8a05c497`，实际 `GatewaySession/AoiWorld`，.NET 10 |

公网既有服务没有重建或重启；复查 Backend、技能 Gateway、技能 Scene 与移动 Scene 仍健康，重启计数 0。新镜像发布与 Docker 验证另见 [VALIDATION.md](VALIDATION.md)。本轮本机 C# 对接的是上述相同源码的独立进程，不把它写成新的镜像或 VPS 验收。

## 公网既有账号与技能

入口 `wss://49.233.181.76/skills/ws`，可信 TLS，Origin 为实际 iframe 来源 `https://developer.unity3dusercontent.com`。使用两个已有受控账号，执行客户端现有手动工具：

```powershell
dotnet run --project Tests/Networking/Networking.csproj --configuration Release -- --skill-lab-live "PATH_TO_PRIVATE_CONTROLLED_ACCOUNTS.json" "wss://49.233.181.76/skills/ws"
```

以上命令在客户端仓库运行；不在普通 CI 中运行。凭据文件只由程序读取，禁止提交或输出内容。程序遇到其他可见玩家会停止攻击，结束/失败时尝试返回移动入口。

实际通过：

- 两个已有身份登录、共享场景、移动；同技能 ID 分别保留 17 / 5 点伤害及各自参数。
- 双端观察同一服务端弹道实例；目标权威 HP 减少 17。定义更新后伤害变成 23；幂等初始化不变更版本，旧期望版本正常拒绝。
- 墙体尺寸、移动阻挡、弹道阻挡、到期清理；重登录保留同进程定义和冷却并清理旧实例。
- 两角色均返回移动场景；注册请求为 **0**。本次单发测量 ack **78.69ms**、首份含实例状态 **128.57ms**，不是延迟分布、浏览器绘制或输入到屏幕测量。
- 另复用一个已有密码测试账号，四种公网入口的密码取票据、login、完整 view 均通过；错误密码返回 401。没有新注册，结束后同样返回移动场景。

非敏感原始报告位于本机忽略目录 `output/csharp-public-integration-20261113.json` 与 `output/account-public-report.json`，不依赖这些私有文件才能阅读本文。四入口密码测试含 bcrypt 和跨机 Auth 链路，不能用登录耗时推断移动/技能实时延迟。

## 新服务隔离联调

执行公开脚本：

```bash
python3 scripts/test_room_stack.py --build /root/mmo-build
```

本轮重新通过三账号、双房间、准备/房主开局、独立弹道、脚本新旧版本并存、Lua 拒绝不扣资源、完整服务重启保留身份/100 金币/归档脚本及结束后回大厅。该脚本自行建立临时账号与 SQLite，不读取真实部署 env。

真实 C# 另外通过本机回环 WebSocket 连接新 C++ Gateway → GameService → Auth → Worker。客户端工作区同时有其他修改，因此导出上述固定提交源码单独编译，不链接活动未提交代码。测试外部胶水发送 room 命令；登录、状态解析、参数化技能初始化/施放仍执行原客户端代码。临时凭据不用于公网。

最终在全新临时服务/数据库/双账号上运行通过 7 项：默认场景登录与状态、默认弹道/墙体双端实例、动态房间 ID/归属/附加元数据解析、房内 Lua/ECS 弹道/墙体双端实例、同房重登、旧世界缓存边界复现、关闭旧 Session 后原身份返回原大厅。原客户端源码没有修改；房间控制由外部测试胶水实现。报告 `output/native-client-integration/report.json` 为 `ok:true`，客户端注册请求为 0；该环境的两个合成账号仅由临时夹具事先创建。独立服务已停止，临时数据库与凭据已清理。

## 必须由客户端组房适配处理的边界

现有客户端没有组房 UI/API。默认兼容场景通过，不等于原界面已有完整房间支持：

1. **等待与结束阶段**：暂停游戏输入和移动心跳，先排空已发送输入，保留连接心跳、状态和房间控制；否则未开局移动的 409 会被当前 `GatewaySession` 视为连接故障。
2. **回到已访问的世界**：当前 `AoiWorld` 会丢弃已退休 world_epoch。离房返回仍在运行的原大厅可能复用这个 epoch；外部适配需以新 `GatewaySession` 重新登录、清空旧世界缓存。正式 API 应把可信的所有权迁移回执和世界清理整合，不能简单关闭旧状态围栏。
3. **Worker 重建**：服务端旋转 session_epoch，当前客户端不会透明接受。关闭旧连接并用已有凭据重新 login 后，再读新状态、初始化技能、准备和开局。禁止重放旧施法。
4. **结束后离开**：Backend 可能已通过周期 session 把角色送回大厅。此时带旧 room_id 的新 leave 正常返回 409；可先读当前路由，或省略 room_id 发幂等 leave。若 Backend 已迁回但 Gateway 尚未采用新 owner，仍可能暂时返回 `room owner changed`，应等待路由刷新、复用相同业务 request_id 有界重试，而不是重复创建迁移。最终以当前场景及新会话状态确认。

这些要求没有通过更改客户端或放松服务端鉴权绕过。新增本地地图、元素联动、组合资产、地形墙与视野是另一套客户端本地能力，当前服务端未交付对应扩展，不能混入弹道/墙体及房间通过结论。

尚未验收真实 Unity/WebGL 页面、键鼠/多点触控、画面和输入到屏幕延迟；这需要相应客户端适配及当前版本构建，不能用本轮 .NET 联调代替。
