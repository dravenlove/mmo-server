# 战斗数值热更与 Demo 验收

本轮可在玩家保持连接时修改技能和怪物数值。C++ 场景在 Tick 命令队列内校验并整体切换配置；不重启容器、不重新登录，也不清空角色状态。它适合测试平衡参数，不包含 Lua、C++ 代码替换、增加技能 ID、改变技能行为或修改玩家生命/法力上限。

完整示例：[默认配置](../configs/combat.default.json)、[Demo 配置](../configs/combat.demo.json)。本轮目标镜像标签为 `hotreload-20261106-v1`，镜像前缀为 `ghcr.io/dravenlove/mmo-server`；当前仍待发布工作流完成。升级前先核对 [验证与发布记录](VALIDATION.md)，确认该标签已发布且可拉取。已执行测试及真实 VPS 的验收范围也以该记录为准。

<a id="initial-upgrade"></a>

## 首次启用：维护升级一次

从旧战斗版升级需要一次维护。之后发布兼容的数值配置无需断开玩家。先记录两台旧镜像标签、备份数据库，并在两台更新源码：

```bash
cd ~/mmo-server
git pull --ff-only
```

在 **scene 主机**运行以下命令。实际 `deploy/scene-host.env` 必须已存在，并已包含正确的密钥和部署参数；工具不会从模板创建残缺 env。

```bash
sudo python3 scripts/combat_config.py init
```

`init` 创建 `deploy/combat-config/current.json`，首次使用默认配置；已有文件会保留。默认目录下，env 中空缺的 `COMBAT_CONFIG_FILE` 会填为 `/app/config/current.json`。修改 env 前会生成权限为 `600` 的 `scene-host.env.<时间>.<随机值>.bak.env` 备份，修改后的 env 也设为 `600`。原有非空配置路径、目录不匹配或重复变量会拒绝自动修改，需要人工核对。

默认配置目录是 `deploy/combat-config`。新版 Compose 将**整个目录**只读挂载到 `/app/config`，而不是单独挂载 `current.json`：

```yaml
volumes:
  - type: bind
    source: ${COMBAT_CONFIG_DIR:-./combat-config}
    target: /app/config
    read_only: true
```

工具通过原子替换文件发布；目录挂载能让容器重新打开替换后的文件。单文件绑定可能继续指向旧文件，不能沿用。配置中不存放密码或内部令牌。

确认新标签已发布后，将**两台实际 env** 中的 `MMO_IMAGE_TAG` 改为 `hotreload-20261106-v1`，保留正确的镜像前缀、数据库设置、密钥及持久卷名。已有 env 不会随模板更新。先拉取全部镜像：

scene 主机：

```bash
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml config --images
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml pull --policy always scene1
```

gateway 主机：

```bash
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml config --images
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml pull --policy always backend gateway
```

`scene1`、`scene2` 使用同一个场景镜像。两台都拉取成功后，按 [维护停止与恢复顺序](DEPLOYMENT_TWO_HOSTS.md#operations)操作：gateway 主机先停入口，再停 Backend；scene 主机再停场景。恢复时先启动 scene 并等健康，再启动 gateway 项目的 Backend 和入口。保留数据库及持久卷，不使用 `down -v`。

```bash
# gateway 主机：先执行
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml stop gateway
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml stop backend
```

```bash
# scene 主机：入口和 Backend 停止后执行
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml stop
sudo docker compose --env-file deploy/scene-host.env -f deploy/scene-host.compose.yaml up -d --no-build --pull never --wait
sudo python3 scripts/combat_config.py status
```

```bash
# gateway 主机：scene 健康后执行
sudo docker compose --env-file deploy/gateway-host.env -f deploy/gateway-host.compose.yaml up -d --no-build --pull never --wait
curl --fail http://127.0.0.1:8080/readyz
```

## 在线试用与数值回滚

更新并启动新版桌面客户端，登录一次后保持连接。在 scene 主机的仓库目录执行：

```bash
sudo python3 scripts/combat_config.py status
sudo python3 scripts/combat_config.py publish configs/combat.demo.json
sudo python3 scripts/combat_config.py status
```

默认操作同一 scene 主机上的 `scene1`、`scene2`。`status` 返回两个场景各自的版本、代次、应用 Tick 和完整配置；`consistent: true` 表示它们与 `current.json` 一致。全局选项必须放在子命令前，例如自定义项目名：

```bash
sudo python3 scripts/combat_config.py --project-name mmo-scene status
```

不重新登录，观察以下变化：

| 观察项 | 默认 `default-v1` | 发布 `demo-v2` 后 |
| --- | --- | --- |
| 客户端配置版本 | `default-v1` | 当前场景推送变为 `demo-v2` |
| 新一次斩击 | 18 伤害 | 36 伤害，目标剩余生命不足时按实际扣除量显示 |
| 四种技能提示 | 默认伤害、消耗、范围、冷却 | 随服务端技能目录更新，提示显示 Demo 数值 |
| 狼最大生命 | 80 | 仅在下次刷新或脱战重置后变为 120 |

正在战斗、待刷新或尚未触发重置的怪物继续持有旧参数；不会因发布配置突然回血或改变攻击数值。怪物的 `config_version` 可用于区分它持有的版本与场景当前版本。旧怪物死亡仍按其持有版本等待刷新：默认狼首次可能仍要等约 8 秒，刷新后才采用 Demo 的 120 生命及之后的 3 秒刷新参数。

角色现有生命、法力、剩余冷却、动作序号和成功动作缓存保持原样；新施法采用新数值。已有 12 秒恢复技能冷却不会被 Demo 的 5 秒配置截短。重试同一成功动作仍返回原结果，包括原施法的 `config_version`。

回切上一次成功发布前的数值：

```bash
sudo python3 scripts/combat_config.py rollback
sudo python3 scripts/combat_config.py status
```

`rollback` 把 `previous.json` 作为一次新发布，成功后代次增加。它是**数值回滚**：不会撤销已造成的伤害、治疗、死亡，也不会还原角色剩余 CD。工具只保留一层 `previous.json`；再次回滚会切回刚才的配置。需要保留多个版本时，另行保存配置文件，并用新的、可辨认的版本名发布。

## 完整配置规则

文件必须是至多 64 KiB 的 UTF-8 JSON 对象，禁止重复键、未知字段、未知技能/怪物 ID、缺失字段及 NaN/Infinity。以下字段全部必填；整数项不接受 `18.0` 这样的 JSON 浮点数。可从默认文件复制，修改数值并换一个新 `version`。

| 顶层字段 | 类型与要求 |
| --- | --- |
| `schema_version` | JSON 整数，固定为 `1` |
| `version` | 1～64 个 ASCII 字母、数字、`.`、`_`、`-`；同一版本名必须保持相同内容 |
| `skills` | 对象，恰好包含 `strike`、`whirlwind`、`charge`、`mend` |
| `monsters` | 对象，恰好包含 `wolf`、`elite` |

每个技能对象恰好包含以下五项：

| 字段 | 类型 | 闭区间 |
| --- | --- | --- |
| `damage` | 整数 | 0～10000 |
| `heal` | 整数 | 0～180 |
| `mp_cost` | 整数 | 0～100 |
| `range` | 有限数值 | 0～100 |
| `cooldown_ms` | 整数，毫秒 | 0～600000 |

技能名称、目标类型和行为固定：三个攻击技能的 `heal` 必须为 `0`；`mend` 的 `damage` 和 `range` 必须为 `0`。突进仍接近至约 1.5 距离并短晕，旋风斩仍为自身周围群攻。玩家上限保持 180 生命、100 法力。

每个怪物对象恰好包含以下五项：

| 字段 | 类型 | 闭区间 |
| --- | --- | --- |
| `max_hp` | 整数 | 1～1000000 |
| `damage` | 整数 | 0～180 |
| `move_speed` | 有限数值 | 0.01～100 |
| `attack_interval_ms` | 整数，毫秒 | 100～600000 |
| `respawn_ms` | 整数，毫秒 | 1000～600000 |

## 发布一致性与故障处理

管理工具先核对两个场景与 `current.json` 一致，分别执行完整预检和代次校验，再备份为 `previous.json`、原子替换 `current.json`，依次通知两个场景重新加载并核对结果。`expected_generation` 使用比较并交换（CAS）拒绝过期发布；单个场景全量校验通过才一次切换，失败不半应用。

两个场景**依次**生效，存在短暂不同版本窗口，不保证跨节点同时生效。发布中失败时，工具仅在能确认文件和场景仍属于本次操作的情况下恢复旧文件、补偿已更新节点。补偿本身也是新一次配置切换，代次可能继续增加。即使补偿完成，失败命令仍返回非零，先查看输出和 `status` 再决定下一步。

如果输出 `manual_recovery_required: true`，或工具被强杀、机器掉电、存在其他管理员并发修改，先核对 `current.json`、`previous.json` 与两个场景的实际状态，不要直接重发或覆盖未知版本。工具使用 `deploy/combat-config/.combat-config.lock` 排除同目录内的并发操作，不会自动删除已有锁。确认没有工具进程持锁后，才人工处理残留锁并运行 `status`；直接调用管理 HTTP 或修改文件的其他进程不会受此锁约束。必要时按维护流程停机，以确认过的配置恢复。

单个进程最多记住 64 个版本名，包含启动版本；同名不同内容返回冲突，达到上限后拒绝新版本，不淘汰旧记录。重复发布当前相同内容幂等，不增加代次；回到历史同内容版本允许并增加代次。版本历史、`generation` 和 `applied_tick` 都是进程内状态，重启后从启动文件重建，代次重新从 1 开始；它们不是持久发布审计日志。

新版恢复玩家快照时，四项剩余 CD 统一允许 0～600000 毫秒，不再以当前配置 CD 为上限，因此缩短技能 CD 后仍能重连、迁移和恢复旧剩余 CD。旧 `combat` 二进制仍按旧技能表校验，可能拒绝新版产生的更长 CD；不能把工具的数值回滚当作应用二进制回退，也不能无条件换回旧镜像。

## 内部接口与开发验收

以下都是场景内部 POST 接口，必须带 `X-Internal-Token`；玩家 TCP 不开放配置管理操作。正式部署通过工具读容器内令牌，不把令牌放入宿主机命令参数。

| 接口 | 请求 | 作用 |
| --- | --- | --- |
| `/v1/config/status` | `{}` | 返回 `config,version,generation,applied_tick` |
| `/v1/config/validate` | `{config:完整配置,expected_generation?:整数}` | 检查 schema、版本同名冲突、历史容量和 CAS；返回候选配置、`valid,current_version,generation,applied_tick,would_change`，不修改状态或历史 |
| `/v1/config/reload` | `{expected_generation?:整数}` | 只读取启动时固定的 `COMBAT_CONFIG_FILE`，成功返回当前状态及 `changed`；请求不能指定路径或配置 |
| `/v1/config/apply` | `{config:完整配置,expected_generation?:整数}` | 仅用于内存调试；成功返回当前状态及 `changed`，不写文件，重启回到文件配置 |

不设置 `COMBAT_CONFIG_FILE` 时使用内置默认配置，`reload` 返回 409；指定文件但启动时无法读取或内容非法会退出。常见管理错误：400 表示配置或请求无效，401 表示内部认证失败，409 表示代次/版本冲突或未启用文件，503 表示版本历史容量等限制，504 表示排队响应超时。超时不保证操作未生效，必须先核对状态。

原生隔离验收（Windows PowerShell）：

```powershell
.\scripts\build.ps1
python scripts/smoke_combat.py --hot-reload
python scripts/test_desktop.py
```

镜像发布后，使用 SQLite / MySQL 两种隔离容器验收：

```bash
python3 scripts/smoke_docker.py --image-prefix ghcr.io/dravenlove/mmo-server --tag hotreload-20261106-v1 --hot-reload
python3 scripts/smoke_docker.py --image-prefix ghcr.io/dravenlove/mmo-server --tag hotreload-20261106-v1 --database mysql --hot-reload
```

本机构建镜像可改用对应的 `--image-prefix`、`--tag`。`--hot-reload` 包含原有战斗验收，并检查在线数值切换、旧 CD、缓存、怪物延迟换参、管理工具及回滚。这些测试使用隔离端口与测试库，不连接实际部署；测试结果和发布证明由 [验证记录](VALIDATION.md)统一维护。
