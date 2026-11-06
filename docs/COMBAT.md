# PvE 战斗试炼场

这一版在原来的移动、账号和场景迁移上增加一套可玩的近战技能与怪物战斗。Go Gateway 接收操作并附带可信角色归属，C++ 场景的 Tick 线程判定距离、消耗、冷却、伤害与死亡。Windows 客户端显示权威结果。

## 1. 怎么玩

登录后，地图里能看到狼和精英守卫。点击活着的怪物选中目标；点击场景区域后使用 WASD / 方向键移动，数字键 1～4 释放技能，也可以点技能栏。Tab 切换可见目标。

| 按键 | 技能 | 效果 | 距离 / 范围 | 消耗 | 冷却 |
| --- | --- | --- | --- | --- | --- |
| 1 | 斩击 | 对选中怪物造成 18 点伤害 | 3.2 | 0 | 0.6 秒 |
| 2 | 旋风斩 | 对身边活怪造成 32 点伤害 | 自身半径 5 | 20 法力 | 5 秒 |
| 3 | 突进 | 接近选中怪物，造成 24 点伤害并短暂击晕 | 14 | 15 法力 | 7 秒 |
| 4 | 战意恢复 | 恢复自身最多 45 点生命 | 自身 | 25 法力 | 12 秒 |

角色生命上限为 180，法力上限为 100。处于激活状态且存活的角色随场景逻辑时间每秒恢复 6 法力。界面的冷却倒计时是显示提示，能否施放仍由服务器决定；超出距离、缺少目标、法力不足、冷却未结束或角色死亡时会拒绝操作。

两个场景各有三只狼和一个精英。怪物受到攻击后追击并反击，脱战后回巢并恢复生命，死亡后约 8 秒刷新。它们不主动袭击未参与战斗的测试角色。角色死亡后不能移动或施法，等待约 5 秒后可点击复活回到原点，恢复全部生命和法力，但不会清空剩余技能冷却。

这一版是战斗试炼场，怪物不发放金币或经验；原有一次性欢迎奖励仍由 Backend 的数据库事务处理。这里没有新增装备、等级和任务系统。

## 2. 状态如何保存

- 场景保存实时生命、法力、冷却、动作序号及怪物仇恨；网络线程只排队，Tick 线程执行修改。
- 新会话重置输入序号，但不会通过重连补满生命/法力或刷新冷却。
- 冻结和待激活角色不能战斗，也不会继续被怪物攻击；这期间法力恢复、技能冷却和复活倒计时暂停。迁移的冻结快照、准备、归属提交和激活携带完整玩家战斗状态。
- Backend 新建 `combat_snapshots` 表，保存归属与会话校验后的战斗快照；旧账号、金币、领取记录和迁移表保留。SQLite 和 MySQL 均使用新增表，不要求手工 ALTER 旧表。
- 正常检查点约每 2 秒一次，正常断开也会尝试保存。进程故障恢复可能回退最近一次检查点之后的战斗变化，不能把它当作逐次技能的持久化事务。
- 冷却以剩余逻辑时间保存；恢复时保守继承剩余冷却。怪物、仇恨和战斗事件属于场景内存，场景重启会重新生成怪物。

施法和复活使用同一会话内的单调动作序号。重复已成功动作不会重复结算；当前进程最近一次成功动作的相同序号、相同内容可返回缓存结果，更旧或内容冲突的序号会被拒绝。动作结果缓存不随迁移或检查点保存；目标场景或重启进程恢复后，会保留动作序号高水位并拒绝旧动作，不能借重试重复结算。网络超时后不要换一个新序号自动重发同一次技能。

## 3. 两台 VPS 更新

本次应用镜像标签为 `combat-20261106-v1`，前缀仍为 `ghcr.io/dravenlove/mmo-server`。以对应 GitHub Actions 发布成功和实际拉取成功为准。

先备份当前 MySQL 数据，并记录两台原镜像标签。两台都更新源码，保留实际 env 中的内部密钥、数据库账号、地址和卷名：

```bash
cd ~/mmo-server
git pull --ff-only
```

编辑两台各自的 env，把 `MMO_IMAGE_TAG` 改为 `combat-20261106-v1`。已有 env 不会跟随模板自动更改。

scene 提前拉取：

```bash
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml config --images
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml pull --policy always scene1
```

gateway 提前拉取：

```bash
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml config --images
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml pull --policy always backend gateway
```

两台都拉取成功后，按 [维护停止与恢复顺序](DEPLOYMENT_TWO_HOSTS.md#operations)更新：先停 gateway 入口、Backend，再停 scene；先启动 scene 并等健康，再启动 gateway。保留原数据库和持久卷，Backend 仍只运行一个实例。

新客户端连接旧服务端仍可做基础联调，但旧服务端不提供战斗数据。需要两台应用服务和桌面客户端都更新，才能完整验收技能。

## 4. 战斗验收

1. 登录并看到四种技能、血蓝及可见怪物。
2. 距离较远时点击斩击，应提示距离不足；突进靠近后可斩击。
3. 连续点仍在冷却的技能，应由服务器拒绝，不能连续造成伤害。
4. 怪物受击后追击反击，角色生命下降；战意恢复能恢复生命并消耗法力。
5. 怪物死亡后停止攻击，等待刷新。
6. 用精英测试死亡：死亡期间不能行动，等待后复活回出生点。
7. 在技能仍冷却时断开重连、切场景，确认冷却和法力没有被刷新。

开发机的隔离验收命令：

```powershell
.\scripts\build.ps1
python scripts/smoke_combat.py
python scripts/test_desktop.py
```

已有新镜像时，还可对 SQLite / MySQL 两种模式做容器验收：

```powershell
python scripts/smoke_docker.py --tag combat-20261106-v1 --combat
python scripts/smoke_docker.py --tag combat-20261106-v1 --database mysql --combat
```

这些脚本使用独立回环端口、随机密钥和测试库，不连接实际部署。已执行的结果以 [验证记录](VALIDATION.md)为准。
