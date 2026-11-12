# 移动入口延迟修复

2026-11-12，针对 `webgl-movement-v1` 的 `/ws` 入口。`ping` 和移动确认只代表请求往返；手感还取决于输入何时反映在下一份完整状态，以及客户端如何显示该状态。

## 已复现的问题

旧 Gateway 每 200ms 收到一次定时事件，又用该事件时间与“上次事件时间 + 200ms”比较。时间戳的纳秒偏差会跳过一个完整周期，公网实际出现 200/400ms 交替的状态间隔。22 份移动状态采样约 3.59 次/秒、最大间隔 401ms。使用已有受控账号发送八次移动、等候首份坐标变化状态，中位 192ms、最大 452ms；该测量不包含浏览器显示和平滑时间。

## 修复与参数

每个定时事件发送一份最新状态，去掉重复的周期比较；失败时仍退避一秒，不追发积压的历史快照。会话核对和每两秒保存仍按实际当前时间执行。

`GATEWAY_STATE_HZ` 可设为整数 1～20；未设置时，`movement` 默认为 20，其余 Profile 为 5。游戏主机 Compose 用 `WEBGL_STATE_HZ=20` 配置移动 Gateway。模拟 Tick 仍为 20Hz；输入格式、250ms 输入失效、最多四条移动在途、会话/归属隔离和 AOI 完整视图保持原约定。

移动状态因此每 50ms 发送一次，停止后也继续发送。其他战斗入口维持原数据量，正常周期不再因时间戳偏差漏发。`/ws` 仍只提供移动/AOI；元素技能使用 `/elemental/ws`，弹道和墙体使用 `/combat/ws`。

Gateway 的 `/healthz` 增加 `state_hz`，用于检查实际配置。推送是服务端下行，不计入原客户端每秒 40 条请求的限制。完整视图的带宽随人数和同步频率增长；20Hz 是此两机联调配置，不是容量承诺。

## 单独更新网关

游戏主机 env 新增 `MMO_GATEWAY_IMAGE_TAG`，留空时沿用 `MMO_IMAGE_TAG`。修复版本为 `latency-20261112-v1`；原 Backend/Scene 可继续使用 `elemental-20261112-v1`。保留原 env、密钥和数据，只增加或更新以下两项：

```dotenv
MMO_GATEWAY_IMAGE_TAG=latency-20261112-v1
WEBGL_STATE_HZ=20
```

镜像发布成功后，在游戏主机执行：

```bash
cd ~/mmo-server
git pull --ff-only
sudo docker compose --env-file deploy/game-host.env -f deploy/game-host.compose.yaml config --quiet
sudo docker compose --env-file deploy/game-host.env -f deploy/game-host.compose.yaml pull gateway-webgl
sudo docker compose --env-file deploy/game-host.env -f deploy/game-host.compose.yaml up -d --no-deps --no-build --pull never --wait gateway gateway-webgl gateway-combat gateway-elemental
curl --fail http://127.0.0.1:8083/healthz
```

四个 Gateway 共用同一镜像，一次拉取足够。更新短暂断开连接，重新登录即可；Backend、五个 Scene、WSS bridge、Caddy 和独立 Auth 不需要重启。更新前保存原 env 和 Compose，用 `--no-deps` 避免同时重建 Backend/Scene。回退时恢复原 Gateway 标签和频率，沿用同样的网关更新命令，不恢复数据库旧备份。

## 验收范围

回归包含可控定时输入的真实连接测试：相邻事件相差一纳秒、迟到事件均产生一份最新状态；另验证频率默认值、可配置值与非法值拒绝。发布后重新测量状态间隔、移动回执和“输入→首份变化状态”，结果记录在 [VALIDATION.md](VALIDATION.md)。

客户端代码和 Unity 构建不在此修复中改动。当前客户端本地源码对位置还有 100～200ms 平滑，且不做本机移动预测；剩余的视觉延迟需按实际网页版本分别评估。服务端状态到达时间不能直接当作浏览器按键到首帧时间。
