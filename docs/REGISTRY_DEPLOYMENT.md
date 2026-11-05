# 发布到网上，VPS 直接拉取镜像

镜像发布到 GitHub Container Registry（GHCR），源码继续放在 GitHub 仓库。两台 VPS 拉取已经编译好的 Linux 镜像，再启动容器，无需 Go、GCC 或 CMake。

| 镜像 | 部署位置 |
| --- | --- |
| `ghcr.io/dravenlove/mmo-server/backend:mvp-20261105-mysql` | A：Ubuntu |
| `ghcr.io/dravenlove/mmo-server/gateway:mvp-20261105-mysql` | A：Ubuntu |
| `ghcr.io/dravenlove/mmo-server/scene:mvp-20261105-mysql` | B：OpenCloudOS；两个场景共用 |
| `ghcr.io/dravenlove/mmo-server/client:mvp-20261105-mysql` | 可选验收客户端 |

以上标签必须已经发布成功才能拉取。新的发布使用新版本号，或使用工作流默认生成的 `sha-提交号` 标签；部署时两台使用同一发布版本。

## 1. 发布镜像

仓库已经包含 [Publish Linux images 工作流](../.github/workflows/publish-images.yaml)。打开 [GitHub Actions](https://github.com/dravenlove/mmo-server/actions/workflows/publish-images.yaml)，点击 **Run workflow**，选择源码分支，填写版本标签。留空时生成 `sha-提交号`。

工作流在 GitHub 的 Ubuntu 构建机上依次构建四个 `linux/amd64` 镜像，执行隔离容器验收，全部通过后推送到 GHCR。构建消耗发生在 GitHub，不占用两台 VPS 的编译资源。本流程使用工作流临时的 `GITHUB_TOKEN` 及 `packages: write` 权限，不需要将个人 token 放入源码或 VPS。[GitHub 官方认证说明](https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-container-registry#authenticating-in-a-github-actions-workflow)

后续修改源码后，重新运行工作流并使用新的标签。若在本机自行构建后使用 `docker push`，仍需另行登录 GHCR 并获得镜像推送权限；仅有 Git 推送权限不等于可以推送镜像。

## 2. 首次发布：设置镜像公开

**源码仓库公开，不会自动让新建镜像公开。** GHCR 新镜像默认是 private，首次发布后需检查四个镜像的可见性：[GitHub 官方说明](https://docs.github.com/en/packages/learn-github-packages/configuring-a-packages-access-control-and-visibility#about-inheritance-of-access-permissions)。

在 GitHub 个人主页的 **Packages** 中，依次打开 `mmo-server/backend`、`mmo-server/gateway`、`mmo-server/scene`、`mmo-server/client`，进入 **Package settings → Change visibility → Public** 并按页面完成确认。以后向同一个镜像发布新标签无需重复设置。

公开的 GHCR 镜像支持匿名拉取，VPS 无需保存 GitHub token。[公开镜像拉取权限](https://docs.github.com/en/packages/learn-github-packages/configuring-a-packages-access-control-and-visibility#about-setting-visibility-and-access-permissions-for-packages)

## 3. VPS 检查网络

分别在两台 VPS 执行：

```bash
curl --connect-timeout 5 --max-time 15 -sS -o /dev/null -w 'HTTP=%{http_code}\n' https://ghcr.io/v2/
```

返回 `401` 表示已经连接到仓库，尚未携带认证信息。`curl -I` 使用 HEAD 请求，某些接口返回 `405`，仍说明请求已到达接口；请用上面的 GET 方式检查。连接超时、DNS 或证书错误需要先解决网络问题。

该检查只验证仓库入口，镜像层下载还可能访问其他分发地址；以实际拉取成功为准。如果 VPS 网络无法稳定下载，可继续使用 [离线 A/B 镜像包](PREBUILT_IMAGES.md)，或将相同镜像同步到自己的国内镜像仓库，并修改 `MMO_IMAGE_PREFIX`。

需要连接指定 MySQL 时，另按 [MySQL 配置](MYSQL.md) 在主机 A 填写数据库地址与账号；场景机不需要这些数据库凭据。

## 4. 修改两台部署配置

在两台的 `~/mmo-server` 执行 `git pull --ff-only`。已有实际 env 文件继续使用；不存在时先按 [两机部署文档](DEPLOYMENT_TWO_HOSTS.md) 从对应模板创建，并填写真实地址和同一随机内部密钥。

Ubuntu 编辑 `deploy/two-host-a.env`，OpenCloudOS 编辑 `deploy/two-host-b.env`，两台都设置：

```dotenv
MMO_IMAGE_PREFIX=ghcr.io/dravenlove/mmo-server
MMO_IMAGE_TAG=mvp-20261105-mysql
```

`MMO_IMAGE_PREFIX` 不包含最后的服务名和版本。`INTERNAL_TOKEN` 两台保持相同，A 的 `SCENE_HOST_IP` 指向 B 的实际私网/VPN地址，B 的 `SCENE_BIND_IP` 绑定同一地址。网关、TLS 和原来的数据库卷配置继续按两机文档填写。

## 5. 先拉取，先启动 B 再启动 A

**B：OpenCloudOS**，同一场景镜像只需拉取一次：

```bash
cd ~/mmo-server
sudo docker compose --env-file deploy/two-host-b.env -f deploy/two-host-b.compose.yaml -p mmo-host-b config --quiet
sudo docker compose --env-file deploy/two-host-b.env -f deploy/two-host-b.compose.yaml -p mmo-host-b pull --policy always scene1
sudo docker compose --env-file deploy/two-host-b.env -f deploy/two-host-b.compose.yaml -p mmo-host-b up -d --no-build --pull never --wait
sudo docker compose --env-file deploy/two-host-b.env -f deploy/two-host-b.compose.yaml -p mmo-host-b ps
```

确认 B 的两个场景健康后，**A：Ubuntu** 执行：

```bash
cd ~/mmo-server
sudo docker compose --env-file deploy/two-host-a.env -f deploy/two-host-a.compose.yaml -p mmo-host-a config --quiet
sudo docker compose --env-file deploy/two-host-a.env -f deploy/two-host-a.compose.yaml -p mmo-host-a pull --policy always backend gateway
sudo docker compose --env-file deploy/two-host-a.env -f deploy/two-host-a.compose.yaml -p mmo-host-a up -d --no-build --pull never --wait
curl --fail http://127.0.0.1:8080/readyz
```

这里先明确执行 `pull`，再用 `--no-build --pull never` 启动，只会运行本地已经拉取的镜像；镜像缺失或标签错误时直接失败，不会退回现场编译。VPS 宿主必须与本次发布的 `linux/amd64` 架构匹配：`uname -m` 应为 `x86_64`。

接着按两机文档检查 A 容器访问 B 的 9001/9002 端口，并完成登录、移动、领取奖励及跨场景传送流程。

验收客户端也可直接拉取，在 Ubuntu 上运行：

```bash
sudo docker pull ghcr.io/dravenlove/mmo-server/client:mvp-20261105-mysql
sudo docker run --rm -it --pull never --network host \
  --mount type=volume,source=mmo-client-tokens,target=/app/tokens \
  ghcr.io/dravenlove/mmo-server/client:mvp-20261105-mysql \
  -addr 127.0.0.1:7000 -name alice -token-file /app/tokens/alice.token
```

网关仅绑定私网网卡时，把 `-addr` 改成 A 的实际绑定地址。公网 TLS 入口使用证书对应的域名和客户端 TLS 参数。

## 6. 后续更新

先在 GitHub 发布并确认新标签成功，然后提前在两台 VPS 拉取新镜像。已有玩家和数据时，按 [维护窗口更新步骤](GITHUB_VPS.md#5-以后从-github-更新) 停机并备份；两边 env 更新为同一新标签，先 B 后 A 启动并验收。保留原来的数据库卷、密钥和证书配置。

拉取镜像不会覆盖玩家数据库，重建容器仍需保留原卷。不要使用 `down -v` 更新实际部署。
