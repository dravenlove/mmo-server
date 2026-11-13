# 可信 Lua 技能计划器

`native-script` 通过 Lua 5.4 C API 执行服务端发布的技能模块。客户端仍只发送技能 ID、参数与施法意图；不接受客户端上传 Lua。现有 C++ Scene 保留资源、动作序号、位置、碰撞、伤害和存档的权威。

执行顺序为：C++ 完成参数、资源和几何预检 → 固定版本 Lua 生成意图 → C++ 验证完整意图 → C++ 一次提交扣费、冷却、动作序号与实例。Lua 没有修改 ECS、发网络请求或操作数据库的 API；错误只能丢弃尚未提交的计划。

## C++ 接口

```cpp
#include "script/script.hpp"
auto module = std::make_shared<const native::script::Script>(version, source);
auto result = module->plan(context);
```

`Script(version, source, Limits{})` 构造时即在预算内加载模块、验证 Lua `plan` 函数；`version()`、`sha256()` 标识不可修改的源码版本。房间持有 `shared_ptr<const Script>` 固定版本，发布新模块只影响新房间。旧版本是否释放由房间管理器决定。

`context` 至少包含 `skill_id` 与 `definition.type`；Scene 还可提供规范化 `origin`、`aim`、`target`、`owner`、`tick` 和 `world_epoch`。输入先在 C++ 校验深度、大小、数值、技能 ID，并按值推入 Lua；Lua 修改局部上下文不会修改 Scene 或 C++ 原对象。Lua 整数保留有符号 64 位精度，超过范围的 JSON 无符号整数明确拒绝。

模块形式：

```lua
return {
    plan = function(ctx)
        if ctx.definition.type == "wall" then
            host.emit("wall", ctx.skill_id)
        else
            host.emit("projectile", ctx.skill_id)
        end
    end
}
```

可用宿主能力只有：

- `host.emit(type, skill_id)`：添加生成意图；当前只允许与规范定义相同的 `projectile` / `wall` 及技能 ID。
- `host.deny(reason)`：拒绝施法，reason 为 1～64 字节 ASCII slug。不能同时生成效果并拒绝。

输出为 `{"effects":[{"type":"projectile","skill_id":"client-bolt"}]}` 或 `{"denied":true,"reason":"walls_disabled"}`。旧施法回执只承诺一个实例，因此当前必须恰好一条效果；Lua 不改变客户端已注册的数值，也不扩大 C++ 行为集。多效果技能、Buff 或异步脚本需在协议与提交模型扩展后另做。

## 配额与隔离

默认上限：

| 项目 | 默认值 |
| --- | --- |
| Lua 堆内存 | 1 MiB |
| Lua 指令 | 50,000，100 条粒度 Hook |
| 暴露 C 函数/宿主调用总数 | 16 |
| 效果数 | 1 |
| 源码 | 64 KiB，仅文本模式 |
| C++ 输入 JSON | 8 KiB、深度 16、节点 1024、字符串 4096 字节 |
| 输出 JSON | 1024 字节 |

每次 `plan` 创建全新预算 VM，执行房间固定的源文本；脚本全局变量不跨调用保留。这样 emit 后异常、无限循环或内存耗尽不会把 Lua 全局变量的半成品带入下一次调用。持久玩法状态由 C++ ECS 保存，脚本不能用全局变量保存 Buff 或房间状态。同一 `Script` 可并发调用。

只暴露有限基础函数、常数时间数学函数与有界字符串函数。没有 `io/os/package/debug/coroutine/require/load/loadfile/dofile/pcall/xpcall/collectgarbage`，没有随机源、字符串模式匹配、rep/dump 或完整 table 库。哈希遍历 `pairs/next` 不开放，脚本使用有序数组的 `ipairs`；`tostring` 只接受原始值，不能观察对象地址制造隐式随机性。字符串元表同时替换，不能通过 `("x"):rep(...)` 绕过白名单。所有暴露 C 函数也计入调用配额，参数数量与字符串长度有上限；Lua 指令 Hook 本身不能限制宿主 C 函数的工作量。

宿主效果先写入固定大小计划缓冲；Lua 加载和执行使用 `lua_pcall`，Lua C 回调只持有平凡局部变量，避免 longjmp 越过 C++ 资源析构。上下文在受保护调用外展开成纯数据操作，再在保护范围内推入 Lua。

错误抛出 `ScriptError`，`code` 为稳定分类（如 `instruction_limit`、`memory_limit`、`host_limit`、`effect_limit`、`invalid_effect`、`invalid_context`、`invalid_module` 或 `script_error`）。错误不附带源码、Lua 栈或请求正文；Scene 应拒绝此次施法并保持 MP、CD、动作序号、对象及事件不变。

## 依赖与验证

Linux 通过 `pkg-config lua5.4` 链接系统 Lua，要求 `>=5.4,<5.5`，编译期检查 5.4 ABI、64 位整数与 double 数值。Ubuntu 24.04 的验证版本为 5.4.6；没有从漂移 master 下载源码。分配器、受保护调用与指令 Hook 使用 [Lua 官方 C API](https://www.lua.org/manual/5.4/manual.html)。

根构建添加 `add_subdirectory(script)` 后，`native-script-tests` / CTest `native-lua-planner` 验证：默认 manifest SHA、两种旧行为、能力白名单、配额、emit 后异常、全局状态不泄漏、并发/房间隔离、输入不可修改、64 位 tick 与固定版本。

默认 `scripts/gameplay/manifest.json` 对应 `skills-v1` / `skills_v1.lua`，SHA-256 为 `85031fa35e81ab00c4434dd838a61946b865ae02f006a597ad28c6dd62e746c7`。`scripts/gameplay/versions/skills-v1.json` 保留同一版本归档，file 以可信脚本根目录为基准；发布新版本不得覆盖归档源码，跨进程重建还应核对房间目录持久化的 SHA。文件读取、路径约束、manifest 校验、发布和旧版本保留由 RoomWorker 管理；本模块不读取客户端文件路径或修改活动房间版本。
