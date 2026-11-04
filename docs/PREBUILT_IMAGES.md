# 本机构建镜像，两台 VPS 加载运行

适合两台 2 核 2 GB VPS：把编译移到开发机，服务器只加载镜像、创建容器和运行服务。之前卡在 Debian 软件包下载的问题也集中在构建机处理。运行容量仍需压测，不能根据成功启动推算在线人数。

| 目标机器 | 需要上传的包 | 包含镜像 |
| --- | --- | --- |
| A：Ubuntu | `mmo-host-a.tar.gz` 及同名 `.sha256` | Backend、Gateway、验收用 CLI 客户端 |
| B：OpenCloudOS | `mmo-host-b.tar.gz` 及同名 `.sha256` | SceneHost；同一个镜像启动两个场景 |

这两个文件是 **Docker 镜像归档**，不是源码 zip，也不是 Windows 的 exe。镜像中已经包含 Linux 程序和运行库；目标机不需要 Go、GCC 或 CMake。Docker 支持 [save 导出镜像](https://docs.docker.com/reference/cli/docker/image/save/) 和 [load 加载 gzip 压缩归档](https://docs.docker.com/reference/cli/docker/image/load/)。

## 1. 确认目标架构

在两台 VPS 执行 `uname -m`。`x86_64` 对应 `linux/amd64`，`aarch64` 对应 `linux/arm64`。构建机可以是 Windows，但 Docker Desktop 必须运行 **Linux containers**；不能把 Windows 编译的 exe 放进 Linux 镜像。[平台兼容说明](https://docs.docker.com/build/building/multi-platform/)

本机先执行：

```powershell
docker version
docker info --format '{{.OSType}}'
```

必须看到 Docker 的 Server 信息，第二条输出 `linux`。只有 Client 信息时，先启动 Docker Desktop。

## 2. Windows 本机打包

在项目根目录的 PowerShell 执行。下面的版本号是示例，新版本使用新的标签；脚本会拒绝覆盖已完成的同名包：

```powershell
python scripts/package_images.py --tag mvp-20261104 --platform linux/amd64 --debian-mirror mirrors.ustc.edu.cn
```

镜像源按构建机实际网络选择；官方源可省略 `--debian-mirror`。Go 模块访问慢时可指定 `--goproxy`。脚本不依赖任何实际 `.env` 文件，不需要 VPS 地址或内部密钥。它会依次构建四个镜像，检查镜像已加载到当前 Docker 引擎，并核对目标架构后导出压缩包。

输出目录：

```text
dist/docker/mvp-20261104-linux-amd64/
  mmo-host-a.tar.gz
  mmo-host-a.tar.gz.sha256
  mmo-host-b.tar.gz
  mmo-host-b.tar.gz.sha256
  manifest.json
```

`manifest.json` 记录源码提交、是否有未提交修改、镜像 ID、平台、文件大小和 SHA-256。详细构建日志保存在 `output/image-build-*/`。构建或导出失败时脚本会停止；不要把 `.partial` 临时文件上传到 VPS。

## 3. 上传文件

使用 WinSCP/SFTP，把 A 包及其校验文件放到 Ubuntu 的 `~/mmo-images/`，B 包及其校验文件放到 OpenCloudOS 的同名目录。上传使用公网 SSH 地址或可达的 VPN 地址；本机未接入云私网时，不能直接访问 VPS 的 `10.x.x.x` 地址。

已有 `~/mmo-server` 源码目录可以继续使用，执行 `git pull --ff-only` 更新部署说明即可。镜像包不上传到 Git 仓库；GitHub 继续保存源码、配置模板和文档。

## 4. 两台分别加载镜像

**Ubuntu：**

```bash
cd ~/mmo-images
sha256sum -c mmo-host-a.tar.gz.sha256
sudo docker load -i mmo-host-a.tar.gz
sudo docker image inspect mmo/backend:mvp-20261104 mmo/gateway:mvp-20261104 mmo/client:mvp-20261104 --format '{{json .RepoTags}} {{.Os}}/{{.Architecture}}'
```

**OpenCloudOS：**

```bash
cd ~/mmo-images
sha256sum -c mmo-host-b.tar.gz.sha256
sudo docker load -i mmo-host-b.tar.gz
sudo docker image inspect mmo/scene:mvp-20261104 --format '{{json .RepoTags}} {{.Os}}/{{.Architecture}}'
```

只有校验成功才加载；加载成功后再检查镜像标签。加载与启动使用同一个 Docker 引擎，不混用指向不同引擎的用户/context。

## 5. 配置文件仍需准备

镜像包不包含运行密钥、数据库或玩家凭据。已有实际 env 文件继续保留，只把两台文件中的版本改为包的实际标签：

```dotenv
MMO_IMAGE_TAG=mvp-20261104
```

如果 env 文件不存在，先从相应模板创建并按 [两机部署文档](DEPLOYMENT_TWO_HOSTS.md) 填写。不要把模板中的示例 IP 当作自己的地址：

```bash
# 仅在 Ubuntu 上执行
test -f deploy/two-host-a.env || (umask 077; cp deploy/two-host-a.env.example deploy/two-host-a.env)
chmod 600 deploy/two-host-a.env

# 仅在 OpenCloudOS 上执行
test -f deploy/two-host-b.env || (umask 077; cp deploy/two-host-b.env.example deploy/two-host-b.env)
chmod 600 deploy/two-host-b.env
```

这些命令从 `~/mmo-server` 运行。A 填 B 的私网/VPN地址，B 绑定自己的真实私网/VPN地址；两台使用同一随机 `INTERNAL_TOKEN`。更新镜像不需要重新生成密钥，也不改变数据库卷。公网玩家入口按部署文档配置 TLS。

## 6. 先启动 B，再启动 A

首次部署先在 **OpenCloudOS** 执行：

```bash
cd ~/mmo-server
sudo docker compose --env-file deploy/two-host-b.env -f deploy/two-host-b.compose.yaml -p mmo-host-b config --quiet
sudo docker compose --env-file deploy/two-host-b.env -f deploy/two-host-b.compose.yaml -p mmo-host-b up -d --no-build --pull never --wait
sudo docker compose --env-file deploy/two-host-b.env -f deploy/two-host-b.compose.yaml -p mmo-host-b ps
```

确认两个场景健康，再在 **Ubuntu** 执行：

```bash
cd ~/mmo-server
sudo docker compose --env-file deploy/two-host-a.env -f deploy/two-host-a.compose.yaml -p mmo-host-a config --quiet
sudo docker compose --env-file deploy/two-host-a.env -f deploy/two-host-a.compose.yaml -p mmo-host-a up -d --no-build --pull never --wait
curl --fail http://127.0.0.1:8080/readyz
```

`--no-build` 禁止编译，`--pull never` 禁止从镜像仓库拉取；标签填错或未加载时会直接失败。随后按两机文档从 A 的两个容器分别检查 B 的 9001/9002，并验证真实客户端流程。

A 包包含客户端，可直接在 Ubuntu 上进行私网/回环测试，无需现场编译客户端：

```bash
sudo docker run --rm -it --pull never --network host \
  --mount type=volume,source=mmo-client-tokens,target=/app/tokens \
  mmo/client:mvp-20261104 -addr 127.0.0.1:7000 -name alice -token-file /app/tokens/alice.token
```

网关未监听回环地址时，将 `-addr` 改为 A 的实际绑定地址。TLS 入口再使用证书对应的域名及客户端 TLS 参数。

## 7. 后续更新和资源观察

新版本在本机构建新标签，上传、校验、加载后，按 [维护窗口更新步骤](GITHUB_VPS.md#5-以后从-github-更新) 停机并备份，修改两边的 `MMO_IMAGE_TAG`，先 B 后 A 重建容器。保留原来的 `BACKEND_VOLUME_NAME`；镜像包与数据库备份是不同的文件，镜像不会携带玩家存档。

运行后用以下命令观察实际资源使用：

```bash
sudo docker stats --no-stream
free -h
df -h
```

2 核 2 GB 可以作为这个 MVP 的试运行环境，是否满足实际玩家量需要压测。镜像导入和解压也需要磁盘空间，保留足够空间存放压缩包、展开后的镜像和数据库。成功启动不代表有自动容灾或容量保证。
