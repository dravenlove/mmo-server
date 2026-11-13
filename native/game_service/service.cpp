#include "game_service/service.hpp"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <set>
#include <sstream>

namespace native::game {
namespace {
using I = std::int64_t;
const std::string columns = "role_id,name,scene_id,owner_epoch,session_epoch,x,y,coins";
std::string text_field(const Json& j, const char* key) {
    if (!j.contains(key)) return {};
    if (!j[key].is_string()) throw Error(400, "请求 JSON 或字段无效");
    return j[key].get<std::string>();
}
I integer_field(const Json& j, const char* key, I fallback = 0) {
    if (!j.contains(key)) return fallback;
    if (!j[key].is_number_integer() || (j[key].is_number_unsigned() && j[key].get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX))) throw Error(400, "整数请求字段无效");
    return j[key].get<I>();
}
double number_field(const Json& j, const char* key) {
    if (!j.contains(key)) return 0;
    if (!j[key].is_number()) throw Error(400, "数值请求字段无效");
    return j[key].get<double>();
}
void fields(const Json& request, std::initializer_list<const char*> allowed) {
    if (!request.is_object()) throw Error(400, "请求必须是 JSON 对象");
    std::set<std::string> keys(allowed.begin(), allowed.end());
    for (auto i = request.begin(); i != request.end(); ++i) if (!keys.count(i.key())) throw Error(400, "请求 JSON 或字段无效");
}
std::string random_hex(int count) {
    std::vector<unsigned char> bytes(static_cast<std::size_t>(count));
    if (RAND_bytes(bytes.data(), count) != 1) throw std::runtime_error("secure random unavailable");
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto byte : bytes) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}
std::string hash_token(const std::string& token) {
    unsigned char sum[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(token.data()), token.size(), sum);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto byte : sum) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}
bool hex(const std::string& value, std::size_t size) {
    return value.size() == size && std::all_of(value.begin(), value.end(), [](unsigned char x) { return std::isxdigit(x) != 0; });
}
bool position(const Json& r) {
    try {
        const double x = number_field(r, "x"), y = number_field(r, "y");
        return std::isfinite(x) && std::isfinite(y) && x >= -100 && x <= 100 && y >= -100 && y <= 100;
    } catch (...) { return false; }
}
bool same_ref(const Json& a, const Json& b) {
    return text_field(a, "role_id") == text_field(b, "role_id") && integer_field(a, "owner_epoch") == integer_field(b, "owner_epoch") && integer_field(a, "session_epoch") == integer_field(b, "session_epoch");
}
bool has_combat(const Json& r) { return r.contains("combat") && !r["combat"].is_null(); }
bool snapshot(const Json& actual, const Json& expected) {
    try {
        if (has_combat(actual)) combat_revision(actual["combat"]);
        return same_ref(actual, expected) && text_field(actual, "scene_id") == text_field(expected, "scene_id") && position(actual);
    } catch (...) { return false; }
}
Json scan_role(const db::Row& row) {
    return {{"role_id", db::text(row, 0)}, {"name", db::text(row, 1)}, {"scene_id", db::text(row, 2)},
        {"owner_epoch", db::integer(row, 3)}, {"session_epoch", db::integer(row, 4)}, {"x", db::number(row, 5)}, {"y", db::number(row, 6)}, {"coins", db::integer(row, 7)}};
}
// Go unicode.IsControl/TrimSpace语义：严格UTF-8、C0/C1控制和Unicode空格。
std::vector<std::uint32_t> unicode(const std::string& s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char first = static_cast<unsigned char>(s[i++]);
        std::uint32_t cp; unsigned remaining;
        if (first < 0x80) { cp = first; remaining = 0; }
        else if (first >= 0xc2 && first <= 0xdf) { cp = first & 0x1f; remaining = 1; }
        else if (first >= 0xe0 && first <= 0xef) { cp = first & 0x0f; remaining = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { cp = first & 7; remaining = 3; }
        else throw Error(400, "无效UTF-8");
        const unsigned bytes = remaining;
        while (remaining--) {
            if (i == s.size()) throw Error(400, "无效UTF-8");
            unsigned char next = static_cast<unsigned char>(s[i++]);
            if ((next & 0xc0) != 0x80) throw Error(400, "无效UTF-8");
            cp = (cp << 6) | (next & 0x3f);
        }
        if ((bytes == 1 && cp < 0x80) || (bytes == 2 && cp < 0x800) || (bytes == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) throw Error(400, "无效UTF-8");
        out.push_back(cp);
    }
    return out;
}
bool space(std::uint32_t cp) { return (cp >= 9 && cp <= 13) || cp == 0x20 || cp == 0x85 || cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000; }
bool control(std::uint32_t cp) { return cp < 32 || (cp >= 0x7f && cp <= 0x9f); }
bool identity_name(const std::string& name) {
    try { auto points = unicode(name); return !points.empty() && points.size() <= 24 && !space(points.front()) && !space(points.back()) && std::none_of(points.begin(), points.end(), control); }
    catch (...) { return false; }
}
std::string trim(const std::string& name) {
    unicode(name);
    std::size_t front = 0, back = name.size();
    while (front < back) { auto rest = unicode(name.substr(front)); if (!space(rest.front())) break; ++front; while (front < back && (static_cast<unsigned char>(name[front]) & 0xc0) == 0x80) ++front; }
    while (back > front) { auto start = back - 1; while (start > front && (static_cast<unsigned char>(name[start]) & 0xc0) == 0x80) --start; auto point = unicode(name.substr(start, back - start)); if (!space(point[0])) break; back = start; }
    return name.substr(front, back - front);
}
void copy_snapshot(Json& destination, const Json& source) {
    destination["x"] = number_field(source, "x");
    destination["y"] = number_field(source, "y");
    if (has_combat(source)) destination["combat"] = source["combat"];
}
}

I combat_revision(const Json& c) {
    if (c.is_null()) return 0;
    if (!c.is_object() || c.dump().size() > 4096) throw Error(400, "战斗快照必须为有限大小的对象");
    for (auto key : {"hp", "max_hp", "mp", "max_mp", "revive_remaining_ms", "revision", "last_action_seq", "action_session"}) if (!c.contains(key)) throw Error(400, "战斗快照数值无效");
    I hp = integer_field(c, "hp"), mhp = integer_field(c, "max_hp"), mp = integer_field(c, "mp"), mmp = integer_field(c, "max_mp");
    I revive = integer_field(c, "revive_remaining_ms"), revision = integer_field(c, "revision");
    if (!c.contains("dead") || !c["dead"].is_boolean() || mhp < 1 || mhp > 1000000 || hp < 0 || hp > mhp || mmp < 1 || mmp > 1000000 || mp < 0 || mp > mmp || revive < 0 || revive > 600000 || revision < 0 || integer_field(c, "last_action_seq") < 0 || integer_field(c, "action_session") < 1 || c["dead"].get<bool>() != (hp == 0)) throw Error(400, "战斗快照数值无效");
    if (!c.contains("cooldowns") || !c["cooldowns"].is_object() || (c["cooldowns"].size() != 4 && c["cooldowns"].size() != 6)) throw Error(400, "战斗冷却快照无效");
    const auto& cd = c["cooldowns"];
    const std::set<std::string> keys{"strike", "whirlwind", "charge", "mend", "arc_bolt", "stone_wall"};
    for (auto i = cd.begin(); i != cd.end(); ++i) if (!keys.count(i.key()) || integer_field(cd, i.key().c_str()) < 0 || integer_field(cd, i.key().c_str()) > 600000) throw Error(400, "战斗冷却快照无效");
    for (auto key : {"strike", "whirlwind", "charge", "mend"}) if (!cd.contains(key)) throw Error(400, "战斗冷却快照缺少基础技能");
    if (cd.contains("arc_bolt") != cd.contains("stone_wall")) throw Error(400, "战斗冷却快照不完整");
    return revision;
}

void parse_scenes(Config& config, const std::string& raw) {
    std::stringstream entries(raw);
    std::string entry;
    while (std::getline(entries, entry, ',')) {
        const auto equal = entry.find('=');
        if (equal == std::string::npos) throw Error(400, "SCENES invalid");
        const auto id = trim(entry.substr(0, equal));
        auto url = trim(entry.substr(equal + 1));
        while (!url.empty() && url.back() == '/') url.pop_back();
        auto scheme = url.find("://");
        if (id.empty() || id.size() > 64 || scheme == std::string::npos || (url.substr(0, scheme) != "http" && url.substr(0, scheme) != "https") || url.size() <= scheme + 3 || url.find_first_of("/?#@", scheme + 3) != std::string::npos || config.scenes.count(id)) throw Error(400, "SCENES invalid");
        config.scenes[id] = url;
        if (config.default_scene.empty()) config.default_scene = id;
    }
    if (config.scenes.empty()) throw Error(400, "SCENES empty");
}

Service::Service(Config c) : config_(std::move(c)), db_(config_.database) {
    if (config_.token.size() < 32 || !config_.scenes.count(config_.default_scene)) throw Error(400, "invalid service configuration");
    for (auto& scene : config_.scenes) { Config check; parse_scenes(check, scene.first + "=" + scene.second); }
    if (!config_.auth_url.empty()) { Config check; parse_scenes(check, "auth=" + config_.auth_url); }
    if (!config_.room_worker_url.empty()) { Config check; parse_scenes(check, "worker=" + config_.room_worker_url); }
    const std::vector<std::string> sqlite_schema{
        "CREATE TABLE IF NOT EXISTS roles (role_id TEXT PRIMARY KEY,name TEXT NOT NULL UNIQUE,token_hash TEXT NOT NULL UNIQUE,scene_id TEXT NOT NULL,owner_epoch INTEGER NOT NULL,epoch_counter INTEGER NOT NULL,session_epoch INTEGER NOT NULL,x REAL NOT NULL,y REAL NOT NULL,coins INTEGER NOT NULL CHECK(coins>=0))",
        "CREATE TABLE IF NOT EXISTS claims (operation_id TEXT PRIMARY KEY,role_id TEXT NOT NULL REFERENCES roles(role_id))",
        "CREATE TABLE IF NOT EXISTS combat_snapshots (role_id TEXT PRIMARY KEY REFERENCES roles(role_id),owner_epoch INTEGER NOT NULL,session_epoch INTEGER NOT NULL,revision INTEGER NOT NULL,state_json TEXT NOT NULL)",
        "CREATE TABLE IF NOT EXISTS migrations (role_id TEXT NOT NULL REFERENCES roles(role_id),request_id TEXT NOT NULL,transfer_id TEXT NOT NULL UNIQUE,target_scene_id TEXT NOT NULL,status TEXT NOT NULL,source_json TEXT NOT NULL,target_json TEXT NOT NULL,PRIMARY KEY(role_id,request_id))",
        "CREATE UNIQUE INDEX IF NOT EXISTS one_pending_transfer ON migrations(role_id) WHERE status NOT IN ('DONE','ABORTED')"};
    const std::vector<std::string> mysql_schema{
        "CREATE TABLE IF NOT EXISTS roles (role_id VARBINARY(128) PRIMARY KEY,name VARBINARY(96) NOT NULL UNIQUE,token_hash BINARY(64) NOT NULL UNIQUE,scene_id VARBINARY(64) NOT NULL,owner_epoch BIGINT NOT NULL,epoch_counter BIGINT NOT NULL,session_epoch BIGINT NOT NULL,x DOUBLE NOT NULL,y DOUBLE NOT NULL,coins BIGINT NOT NULL CHECK(coins>=0)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS claims (operation_id VARBINARY(160) PRIMARY KEY,role_id VARBINARY(128) NOT NULL,FOREIGN KEY(role_id) REFERENCES roles(role_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS combat_snapshots (role_id VARBINARY(128) PRIMARY KEY,owner_epoch BIGINT NOT NULL,session_epoch BIGINT NOT NULL,revision BIGINT NOT NULL,state_json TEXT NOT NULL,FOREIGN KEY(role_id) REFERENCES roles(role_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
        "CREATE TABLE IF NOT EXISTS migrations (role_id VARBINARY(128) NOT NULL,request_id VARBINARY(128) NOT NULL,transfer_id VARBINARY(128) NOT NULL UNIQUE,target_scene_id VARBINARY(64) NOT NULL,status VARBINARY(16) NOT NULL,source_json JSON NOT NULL,target_json JSON NOT NULL,pending_role VARBINARY(128) GENERATED ALWAYS AS (CASE WHEN status NOT IN ('DONE','ABORTED') THEN role_id ELSE NULL END) STORED,PRIMARY KEY(role_id,request_id),UNIQUE KEY one_pending_transfer (pending_role),FOREIGN KEY(role_id) REFERENCES roles(role_id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"};
    for (const auto& sql : db_.is_mysql() ? mysql_schema : sqlite_schema) db_.exec(sql);
    if (db_.is_mysql()) {
        auto count = db_.query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN ('roles','claims','migrations','combat_snapshots') AND ENGINE='InnoDB'");
        if (db::integer(count.at(0), 0) != 4) throw Error(503, "MySQL tables must use InnoDB");
    }
    if (!config_.room_worker_url.empty()) {
        while (config_.room_worker_url.back() == '/') config_.room_worker_url.pop_back();
        room_schema(); restore_room_routes(); start_room_acks();
    }
}

Json Service::role(const std::string& id) {
    auto rows = db_.query("SELECT " + columns + " FROM roles WHERE role_id=?", {id});
    if (rows.empty()) throw Error(404, "角色不存在");
    auto result = scan_role(rows[0]); load_combat(result); return result;
}
Json Service::route(const Json& r) {
    const auto scene_id = text_field(r, "scene_id");
    auto found = config_.scenes.find(scene_id);
    if (found == config_.scenes.end()) throw Error(503, "角色场景不在配置中");
    Json result{{"role", r}, {"scene_url", found->second}};
    if (room_scene(scene_id)) { result["room_id"] = scene_id; result["room_profile"] = room_record(scene_id).at("profile"); }
    return result;
}
Json Service::validate_session(const Json& req) {
    auto id = text_field(req, "role_id");
    if (id.empty() || id.size() > 128 || integer_field(req, "session_epoch") <= 0) throw Error(400, "无效会话");
    auto r = role(id);
    if (integer_field(r, "session_epoch") != integer_field(req, "session_epoch")) throw Error(409, "会话已失效，请重新登录");
    return r;
}
void Service::load_combat(Json& r) {
    auto rows = db_.query("SELECT owner_epoch,session_epoch,state_json FROM combat_snapshots WHERE role_id=?", {text_field(r, "role_id")});
    if (rows.empty()) return;
    if (db::integer(rows[0], 0) != integer_field(r, "owner_epoch") || db::integer(rows[0], 1) > integer_field(r, "session_epoch")) throw Error(503, "战斗快照归属不匹配");
    r["combat"] = Json::parse(db::text(rows[0], 2)); combat_revision(r["combat"]);
}
void Service::save_combat(const Json& r) {
    if (!has_combat(r)) return;
    const auto revision = combat_revision(r["combat"]);
    auto rows = db_.query("SELECT owner_epoch,session_epoch,revision FROM combat_snapshots WHERE role_id=?", {text_field(r, "role_id")});
    if (rows.empty()) db_.exec("INSERT INTO combat_snapshots(role_id,owner_epoch,session_epoch,revision,state_json) VALUES(?,?,?,?,?)", {text_field(r, "role_id"), integer_field(r, "owner_epoch"), integer_field(r, "session_epoch"), revision, r["combat"].dump()});
    else {
        if (db::integer(rows[0], 0) == integer_field(r, "owner_epoch") && db::integer(rows[0], 1) == integer_field(r, "session_epoch") && db::integer(rows[0], 2) > revision) throw Error(409, "战斗快照已过期");
        db_.exec("UPDATE combat_snapshots SET owner_epoch=?,session_epoch=?,revision=?,state_json=? WHERE role_id=?", {integer_field(r, "owner_epoch"), integer_field(r, "session_epoch"), revision, r["combat"].dump(), text_field(r, "role_id")});
    }
}
void Service::save_position(const Json& r) {
    db_.exec("UPDATE roles SET x=?,y=? WHERE role_id=? AND owner_epoch=? AND session_epoch=?", {number_field(r, "x"), number_field(r, "y"), text_field(r, "role_id"), integer_field(r, "owner_epoch"), integer_field(r, "session_epoch")});
}
Json Service::scene(const std::string& id, const std::string& op, const Json& req) {
    if (!config_.scenes.count(id)) throw Error(400, "未知场景");
    auto body = req;
    if (room_scene(id)) {
        Json metadata;
        if (op == "remove") {
            // 清理不能复活已回收房间，也不能误删重启后同ID的新世界实体。
            try { metadata = http_post(config_.room_worker_url, "/v1/rooms/status", {{"room_id", id}}, config_.token, 2000); }
            catch (const Error& error) { if (error.status == 404) return {{"accepted", true}}; throw; }
        } else metadata = ensure_room(id);
        body["_room_world_epoch"] = metadata.at("world_epoch");
    }
    try { return http_post(config_.scenes.at(id), "/v1/" + op, body, config_.token, 2000); }
    catch (const Error& error) {
        // 不存在的临时房间没有可复活的旧实体；移除仍须可幂等完成迁移清理。
        if (room_scene(id) && op == "remove" && error.status == 404) return {{"accepted", true}};
        throw;
    }
}
Json Service::auth(const std::string& op, const Json& req) {
    try { return http_post(config_.auth_url, "/v1/" + op, req, config_.token, 5000); }
    catch (const Error& error) {
        if (error.status == 400) throw Error(400, "账号请求格式无效");
        if (error.status == 401) throw Error(401, op.rfind("account_", 0) == 0 ? "账号、密码或原登录凭据无效" : "登录 token 无效");
        if (error.status == 409) throw Error(409, "账号名或密码绑定已存在");
        if (error.status == 429) throw Error(429, "登录请求过于频繁，请稍后重试");
        throw Error(503, "登录鉴权服务暂不可用");
    }
}
Json Service::identity(const std::string& op, const Json& req) {
    if (op == "register") {
        fields(req, {"name"}); auto out = auth("register", req);
        if (!hex(text_field(out, "role_id"), 32) || !hex(text_field(out, "token"), 64)) throw Error(503, "鉴权返回无效账号");
        return {{"token", out["token"]}, {"role_id", out["role_id"]}};
    }
    fields(req, {"token"}); auto token = text_field(req, "token");
    if (token.size() != 64) throw Error(401, "登录 token 无效");
    auto out = auth("authenticate", req);
    auto id = text_field(out, "role_id"), name = text_field(out, "name"), stable = text_field(out, "legacy_token_hash");
    if (!hex(id, 32) || !identity_name(name) || (!stable.empty() && !hex(stable, 64))) throw Error(503, "鉴权返回无效身份");
    if (stable.empty()) stable = hash_token(token);
    // 跨地域鉴权先完成，再持有游戏状态锁。
    std::lock_guard<std::mutex> guard(mutex_);
    auto rows = db_.query("SELECT " + columns + " FROM roles WHERE role_id=?", {id});
    if (rows.empty()) {
        try { db_.exec("INSERT INTO roles(role_id,name,token_hash,scene_id,owner_epoch,epoch_counter,session_epoch,x,y,coins) VALUES(?,?,?,?,1,1,0,0,0,0)", {id, name, stable, config_.default_scene}); }
        catch (...) { throw Error(503, "游戏角色初始化失败；请保留凭据稍后重新登录"); }
    } else if (db::text(rows[0], 1) != name) throw Error(503, "角色身份不匹配");
    return login_role(id);
}
Json Service::account(const std::string& op, const Json& req) {
    if (config_.auth_url.empty()) throw Error(503, "账号密码登录需要配置独立 Auth 服务");
    if (op == "account_bind") fields(req, {"token", "password"}); else fields(req, {"name", "password"});
    for (auto i = req.begin(); i != req.end(); ++i) if (!i.value().is_string()) throw Error(400, "账号请求字段无效");
    auto out = auth(op, req);
    if (!hex(text_field(out, "role_id"), 32) || !identity_name(text_field(out, "name"))) throw Error(503, "鉴权返回无效账号");
    if (op == "account_bind") { if (!out.value("bound", false)) throw Error(503, "鉴权返回无效绑定结果"); return out; }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto expiry = integer_field(out, "expires_at_unix_ms");
    if (!hex(text_field(out, "token"), 64) || expiry <= now || expiry > now + 25 * 60 * 60 * 1000) throw Error(503, "鉴权返回无效登录票据");
    return out;
}
Json Service::register_local(const Json& req) {
    fields(req, {"name"}); auto name = trim(text_field(req, "name"));
    if (!identity_name(name)) throw Error(400, "名字需要 1 到 24 个字符且不含控制字符");
    if (db::integer(db_.query("SELECT COUNT(*) FROM roles WHERE name=?", {name}).at(0), 0)) throw Error(409, "名字已被注册；已有角色请使用原 token 登录");
    auto token = random_hex(32), id = random_hex(16);
    db_.exec("INSERT INTO roles(role_id,name,token_hash,scene_id,owner_epoch,epoch_counter,session_epoch,x,y,coins) VALUES(?,?,?,?,1,1,0,0,0,0)", {id, name, hash_token(token), config_.default_scene});
    return {{"token", token}, {"role_id", id}};
}
Json Service::login_local(const Json& req) {
    fields(req, {"token"}); auto token = text_field(req, "token");
    if (token.size() != 64) throw Error(401, "登录 token 无效");
    auto rows = db_.query("SELECT role_id FROM roles WHERE token_hash=?", {hash_token(token)});
    if (rows.empty()) throw Error(401, "登录 token 无效");
    return login_role(db::text(rows[0], 0));
}
Json Service::login_role(const std::string& id) {
    recover_role(id);
    db_.exec("UPDATE roles SET session_epoch=session_epoch+1 WHERE role_id=?", {id});
    auto r = role(id); Json out;
    if (room_scene(text_field(r, "scene_id"))) recover_finished_room(r);
    if (room_scene(text_field(r, "scene_id"))) bind_room_world(r, ensure_room(text_field(r, "scene_id")), false);
    try { out = scene(text_field(r, "scene_id"), "enter", {{"role", r}}); }
    catch (...) { throw Error(503, "当前场景无法入场，请稍后重新登录"); }
    if (!out.contains("role") || !snapshot(out["role"], r)) throw Error(503, "场景返回了无效角色快照");
    copy_snapshot(r, out["role"]);
    db::Tx tx(db_); save_position(r); save_combat(r); tx.commit(); return route(r);
}
Json Service::session(const Json& req) {
    fields(req, {"role_id", "session_epoch"}); validate_session(req); auto id = text_field(req, "role_id"); recover_role(id);
    auto r = role(id);
    if (room_scene(text_field(r, "scene_id"))) {
        recover_finished_room(r);
        if (room_scene(text_field(r, "scene_id"))) {
            const auto meta = ensure_room(text_field(r, "scene_id"));
            bind_room_world(r, meta, true);
            bool present = false;
            for (const auto& member : meta.value("members", Json::array())) {
                const auto& value = member.contains("role") ? member["role"] : member;
                if (value.value("role_id", std::string()) == text_field(r, "role_id") &&
                    value.value("owner_epoch", std::int64_t{0}) == integer_field(r, "owner_epoch") &&
                    value.value("session_epoch", std::int64_t{0}) == integer_field(r, "session_epoch")) present = true;
            }
            if (!present) {
                const auto entered = scene(text_field(r, "scene_id"), "enter", {{"role", r}});
                if (!entered.contains("role") || !snapshot(entered["role"], r)) throw Error(503, "invalid restored room role");
                copy_snapshot(r, entered["role"]);
                db::Tx tx(db_); save_position(r); save_combat(r); tx.commit();
            }
        }
    }
    return route(r);
}
Json Service::checkpoint(const Json& req) {
    fields(req, {"role"}); if (!req.contains("role")) throw Error(400, "无效角色快照");
    const auto& r = req["role"];
    fields(r, {"role_id", "name", "scene_id", "owner_epoch", "session_epoch", "x", "y", "coins", "combat"});
    for (auto key : {"role_id", "name", "scene_id"}) text_field(r, key);
    integer_field(r, "coins");
    if (text_field(r, "role_id").empty() || integer_field(r, "owner_epoch") <= 0 || integer_field(r, "session_epoch") <= 0 || !position(r)) throw Error(400, "无效角色快照");
    if (has_combat(r)) combat_revision(r["combat"]);
    db::Tx tx(db_);
    auto count = db_.exec("UPDATE roles SET x=?,y=? WHERE role_id=? AND scene_id=? AND owner_epoch=? AND session_epoch=? AND NOT EXISTS(SELECT 1 FROM migrations WHERE role_id=? AND status NOT IN ('DONE','ABORTED'))", {number_field(r, "x"), number_field(r, "y"), text_field(r, "role_id"), text_field(r, "scene_id"), integer_field(r, "owner_epoch"), integer_field(r, "session_epoch"), text_field(r, "role_id")});
    if (count != 1) throw Error(409, "快照归属或会话已失效，或角色正在迁移");
    save_combat(r); tx.commit();
    try { queue_room_ack(r); } catch (...) { /* 缓存确认失败不改变已成功的持久化提交。 */ }
    return {{"ok", true}};
}
Json Service::claim(const Json& req) {
    fields(req, {"role_id", "session_epoch"}); validate_session(req); auto id = text_field(req, "role_id"); recover_role(id);
    db::Tx tx(db_); bool granted = true;
    try { db_.exec("INSERT INTO claims(operation_id,role_id) VALUES(?,?)", {"welcome:" + id, id}); }
    catch (const db::Error& error) { if (!error.unique()) throw; granted = false; }
    if (granted) db_.exec("UPDATE roles SET coins=coins+100 WHERE role_id=?", {id});
    auto coins = db::integer(db_.query("SELECT coins FROM roles WHERE role_id=?", {id}).at(0), 0);
    tx.commit(); return {{"coins", coins}, {"granted", granted}};
}
Json Service::execute(const std::string& op, const Json& req) {
    if (op.rfind("account_", 0) == 0) return account(op, req);
    if (!config_.auth_url.empty() && (op == "register" || op == "login")) return identity(op, req);
    std::lock_guard<std::mutex> guard(mutex_);
    if (op == "register") return register_local(req);
    if (op == "login") return login_local(req);
    if (op == "session") return session(req);
    if (op == "checkpoint") return checkpoint(req);
    if (op == "claim") return claim(req);
    if (op == "transfer") {
        if (req.contains("target_scene_id") && req["target_scene_id"].is_string() && room_scene(req["target_scene_id"])) throw Error(400, "use room_join for dynamic room entry");
        return transfer(req);
    }
    if (op == "room_drain") return drain_rooms(req);
    if (op.rfind("room_", 0) == 0) return room_request(op, req);
    throw Error(404, "接口不存在");
}
void Service::health() { std::lock_guard<std::mutex> guard(mutex_); db_.ping(); }
}
