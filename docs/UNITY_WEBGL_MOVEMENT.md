# Unity WebGL 移动与 AOI 接入

客户端源码：[dravenlove/AXIOMANCY 的 feat/network-movement-aoi](https://github.com/dravenlove/AXIOMANCY/tree/feat/network-movement-aoi)。本分支为现有 TCP Gateway 增加独立 WebSocket 转接程序，复用 register/login/move/view/ping 协议，不修改 SceneHost 的权威移动、速度、AOI 或战斗规则。

## 已部署的入口

- WebSocket：`wss://gate.draven.world/ws`
- 健康检查：`https://gate.draven.world/healthz`
- 本机 bridge：`127.0.0.1:7001`
- 原 Gateway 同机地址：`10.2.0.11:7000`，公网映射为 `49.233.181.76:7000`
- 静态网页目录：`/srv/axiomancy/`。目前只有明确标注等待 Unity 产物的状态页，没有声称已构建 Unity 客户端。

Caddy 负责 HTTPS 证书和 WebSocket Upgrade 转发。原 Gateway / Backend 容器保持运行，没有升级其镜像或改写现有服务端工作目录。

## 实现

`cmd/websocket-bridge` 为独立进程；`internal/wsbridge` 将每条 WebSocket JSON 文本压缩空白并追加换行交给固定 TCP 上游，将上游每行作为一条文本返回。每个浏览器连接对应一个独立 TCP 连接，身份认证仍由原 Gateway / Backend 完成。

消息限制 64 KiB，只接受 JSON 对象和文本帧；Origin 使用明确允许列表；不接受 URL 查询参数，凭据不得放进 URL。桥接层只有一个 WebSocket 数据写入者，Ping 用并发安全的控制帧接口。关闭桥接服务会主动关闭已 Upgrade 的连接，不依赖 HTTP Shutdown 自动处理。

参数 / 环境变量：

| 参数 | 环境变量 | 默认 |
| --- | --- | --- |
| `-listen` | `WS_LISTEN` | `127.0.0.1:7001` |
| `-upstream` | `WS_UPSTREAM` | `127.0.0.1:7000` |
| `-origins` | `WS_ALLOWED_ORIGINS` | `http://127.0.0.1:8088,http://localhost:8088` |
| `-upstream-tls` | `WS_UPSTREAM_TLS` | `false` |

公网部署只允许 `https://gate.draven.world`。同机上游走已有明文回环/私网 TCP；启用上游 TLS 时验证证书与主机名，不提供跳过验证选项。TLS 终止在 Caddy，bridge 自身绑定回环，不开放公网 7001。

## 构建与部署

```sh
go test ./...
go vet ./...
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath -ldflags='-s -w' -o websocket-bridge ./cmd/websocket-bridge
```

把程序与 `deploy/mmo-websocket.service`、`deploy/gate.draven.world.caddy`、`deploy/install-websocket.sh` 放到部署暂存目录，按真实上游绑定地址调整 service，再执行：

```sh
sudo sh deploy/install-websocket.sh /你的部署暂存目录
```

脚本备份原 Caddyfile，添加一个域名 include，安装新的 systemd 服务。它不操作已有 Gateway / Backend 容器。部署位置：

| 内容 | 路径 |
| --- | --- |
| 程序 | `/opt/mmo-websocket/websocket-bridge` |
| 服务 | `/etc/systemd/system/mmo-websocket.service` |
| 域名配置 | `/etc/caddy/conf.d/axiomancy.caddy` |
| Caddy 备份 | `/etc/caddy/Caddyfile.before-axiomancy-20261111` |

`DynamicUser`、只读系统和低权限限制由 service 配置。也可按 `deploy/websocket.compose.yaml` 单独运行转接容器；这个镜像没有自动加入原有四镜像发布流程，不应假定它已发布到 GHCR。

## 检查与真实联调

bridge 检查覆盖 JSONL 边界、Origin 与查询参数拒绝、二进制/超限关闭、连接关闭及配置校验。所有原 Go 测试与 vet 也通过。

可显式启用真实服务器集成，启动一个有时间限制、自动清理的本机 bridge fixture，并执行 Unity 同一份 C# 网络核心：

```powershell
$env:MMO_LIVE_GATEWAY='49.233.181.76:7000'
$env:UNITY_CLIENT_PROJECT='你的 AXIOMANCY 路径'
go test ./internal/wsbridge -run TestLiveUnityClient -count=1 -v -timeout 100s
```

此项会创建/复用两个专用角色，凭据保存在客户端仓库的忽略目录 `.local/`，不输出到日志。未配置这两个环境变量时，日常 `go test ./...` 不连接公网服务器。

2026-11-11 已通过本机转接与公网 WSS 的真实链路：两角色登录、方向移动、停止回执后坐标稳定、AOI 进入/离开/返回、同凭据重连与新会话序号。Unity 引擎编译和 WebGL 操作仍需用户手动构建。现有 Gateway 逐连接串行处理及 SceneHost 调用会影响操作时延，此转接层不消除原有服务端延迟。

## 运维与回退

```sh
sudo systemctl status mmo-websocket --no-pager
sudo journalctl -u mmo-websocket -n 40 --no-pager
curl --fail https://gate.draven.world/healthz
```

需要回退时，停止新的 `mmo-websocket` 服务，把域名 include 改为不匹配 `.caddy` 的后缀，再校验并 reload Caddy。这样可以单独撤下新入口，无需停止旧 TCP 游戏服务。重新使用旧程序时，可从 `/opt/mmo-websocket/websocket-bridge.previous` 恢复；首次部署没有旧程序备份。
