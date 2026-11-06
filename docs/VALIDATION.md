# 验证记录

日期：2026-11-04 基线；2026-11-05 增加 MySQL 和桌面客户端；2026-11-06 增加 PvE 战斗验证。以下结果不等于容量或生产可用性承诺。

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
