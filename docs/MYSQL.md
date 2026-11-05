# Backend 连接指定的 MySQL

Backend 可通过配置选择 SQLite 或指定的 MySQL，Gateway 和 C++ 场景不直接连接数据库。修改地址、端口、账号或库名后重建 Backend 容器即可，不需要重新编译镜像。此功能需要包含 MySQL 支持的新镜像，例如 `mvp-20261105-mysql`；旧版 `mvp-20261104` 只有 SQLite 支持。

## 1. 数据库准备

支持 MySQL **8.0.16 或更高版本**，实际验证使用 MySQL 8.0.46。连接到 MySQL 的可写主实例，并为本服务准备独立数据库。Backend 首次启动会在指定库中创建 `roles`、`claims`、`migrations` 三张 InnoDB 表，不会自动创建数据库或账号。

例如由数据库管理员创建应用库和账号，下面的地址、库名和密码均为示例：

```sql
CREATE DATABASE mmo_game CHARACTER SET utf8mb4;
CREATE USER 'mmo_app'@'10.20.0.11' IDENTIFIED BY '替换为实际密码';
GRANT SELECT, INSERT, UPDATE, CREATE, INDEX, REFERENCES
  ON mmo_game.* TO 'mmo_app'@'10.20.0.11';
```

授权中的主机地址应是数据库实际看到的 Backend 出口地址。两机方案一般是主机 A 的私网/VPN地址；容器访问其他主机时通常经过宿主 NAT。MySQL 的监听地址、数据库访问白名单、云防火墙和网络路由都要允许 A 连接数据库端口。

## 2. Docker 配置

两机部署只需在 **A：Ubuntu** 的 `deploy/two-host-a.env` 配置数据库；B 的场景服务不需要 MySQL 密码。分机 Backend 使用 `deploy/backend.env`，同机演示使用根目录 `.env`。

```dotenv
BACKEND_DB_DRIVER=mysql
MYSQL_HOST=10.20.0.30
MYSQL_PORT=3306
MYSQL_USER=mmo_app
MYSQL_PASSWORD='填写实际密码'
MYSQL_DATABASE=mmo_game
MYSQL_TLS=false
```

将 `MYSQL_HOST` 改为你指定的 MySQL IP 或主机名；不要带 `http://`、`mysql://` 或端口。端口单独放在 `MYSQL_PORT`。用户名、密码、库名均可独立修改，密码中的 `@`、`:`、`/` 不需要按连接串语法转义。Compose env 文件中的密码建议用单引号，保留 `$` 等字符的字面值；密码包含单引号时需按 [Compose env 文件语法](https://docs.docker.com/compose/how-tos/environment-variables/variable-interpolation/#env-file-syntax) 转义。

`MYSQL_TLS=true` 要求 TLS 并校验服务器证书，证书须被运行镜像信任且与 `MYSQL_HOST` 匹配；`false` 是受控私网/隧道部署的默认值。本实现不提供跳过证书校验或静默降级选项。

使用 GHCR 镜像时，两台应用主机的版本还需统一设置为本次已成功发布的标签：

```dotenv
MMO_IMAGE_PREFIX=ghcr.io/dravenlove/mmo-server
MMO_IMAGE_TAG=mvp-20261105-mysql
```

配置和密钥只在实际 env 文件保存，不提交 Git，也不作为镜像构建参数。修改 env 后仅 `docker compose restart` 不会更新容器环境；需要执行 `up` 重建容器。

## 3. 启动与修改地址

首次部署先按 [镜像仓库部署](REGISTRY_DEPLOYMENT.md) 拉取镜像，并启动 B 的两个场景。然后在 A 执行：

```bash
cd ~/mmo-server
git pull --ff-only
chmod 600 deploy/two-host-a.env
sudo docker compose --env-file deploy/two-host-a.env -f deploy/two-host-a.compose.yaml -p mmo-host-a config --quiet
sudo docker compose --env-file deploy/two-host-a.env -f deploy/two-host-a.compose.yaml -p mmo-host-a pull --policy always backend gateway
sudo docker compose --env-file deploy/two-host-a.env -f deploy/two-host-a.compose.yaml -p mmo-host-a up -d --no-build --pull never --wait
curl --fail http://127.0.0.1:8080/readyz
```

以后只改数据库地址或端口时，编辑同一 env 文件的 `MYSQL_HOST`、`MYSQL_PORT`，再执行最后的 `up` 命令；同一镜像无需重新构建或重新拉取。可追加 `--force-recreate backend` 明确只重建 Backend。

仅改变地址不会复制数据库。新地址必须指向已经具备所需数据的库，或明确作为空库重新开始。已有玩家时按维护窗口停机并备份，再执行地址变更或版本更新。

## 4. 本机和可选启动参数

Windows PowerShell 可以用环境变量指定账号，并用启动参数覆盖地址：

```powershell
$env:INTERNAL_TOKEN = '填入与场景服务相同的内部密钥'
$env:MYSQL_USER = 'mmo_app'
$env:MYSQL_PASSWORD = '填写实际密码'
$env:MYSQL_DATABASE = 'mmo_game'
go run ./cmd/backend -db-driver mysql -mysql-host 10.20.0.30 -mysql-port 3306
```

可选参数 `-db-driver`、`-db-path`、`-mysql-host`、`-mysql-port` 优先于对应环境变量；密码使用环境变量传递。Windows 本机不在云私网时，需先通过 VPN 或受控隧道使数据库地址可达。

若已经有外部只读挂载的密码文件，也可给 Backend 设置 `MYSQL_PASSWORD_FILE`（容器内路径），它优先于 `MYSQL_PASSWORD`。需要自行通过 Compose override 挂载文件并传入该环境变量；基础模板使用普通 env 配置。

## 5. 数据、独占和恢复边界

- 默认仍为 `BACKEND_DB_DRIVER=sqlite`。切到 MySQL 不会读取、清空或自动迁移旧 SQLite 文件；原来的 SQLite 卷保留。首次使用空 MySQL 库时账号数据从空库开始，需要保留旧玩家数据时应先安排停机迁移。
- **Backend 仍只运行一个实例。** SQLite 使用文件锁，MySQL 使用 `GET_LOCK` 会话锁；同一 MySQL 服务的同一库只能由一个本项目 Backend 打开。更换数据库不代表应用已支持多副本或数据库自动容灾。
- MySQL 独占锁和全部业务 SQL 固定在同一条连接。连接丢失后旧进程不会偷偷换连接继续写；后台健康检查会停止进程，由 Compose 的 `restart: unless-stopped` 重启并重新取锁。数据一致性优先于继续使用已经失锁的进程。[MySQL 会话锁说明](https://dev.mysql.com/doc/refman/8.4/en/locking-functions.html)
- 迁移的准备、归属提交、目标激活与恢复逻辑保留。MySQL 的生成列唯一索引保证每个角色最多一条未完成迁移；角色名采用字节比较，保留原来区分大小写和重音的语义。
- MySQL 模式下玩家数据保存在指定 MySQL 中。复制 `/app/data` 中的 SQLite 文件不能备份 MySQL；备份和恢复使用该 MySQL 实例的维护流程。

## 6. 开发验证

先准备本地 Docker Linux 引擎和 MySQL 测试镜像：

```powershell
docker pull mysql:8.0
python scripts/test_mysql.py
```

脚本仅启动随机命名的本机测试容器，生成临时密码，为每个用例创建独立测试库；结束后清理自己的容器和测试卷，不读取实际部署的 `MYSQL_HOST`。覆盖登录、并发奖励、迁移恢复、相同坐标检查点、Unicode 名字、唯一约束及失去数据库会话后的旧写入者隔离。

使用已经构建到本机的应用镜像做端到端检查：

```powershell
python scripts/smoke_docker.py --tag mvp-20261105-mysql --database mysql
```

该脚本使用独立 MySQL 容器与随机项目，验证数据库重启后的 Backend 自动恢复，以及全部容器停止后保留玩家状态。发布工作流在 SQLite 和 MySQL 验收通过后才推送四个镜像。这里的验证不替代你指定数据库的真实网络、账号权限和证书验收。
