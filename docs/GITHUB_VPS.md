# GitHub 克隆与两台 VPS 部署

仓库：<https://github.com/dravenlove/mmo-server>，公开可读。两台 VPS 使用 HTTPS 克隆即可，不需要 GitHub 账号、访问令牌或 SSH 部署密钥。

| 机器 | 系统 | 部署内容 | 配置文件 |
| --- | --- | --- | --- |
| A | Ubuntu 24.04.4 LTS | Go Gateway、Go Backend、SQLite | `deploy/two-host-a.compose.yaml` |
| B | OpenCloudOS 9.6 | C++ scene-1、scene-2 | `deploy/two-host-b.compose.yaml` |

两边使用相同源码，分别在本机构建容器。宿主系统不需要安装 Go 或 C++ 编译器。以下安装说明面向新机器；已有 Docker 或其他容器服务时，先检查现有安装，避免混装或卸载正在使用的软件包。

## 1. 检查系统

分别登录两台 VPS：

```bash
cat /etc/os-release
uname -m
df -h
free -h
```

记录实际 CPU 架构，例如 `x86_64` 或 `aarch64`。在各机本地构建会使用本机架构；如果以后分发预构建镜像，要选择对应架构。两台系统名称不同不影响 HTTP 通信。

安装命令以下使用 `sudo`；已经用 root 登录时可去掉 `sudo`。先检查现有 Docker：

```bash
sudo docker version
sudo docker compose version
sudo docker buildx version
```

Docker 服务与这两个插件均可用时，直接进入第 3 节。需要支持 `docker compose up --wait`；不要使用旧的 Python 版 `docker-compose` 代替本项目命令。

## 2. 安装 Docker

### A：Ubuntu 24.04.4 LTS

Ubuntu 24.04 属于 Docker 官方支持的系统。新机器按 [Docker 官方 Ubuntu 安装说明](https://docs.docker.com/engine/install/ubuntu/) 配置仓库：

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates curl git openssl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

# Ubuntu 24.04 的发行代号是 noble；使用本机 CPU 架构。
printf 'Types: deb\nURIs: https://download.docker.com/linux/ubuntu\nSuites: noble\nComponents: stable\nArchitectures: %s\nSigned-By: /etc/apt/keyrings/docker.asc\n' "$(dpkg --print-architecture)" | sudo tee /etc/apt/sources.list.d/docker.sources >/dev/null

sudo apt-get update
sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
sudo systemctl enable --now docker
```

如果已有 `docker.io`、`podman-docker` 或其他容器运行时导致包冲突，按官方说明确认现有服务和数据后处理；不要把强制卸载作为首次安装步骤。

### B：OpenCloudOS 9.6

OpenCloudOS 9 使用自己的软件仓库。社区 EPOL9 提供 Docker CE；官方 FAQ 给出了 `epol-release` 的启用方法。这里使用 OpenCloudOS 软件源中的包及其依赖。来源：[OpenCloudOS 软件源说明](https://docs.opencloudos.org/faq/)、[Docker CE 发行说明](https://docs.opencloudos.org/release/v9.4/#76docker-ce-2801)、[EPOL9 软件包目录](https://mirrors.opencloudos.tech/epol/9/Everything/x86_64/os/Packages/)。

```bash
sudo dnf install -y git curl ca-certificates openssl epol-release
sudo dnf makecache

# 先确认当前镜像的软件源能找到 Engine、CLI 和两个插件。
sudo dnf info docker-ce docker-ce-cli docker-buildx-plugin docker-compose-plugin
sudo dnf install -y docker-ce docker-ce-cli docker-buildx-plugin docker-compose-plugin
sudo systemctl enable --now docker
```

包的实际版本以服务器仓库为准。若 `dnf info` 找不到某个包，先检查 `sudo dnf repolist` 是否启用了 EPOL9，并按上面的官方软件源说明修复。这里不把 OpenCloudOS 的 `9.6` 当作 CentOS 仓库路径，也不关闭 GPG 校验来绕过安装错误。已有可用的 Docker/Moby 时，保留现有引擎并补齐兼容插件即可。

### 两台都验证

```bash
sudo docker version
sudo docker compose version
sudo docker buildx version
sudo docker compose up --help
sudo docker run --rm hello-world
```

确认 Docker 显示客户端和服务端版本，Compose 帮助包含 `--wait`，测试容器能正常退出。拉镜像失败时先解决出站网络和镜像源问题，再构建本项目。后文使用 `sudo docker`；两机部署文档中的 `docker` 也可同样加 `sudo`。

如果镜像拉取很快，但构建在 `apt-get` 或 `go mod download` 停留很久，参见 [构建慢的排查与换源](BUILD_TROUBLESHOOTING.md)。这几步使用不同的下载来源，需要分别处理。

## 3. 两台都克隆代码

如果 Docker 已预装而跳过了第 2 节，仍需在 A 安装 `git curl openssl`（`sudo apt-get install -y git curl openssl`），在 B 安装它们（`sudo dnf install -y git curl openssl`）。

在各自登录用户的目录中执行：

```bash
git clone https://github.com/dravenlove/mmo-server.git
cd mmo-server
git status --short
git rev-parse HEAD
```

两台输出的完整提交号应该相同。若克隆时间不同导致提交号不同，先在较旧的一台执行 `git pull --ff-only`，再核对；不要混用两个版本部署。

源码公开不代表运行配置公开。仓库只带空密钥模板，实际的 `deploy/*.env`、数据库、玩家 token、证书私钥和备份被 `.gitignore` 排除。只在服务器上创建实际 env 文件，不使用 `git add -f` 上传它们。

## 4. 填配置并启动：先 B，后 A

按 [两机部署文档](DEPLOYMENT_TWO_HOSTS.md) 的第 1–4 节执行，顺序为：

1. 确认 A、B 的私网或加密 VPN 地址互通。示例 `10.20.0.11`、`10.20.0.22` 必须替换成实际地址。
2. 运行一次 `openssl rand -hex 32`，把同一个随机密钥填入两台 env 的 `INTERNAL_TOKEN`。
3. B 创建 `deploy/two-host-b.env`，设置自己的 `SCENE_BIND_IP`，启动两个场景。
4. A 创建 `deploy/two-host-a.env`，设置指向 B 的 `SCENE_HOST_IP` 和 A 的 `GATEWAY_BIND_IP`，启动 Backend 与网关。
5. 从 A 的网关与 Backend 容器分别检查 B 的两个场景，再用客户端测试移动、切换和奖励。

跨机内部 HTTP 应在私网或加密 VPN 上运行；共享密钥本身不加密流量。公网玩家入口使用 TLS，详见 [TLS 配置](DEPLOYMENT.md#4-网关-tls)。数据库只存在 A 的持久卷里，不需要在 B 安装数据库。

端口安排：A 的 TCP 7000 给玩家；8080 仅回环健康检查，8081 不发布到宿主机。B 的 TCP 9001/9002 绑定私网/VPN 地址，只允许 A 的实际出站来源和受控运维来源访问。云安全组与 Docker 转发规则要一起验证；仅配置 UFW/firewalld 不足以证明容器端口已受限，参见 [Docker 防火墙说明](https://docs.docker.com/engine/network/packet-filtering-firewalls/)。

## 5. 以后从 GitHub 更新

本 MVP 使用**维护窗口内整体更新**。场景重建会丢失内存实时状态，需要客户端重新登录；不能把更新流程当作无损滚动升级。

先在两台机器的仓库根目录分别更新源码；`git status --short` 应为空，若有源码修改，先另行保存并处理，不要执行强制覆盖。实际 `.env` 文件被忽略，会留在本机。

```bash
git status --short
git rev-parse HEAD
git pull --ff-only
git rev-parse HEAD
```

记下更新前的提交号用于排查，并核对更新后两台提交号相同。如果新版本更改 env 模板，比较新模板并手动补充字段，保留原随机密钥、地址、TLS 设置和 `BACKEND_VOLUME_NAME`。Git 更新不会替你迁移配置或数据库。

**先构建好两边的镜像，再开始停机。** 构建失败时，原容器仍可继续运行。B 上：

```bash
sudo docker compose --env-file deploy/two-host-b.env -f deploy/two-host-b.compose.yaml -p mmo-host-b config --quiet
sudo docker compose --env-file deploy/two-host-b.env -f deploy/two-host-b.compose.yaml -p mmo-host-b build
```

A 上：

```bash
sudo docker compose --env-file deploy/two-host-a.env -f deploy/two-host-a.compose.yaml -p mmo-host-a config --quiet
sudo docker compose --env-file deploy/two-host-a.env -f deploy/two-host-a.compose.yaml -p mmo-host-a build
```

两边构建成功后，先在 **A** 停入口和业务：

```bash
sudo docker compose --env-file deploy/two-host-a.env -f deploy/two-host-a.compose.yaml -p mmo-host-a stop gateway
sudo docker compose --env-file deploy/two-host-a.env -f deploy/two-host-a.compose.yaml -p mmo-host-a stop backend
```

再在 **B** 停场景：

```bash
sudo docker compose --env-file deploy/two-host-b.env -f deploy/two-host-b.compose.yaml -p mmo-host-b stop
```

此时按 [两机备份说明](DEPLOYMENT_TWO_HOSTS.md#5-停止数据与已有部署迁移) 备份 A 的完整数据库卷。然后先启动 **B**，确认健康：

```bash
sudo docker compose --env-file deploy/two-host-b.env -f deploy/two-host-b.compose.yaml -p mmo-host-b up -d --no-build --wait
sudo docker compose --env-file deploy/two-host-b.env -f deploy/two-host-b.compose.yaml -p mmo-host-b ps
```

最后启动 **A**：

```bash
sudo docker compose --env-file deploy/two-host-a.env -f deploy/two-host-a.compose.yaml -p mmo-host-a up -d --no-build --wait
sudo docker compose --env-file deploy/two-host-a.env -f deploy/two-host-a.compose.yaml -p mmo-host-a ps
curl --fail http://127.0.0.1:8080/readyz
```

按两机文档重新做容器间连通检查和客户端重登验收。整个过程保留命名卷，**不要执行 `down -v`**。若升级包含数据库格式变化，旧代码不一定能直接读取新数据库；恢复需同时考虑对应源码版本、配置和停机备份，不要只回退 Git 提交。

## 6. 当前已验证与待验证

本机已通过 Windows 原生构建、Go 测试与静态检查、C++ CTest 2/2 和四进程端到端测试 15/15；Go 程序完成 Linux amd64 交叉编译，七组 Compose 配置展开通过。详情见 [验证记录](VALIDATION.md)。

当前没有登录两台 VPS 执行安装与部署，本机也没有可用的 Docker daemon。上面的系统安装命令依据官方资料整理；OpenCloudOS 包名已核对官方 EPOL9 目录，但尚未在你的 VPS 安装。Docker 镜像构建、C++ Linux 编译、容器启动及两机网络仍需按文档在目标机器验收。
