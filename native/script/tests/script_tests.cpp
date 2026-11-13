#include "script/script.hpp"

#include <atomic>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using native::script::Json;
using native::script::Limits;
using native::script::Script;
using native::script::ScriptError;

namespace {
void check(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
std::string read(const char* file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("test fixture unavailable");
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}
template<class Function> void rejects(const char* expected, Function action) {
    try { action(); }
    catch (const ScriptError& error) {
        if (error.code == expected) return;
        throw std::runtime_error(std::string("wrong rejection: ") + error.code + ", expected " + expected);
    }
    throw std::runtime_error(std::string("planner accepted invalid operation: ") + expected);
}
Json context(const char* type = "projectile") {
    return {{"skill_id", "client-bolt"}, {"definition", {{"type", type}, {"damage", 28}, {"speed", 18}, {"radius", .25}}},
            {"origin", {{"x", 0}, {"y", 0}}}, {"aim", {{"dx", 1}, {"dy", 0}}},
            {"target", {{"x", 4}, {"y", 4}}}, {"owner", {{"role_id", "controlled-role"}, {"owner_epoch", 1}, {"session_epoch", 2}}},
            {"tick", std::uint64_t(9007199254740993ULL)}, {"world_epoch", "controlled-world"}};
}
const char* pass = "return {plan=function(c) host.emit(c.definition.type,c.skill_id) end}";
void effect(const Json& plan, const char* kind) {
    check(plan.contains("effects") && plan["effects"].size() == 1, "effect cardinality changed");
    check(plan["effects"][0]["type"] == kind && plan["effects"][0]["skill_id"] == "client-bolt", "effect identity changed");
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 3, "default Lua fixture arguments required");
        const auto source = read(argv[1]);
        const auto manifest = Json::parse(read(argv[2]));
        const Script baseline(manifest.at("version"), source);
        check(baseline.sha256() == manifest.at("sha256"), "default script SHA does not match manifest");
        effect(baseline.plan(context()), "projectile");
        effect(baseline.plan(context("wall")), "wall");
        check(baseline.version() == "skills-v1", "version was not pinned");
        const auto before = context();
        baseline.plan(before);
        check(before == context(), "Lua modified caller's C++ context");

        const Script libraries("capabilities-v1", R"(
return {plan=function(c)
    assert(io==nil and os==nil and package==nil and debug==nil and require==nil and coroutine==nil)
    assert(load==nil and loadfile==nil and dofile==nil and collectgarbage==nil and pcall==nil and xpcall==nil)
    assert(math.random==nil and math.randomseed==nil and string.rep==nil and string.dump==nil and string.find==nil)
    assert(("abc").rep==nil and ("abc").dump==nil and getmetatable==nil and setmetatable==nil)
    assert(pairs==nil and next==nil)
    assert(math.max(c.definition.damage, 0)==28 and string.sub(c.skill_id,1,6)=="client")
    assert(c.tick==9007199254740993)
    host.emit(c.definition.type,c.skill_id)
end})");
        effect(libraries.plan(context()), "projectile");

        const Script stateless("stateless-v1", R"(
local calls=0
return {plan=function(c)
    calls=calls+1
    if calls~=1 then host.deny("shared_global_leak") else host.emit(c.definition.type,c.skill_id) end
end})");
        effect(stateless.plan(context()), "projectile");
        effect(stateless.plan(context()), "projectile");
        auto first_room = context(); first_room["room_id"] = "room-one";
        auto second_room = context(); second_room["room_id"] = "room-two";
        effect(stateless.plan(first_room), "projectile");
        effect(stateless.plan(second_room), "projectile");

        const Script throws_after_emit("rollback-v1", R"(
local poisoned=false
return {plan=function(c)
    if c.fail then poisoned=true;host.emit(c.definition.type,c.skill_id);error("synthetic failure") end
    if poisoned then host.deny("failed_call_leaked") else host.emit(c.definition.type,c.skill_id) end
end})");
        auto failure = context(); failure["fail"] = true;
        rejects("script_error", [&] { throws_after_emit.plan(failure); });
        effect(throws_after_emit.plan(context()), "projectile");

        const Script newer("skills-v2", R"(
return {plan=function(c)
    if c.definition.type=="wall" then host.deny("walls_disabled")
    else host.emit(c.definition.type,c.skill_id) end
end})");
        check(newer.plan(context("wall")) == Json{{"denied", true}, {"reason", "walls_disabled"}}, "denial contract changed");
        effect(baseline.plan(context("wall")), "wall");
        check(newer.sha256() != baseline.sha256() && newer.version() != baseline.version(), "version/source identity changed");

        Limits small_instructions; small_instructions.instructions = 1000;
        const Script infinite("loop-v1", "return {plan=function(c) while true do end end}", small_instructions);
        rejects("instruction_limit", [&] { infinite.plan(context()); });
        rejects("instruction_limit", [&] { Script invalid("init-loop-v1", "while true do end", small_instructions); });
        Limits small_memory; small_memory.memory_bytes = 131072;
        const Script allocates("memory-v1", "return {plan=function(c) local text='xxxxxxxx'; while true do text=text..text end end}", small_memory);
        rejects("memory_limit", [&] { allocates.plan(context()); });
        const Script too_many_calls("calls-v1", "return {plan=function(c) for i=1,100 do type(c) end;host.emit(c.definition.type,c.skill_id) end}");
        rejects("host_limit", [&] { too_many_calls.plan(context()); });
        const Script address("address-v1", "return {plan=function(c) local address=tostring({});host.emit(c.definition.type,c.skill_id) end}");
        rejects("host_limit", [&] { address.plan(context()); });
        const Script too_many_effects("effects-v1", "return {plan=function(c) host.emit(c.definition.type,c.skill_id);host.emit(c.definition.type,c.skill_id) end}");
        rejects("effect_limit", [&] { too_many_effects.plan(context()); });
        const Script empty("empty-v1", "return {plan=function(c) end}");
        rejects("invalid_effect", [&] { empty.plan(context()); });
        const Script wrong_id("wrong-id-v1", "return {plan=function(c) host.emit(c.definition.type,'another-player') end}");
        rejects("invalid_effect", [&] { wrong_id.plan(context()); });
        const Script wrong_type("wrong-type-v1", "return {plan=function(c) host.emit('wall',c.skill_id) end}");
        rejects("invalid_effect", [&] { wrong_type.plan(context()); });
        const Script mutates_context("mutating-v1", "return {plan=function(c) c.definition.type='wall';host.emit(c.definition.type,c.skill_id) end}");
        rejects("invalid_effect", [&] { mutates_context.plan(context()); });
        const Script conflicting("conflict-v1", "return {plan=function(c) host.emit(c.definition.type,c.skill_id);host.deny('late_denial') end}");
        rejects("invalid_effect", [&] { conflicting.plan(context()); });
        rejects("invalid_phase", [&] { Script invalid("init-emit-v1", "host.emit('projectile','client-bolt');return {plan=function(c) end}"); });

        const Script valid("limits-v1", pass);
        auto oversized = context(); oversized["payload"] = std::string(8192, 'x');
        rejects("invalid_context", [&] { valid.plan(oversized); });
        auto bad_number = context(); bad_number["not_finite"] = std::numeric_limits<double>::infinity();
        rejects("invalid_context", [&] { valid.plan(bad_number); });
        auto large_integer = context(); large_integer["tick"] = std::numeric_limits<std::uint64_t>::max();
        rejects("invalid_context", [&] { valid.plan(large_integer); });
        auto deep = context(); Json nested = Json::object();
        for (unsigned i = 0; i < 40; ++i) nested = Json{{"next", nested}};
        deep["deep"] = nested;
        rejects("invalid_context", [&] { valid.plan(deep); });
        auto bad_id = context(); bad_id["skill_id"] = "../other";
        rejects("invalid_context", [&] { valid.plan(bad_id); });
        rejects("invalid_version", [&] { Script invalid("../version", pass); });
        rejects("source_limit", [&] { Script invalid("source-v1", std::string(65537, ' ')); });
        rejects("invalid_module", [&] { Script invalid("module-v1", "return {plan=1}"); });
        rejects("invalid_module", [&] { Script invalid("module-v1", "return {plan=type}"); });
        rejects("script_error", [&] { Script invalid("binary-v1", std::string("\x1bLua", 4)); });
        Limits output_cap; output_cap.max_output_bytes = 64;
        const Script verbose("output-v1", "return {plan=function(c) host.deny('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa') end}", output_cap);
        rejects("output_limit", [&] { verbose.plan(context()); });

        // 同一不可变版本被多个房间/线程同时持有，不共享VM或失败后的可变全局。
        std::atomic<bool> concurrent_ok{true};
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < 4; ++i) threads.emplace_back([&] {
            try { for (unsigned j = 0; j < 16; ++j) effect(stateless.plan(context()), "projectile"); }
            catch (...) { concurrent_ok = false; }
        });
        for (auto& thread : threads) thread.join();
        check(concurrent_ok, "immutable script shared mutable execution state");
        std::cout << "Lua planner budgets, capabilities, transactional plans, isolation and pinned versions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Lua planner test failed: " << error.what() << '\n';
        return 1;
    }
}
