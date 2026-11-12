# WebGL 核心战斗 v1 部署

目标标签 `combat-20261112-v1`。游戏主机在原 6 个服务上增加 `scene-combat` 与 `gateway-combat`，仍共用一个 Game Backend。鉴权主机、MySQL 地址、数据库、密钥和旧数据卷保留。先确认发行流水线成功和镜像已发布，再执行拉取；源码更新不等于镜像已经发布。

## 升级顺序

1. 更新源码，保留已有 `deploy/game-host.env`。先在独立环境执行 `go test ./...`、`go vet ./...`、7 项 CTest、`python scripts/smoke_abilities.py`；容器回归使用 `smoke_docker.py --webgl`，含原移动与新战斗协议测试。
2. 维护前备份原 Compose、环境、Native bridge、Caddy 配置和 MySQL。先停止入口与所有 Gateway，让断线位置/战斗快照完成保存，再取得一致的数据库备份。

   本次 MySQL 8 导出使用 `--single-transaction --no-tablespaces --skip-triggers --set-gtid-purged=OFF`，连接信息通过权限 600 的临时配置文件提供，备份权限同为 600。没有为了导出扩大数据库账号权限。不要把真实密码写在命令行或提交到仓库。

3. 新 Backend 可读四键旧冷却与六键新冷却。场景也保留新增冷却，避免角色跨回移动场景或旧战斗场景时刷新技能。需要协调更新整套游戏服务；不能把已产生六键快照的数据库直接交回旧版 Backend/Scene。回退应使用经过兼容验证的程序，或维护窗口内恢复升级前的一致备份。
4. 原 env 只调整镜像标签为 `combat-20261112-v1`，按需添加 `COMBAT_PVP_ENABLED=true`；不要用 example 覆盖真实配置。

```bash
sudo docker compose --env-file deploy/game-host.env -f deploy/game-host.compose.yaml config --quiet
sudo docker compose --env-file deploy/game-host.env -f deploy/game-host.compose.yaml pull --policy always
sudo docker compose --env-file deploy/game-host.env -f deploy/game-host.compose.yaml up -d --no-build --pull never --wait
sudo docker compose --env-file deploy/game-host.env -f deploy/game-host.compose.yaml ps
curl --fail http://127.0.0.1:8084/readyz
```

发布与更新都不在 VPS 上编译。Scene 的 9004、Gateway 的 7003/8084 和 bridge 的 7004 只绑定回环地址，不新增公网端口。

## 战斗 WSS

使用与源码同版本构建的 `/opt/mmo-websocket/websocket-bridge`；更新前备份旧程序与 build.json。保留原 `/etc/mmo-websocket.env` 和 `mmo-websocket.service`，从 [战斗环境模板](../deploy/websocket-combat.env.example) 新建 `/etc/mmo-websocket-combat.env`，将示例 IP 改为真实 IP，精确继承已有网页 Origin，权限 600。

从 [服务模板](../deploy/mmo-websocket-combat.service.example) 安装 `/etc/systemd/system/mmo-websocket-combat.service`。它运行同一程序，但使用独立环境与 7004 端口，声明 `WS_PROTOCOL_VERSION=webgl-combat-v1`，上游为本机 7003。

Caddy 在原 IP/域名站点内加入两条路由，参照 [IP 站点模板](../deploy/ip-webgl.caddy.example)：

```caddyfile
handle /combat/ws {
    rewrite * /ws
    reverse_proxy 127.0.0.1:7004
}
handle /combat/healthz {
    rewrite * /healthz
    reverse_proxy 127.0.0.1:7004
}
```

保留原 `/ws`、`/healthz`、TLS 和静态网页。使用实际服务的环境变量验证 Caddy 配置，再启动战斗 bridge、重载 Caddy；不要关闭 TLS 验证或重新申请无关证书。

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now mmo-websocket-combat.service
sudo systemctl reload caddy
curl --fail http://127.0.0.1:7004/healthz
```

从真实客户网络验证新的 HTTPS 健康检查、精确 Origin、ping 协议、登录、弹道伤害及墙体。原移动入口另做回归。不要对线上直接运行会创建隔离测试账号和数据库的原生 smoke 脚本。

客户端字段、拒绝语义和验收步骤见 [联调文档](WEBGL_COMBAT_HANDOFF.md)。两个新技能的规则目前为代码常量，不通过旧四技能配置热更接口变更。
