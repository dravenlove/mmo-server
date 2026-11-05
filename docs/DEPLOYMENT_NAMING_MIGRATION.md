# 已有部署切换到角色命名

日常文档统一使用主机角色：

| 主机角色 | 内容 | Compose 文件 | env 文件 | 项目名 |
| --- | --- | --- | --- | --- |
| `gateway` | Go Gateway + Backend | `deploy/gateway-host.compose.yaml` | `deploy/gateway-host.env` | `mmo-gateway` |
| `scene` | 两个 C++ 场景 | `deploy/scene-host.compose.yaml` | `deploy/scene-host.env` | `mmo-scene` |

文件名包含 `-host`，与仅部署单个服务的 `gateway.compose.yaml`、`scene.compose.yaml` 区分。下文中的旧路径和旧项目名仅用于识别已经运行的部署。

新部署直接按 [统一部署流程](DEPLOYMENT_TWO_HOSTS.md#fresh)使用新模板。如果以前使用的是 `two-host-*.env` 和 `mmo-host-*`，需要在维护窗口按以下顺序切换；不要直接用新项目名启动另一组容器。

## 1. 两台更新代码，并复制原配置

两台仓库都先执行 `git pull --ff-only`。这次是部署文件与文档命名调整，继续使用已经加载的同版本镜像即可。

在 **`gateway` 主机**复制已有配置；已有新文件时不会覆盖：

```bash
cd ~/mmo-server
test -f deploy/gateway-host.env || (umask 077; cp deploy/two-host-a.env deploy/gateway-host.env)
chmod 600 deploy/gateway-host.env
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml -p mmo-gateway config --quiet
```

在 **`scene` 主机**复制已有配置：

```bash
cd ~/mmo-server
test -f deploy/scene-host.env || (umask 077; cp deploy/two-host-b.env deploy/scene-host.env)
chmod 600 deploy/scene-host.env
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml -p mmo-scene config --quiet
```

这里复制的是原来的实际 env，保留内部密钥、IP、镜像标签、MySQL 账号和 `BACKEND_VOLUME_NAME`。不要用空密钥模板覆盖原配置。若旧实际文件不存在，先定位正在使用的配置再继续。

## 2. 先停入口和业务，再停场景

在 **`gateway` 主机**使用旧项目名停止现有服务。Compose 文件已经改名，因此这里用新文件配合旧项目名管理原容器：

```bash
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml -p mmo-host-a stop gateway
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml -p mmo-host-a stop backend
```

再在 **`scene` 主机**移除旧项目的容器和网络：

```bash
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml -p mmo-host-b down
```

此时按使用的数据库方式备份：SQLite 按 [SQLite 备份补充](DEPLOYMENT.md#7-备份与恢复)备份原卷，MySQL 按数据库维护流程备份。然后在 **`gateway` 主机**移除旧项目的容器和网络：

```bash
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml -p mmo-host-a down
```

这里不带 `-v`，保留持久卷。新模板继续使用已有的 `BACKEND_VOLUME_NAME`；默认值仍为 `mmo-two-host-backend-data`，避免仅因项目改名而切换到空数据库。MySQL 模式也继续使用原地址和库名。

首次用新项目启动时，Compose 可能提示已有卷带着旧项目标签。已在独立测试卷中验证会继续复用同一卷；按卷名核对即可，不要为清除提示删除原卷。

## 3. 先启动 scene，再启动 gateway

在 **`scene` 主机**：

```bash
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml -p mmo-scene up -d --no-build --pull never --wait
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml -p mmo-scene ps
```

确认两个场景健康后，在 **`gateway` 主机**：

```bash
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml -p mmo-gateway up -d --no-build --pull never --wait
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml -p mmo-gateway ps
curl --fail http://127.0.0.1:8080/readyz
```

随后按 [两机部署文档](DEPLOYMENT_TWO_HOSTS.md) 检查容器间连通，并使用原客户端凭据登录，确认角色、场景和金币保持一致。更新会断开客户端，需要重新登录。

## 4. 离线镜像包

后续打包使用 `mmo-gateway.tar.gz` 和 `mmo-scene.tar.gz`。已经生成的旧包仍包含相同 Docker 镜像标签，可以继续加载；包文件改名不需要重新编译应用，也不影响数据库。
