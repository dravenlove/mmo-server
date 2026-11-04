# Linux Docker 部署

本目录提供单机演示和各角色独立部署。所有镜像默认使用非 root 的 UID/GID `10001:10001` 运行；Backend 是唯一持久化进程，单实例 SQLite 数据目录必须挂载持久卷。

**只有两台主机时，请优先使用 [两机部署步骤](DEPLOYMENT_TWO_HOSTS.md)**：A 运行网关与 Backend，B 运行两个场景。下文按角色分别说明，四个角色不要求四台物理主机。

## 1. 端口与主机规划

下面地址仅为示例，部署时替换为各主机实际存在的私网地址：

| 主机 | 示例地址 | 角色 | 端口与允许访问者 |
| --- | --- | --- | --- |
| Backend | `10.20.0.11` | Go Backend、SQLite 卷 | TCP 8081，仅网关与受控运维来源 |
| Scene 1 | `10.20.0.21` | C++ `scene-1` | TCP 9001，仅 Backend、网关与受控运维来源 |
| Scene 2 | `10.20.0.22` | C++ `scene-2` | TCP 9002，仅 Backend、网关与受控运维来源 |
| Gateway | `10.20.0.31` | Go Gateway | TCP 7000 给客户端；8080 仅本机健康检查 |

**只有网关 TCP 7000 是客户端入口。** 客户端不连接 Backend、场景或数据库。四个服务的内部网络必须互通；Backend 和网关都需要访问两个场景，因为网关根据 Backend 返回的 `scene_url` 直接发送移动和视图请求。

在云安全组、上游防火墙或 Docker 对应的转发链中按上表限制源 IP。不要仅凭宿主机普通入站规则假定 Docker 发布端口已被拦截；Docker 的端口发布规则与主机防火墙交互需要实际检查。内部服务绑定主机私网地址，禁止把 8081、9001、9002 开到公网。[Docker 防火墙说明](https://docs.docker.com/engine/network/packet-filtering-firewalls/)

当前内部传输是 HTTP，`X-Internal-Token` 只提供共享密钥认证，不加密流量。请部署在可信私网或加密 VPN 中。所有服务使用相同的至少 32 字符随机 `INTERNAL_TOKEN`；它不是玩家 token，不能放进客户端。环境变量对有 Docker 管理权限的人可见，不要上传 `.env`、证书私钥或玩家 token 文件。

## 2. 构建镜像

在有源码和网络的 Linux 构建机、项目根目录运行：

```bash
docker build -f Dockerfile.golang --build-arg SERVICE=backend -t mmo/backend:dev .
docker build -f Dockerfile.golang --build-arg SERVICE=gateway -t mmo/gateway:dev .
docker build -f Dockerfile.golang --build-arg SERVICE=client -t mmo/client:dev .
docker build -f Dockerfile.scene -t mmo/scene:dev .
```

Go 构建使用 `golang:1.26-bookworm`、`CGO_ENABLED=0`；可通过 `--build-arg GO_VERSION=...` 改用经过验证的兼容版本。运行时含 CA 证书。C++ 构建使用 C++17、CMake 和随仓库提供的 `third_party/` 头文件，运行时安装所需 C++ 运行库。两个镜像均采用多阶段构建。[Docker 多阶段构建](https://docs.docker.com/build/building/multi-stage/)

首次构建仍会拉取基础镜像、系统软件包和 Go 模块，因此不是完全离线构建。当前镜像标签用于演示；正式交付应将版本标记替换为自己的固定发布版本，并记录镜像摘要。构建架构应与目标服务器一致；将 amd64 镜像直接发到 arm64 服务器不构成可用部署。

通过仓库分发，或保存后复制到对应目标主机：

```bash
docker image save -o mmo-backend.tar mmo/backend:dev
docker image save -o mmo-gateway.tar mmo/gateway:dev
docker image save -o mmo-scene.tar mmo/scene:dev
docker image save -o mmo-client.tar mmo/client:dev
```

在各目标主机加载其需要的包，例如：

```bash
docker image load -i mmo-scene.tar
```

把 `deploy/` 配置文件传到目标主机即可，镜像运行不要求保留 Go/C++ 编译工具或完整源码。多机模板没有 `build` 字段，直接启动已加载或仓库可拉取的镜像。

## 3. 准备配置

在可信管理终端生成一次共享随机密钥：

```bash
openssl rand -hex 32
```

在对应角色主机上，把相应示例复制为实际环境文件，填写同一个密钥与实际 IP。以下命令以保留项目 `deploy/` 目录结构为例：

```bash
cp deploy/backend.env.example deploy/backend.env
cp deploy/scene1.env.example deploy/scene1.env
cp deploy/scene2.env.example deploy/scene2.env
cp deploy/gateway.env.example deploy/gateway.env
chmod 600 deploy/*.env
```

每台主机只需要属于它的配置。示例中的 `INTERNAL_TOKEN` 故意留空，未填写时 Compose 会拒绝启动；服务本身还会检查最小长度。

关键配置：

- Backend：`BACKEND_BIND_IP` 是此主机真实私网 IP；`SCENES` 中的两个地址必须可被 Backend 和网关同时访问。`BACKEND_VOLUME_NAME` 是此主机的持久命名卷名称。
- 场景：`SCENE_BIND_IP` 是当前主机的私网 IP，`SCENE_ID` 唯一且与 Backend 配置一致。两台独立主机可以使用相同端口，本示例采用 9001、9002 便于区分。
- 网关：`BACKEND_URL` 指向 Backend 的真实私网地址；`GATEWAY_BIND_IP` 决定玩家入口绑定的宿主机地址。填写 `0.0.0.0` 会监听所有网卡，需配合防火墙和 TLS。

容器内的 `127.0.0.1` 指向当前容器。跨主机也不能使用单机 Compose 的 `scene1`、`scene2`、`backend` 服务名，它们没有跨 Docker 主机自动解析和路由能力。映射端口与 `SCENES` URL 应保持一致。

## 4. 网关 TLS

公网入口配置可被客户端验证的服务端证书。将证书链和私钥放在网关主机目录，例如 `/opt/mmo/certs`，并在 `deploy/gateway.env` 中填写：

```dotenv
TLS_CERT_DIR=/opt/mmo/certs
TLS_CERT_FILE=/certs/fullchain.pem
TLS_KEY_FILE=/certs/privkey.pem
```

`TLS_CERT_DIR` 是宿主机绝对路径，后两项是容器内路径。该目录只读挂载到 `/certs`；确保容器 UID/GID 10001 能读取文件，私钥不要设为所有用户可读。例如由 root 管理目录并给组 10001 读取权限：

```bash
sudo chown root:10001 /opt/mmo/certs /opt/mmo/certs/fullchain.pem /opt/mmo/certs/privkey.pem
sudo chmod 750 /opt/mmo/certs
sudo chmod 640 /opt/mmo/certs/fullchain.pem /opt/mmo/certs/privkey.pem
```

证书的 SAN 必须覆盖客户端连接的域名或 IP。客户端使用 `-tls` 开启验证；私有 CA 时追加 `-ca` 指向可信 CA 证书：

```bash
go run ./cmd/client -addr game.example.com:7000 -tls \
  -name alice -token-file ./tokens/alice.token
go run ./cmd/client -addr game.internal.example:7000 -tls -ca ./private-ca.pem \
  -name bob -token-file ./tokens/bob.token
```

`game.example.com` 等是占位域名，请替换。不提供跳过证书验证的部署步骤。仅本机或可信私网演示可以同时清空 `TLS_CERT_FILE` 和 `TLS_KEY_FILE`；证书轮换后重新创建网关容器使其加载新文件。

## 5. 分机器启动顺序

先检查各机器的 Docker Engine 与 Compose 可用，并确认安全组和私网互通。使用固定项目名避免同机不同角色配置彼此覆盖。

Scene 1 主机：

```bash
docker compose --env-file deploy/scene1.env -f deploy/scene.compose.yaml -p mmo-scene1 config --quiet
docker compose --env-file deploy/scene1.env -f deploy/scene.compose.yaml -p mmo-scene1 up -d --wait
curl --fail http://10.20.0.21:9001/healthz
```

Scene 2 主机：

```bash
docker compose --env-file deploy/scene2.env -f deploy/scene.compose.yaml -p mmo-scene2 config --quiet
docker compose --env-file deploy/scene2.env -f deploy/scene.compose.yaml -p mmo-scene2 up -d --wait
curl --fail http://10.20.0.22:9002/healthz
```

Backend 主机：

```bash
docker compose --env-file deploy/backend.env -f deploy/backend.compose.yaml -p mmo-backend config --quiet
docker compose --env-file deploy/backend.env -f deploy/backend.compose.yaml -p mmo-backend up -d --wait
curl --fail http://10.20.0.11:8081/healthz
```

Gateway 主机：

```bash
docker compose --env-file deploy/gateway.env -f deploy/gateway.compose.yaml -p mmo-gateway config --quiet
docker compose --env-file deploy/gateway.env -f deploy/gateway.compose.yaml -p mmo-gateway up -d --wait
curl --fail http://127.0.0.1:8080/healthz
```

独立 Compose 项目无法通过 `depends_on` 等待其他机器；因此按“两个场景 → Backend → 网关”顺序启动，并从 Backend 与网关主机检查它们需要访问的内部 URL。单机根 `compose.yaml` 则设置了健康检查依赖。[Compose 服务与健康检查](https://docs.docker.com/reference/compose-file/services/)

启动后用客户端完成：首次注册、移动、进入 `scene-2`、连续领取两次奖励、退出重登。确认金币只增加一次，断开后可以使用原 token 重登。不要仅凭 HTTP `/healthz` 返回 200 判定完整业务通过。

## 6. 停止、重启与升级

同机部署的常规停止：

```bash
docker compose stop gateway
docker compose stop backend
docker compose stop scene1 scene2
```

多机使用相同角色顺序，在对应主机运行对应项目的 `stop`。先断开网关使连接有机会保存位置，再停止 Backend，最后停止场景。位置保存是尽力而为，异常退出仍可能回退至最近成功快照。

普通恢复按第 5 节启动顺序运行 `up -d --wait`。修改 `.env`、镜像标签或证书后使用 `up -d --force-recreate --wait`；`restart` 不会重新读取变更后的容器环境变量。

场景重启会丢失角色内存、未落盘位置及代次墓碑；客户端重新登录后由 Backend 从当前归属和最近快照恢复。当前设计没有场景热迁移、自动故障切换或零停机升级。服务不可用时不会把角色随意路由到另一个场景。

`docker compose down` 删除容器和网络、保留命名卷；**`down -v` 会删除命名卷**。数据库卷仅供一个 Backend 进程使用；即使进程锁能够拒绝误启动，也不要通过 `--scale backend=2` 或共享网络盘实现“高可用”。

## 7. 备份与恢复

SQLite 处于 WAL 模式时，不能在运行中只复制 `mmo.db` 并认为获得了完整备份。这里采用停机备份整个数据目录，保留可能存在的 WAL 文件；在线备份需另行使用 SQLite 备份接口。[SQLite WAL 说明](https://sqlite.org/wal.html)。以下示例面向分机 Backend 的 `mmo-backend-data` 命名卷；本机根 Compose 改为 `mmo-demo-backend-data`。

先停止所有连接该 Backend 的网关，再停止 Backend，确认它已退出。提前备份 `.env` 和玩家 token 文件到受保护的位置，但不要将密钥打包进公共镜像。

```bash
docker compose --env-file deploy/backend.env -f deploy/backend.compose.yaml -p mmo-backend stop
docker compose --env-file deploy/backend.env -f deploy/backend.compose.yaml -p mmo-backend ps --all
umask 077
mkdir -p backups
docker run --rm --user 0:0 --entrypoint tar \
  --mount type=volume,source=mmo-backend-data,target=/data,readonly \
  --mount type=bind,src="$PWD/backups",dst=/backup \
  -w /data mmo/backend:dev -czf /backup/mmo-data.tar.gz .
```

这里临时备份容器使用 root，以保留数据目录的所有者与权限；业务服务仍使用非 root 用户运行。备份文件包含账号凭据哈希、角色和资产，应保密，并复制到其他磁盘或机器。

恢复时使用**新的空卷**，保留旧卷以便核对或回退。先停止网关、Backend 和两个场景，避免旧场景内存与恢复数据库相互冲突。确认 `mmo-backend-restored` 尚未被其他服务使用，然后：

```bash
docker volume create mmo-backend-restored
docker run --rm --user 0:0 --entrypoint tar \
  --mount type=volume,source=mmo-backend-restored,target=/data \
  --mount type=bind,src="$PWD/backups",dst=/backup,readonly \
  -w /data mmo/backend:dev -xzpf /backup/mmo-data.tar.gz
```

将 `deploy/backend.env` 的 `BACKEND_VOLUME_NAME` 改为 `mmo-backend-restored`，按第 5 节顺序启动。重新登录抽查账号、金币和场景归属；确认成功后再决定旧卷的保留周期。恢复旧备份会回退备份后的资产和迁移记录，不是无损灾难恢复。

## 8. 故障排查与当前边界

| 现象 | 首先检查 |
| --- | --- |
| Compose 提示必填变量为空 | 是否指定正确 `--env-file`，是否填写 `INTERNAL_TOKEN`、地址和场景 ID |
| 内部请求被拒绝 | 四个服务的共享密钥是否一致且至少 32 字符；不要在日志中打印密钥 |
| 网关健康但无法进入场景 | 网关能否访问 Backend 和 `SCENES` 中的场景私网地址；健康检查不覆盖完整链路 |
| 迁移停在处理中 | 源和目标场景是否恢复；重试原 request_id 或重新登录，由持久迁移记录继续处理，不手动改归属 |
| Backend 数据目录不可写 | 命名卷或挂载目录是否可由 UID/GID 10001 写入；不能把数据目录只读挂载 |
| TLS 失败 | 容器内证书路径、读取权限、域名 SAN、有效期与客户端 CA 是否匹配 |
| 场景重启后旧连接出错 | 重新登录以重建场景内存；位置可能回退到最近成功快照 |
| 场景拒绝新角色或新迁移 | 检查下面的已加载角色、历史身份和取消迁移记录上限；断线不会自动释放角色槽位 |

场景使用明确的内存容量上限，达到后拒绝相应的新操作：

| 上限 | 计数范围 | 处理方式与边界 |
| --- | --- | --- |
| 每实例 128 个已加载角色 | 包括在线、离线、冻结、待激活角色；当前不会自动回收离线角色 | 新入场或准备对象需要空槽位。完成迁出可以释放已加载对象；也可在保存状态、停止入口并处理未决迁移后受控重启，但未保存位置会回退 |
| 每实例 8192 个历史 `role_id` 跟踪与隔离记录 | 保存出现过的角色身份和删除代次墓碑，用于拒绝延迟旧请求 | 达到后拒绝新身份。当前没有在线清理机制；受控重启会清空这些内存记录，也会失去它们提供的旧请求隔离，不能当作透明扩容 |
| 每角色每会话 256 条取消迁移记录 | 用于阻止已经取消的迁移被迟到请求重新执行 | 达到后拒绝新的 `freeze`，返回容量错误；先完成或恢复已有迁移，再重新登录，较高的 `session_epoch` 可开始新的会话记录 |

受控重启应先停止网关入口，确保 Backend 仍可保存位置并处理迁移，检查未决迁移状态后按第 6 节停止和恢复。重启后的客户端必须重新登录；不要在网络故障和未决迁移尚未澄清时，把重启当作自动修复手段。

固定 Tick 默认 20Hz，AOI 半径默认 30，移动速度默认每秒 6 个单位。上述配置和容量限制不是经过压测验证的性能承诺，也不等同于支持 128 人持续在线。视图每连接每 200ms 轮询一次，尚未实现跨玩家批量推送。Backend 的协调与 SQLite 持久化是单点，不支持多副本扩容。

迁移使用持久记录、角色归属代次和会话代次；提交前可恢复撤销，提交后继续目标激活。遇到不确定状态会暂停角色而不是宣称迁移成功。场景进程重启会丢失内存墓碑，当前实现不承诺在任意网络分区、延迟旧请求和任意进程重启组合下仍提供生产级故障接管，不能据此宣称严格高可用或零数据丢失。

已完成 Windows 原生四进程端到端 smoke 的 15/15 项检查、Go 测试与静态检查、C++ CTest 2/2 项，以及网关原生 TLS 验证。三个 Go 程序的 Linux amd64 交叉编译已通过；开发命令见 [README](../README.md#本机开发验证)，完整结果见 [验证记录](VALIDATION.md)。当前环境的 Docker Engine 不可用，现有 WSL 编译工具过旧，因此 C++ Linux 编译、Docker 镜像构建、容器运行、容器内 TLS 证书加载与跨机网络尚需在目标环境验收。七份环境的 Compose 配置展开已通过（包含两机 A/B 配置），属于配置静态校验，不代表上述运行验证通过。
