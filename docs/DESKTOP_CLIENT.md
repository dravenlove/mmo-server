# Windows 桌面联调客户端

桌面程序直接连接已部署的 gateway，可在窗口中查看角色、怪物和邻近玩家，移动、释放技能、领取奖励与切换场景。战斗版本的玩法和服务端更新见 [COMBAT.md](COMBAT.md)。Windows 本机运行客户端即可；远程服务端继续按 [统一部署流程](DEPLOYMENT_TWO_HOSTS.md)运行。

## 1. 打开并连接

双击 `dist/desktop/MMO.Client.exe`。当前开发机已有 .NET 10 Desktop Runtime，可直接运行已构建的程序；其他 Windows x64 电脑的运行条件见第 4 节。

1. 在“网关地址”填写 `game.example.com:7000`，替换为自己的域名或可达地址。这里填写 `主机:端口`，不带 `http://`。
2. “TLS 加密”开关与 gateway 配置保持一致。网关启用 TLS 时，地址须匹配证书中的域名；证书链由系统验证。
3. 先点“测试连接”。它建立 TCP/TLS 连接并发送 JSON 游戏协议 ping，显示连接与往返耗时，**不注册角色、不登录、不领取奖励**。测试成功表示网关协议可达，业务还需下一步验证。
4. 在“本机角色档案”填写一个新的、尚未使用的角色名，例如 `desktop-demo`，再点“连接并登录”。首次会注册并保存凭据；同一 Windows 用户再次使用相同地址、TLS 方式和角色名时，会读取原凭据登录。

已有命令行客户端的角色凭据不会自动导入。第一次使用桌面程序时请选择新角色名；只填写一个已存在的角色名并不能取得该角色的登录凭据。

## 2. 操作与验收

| 操作 | 使用方法与预期 |
| --- | --- |
| 移动 | 先点击场景区域，再按住 WASD 或方向键；也可按住右侧方向按钮。松开后停止，坐标由服务器更新 |
| 查看邻近玩家 | 场景实时显示自己和 AOI 范围内的玩家；需要两个不同角色同时在线观察 |
| 刷新视图 | 点击“刷新视图”，检查场景、坐标和玩家状态 |
| 领取奖励 | 点击“领取 100 金币”；本角色首次领取增加 100，重复领取不再加钱 |
| 切换场景 | 选择 `scene-1` 或 `scene-2`，点击“切换场景”；切换期间暂停移动，成功后检查场景名 |
| 战斗 | 点击活怪选中，Tab切换目标，地图焦点下按1～4或点击技能栏；服务器判定距离、消耗和冷却 |
| 死亡复活 | 血量归零后停止行动，等待结束再点击复活 |
| 往返延迟 | 右侧显示游戏协议请求的往返耗时，用于观察当前连接 |
| 断线重连 | 连接断开后，保持原地址、TLS 方式和角色档案，重新点击“连接并登录” |

切换失败后，按钮变为“重试同一次切换”，重试会复用原请求编号和目标。需要开始一项新的切换操作时，先断开再重新登录，然后选择目标。窗口底部的“连接与操作记录”会显示结果。

建议按“登录 → 移动 → 重复领取 → 切换 → 断开重连”的顺序验收。已有角色可能已经领取奖励或位于 scene-2，应按它的现有状态判断结果。同一角色凭据只运行一个活动客户端。

## 3. 远程地址、TLS 和凭据

Windows 电脑需能访问 gateway 的玩家入口 TCP 7000。云私网地址通常需先通过 VPN 或受控隧道连接；公网入口使用自己的公网地址或已解析域名。

公网登录前，按 [网关 TLS 配置](DEPLOYMENT_TWO_HOSTS.md#8-开放公网入口时再配置-tls)配置证书。桌面客户端会验证证书链和连接域名，不提供跳过验证的开关。

云安全组只需在 gateway 的入站规则中允许**测试电脑当前公网出口 IP**访问 TCP 7000。scene、MySQL 和 Backend 的内部端口继续在私网使用。公网 EIP 映射到网关主机私网地址时，`GATEWAY_BIND_IP` 可以继续填写网卡上实际存在的绑定地址；公网接入不要求一律改成 `0.0.0.0`。

客户端本地数据位于：

```text
%LOCALAPPDATA%\MmoServerClient\
  settings.json
  credentials\
```

- `settings.json` 只保存地址、TLS 选项和角色名，不保存登录 token。
- `credentials` 按“地址 + TLS 方式 + 角色名”区分档案，登录凭据使用 Windows DPAPI CurrentUser 加密保护。请保留原 Windows 用户的凭据环境，不将它当作通用的跨用户迁移文件。
- 修改地址或 TLS 方式会查找另一个本地档案。连接同一服务器时换域名/IP，也不会自动关联原档案。
- 用户保存的 `settings.json` 优先于 exe 旁的 `client.settings.json`。重新构建后的预填地址没有覆盖旧值时，直接在窗口中修改并保存。

客户端无需填写 `INTERNAL_TOKEN` 或 MySQL 密码。

## 4. 从源码构建和分发

构建需要 Windows x64 环境和 .NET 10 SDK。在项目根目录执行：

```powershell
.\scripts\build-desktop.ps1 -Address 'game.example.com:7000' -Tls -Shortcut
```

示例域名需替换。`-Tls` 仅用于已经启用 TLS 的 gateway；可信私网明文联调时省略它。`-Shortcut` 会在当前用户桌面创建“MMO 测试客户端”快捷方式。

默认输出为依赖运行时的单文件 exe：

```text
dist/desktop/
  MMO.Client.exe
  MMO.Client.exe.sha256
  client.settings.json
```

运行默认包需要 **.NET 10 Desktop Runtime x64**。当前开发机已满足；在其他电脑可安装 [微软官方 .NET 10 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/10.0)，或者在构建时直接包含运行时：

```powershell
.\scripts\build-desktop.ps1 -Address 'game.example.com:7000' -Tls -SelfContained
```

`-SelfContained` 生成的包更大，可交给没有预装 .NET 的 Windows x64 电脑。建议分发整个 `dist/desktop` 目录，保留预填地址和 SHA-256 校验文件；实际服务器地址只写入本机被 Git 忽略的交付配置。[单文件发布说明](https://learn.microsoft.com/en-us/dotnet/core/deploying/single-file/overview)

## 5. 常见问题

| 现象 | 检查方法 |
| --- | --- |
| exe 提示缺少运行时 | 安装 .NET 10 Desktop Runtime x64，或改用 SelfContained 包 |
| 连接超时 | 地址、端口、域名解析、电脑出站网络，以及 gateway 安全组是否允许当前电脑的公网出口 IP |
| 连接被拒绝 | gateway 是否正在运行，玩家入口是否监听并发布 TCP 7000 |
| TLS 握手或证书失败 | TLS 开关是否与服务端一致，证书链是否受信任，地址是否匹配证书域名 |
| 测试连接成功但登录失败 | 查看窗口操作记录；再在服务器检查 Backend/MySQL 和 gateway 到 scene 的私网通信 |
| 角色名已存在 | 使用有本地凭据的原地址/TLS/角色档案，或为桌面测试选择新角色名；CLI token 不会自动导入 |
| 按键没有移动 | 先点击场景使其获得焦点，再按住 WASD 或方向键；也可按住方向按钮 |
| 重新打开仍是旧地址 | 修改窗口中的地址；本机 settings.json 优先于打包预填配置 |

## 6. 开发验证范围

仅运行客户端连接远程 gateway 时，Windows 本机不需要启动服务端。下面的脚本用于协议核心开发验收，需要 .NET 10 SDK、Python 3 和已构建的本机 Go/C++ 服务二进制；服务构建方法见 [本机开发说明](LOCAL_DEBUG.md)。

```powershell
python scripts/test_desktop.py
```

脚本启动独立的本机四服务，使用动态回环端口、临时密钥和隔离 SQLite 数据库，测试后清理本次进程，报告保留在 `output/desktop-check-*`。它不会连接真实部署的 MySQL 或远程服务器。

目前协议核心检查 16/16、本机 GUI 检查 6/6 已通过，具体范围见 [验证记录](VALIDATION.md)。公网链路目前完成了 Windows 本机到网关的 TCP 连接与无账号 ping；公网注册登录、移动、奖励和场景切换业务仍需实际验收。
