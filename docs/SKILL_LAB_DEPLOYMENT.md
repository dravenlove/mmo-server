# 参数化技能联调部署

镜像标签 `skill-lab-20261112-v1`。在现有游戏主机新增 `scene-skills` / `gateway-skills`，合计 12 个容器。新 C++ 场景为共享多人 `skill_lab` 模式，Gateway 为 `skill_lab` Profile；正式 Auth 继续在另一台主机。该配置没有将伤害或碰撞委托给客户端。

## 配置与启动

保留已有真实 env、密钥、MySQL、数据卷与鉴权隧道，不用 example 覆盖配置。此更新可保留原 Backend 和五个旧 Scene 镜像，只更新 Gateway、新技能 Scene 和原生 WSS bridge：

```dotenv
MMO_IMAGE_TAG=elemental-20261112-v1
MMO_GATEWAY_IMAGE_TAG=skill-lab-20261112-v1
MMO_SKILL_LAB_IMAGE_TAG=skill-lab-20261112-v1
COMBAT_STATE_HZ=20
SKILL_LAB_STATE_HZ=20
```

Backend 新增场景映射 `scene-skills=http://scene-skills:9006`，所以需要协调重启 Backend 载入配置；不能在同一数据库上同时运行两个 Backend。旧五个 Scene 无需重启。新端口只绑定回环：C++ 9006、Gateway 7007/8086、Native bridge 7008。

1. 等待 [镜像发布工作流](../.github/workflows/publish-images.yaml) 全部成功，再拉取 Gateway 和 Scene。海外镜像下载慢时可在可达主机 `docker save`，通过 SSH 传输；核对 SHA-256、平台和源码标签后加载。
2. 备份当前 env、Compose、Native 程序/元数据、三个 bridge 环境、Caddy 两个站点。维护窗口停止入口和 Gateway，完成最终角色 checkpoint，再停止 Backend；备份角色、战斗快照、归属迁移及账号表。
3. 更新源码与上述参数，先启动新 Scene 和 Backend；核对数据与旧 Scene 容器 ID，再启动五个 Gateway。
4. 更新 `/opt/mmo-websocket/websocket-bridge`，继承原环境中的精确 Origin。安装 [skills 环境模板](../deploy/websocket-skills.env.example) 到 `/etc/mmo-websocket-skills.env`，权限 600；安装 [服务模板](../deploy/mmo-websocket-skills.service.example)，并重新启动原三个桥接进程。
5. 在原 Caddy 的 IP/域名站点增加 `/skills/ws` 和 `/skills/healthz`，重写为 `/ws` / `/healthz` 并代理到 `127.0.0.1:7008`。保留旧路由、TLS、自动续期和静态页面；校验配置后重载。

具体路由见 [Caddy 模板](../deploy/ip-webgl.caddy.example)。健康检查：

`Type=simple` 的 systemd 启动返回不代表端口已经监听。桥接进程的就绪检查应在最多 10 秒内短间隔重试，通过后再最后重载 Caddy 新路由。本次首次立即检查触发回退，补充有界重试后完成切换；数据库没有恢复到旧备份。

```bash
sudo docker compose --env-file deploy/game-host.env -f deploy/game-host.compose.yaml ps
curl --fail http://127.0.0.1:9006/healthz
curl --fail http://127.0.0.1:8086/readyz
curl --fail http://127.0.0.1:7008/healthz
```

应分别显示 `skill_lab` 场景、Gateway ready、`webgl-skill-lab-v1`。Native bridge 自动协商 `permessage-deflate`，不要求修改客户端 JSON；不支持压缩的客户端仍可连接。解压后的消息大小仍受限制。

## 本机与发布检查

```powershell
./scripts/build.ps1
go test ./...
go vet ./...
ctest --test-dir build/scene -C Release --output-on-failure
python scripts/smoke_skill_lab.py
```

新原生 fixture 只创建随机回环端口、临时 SQLite、临时账号，退出后停止自己的进程。`smoke_docker.py --webgl` 同时检查移动、旧弹道、元素、参数化技能四套协议，并在 MySQL / 独立鉴权环境执行新技能注册与实例测试。不要对线上运行会注册测试账号的 fixture；线上只复用受控凭据。

## 延迟与回退边界

Gateway 逐连接串行异步存档，合并尚未执行的旧快照，正常关闭时排空；状态查询也限制为每连接最多一个在途，不占住技能请求处理循环。旧历史事件不在每份 `state` 重发，显式 `view` 仍能读取近期窗口。旧 `/combat/ws` 的同步频率可通过 `COMBAT_STATE_HZ` 调整到 20；完整实例集合与权威生命保持不变。

数据库存档不是技能生成事务的一部分；崩溃可能丢失尚未持久化的最近角色状态，自定义定义/冷却也不作为持久档保存。后续若进入正式经济系统，需要独立确定战斗日志与恢复承诺。

新场景归属会记录在角色表。有人进入新场景后，回退不能直接删除该场景映射；先迁出相关角色，或继续保留新 Scene/路由。不要用旧数据库备份覆盖开放入口后产生的新数据。

客户端接入与参数边界见 [SKILL_LAB_HANDOFF.md](SKILL_LAB_HANDOFF.md)。实际发布与公网测试见 [VALIDATION.md](VALIDATION.md)；Unity 构建、浏览器输入到首帧与服务端协议验收分别记录。
