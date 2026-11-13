# 固定的 C++ 依赖

源码随仓库保存，CMake 配置时无需下载。升级时同时更新文件、许可证和校验值。

| 文件 | 上游版本 | SHA-256 |
| --- | --- | --- |
| httplib.h | [cpp-httplib v0.64.0](https://github.com/yhirose/cpp-httplib/tree/v0.64.0) | 805169049e6a6e9ba7729d7e01d14ced825029c88a277586602a6b38df4d5357 |
| nlohmann/json.hpp | [nlohmann/json v3.12.0](https://github.com/nlohmann/json/tree/v3.12.0) | aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63 |
| entt/entt.hpp | [EnTT v3.13.2](https://github.com/skypjack/entt/tree/v3.13.2) | 31be51708c7325ecb7877182fa9ee68a20fb2121ce6bcd6c1687506f0fb1c8b5 |

以上三种依赖使用 MIT 许可证，完整文本保存在相邻 LICENSE 文件；EnTT 的许可证位于 [entt/LICENSE](entt/LICENSE)，版本与校验值另记于 [entt/VERSION.sha256](entt/VERSION.sha256)。EnTT 已用于 Scene 的权威组件存储，具体边界见 [房间交接](../docs/ROOMS_LUA_ECS.md)。Go 依赖由 go.mod/go.sum 固定。

Lua 不在此目录内 vendoring。`native/script/` 使用系统 Lua 5.4 官方 C API，CMake 通过 pkg-config 要求 `lua5.4 >= 5.4` 且 `< 5.5`；Ubuntu 构建依赖 `liblua5.4-dev`，镜像运行依赖 `liblua5.4-0`。`Dockerfile.native` 安装这些系统包，不使用 LuaJIT，也不在配置时下载 Lua 源码。实际系统包版本与镜像 revision/digest 以对应构建记录为准。
