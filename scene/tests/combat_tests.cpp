#include "scene.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

using mmo::Json;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void fails(int status, const std::function<void()>& operation) {
    try { operation(); }
    catch (const mmo::Error& error) { check(error.status == status, "unexpected rejection status"); return; }
    throw std::runtime_error("expected operation rejection");
}
Json role(const std::string& id = "hero", const std::string& scene = "scene-1", int owner = 1, int session = 1) {
    return {{"role_id", id}, {"name", id}, {"scene_id", scene}, {"owner_epoch", owner},
        {"session_epoch", session}, {"x", 0}, {"y", 0}, {"coins", 0}};
}
Json actor_ref(const std::string& id = "hero", int owner = 1, int session = 1) {
    return {{"role_id", id}, {"owner_epoch", owner}, {"session_epoch", session}};
}
Json action(const Json& reference, int seq, const std::string& skill, const std::string& target = "") {
    auto result = reference;
    result.update({{"seq", seq}, {"skill_id", skill}, {"target_id", target}});
    return result;
}
Json transfer(const Json& reference, const std::string& id) { auto result = reference; result["transfer_id"] = id; return result; }
Json view(mmo::Scene& scene, const Json& reference = actor_ref()) { return scene.execute("/v1/view", reference); }
Json mob(const Json& state, const std::string& id) {
    for (const auto& candidate : state["mobs"]) if (candidate["id"] == id) return candidate;
    throw std::runtime_error("expected monster in AOI");
}
void advance(mmo::Scene& scene, double seconds, const Json& reference = actor_ref(), bool heartbeat = true) {
    for (int i = 0; i < static_cast<int>(std::ceil(seconds * 10)); ++i) {
        if (heartbeat) view(scene, reference);
        scene.step(0.1);
    }
}
}

int main() {
    try {
        mmo::Scene scene({});
        scene.execute("/v1/enter", {{"role", role()}});
        auto initial = view(scene);
        check(initial["mobs"].size() == 4 && initial["skills"].size() == 4, "combat catalog or monsters missing");
        check(initial["self"]["combat"]["hp"] == 180 && initial["self"]["combat"]["mp"] == 100, "legacy role defaults incorrect");
        fails(400, [&] { scene.execute("/v1/cast", action(actor_ref(), 1, "forged")); });
        fails(404, [&] { scene.execute("/v1/cast", action(actor_ref(), 1, "strike", "other-player")); });
        fails(409, [&] { scene.execute("/v1/cast", action(actor_ref(), 1, "strike", "npc-1")); });
        check(view(scene)["self"]["combat"] == initial["self"]["combat"], "rejected cast mutated resources");

        const auto charge = action(actor_ref(), 1, "charge", "npc-1");
        auto charged = scene.execute("/v1/cast", charge);
        check(charged["hits"][0]["damage"] == 24 && charged["role"]["combat"]["mp"] == 85, "charge damage or cost incorrect");
        check(scene.execute("/v1/cast", charge) == charged, "identical skill retry changed result");
        fails(409, [&] { scene.execute("/v1/cast", action(actor_ref(), 1, "charge", "npc-2")); });
        fails(409, [&] { scene.execute("/v1/cast", action(actor_ref(), 2, "charge", "npc-1")); });
        check(mob(view(scene), "npc-1")["hp"] == 56, "retry or cooldown rejection dealt damage");
        scene.execute("/v1/cast", action(actor_ref(), 2, "strike", "npc-1"));
        scene.execute("/v1/cast", action(actor_ref(), 3, "whirlwind"));
        check(mob(view(scene), "npc-1")["hp"] == 6, "single-target/AOE damage incorrect");
        fails(409, [&] { scene.execute("/v1/cast", action(actor_ref(), 4, "strike", "npc-1")); });
        advance(scene, .6);
        scene.execute("/v1/cast", action(actor_ref(), 4, "strike", "npc-1"));
        check(mob(view(scene), "npc-1")["alive"] == false, "monster did not die");
        fails(409, [&] { scene.execute("/v1/cast", action(actor_ref(), 5, "strike", "npc-1")); });
        advance(scene, 8);
        check(mob(view(scene), "npc-1")["hp"] == 80, "monster did not respawn");
        check(view(scene)["events"].size() <= 32, "event history is unbounded");

        mmo::Scene healer({});
        auto injured = role(); injured["combat"] = initial["self"]["combat"]; injured["combat"]["hp"] = 60;
        healer.execute("/v1/enter", {{"role", injured}});
        auto heal = action(actor_ref(), 1, "mend");
        auto healed = healer.execute("/v1/cast", heal);
        check(healed["healed"] == 45 && healed["role"]["combat"]["hp"] == 105, "healing incorrect");
        check(healer.execute("/v1/cast", heal) == healed, "healing retry doubled effect");
        advance(healer, .5);
        auto before_freeze = view(healer)["self"]["combat"];
        check(before_freeze["mp"] == 78 && before_freeze["cooldowns"]["mend"] == 11500, "mana regeneration or cooldown tick incorrect");
        auto frozen_ref = transfer(actor_ref(), "combat-transfer");
        auto frozen = healer.execute("/v1/freeze", frozen_ref)["role"];
        healer.step(1);
        check(healer.execute("/v1/freeze", frozen_ref)["role"]["combat"] == before_freeze, "frozen combat state changed");
        fails(409, [&] { healer.execute("/v1/cast", action(actor_ref(), 2, "mend")); });
        auto revive_frozen = actor_ref(); revive_frozen["seq"] = 2;
        fails(409, [&] { healer.execute("/v1/revive", revive_frozen); });
        healer.execute("/v1/unfreeze", frozen_ref);
        auto newer = role("hero", "scene-1", 1, 2);
        auto logged = healer.execute("/v1/enter", {{"role", newer}})["role"]["combat"];
        check(logged["hp"] == 105 && logged["cooldowns"]["mend"] == 11500 && logged["last_action_seq"] == 0, "relogin reset combat state");
        check(logged["action_session"] == 2 && logged["revision"] > before_freeze["revision"], "new-session combat revision missing");
        fails(409, [&] { healer.execute("/v1/cast", action(actor_ref(), 2, "mend")); });
        fails(409, [&] { healer.execute("/v1/cast", action(actor_ref("hero", 1, 2), 1, "mend")); });

        // 迁移使用完整冻结快照，准备态停止计时，激活后不刷新资源与冷却。
        auto snapshot = healer.execute("/v1/freeze", transfer(actor_ref("hero", 1, 2), "next"))["role"];
        snapshot["scene_id"] = "scene-2"; snapshot["owner_epoch"] = 2;
        mmo::Config second_config; second_config.scene_id = "scene-2";
        mmo::Scene destination(second_config);
        auto preparing = Json{{"role", snapshot}, {"transfer_id", "next"}};
        destination.execute("/v1/prepare", preparing); destination.step(1);
        auto prepared = destination.execute("/v1/prepare", preparing)["role"];
        check(prepared["combat"] == snapshot["combat"], "prepared combat timers ran");
        fails(409, [&] { destination.execute("/v1/cast", action(actor_ref("hero", 2, 2), 1, "mend")); });
        destination.execute("/v1/activate", transfer(actor_ref("hero", 2, 2), "next"));
        check(view(destination, actor_ref("hero", 2, 2))["self"]["combat"] == snapshot["combat"], "migration refreshed combat");

        // 同会话迁移会保留动作高水位；缓存不跨进程恢复时拒绝重复结算。
        mmo::Scene caster({}); caster.execute("/v1/enter", {{"role", role()}});
        caster.execute("/v1/cast", action(actor_ref(), 9, "charge", "npc-1"));
        auto migrated = caster.execute("/v1/freeze", transfer(actor_ref(), "sequence"))["role"];
        migrated["scene_id"] = "scene-2"; migrated["owner_epoch"] = 2;
        mmo::Scene target(second_config);
        target.execute("/v1/prepare", {{"role", migrated}, {"transfer_id", "sequence"}});
        target.execute("/v1/activate", transfer(actor_ref("hero", 2), "sequence"));
        fails(409, [&] { target.execute("/v1/cast", action(actor_ref("hero", 2), 9, "charge", "npc-1")); });
        check(mob(view(target, actor_ref("hero", 2)), "npc-1")["hp"] == 80, "replayed cast hit target scene");

        mmo::Scene death({});
        auto fragile = role(); fragile["combat"] = initial["self"]["combat"]; fragile["combat"]["hp"] = 1;
        death.execute("/v1/enter", {{"role", fragile}});
        death.execute("/v1/cast", action(actor_ref(), 1, "charge", "npc-1"));
        advance(death, 1.2);
        auto dead = view(death)["self"]["combat"];
        check(dead["dead"] == true && dead["hp"] == 0, "monster AI did not retaliate or kill");
        auto move = actor_ref(); move.update({{"seq", 1}, {"dx", 1}, {"dy", 0}});
        fails(409, [&] { death.execute("/v1/move", move); });
        fails(409, [&] { death.execute("/v1/cast", action(actor_ref(), 2, "mend")); });
        auto resurrect = actor_ref(); resurrect["seq"] = 2;
        fails(409, [&] { death.execute("/v1/revive", resurrect); });
        advance(death, 5);
        auto revived = death.execute("/v1/revive", resurrect);
        check(revived["role"]["combat"]["hp"] == 180 && revived["role"]["combat"]["mp"] == 100, "revive did not restore resources");
        check(revived["role"]["x"] == 0 && revived["role"]["y"] == 0, "revive did not return to spawn");
        check(revived["role"]["combat"]["cooldowns"]["charge"].get<int>() > 0, "revive refreshed cooldowns");
        check(death.execute("/v1/revive", resurrect) == revived, "revive retry was not idempotent");

        mmo::Scene poor({}); auto empty = injured; empty["combat"]["mp"] = 0;
        poor.execute("/v1/enter", {{"role", empty}});
        fails(409, [&] { poor.execute("/v1/cast", action(actor_ref(), 1, "mend")); });
        check(view(poor)["self"]["combat"]["hp"] == 60, "mana rejection healed player");

        mmo::Scene offline({}); auto nearby = role(); nearby["x"] = -8; nearby["y"] = 4;
        offline.execute("/v1/enter", {{"role", nearby}});
        offline.execute("/v1/cast", action(actor_ref(), 1, "strike", "npc-1"));
        advance(offline, 4.1, actor_ref(), false);
        auto abandoned = view(offline);
        int remaining_hp = abandoned["self"]["combat"]["hp"];
        check(remaining_hp < 180 && remaining_hp > 0 && mob(abandoned, "npc-1")["target_id"] == "", "offline lease did not reset monster");
        advance(offline, 2);
        check(view(offline)["self"]["combat"]["hp"] == remaining_hp, "monster kept attacking offline player");

        auto bad = injured; bad["combat"]["hp"] = 999;
        fails(400, [&] { poor.execute("/v1/enter", {{"role", bad}}); });
        bad = injured; bad["combat"]["dead"] = true;
        fails(400, [&] { poor.execute("/v1/enter", {{"role", bad}}); });
        bad = injured; bad["combat"]["revision"] = INT64_MAX;
        fails(400, [&] { poor.execute("/v1/enter", {{"role", bad}}); });

        mmo::Config full_config; full_config.scene_id = std::string(128, 's');
        mmo::Scene crowded(full_config);
        std::string own;
        for (int i = 0; i < 128; ++i) {
            std::string id = std::to_string(i) + std::string(124, 'x');
            auto long_role = role(id, full_config.scene_id); long_role["name"] = std::string(96, 'n');
            crowded.execute("/v1/enter", {{"role", long_role}});
            if (i == 127) own = id;
        }
        auto crowded_view = view(crowded, actor_ref(own));
        check(crowded_view.dump().size() < 65536, "combat AOI frame exceeded 64KiB");
        check(crowded_view["players_truncated"] == true && crowded_view["visible_player_count"] == 128, "AOI truncation not reported");
        bool contains_self = false;
        for (const auto& player : crowded_view["players"]) if (player["role_id"] == own) contains_self = true;
        check(contains_self, "bounded AOI omitted caller");

        std::cout << "scene combat, AI, lifecycle, migration and frame limits passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "scene combat failure: " << error.what() << '\n';
        return 1;
    }
}
