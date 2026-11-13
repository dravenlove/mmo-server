#include "rooms_tests.hpp"
#include "game_service/service.hpp"
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace native::game::test {
namespace {
using I = std::int64_t;
void check(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
template<class Action> void refuses(int expected, Action action) {
    try { action(); } catch (const Error& error) { if (error.status == expected) return; throw; }
    throw std::runtime_error("room operation unexpectedly succeeded");
}
Json reference(const Json& role) {
    return {{"role_id", role.at("role_id")}, {"owner_epoch", role.at("owner_epoch")}, {"session_epoch", role.at("session_epoch")}};
}
Json combat(I hp = 180, I revision = 1, I session = 1) {
    return {{"hp", hp}, {"max_hp", 180}, {"mp", 37}, {"max_mp", 100}, {"dead", hp == 0}, {"revive_remaining_ms", 0},
            {"revision", revision}, {"last_action_seq", 0}, {"action_session", session},
            {"cooldowns", {{"strike", 0}, {"whirlwind", 0}, {"charge", 0}, {"mend", 0}}}};
}
struct Player { Json role; std::string mode, transfer; bool ready = false, connected = true; };
struct Arena {
    Json spec;
    std::string state = "lobby";
    std::map<std::string, Player> players;
    std::map<std::string, I> removed;
    I generation = 1;
};
// Worker是带真实HTTP边界的控制/迁移夹具，不实现战斗算法；战斗由Scene/Worker测试独立覆盖。
struct Worker {
    httplib::Server server;
    std::thread thread;
    std::mutex mutex;
    std::string url, token, default_version = "script-v1";
    std::map<std::string, std::string> hashes{{"script-v1", std::string(64, 'a')}, {"script-v2", std::string(64, 'c')}};
    Arena lobby;
    std::map<std::string, Arena> rooms;
    int lost_lobby_admission = 0;
    bool management_unavailable = false;
    I creations = 0;
    Json last_create;

    explicit Worker(std::string secret) : token(std::move(secret)) {
        lobby.spec = {{"room_id", "scene-1"}};
        server.new_task_queue = [] { return new httplib::ThreadPool(4, 4, 32); };
        server.set_keep_alive_max_count(1);
        auto handler = [this](const httplib::Request& request, httplib::Response& response) {
            if (request.get_header_value("X-Internal-Token") != token) { response.status = 401; return; }
            std::lock_guard<std::mutex> guard(mutex);
            try {
                auto value = dispatch(request.path, Json::parse(request.body));
                response.set_content(value.dump(), "application/json");
            } catch (const Error& error) {
                response.status = error.status; response.set_content(Json{{"error", error.what()}}.dump(), "application/json");
            } catch (...) {
                response.status = 500; response.set_content("{\"error\":\"synthetic worker failure\"}", "application/json");
            }
        };
        server.Post(R"(/v1/.*)", handler);
        server.Post(R"(/rooms/[^/]+/v1/.*)", handler);
        const int port = server.bind_to_any_port("127.0.0.1");
        check(port > 0, "room worker fixture bind failed");
        url = "http://127.0.0.1:" + std::to_string(port);
        thread = std::thread([this] { server.listen_after_bind(); }); server.wait_until_ready();
    }
    ~Worker() { server.stop(); thread.join(); }
    Json metadata(const Arena& arena) const {
        Json members = Json::array();
        for (const auto& item : arena.players) {
            const auto& p = item.second;
            auto row = reference(p.role); row["name"] = p.role["name"]; row["mode"] = p.mode; row["connected"] = p.connected; row["ready"] = p.ready;
            members.push_back(std::move(row));
        }
        auto out = arena.spec; out["state"] = arena.state; out["members"] = members;
        out["member_count"] = members.size(); out["scene_id"] = out["room_id"];
        out["world_epoch"] = "synthetic-world-" + std::to_string(arena.generation);
        return out;
    }
    Json dispatch(const std::string& path, const Json& request) {
        if (management_unavailable && path.rfind("/v1/rooms/", 0) == 0) throw Error(503, "synthetic unavailable worker management");
        if (path == "/v1/rooms/create") {
            const auto id = request.at("room_id").get<std::string>(); last_create = request;
            if (rooms.count(id)) return metadata(rooms.at(id));
            Arena arena; arena.spec = request;
            const auto version = request.value("script_version", default_version);
            if (!hashes.count(version)) throw Error(404, "script version missing");
            arena.spec["script_version"] = version; arena.spec["script_sha256"] = hashes.at(version);
            if (request.contains("script_sha256") && request["script_sha256"] != arena.spec["script_sha256"])
                throw Error(409, "script archive digest conflict");
            arena.generation = ++creations; rooms.emplace(id, std::move(arena)); return metadata(rooms.at(id));
        }
        if (path == "/v1/rooms/status") {
            const auto id = request.at("room_id").get<std::string>();
            if (!rooms.count(id)) throw Error(404, "room not found");
            return metadata(rooms.at(id));
        }
        Arena* arena = &lobby;
        auto operation = path.substr(4);
        bool temporary = path.rfind("/rooms/", 0) == 0;
        if (temporary) {
            const auto slash = path.find('/', 7);
            const auto id = path.substr(7, slash - 7);
            if (!rooms.count(id)) throw Error(404, "room not found");
            arena = &rooms.at(id); operation = path.substr(slash + 4);
        }
        if (operation == "capabilities") return {{"owner_rotation", false}};
        const Json role = request.value("role", Json::object());
        const Json actor = operation == "enter" || operation == "prepare" ? role : request;
        const auto id = actor.at("role_id").get<std::string>();
        auto found = arena->players.find(id);
        bool exists = found != arena->players.end();
        auto exact = [&] { return exists && found->second.role["owner_epoch"] == actor["owner_epoch"] && found->second.role["session_epoch"] == actor["session_epoch"]; };
        const auto transfer = request.value("transfer_id", std::string{});
        if (operation == "enter") {
            if (!exists) {
                if ((temporary && arena->state == "finished") || arena->removed[id] >= actor["owner_epoch"].get<I>()) throw Error(409, "owner removed");
                Player player{role, "active", {}};
                if (!player.role.contains("combat")) player.role["combat"] = combat(180, 1, role.at("session_epoch"));
                arena->players.emplace(id, std::move(player));
            } else {
                auto& player = found->second;
                if (player.mode != "active" || player.role["owner_epoch"] != actor["owner_epoch"] || player.role["session_epoch"].get<I>() > actor["session_epoch"].get<I>())
                    throw Error(409, "owner or session changed");
                if (!player.connected && player.role["session_epoch"] == actor["session_epoch"]) throw Error(409, "disconnected session");
                if (player.role["session_epoch"] != actor["session_epoch"]) {
                    player.ready = false; player.role["combat"]["action_session"] = actor["session_epoch"];
                    player.role["combat"]["last_action_seq"] = 0;
                    player.role["combat"]["revision"] = player.role["combat"]["revision"].get<I>() + 1;
                }
                player.connected = true;
                player.role["session_epoch"] = actor["session_epoch"]; player.role["coins"] = actor["coins"];
            }
            if (!temporary && lost_lobby_admission > 0) { --lost_lobby_admission; throw Error(503, "synthetic lost admission response"); }
            return {{"role", arena->players.at(id).role}};
        }
        if (operation == "prepare") {
            if (exists) {
                if (!exact() || found->second.transfer != transfer) throw Error(409, "prepare conflict");
            } else {
                if ((temporary && arena->state == "finished") || arena->removed[id] >= actor["owner_epoch"].get<I>()) throw Error(409, "owner removed");
                arena->players.emplace(id, Player{role, "prepared", transfer});
            }
            return {{"role", arena->players.at(id).role}};
        }
        if (operation == "remove") {
            if (exists && !exact()) throw Error(409, "stale remove");
            arena->removed[id] = std::max(arena->removed[id], actor["owner_epoch"].get<I>());
            arena->players.erase(id); return {{"accepted", true}};
        }
        if (temporary && operation == "finish" && request.value("control_authorized", false)) {
            if (arena->spec.at("host_role_id") != id || actor.at("owner_epoch").get<I>() <= 0 || actor.at("session_epoch").get<I>() <= 0)
                throw Error(403, "host required");
            for (const auto& member : arena->players) if (member.second.mode != "active") throw Error(409, "ownership transition active");
            arena->state = "finished"; return metadata(*arena);
        }
        if (!exact()) throw Error(exists ? 409 : 404, "stale or missing role");
        auto& player = found->second;
        if (operation == "freeze") {
            if (player.mode == "frozen" && player.transfer != transfer) throw Error(409, "freeze conflict");
            player.mode = "frozen"; player.transfer = transfer; return {{"role", player.role}};
        }
        if (operation == "unfreeze") { player.mode = "active"; return {{"accepted", true}}; }
        if (operation == "activate") {
            if (player.transfer != transfer) throw Error(409, "activation conflict");
            player.mode = "active";
            if (!temporary && lost_lobby_admission > 0) { --lost_lobby_admission; throw Error(503, "synthetic lost activation response"); }
            return {{"role", player.role}};
        }
        if (operation == "ready") {
            if (!request.contains("ready") || !request["ready"].is_boolean()) throw Error(400, "ready must be boolean");
            player.ready = request.at("ready"); return metadata(*arena);
        }
        if (operation == "start" || operation == "finish") {
            if (arena->spec.at("host_role_id") != id) throw Error(403, "host required");
            if (operation == "start") for (const auto& item : arena->players) if (!item.second.ready) throw Error(409, "not ready");
            arena->state = operation == "start" ? "playing" : "finished"; return metadata(*arena);
        }
        throw Error(404, "unknown scene operation");
    }
    void progress(const std::string& room, const std::string& id, I hp, I revision) {
        std::lock_guard<std::mutex> guard(mutex);
        auto& player = rooms.at(room).players.at(id);
        player.role["x"] = 11.; player.role["y"] = 13.;
        player.role["combat"] = combat(hp, revision, player.role.at("session_epoch"));
    }
    void forget_rooms() { std::lock_guard<std::mutex> guard(mutex); rooms.clear(); default_version = "script-v2"; }
    I creation_count() { std::lock_guard<std::mutex> guard(mutex); return creations; }
    Json creation_request() { std::lock_guard<std::mutex> guard(mutex); return last_create; }
    void lose_next_lobby_admission() { std::lock_guard<std::mutex> guard(mutex); lost_lobby_admission = 1; }
    void disconnect(const std::string& room, const std::string& id) { std::lock_guard<std::mutex> guard(mutex); rooms.at(room).players.at(id).connected = false; }
    void suspend_management(bool value) { std::lock_guard<std::mutex> guard(mutex); management_unavailable = value; }
};
}

void room_tests(const native::db::Config& database) {
    const auto suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()).substr(8);
    const std::string key = "synthetic-room-backend-test-internal-token-32plus";
    Worker worker(key);
    Config config; config.token = key; config.database = database; config.default_scene = "scene-1";
    config.scenes = {{"scene-1", worker.url}}; config.room_worker_url = worker.url;
    auto service = std::make_unique<Service>(config);
    auto host_account = service->execute("register", {{"name", "room-host-" + suffix}});
    auto guest_account = service->execute("register", {{"name", "room-guest-" + suffix}});
    auto host = service->execute("login", {{"token", host_account["token"]}})["role"];
    auto guest = service->execute("login", {{"token", guest_account["token"]}})["role"];
    auto session = [&](const Json& role) { return service->execute("session", {{"role_id", role["role_id"]}, {"session_epoch", role["session_epoch"]}})["role"]; };
    service->execute("claim", {{"role_id", host["role_id"]}, {"session_epoch", host["session_epoch"]}});
    host = session(host); check(host["coins"] == 100, "room fixture welcome credit missing");
    auto request = [&](const Json& role, const std::string& id = {}) {
        auto out = reference(role); out["profile"] = "skill_lab"; if (!id.empty()) out["room_id"] = id; return out;
    };
    auto create = request(host); create.update({{"capacity", 2}, {"request_id", "room-create-one"}});
    const auto metadata = service->execute("room_create", create);
    const auto room = metadata.at("room_id").get<std::string>();
    check(metadata["script_version"] == "script-v1" && metadata["script_sha256"] == std::string(64, 'a'), "room creation did not pin source");
    check(service->execute("room_create", create)["room_id"] == room && worker.creation_count() == 1, "room create retry allocated another world");
    auto changed = create; changed["capacity"] = 3; refuses(409, [&] { service->execute("room_create", changed); });
    changed = create; changed["host_role_id"] = guest["role_id"]; refuses(400, [&] { service->execute("room_create", changed); });
    changed = create; changed["session_epoch"] = 0; refuses(400, [&] { service->execute("room_create", changed); });
    auto second_request = create; second_request["request_id"] = "room-create-two";
    const auto second_room = service->execute("room_create", second_request).at("room_id").get<std::string>();
    auto list = service->execute("room_list", request(host));
    check(list["rooms"].size() == 2, "room directory list lost allocations");
    auto join = request(host, room); join["request_id"] = "join-one";
    const auto before_join = host;
    host = service->execute("room_join", join)["role"];
    check(host["role_id"] == host_account["role_id"] && host["coins"] == 100 && host["session_epoch"] == before_join["session_epoch"] && host["owner_epoch"].get<I>() > before_join["owner_epoch"].get<I>(), "room join changed identity/assets/session");
    check(service->execute("room_join", join)["role"]["owner_epoch"] == host["owner_epoch"], "lost join response cannot replay original owner");
    auto wrong_target = join; wrong_target["room_id"] = second_room;
    refuses(409, [&] { service->execute("room_join", wrong_target); });
    auto not_joined = request(guest, room); not_joined["ready"] = true;
    refuses(409, [&] { service->execute("room_ready", not_joined); });
    auto guest_join = request(guest, room); guest_join["request_id"] = "guest-join";
    guest = service->execute("room_join", guest_join)["role"];
    auto ready = request(host, room); ready["ready"] = "yes";
    refuses(400, [&] { service->execute("room_ready", ready); });
    refuses(403, [&] { service->execute("room_start", request(guest, room)); });
    refuses(403, [&] { service->execute("room_finish", request(guest, room)); });
    refuses(409, [&] { service->execute("room_start", request(host, room)); });
    ready["ready"] = true; service->execute("room_ready", ready);
    ready = request(guest, room); ready["ready"] = true; service->execute("room_ready", ready);
    check(service->execute("room_start", request(host, room))["state"] == "playing", "ready room did not start");
    auto leave = request(guest); leave["request_id"] = "guest-leave";
    guest = service->execute("room_leave", leave)["role"];
    check(guest["scene_id"] == "scene-1" && guest["role_id"] == guest_account["role_id"], "room leave lost guest identity");
    check(service->execute("room_leave", leave)["role"]["owner_epoch"] == guest["owner_epoch"], "lost leave response cannot replay original owner");
    auto stale = leave; stale["request_id"] = "new-stale-owner";
    refuses(409, [&] { service->execute("room_leave", stale); });
    service.reset(); service = std::make_unique<Service>(config);
    auto route = service->execute("session", {{"role_id", host["role_id"]}, {"session_epoch", host["session_epoch"]}});
    check(route["scene_url"] == worker.url + "/rooms/" + room && route["role"]["coins"] == 100, "Backend restart did not restore room route/assets");
    const auto before_worker_restart = host;
    worker.forget_rooms(); host = session(host);
    check(host["session_epoch"].get<I>() == before_worker_restart["session_epoch"].get<I>() + 1 &&
          host["owner_epoch"] == before_worker_restart["owner_epoch"],
          "Worker world restart did not fence the old session without changing ownership");
    auto stale_world_checkpoint = before_worker_restart;
    stale_world_checkpoint["x"] = 99.; stale_world_checkpoint["y"] = 98.;
    stale_world_checkpoint["combat"] = combat(2, 1000, before_worker_restart.at("session_epoch"));
    refuses(409, [&] { service->execute("checkpoint", {{"role", stale_world_checkpoint}}); });
    // 新世界会话可以保存，重复查询不应再次旋转；迟到的旧世界高版本快照也不能覆盖。
    service->execute("checkpoint", {{"role", host}});
    const auto rebound = session(host);
    check(rebound["session_epoch"] == host["session_epoch"] && rebound["x"] == host["x"] &&
          rebound["y"] == host["y"] && rebound["combat"] == host["combat"],
          "old-world checkpoint changed restored state or unchanged world rotated session again");
    refuses(409, [&] { session(before_worker_restart); });
    {
        std::lock_guard<std::mutex> guard(worker.mutex);
        check(worker.rooms.at(room).players.at(host.at("role_id")).role["session_epoch"] == host["session_epoch"],
              "Worker restart returned a new session without actually binding the new world actor");
    }
    const auto restored_request = worker.creation_request();
    check(restored_request["script_version"] == "script-v1" && restored_request["script_sha256"] == std::string(64, 'a'), "Worker restart chose mutable default script instead of saved pin");
    check(host["role_id"] == host_account["role_id"] && host["coins"] == 100, "Worker restore changed permanent role/assets");
    {
        std::lock_guard<std::mutex> guard(worker.mutex); worker.rooms.at(room).spec["script_sha256"] = std::string(64, 'b');
    }
    refuses(503, [&] { service->execute("room_status", request(host, room)); });
    {
        std::lock_guard<std::mutex> guard(worker.mutex); worker.rooms.at(room).spec["script_sha256"] = std::string(64, 'a');
    }
    worker.progress(room, host.at("role_id"), 71, 20);
    check(service->execute("room_finish", request(host, room))["state"] == "finished", "host could not finish restored room");
    worker.lose_next_lobby_admission();
    refuses(503, [&] { session(host); });
    service.reset(); service = std::make_unique<Service>(config);
    host = session(host);
    check(host["scene_id"] == "scene-1" && host["coins"] == 100 && host["combat"]["hp"] == 71, "finished room recovery lost committed combat/assets");
    {
        std::lock_guard<std::mutex> guard(worker.mutex);
        check(worker.lobby.players.at(host.at("role_id")).role["owner_epoch"] == host["owner_epoch"], "finished-room session returned a lobby route without entering");
    }
    // 已结束且Worker回收/重启遗忘的房间没有源实体，但后台session仍应把旧角色实际送回大厅。
    auto rejoin = request(host, second_room); rejoin["request_id"] = "join-retired-room";
    host = service->execute("room_join", rejoin)["role"];
    service->execute("room_finish", request(host, second_room)); worker.forget_rooms();
    host = session(host);
    check(host["scene_id"] == "scene-1" && host["coins"] == 100 && host["combat"]["hp"] == 71, "retired room stranded or reset the character");
    {
        std::lock_guard<std::mutex> guard(worker.mutex);
        check(worker.lobby.players.count(host.at("role_id")) && worker.lobby.players.at(host.at("role_id")).role["owner_epoch"] == host["owner_epoch"], "retired-room session did not create the actual lobby entity");
    }
    host = service->execute("login", {{"token", host_account["token"]}})["role"];
    check(host["role_id"] == host_account["role_id"] && host["coins"] == 100 && host["combat"]["hp"] == 71, "permanent token or stored room character failed after restart");
    check(!service->execute("claim", {{"role_id", host["role_id"]}, {"session_epoch", host["session_epoch"]}})["granted"].get<bool>(), "room recovery duplicated persistent reward");
    // 管理排空必须覆盖离线角色的未存档实时HP，且不能更换永久凭据或授予额外金币。
    auto create_drain = request(host); create_drain.update({{"capacity", 2}, {"request_id", "drain-room"}});
    const auto drain_room = service->execute("room_create", create_drain).at("room_id").get<std::string>();
    auto enter_drain = request(guest, drain_room); enter_drain["request_id"] = "guest-before-drain";
    guest = service->execute("room_join", enter_drain)["role"];
    worker.progress(drain_room, guest.at("role_id"), 55, 30); worker.disconnect(drain_room, guest.at("role_id"));
    refuses(400, [&] { service->execute("room_drain", {{"role_id", guest["role_id"]}}); });
    worker.suspend_management(true);
    const auto blocked_drain = service->execute("room_drain", Json::object());
    check(blocked_drain["complete"] == false && !blocked_drain["failed"].empty() && blocked_drain["moved"].empty() &&
          blocked_drain["remaining_room_roles"] == 1, "unavailable Worker forced an unsafe room evacuation");
    worker.suspend_management(false);
    const auto drained = service->execute("room_drain", Json::object());
    check(drained["complete"] == true && drained["failed"].empty() && drained["moved"].size() == 1 &&
          drained["remaining_room_roles"] == 0 && drained["unfinished_migrations"] == 0, "internal room drain failed to evacuate offline participant");
    const auto repeated_drain = service->execute("room_drain", {{"room_id", drain_room}});
    check(repeated_drain["complete"] == true && repeated_drain["moved"].empty() && repeated_drain["selected_remaining_room_roles"] == 0,
          "room drain replay moved a role again");
    guest = service->execute("login", {{"token", guest_account["token"]}})["role"];
    check(guest["scene_id"] == "scene-1" && guest["role_id"] == guest_account["role_id"] && guest["coins"] == 0 && guest["combat"]["hp"] == 55,
          "offline drain lost live combat, changed token/identity or credited coins");
}
}
