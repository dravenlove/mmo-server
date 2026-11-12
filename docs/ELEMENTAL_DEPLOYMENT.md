# 元素试炼场部署

目标镜像 `elemental-20261112-v1`。游戏主机共 10 个服务，在原 8 个上新增一个 `scene-elemental` 和一个 `gateway-elemental`。自由区与个人试炼场是同一 C++ 进程的两个逻辑场景，每个角色独立实例；仍由单个 Game Backend 持久化归属。

## 发布与验证

先完成发行流水线，再拉取镜像；不在 VPS 上编译。发布检查包含 8 项 CTest，其中元素测试与客户端参考实现的 22 组快照比对，并验证胜负、暂停、归属轮换与位置恢复。`smoke_docker.py --webgl` 同时回归移动、弹道/墙体与元素三个协议。

本机可执行：

```powershell
./scripts/build.ps1
go test ./...
go vet ./...
ctest --test-dir build/scene -C Release --output-on-failure
python scripts/smoke_elemental.py
```

最后一个脚本只使用临时进程、随机回环端口和独立 SQLite；不会默认连接线上。

## 游戏主机升级

保留已有 env、密钥、数据库、卷和鉴权链路。维护前备份 Caddy、三个 bridge 的配置/程序，以及角色、战斗快照和迁移日志；停止入口和所有 Gateway，完成最终权威快照后停止 Backend，再做一致 MySQL 备份。MySQL 8 导出沿用 `--set-gtid-purged=OFF`，凭据只通过权限 600 的私有配置文件传入。

新增 Backend 场景映射已在 `deploy/game-host.compose.yaml` 中：

```text
scene-elemental-world=http://scene-elemental:9005
scene-elemental-arena=http://scene-elemental:9005
```

将原 env 的镜像标签更新为新版本，先拉取，再协调启动；不要用 example 覆盖真实配置。9005、7005、8085、7006 都只绑定回环，无需新增公网端口。

```bash
sudo docker compose --env-file deploy/game-host.env -f deploy/game-host.compose.yaml config --quiet
sudo docker compose --env-file deploy/game-host.env -f deploy/game-host.compose.yaml pull --policy always
sudo docker compose --env-file deploy/game-host.env -f deploy/game-host.compose.yaml up -d --no-build --pull never --wait
sudo docker compose --env-file deploy/game-host.env -f deploy/game-host.compose.yaml ps
curl --fail http://127.0.0.1:8085/readyz
```

新增 `scene-elemental` 使用 `SCENE_MODE=elemental`，固定 20Hz；`gateway-elemental` 使用 `GATEWAY_PROFILE=elemental`，自由区/试炼场分别由 `GATEWAY_LOGIN_SCENE` / `GATEWAY_ARENA_SCENE` 指定。

## WSS 入口

升级 Native bridge 后保留原移动、弹道 bridge；复制 [元素环境模板](../deploy/websocket-elemental.env.example) 到 `/etc/mmo-websocket-elemental.env`，权限 600，按当前真实网页继承精确 Origin。复制 [服务模板](../deploy/mmo-websocket-elemental.service.example) 到 `/etc/systemd/system/mmo-websocket-elemental.service`。

在原 Caddy IP/域名站点内增加：

```caddyfile
handle /elemental/ws {
    rewrite * /ws
    reverse_proxy 127.0.0.1:7006
}
handle /elemental/healthz {
    rewrite * /healthz
    reverse_proxy 127.0.0.1:7006
}
```

配置模板见 [ip-webgl.caddy.example](../deploy/ip-webgl.caddy.example)。保留 TLS、原两条 WSS 路由及静态网页，验证 Caddy 配置后重载并启动新 bridge。元素健康检查应声明 `webgl-elemental-v1`。

恢复入口前逐项核对原角色、账号、余额和旧战斗快照。公网验收复用受控账号，不在终端、文档或聊天打印 token。线上不要直接运行会新建测试库/账号的 smoke 脚本。

新逻辑场景的归属会持久化在角色表；回退到不认识这些场景的旧版本前，需要先迁出角色并验证路由兼容，或在维护窗口恢复升级前一致备份。不能只改旧镜像标签后启动。

客户端接口与重连边界见 [WEBGL_ELEMENTAL_HANDOFF.md](WEBGL_ELEMENTAL_HANDOFF.md)。
