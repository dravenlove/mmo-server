# Docker 构建慢：定位下载来源

先看日志停在哪个 `RUN`，不要仅凭总时长判断编译性能。保留当前构建，在另一 SSH 终端做只读检查；确定原因后再决定是否中断。新一次构建可使用 Compose 的 `--progress plain`，显示完整输出。

| 日志位置 | 当前在做什么 | 优先检查 |
| --- | --- | --- |
| `FROM …`、`resolve`、镜像层下载 | 拉取 Docker 基础镜像 | 镜像仓库访问、Docker 代理或镜像加速配置 |
| `RUN apt-get …`，输出 `Get:`、`Ign:`、`Err:` | 从 Debian 软件源下载索引和安装包 | 容器访问 Debian 源的网络 |
| `RUN go mod download` | 下载 Go 模块及校验信息 | `GOPROXY`、模块代理与校验服务的连通性 |
| `go build`、`cmake --build`、`cc1plus` | 编译源码 | CPU、内存、交换空间和磁盘压力 |

本项目的运行镜像和 C++ 构建镜像来自 `debian:bookworm-slim`。即使宿主是 Ubuntu 或 OpenCloudOS，构建日志里的 `apt-get` 也在 Debian 容器内执行。修改宿主机的 apt/dnf 源不会自动修改镜像里的源。

现场曾出现：基础镜像约 1.8 秒完成、上下文约 0.1 秒完成，`apt-get` 安装编译工具约 3060 秒仍在输出软件包下载记录。这定位到软件包下载/安装阶段，尚未进入 C++ 编译；不能据此判定另一台 Go 机器也停在相同步骤。

另一份现场日志中，Backend、Gateway 已经完成 `exporting` 和 `unpacking to mmo/…:dev`，但同一构建仍在等待 `scene2` 的 apt 下载（约 4401 秒）。这说明构建列表包含 Go 与场景服务，而不是 Go 编译未完成。按两机方案，`gateway` 的配置只应包含 `backend`、`gateway`；根目录 Compose 是四服务同机演示配置。

## 1. 从 VPS 比较软件源

在构建机器的另一个 SSH 终端执行下面的只读下载测试。它只下载并丢弃仓库索引，不安装软件，也不改系统配置：

```bash
for host in deb.debian.org mirrors.ustc.edu.cn mirrors.tuna.tsinghua.edu.cn; do
  printf '\nTesting %s\n' "$host"
  curl -L --fail --connect-timeout 5 --max-time 20 -o /dev/null \
    -w 'HTTP=%{http_code} seconds=%{time_total} bytes_per_second=%{speed_download}\n' \
    "http://$host/debian/dists/bookworm/InRelease"
done
```

选择在这台 VPS 上成功且明显更快的来源。国内镜像不一定适合海外机器；两台 VPS 可以选不同的镜像源。小索引的下载只是初筛，不保证所有安装包的吞吐；如果宿主很快而构建仍慢，继续检查 Docker 的 DNS、网络和代理，不能只根据宿主测速下结论。

下面用 `mirrors.ustc.edu.cn` 举例。其 Debian 帮助确认 Bookworm 容器使用 `/etc/apt/sources.list.d/debian.sources`，替换 `deb.debian.org` 可同时切换主仓库和安全更新仓库。[中科大镜像配置说明](https://mirrors.ustc.edu.cn/help/debian.html)

## 2. 确认替代源可用后重新构建

如果旧构建仍卡在下载，可在它所在的终端按 `Ctrl+C` 中断构建。已完成且仍匹配的层可以复用，但当前未完成的 apt 层需要重新执行；切换软件源也会使相关安装层重新执行。保留现有 Docker 缓存，不加 `--no-cache`。

在仓库根目录更新代码：

```bash
git pull --ff-only
```

### `scene`：OpenCloudOS 场景机

编辑已经填写好密钥和地址的 `deploy/scene-host.env`，新增或修改这一行（只填域名，不填协议或路径）：

```dotenv
DEBIAN_MIRROR=mirrors.ustc.edu.cn
```

不要用模板覆盖现有 env 文件。然后运行：

```bash
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml -p mmo-scene --progress plain build scene1
```

`scene1` 和 `scene2` 共用同一个镜像标签，构建一次即可。在这次首次部署中，构建成功后启动两个容器：

```bash
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml -p mmo-scene up -d --no-build --wait
```

### `gateway`：Ubuntu 网关与业务机

日志出现 `exporting`、`naming` 或 `unpacking`，不足以证明当前 Docker 引擎中已有可复用的镜像。`gateway` 可以中断这次不需要的场景构建，但是否能跳过 Go 构建，应以同一台机器、同一个 Docker 引擎上的镜像检查结果为准：

```bash
sudo docker image inspect mmo/backend:dev mmo/gateway:dev --format '{{json .RepoTags}}'
```

上例与当前 `MMO_IMAGE_TAG=dev` 对应；修改过标签时用实际标签。两个镜像都存在且版本与本次部署一致时，`gateway` 不必再次构建，配置好 `gateway` 的 env 并等 `scene` 健康后，直接执行本节的 `up --no-build`。新的换源参数只影响后续构建。

若返回 `No such image`，先核对检查命令与先前构建是否在同一台 VPS、使用同一个 Docker 引擎。普通用户和 `sudo` 可能因各自的 context 或 Rootless 配置连接不同引擎，不能仅凭是否带 `sudo` 判定。可比较下面输出中的引擎 ID；这些命令不修改配置：[Docker context 说明](https://docs.docker.com/engine/manage-resources/contexts/)。

```bash
docker info --format 'name={{.Name}} id={{.ID}}'
sudo docker info --format 'name={{.Name}} id={{.ID}}'
```

如果镜像存在于预期的另一个引擎，后续检查、构建与运行统一使用那个引擎。如果目标引擎确实没有镜像，先完成下方 `gateway` 的构建，成功后重新检查镜像，再使用 `up --no-build`；不要反复尝试启动不存在的本地镜像。`gateway` 的构建只包含 Backend 和 Gateway，不需要等待 C++ 场景。

如果 `gateway` 的日志同样停在 `apt-get`，在 `deploy/gateway-host.env` 中设置它实测更快的 `DEBIAN_MIRROR`。如果停在 Go 模块下载，则需要另行选择可访问的 Go 模块代理，并设置 `GOPROXY`；默认是 `https://proxy.golang.org,direct`。这项设置不关闭 `go.sum` 或校验数据库验证，参见 [Go 模块环境变量说明](https://go.dev/ref/mod#environment-variables)。

```bash
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml -p mmo-gateway --progress plain build
```

本次首次部署中，在 `scene` 已经健康、`gateway` 的镜像构建成功后启动 `gateway`：

```bash
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml -p mmo-gateway up -d --no-build --wait
```

以上启动步骤针对尚未投入使用的首次部署。已有玩家和数据时，沿用 [维护窗口更新步骤](GITHUB_VPS.md#5-以后从-github-更新) 完成停机、备份和启动。

### 不使用 Compose 构建时

对应的直接构建方式：

```bash
sudo docker build --progress=plain -f Dockerfile.scene \
  --build-arg DEBIAN_MIRROR=mirrors.ustc.edu.cn -t mmo/scene:dev .
```

镜像标签要与部署 env 中的 `MMO_IMAGE_TAG` 一致。配置参数的语义见 [Docker 构建参数说明](https://docs.docker.com/build/building/variables/)。

## 3. 下载完成后仍慢

进入真正的编译步骤后，再在另一终端检查资源：

```bash
free -h
nproc
df -h
vmstat 1 5
```

持续发生交换、内存不足或编译器被系统杀死时，应针对资源处理；换软件源不会加速已经在本地执行的编译。请保留最后的错误文本和失败阶段。

## 4. 配置边界

- `DEBIAN_MIRROR` 作用于所有使用 apt 的构建/运行阶段；根 Compose 与两机 Compose 都传递该参数，默认仍为 `deb.debian.org`。
- 该参数保留基础镜像原有的 HTTP 软件源形式和 Debian `Signed-By` 签名校验。精简镜像尚未安装 CA 时，直接切换 HTTPS 源可能无法验证证书；不要通过关闭签名或证书校验绕过错误。[清华镜像引导说明](https://mirrors.tuna.tsinghua.edu.cn/help/debian/)
- APT 增加了两次重试以及 HTTP/HTTPS 的 30 秒连接/数据等待超时；这是网络等待超时，不是整个构建的总时限。持续有少量数据的慢连接仍可能耗时很久。[APT 网络配置](https://manpages.debian.org/bookworm/apt/apt.conf.5.en.html)
- 本机 Docker Desktop 恢复后已使用中科大源完成四个 Linux 镜像的实际构建及容器验证；VPS 网络和换源效果仍需在目标机器验收。小配置 VPS 可以直接使用 [本机构建的镜像包](PREBUILT_IMAGES.md)，避免在目标机编译。
