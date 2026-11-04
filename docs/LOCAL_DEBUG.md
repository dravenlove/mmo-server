# Windows 本机调试

本机直接运行四个原生进程：Gateway、Backend、scene-1、scene-2。它们仍通过 TCP/HTTP 通信，因此可以在一台 Windows 电脑上调试跨服务调用。默认全部监听 `127.0.0.1`，不需要 Docker 或 VPS。

## 1. 首次准备

需要 Go、CMake、Visual Studio 2022 的“使用 C++ 的桌面开发”工具，以及 VS Code 的 Go（`golang.Go`）和 C/C++（`ms-vscode.cpptools`）扩展。Python 3 用于自动验证。已有工具无需重复安装。

在项目根目录打开 PowerShell：

```powershell
# 已有本项目时无需重新 clone；其他电脑可从 GitHub 克隆。
Set-Location C:\Users\admin\Desktop\game_sever
code --install-extension golang.Go
code --install-extension ms-vscode.cpptools
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\prepare-debug.ps1
code .
```

`ExecutionPolicy Bypass` 仅对这次脚本进程生效，不修改系统执行策略。`prepare-debug.ps1` 首次创建随机内部密钥，以后复用；它不会打印密钥或修改 VPS 配置。关闭优化的调试版单独放在 `bin/debug/`，保留原有 `bin/` Release 文件。

| 内容 | 本机位置 |
| --- | --- |
| Debug 程序与 C++ PDB 符号 | `bin/debug/` |
| 四个服务共用的随机密钥 | `build/local-debug.env` |
| 本机调试数据库 | `data/local-debug/mmo.db` |
| 调试玩家登录凭据 | `tokens/local-debug/` |

这些运行文件都被 Git 忽略，不会进入公开仓库。Go 调试器 Delve（`dlv`）需要和本机 Go 版本兼容；扩展找不到它时，可以使用 VS Code 命令面板的 `Go: Install/Update Tools` 安装。已有可用版本无需更新，不要为绕过不兼容错误关闭版本检查。

## 2. 一起启动四个服务

在 VS Code 中打开**项目根目录**，按 `Ctrl+Shift+D` 进入“运行和调试”，从下拉框选择 **`MMO: all services (Windows)`**，按 **F5**。

它会先构建 Debug 版本，再启动四个调试会话。等待调试控制台出现监听信息；可从控制台下拉框选择具体服务。首次应先手动执行第 1 节的准备命令，让环境文件在扩展解析配置前存在。

在另一个 VS Code PowerShell 终端验证：

```powershell
Invoke-RestMethod http://127.0.0.1:9001/healthz
Invoke-RestMethod http://127.0.0.1:9002/healthz
Invoke-RestMethod http://127.0.0.1:8081/healthz
Invoke-RestMethod http://127.0.0.1:18080/readyz
```

没有配置服务端断点时，这些请求应成功。Gateway 的 `readyz` 只覆盖到 Backend 的连通，场景仍需分别检查。

本地调试使用玩家端口 `7000`、Backend `8081`、场景 `9001/9002`，网关健康检查单独用 `18080`，避开这台电脑已经被其他程序占用的 `8080`。Docker 部署配置的健康检查端口仍是 `8080`。

如只想启动一个服务，也可选择四个单独配置。单独配置不自动重新构建，先运行 `prepare-debug.ps1`；其他依赖服务仍需启动。不要重复启动同一个配置，或同时启动占用这些端口的 Docker/原生服务。

## 3. 用客户端触发代码

在项目根目录的另一个终端运行：

```powershell
.\bin\debug\client.exe -addr 127.0.0.1:7000 -name alice -token-file .\tokens\local-debug\alice.token
```

第一次会注册并保存登录凭据；以后使用同一路径重登。输入：

```text
view
move 1 0
transfer scene-2
claim
claim
quit
```

第一次 `claim` 增加 100 金币，第二次不再增加。`move` 是方向输入，超过 250ms 没有新输入便停止；它不是无限持续移动。再开一个终端，用 `-name bob -token-file .\tokens\local-debug\bob.token` 登录，可以观察同场景的附近玩家。

不要让两个客户端同时复用 Alice 的凭据，否则新会话会使旧连接失效。

## 4. 从这些断点开始

先在对应代码行左侧点一下设置断点，再在客户端执行对应命令：

| 想观察的流程 | 断点位置 | 客户端操作 |
| --- | --- | --- |
| 网关如何分派玩家命令 | `internal/gateway/server.go` 的 `switch req.Op` 内 | `view`、`move`、`claim` |
| 奖励为何只能领取一次 | `internal/backend/server.go` 的 `(*Server).claim` | `claim`，然后再执行一次 |
| C++ 如何接收移动输入 | `scene/src/scene.cpp` 的 `Scene::execute`，`/v1/move` 分支 | `move 1 0` |
| 跨场景归属如何变更 | `internal/backend/transfer.go` 的 `(*Server).transfer` | `transfer scene-2` |

Go 用 `-gcflags=all=-N -l` 关闭优化与内联并保留路径，C++ 使用 CMake 的 Debug 配置并保留 PDB。配置依据 [Go 扩展调试说明](https://github.com/golang/vscode-go/wiki/debugging) 和 [C/C++ 调试配置说明](https://code.visualstudio.com/docs/cpp/launch-json-reference)。

断住后在左侧查看变量与调用栈：**F10** 单步跳过，**F11** 进入函数，**F5** 继续。两个 C++ 场景共用同一份源码，断点可能在任一场景命中；在调用栈里看当前是 Scene 1 还是 Scene 2。

一个进程暂停时，其他进程的网络超时计时仍在继续；网关会定期查询场景和会话。长时间停在断点上可能触发超时、断线或恢复流程，这不代表业务本身一定有错。先从一次请求的快速单步开始；需要长时间观察数据算法时，优先调试对应单元测试。继续执行后如连接失效，用原 token 文件重登；迁移超时用同一个 `request_id` 重试。

## 5. 改代码、重新运行和停止

修改后先让客户端 `quit`，再停止四个调试会话，重新选 `MMO: all services (Windows)` 按 F5，会重新编译。不要在程序运行时覆盖对应 exe；Windows 会阻止写入使用中的文件。

组合配置的 `stopAll: true` 会在停止任意成员时停止整个组合。调试器的停止操作可能直接终止进程，适合本机开发，不代替部署文档的有序停机；角色位置可能回退到最近一次快照。数据库和 token 文件会保留，可继续重登。

想重新开始一批测试时，先停止全部调试进程，再将 `data/local-debug` 和 `tokens/local-debug` **一起改名备份**，重新执行准备脚本。仅换数据库而保留旧玩家 token，会导致找不到账号；不需要改动其他 `data/` 或 VPS 数据。

## 6. 自动验证与常见问题

准备 Debug 程序后，可在不打开编辑器的情况下运行：

```powershell
go test ./...
go vet ./...
ctest --test-dir build/scene -C Debug --output-on-failure
python scripts/smoke.py --bin-dir bin/debug
```

`smoke.py` 自行选择端口、创建临时数据库、启动四个进程，执行完成后退出；它用于检查完整流程，不会留下可手动交互的服务器。测试数据库与日志在 `output/smoke-*/`，不会使用上述调试数据库。

- **F5 没有 Go/C++ 调试器**：检查对应扩展是否安装并启用，打开的是整个项目文件夹。
- **找不到 `local-debug.env` 或 exe**：先手动执行 `prepare-debug.ps1`。
- **端口被占用**：检查之前的调试进程或容器；只停止你确认属于本项目的实例。
- **数据库已被打开**：同一个调试数据库只能运行一个 Backend。
- **Go 空心断点或局部变量缺失**：确认选择的是本项目 Debug 配置，修改后已重新编译。
- **C++ 断点未加载**：确认运行 `bin/debug/scene-server.exe`，相邻存在新编译的 `scene-server.pdb`。
