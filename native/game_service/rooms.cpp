#include "game_service/service.hpp"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <set>
#include <sstream>

namespace native::game {
namespace {
using I = std::int64_t;
std::string text(const Json& j, const char* key, bool optional = false) {
    if (!j.contains(key) && optional) return {};
    if (!j.contains(key) || !j[key].is_string()) throw Error(400, std::string("invalid ") + key);
    return j[key].get<std::string>();
}
I integer(const Json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_number_integer() ||
        (j[key].is_number_unsigned() && j[key].get<std::uint64_t>() > INT64_MAX)) throw Error(400, std::string("invalid ") + key);
    return j[key].get<I>();
}
bool id_valid(const std::string& id) {
    return id.size() == 37 && id.rfind("room-", 0) == 0 &&
           id.find_first_not_of("0123456789abcdef", 5) == std::string::npos;
}
std::string random_room() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, 16) != 1) throw Error(503, "secure random unavailable");
    std::ostringstream out; out << "room-" << std::hex << std::setfill('0');
    for (auto b : bytes) out << std::setw(2) << static_cast<unsigned>(b);
    return out.str();
}
std::string operation_key(const std::string& operation, const Json& req) {
    const auto request = text(req, "request_id");
    if (request.empty() || request.size() > 128 ||
        std::any_of(request.begin(), request.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw Error(400, "request_id is required (1..128 bytes)");
    const auto value = text(req, "role_id") + ":" + request;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash);
    std::ostringstream out; out << operation << '-' << std::hex << std::setfill('0');
    for (auto b : hash) out << std::setw(2) << static_cast<unsigned>(b);
    return out.str();
}
Json reference(const Json& r) {
    return {{"role_id", r.at("role_id")}, {"owner_epoch", r.at("owner_epoch")}, {"session_epoch", r.at("session_epoch")}};
}
std::string profile(const Json& req) {
    const auto p = text(req, "profile");
    if (p != "movement" && p != "abilities" && p != "skill_lab" && p != "combat")
        throw Error(400, "room profile is not supported");
    return p;
}
}

bool Service::room_scene(const std::string& id) const {
    return !config_.room_worker_url.empty() && id_valid(id);
}

void Service::room_schema() {
    const std::string sql = db_.is_mysql()
        ? "CREATE TABLE IF NOT EXISTS rooms_directory (room_id VARBINARY(64) PRIMARY KEY,host_role_id VARBINARY(128) NOT NULL,capacity BIGINT NOT NULL,profile VARBINARY(32) NOT NULL,script_version VARBINARY(64) NOT NULL,script_sha256 VARBINARY(64) NOT NULL,create_key VARBINARY(128) NOT NULL UNIQUE,status VARBINARY(16) NOT NULL,FOREIGN KEY(host_role_id) REFERENCES roles(role_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
        : "CREATE TABLE IF NOT EXISTS rooms_directory (room_id TEXT PRIMARY KEY,host_role_id TEXT NOT NULL REFERENCES roles(role_id),capacity INTEGER NOT NULL,profile TEXT NOT NULL,script_version TEXT NOT NULL,script_sha256 TEXT NOT NULL,create_key TEXT NOT NULL UNIQUE,status TEXT NOT NULL)";
    db_.exec(sql);
    db_.exec(db_.is_mysql()
        ? "CREATE TABLE IF NOT EXISTS room_returns (role_id VARBINARY(128) PRIMARY KEY,scene_id VARBINARY(64) NOT NULL,bound_room_id VARBINARY(64) NOT NULL DEFAULT '',world_epoch VARBINARY(128) NOT NULL DEFAULT '',FOREIGN KEY(role_id) REFERENCES roles(role_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
        : "CREATE TABLE IF NOT EXISTS room_returns (role_id TEXT PRIMARY KEY REFERENCES roles(role_id),scene_id TEXT NOT NULL,bound_room_id TEXT NOT NULL DEFAULT '',world_epoch TEXT NOT NULL DEFAULT '')");
    if (db_.is_mysql() && db::integer(db_.query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN ('rooms_directory','room_returns') AND ENGINE='InnoDB'").at(0), 0) != 2)
        throw Error(503, "room tables must use InnoDB");
}

void Service::restore_room_routes() {
    for (const auto& row : db_.query("SELECT room_id FROM rooms_directory")) {
        const auto id = db::text(row, 0);
        if (!id_valid(id) || config_.scenes.count(id)) throw Error(503, "invalid or conflicting persisted room route");
        config_.scenes[id] = config_.room_worker_url + "/rooms/" + id;
    }
}

Json Service::room_record(const std::string& id) {
    if (!id_valid(id)) throw Error(400, "invalid room_id");
    const auto rows = db_.query("SELECT room_id,host_role_id,capacity,profile,script_version,script_sha256,status FROM rooms_directory WHERE room_id=?", {id});
    if (rows.empty()) throw Error(404, "room not found");
    const auto& r = rows[0];
    return {{"room_id", db::text(r, 0)}, {"host_role_id", db::text(r, 1)}, {"capacity", db::integer(r, 2)},
            {"profile", db::text(r, 3)}, {"script_version", db::text(r, 4)}, {"script_sha256", db::text(r, 5)}, {"directory_status", db::text(r, 6)}};
}

Json Service::ensure_room(const std::string& id) {
    auto record = room_record(id);
    Json result;
    try { result = http_post(config_.room_worker_url, "/v1/rooms/status", {{"room_id", id}}, config_.token, 2000); }
    catch (const Error& error) {
        if (error.status != 404) throw;
        if (record["directory_status"] == "FINISHED") {
            record.erase("directory_status"); record["state"] = "finished"; record["members"] = Json::array(); return record;
        }
        auto spec = record; spec.erase("directory_status");
        if (spec["script_version"] == "") spec.erase("script_version");
        if (spec["script_sha256"] == "") spec.erase("script_sha256");
        result = http_post(config_.room_worker_url, "/v1/rooms/create", spec, config_.token, 2000);
    }
    if (!result.is_object() || text(result, "room_id") != id || text(result, "script_version").empty())
        throw Error(503, "invalid room worker metadata");
    if (record["script_version"] != "" && result["script_version"] != record["script_version"])
        throw Error(503, "room script version changed unexpectedly");
    const auto digest = text(result, "script_sha256");
    if (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != std::string::npos ||
        (record["script_sha256"] != "" && result["script_sha256"] != record["script_sha256"]))
        throw Error(503, "room script checksum changed unexpectedly");
    const auto state = text(result, "state");
    if (state != "lobby" && state != "playing" && state != "finished") throw Error(503, "invalid room state");
    if (record["directory_status"] == "FINISHED" && state != "finished")
        throw Error(503, "finished room cannot be reopened");
    db_.exec("UPDATE rooms_directory SET script_version=?,script_sha256=?,status=? WHERE room_id=?", {
        text(result, "script_version"), digest, std::string(state == "finished" ? "FINISHED" : "OPEN"), id});
    return result;
}

void Service::bind_room_world(Json& r, const Json& metadata, bool rotate_session) {
    const auto id = text(r, "role_id"), room = text(r, "scene_id"), epoch = text(metadata, "world_epoch");
    if (epoch.empty() || epoch.size() > 128) throw Error(503, "invalid room world epoch");
    auto rows = db_.query("SELECT bound_room_id,world_epoch FROM room_returns WHERE role_id=?", {id});
    db::Tx tx(db_);
    if (rows.empty()) db_.exec("INSERT INTO room_returns(role_id,scene_id,bound_room_id,world_epoch) VALUES(?,?,?,?)", {id, config_.default_scene, room, epoch});
    else {
        // 新世界以新会话围栏拒绝迟到的旧世界施法/移动/最终检查点，不只依赖UI epoch。
        if (rotate_session && db::text(rows[0], 0) == room && !db::text(rows[0], 1).empty() && db::text(rows[0], 1) != epoch) {
            if (db_.exec("UPDATE roles SET session_epoch=session_epoch+1 WHERE role_id=? AND owner_epoch=? AND session_epoch=?", {id, integer(r, "owner_epoch"), integer(r, "session_epoch")}) != 1) throw Error(409, "room session changed");
            r["session_epoch"] = integer(r, "session_epoch") + 1;
        }
        db_.exec("UPDATE room_returns SET bound_room_id=?,world_epoch=? WHERE role_id=?", {room, epoch, id});
    }
    tx.commit();
}

void Service::recover_finished_room(Json& r, const std::string& recovery_key) {
    const auto id = text(r, "scene_id");
    auto meta = ensure_room(id);
    if (meta.value("state", std::string()) != "finished") return;
    auto destination = config_.default_scene;
    const auto previous = db_.query("SELECT scene_id FROM room_returns WHERE role_id=?", {text(r, "role_id")});
    if (!previous.empty() && config_.scenes.count(db::text(previous[0], 0)) && !room_scene(db::text(previous[0], 0))) destination = db::text(previous[0], 0);
    const auto key = recovery_key.empty() ? "room-ended-" + id + "-" + std::to_string(integer(r, "session_epoch")) : recovery_key;
    // 活实体使用原迁移事务，先持久化最终快照，再清理源，禁止先remove后存档。
    if (meta.contains("members") && meta["members"].is_array()) {
        for (const auto& member : meta["members"]) {
            const auto& value = member.contains("role") ? member["role"] : member;
            if (value.value("role_id", std::string()) != text(r, "role_id")) continue;
            auto actual = scene(id, "enter", {{"role", r}});
            const auto& live = actual.at("role");
            if (live.at("role_id") != r["role_id"] || live.at("owner_epoch") != r["owner_epoch"] || live.at("session_epoch") != r["session_epoch"])
                throw Error(503, "invalid finished room recovery");
            auto input = reference(r); input["request_id"] = key; input["target_scene_id"] = destination;
            r = transfer(input).at("role"); return;
        }
    }
    // 源已明确回收：原检查点作为恢复基线，COMMITTED意图和归属在同一事务发布。
    auto target = r;
    db::Tx tx(db_);
    db_.exec("UPDATE roles SET epoch_counter=epoch_counter+1 WHERE role_id=?", {text(r, "role_id")});
    const auto epoch = db::integer(db_.query("SELECT epoch_counter FROM roles WHERE role_id=?", {text(r, "role_id")}).at(0), 0);
    if (epoch <= integer(r, "owner_epoch")) throw Error(503, "invalid room recovery epoch");
    target["scene_id"] = destination; target["owner_epoch"] = epoch; target["x"] = 0.; target["y"] = 0.;
    const auto transfer_id = random_room();
    db_.exec("INSERT INTO migrations(role_id,request_id,transfer_id,target_scene_id,status,source_json,target_json) VALUES(?,?,?,?,'COMMITTED',?,?)", {
        text(r, "role_id"), key, transfer_id, destination, r.dump(), target.dump()});
    if (db_.exec("UPDATE roles SET scene_id=?,owner_epoch=?,x=0,y=0 WHERE role_id=? AND owner_epoch=? AND session_epoch=?", {
        destination, epoch, text(r, "role_id"), integer(r, "owner_epoch"), integer(r, "session_epoch")}) != 1) throw Error(409, "room recovery ownership changed");
    save_combat(target); tx.commit();
    Migration migration{text(r, "role_id"), key, transfer_id, destination, "COMMITTED", r, target};
    finish_committed(migration); r = role(text(r, "role_id"));
}

Json Service::room_request(const std::string& operation, const Json& req) {
    if (config_.room_worker_url.empty()) throw Error(409, "room worker is not configured");
    const std::set<std::string> allowed{"role_id", "owner_epoch", "session_epoch", "room_id", "request_id", "capacity", "profile", "ready"};
    if (!req.is_object()) throw Error(400, "invalid room request");
    for (auto i = req.begin(); i != req.end(); ++i) if (!allowed.count(i.key())) throw Error(400, "unknown room request field");
    auto current = validate_session(req);
    recover_role(text(current, "role_id")); current = role(text(current, "role_id"));
    if (operation == "room_join" || operation == "room_leave") {
        const auto key = operation_key(operation == "room_join" ? "room-join" : "room-leave", req);
        const auto previous = migration(text(current, "role_id"), key);
        if (previous) {
            if (operation == "room_join" && previous->target_scene_id != text(req, "room_id")) throw Error(409, "request_id changed room target");
            if (operation == "room_leave" && req.contains("room_id") && previous->source.at("scene_id") != req["room_id"]) throw Error(409, "request_id changed source room");
            auto input = reference(current); input["request_id"] = key; input["target_scene_id"] = previous->target_scene_id;
            return transfer(input);
        }
    }
    if (integer(req, "owner_epoch") != integer(current, "owner_epoch")) throw Error(409, "room owner changed");
    const auto selected_profile = profile(req);
    if (operation == "room_create") {
        const auto key = operation_key("room-create", req);
        const auto capacity = req.contains("capacity") ? integer(req, "capacity") : I{4};
        if (capacity < 1 || capacity > 16) throw Error(400, "capacity must be 1..16");
        const auto existing = db_.query("SELECT room_id FROM rooms_directory WHERE create_key=?", {key});
        std::string id;
        if (!existing.empty()) {
            id = db::text(existing[0], 0); const auto record = room_record(id);
            if (record["profile"] != selected_profile || record["capacity"] != capacity || record["host_role_id"] != current["role_id"])
                throw Error(409, "request_id changed room creation parameters");
        } else {
            if (db::integer(db_.query("SELECT COUNT(*) FROM rooms_directory WHERE status<>'FINISHED'").at(0), 0) >= 16 ||
                db::integer(db_.query("SELECT COUNT(*) FROM rooms_directory WHERE host_role_id=? AND status<>'FINISHED'", {text(current, "role_id")}).at(0), 0) >= 4)
                throw Error(409, "room allocation capacity reached");
            id = random_room();
            db_.exec("INSERT INTO rooms_directory(room_id,host_role_id,capacity,profile,script_version,script_sha256,create_key,status) VALUES(?,?,?,?,'','',?,'CREATING')", {
                id, text(current, "role_id"), capacity, selected_profile, key});
            config_.scenes[id] = config_.room_worker_url + "/rooms/" + id;
        }
        return ensure_room(id);
    }
    if (operation == "room_list") {
        Json rooms = Json::array();
        for (const auto& row : db_.query("SELECT room_id FROM rooms_directory WHERE profile=? AND status<>'FINISHED' ORDER BY room_id", {selected_profile})) {
            auto record = room_record(db::text(row, 0)); record.erase("directory_status"); rooms.push_back(std::move(record));
        }
        return {{"rooms", rooms}};
    }
    if (operation == "room_leave") {
        const auto key = operation_key("room-leave", req);
        if (req.contains("room_id") && text(req, "room_id") != text(current, "scene_id")) throw Error(409, "room changed before leave");
        std::string target;
        const auto past = migration(text(current, "role_id"), key);
        if (past) target = past->target_scene_id;
        else {
            if (!room_scene(text(current, "scene_id"))) return route(current);
            const auto metadata = ensure_room(text(current, "scene_id"));
            if (metadata.value("state", std::string()) == "finished" && metadata.value("members", Json::array()).empty()) {
                recover_finished_room(current, key); return route(current);
            }
            const auto rows = db_.query("SELECT scene_id FROM room_returns WHERE role_id=?", {text(current, "role_id")});
            target = rows.empty() ? config_.default_scene : db::text(rows[0], 0);
            if (!config_.scenes.count(target) || room_scene(target)) throw Error(503, "invalid return scene");
        }
        auto input = reference(current); input["request_id"] = key; input["target_scene_id"] = target;
        return transfer(input);
    }
    const auto id = req.contains("room_id") ? text(req, "room_id") : text(current, "scene_id");
    const auto record = room_record(id);
    if (record["profile"] != selected_profile) throw Error(409, "room profile differs from this gateway");
    auto metadata = ensure_room(id);
    if (operation == "room_status") {
        if (metadata.value("state", std::string()) == "finished") persist_finished_members(id);
        return metadata;
    }
    if (operation == "room_join") {
        if (metadata.value("state", std::string()) == "finished") throw Error(409, "room is finished");
        const auto key = operation_key("room-join", req);
        if (!room_scene(text(current, "scene_id"))) {
            const auto rows = db_.query("SELECT scene_id FROM room_returns WHERE role_id=?", {text(current, "role_id")});
            if (rows.empty()) db_.exec("INSERT INTO room_returns(role_id,scene_id) VALUES(?,?)", {text(current, "role_id"), text(current, "scene_id")});
            else db_.exec("UPDATE room_returns SET scene_id=? WHERE role_id=?", {text(current, "scene_id"), text(current, "role_id")});
        }
        auto input = reference(current); input["request_id"] = key; input["target_scene_id"] = id;
        auto result = transfer(input);
        auto joined = result.at("role"); bind_room_world(joined, ensure_room(id), false);
        return route(joined);
    }
    auto body = reference(current);
    if (operation == "room_finish") {
        if (record["host_role_id"] != current["role_id"]) throw Error(403, "only the room host can finish");
        if (metadata.value("state", std::string()) == "finished") return metadata;
        body["_room_world_epoch"] = metadata.at("world_epoch");
        body["control_authorized"] = true;
        auto result = http_post(config_.scenes.at(id), "/v1/finish", body, config_.token, 2000);
        db_.exec("UPDATE rooms_directory SET status='FINISHED' WHERE room_id=?", {id});
        persist_finished_members(id); return result;
    }
    if (text(current, "scene_id") != id) throw Error(409, "join the room first");
    body["_room_world_epoch"] = metadata.at("world_epoch");
    if (operation == "room_ready") {
        if (!req.contains("ready") || !req["ready"].is_boolean()) throw Error(400, "ready must be boolean");
        body["ready"] = req["ready"];
    } else if (operation != "room_start") throw Error(404, "unknown room operation");
    return http_post(config_.scenes.at(id), operation == "room_ready" ? "/v1/ready" : "/v1/start", body, config_.token, 2000);
}
} // namespace native::game
