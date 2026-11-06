#include "scene.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>

using mmo::Json;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void fails(int status, const std::function<void()>& operation) {
    try { operation(); }
    catch (const mmo::Error& error) { check(error.status == status, "unexpected rejection status"); return; }
    throw std::runtime_error("expected config rejection");
}
Json role(const std::string& scene = "scene-1", int owner = 1, int session = 1) {
    return {{"role_id", "hero"}, {"name", "hero"}, {"scene_id", scene}, {"owner_epoch", owner},
        {"session_epoch", session}, {"x", 0}, {"y", 0}, {"coins", 0}};
}
Json ref(int owner = 1, int session = 1) { return {{"role_id", "hero"}, {"owner_epoch", owner}, {"session_epoch", session}}; }
Json action(int seq, const std::string& skill, const std::string& target = "") {
    auto request = ref(); request.update({{"seq", seq}, {"skill_id", skill}, {"target_id", target}}); return request;
}
Json status(mmo::Scene& scene) { return scene.execute("/v1/config/status", Json::object()); }
Json view(mmo::Scene& scene, const Json& reference = ref()) { return scene.execute("/v1/view", reference); }
Json mob(mmo::Scene& scene, const std::string& id = "npc-1") {
    const auto state = view(scene);
    for (const auto& value : state["mobs"]) if (value["id"] == id) return value;
    throw std::runtime_error("monster missing");
}
Json apply_config(mmo::Scene& scene, const Json& config, int generation = 0) {
    Json request{{"config", config}};
    if (generation) request["expected_generation"] = generation;
    return scene.execute("/v1/config/apply", request);
}
void advance(mmo::Scene& scene, int steps, bool heartbeat = true) {
    for (int i = 0; i < steps; ++i) { if (heartbeat) view(scene); scene.step(0.1); }
}
}

int main() {
    try {
        mmo::Scene scene({});
        const auto initial = status(scene);
        const auto defaults = initial["config"];
        check(initial["version"] == "default-v1" && initial["generation"] == 1 && initial["applied_tick"] == 0,
            "initial config metadata incorrect");
        check(defaults["monsters"]["elite"]["max_hp"] == 240 && defaults["monsters"]["elite"]["respawn_ms"] == 8000,
            "elite defaults changed");
        auto entered = scene.execute("/v1/enter", {{"role", role()}})["role"];
        entered["combat"]["hp"] = 100;
        mmo::Scene live({}); live.execute("/v1/enter", {{"role", entered}});
        const auto heal_request = action(1, "mend");
        const auto healed = live.execute("/v1/cast", heal_request);
        const auto before = view(live)["self"]["combat"];
        auto candidate = defaults; candidate["version"] = "balance-v2";
        candidate["skills"]["strike"].update({{"damage", 40}, {"mp_cost", 3}, {"range", 100}, {"cooldown_ms", 100}});
        candidate["skills"]["mend"].update({{"heal", 30}, {"cooldown_ms", 100}});
        candidate["monsters"]["wolf"].update({{"max_hp", 200}, {"damage", 1}, {"respawn_ms", 1000}});
        const auto validated = live.execute("/v1/config/validate", {{"config", candidate}, {"expected_generation", 1}});
        check(validated["valid"] == true && validated["would_change"] == true && validated["generation"] == 1,
            "validate metadata incorrect");
        check(status(live) == initial && view(live)["self"]["combat"] == before, "validate mutated state");
        const auto applied = apply_config(live, candidate, 1);
        check(applied["generation"] == 2 && applied["changed"] == true, "reload did not advance generation");
        check(view(live)["config_version"] == "balance-v2" && view(live)["config_generation"] == 2,
            "view missing current config");
        check(view(live)["self"]["combat"] == before, "reload changed HP/MP/CD/revision/action high water");
        check(live.execute("/v1/cast", heal_request) == healed && healed["config_version"] == "default-v1",
            "reload invalidated action cache");
        fails(409, [&] { live.execute("/v1/cast", action(2, "mend")); });
        check(apply_config(live, candidate, 2)["changed"] == false && status(live)["generation"] == 2,
            "identical reload was not idempotent");
        fails(409, [&] { apply_config(live, candidate, 1); });
        auto conflict = candidate; conflict["skills"]["strike"]["damage"] = 41;
        fails(409, [&] { apply_config(live, conflict); });
        const auto strike = live.execute("/v1/cast", action(2, "strike", "npc-2"));
        check(strike["hits"][0]["damage"] == 40 && strike["role"]["combat"]["mp"] == 72 &&
            strike["role"]["combat"]["cooldowns"]["strike"] == 100 && strike["config_version"] == "balance-v2",
            "new cast did not use complete new skill parameters");
        check(mob(live, "npc-2")["max_hp"] == 80 && mob(live, "npc-2")["config_version"] == "default-v1",
            "reload changed existing monster parameters");
        const auto rollback = apply_config(live, defaults, 2);
        check(rollback["generation"] == 3 && rollback["version"] == "default-v1", "historical rollback failed");
        check(live.execute("/v1/cast", action(2, "strike", "npc-2")) == strike, "rollback invalidated action cache");
        fails(409, [&] { apply_config(live, conflict); });

        // 缩短新技能 CD 不截断已有 CD，重连、重启和跨场景准备仍接收旧剩余时间。
        apply_config(live, candidate);
        auto reconnect = role("scene-1", 1, 2);
        auto connected = live.execute("/v1/enter", {{"role", reconnect}})["role"];
        check(connected["combat"]["cooldowns"]["mend"] == 12000 && connected["combat"]["hp"] == 145,
            "reconnect refreshed combat after shorter CD reload");
        auto freeze = ref(1, 2); freeze["transfer_id"] = "balance-transfer";
        auto frozen = live.execute("/v1/freeze", freeze)["role"];
        frozen["scene_id"] = "scene-2"; frozen["owner_epoch"] = 2;
        mmo::Config destination_config; destination_config.scene_id = "scene-2"; destination_config.combat_config = candidate;
        mmo::Scene destination(destination_config);
        const Json prepared{{"role", frozen}, {"transfer_id", "balance-transfer"}};
        destination.execute("/v1/prepare", prepared); destination.step(1);
        check(destination.execute("/v1/prepare", prepared)["role"]["combat"] == frozen["combat"],
            "prepared old cooldown was lost or advanced");
        auto activate = ref(2, 2); activate["transfer_id"] = "balance-transfer";
        destination.execute("/v1/activate", activate);
        check(view(destination, ref(2, 2))["self"]["combat"] == frozen["combat"], "migration changed old cooldown");
        mmo::Scene restarted(destination_config);
        check(restarted.execute("/v1/enter", {{"role", frozen}})["role"]["combat"] == frozen["combat"],
            "restart rejected or refreshed old cooldown");
        auto too_long = frozen; too_long["combat"]["cooldowns"]["mend"] = 600001;
        fails(400, [&] { restarted.execute("/v1/enter", {{"role", too_long}}); });

        // 所有失败和预检都必须保持配置及完整玩家状态；预检不占用版本名。
        mmo::Scene strict({}); strict.execute("/v1/enter", {{"role", role()}});
        const auto strict_state = view(strict)["self"]["combat"];
        const auto strict_status = status(strict);
        auto invalid = [&](const std::function<void(Json&)>& mutate) {
            auto bad = candidate; mutate(bad);
            fails(400, [&] { apply_config(strict, bad); });
            fails(400, [&] { strict.execute("/v1/config/validate", {{"config", bad}}); });
            check(status(strict) == strict_status && view(strict)["self"]["combat"] == strict_state,
                "invalid configuration partially applied");
        };
        invalid([](Json& c) { c["schema_version"] = 2; });
        invalid([](Json& c) { c["extra"] = true; });
        invalid([](Json& c) { c.erase("monsters"); });
        invalid([](Json& c) { c["version"] = "not a version"; });
        invalid([](Json& c) { c["skills"]["unknown"] = c["skills"]["strike"]; });
        invalid([](Json& c) { c["skills"]["strike"].erase("damage"); });
        invalid([](Json& c) { c["skills"]["strike"]["damage"] = 18.0; });
        invalid([](Json& c) { c["skills"]["strike"]["heal"] = 1; });
        invalid([](Json& c) { c["skills"]["mend"]["damage"] = 1; });
        invalid([](Json& c) { c["skills"]["mend"]["range"] = 1; });
        invalid([](Json& c) { c["skills"]["strike"]["cooldown_ms"] = 600001; });
        invalid([](Json& c) { c["skills"]["strike"]["mp_cost"] = -1; });
        invalid([](Json& c) { c["skills"]["strike"]["range"] = std::numeric_limits<double>::quiet_NaN(); });
        invalid([](Json& c) { c["skills"]["strike"]["range"] = std::numeric_limits<double>::infinity(); });
        invalid([](Json& c) { c["monsters"]["wolf"]["max_hp"] = 0; });
        invalid([](Json& c) { c["monsters"]["wolf"]["move_speed"] = 0; });
        invalid([](Json& c) { c["monsters"]["wolf"]["attack_interval_ms"] = 99; });
        invalid([](Json& c) { c["monsters"]["wolf"]["respawn_ms"] = 999; });
        invalid([](Json& c) { c["monsters"]["wolf"]["damage"] = 181; });
        fails(400, [&] { strict.execute("/v1/config/apply", {{"config", candidate}, {"path", "other"}}); });
        fails(400, [&] { strict.execute("/v1/config/status", {{"extra", true}}); });
        auto draft = candidate; draft["version"] = "draft";
        strict.execute("/v1/config/validate", {{"config", draft}});
        draft["skills"]["strike"]["damage"] = 41;
        check(apply_config(strict, draft)["generation"] == 2, "validate reserved history content");
        mmo::Config bad_start; bad_start.combat_config = draft; bad_start.combat_config["monsters"].erase("wolf");
        fails(400, [&] { mmo::Scene invalid_start(bad_start); });

        // 怪物在脱战/刷新时才整体使用新参数，死亡等待沿用所持版本。
        mmo::Scene npcs({}); auto nearby = role(); nearby["x"] = -8; nearby["y"] = 4;
        npcs.execute("/v1/enter", {{"role", nearby}});
        npcs.execute("/v1/cast", action(1, "strike", "npc-1"));
        apply_config(npcs, candidate);
        check(mob(npcs)["hp"] == 62 && mob(npcs)["max_hp"] == 80 && mob(npcs)["config_version"] == "default-v1",
            "reload healed or replaced active monster");
        npcs.step(0.1);
        check(view(npcs)["self"]["combat"]["hp"] == 172, "active monster adopted new damage immediately");
        advance(npcs, 32, false);
        check(mob(npcs)["hp"] == 200 && mob(npcs)["config_version"] == "balance-v2", "reset did not adopt new monster config");
        const auto hp = view(npcs)["self"]["combat"]["hp"].get<int>();
        npcs.execute("/v1/cast", action(2, "strike", "npc-1")); npcs.step(0.1);
        check(view(npcs)["self"]["combat"]["hp"] == hp - 1, "reset did not adopt new monster damage");

        mmo::Scene respawn({}); respawn.execute("/v1/enter", {{"role", role()}});
        auto lethal = candidate; lethal["version"] = "lethal";
        lethal["skills"]["strike"].update({{"damage", 10000}, {"cooldown_ms", 0}});
        apply_config(respawn, lethal);
        respawn.execute("/v1/cast", action(1, "strike", "npc-1"));
        check(mob(respawn)["respawn_remaining_ms"] == 8000, "death used new rather than held respawn period");
        advance(respawn, 11); check(mob(respawn)["alive"] == false, "old monster respawn timer was shortened");
        advance(respawn, 69);
        check(mob(respawn)["hp"] == 200 && mob(respawn)["config_version"] == "lethal", "respawn did not adopt new config");
        auto next = lethal; next["version"] = "next"; next["monsters"]["wolf"]["max_hp"] = 300;
        next["monsters"]["wolf"]["respawn_ms"] = 2000;
        apply_config(respawn, next);
        respawn.execute("/v1/cast", action(2, "strike", "npc-1"));
        check(mob(respawn)["respawn_remaining_ms"] == 1000, "monster did not retain adopted respawn period");
        advance(respawn, 10);
        check(mob(respawn)["hp"] == 300 && mob(respawn)["config_version"] == "next", "second respawn version incorrect");

        mmo::Scene history({});
        for (int i = 1; i < 64; ++i) { auto c = defaults; c["version"] = "history-" + std::to_string(i); apply_config(history, c); }
        auto overflow = defaults; overflow["version"] = "history-overflow";
        const auto full_history = status(history);
        fails(503, [&] { apply_config(history, overflow); });
        fails(503, [&] { history.execute("/v1/config/validate", {{"config", overflow}}); });
        check(status(history) == full_history, "history overflow changed active config");
        check(apply_config(history, defaults)["generation"] == 65, "history capacity blocked an existing-version rollback");
        auto renamed_defaults = defaults; renamed_defaults["skills"]["strike"]["damage"] = 19;
        fails(409, [&] { apply_config(history, renamed_defaults); });
        std::cout << "scene config schema, atomic reload, CAS, rollback, old snapshots and NPC version adoption passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "scene config failure: " << error.what() << '\n';
        return 1;
    }
}
