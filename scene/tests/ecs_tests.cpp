#include "scene.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using mmo::Json;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Position {
    static constexpr bool in_place_delete = true;
    static constexpr std::size_t page_size = 64;
    std::string id;
    double x = 0;
};
struct Category {};
Json role(const char* id = "a", double x = 0) {
    return {{"role_id", id}, {"name", id}, {"scene_id", "room"}, {"owner_epoch", 1},
            {"session_epoch", 1}, {"x", x}, {"y", 0}, {"coins", 17}};
}
Json ref(const char* id = "a") { return {{"role_id", id}, {"owner_epoch", 1}, {"session_epoch", 1}}; }
Json bolt(int seq = 1) {
    auto result = ref();
    result.update({{"seq", seq}, {"skill_id", "arc_bolt"}, {"dx", 1}, {"dy", 0}});
    return result;
}
Json wall() {
    auto result = ref();
    result.update({{"seq", 2}, {"skill_id", "stone_wall"}, {"dx", 1}, {"dy", 0}, {"x", 3}, {"y", 0}});
    return result;
}
Json view(mmo::Scene& scene) { return scene.execute("/v1/view", ref()); }
Json normalized(Json value) {
    if (value.is_object()) {
        value.erase("world_epoch");
        // 对外实例ID包含随机世界代次；等价比较只规范化这一不稳定前缀。
        if (value.contains("id") && value["id"].is_string()) {
            auto id = value["id"].get<std::string>();
            if (id.rfind("p-", 0) == 0 || id.rfind("w-", 0) == 0) value["id"] = id.substr(0, 2) + id.substr(id.rfind('-') + 1);
        }
        if (value.contains("object_id") && value["object_id"].is_string()) {
            auto id = value["object_id"].get<std::string>();
            if (id.rfind("p-", 0) == 0 || id.rfind("w-", 0) == 0) value["object_id"] = id.substr(0, 2) + id.substr(id.rfind('-') + 1);
        }
        for (auto& item : value.items()) item.value() = normalized(std::move(item.value()));
    } else if (value.is_array()) {
        for (auto& item : value) item = normalized(std::move(item));
    }
    return value;
}
void storage_invariants() {
    entt::registry registry;
    mmo::EcsStorage<Position, Category> data(registry);
    data.emplace("kept", Position{"kept", 3});
    Position* address = &data.at("kept");
    for (int i = 0; i < 300; ++i) {
        Position component{"other-" + std::to_string(i), double(i)};
        data.emplace(component.id, std::move(component));
    }
    for (int i = 0; i < 300; i += 2) data.erase("other-" + std::to_string(i));
    check(&data.at("kept") == address && address->x == 3, "ECS allocation/deletion invalidated another component reference");
    check(data.size() == data.component_count(), "component pool differs from identity index");
    std::string previous;
    for (const auto& item : data) { check(previous.empty() || previous < item.first, "external ID iteration changed order"); previous = item.first; }
    data.at("kept").x = 9;
    auto handle = registry.view<Position, Category>().begin();
    bool found = false;
    for (; handle != registry.view<Position, Category>().end(); ++handle) {
        auto& component = registry.get<Position>(*handle);
        if (component.id == "kept") { check(component.x == 9 && &component == address, "map facade stores a mirrored component"); found = true; }
    }
    check(found, "authoritative component not in registry");
    auto it = data.begin();
    while (it != data.end()) it = data.erase(it);
    check(data.empty() && data.component_count() == 0, "ECS removal leaked components");
}
void isolated_scenes_and_hook_equivalence() {
    mmo::Config config; config.scene_id = "room"; config.abilities_enabled = true;
    mmo::Scene original(config), other(config);
    int planned = 0;
    config.ability_planner = [&](const Json& context) {
        ++planned;
        check(context["owner"]["role_id"] == "a", "script received wrong identity");
        check(context["origin"]["x"] == 0, "script received untrusted origin");
        check(context["aim"]["dx"] == 1 && context["aim"]["dy"] == 0, "script aim was not normalized");
        return Json{{"effects", Json::array({{{"type", context["definition"]["type"]}, {"skill_id", context["skill_id"]}}})}};
    };
    mmo::Scene scripted(config);
    for (auto* scene : {&original, &other, &scripted}) {
        scene->execute("/v1/enter", {{"role", role()}});
        scene->execute("/v1/enter", {{"role", role("b", 6)}});
    }
    check(original.world_epoch() != other.world_epoch(), "two rooms share world generation");
    check(original.ecs_stats()["players"] == 2 && original.ecs_stats()["npcs"] == 4, "ECS stats do not reflect authoritative components");
    auto before_other = view(other);
    check(normalized(original.execute("/v1/cast", bolt())) == normalized(scripted.execute("/v1/cast", bolt())), "script changed canonical projectile outcome");
    for (int i = 0; i < 10; ++i) {
        original.step(.05); scripted.step(.05);
        check(normalized(view(original)) == normalized(view(scripted)), "scripted and legacy systems diverged for identical ticks");
    }
    check(view(other) == before_other && other.ecs_stats()["projectiles"] == 0, "another room changed from movement/damage/events");
    // 等冷却后再验证墙体的同规则编排与寿命。
    check(normalized(original.execute("/v1/cast", wall())) == normalized(scripted.execute("/v1/cast", wall())), "script changed canonical wall outcome");
    check(original.ecs_stats()["walls"] == 1, "wall is not a registry component");
    check(planned == 2, "planner was not used exactly once per successful cast");
    auto state = original.role_state("a");
    check(state && (*state)["mode"] == "active" && (*state)["connected"] == true && (*state)["role"]["coins"] == 17, "read-only role metadata changed identity");
    check(!original.role_state("missing") && original.role_states().size() == 2, "role introspection is inconsistent");
    const auto light = original.role_states(false);
    check(light.size() == 2 && light[0]["role"]["role_id"] == "a" && light[0]["role"].size() == 5 &&
          !light[0]["role"].contains("combat") && !light[0]["role"].contains("x") && !light[0]["role"].contains("coins"),
          "room membership metadata serialized full resources or positions");
    check(original.role_state("a", false) == std::optional<Json>(light[0]), "light role lookup differs from membership metadata");
    auto before_stop = (*original.role_state("a"))["role"];
    original.stop_combat();
    check(original.ecs_stats()["walls"] == 0 && original.ecs_stats()["projectiles"] == 0, "room finish retained combat objects");
    check((*original.role_state("a"))["role"] == before_stop && (*original.role_state("a"))["connected"] == true, "room finish changed assets or disconnected ownership");
    original.execute("/v1/disconnect", ref());
    check(original.ecs_stats()["walls"] == 0 && original.ecs_stats()["projectiles"] == 0, "disconnect leaked ECS ability components");
    check((*original.role_state("a"))["connected"] == false, "disconnect flag was mirrored incorrectly");
}
void failed_plans_do_not_commit() {
    for (int kind = 0; kind < 6; ++kind) {
        mmo::Config config; config.scene_id = "room"; config.abilities_enabled = true;
        bool failing = true;
        config.ability_planner = [&](const Json& context) -> Json {
            if (!failing) return {{"effects", Json::array({{{"type", context["definition"]["type"]}, {"skill_id", context["skill_id"]}}})}};
            if (kind == 0) throw std::runtime_error("synthetic script error");
            if (kind == 1) return Json::array();
            if (kind == 2) return {{"effects", Json::array()}};
            if (kind == 3) return {{"effects", Json::array({{{"type", "projectile"}, {"skill_id", "arc_bolt"}}, {{"type", "wall"}, {"skill_id", "arc_bolt"}}})}};
            if (kind == 4) return {{"effects", Json::array({{{"type", "wall"}, {"skill_id", "arc_bolt"}}})}};
            return {{"denied", true}, {"reason", "server policy"}};
        };
        mmo::Scene scene(config); scene.execute("/v1/enter", {{"role", role()}});
        const auto before = view(scene), stats = scene.ecs_stats();
        auto refused = scene.execute("/v1/cast", bolt());
        check(refused["accepted"] == false && refused["reason"] == (kind == 5 ? "script_denied" : "script_error"), "bad script plan was accepted");
        check(view(scene) == before && scene.ecs_stats() == stats, "failed plan partially consumed MP/cooldown/sequence or generated objects/events");
        failing = false;
        check(scene.execute("/v1/cast", bolt())["accepted"] == true, "failure consumed action sequence or left cooldown");
    }
}
void idle_lease_without_simulation() {
    mmo::Config config; config.scene_id = "room"; config.abilities_enabled = true;
    mmo::Scene scene(config); scene.execute("/v1/enter", {{"role", role()}});
    scene.execute("/v1/cast", bolt());
    const auto before = view(scene), stats = scene.ecs_stats();
    for (int i = 0; i < 4; ++i) scene.advance_leases(1);
    check((*scene.role_state("a"))["lease_expired"] == true, "waiting room lease never expires");
    check(scene.player_count() == 0 && scene.role_count() == 1, "idle lease deleted an unsaved role or kept it live");
    check(scene.tick() == 0 && scene.ecs_stats() == stats && view(scene) == before, "idle lease advanced resources, physics, NPCs, events or tick");
}
}
int main() {
    try {
        storage_invariants(); isolated_scenes_and_hook_equivalence(); failed_plans_do_not_commit(); idle_lease_without_simulation();
        std::cout << "scene ECS isolation and transactional planner invariants passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "scene ECS test failure: " << error.what() << '\n'; return 1;
    }
}
