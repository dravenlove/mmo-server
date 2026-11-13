#include "room/worker.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifndef _WIN32
#include <sys/stat.h>
#endif

using native::Json;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void refuses_at(F work, int status, int line) {
    try { work(); }
    catch (const native::Error& error) { check(error.status == status, "wrong native error status"); return; }
    catch (const mmo::Error& error) { check(error.status == status, "wrong scene error status"); return; }
    throw std::runtime_error("invalid room operation accepted at line " + std::to_string(line));
}
#define refuses(...) refuses_at(__VA_ARGS__, __LINE__)
std::string read(const std::filesystem::path& path) { std::ifstream file(path, std::ios::binary); return {(std::istreambuf_iterator<char>(file)), {}}; }
void write(const std::filesystem::path& path, const std::string& value) { std::ofstream file(path, std::ios::binary); file << value; }
Json role(const std::string& id, const std::string& scene) {
    return {{"role_id", id}, {"name", id}, {"scene_id", scene}, {"owner_epoch", 1}, {"session_epoch", 1}, {"coins", 17}, {"x", 0}, {"y", 0}};
}
Json ref(const std::string& id, int session = 1) { return {{"role_id", id}, {"owner_epoch", 1}, {"session_epoch", session}}; }
Json creation(const std::string& id, const std::string& host = "shared") {
    return {{"room_id", id}, {"host_role_id", host}, {"capacity", 2}, {"profile", "skill_lab"}};
}
std::string route(const std::string& id, const std::string& op) { return "/rooms/" + id + "/v1/" + op; }
Json projectile() { return {{"type", "projectile"}, {"damage", 7}, {"mp_cost", 2}, {"cooldown_ms", 100}, {"range", 20}, {"speed", 12}, {"radius", .2}}; }
Json wall() { return {{"type", "wall"}, {"damage", 0}, {"mp_cost", 3}, {"cooldown_ms", 100}, {"range", 8}, {"length", 2}, {"thickness", .5}, {"duration_ms", 1000}}; }
}
int main() {
    const auto directory = std::filesystem::temp_directory_path() / ("mmo-room-core-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{directory};
    try {
        std::filesystem::create_directories(directory / "versions");
        const auto original_manifest = native::room::read_json_file(ROOM_FIXTURE_MANIFEST);
        const auto v1 = original_manifest.at("version").get<std::string>();
        const auto source = read(std::filesystem::path(ROOM_FIXTURE_MANIFEST).parent_path() / original_manifest.at("file").get<std::string>());
        write(directory / "skills_v1.lua", source);
        auto manifest = original_manifest; manifest["file"] = "skills_v1.lua";
        write(directory / "manifest.json", manifest.dump()); write(directory / "versions" / (v1 + ".json"), manifest.dump());
        native::room::Config config;
        config.scene.scene_id = "legacy"; config.scene.skill_lab_enabled = true; config.scene.abilities_enabled = true;
        config.script_manifest = (directory / "manifest.json").string(); config.max_rooms = 4; config.finished_retention_seconds = 0;
        native::room::Manager manager(config);
        auto a = manager.execute("/v1/rooms/create", creation("room-one"));
        auto b = manager.execute("/v1/rooms/create", creation("room-two"));
        check(a["world_epoch"] != b["world_epoch"], "rooms share world generation");
        check(manager.execute("/v1/rooms/create", creation("room-one"))["world_epoch"] == a["world_epoch"], "create retry allocated another room");
        auto changed = creation("room-one"); changed["capacity"] = 3;
        refuses([&] { manager.execute("/v1/rooms/create", changed); }, 409);
        auto enter = [&](const std::string& room, const std::string& id) { return manager.execute(route(room, "enter"), {{"role", role(id, room)}}); };
        enter("room-one", "shared"); enter("room-two", "shared"); enter("room-one", "peer");
        refuses([&] { enter("room-one", "overflow"); }, 409);
        auto move = ref("shared"); move.update({{"seq", 1}, {"dx", 1}, {"dy", 0}});
        refuses([&] { manager.execute(route("room-one", "move"), move); }, 409);
        for (int i = 0; i < 10; ++i) manager.step(.05);
        check(manager.execute(route("room-one", "view"), ref("shared"))["tick"] == 0, "waiting room simulated combat");
        auto ready = [&](const std::string& room, const std::string& id, int session = 1) { auto req = ref(id, session); req["ready"] = true; return manager.execute(route(room, "ready"), req); };
        ready("room-one", "shared");
        refuses([&] { manager.execute(route("room-one", "start"), ref("peer")); }, 403);
        refuses([&] { manager.execute(route("room-one", "start"), ref("shared")); }, 409);
        ready("room-one", "peer"); ready("room-two", "shared");
        check(manager.execute(route("room-one", "start"), ref("shared"))["state"] == "playing", "host start failed");
        manager.execute(route("room-two", "start"), ref("shared"));
        const auto pinned_config = manager.execute(route("room-one", "config/status"), Json::object());
        refuses([&] { manager.execute(route("room-one", "config/apply"), {{"config", pinned_config["config"]}}); }, 409);
        refuses([&] { manager.execute(route("room-one", "config/reload"), {{"config", pinned_config["config"]}}); }, 409);
        check(manager.execute(route("room-one", "config/validate"), {{"config", pinned_config["config"]}})["valid"], "read-only room rule validation was disabled");
        const auto legacy_config = manager.execute("/v1/config/status", Json::object());
        check(manager.execute("/v1/config/apply", {{"config", legacy_config["config"]}})["version"] == legacy_config["version"], "legacy config apply compatibility changed");
        manager.execute(route("room-one", "move"), move); manager.step(.05);
        auto first = manager.execute(route("room-one", "view"), ref("shared"));
        auto second = manager.execute(route("room-two", "view"), ref("shared"));
        check(first["self"]["x"].get<double>() > second["self"]["x"].get<double>(), "same role movement crossed rooms");
        auto initialize = [&](const std::string& room, const std::string& id, const Json& def) {
            auto view = manager.execute(route(room, "view"), ref("shared")); auto req = ref("shared");
            req.update({{"skill_id", id}, {"definition", def}, {"expected_definition_version", 0}, {"world_epoch", view["world_epoch"]}});
            auto result = manager.execute(route(room, "skill_init"), req); check(result["accepted"], "skill registration failed");
            return result["definition"]["definition_version"];
        };
        const auto bolt_version = initialize("room-one", "client-bolt", projectile());
        auto cast = ref("shared"); cast.update({{"skill_id", "client-bolt"}, {"seq", 1}, {"definition_version", bolt_version}, {"world_epoch", first["world_epoch"]}, {"dx", 1}, {"dy", 0}});
        check(manager.execute(route("room-one", "cast"), cast)["accepted"], "Lua projectile failed");
        first = manager.execute(route("room-one", "view"), ref("shared")); second = manager.execute(route("room-two", "view"), ref("shared"));
        check(first["projectiles"].size() == 1 && second["projectiles"].empty(), "projectile crossed room registry");
        check(first["events"].size() > second["events"].size(), "events crossed room registry");
        // 发布只改变之后创建的房间；老房间代码由shared_ptr持有，不再读漂移文件。
        const std::string v2_source = "return { plan=function(ctx) if ctx.definition.type=='wall' then host.deny('walls_paused') else host.emit(ctx.definition.type,ctx.skill_id) end end }";
        native::script::Script v2("skills-v2", v2_source);
        write(directory / "skills_v2.lua", v2_source);
        write(directory / "manifest.json", Json{{"version", "skills-v2"}, {"file", "skills_v2.lua"}, {"sha256", v2.sha256()}}.dump());
        auto published = manager.execute("/v1/rooms/scripts/reload", {{"expected_generation", 1}});
        check(published["script_version"] == "skills-v2", "script publish failed");
        check(manager.execute("/v1/rooms/create", creation("room-one"))["script_version"] == v1, "create retry rebound script after publication");
        auto third = manager.execute("/v1/rooms/create", creation("room-three"));
        check(third["script_version"] == "skills-v2", "new room did not use active script");
        auto damaged = role("shared", "room-three"); damaged["combat"] = first["self"]["combat"];
        damaged["combat"]["hp"] = 77; damaged["combat"]["last_action_seq"] = 0;
        manager.execute(route("room-three", "enter"), {{"role", damaged}}); ready("room-three", "shared"); manager.execute(route("room-three", "start"), ref("shared"));
        auto third_wall = initialize("room-three", "client-wall", wall());
        auto wall_cast = ref("shared"); wall_cast.update({{"skill_id", "client-wall"}, {"seq", 1}, {"definition_version", third_wall}, {"world_epoch", third["world_epoch"]}, {"dx", 1}, {"dy", 0}, {"x", 4}, {"y", 4}});
        const auto before = manager.execute(route("room-three", "view"), ref("shared"));
        const auto denied = manager.execute(route("room-three", "cast"), wall_cast);
        check(!denied["accepted"].get<bool>() && denied["reason"] == "script_denied", "new room did not execute pinned script denial policy");
        const auto after = manager.execute(route("room-three", "view"), ref("shared"));
        check(after["self"]["combat"] == before["self"]["combat"] && after["walls"].empty(), "script denial partially consumed resources");
        auto old_wall = initialize("room-one", "client-wall", wall());
        wall_cast.update({{"seq", 2}, {"definition_version", old_wall}, {"world_epoch", a["world_epoch"]}});
        check(manager.execute(route("room-one", "cast"), wall_cast)["accepted"], "old room did not retain old Lua program");
        write(directory / "manifest.json", Json{{"version", "skills-v3"}, {"file", "skills_v2.lua"}, {"sha256", std::string(64, '0')}}.dump());
        refuses([&] { manager.execute("/v1/rooms/scripts/reload", Json::object()); }, 400);
        check(manager.execute("/v1/rooms/scripts/status", Json::object())["script_version"] == "skills-v2", "failed publish changed active version");
        // 重启只从可信archive重新取得固定脚本，不悄悄用新版本重建旧房间。
        write(directory / "manifest.json", Json{{"version", "skills-v2"}, {"file", "skills_v2.lua"}, {"sha256", v2.sha256()}}.dump());
        native::room::Manager restarted(config);
        auto restore = creation("room-restored"); restore["script_version"] = v1;
        check(restarted.execute("/v1/rooms/create", restore)["script_version"] == v1, "pinned script archive failed after restart");
        restore["room_id"] = "room-unavailable"; restore["script_version"] = "missing-v9";
        refuses([&] { restarted.execute("/v1/rooms/create", restore); }, 409);
        auto pinned = creation("room-pinned"); pinned["script_version"] = v1; pinned["script_sha256"] = manifest["sha256"];
        write(directory / "skills_v1.lua", v2_source);
        native::script::Script changed_v1(v1, v2_source);
        write(directory / "versions" / (v1 + ".json"), Json{{"version", v1}, {"file", "skills_v1.lua"}, {"sha256", changed_v1.sha256()}}.dump());
        native::room::Manager checksum_restart(config);
        refuses([&] { checksum_restart.execute("/v1/rooms/create", pinned); }, 409);
        write(directory / "skills_v1.lua", source); write(directory / "versions" / (v1 + ".json"), manifest.dump());
#ifndef _WIN32
        check(mkfifo((directory / "blocked.fifo").c_str(), 0600) == 0, "cannot create synthetic FIFO fixture");
        write(directory / "manifest.json", Json{{"version", "skills-v3"}, {"file", "blocked.fifo"}, {"sha256", std::string(64, '0')}}.dump());
        refuses([&] { manager.execute("/v1/rooms/scripts/reload", Json::object()); }, 400);
        write(directory / "manifest.json", Json{{"version", "skills-v2"}, {"file", "skills_v2.lua"}, {"sha256", v2.sha256()}}.dump());
#endif
        // 同进程断线重连使用新session，旧session不可施法，角色身份/金币保持。
        manager.execute(route("room-two", "disconnect"), ref("shared"));
        auto returned = role("shared", "room-two"); returned["session_epoch"] = 2;
        manager.execute(route("room-two", "enter"), {{"role", returned}});
        refuses([&] { manager.execute(route("room-two", "view"), ref("shared")); }, 409);
        check(manager.execute(route("room-two", "view"), ref("shared", 2))["self"]["coins"] == 17, "reconnect changed persistent identity");
        manager.execute(route("room-three", "finish"), ref("shared"));
        auto unavailable = ref("shared"); unavailable.update({{"seq", 2}, {"dx", 1}, {"dy", 0}});
        refuses([&] { manager.execute(route("room-three", "move"), unavailable); }, 409);
        manager.execute(route("room-three", "disconnect"), ref("shared")); manager.step(.05);
        check(manager.execute("/v1/rooms/status", {{"room_id", "room-three"}})["member_count"] == 1, "disconnected room discarded an unacknowledged final snapshot");
        auto stale_database_role = role("shared", "room-three"); stale_database_role["session_epoch"] = 2;
        manager.execute(route("room-three", "enter"), {{"role", stale_database_role}});
        auto final = manager.execute(route("room-three", "view"), ref("shared", 2))["self"];
        check(final["combat"]["hp"] == 77, "finished reconnect rolled back authoritative HP to old database snapshot");
        check(!manager.execute(route("room-three", "persisted"), {{"role", final}})["released"].get<bool>(), "save ACK removed a connected live role");
        auto frozen_ref = ref("shared", 2); frozen_ref["transfer_id"] = "frozen-save-ack";
        manager.execute(route("room-three", "freeze"), frozen_ref);
        check(!manager.execute(route("room-three", "persisted"), {{"role", final}})["released"].get<bool>(), "save ACK removed a frozen ownership transition");
        manager.execute(route("room-three", "unfreeze"), frozen_ref);
        final = manager.execute(route("room-three", "disconnect"), ref("shared", 2))["role"];
        auto stale_saved = final; stale_saved["combat"]["hp"] = 180;
        check(!manager.execute(route("room-three", "persisted"), {{"role", stale_saved}})["released"].get<bool>(), "old saved snapshot released a newer authoritative role");
        stale_saved = final; stale_saved["session_epoch"] = 99;
        check(!manager.execute(route("room-three", "persisted"), {{"role", stale_saved}})["released"].get<bool>(), "stale session ACK removed another role generation");
        check(manager.execute(route("room-three", "final_roles"), Json::object())["roles"][0]["role"]["combat"]["hp"] == 77, "internal final snapshot changed");
        final["coins"] = 999; // 余额以Backend为准，不阻止相同战斗/位置快照的ACK。
        check(manager.execute(route("room-three", "persisted"), {{"role", final}})["released"], "correct save ACK did not remove disconnected role");
        check(manager.execute(route("room-three", "persisted"), {{"role", final}})["released"], "save ACK retry was not idempotent");
        manager.step(.05);
        refuses([&] { manager.execute("/v1/rooms/status", {{"room_id", "room-three"}}); }, 404);
        refuses([&] { manager.execute("/v1/rooms/create", creation("room-three")); }, 409);
        auto never_joined = manager.execute("/v1/rooms/create", creation("room-empty"));
        auto control = ref("shared"); control["control_authorized"] = true;
        manager.execute(route("room-empty", "finish"), control); manager.step(.05);
        refuses([&] { manager.execute("/v1/rooms/status", {{"room_id", "room-empty"}}); }, 404);
        check(manager.health()["rooms"] == 2, "room cleanup removed another room");
        native::room::Manager lease_test(config);
        lease_test.execute("/v1/rooms/create", creation("room-idle"));
        for (const auto* id : {"shared", "peer"}) {
            lease_test.execute(route("room-idle", "enter"), {{"role", role(id, "room-idle")}});
            auto input = ref(id); input["ready"] = true; lease_test.execute(route("room-idle", "ready"), input);
        }
        for (int i = 0; i < 80; ++i) lease_test.step(.05);
        auto idle = lease_test.execute("/v1/rooms/status", {{"room_id", "room-idle"}});
        check(idle["tick"] == 0 && idle["members"][0]["lease_expired"] == true, "waiting room advanced simulation or froze liveness leases");
        refuses([&] { lease_test.execute(route("room-idle", "start"), ref("shared")); }, 409);
        auto host_ready = ref("shared"); host_ready["ready"] = true;
        lease_test.execute(route("room-idle", "ready"), host_ready);
        auto guest_ready = ref("peer"); guest_ready["ready"] = true;
        lease_test.execute(route("room-idle", "ready"), guest_ready);
        lease_test.execute(route("room-idle", "start"), ref("shared"));
        lease_test.execute(route("room-idle", "finish"), ref("shared"));
        const auto exported = lease_test.execute(route("room-idle", "final_roles"), Json::object());
        for (int i = 0; i < 80; ++i) lease_test.step(.05);
        check(lease_test.execute("/v1/rooms/status", {{"room_id", "room-idle"}})["member_count"] == 2, "lease expiry discarded unconfirmed snapshots");
        for (const auto& item : exported["roles"])
            check(lease_test.execute(route("room-idle", "persisted"), {{"role", item["role"]}})["released"], "saved expired role was not released");
        lease_test.step(.05);
        refuses([&] { lease_test.execute("/v1/rooms/status", {{"room_id", "room-idle"}}); }, 404);
        // 同room_id重建后，旧Worker世界的迟到控制面命令不能生成prepared实体或结束新房间。
        native::room::Manager previous_worker(config), next_worker(config);
        const auto previous_world = previous_worker.execute("/v1/rooms/create", creation("room-world-fence"));
        const auto next_world = next_worker.execute("/v1/rooms/create", creation("room-world-fence"));
        check(previous_world["world_epoch"] != next_world["world_epoch"], "worker restart reused world epoch");
        auto old_enter = Json{{"role", role("shared", "room-world-fence")}, {"_room_world_epoch", previous_world["world_epoch"]}};
        refuses([&] { next_worker.execute(route("room-world-fence", "enter"), old_enter); }, 409);
        old_enter["transfer_id"] = "old-worker-prepare";
        refuses([&] { next_worker.execute(route("room-world-fence", "prepare"), old_enter); }, 409);
        auto old_finish = ref("shared"); old_finish["control_authorized"] = true; old_finish["_room_world_epoch"] = previous_world["world_epoch"];
        refuses([&] { next_worker.execute(route("room-world-fence", "finish"), old_finish); }, 409);
        auto fenced_state = next_worker.execute("/v1/rooms/status", {{"room_id", "room-world-fence"}});
        check(fenced_state["state"] == "lobby" && fenced_state["member_count"] == 0, "old world command mutated rebuilt room");
        auto current_enter = Json{{"role", role("shared", "room-world-fence")}, {"_room_world_epoch", next_world["world_epoch"]}};
        next_worker.execute(route("room-world-fence", "enter"), current_enter);
        auto wrong_public_epoch = ref("shared");
        wrong_public_epoch.update({{"_room_world_epoch", next_world["world_epoch"]}, {"world_epoch", previous_world["world_epoch"]},
                                   {"skill_id", "separate-public-fence"}, {"expected_definition_version", 0}, {"definition", projectile()}});
        check(!next_worker.execute(route("room-world-fence", "skill_init"), wrong_public_epoch)["accepted"].get<bool>(), "private fence removal erased client world_epoch validation");
        auto current_ready = ref("shared"); current_ready["ready"] = true; current_ready["_room_world_epoch"] = next_world["world_epoch"];
        check(next_worker.execute(route("room-world-fence", "ready"), current_ready)["members"][0]["ready"], "current private fence was not stripped before room field validation");
        std::cout << "room isolation, Lua publication pinning, lifecycle, archive, authority and reconnect passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "room tests failed: " << error.what() << '\n'; return 1; }
}
