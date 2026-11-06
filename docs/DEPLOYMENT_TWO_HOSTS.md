# 统一部署流程：gateway + scene + MySQL

这份文档是两台应用 VPS 的部署入口。主流程使用已经发布的 Docker 镜像和已有 MySQL，VPS 只拉取、启动，不进行 Go/C++ 编译。

- [首次部署](#fresh)：按第 1–7 步完成私网联调。
- [已有部署更新](#updates)：保留原配置、密钥和数据，按维护窗口更新。
- [停止与恢复](#operations)：日常操作顺序。
- [常见问题](#troubleshooting)：按具体症状定位。

## 先确认主机和当前状态

| 角色 | 机器 | 运行内容 | 配置文件 |
| --- | --- | --- | --- |
| `gateway` | Ubuntu 24.04.4 LTS，2GB 内存 | Go Gateway、Go Backend | `deploy/gateway-host.env` |
| `scene` | OpenCloudOS 9.6，4GB 内存 | C++ scene-1、scene-2 | `deploy/scene-host.env` |
| `mysql` | 已有 MySQL 服务 | 账号、金币、角色快照、迁移记录 | 地址和账号填在 gateway 的 env 中 |

下文使用可替换的示例地址：`gateway=10.20.0.11`、`scene=10.20.0.22`、`mysql=10.20.0.30:3306`。部署时替换为实际地址；角色名称本身不会创建 DNS 记录。

当前发布版本：`mvp-20261105-mysql`，镜像前缀：`ghcr.io/dravenlove/mmo-server`。两台使用相同版本。现有镜像为 `linux/amd64`，目标机 `uname -m` 应输出 `x86_64`。

两份主机模板已默认使用上述 GHCR 镜像。已有实际 env 会保留原值；更新代码后仍需检查自己的 `MMO_IMAGE_PREFIX` 和 `MMO_IMAGE_TAG`。启动前的 `config --images` 只输出镜像名，应与上述发布版本一致。

先在两台查看现有项目：

```bash
sudo docker compose ls
```

- 尚未启动服务：进入下面的首次部署。
- 已有 `mmo-gateway` / `mmo-scene`：使用[更新流程](#updates)，不要覆盖原 env。
- 已使用旧文件名或旧项目名运行：先完成[配置改名迁移](DEPLOYMENT_NAMING_MIGRATION.md)，然后按本文继续。只更新命名无需重新构建镜像。
- 原来使用 SQLite 且需要保留玩家：先备份并安排数据迁移；改成 MySQL 连接参数不会自动复制旧数据。

<a id="fresh"></a>
## 1. 两台检查 Docker 并更新代码

分别在 `gateway`、`scene` 执行：

```bash
uname -m
sudo docker version
sudo docker compose version
```

Docker 需要显示 Server 信息，Compose 需要支持 `up --wait`。你已经安装好 Docker 时继续下一步；全新机器先按[系统安装补充](GITHUB_VPS.md#2-安装-docker)安装。宿主机需有 `git`、`curl`、`openssl`；缺失时在 Ubuntu 用 `sudo apt-get install -y git curl openssl`，OpenCloudOS 用 `sudo dnf install -y git curl openssl` 安装。Go、GCC 和 CMake 不需要安装。

两台分别克隆或更新仓库：

```bash
cd ~
if [ -d mmo-server/.git ]; then
  cd mmo-server
  git pull --ff-only
else
  git clone https://github.com/dravenlove/mmo-server.git
  cd mmo-server
fi
git rev-parse HEAD
```

后续命令均在 `~/mmo-server` 执行。两台的源码提交号应一致。实际 env 文件被 Git 忽略，更新代码不会替你填写或迁移配置；已有源码修改需先保存再更新。

## 2. 准备指定 MySQL

这一步在你已有的 **MySQL 管理端**执行，不是在 scene 容器里执行。要求 MySQL 8.0.16+，使用可写主实例。

```sql
SELECT VERSION();
```

已有独立数据库和应用账号时继续使用。需要新建时，由数据库管理员执行下面的示例，并替换密码与授权来源地址：

```sql
CREATE DATABASE mmo_game CHARACTER SET utf8mb4;
CREATE USER 'mmo_app'@'10.20.0.11' IDENTIFIED BY '替换为实际密码';
GRANT SELECT, INSERT, UPDATE, CREATE, INDEX, REFERENCES
  ON mmo_game.* TO 'mmo_app'@'10.20.0.11';
```

授权来源应为 MySQL 实际看到的 gateway 出口地址。Backend 首次启动会在指定库中创建四张业务表，不会创建数据库或账号；战斗版新增 `combat_snapshots` 表，已有账号需保留建表权限。MySQL 参数只配置在 gateway 主机，scene 不需要数据库密码。

网络需满足：

| 来源 | 目标 | 端口 | 用途 |
| --- | --- | --- | --- |
| gateway 的两个容器 | scene 私网/VPN地址 | 9001、9002 | 场景调用 |
| gateway 的 Backend 容器 | 指定 MySQL | 3306 或配置的端口 | 数据持久化 |
| 联调客户端 | gateway 绑定地址 | 7000 | 玩家连接 |

scene 端口和 MySQL 端口仅允许相应私网/VPN来源及受控运维来源。Backend 不发布宿主机 8081，gateway 健康端口 8080 只绑定回环。内部 HTTP 的共享密钥提供认证，跨机流量使用可信私网或加密 VPN。

## 3. 生成一次内部密钥

首次部署时，在可信终端执行一次：

```bash
openssl rand -hex 32
```

将生成的同一个值填入两台 env 的 `INTERNAL_TOKEN`。这是服务之间的密钥；MySQL 使用自己的账号密码，玩家使用客户端保存的登录凭据。已有部署继续保留原密钥，更新镜像无需重新生成。

## 4. 配置并启动 scene

以下操作只在 **scene 主机**执行。

先创建实际配置文件；已经存在时不会覆盖：

```bash
cd ~/mmo-server
test -f deploy/scene-host.env || (umask 077; cp deploy/scene-host.env.example deploy/scene-host.env)
chmod 600 deploy/scene-host.env
```

用你熟悉的编辑器或 SFTP 编辑 `deploy/scene-host.env`，整理为下面的配置。填入密钥，并将示例 IP 替换为 scene 实际网卡上的地址：

```dotenv
INTERNAL_TOKEN=
MMO_IMAGE_PREFIX=ghcr.io/dravenlove/mmo-server
MMO_IMAGE_TAG=mvp-20261105-mysql
SCENE_BIND_IP=10.20.0.22
TICK_HZ=20
AOI_RADIUS=30
MOVE_SPEED=6
```

校验、拉取并启动。两个场景共用同一个镜像，只拉取一次：

```bash
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml config --quiet
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml config --images
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml pull --policy always scene1
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml up -d --no-build --pull never --wait
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml ps
```

预期项目为 `mmo-scene`，`scene1`、`scene2` 都显示 healthy。任意命令报错时先处理，再继续下一步。

## 5. 配置并启动 gateway

以下操作只在 **gateway 主机**执行。

```bash
cd ~/mmo-server
test -f deploy/gateway-host.env || (umask 077; cp deploy/gateway-host.env.example deploy/gateway-host.env)
chmod 600 deploy/gateway-host.env
```

编辑 `deploy/gateway-host.env`。填入与 scene 相同的内部密钥，填写真实数据库账号、密码和库名，并替换三个示例 IP：

```dotenv
INTERNAL_TOKEN=
MMO_IMAGE_PREFIX=ghcr.io/dravenlove/mmo-server
MMO_IMAGE_TAG=mvp-20261105-mysql
SCENE_HOST_IP=10.20.0.22
GATEWAY_BIND_IP=10.20.0.11
BACKEND_DB_DRIVER=mysql
MYSQL_HOST=10.20.0.30
MYSQL_PORT=3306
MYSQL_USER=mmo_app
MYSQL_PASSWORD=''
MYSQL_DATABASE=mmo_game
MYSQL_TLS=false
BACKEND_VOLUME_NAME=mmo-two-host-backend-data
TLS_CERT_DIR=./certs
TLS_CERT_FILE=
TLS_KEY_FILE=
```

`INTERNAL_TOKEN` 和 `MYSQL_PASSWORD` 的空值必须按实际环境填写。密码用单引号保存，避免 `$` 被 Compose 当作变量；包含单引号时按 [env 文件语法](https://docs.docker.com/compose/how-tos/environment-variables/variable-interpolation/#env-file-syntax)转义。实际配置不提交 Git。

这里先将玩家入口绑定到 gateway 私网地址用于联调。`MYSQL_TLS=false` 用于受控私网/隧道；数据库要求 TLS 时使用 `true`，证书须受信任且匹配 `MYSQL_HOST`。

校验、拉取并启动：

```bash
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml config --quiet
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml config --images
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml pull --policy always backend gateway
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml up -d --no-build --pull never --wait
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml ps
curl --fail http://127.0.0.1:8080/readyz
```

预期项目为 `mmo-gateway`，`backend`、`gateway` 都 healthy，readyz 返回成功。Backend 日志应包含 `database=mysql`。MySQL 无法连接、权限不足或库名错误时先修复，不进入下一步。

## 6. 验证容器之间真的互通

在 **gateway 主机**执行，把示例地址替换为 scene 的实际私网/VPN地址：

```bash
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml exec gateway curl --fail --connect-timeout 3 --max-time 5 http://10.20.0.22:9001/healthz
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml exec gateway curl --fail --connect-timeout 3 --max-time 5 http://10.20.0.22:9002/healthz
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml exec backend curl --fail --connect-timeout 3 --max-time 5 http://10.20.0.22:9001/healthz
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml exec backend curl --fail --connect-timeout 3 --max-time 5 http://10.20.0.22:9002/healthz
```

四次都应返回成功。readyz 只覆盖网关到 Backend 的就绪检查，不能代替这四次检查和下一步的真实业务验收。

## 7. 用容器客户端完成业务验收

在 **gateway 主机**运行，不需要在 VPS 编译客户端。`-addr` 使用 gateway 在第 5 步绑定的实际地址：

```bash
sudo docker pull ghcr.io/dravenlove/mmo-server/client:mvp-20261105-mysql
sudo docker run --rm -it --pull never --network host \
  --mount type=volume,source=mmo-client-tokens,target=/app/tokens \
  ghcr.io/dravenlove/mmo-server/client:mvp-20261105-mysql \
  -addr 10.20.0.11:7000 -name deploy-check -token-file /app/tokens/deploy-check.token
```

逐行输入以下命令；执行 `move 1 0` 后等待约 1 秒，再输入下一条 `view`，让场景完成 Tick。不要整块立即粘贴：

```text
view
move 1 0
view
claim
claim
transfer scene-2
view
quit
```

验收标准：能注册并登录、移动后坐标变化、奖励总额只有 100 金币、传送后场景为 scene-2。新账号第一次 claim 应为 granted=true，第二次为 false；已经领取过的账号再次领取仍为 false。

再次运行同一客户端命令，应使用卷内原凭据登录，角色和领取记录保留。客户端 token 卷和 MySQL 数据库分别保存凭据与玩家数据，都需要保留。至此私网最小部署完成。

## 8. 开放公网入口时再配置 TLS

公网玩家只访问 gateway 的 TCP 7000。在 gateway 的项目根目录执行 `mkdir -p deploy/certs`，将匹配玩家域名的有效证书链 `fullchain.pem` 和私钥 `privkey.pem` 放入该目录。然后修改 `deploy/gateway-host.env`：

```dotenv
GATEWAY_BIND_IP=0.0.0.0
TLS_CERT_DIR=./certs
TLS_CERT_FILE=/certs/fullchain.pem
TLS_KEY_FILE=/certs/privkey.pem
```

证书目录只读挂入容器，赋予运行组 10001 读取权限并重建网关：

```bash
sudo chown root:10001 deploy/certs deploy/certs/fullchain.pem deploy/certs/privkey.pem
sudo chmod 750 deploy/certs
sudo chmod 640 deploy/certs/fullchain.pem deploy/certs/privkey.pem
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml config --quiet
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml up -d --no-build --pull never --force-recreate --wait gateway
```

域名解析应指向 gateway 的玩家入口地址，云安全组允许玩家访问 TCP 7000。客户端的 `-addr` 使用证书域名并加 `-tls`；自签 CA 的详细参数见 [TLS 补充](DEPLOYMENT.md#4-网关-tls)。scene 的 9001/9002、Backend 和数据库继续走私网/VPN，不作为玩家入口。Docker 发布端口的访问限制还应核对云安全组及转发链，参见 [Docker 防火墙说明](https://docs.docker.com/engine/network/packet-filtering-firewalls/)。

<a id="updates"></a>
## 9. 已有部署的后续更新

先在 GitHub 发布并确认新镜像标签，然后在维护窗口更新。文档或配置改名可继续使用现有应用镜像；应用源码变更使用新标签。

1. 两台执行 `git pull --ff-only`，核对源码。保留原 env、内部密钥、数据库配置、证书和卷名。
2. 两台 env 的 `MMO_IMAGE_TAG` 更新为同一已发布标签。
3. 先完成两台镜像拉取，再停止当前服务。

在 **scene 主机**提前拉取：

```bash
cd ~/mmo-server
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml config --quiet
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml config --images
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml pull --policy always scene1
```

在 **gateway 主机**提前拉取：

```bash
cd ~/mmo-server
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml config --quiet
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml config --images
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml pull --policy always backend gateway
```

拉取成功后，按 [停止顺序](#operations)停机并备份 MySQL，再按同节的恢复顺序启动。重做第 6、7 步验收。客户端需要重新登录；本 MVP 不提供无损滚动升级。

若只改 MySQL 地址、端口或账号，无需新镜像。确认新目标库已有所需数据，在维护窗口修改 gateway env 后执行；已按下一节停止全部服务时，改用该节的完整恢复顺序：

```bash
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml config --quiet
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml up -d --no-build --pull never --force-recreate --wait backend
curl --fail http://127.0.0.1:8080/readyz
```

单独 restart 不会更新容器环境变量。切库也不会复制数据库，已有玩家时先停止入口并备份，地址变更后按 [恢复顺序](#operations)启动。

<a id="operations"></a>
## 10. 日常停止、恢复和备份

**停止顺序：先 gateway 入口，再 Backend，最后 scene。**

在 gateway 主机：

```bash
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml stop gateway
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml stop backend
```

再在 scene 主机：

```bash
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml stop
```

MySQL 模式下备份指定 MySQL 实例的数据。`BACKEND_VOLUME_NAME` 对应的本地卷不包含 MySQL 玩家数据；只有 SQLite 模式使用它保存数据库文件。SQLite 停机备份方法见[补充说明](DEPLOYMENT.md#7-备份与恢复)。

**恢复顺序：MySQL 可用 → scene 健康 → gateway 健康。**

先在 scene 主机：

```bash
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml up -d --no-build --pull never --wait
```

再在 gateway 主机：

```bash
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml up -d --no-build --pull never --wait
curl --fail http://127.0.0.1:8080/readyz
```

保留客户端凭据卷和数据库。正常更新不使用 `down -v`；Backend 始终只运行一个实例。若版本涉及数据库格式变化，回退时同时考虑对应镜像、配置和停机备份，不能只回退 Git 提交。

<a id="troubleshooting"></a>
## 11. 常见问题按现象处理

| 现象 | 优先检查 |
| --- | --- |
| couldn't find env file | 第 4、5 步是否在对应主机创建了实际 env；不能只保留 example |
| 拉取镜像 denied / not found | 镜像前缀、标签、仓库可见性；当前发布镜像支持匿名拉取 |
| 拉取超时 / DNS 错误 | VPS 到 GHCR 及镜像分发地址的网络；可用[离线镜像包](PREBUILT_IMAGES.md)替代下载 |
| address not available | 绑定 IP 是否实际存在于这台主机；角色名称不是绑定地址 |
| Backend 重启、MySQL 连接失败 | MYSQL_HOST/PORT、账号授权来源、库名、MySQL 版本和网络白名单 |
| 数据库被其他 Backend 占用 | 是否仍有旧项目或第二个 Backend；按[旧命名迁移](DEPLOYMENT_NAMING_MIGRATION.md)检查 |
| 都 healthy 但登录/移动失败 | 第 6 步容器互通，以及两台 INTERNAL_TOKEN 是否完全相同 |
| 改完 env 没有生效 | 使用 up 重建对应容器；单独 restart 不会重新读取 env |

在出错的主机查看日志：

```bash
# scene 主机
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml logs --tail 80 scene1 scene2
```

```bash
# gateway 主机
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml logs --tail 80 backend gateway
```

配置校验使用 `config --quiet`，避免将密钥和密码整份打印出来。反馈问题时保留错误和阶段，隐藏实际密码、内部密钥及玩家 token。

本机已验证 Linux Docker、SQLite / MySQL 业务流程和匿名拉取，范围见[验证记录](VALIDATION.md)。真实 VPS 私网路由、数据库账号权限、证书与容量仍需现场验收。

补充资料按需使用：[系统安装](GITHUB_VPS.md)、[MySQL 参数与启动选项](MYSQL.md)、[镜像发布](REGISTRY_DEPLOYMENT.md)、[离线镜像包](PREBUILT_IMAGES.md)、[旧命名迁移](DEPLOYMENT_NAMING_MIGRATION.md)。
