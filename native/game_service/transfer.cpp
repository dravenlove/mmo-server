#include "game_service/service.hpp"
#include <openssl/rand.h>
#include <iomanip>
#include <cmath>
#include <set>
#include <sstream>

namespace native::game {
namespace {
using I = std::int64_t;
std::string str(const Json& j, const char* key) { return j.value(key, std::string{}); }
I num(const Json& j, const char* key) {
    if (!j.contains(key)) return 0;
    if (!j[key].is_number_integer() || (j[key].is_number_unsigned() && j[key].get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX))) throw Error(400, "无效迁移整数字段");
    return j[key].get<I>();
}
Json reference(const Json& r, const std::string& id) { return {{"role_id", str(r, "role_id")}, {"owner_epoch", num(r, "owner_epoch")}, {"session_epoch", num(r, "session_epoch")}, {"transfer_id", id}}; }
bool same_ref(const Json& a, const Json& b) { return str(a, "role_id") == str(b, "role_id") && num(a, "owner_epoch") == num(b, "owner_epoch") && num(a, "session_epoch") == num(b, "session_epoch"); }
bool valid_snapshot(const Json& a, const Json& b) {
    try {
        if (!a.is_object() || !same_ref(a, b) || str(a, "scene_id") != str(b, "scene_id")) return false;
        if (!a.contains("x") || !a.contains("y") || !a["x"].is_number() || !a["y"].is_number()) return false;
        const auto x = a["x"].get<double>(), y = a["y"].get<double>();
        if (!std::isfinite(x) || !std::isfinite(y) || x < -100 || x > 100 || y < -100 || y > 100) return false;
        if (a.contains("combat")) combat_revision(a["combat"]);
        return true;
    } catch (...) { return false; }
}
void copy(Json& target, const Json& source) {
    target["x"] = source["x"]; target["y"] = source["y"];
    if (source.contains("combat") && !source["combat"].is_null()) target["combat"] = source["combat"];
}
std::string random_id() {
    unsigned char data[16];
    if (RAND_bytes(data, 16) != 1) throw std::runtime_error("secure random unavailable");
    std::ostringstream out; out << std::hex << std::setfill('0');
    for (auto b : data) out << std::setw(2) << static_cast<unsigned>(b);
    return out.str();
}
}
std::optional<Service::Migration> Service::migration(const std::string& role_id, const std::optional<std::string>& request_id) {
    auto rows = request_id ? db_.query("SELECT role_id,request_id,transfer_id,target_scene_id,status,source_json,target_json FROM migrations WHERE role_id=? AND request_id=?", {role_id, *request_id}) : db_.query("SELECT role_id,request_id,transfer_id,target_scene_id,status,source_json,target_json FROM migrations WHERE role_id=? AND status NOT IN ('DONE','ABORTED')", {role_id});
    if (rows.empty()) return {};
    auto& r = rows[0];
    return Migration{db::text(r, 0), db::text(r, 1), db::text(r, 2), db::text(r, 3), db::text(r, 4), Json::parse(db::text(r, 5)), Json::parse(db::text(r, 6))};
}
void Service::set_status(Migration& m, const std::string& status) {
    db_.exec("UPDATE migrations SET status=? WHERE role_id=? AND request_id=?", {status, m.role_id, m.request_id}); m.status = status;
}
Service::Migration Service::begin_migration(const Json& r, const Json& req) {
    const auto id = str(r, "role_id");
    db::Tx tx(db_);
    db_.exec("UPDATE roles SET epoch_counter=epoch_counter+1 WHERE role_id=?", {id});
    auto epoch = db::integer(db_.query("SELECT epoch_counter FROM roles WHERE role_id=?", {id}).at(0), 0);
    if (epoch <= num(r, "owner_epoch")) throw Error(503, "owner epoch counter exhausted or invalid");
    Json target = r; target["scene_id"] = str(req, "target_scene_id"); target["owner_epoch"] = epoch;
    Migration m{id, str(req, "request_id"), random_id(), str(req, "target_scene_id"), "PREPARING", r, target};
    db_.exec("INSERT INTO migrations(role_id,request_id,transfer_id,target_scene_id,status,source_json,target_json) VALUES(?,?,?,?,?,?,?)", {m.role_id, m.request_id, m.transfer_id, m.target_scene_id, m.status, m.source.dump(), m.target.dump()});
    tx.commit(); return m;
}
void Service::commit_migration(Migration& m) {
    db::Tx tx(db_);
    auto count = db_.exec("UPDATE roles SET scene_id=?,owner_epoch=?,x=?,y=? WHERE role_id=? AND scene_id=? AND owner_epoch=? AND session_epoch=?", {str(m.target, "scene_id"), num(m.target, "owner_epoch"), m.target["x"].get<double>(), m.target["y"].get<double>(), m.role_id, str(m.source, "scene_id"), num(m.source, "owner_epoch"), num(m.source, "session_epoch")});
    if (count != 1) throw Error(409, "迁移提交时源归属已变化");
    save_combat(m.target);
    if (db_.exec("UPDATE migrations SET status='COMMITTED' WHERE role_id=? AND request_id=? AND status='PREPARING'", {m.role_id, m.request_id}) != 1) throw Error(409, "迁移状态已变化");
    tx.commit(); m.status = "COMMITTED";
}
[[noreturn]] void Service::cancel_after_failure(Migration& m, const std::string& message) {
    try { set_status(m, "ABORTING"); }
    catch (...) { throw Error(503, "迁移取消状态待核实，请稍后重试"); }
    abort_migration(m);
    throw Error(503, message + "；已安全取消，使用新的 request_id 可重试");
}
void Service::abort_migration(Migration& m) {
    auto current = role(m.role_id);
    if (!same_ref(current, m.source) || str(current, "scene_id") != str(m.source, "scene_id")) throw Error(503, "迁移撤销归属校验失败，角色保持暂停");
    if (m.status != "ABORTING") set_status(m, "ABORTING");
    // 必须先作废目标代次；超时结果不明时绝不盲目恢复源。
    try { scene(str(m.target, "scene_id"), "remove", reference(m.target, m.transfer_id)); }
    catch (...) { throw Error(503, "迁移撤销等待目标场景恢复，角色暂时暂停"); }
    try {
        try { scene(str(m.source, "scene_id"), "unfreeze", reference(m.source, m.transfer_id)); }
        catch (const Error& error) { if (error.status != 404) throw; }
        auto result = scene(str(m.source, "scene_id"), "enter", {{"role", m.source}});
        if (!result.contains("role") || !valid_snapshot(result["role"], m.source)) throw Error(503, "invalid restored role");
        copy(m.source, result["role"]);
    } catch (...) { throw Error(503, "迁移撤销等待源场景恢复，角色暂时暂停"); }
    db::Tx tx(db_); save_position(m.source); save_combat(m.source);
    db_.exec("UPDATE migrations SET status='ABORTED' WHERE role_id=? AND request_id=?", {m.role_id, m.request_id});
    tx.commit(); m.status = "ABORTED";
}
void Service::finish_committed(Migration& m) {
    auto current = role(m.role_id);
    if (!same_ref(current, m.target) || str(current, "scene_id") != str(m.target, "scene_id")) throw Error(503, "已提交迁移的目标归属校验失败");
    Json result;
    try { result = scene(str(m.target, "scene_id"), "prepare", {{"role", m.target}, {"transfer_id", m.transfer_id}}); }
    catch (...) { throw Error(503, "迁移已提交，等待目标场景准备恢复"); }
    if (!result.contains("role") || !valid_snapshot(result["role"], m.target)) throw Error(503, "目标场景准备快照无效");
    try { result = scene(str(m.target, "scene_id"), "activate", reference(m.target, m.transfer_id)); }
    catch (...) { throw Error(503, "迁移已提交，等待目标场景激活；请重试相同请求"); }
    if (!result.contains("role") || !valid_snapshot(result["role"], m.target)) throw Error(503, "目标场景激活快照无效");
    try { scene(str(m.source, "scene_id"), "remove", reference(m.source, m.transfer_id)); }
    catch (...) { throw Error(503, "目标已激活，等待源场景清理；请稍后重试"); }
    db::Tx tx(db_); save_position(result["role"]); save_combat(result["role"]);
    db_.exec("UPDATE migrations SET status='DONE' WHERE role_id=? AND request_id=?", {m.role_id, m.request_id});
    tx.commit(); m.status = "DONE";
}
void Service::recover_migration(Migration& m) {
    if (m.status == "PREPARING" || m.status == "ABORTING") abort_migration(m);
    else if (m.status == "COMMITTED") finish_committed(m);
    else if (m.status != "DONE" && m.status != "ABORTED") throw Error(503, "未知迁移状态，角色保持暂停");
}
void Service::recover_role(const std::string& id) { auto m = migration(id); if (m) recover_migration(*m); }
void Service::recover() {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto rows = db_.query("SELECT role_id,request_id FROM migrations WHERE status NOT IN ('DONE','ABORTED')");
    std::size_t failed = 0;
    for (auto& row : rows) {
        try { auto m = migration(db::text(row, 0), db::text(row, 1)); if (m) recover_migration(*m); }
        catch (...) { ++failed; }
    }
    if (failed) throw Error(503, "pending migrations await scene recovery (" + std::to_string(failed) + ")");
}
Json Service::transfer(const Json& req) {
    if (!req.is_object()) throw Error(400, "无效迁移请求");
    const std::set<std::string> allowed{"role_id", "owner_epoch", "session_epoch", "target_scene_id", "request_id", "restart"};
    for (auto it = req.begin(); it != req.end(); ++it) if (!allowed.count(it.key())) throw Error(400, "无效迁移请求");
    auto id = str(req, "role_id"), target = str(req, "target_scene_id"), request_id = str(req, "request_id");
    if (request_id.empty() || request_id.size() > 128 || num(req, "owner_epoch") <= 0 || !config_.scenes.count(target)) throw Error(400, "无效迁移请求");
    // nlohmann已验证UTF-8；C0/C1控制的UTF-8编码都在此拒绝。
    for (std::size_t i = 0; i < request_id.size(); ++i) {
        auto byte = static_cast<unsigned char>(request_id[i]);
        if (byte < 32 || byte == 127 || (byte == 0xc2 && i + 1 < request_id.size() && static_cast<unsigned char>(request_id[i + 1]) >= 0x80 && static_cast<unsigned char>(request_id[i + 1]) <= 0x9f)) throw Error(400, "无效迁移请求标识");
    }
    auto r = validate_session(req);
    auto previous = migration(id, request_id);
    if (previous) {
        if (previous->target_scene_id != target) throw Error(409, "相同 request_id 不能用于不同目标");
        if (previous->status != "DONE" && previous->status != "ABORTED") recover_migration(*previous);
        if (previous->status == "ABORTED") throw Error(409, "这次迁移已经取消，请使用新的 request_id");
        return route(role(id));
    }
    recover_role(id); r = role(id);
    if (!same_ref(req, r)) throw Error(409, "角色归属已变化，请刷新会话路由");
    const auto restart = req.value("restart", false);
    if (str(r, "scene_id") == target && !restart) throw Error(400, "角色已在目标场景");
    if (restart) {
        if (str(r, "scene_id") != target) throw Error(400, "归属轮换必须使用当前场景");
        try { if (!scene(target, "capabilities", Json::object()).value("owner_rotation", false)) throw Error(400, "unsupported"); }
        catch (...) { throw Error(400, "当前场景不支持同场景归属轮换"); }
    }
    auto m = begin_migration(r, req);
    Json frozen;
    try { frozen = scene(str(m.source, "scene_id"), "freeze", reference(m.source, m.transfer_id)); }
    catch (...) { cancel_after_failure(m, "源场景冻结失败，迁移未提交"); }
    if (!frozen.contains("role") || !valid_snapshot(frozen["role"], m.source)) cancel_after_failure(m, "源场景返回了无效快照");
    copy(m.source, frozen["role"]); copy(m.target, frozen["role"]);
    try { db_.exec("UPDATE migrations SET source_json=?,target_json=? WHERE role_id=? AND request_id=? AND status='PREPARING'", {m.source.dump(), m.target.dump(), m.role_id, m.request_id}); }
    catch (...) { throw Error(503, "保存迁移快照失败，角色暂时暂停；重试或重新登录恢复"); }
    Json prepared;
    try { prepared = scene(str(m.target, "scene_id"), "prepare", {{"role", m.target}, {"transfer_id", m.transfer_id}}); }
    catch (...) { cancel_after_failure(m, "目标场景准备失败，迁移未提交"); }
    if (!prepared.contains("role") || !valid_snapshot(prepared["role"], m.target)) cancel_after_failure(m, "目标场景返回了无效快照");
    try { commit_migration(m); }
    catch (...) { throw Error(503, "归属提交结果待核实，请重试相同 request_id 或重新登录"); }
    finish_committed(m); return route(role(id));
}
}
