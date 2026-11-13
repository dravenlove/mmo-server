#pragma once

#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace native::script {
using Json = nlohmann::json;

struct Limits {
    std::size_t memory_bytes = 1024 * 1024;
    std::uint64_t instructions = 50000;
    unsigned host_calls = 16;
    unsigned effects = 1;
    std::size_t max_source_bytes = 65536;
    std::size_t max_context_bytes = 8192;
    unsigned max_context_depth = 16;
    unsigned max_context_nodes = 1024;
    std::size_t max_string_bytes = 4096;
    std::size_t max_output_bytes = 1024;
};

// 错误不包含脚本正文、Lua栈、请求正文或玩家凭据。
class ScriptError : public std::runtime_error {
public:
    const std::string code;
    explicit ScriptError(std::string reason);
};

// 版本与源码不可修改；可由多个房间持有各自固定的shared_ptr<const Script>。
// 每次plan使用全新预算VM，所有持久玩法状态仍由C++ ECS持有。
class Script {
public:
    Script(std::string version, std::string source, Limits limits = {});
    const std::string& version() const noexcept { return version_; }
    const std::string& sha256() const noexcept { return sha256_; }
    Json plan(const Json& context) const;
private:
    const std::string version_;
    const std::string source_;
    const Limits limits_;
    const std::string sha256_;
};
}
