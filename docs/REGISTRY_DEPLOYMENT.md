# 镜像发布补充：GitHub Actions 与 GHCR

**VPS 部署请直接使用 [统一部署流程](DEPLOYMENT_TWO_HOSTS.md)**。本页用于维护者发布镜像、检查可见性和排查仓库访问。

镜像发布到 GitHub Container Registry（GHCR），源码继续放在 GitHub 仓库。两台 VPS 拉取已经编译好的 Linux 镜像，再启动容器，无需 Go、GCC 或 CMake。

| 镜像 | 部署位置 |
| --- | --- |
| `ghcr.io/dravenlove/mmo-server/backend:transport-20261106-v1` | `gateway`：Ubuntu |
| `ghcr.io/dravenlove/mmo-server/gateway:transport-20261106-v1` | `gateway`：Ubuntu |
| `ghcr.io/dravenlove/mmo-server/scene:transport-20261106-v1` | `scene`：OpenCloudOS；两个场景共用 |
| `ghcr.io/dravenlove/mmo-server/client:transport-20261106-v1` | 可选验收客户端 |

上述 `transport-20261106-v1` 标签已经 [发布成功](https://github.com/dravenlove/mmo-server/actions/runs/42219654315)，包含数值热更和内部连接等待修复，四个镜像已通过匿名拉取检查。新的应用版本使用新标签，或使用工作流默认生成的 `sha-提交号`；两台部署同一版本。

## 1. 发布镜像

仓库已经包含 [Publish Linux images 工作流](../.github/workflows/publish-images.yaml)。打开 [GitHub Actions](https://github.com/dravenlove/mmo-server/actions/workflows/publish-images.yaml)，点击 **Run workflow**，选择源码分支，填写版本标签。留空时生成 `sha-提交号`。

工作流在 GitHub 的 Ubuntu 构建机上依次构建四个 `linux/amd64` 镜像，执行隔离容器验收，全部通过后推送到 GHCR。构建消耗发生在 GitHub，不占用两台 VPS 的编译资源。本流程使用工作流临时的 `GITHUB_TOKEN` 及 `packages: write` 权限，不需要将个人 token 放入源码或 VPS。[GitHub 官方认证说明](https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-container-registry#authenticating-in-a-github-actions-workflow)

后续修改源码后，重新运行工作流并使用新的标签。若在本机自行构建后使用 `docker push`，仍需另行登录 GHCR 并获得镜像推送权限；仅有 Git 推送权限不等于可以推送镜像。

## 2. 首次发布：设置镜像公开

当前四个镜像已公开，VPS 可匿名拉取，无需重复调整。以下说明适用于以后首次创建的新镜像或复制到自己的仓库。

**源码仓库公开，不会自动让新建镜像公开。** GHCR 新镜像默认是 private，首次发布后需检查四个镜像的可见性：[GitHub 官方说明](https://docs.github.com/en/packages/learn-github-packages/configuring-a-packages-access-control-and-visibility#about-inheritance-of-access-permissions)。

在 GitHub 个人主页的 **Packages** 中，依次打开 `mmo-server/backend`、`mmo-server/gateway`、`mmo-server/scene`、`mmo-server/client`，进入 **Package settings → Change visibility → Public** 并按页面完成确认。以后向同一个镜像发布新标签无需重复设置。

公开的 GHCR 镜像支持匿名拉取，VPS 无需保存 GitHub token。[公开镜像拉取权限](https://docs.github.com/en/packages/learn-github-packages/configuring-a-packages-access-control-and-visibility#about-setting-visibility-and-access-permissions-for-packages)

## 3. VPS 检查网络

分别在两台 VPS 执行：

```bash
curl --connect-timeout 5 --max-time 15 -sS -o /dev/null -w 'HTTP=%{http_code}\n' https://ghcr.io/v2/
```

返回 `401` 表示已经连接到仓库，尚未携带认证信息。`curl -I` 使用 HEAD 请求，某些接口返回 `405`，仍说明请求已到达接口；请用上面的 GET 方式检查。连接超时、DNS 或证书错误需要先解决网络问题。

该检查只验证仓库入口，镜像层下载还可能访问其他分发地址；以实际拉取成功为准。如果 VPS 网络无法稳定下载，可继续使用 [离线 `gateway`/`scene` 镜像包](PREBUILT_IMAGES.md)，或将相同镜像同步到自己的国内镜像仓库，并修改 `MMO_IMAGE_PREFIX`。

## 4. VPS 使用已发布镜像

按照 [统一部署流程](DEPLOYMENT_TWO_HOSTS.md#fresh)创建实际配置、设置 `MMO_IMAGE_PREFIX` / `MMO_IMAGE_TAG`、配置 MySQL，再依次拉取并启动。完整的 scene / gateway 命令和容器客户端验收均维护在该文档中。

`MMO_IMAGE_PREFIX` 不包含最后的服务名和版本。发布镜像不携带 VPS 地址、内部密钥、数据库密码或玩家数据；这些由各主机的实际 env 与数据库提供。

后续更新使用 [统一更新流程](DEPLOYMENT_TWO_HOSTS.md#updates)。文档和部署命名调整可继续使用同一应用镜像，无需触发构建工作流。
