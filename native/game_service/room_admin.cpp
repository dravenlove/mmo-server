#include "game_service/service.hpp"
#include <chrono>
#include <set>
#include <string>

namespace native::game {
namespace {
std::string admin_text(const Json& object, const char* key) {
    if (!object.contains(key) || !object[key].is_string()) throw Error(400, std::string("invalid ") + key);
    return object[key].get<std::string>();
}
}

// 仅由通过内部凭据检查的管理HTTP入口调用，不暴露给玩家Gateway。
// 必须持有Service状态锁；每次有界推进，失败保留原角色及恢复意图，重复调用继续排空。
Json Service::drain_rooms(const Json& request) {
    if (config_.room_worker_url.empty()) throw Error(409, "room worker is not configured");
    if (!request.is_object() || request.size() > (request.contains("room_id") ? 1u : 0u))
        throw Error(400, "room drain only accepts optional room_id");
    std::string selected;
    if (request.contains("room_id")) {
        selected = admin_text(request, "room_id");
        room_record(selected); // 复用权威目录的ID/存在性校验，绝不创建替代空房间。
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::size_t work = 0;
    bool exhausted = false;
    Json moved = Json::array(), failed = Json::array();
    auto budget = [&] {
        if (work >= 32 || std::chrono::steady_clock::now() >= deadline) { exhausted = true; return false; }
        return true;
    };
    auto report = [&](const std::string& room, const std::string& id, const char* stage, int code) {
        Json value{{"room_id", room}, {"stage", stage}, {"status", code}};
        if (!id.empty()) value["role_id"] = id;
        failed.push_back(std::move(value));
    };
    std::string pending_sql = "SELECT role_id,source_json,target_json FROM migrations WHERE status NOT IN ('DONE','ABORTED')";
    db::Params pending_params;
    if (!selected.empty()) {
        pending_sql += " AND (target_scene_id=? OR source_json LIKE ? OR role_id IN (SELECT role_id FROM roles WHERE scene_id=?))";
        pending_params = {selected, "%" + selected + "%", selected};
    } else {
        pending_sql += " AND (target_scene_id IN (SELECT room_id FROM rooms_directory) OR source_json LIKE '%room-%')";
    }
    const auto pending = db_.query(pending_sql + " ORDER BY role_id LIMIT 64", pending_params);
    for (const auto& row : pending) {
        if (!budget()) break;
        const auto id = db::text(row, 0);
        std::string from, to;
        try {
            from = Json::parse(db::text(row, 1)).value("scene_id", std::string());
            to = Json::parse(db::text(row, 2)).value("scene_id", std::string());
        } catch (...) { report(selected, id, "migration_metadata", 503); ++work; continue; }
        if (!room_scene(from) && !room_scene(to)) continue;
        ++work;
        try { recover_role(id); }
        catch (const db::Error&) { throw; }
        catch (const Error& error) { report(room_scene(from) ? from : to, id, "migration_recovery", error.status); }
        catch (...) { report(room_scene(from) ? from : to, id, "migration_recovery", 503); }
    }
    std::string rooms_sql = "SELECT room_id FROM rooms_directory WHERE ";
    db::Params room_params;
    if (!selected.empty()) { rooms_sql += "room_id=?"; room_params = {selected}; }
    else rooms_sql += "status<>'FINISHED' OR EXISTS (SELECT 1 FROM roles WHERE roles.scene_id=rooms_directory.room_id)";
    const auto rooms = db_.query(rooms_sql + " ORDER BY room_id LIMIT 64", room_params);
    for (const auto& row : rooms) {
        if (!budget()) break;
        const auto room = db::text(row, 0);
        try {
            auto record = room_record(room);
            const auto metadata = ensure_room(room);
            if (metadata.value("state", std::string()) != "finished") {
                const auto host = role(admin_text(record, "host_role_id"));
                Json control{{"role_id", host.at("role_id")}, {"owner_epoch", host.at("owner_epoch")},
                             {"session_epoch", host.at("session_epoch")}, {"control_authorized", true},
                             {"_room_world_epoch", metadata.at("world_epoch")}};
                const auto result = http_post(config_.scenes.at(room), "/v1/finish", control, config_.token, 2000);
                if (!result.is_object() || result.value("room_id", std::string()) != room ||
                    result.value("state", std::string()) != "finished") throw Error(503, "invalid room finish acknowledgement");
            }
            db_.exec("UPDATE rooms_directory SET status='FINISHED' WHERE room_id=?", {room});
        } catch (const db::Error&) { throw; }
        catch (const Error& error) { ++work; report(room, {}, "finish", error.status); continue; }
        catch (...) { ++work; report(room, {}, "finish", 503); continue; }

        for (const auto& player : db_.query("SELECT role_id FROM roles WHERE scene_id=? ORDER BY role_id", {room})) {
            if (!budget()) break;
            ++work;
            const auto id = db::text(player, 0);
            try {
                recover_role(id);
                auto current = role(id);
                if (current.at("scene_id") != room) continue;
                // 维护排空也覆盖断线角色。新会话既使旧网关失效，也允许Scene重新绑定
                // disconnected实体，同时保留其尚未存入SQL的权威HP/MP，不能用DB旧快照覆盖。
                if (db_.exec("UPDATE roles SET session_epoch=session_epoch+1 WHERE role_id=? AND scene_id=? AND owner_epoch=? AND session_epoch=?", {
                        id, room, current.at("owner_epoch").get<std::int64_t>(), current.at("session_epoch").get<std::int64_t>()}) != 1)
                    throw Error(409, "room role changed during drain");
                current = role(id);
                recover_finished_room(current);
                if (room_scene(current.at("scene_id").get<std::string>())) throw Error(503, "room evacuation did not complete");
                moved.push_back({{"role_id", id}, {"from_scene", room}, {"to_scene", current.at("scene_id")},
                                 {"owner_epoch", current.at("owner_epoch")}, {"session_epoch", current.at("session_epoch")}});
            } catch (const db::Error&) { throw; }
            catch (const Error& error) { report(room, id, "evacuation", error.status); }
            catch (...) { report(room, id, "evacuation", 503); }
        }
    }
    const auto remaining = db::integer(db_.query("SELECT COUNT(*) FROM roles WHERE scene_id IN (SELECT room_id FROM rooms_directory)").at(0), 0);
    const auto unfinished = db::integer(db_.query("SELECT COUNT(*) FROM migrations WHERE status NOT IN ('DONE','ABORTED')").at(0), 0);
    auto selected_remaining = remaining, selected_pending = unfinished;
    if (!selected.empty()) {
        selected_remaining = db::integer(db_.query("SELECT COUNT(*) FROM roles WHERE scene_id=?", {selected}).at(0), 0);
        selected_pending = db::integer(db_.query("SELECT COUNT(*) FROM migrations WHERE status NOT IN ('DONE','ABORTED') AND (target_scene_id=? OR source_json LIKE ? OR role_id IN (SELECT role_id FROM roles WHERE scene_id=?))",
                                               {selected, "%" + selected + "%", selected}).at(0), 0);
    }
    return {{"moved", moved}, {"failed", failed}, {"remaining_room_roles", remaining}, {"unfinished_migrations", unfinished},
            {"selected_remaining_room_roles", selected_remaining}, {"selected_unfinished_migrations", selected_pending},
            {"budget_exhausted", exhausted}, {"complete", selected_remaining == 0 && selected_pending == 0 && failed.empty() && !exhausted}};
}
}  // namespace native::game
