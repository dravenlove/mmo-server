# 登录后状态中断或移动超时

客户端 ping 成功只验证客户端到 Gateway 的往返；移动和状态还需要 Gateway 调用 Scene，保存和会话检查还会经过 Backend/MySQL。

| 客户端提示 | 代码中的含义 |
| --- | --- |
| `service response interrupted` | Gateway 已取得内部 HTTP 响应头，但读取正文失败；可能是正文截断、连接中断或请求期限到达 |
| `service unavailable or timed out; retry with the same operation id` | 内部 HTTP 调用尚未取得完整响应头就失败；可能是连接失败、断开或超时 |

Scene 调用期限为 2 秒。这些提示不能单独区分网络问题、工作线程等待或服务重启，也不能证明动作没有执行。不要给施法、迁移、领取等有副作用的操作随意生成新的编号重试。

## 先读取现场信息

**Ubuntu gateway 主机：**

```bash
cd ~/mmo-server
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml ps -a
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml logs --since 10m --tail 100 gateway backend
```

下面的测试从正在运行的 Backend 容器读取实际配置的 Scene 地址，分别测小响应和配置响应。不会注册角色、移动、修改规则或输出内部凭据；不要启用 `set -x` 或给 curl 添加 `-v`。

```bash
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml exec -T backend sh -c '
for entry in $(printf "%s" "$SCENES" | tr "," " "); do
  scene=${entry%%=*}
  url=${entry#*=}
  for attempt in 1 2 3; do
    printf "%s health attempt=%s " "$scene" "$attempt"
    curl --noproxy "*" --silent --show-error --connect-timeout 2 --max-time 4 \
      --output /dev/null --write-out "http=%{http_code} first=%{time_starttransfer}s total=%{time_total}s bytes=%{size_download}\n" "$url/healthz"
    printf "%s config-status attempt=%s " "$scene" "$attempt"
    curl --noproxy "*" --silent --show-error --connect-timeout 2 --max-time 4 \
      --header "Content-Type: application/json" --header "X-Internal-Token: $INTERNAL_TOKEN" \
      --data "{}" --output /dev/null \
      --write-out "http=%{http_code} first=%{time_starttransfer}s total=%{time_total}s bytes=%{size_download}\n" "$url/v1/config/status"
  done
done'
```

配置状态返回 404 时，实际运行的 Scene 仍不支持热更接口；这里的 404 不代表连接超时。`first` 很低而 `total` 持续超时，说明至少要进一步检查正文传输；不能仅看 ping 或 `/healthz` 就认定大响应也正常。

**OpenCloudOS scene 主机：**

```bash
cd ~/mmo-server
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml ps -a
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml logs --since 10m --tail 100 scene1 scene2
sudo docker stats --no-stream
```

## 安装本次程序修复

镜像版本 `transport-20261106-v1` 已[发布成功](https://github.com/dravenlove/mmo-server/actions/runs/42219654315)，四个镜像的匿名拉取与源码标签核对均通过。按以下顺序操作。两台的实际 env、共享密钥、数据库参数和热更配置继续保留；命令只修改镜像前缀和版本。客户端需要重新登录，近期位置以最后成功保存的快照为准。

**1. Ubuntu gateway 主机：提前拉取。**

```bash
cd ~/mmo-server &&
git pull --ff-only origin main &&
test -f deploy/gateway-host.env &&
sed -i -e 's|^MMO_IMAGE_PREFIX=.*|MMO_IMAGE_PREFIX=ghcr.io/dravenlove/mmo-server|' \
  -e 's/^MMO_IMAGE_TAG=.*/MMO_IMAGE_TAG=transport-20261106-v1/' deploy/gateway-host.env &&
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml config --quiet &&
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml config --images &&
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml pull --policy always backend gateway
```

确认列出的两个镜像都是 `ghcr.io/dravenlove/mmo-server/...:transport-20261106-v1`，拉取成功后再继续。

**2. OpenCloudOS scene 主机：提前拉取。**

```bash
cd ~/mmo-server &&
git pull --ff-only origin main &&
test -f deploy/scene-host.env &&
sed -i -e 's|^MMO_IMAGE_PREFIX=.*|MMO_IMAGE_PREFIX=ghcr.io/dravenlove/mmo-server|' \
  -e 's/^MMO_IMAGE_TAG=.*/MMO_IMAGE_TAG=transport-20261106-v1/' deploy/scene-host.env &&
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml config --quiet &&
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml config --images &&
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml pull --policy always scene1
```

两个 Scene 共用同一镜像，只需拉取一次。**两台全部拉取成功后才开始停机**；任何一步失败时先处理该错误，不继续后面的启动步骤。

**3. 回到 Ubuntu gateway 主机：关闭入口并保存快照。**

```bash
cd ~/mmo-server &&
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml stop gateway &&
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml stop backend
```

按原有备份安排保存 MySQL 数据。这个修复不修改数据库结构。

**4. OpenCloudOS scene 主机：重建两个场景容器。**

```bash
cd ~/mmo-server &&
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml up -d --no-build --pull never --force-recreate --wait &&
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml ps
```

必须看到 `scene1`、`scene2` 都使用新版本且 `healthy`。

**5. Ubuntu gateway 主机：重建业务与网关容器。**

```bash
cd ~/mmo-server &&
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml up -d --no-build --pull never --force-recreate --wait &&
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml ps &&
curl --fail --connect-timeout 3 --max-time 5 http://127.0.0.1:8080/readyz
```

两个服务健康后，重新登录现有桌面客户端，连续移动并施法测试。仍报错时立即读取上面的日志；新 Gateway 的 `upstream request failed` 记录会指出具体 Scene/Backend、接口、耗时和中断阶段。

## 已修复的空闲连接占用问题

旧 Scene 的 HTTP 线程池上限为 8，而每条保活连接在响应后还会占住工作线程最多 5 秒。保留 8 条这样的空闲连接后，第 9 条请求会等待，超过 Gateway 的 2 秒期限；不需要游戏 Tick 本身过载就能触发。

修复让 Scene 在每次响应后关闭内部 HTTP 连接，及时释放工作线程；仍保留有界线程池、命令队列、超时和动作归属校验。客户端到 Gateway 的游戏 TCP 连接保持原样。代价是每个内部 Scene 请求增加一次 TCP 建连，后续扩大在线人数前需要重新测容量。

回归测试保留 12 个已完成请求的客户端，再请求鉴权视图。旧程序在第 9 个请求超时，新程序通过；这证明修复了一个可触发超时的缺陷，不证明所有现场正文中断都由它引起。

新 Gateway 的内部传输失败日志包含 `service`、`endpoint`、`path`、`phase`、`elapsed_ms` 和底层错误：`request` 表示尚未取得完整响应头，`read_body` 表示读取正文失败，`decode` 表示响应格式无效。日志不记录请求正文、响应正文或鉴权头，也不自动重放失败的 POST。

首次安装程序修复需要更新镜像并重建容器，不能通过战斗数值热更加载。按[两台主机维护更新](DEPLOYMENT_TWO_HOSTS.md#updates)操作；保留实际 env、现有数据库卷、配置目录和玩家凭据。镜像发布和实际验证记录见[验证记录](VALIDATION.md)。
