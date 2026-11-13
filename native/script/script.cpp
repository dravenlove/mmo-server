#include "script/script.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <openssl/sha.h>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

static_assert(LUA_VERSION_NUM == 504, "The planner requires the Lua 5.4 ABI");
static_assert(sizeof(lua_Integer) >= sizeof(std::int64_t), "The planner requires 64-bit Lua integers");
static_assert(sizeof(lua_Number) >= sizeof(double), "The planner requires double-precision Lua numbers");

namespace native::script {
namespace {
constexpr unsigned instruction_quantum = 100;
constexpr const char* id_characters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-";

enum class Fault { none, memory, instructions, host, effects, phase, invalid_effect, module, script };
const char* fault_name(Fault value) noexcept {
    switch (value) {
    case Fault::memory: return "memory_limit";
    case Fault::instructions: return "instruction_limit";
    case Fault::host: return "host_limit";
    case Fault::effects: return "effect_limit";
    case Fault::phase: return "invalid_phase";
    case Fault::invalid_effect: return "invalid_effect";
    case Fault::module: return "invalid_module";
    default: return "script_error";
    }
}

bool valid_id(const std::string& text, std::size_t maximum) {
    return !text.empty() && text.size() <= maximum && text.find_first_not_of(id_characters) == std::string::npos;
}

std::string digest(const std::string& source) {
    unsigned char bytes[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(source.data()), source.size(), bytes);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : bytes) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

void validate_limits(const Limits& limits) {
    if (limits.memory_bytes < 65536 || limits.memory_bytes > 16 * 1024 * 1024 ||
        limits.instructions < 100 || limits.instructions > 1000000 || limits.host_calls < 1 || limits.host_calls > 1024 ||
        limits.effects < 1 || limits.effects > 8 || limits.max_source_bytes < 1 || limits.max_source_bytes > 65536 ||
        limits.max_context_bytes < 64 || limits.max_context_bytes > 65536 || limits.max_context_depth < 1 || limits.max_context_depth > 32 ||
        limits.max_context_nodes < 1 || limits.max_context_nodes > 8192 || limits.max_string_bytes < 1 || limits.max_string_bytes > 8192 ||
        limits.max_output_bytes < 64 || limits.max_output_bytes > 8192) throw ScriptError("invalid_limits");
}

std::string checked_digest(const std::string& source, const Limits& limits) {
    validate_limits(limits);
    if (source.empty() || source.size() > limits.max_source_bytes) throw ScriptError("source_limit");
    return digest(source);
}

// 输入在受保护调用外预检并展开为纯数据操作；Lua longjmp不会跳过JSON迭代器等C++析构。
struct Push {
    enum Kind { object, array, key, string, integer, number, boolean, null, set, set_index } kind;
    const char* bytes = nullptr;
    std::size_t length = 0;
    lua_Integer whole = 0;
    lua_Number real = 0;
    int count = 0;
};

void flatten(const Json& value, const Limits& limits, unsigned depth, unsigned& nodes, std::vector<Push>& out) {
    if (depth > limits.max_context_depth || ++nodes > limits.max_context_nodes) throw ScriptError("invalid_context");
    if (value.is_object()) {
        out.push_back({Push::object, nullptr, 0, 0, 0, static_cast<int>(value.size())});
        for (auto item = value.begin(); item != value.end(); ++item) {
            if (item.key().size() > 128) throw ScriptError("invalid_context");
            out.push_back({Push::key, item.key().data(), item.key().size()});
            flatten(item.value(), limits, depth + 1, nodes, out);
            out.push_back({Push::set});
        }
    } else if (value.is_array()) {
        out.push_back({Push::array, nullptr, 0, 0, 0, static_cast<int>(value.size())});
        lua_Integer index = 0;
        for (const auto& item : value) {
            flatten(item, limits, depth + 1, nodes, out);
            out.push_back({Push::set_index, nullptr, 0, ++index});
        }
    } else if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.size() > limits.max_string_bytes) throw ScriptError("invalid_context");
        out.push_back({Push::string, text.data(), text.size()});
    } else if (value.is_number_unsigned()) {
        auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<lua_Integer>::max())) throw ScriptError("invalid_context");
        out.push_back({Push::integer, nullptr, 0, static_cast<lua_Integer>(number)});
    } else if (value.is_number_integer()) {
        out.push_back({Push::integer, nullptr, 0, static_cast<lua_Integer>(value.get<std::int64_t>())});
    } else if (value.is_number_float()) {
        auto number = value.get<double>();
        if (!std::isfinite(number)) throw ScriptError("invalid_context");
        out.push_back({Push::number, nullptr, 0, 0, number});
    } else if (value.is_boolean()) {
        out.push_back({Push::boolean, nullptr, 0, value.get<bool>() ? 1 : 0});
    } else if (value.is_null()) out.push_back({Push::null});
    else throw ScriptError("invalid_context");
}

struct Effect {
    bool wall = false;
    std::array<char, 33> skill{};
    std::size_t length = 0;
};

struct Run {
    const std::string& source;
    const Limits& limits;
    const std::string* expected_id = nullptr;
    const std::string* expected_type = nullptr;
    const std::vector<Push>* input = nullptr;
    std::size_t allocated = 0;
    std::uint64_t instructions = 0;
    unsigned calls = 0, effect_count = 0;
    Fault fault = Fault::none;
    bool invoke = false, planning = false, denied = false;
    std::array<Effect, 8> effects{};
    std::array<char, 65> reason{};
    std::size_t reason_length = 0;
};

void* allocate(void* user, void* pointer, std::size_t old_size, std::size_t new_size) noexcept {
    auto* run = static_cast<Run*>(user);
    // 新块的old_size是Lua类型标记，不是已分配长度。
    const auto previous = pointer ? old_size : 0;
    if (new_size == 0) {
        std::free(pointer);
        run->allocated -= previous;
        return nullptr;
    }
    if (new_size > run->limits.memory_bytes || run->allocated - previous > run->limits.memory_bytes - new_size) {
        if (run->fault == Fault::none) run->fault = Fault::memory;
        return nullptr;
    }
    auto* result = std::realloc(pointer, new_size);
    if (result) run->allocated = run->allocated - previous + new_size;
    else if (run->fault == Fault::none) run->fault = Fault::memory;
    return result;
}

Run* state(lua_State* lua) noexcept { return *static_cast<Run**>(lua_getextraspace(lua)); }

// 所有Lua C回调只使用平凡局部变量，错误经pcall返回，不跨越C++资源对象析构。
int fail(lua_State* lua, Fault fault) {
    auto* run = state(lua);
    if (run->fault == Fault::none) run->fault = fault;
    return luaL_error(lua, "planner rejected");
}

bool call_allowed(lua_State* lua) noexcept {
    auto* run = state(lua);
    return ++run->calls <= run->limits.host_calls;
}

void instruction_hook(lua_State* lua, lua_Debug*) {
    auto* run = state(lua);
    run->instructions += instruction_quantum;
    if (run->instructions >= run->limits.instructions) fail(lua, Fault::instructions);
}

int emit(lua_State* lua) {
    auto* run = state(lua);
    if (!call_allowed(lua)) return fail(lua, Fault::host);
    if (!run->planning) return fail(lua, Fault::phase);
    if (run->denied) return fail(lua, Fault::invalid_effect);
    if (lua_gettop(lua) != 2 || lua_type(lua, 1) != LUA_TSTRING || lua_type(lua, 2) != LUA_TSTRING) return fail(lua, Fault::invalid_effect);
    std::size_t type_length = 0, id_length = 0;
    const char* type = lua_tolstring(lua, 1, &type_length);
    const char* id = lua_tolstring(lua, 2, &id_length);
    if (type_length != run->expected_type->size() || std::memcmp(type, run->expected_type->data(), type_length) != 0 ||
        id_length != run->expected_id->size() || std::memcmp(id, run->expected_id->data(), id_length) != 0) return fail(lua, Fault::invalid_effect);
    if (run->effect_count >= run->limits.effects) return fail(lua, Fault::effects);
    auto* effect = &run->effects[run->effect_count++];
    effect->wall = type_length == 4;
    effect->length = id_length;
    std::memcpy(effect->skill.data(), id, id_length);
    return 0;
}

int deny(lua_State* lua) {
    auto* run = state(lua);
    if (!call_allowed(lua)) return fail(lua, Fault::host);
    if (!run->planning) return fail(lua, Fault::phase);
    if (run->denied || run->effect_count || lua_gettop(lua) != 1 || lua_type(lua, 1) != LUA_TSTRING) return fail(lua, Fault::invalid_effect);
    std::size_t length = 0;
    const char* text = lua_tolstring(lua, 1, &length);
    if (length == 0 || length > 64) return fail(lua, Fault::invalid_effect);
    for (std::size_t i = 0; i < length; ++i) if (!std::strchr(id_characters, text[i]) || text[i] == '\0') return fail(lua, Fault::invalid_effect);
    run->denied = true;
    run->reason_length = length;
    std::memcpy(run->reason.data(), text, length);
    return 0;
}

int guarded_function(lua_State* lua) {
    if (!call_allowed(lua)) return fail(lua, Fault::host);
    const int count = lua_gettop(lua);
    if (count > 64) return fail(lua, Fault::host);
    if (lua_toboolean(lua, lua_upvalueindex(2))) {
        const int type = lua_type(lua, 1);
        if (count != 1 || (type != LUA_TNIL && type != LUA_TBOOLEAN && type != LUA_TNUMBER && type != LUA_TSTRING)) return fail(lua, Fault::host);
    }
    for (int i = 1; i <= count; ++i) if (lua_type(lua, i) == LUA_TSTRING && lua_rawlen(lua, i) > state(lua)->limits.max_string_bytes) return fail(lua, Fault::host);
    lua_pushvalue(lua, lua_upvalueindex(1));
    lua_insert(lua, 1);
    lua_call(lua, count, LUA_MULTRET);
    return lua_gettop(lua);
}

bool base_allowed(const char* name) noexcept {
    for (const auto* allowed : {"_G", "_VERSION", "assert", "error", "ipairs", "select", "tonumber", "tostring", "type"})
        if (std::strcmp(allowed, name) == 0) return true;
    return false;
}

void expose_library(lua_State* lua, const char* name, const char* const* functions, unsigned count, lua_CFunction open) {
    open(lua); // 原库只留在栈上；不把文件/随机/模式匹配等能力暴露给脚本。
    const int original = lua_gettop(lua);
    lua_createtable(lua, 0, static_cast<int>(count));
    for (unsigned i = 0; i < count; ++i) {
        lua_getfield(lua, original, functions[i]);
        lua_pushcclosure(lua, guarded_function, 1);
        lua_setfield(lua, -2, functions[i]);
    }
    if (std::strcmp(name, "string") == 0) {
        // luaopen_string默认给字符串安装完整方法表，必须一起替换以防 :rep/:dump 绕过白名单。
        lua_pushliteral(lua, "");
        lua_createtable(lua, 0, 1);
        lua_pushvalue(lua, original + 1);
        lua_setfield(lua, -2, "__index");
        lua_setmetatable(lua, -2);
        lua_pop(lua, 1);
    }
    lua_setglobal(lua, name);
    lua_pop(lua, 1);
}

void push_context(lua_State* lua) {
    const auto* operations = state(lua)->input;
    for (std::size_t i = 0; i < operations->size(); ++i) {
        const auto* operation = &(*operations)[i];
        switch (operation->kind) {
        case Push::object: lua_createtable(lua, 0, operation->count); break;
        case Push::array: lua_createtable(lua, operation->count, 0); break;
        case Push::key:
        case Push::string: lua_pushlstring(lua, operation->bytes, operation->length); break;
        case Push::integer: lua_pushinteger(lua, operation->whole); break;
        case Push::number: lua_pushnumber(lua, operation->real); break;
        case Push::boolean: lua_pushboolean(lua, operation->whole != 0); break;
        case Push::null: lua_pushnil(lua); break;
        case Push::set: lua_rawset(lua, -3); break;
        case Push::set_index: lua_rawseti(lua, -2, operation->whole); break;
        }
    }
}

int bootstrap(lua_State* lua) {
    luaopen_base(lua);
    lua_pop(lua, 1);
    lua_pushglobaltable(lua);
    lua_pushnil(lua);
    while (lua_next(lua, -2)) {
        const char* name = lua_tostring(lua, -2);
        lua_pop(lua, 1);
        if (!name || !base_allowed(name)) {
            lua_pushvalue(lua, -1);
            lua_pushnil(lua);
            lua_rawset(lua, -4);
        }
    }
    lua_pop(lua, 1);
    for (const auto* name : {"assert", "error", "ipairs", "select", "tonumber", "tostring", "type"}) {
        lua_getglobal(lua, name);
        // 不暴露对象地址字符串；哈希遍历pairs/next也不开放，脚本使用有序数组ipairs。
        lua_pushboolean(lua, std::strcmp(name, "tostring") == 0);
        lua_pushcclosure(lua, guarded_function, 2);
        lua_setglobal(lua, name);
    }
    const char* math_functions[]{"abs", "ceil", "floor", "min", "max", "fmod", "sqrt", "sin", "cos", "atan"};
    expose_library(lua, "math", math_functions, 10, luaopen_math);
    const char* string_functions[]{"byte", "char", "len", "sub", "lower", "upper"};
    expose_library(lua, "string", string_functions, 6, luaopen_string);
    lua_createtable(lua, 0, 2);
    lua_pushcfunction(lua, emit); lua_setfield(lua, -2, "emit");
    lua_pushcfunction(lua, deny); lua_setfield(lua, -2, "deny");
    lua_setglobal(lua, "host");

    auto* run = state(lua);
    if (luaL_loadbufferx(lua, run->source.data(), run->source.size(), "trusted-skill-module", "t") != LUA_OK) return lua_error(lua);
    lua_call(lua, 0, 1);
    if (!lua_istable(lua, -1)) return fail(lua, Fault::module);
    lua_getfield(lua, -1, "plan");
    if (!lua_isfunction(lua, -1) || lua_iscfunction(lua, -1)) return fail(lua, Fault::module);
    if (run->invoke) {
        if (!lua_checkstack(lua, static_cast<int>(run->limits.max_context_depth * 2 + 32))) return fail(lua, Fault::memory);
        push_context(lua);
        run->planning = true;
        lua_call(lua, 1, 0);
    }
    return 0;
}

void execute(Run& run) {
    using State = std::unique_ptr<lua_State, decltype(&lua_close)>;
    State lua(lua_newstate(allocate, &run), lua_close);
    if (!lua) throw ScriptError("memory_limit");
    *static_cast<Run**>(lua_getextraspace(lua.get())) = &run;
    lua_sethook(lua.get(), instruction_hook, LUA_MASKCOUNT, instruction_quantum);
    lua_pushcfunction(lua.get(), bootstrap);
    const int status = lua_pcall(lua.get(), 0, 0, 0);
    if (status != LUA_OK || run.fault != Fault::none) {
        if (status == LUA_ERRMEM && run.fault == Fault::none) run.fault = Fault::memory;
        throw ScriptError(fault_name(run.fault));
    }
}
}

ScriptError::ScriptError(std::string reason) : std::runtime_error("Lua planner rejected: " + reason), code(std::move(reason)) {}

Script::Script(std::string version, std::string source, Limits limits)
    : version_(std::move(version)), source_(std::move(source)), limits_(limits), sha256_(checked_digest(source_, limits_)) {
    if (!valid_id(version_, 64)) throw ScriptError("invalid_version");
    Run run{source_, limits_};
    execute(run);
}

Json Script::plan(const Json& context) const {
    std::vector<Push> input;
    unsigned nodes = 0;
    const std::string* id = nullptr;
    const std::string* type = nullptr;
    try {
        if (!context.is_object() || !context.contains("skill_id") || !context["skill_id"].is_string() ||
            !context.contains("definition") || !context["definition"].is_object() || !context["definition"].contains("type") || !context["definition"]["type"].is_string()) throw ScriptError("invalid_context");
        id = &context["skill_id"].get_ref<const std::string&>();
        type = &context["definition"]["type"].get_ref<const std::string&>();
        if (!valid_id(*id, 32) || (*type != "projectile" && *type != "wall")) throw ScriptError("invalid_context");
        flatten(context, limits_, 0, nodes, input);
        if (context.dump().size() > limits_.max_context_bytes) throw ScriptError("invalid_context");
    } catch (const Json::exception&) { throw ScriptError("invalid_context"); }
    Run run{source_, limits_};
    run.expected_id = id; run.expected_type = type; run.input = &input; run.invoke = true;
    execute(run);
    Json output;
    if (run.denied) output = {{"denied", true}, {"reason", std::string(run.reason.data(), run.reason_length)}};
    else {
        // 旧JSON施法回执只承诺一个实例；多效果要在明确的新协议中另做事务化扩展。
        if (run.effect_count != 1) throw ScriptError("invalid_effect");
        const auto& effect = run.effects[0];
        output = {{"effects", Json::array({{{"type", effect.wall ? "wall" : "projectile"}, {"skill_id", std::string(effect.skill.data(), effect.length)}}})}};
    }
    if (output.dump().size() > limits_.max_output_bytes) throw ScriptError("output_limit");
    return output;
}
}
