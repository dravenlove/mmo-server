# 系统准备补充：Docker 安装与 GitHub 克隆

本页仅补充 Ubuntu / OpenCloudOS 系统准备。**正式部署入口是 [统一部署流程](DEPLOYMENT_TWO_HOSTS.md)**，已安装好 Docker 时直接从那里开始。

仓库：[dravenlove/mmo-server](https://github.com/dravenlove/mmo-server)，公开可读。两台 VPS 使用 HTTPS 克隆即可，不需要 GitHub 账号、访问令牌或 SSH 部署密钥。应用镜像从 GHCR 拉取，宿主机不需要安装 Go、GCC 或 CMake。

以下安装命令用于新机器；已有 Docker 或其他容器服务时，先检查现有安装，避免混装或卸载正在使用的软件包。

## 1. 检查系统

分别登录两台 VPS：

```bash
cat /etc/os-release
uname -m
df -h
free -h
```

本次已发布镜像为 `linux/amd64`，目标机应输出 `x86_64`。其他架构需另外构建对应镜像。两台系统名称不同不影响容器之间通信。

安装命令以下使用 `sudo`；已经用 root 登录时可去掉 `sudo`。先检查现有 Docker：

```bash
sudo docker version
sudo docker compose version
sudo docker buildx version
```

Docker 服务与 Compose 插件均可用时，直接进入第 3 节；Buildx 用于构建，VPS 仅拉取运行时无需单独使用它。需要支持 `docker compose up --wait`；不要使用旧的 Python 版 `docker-compose` 代替本项目命令。

## 2. 安装 Docker

### `gateway`：Ubuntu 24.04.4 LTS

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

### `scene`：OpenCloudOS 9.6

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

确认 Docker 显示客户端和服务端版本，Compose 帮助包含 `--wait`，测试容器能正常退出。拉镜像失败时先解决出站网络和镜像源问题，再部署应用。统一部署流程使用 `sudo docker`。

VPS 直接拉取应用镜像即可。只有需要自行构建时，才查看 [构建慢的排查与换源](BUILD_TROUBLESHOOTING.md)。

## 3. 两台都克隆代码

如果 Docker 已预装而跳过了第 2 节，仍需在 `gateway` 安装 `git curl openssl`（`sudo apt-get install -y git curl openssl`），在 `scene` 安装它们（`sudo dnf install -y git curl openssl`）。

在各自登录用户的目录中执行：

```bash
git clone https://github.com/dravenlove/mmo-server.git
cd mmo-server
git status --short
git rev-parse HEAD
```

两台输出的完整提交号应该相同。若克隆时间不同导致提交号不同，先在较旧的一台执行 `git pull --ff-only`，再核对；不要混用两个版本部署。

源码公开不代表运行配置公开。仓库只带空密钥模板，实际的 `deploy/*.env`、数据库、玩家 token、证书私钥和备份被 `.gitignore` 排除。只在服务器上创建实际 env 文件，不使用 `git add -f` 上传它们。

## 4. 配置、启动和验收

回到 [统一部署流程的首次部署部分](DEPLOYMENT_TWO_HOSTS.md#fresh)：准备指定 MySQL，填写两份实际 env，先启动 scene，再启动 gateway，然后完成容器互通与客户端业务验收。本页不再重复另一套启动步骤。

## 5. 以后从 GitHub 更新

按 [统一部署流程的更新部分](DEPLOYMENT_TWO_HOSTS.md#updates)操作：更新源码和指定镜像标签，先拉取，再停机备份，按 scene → gateway 顺序恢复。保留原密钥、数据库、证书和卷名；`git pull` 只更新源码与模板，不会替你重建容器。

已运行旧项目名的部署先完成 [配置改名迁移](DEPLOYMENT_NAMING_MIGRATION.md)。

## 6. 验证范围

本机已完成 Windows 原生、Linux Docker、SQLite / MySQL 业务测试和四个 GHCR 镜像的匿名拉取，详见 [验证记录](VALIDATION.md)。系统安装命令按官方资料整理，真实 VPS 的安装、私网路由、账号权限、证书与容量仍需目标环境验收。
