#include "scene.hpp"
#include <iostream>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <utility>

namespace mmo {
namespace {
std::string string_field(const Json& j, const char* name, std::size_t maximum = 128) {
    auto it = j.find(name);
    if (it == j.end() || !it->is_string()) throw Error(400, std::string("invalid ") + name);
    auto value = it->get<std::string>();
    if (value.empty() || value.size() > maximum || value.find('\0') != std::string::npos)
        throw Error(400, std::string("invalid ") + name);
    return value;
}

std::int64_t integer_field(const Json& j, const char* name, bool allow_zero = false) {
    auto it = j.find(name);
    if (it == j.end() || !it->is_number_integer()) throw Error(400, std::string("invalid ") + name);
    if (it->is_number_unsigned() && it->get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX))
        throw Error(400, std::string("invalid ") + name);
    auto value = it->get<std::int64_t>();
    if (value < (allow_zero ? 0 : 1)) throw Error(400, std::string("invalid ") + name);
    return value;
}

double real_field(const Json& j, const char* name, double maximum) {
    auto it = j.find(name);
    if (it == j.end() || !it->is_number()) throw Error(400, std::string("invalid ") + name);
    double value = it->get<double>();
    if (!std::isfinite(value) || std::abs(value) > maximum)
        throw Error(400, std::string("invalid ") + name);
    return value;
}

bool same_ref(const Role& a, const Role& b) {
    return a.role_id == b.role_id && a.owner_epoch == b.owner_epoch && a.session_epoch == b.session_epoch;
}

void changed(Role::Combat& combat) {
    if (combat.revision == INT64_MAX) throw Error(503, "combat revision exhausted");
    ++combat.revision;
}

int milliseconds(double seconds) {
    return static_cast<int>(std::ceil(std::max(0.0, seconds) * 1000.0 - 1e-7));
}

struct Skill {
    const char* id;
    const char* name;
    const char* description;
    double range, cooldown;
    int mp_cost, damage, heal;
    const char* target;
    Json json() const {
        return {{"id", id}, {"name", name}, {"description", description}, {"range", range},
            {"cooldown_ms", milliseconds(cooldown)}, {"mp_cost", mp_cost},
            {"damage", damage}, {"heal", heal}, {"target", target}};
    }
};

const std::array<Skill, 4> skills{{
    {"strike", "斩击", "对近身目标造成伤害。", 3.2, 0.6, 0, 18, 0, "enemy"},
    {"whirlwind", "旋风斩", "攻击周围所有存活怪物。", 5, 5, 20, 32, 0, "self"},
    {"charge", "突进", "突进到目标身边并短暂击晕。", 14, 7, 15, 24, 0, "enemy"},
    {"mend", "战意恢复", "恢复自身生命。", 0, 12, 25, 0, 45, "self"}
}};

void exact_keys(const Json& value, std::initializer_list<const char*> keys) {
    if (!value.is_object() || value.size() != keys.size()) throw Error(400, "unexpected config fields");
    for (auto key : keys) if (!value.contains(key)) throw Error(400, std::string("missing config field: ") + key);
}

std::int64_t config_integer(const Json& value, const char* key, std::int64_t low, std::int64_t high) {
    auto number = integer_field(value, key, true);
    if (number < low || number > high) throw Error(400, std::string("config value out of range: ") + key);
    return number;
}

double config_real(const Json& value, const char* key, double low, double high) {
    auto number = real_field(value, key, high);
    if (number < low) throw Error(400, std::string("config value out of range: ") + key);
    return number;
}

Json default_combat_config() {
    Json defaults{{"schema_version", 1}, {"version", "default-v1"}, {"skills", Json::object()},
        {"monsters", {
            {"wolf", {{"max_hp", 80}, {"damage", 8}, {"move_speed", 3.2}, {"attack_interval_ms", 1300}, {"respawn_ms", 8000}}},
            {"elite", {{"max_hp", 240}, {"damage", 14}, {"move_speed", 2.4}, {"attack_interval_ms", 1800}, {"respawn_ms", 8000}}}
        }}};
    for (const auto& skill : skills) defaults["skills"][skill.id] = {
        {"damage", skill.damage}, {"heal", skill.heal}, {"mp_cost", skill.mp_cost},
        {"range", skill.range}, {"cooldown_ms", milliseconds(skill.cooldown)}};
    return defaults;
}

Json validate_config(const Json& value) {
    if (value.dump().size() > 64 * 1024) throw Error(400, "combat config exceeds 64KiB");
    exact_keys(value, {"schema_version", "version", "skills", "monsters"});
    config_integer(value, "schema_version", 1, 1);
    const auto version = string_field(value, "version", 64);
    for (unsigned char character : version)
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-'))
            throw Error(400, "invalid config version");
    exact_keys(value["skills"], {"strike", "whirlwind", "charge", "mend"});
    for (const auto& skill : skills) {
        const auto& entry = value["skills"][skill.id];
        exact_keys(entry, {"damage", "heal", "mp_cost", "range", "cooldown_ms"});
        config_integer(entry, "damage", 0, 10000);
        config_integer(entry, "heal", 0, 180);
        config_integer(entry, "mp_cost", 0, 100);
        config_real(entry, "range", 0, 100);
        config_integer(entry, "cooldown_ms", 0, 600000);
        if (std::string(skill.id) == "mend") {
            if (entry["damage"] != 0 || entry["range"].get<double>() != 0)
                throw Error(400, "mend has fixed healing behavior");
        } else if (entry["heal"] != 0) throw Error(400, "attack skills have fixed damage behavior");
    }
    exact_keys(value["monsters"], {"wolf", "elite"});
    for (const auto& entry : value["monsters"]) {
        exact_keys(entry, {"max_hp", "damage", "move_speed", "attack_interval_ms", "respawn_ms"});
        config_integer(entry, "max_hp", 1, 1000000);
        config_integer(entry, "damage", 0, 180);
        config_real(entry, "move_speed", 0.01, 100);
        config_integer(entry, "attack_interval_ms", 100, 600000);
        config_integer(entry, "respawn_ms", 1000, 600000);
    }
    return value;
}

Skill configured_skill(const Skill& metadata, const Json& config) {
    auto skill = metadata;
    const auto& entry = config["skills"][skill.id];
    skill.damage = entry["damage"].get<int>();
    skill.heal = entry["heal"].get<int>();
    skill.mp_cost = entry["mp_cost"].get<int>();
    skill.range = entry["range"].get<double>();
    skill.cooldown = entry["cooldown_ms"].get<double>() / 1000;
    return skill;
}

Role::Combat parse_combat(const Json& value, std::int64_t session) {
    Role::Combat combat;
    combat.action_session = session;
    if (value.is_null()) return combat;
    if (!value.is_object() || value.dump().size() > 4096) throw Error(400, "invalid combat snapshot");
    auto bounded = [&](const char* key, std::int64_t high) {
        auto n = integer_field(value, key, true);
        if (n > high) throw Error(400, std::string("invalid combat ") + key);
        return n;
    };
    if (integer_field(value, "max_hp") != 180 || integer_field(value, "max_mp") != 100)
        throw Error(400, "invalid combat maxima");
    combat.hp = static_cast<int>(bounded("hp", 180));
    combat.mp = static_cast<double>(bounded("mp", 100));
    if (!value.contains("dead") || !value["dead"].is_boolean()) throw Error(400, "invalid combat dead flag");
    combat.dead = value["dead"].get<bool>();
    if (combat.dead != (combat.hp == 0)) throw Error(400, "combat death state mismatch");
    combat.revive_remaining = static_cast<double>(bounded("revive_remaining_ms", 5000)) / 1000;
    if (!combat.dead && combat.revive_remaining != 0) throw Error(400, "invalid revive state");
    combat.revision = integer_field(value, "revision");
    // 为正常 Tick 增长保留充分余量，拒绝损坏数据把计数器推到有符号溢出边缘。
    if (combat.revision > 9007199254740991LL) throw Error(400, "combat revision outside safe range");
    combat.last_action_seq = integer_field(value, "last_action_seq", true);
    combat.action_session = integer_field(value, "action_session", true);
    if (!value.contains("cooldowns") || !value["cooldowns"].is_object()) throw Error(400, "invalid cooldown snapshot");
    for (const auto& skill : skills) {
        auto remaining = integer_field(value["cooldowns"], skill.id, true);
        // 快照冷却是已结算状态，不随当前配置缩短；允许重连、迁移和配置回滚。
        if (remaining > 600000) throw Error(400, "invalid skill cooldown");
        combat.cooldowns[skill.id] = static_cast<double>(remaining) / 1000;
    }
    const auto& cooldowns = value["cooldowns"];
    if (cooldowns.contains("arc_bolt") != cooldowns.contains("stone_wall")) throw Error(400, "incomplete ability cooldown snapshot");
    for (const auto* id : {"arc_bolt", "stone_wall"}) if (cooldowns.contains(id)) {
        const auto remaining = integer_field(cooldowns, id, true);
        if (remaining > 600000) throw Error(400, "invalid ability cooldown");
        combat.cooldowns[id] = static_cast<double>(remaining) / 1000;
    }
    if (combat.action_session != session) {
        combat.action_session = session;
        combat.last_action_seq = 0;
        changed(combat);
    }
    return combat;
}
} // namespace

Json Role::Combat::json(bool full) const {
    Json result{{"hp", hp}, {"max_hp", 180}, {"mp", static_cast<int>(std::floor(mp + 1e-9))},
        {"max_mp", 100}, {"dead", dead}, {"revive_remaining_ms", milliseconds(revive_remaining)},
        {"revision", revision}};
    if (full) {
        result["last_action_seq"] = last_action_seq;
        result["action_session"] = action_session;
        result["cooldowns"] = Json::object();
        for (const auto& pair : cooldowns) result["cooldowns"][pair.first] = milliseconds(pair.second);
    }
    return result;
}

Json Role::json(bool full_combat) const {
    return {{"role_id", role_id}, {"name", name}, {"scene_id", scene_id},
            {"owner_epoch", owner_epoch}, {"session_epoch", session_epoch},
            {"x", x}, {"y", y}, {"coins", coins}, {"combat", combat.json(full_combat)}};
}

Json Scene::Npc::json() const {
    return {{"id", id}, {"name", name}, {"kind", kind}, {"x", x}, {"y", y},
        {"hp", hp}, {"max_hp", max_hp}, {"alive", hp > 0},
        {"respawn_remaining_ms", milliseconds(respawn_remaining)}, {"target_id", target_id},
        {"config_version", config_version}};
}

Scene::Scene(Config config) : config_(std::move(config)) {
    if (config_.abilities_enabled && !config_.combat_enabled) throw std::invalid_argument("abilities require combat mode");
    std::random_device random;
    world_epoch_ = std::to_string(random()) + "-" + std::to_string(random());
    combat_config_ = validate_config(config_.combat_config.is_null() ? default_combat_config() : config_.combat_config);
    config_history_.emplace(combat_config_["version"].get<std::string>(), combat_config_);
    const std::array<std::pair<double, double>, 4> spawns{{{-8, 4}, {10, 4}, {6, -10}, {18, 16}}};
    for (std::size_t i = 0; i < spawns.size(); ++i) {
        Npc npc;
        npc.id = "npc-" + std::to_string(i + 1);
        npc.name = i == 3 ? "试炼守卫" : "荒野狼";
        npc.kind = i == 3 ? "elite" : "wolf";
        npc.spawn_x = spawns[i].first;
        npc.spawn_y = spawns[i].second;
        reset_npc(npc);
        npcs_.emplace(npc.id, std::move(npc));
    }
}

Role Scene::parse_role(const Json& request) const {
    if (!request.contains("role") || !request["role"].is_object()) throw Error(400, "invalid role");
    const auto& j = request["role"];
    Role role;
    role.role_id = string_field(j, "role_id");
    role.name = string_field(j, "name", 96);
    role.scene_id = string_field(j, "scene_id");
    role.owner_epoch = integer_field(j, "owner_epoch");
    role.session_epoch = integer_field(j, "session_epoch");
    role.coins = integer_field(j, "coins", true);
    role.x = real_field(j, "x", 100);
    role.y = real_field(j, "y", 100);
    role.combat = parse_combat(j.value("combat", Json()), role.session_epoch);
    if (config_.abilities_enabled && !role.combat.cooldowns.count("arc_bolt")) {
        role.combat.cooldowns["arc_bolt"] = role.combat.cooldowns["stone_wall"] = 0;
        changed(role.combat);
    }
    if (role.scene_id != config_.scene_id) throw Error(409, "scene mismatch");
    return role;
}

void Scene::ensure_tracked(const std::string& role_id) {
    if (!removed_epochs_.count(role_id)) {
        if (removed_epochs_.size() >= config_.max_tracked_roles) throw Error(503, "scene identity capacity exhausted");
        removed_epochs_.emplace(role_id, 0);
    }
}

std::size_t Scene::occupied_count() const {
    std::size_t count=0;
    for (const auto& pair:entities_) if (pair.second.mode!=Mode::Active || (pair.second.connected && pair.second.lease_age<=3)) ++count;
    return count;
}
std::size_t Scene::player_count() const {
    std::size_t count=0;
    for (const auto& pair:entities_) if (pair.second.mode==Mode::Active && pair.second.connected && pair.second.lease_age<=3) ++count;
    return count;
}

void Scene::admit(const Role& role) {
    ensure_tracked(role.role_id);
    if (role.owner_epoch <= removed_epochs_.at(role.role_id)) throw Error(409, "removed owner epoch");
    if (occupied_count() >= config_.max_players) throw Error(503, "scene player capacity exhausted");
}

Scene::Entity& Scene::matching(const Json& request, bool active_only) {
    auto id = string_field(request, "role_id");
    auto owner = integer_field(request, "owner_epoch");
    auto session = integer_field(request, "session_epoch");
    auto it = entities_.find(id);
    if (it == entities_.end()) throw Error(404, "role not in scene");
    auto& entity = it->second;
    if (entity.role.owner_epoch != owner || entity.role.session_epoch != session)
        throw Error(409, "stale role reference");
    if (active_only && (entity.mode != Mode::Active || !entity.connected)) throw Error(409, "role is not active");
    return entity;
}

Json Scene::enter(const Json& request) {
    auto role = parse_role(request);
    auto it = entities_.find(role.role_id);
    if (it != entities_.end()) {
        auto& entity = it->second;
        if (entity.mode != Mode::Active) throw Error(409, "role is frozen or prepared");
        if (role.owner_epoch != entity.role.owner_epoch || role.session_epoch < entity.role.session_epoch)
            throw Error(409, "owner or session conflict");
        if (role.session_epoch == entity.role.session_epoch && !entity.connected) throw Error(409, "disconnected session; log in again");
        if (role.session_epoch > entity.role.session_epoch) {
            if ((!entity.connected || entity.lease_age>3) && occupied_count()>=config_.max_players) throw Error(503,"scene player capacity exhausted");
            entity.connected = true;
            clear_abilities(role.role_id);
            entity.role.session_epoch = role.session_epoch;
            entity.role.coins = role.coins;
            entity.role.name = role.name;
            entity.role.combat.action_session = role.session_epoch;
            entity.role.combat.last_action_seq = 0;
            changed(entity.role.combat);
            entity.last_action_key.clear();
            entity.last_action_result = Json();
            entity.last_seq = entity.applied_seq = 0;
            entity.dx = entity.dy = 0;
            entity.input_age = 1;
            entity.cancelled_transfers.clear();
            entity.transfer_id.clear();
        }
        entity.lease_age = 0;
        return {{"role", entity.role.json()}};
    }
    admit(role);
    Entity entity;
    entity.role = role;
    entities_.emplace(role.role_id, std::move(entity));
    return {{"role", role.json()}};
}

Json Scene::prepare(const Json& request) {
    auto role = parse_role(request);
    auto transfer = string_field(request, "transfer_id");
    auto it = entities_.find(role.role_id);
    if (it != entities_.end()) {
        const auto& entity = it->second;
        if (!same_ref(entity.role, role) || entity.transfer_id != transfer || entity.mode == Mode::Frozen || entity.cancelled_transfers.count(transfer))
            throw Error(409, "conflicting prepared role");
        return {{"role", entity.role.json()}};
    }
    admit(role);
    Entity entity;
    entity.role = role;
    entity.mode = Mode::Prepared;
    entity.transfer_id = transfer;
    entities_.emplace(role.role_id, std::move(entity));
    return {{"role", role.json()}};
}

Json Scene::config_status() const {
    return {{"config", combat_config_}, {"version", combat_config_["version"]},
        {"generation", config_generation_}, {"applied_tick", config_applied_tick_}};
}

Json Scene::reload_config(const Json& request, bool validate_only) {
    for (auto it = request.begin(); it != request.end(); ++it)
        if (it.key() != "config" && it.key() != "expected_generation") throw Error(400, "unexpected config request field");
    if (!request.contains("config")) throw Error(400, "complete config required");
    if (request.contains("expected_generation") &&
        integer_field(request, "expected_generation", true) != static_cast<std::int64_t>(config_generation_))
        throw Error(409, "config generation conflict");
    auto candidate = validate_config(request["config"]);
    const auto version = candidate["version"].get<std::string>();
    const auto previous = config_history_.find(version);
    if (previous != config_history_.end() && previous->second != candidate)
        throw Error(409, "config version already has different content");
    if (previous == config_history_.end() && config_history_.size() >= 64)
        throw Error(503, "config version history full; restart required");
    const bool change = candidate != combat_config_;
    if (change && config_generation_ >= 9007199254740991ULL) throw Error(503, "config generation exhausted");
    if (validate_only) return {{"valid", true}, {"config", candidate}, {"version", version},
        {"current_version", combat_config_["version"]}, {"generation", config_generation_},
        {"applied_tick", config_applied_tick_}, {"would_change", change}};
    if (change) {
        // 完整验证结束后才发布新配置。既有玩家、怪物和动作缓存保持原样。
        if (previous == config_history_.end()) config_history_.emplace(version, candidate);
        combat_config_.swap(candidate);
        ++config_generation_;
        config_applied_tick_ = tick_;
    }
    auto result = config_status();
    result["changed"] = change;
    return result;
}

Json Scene::execute(const std::string& path, const Json& request) {
    if (!request.is_object()) throw Error(400, "JSON object required");
    if (path == "/v1/config/status") {
        if (!request.empty()) throw Error(400, "config status takes an empty object");
        return config_status();
    }
    if (path == "/v1/config/reload" || path == "/v1/config/apply") return reload_config(request);
    if (path == "/v1/config/validate") return reload_config(request, true);
    if (path == "/v1/enter") return enter(request);
    if (path == "/v1/prepare") return prepare(request);
    if (path == "/v1/remove") {
        auto id = string_field(request, "role_id");
        auto owner = integer_field(request, "owner_epoch");
        auto session = integer_field(request, "session_epoch");
        auto it = entities_.find(id);
        if (it != entities_.end()) {
            if (it->second.role.owner_epoch != owner || it->second.role.session_epoch != session)
                throw Error(409, "stale role reference");
            if (request.contains("transfer_id")) {
                auto transfer = string_field(request, "transfer_id");
                if (!it->second.transfer_id.empty() && it->second.transfer_id != transfer)
                    throw Error(409, "transfer conflict");
            }
        }
        ensure_tracked(id);
        removed_epochs_[id] = std::max(removed_epochs_[id], owner);
        if (it != entities_.end()) { clear_abilities(id); entities_.erase(it); }
        return {{"accepted", true}};
    }

    if (path == "/v1/disconnect") {
        auto& entity = matching(request);
        if (entity.mode != Mode::Active) throw Error(409,"role is not active");
        entity.connected=false; entity.dx=entity.dy=0; entity.input_age=1;
        clear_abilities(entity.role.role_id);
        return {{"role",entity.role.json()}};
    }
    if (path == "/v1/move_batch") {
        if (!request.contains("inputs") || !request["inputs"].is_array() || request["inputs"].empty() || request["inputs"].size()>4) throw Error(400,"movement batch needs 1..4 inputs");
        Json results=Json::array();
        for (const auto& input : request["inputs"]) {
            if (!input.is_object()) {results.push_back({{"ok",false},{"error","invalid input"}});continue;}
            auto item=input;
            // 身份仅来自网关绑定的会话，不允许批内记录覆盖。
            for (auto key : {"role_id","owner_epoch","session_epoch"}) item[key]=request.at(key);
            try {results.push_back({{"ok",true},{"data",execute("/v1/move",item)}});}
            catch (const Error& error) {results.push_back({{"ok",false},{"error",error.what()}});}
            catch (const Json::exception&) {results.push_back({{"ok",false},{"error","invalid input fields"}});}
        }
        return {{"results",results}};
    }
    if (!config_.combat_enabled && (path=="/v1/cast" || path=="/v1/revive")) throw Error(409,"combat is disabled in this scene");
    auto& entity = matching(request, path == "/v1/move" || path == "/v1/view" || path == "/v1/cast" || path == "/v1/revive");
    if (path == "/v1/cast") return cast(entity, request);
    if (path == "/v1/revive") return revive(entity, request);
    if (path == "/v1/move") {
        if (config_.combat_enabled && entity.role.combat.dead) {
            if (config_.abilities_enabled) return {{"accepted",false},{"reason","dead"}};
            throw Error(409, "dead players cannot move; revive first");
        }
        auto seq = integer_field(request, "seq");
        double dx = real_field(request, "dx", 1), dy = real_field(request, "dy", 1);
        if (seq <= entity.last_seq) throw Error(409, "input sequence must increase");
        double length = std::hypot(dx, dy);
        if (length > 1) { dx /= length; dy /= length; }
        entity.last_seq = seq;
        entity.trace_id = request.contains("trace_id") && request["trace_id"].is_string() ? request["trace_id"].get<std::string>().substr(0,64) : "";
        entity.dx = dx;
        entity.dy = dy;
        entity.input_age = 0;
        entity.lease_age = 0;
        return {{"accepted", true}};
    }
    if (path == "/v1/view") {
        entity.lease_age = 0;
        return view(entity);
    }
    if (path == "/v1/freeze") {
        auto transfer = string_field(request, "transfer_id");
        if (entity.cancelled_transfers.count(transfer)) throw Error(409, "transfer was cancelled");
        if (entity.mode == Mode::Active && entity.cancelled_transfers.size() >= 256)
            throw Error(503, "transfer history capacity exhausted; reconnect required");
        if (entity.mode == Mode::Prepared || (entity.mode == Mode::Frozen && entity.transfer_id != transfer))
            throw Error(409, "transfer conflict");
        entity.mode = Mode::Frozen;
        clear_abilities(entity.role.role_id);
        entity.transfer_id = transfer;
        entity.dx = entity.dy = 0;
        return {{"role", entity.role.json()}};
    }
    if (path == "/v1/unfreeze") {
        auto transfer = string_field(request, "transfer_id");
        if (entity.mode == Mode::Prepared || (entity.mode == Mode::Frozen && entity.transfer_id != transfer))
            throw Error(409, "transfer conflict");
        // 达到上限后 freeze 已统一拒绝，因此无需无限保存新的取消标记。
        if (entity.cancelled_transfers.size() < 256) entity.cancelled_transfers.insert(transfer);
        entity.mode = Mode::Active;
        entity.dx = entity.dy = 0;
        entity.input_age = 1;
        entity.lease_age = 0;
        return {{"accepted", true}};
    }
    if (path == "/v1/activate") {
        auto transfer = string_field(request, "transfer_id");
        if (entity.transfer_id != transfer || entity.mode == Mode::Frozen || entity.cancelled_transfers.count(transfer))
            throw Error(409, "transfer conflict");
        entity.mode = Mode::Active;
        entity.lease_age = 0;
        return {{"role", entity.role.json()}};
    }
    throw Error(404, "unknown scene endpoint");
}

void Scene::step(double dt) {
    if (!std::isfinite(dt) || dt <= 0 || dt > 1) throw std::invalid_argument("invalid step duration");
    ++tick_;
    for (auto& pair : entities_) {
        auto& entity = pair.second;
        if (entity.mode != Mode::Active) continue;
        entity.lease_age = std::min(4.0, entity.lease_age + dt);
        if (config_.combat_enabled) step_combat(entity, dt);
        // 输入失联后最多继续移动 250ms，避免网关断开留下无限移动角色。
        double elapsed = entity.connected ? std::min(dt, std::max(0.0, 0.25 - entity.input_age)) : 0;
        const auto position = wall_move(entity.role.x, entity.role.y,
            std::clamp(entity.role.x + entity.dx * config_.move_speed * elapsed, -100.0, 100.0),
            std::clamp(entity.role.y + entity.dy * config_.move_speed * elapsed, -100.0, 100.0), 0.45);
        entity.role.x = position.first; entity.role.y = position.second;
        entity.input_age = std::min(1.0, entity.input_age + dt);
        if (entity.last_seq!=entity.applied_seq) {
            entity.applied_seq=entity.last_seq;
            if (config_.trace_movement) std::cout<<Json{{"phase","direction_applied"},{"id",entity.trace_id},{"seq",entity.last_seq},{"tick",tick_},{"scene_id",config_.scene_id},{"x",entity.role.x},{"y",entity.role.y}}.dump()<<std::endl;
        }
    }
    if (config_.combat_enabled) step_npcs(dt);
    if (config_.abilities_enabled) step_abilities(dt);
}

Json Scene::view(Entity& entity) {
    Json players = Json::array(), mobs = Json::array(), events = Json::array(), catalog = Json::array();
    const double radius_squared = config_.aoi_radius * config_.aoi_radius;
    auto visible = [&](double x, double y) {
        double dx = x - entity.role.x, dy = y - entity.role.y;
        return dx * dx + dy * dy <= radius_squared;
    };
    for (const auto& pair : npcs_) if (visible(pair.second.x, pair.second.y)) mobs.push_back(pair.second.json());
    for (const auto& event : events_) {
        if (visible(event["x"].get<double>(), event["y"].get<double>()) ||
            event["source_id"] == entity.role.role_id || event["target_id"] == entity.role.role_id)
            events.push_back(event);
    }
    for (const auto& skill : skills) catalog.push_back(configured_skill(skill, combat_config_).json());
    Json result{{"self", entity.role.json()}, {"players", players}, {"tick", tick_},
        {"scene_id", config_.scene_id}, {"mobs", mobs}, {"skills", catalog}, {"events", events},
        {"config_version", combat_config_["version"]}, {"config_generation", config_generation_},
        {"config_applied_tick", config_applied_tick_}};
    if (!config_.combat_enabled) {
        result["self"].erase("combat"); result.erase("mobs"); result.erase("skills"); result.erase("events");
    }
    if (config_.abilities_enabled) ability_view(result, entity);
    // 为自身和元数据预留空间；极端长名字、满员 AOI 也不超过客户端 64KiB 帧上限。
    std::size_t used = result.dump(-1, ' ', true).size() + entity.role.json(false).dump(-1, ' ', true).size() + 1;
    auto self_summary=entity.role.json(false);
    if (!config_.combat_enabled) self_summary.erase("combat");
    players.push_back(std::move(self_summary));
    std::size_t visible_count = 1;
    bool truncated = false;
    for (const auto& pair : entities_) {
        const auto& other = pair.second;
        if (other.role.role_id==entity.role.role_id || other.mode != Mode::Active || !other.connected || other.lease_age>3 || !visible(other.role.x, other.role.y)) continue;
        ++visible_count;
        auto summary = other.role.json(false);
        if (!config_.combat_enabled) summary.erase("combat");
        if (players.size()>=128) {truncated=true;continue;}
        if (other.role.role_id != entity.role.role_id) {
            const auto size = summary.dump(-1, ' ', true).size() + 1;
            if (used + size > 60 * 1024) { truncated = true; continue; }
            used += size;
        }
        players.push_back(std::move(summary));
    }
    result["players"] = std::move(players);
    result["players_truncated"] = truncated;
    result["visible_player_count"] = visible_count;
    return result;
}

void Scene::emit(const std::string& type, const std::string& source, const std::string& target,
    const std::string& skill, int value, double x, double y, const std::string& object_id) {
    events_.push_back({{"id", ++next_event_id_}, {"type", type}, {"source_id", source},
        {"target_id", target}, {"skill_id", skill}, {"value", value}, {"x", x}, {"y", y}, {"tick", tick_}});
    if (!object_id.empty()) events_.back()["object_id"] = object_id;
    while (events_.size() > 32) events_.pop_front();
}

void Scene::damage_npc(Entity& entity, Npc& npc, const std::string& skill, int amount, Json& hits) {
    int damage = std::min(npc.hp, amount);
    npc.hp -= damage;
    npc.target_id = entity.role.role_id;
    emit("damage", entity.role.role_id, npc.id, skill, damage, npc.x, npc.y);
    if (npc.hp == 0) {
        npc.target_id.clear();
        npc.respawn_remaining = npc.respawn_period;
        emit("death", entity.role.role_id, npc.id, skill, 0, npc.x, npc.y);
    }
    hits.push_back({{"target_id", npc.id}, {"damage", damage}, {"hp", npc.hp},
        {"max_hp", npc.max_hp}, {"dead", npc.hp == 0}});
}

Json Scene::cast(Entity& entity, const Json& request) {
    if (config_.abilities_enabled) return cast_ability(entity, request);
    auto seq = integer_field(request, "seq");
    auto skill_id = string_field(request, "skill_id", 32);
    std::string target;
    if (request.contains("target_id")) {
        if (!request["target_id"].is_string()) throw Error(400, "invalid target_id");
        target = request["target_id"].get<std::string>();
        if (target.size() > 128 || target.find('\0') != std::string::npos) throw Error(400, "invalid target_id");
    }
    auto& combat = entity.role.combat;
    const auto key = "cast:" + skill_id + ":" + target;
    if (seq <= combat.last_action_seq) {
        if (seq == combat.last_action_seq && key == entity.last_action_key && !entity.last_action_result.is_null())
            return entity.last_action_result;
        throw Error(409, "old action sequence; action was not repeated");
    }
    if (combat.dead) throw Error(409, "dead players cannot cast; revive first");
    auto found = std::find_if(skills.begin(), skills.end(), [&](const Skill& skill) { return skill_id == skill.id; });
    if (found == skills.end()) throw Error(400, "unknown skill");
    const auto skill = configured_skill(*found, combat_config_);
    Npc* npc = nullptr;
    if (std::string(skill.target) == "enemy") {
        auto it = npcs_.find(target);
        if (it == npcs_.end()) throw Error(404, "target is not a scene monster");
        npc = &it->second;
        if (npc->hp == 0) throw Error(409, "target is dead");
        if (std::hypot(npc->x - entity.role.x, npc->y - entity.role.y) > skill.range)
            throw Error(409, "target is out of skill range");
    } else if (!target.empty() && target != entity.role.role_id) {
        throw Error(400, "this skill targets self");
    }
    if (combat.cooldowns.at(skill_id) > 1e-7) throw Error(409, "skill is cooling down");
    if (combat.mp + 1e-7 < skill.mp_cost) throw Error(409, "not enough mana");

    // 完成全部验证后才消耗资源，重复或非法请求不改变战斗结算。
    entity.lease_age = 0;
    combat.mp -= skill.mp_cost;
    combat.cooldowns[skill_id] = skill.cooldown;
    combat.last_action_seq = seq;
    changed(combat);
    if (skill_id == "charge") {
        double distance = std::hypot(npc->x - entity.role.x, npc->y - entity.role.y);
        if (distance > 1.5) {
            entity.role.x = std::clamp(npc->x + (entity.role.x - npc->x) * 1.5 / distance, -100.0, 100.0);
            entity.role.y = std::clamp(npc->y + (entity.role.y - npc->y) * 1.5 / distance, -100.0, 100.0);
        }
        npc->stun_remaining = std::max(npc->stun_remaining, 1.0);
        entity.dx = entity.dy = 0;
    }
    emit("cast", entity.role.role_id, target, skill_id, 0, entity.role.x, entity.role.y);
    Json hits = Json::array();
    int healed = 0;
    if (npc) damage_npc(entity, *npc, skill_id, skill.damage, hits);
    else if (skill_id == "whirlwind") {
        for (auto& pair : npcs_) {
            auto& other = pair.second;
            if (other.hp > 0 && std::hypot(other.x - entity.role.x, other.y - entity.role.y) <= skill.range)
                damage_npc(entity, other, skill_id, skill.damage, hits);
        }
    } else {
        healed = std::min(180 - combat.hp, skill.heal);
        combat.hp += healed;
        emit("heal", entity.role.role_id, entity.role.role_id, skill_id, healed, entity.role.x, entity.role.y);
    }
    Json result{{"accepted", true}, {"seq", seq}, {"skill_id", skill_id}, {"target_id", target},
        {"hits", hits}, {"healed", healed}, {"role", entity.role.json()}, {"tick", tick_},
        {"config_version", combat_config_["version"]}};
    entity.last_action_key = key;
    entity.last_action_result = result;
    return result;
}

Json Scene::revive(Entity& entity, const Json& request) {
    auto seq = integer_field(request, "seq");
    auto& combat = entity.role.combat;
    auto reject = [&](const char* reason) -> Json {
        return {{"accepted",false},{"reason",reason},{"seq",seq},{"role",entity.role.json()},
            {"retry_after_ms",milliseconds(combat.revive_remaining)},{"tick",tick_},{"world_epoch",world_epoch_}};
    };
    if (seq <= combat.last_action_seq) {
        if (seq == combat.last_action_seq && entity.last_action_key == "revive" && !entity.last_action_result.is_null())
            return entity.last_action_result;
        if (config_.abilities_enabled) return reject("stale_action_seq");
        throw Error(409, "old action sequence; action was not repeated");
    }
    if (!combat.dead) {if (config_.abilities_enabled) return reject("not_dead");throw Error(409, "player is not dead");}
    if (combat.revive_remaining > 1e-7) {if (config_.abilities_enabled) return reject("revive_not_ready");throw Error(409, "revive is not ready");}
    combat.hp = 180; combat.mp = 100; combat.dead = false; combat.revive_remaining = 0;
    combat.last_action_seq = seq; changed(combat);
    entity.role.x = entity.role.y = 0;
    entity.dx = entity.dy = 0; entity.input_age = 1; entity.lease_age = 0;
    emit("revive", entity.role.role_id, entity.role.role_id, "", 180, 0, 0);
    Json result{{"accepted", true}, {"seq", seq}, {"role", entity.role.json()}, {"tick", tick_}};
    if (config_.abilities_enabled) result["world_epoch"] = world_epoch_;
    entity.last_action_key = "revive"; entity.last_action_result = result;
    return result;
}

void Scene::step_combat(Entity& entity, double dt) {
    auto& combat = entity.role.combat;
    bool updated = false;
    for (auto& pair : combat.cooldowns) {
        double next = std::max(0.0, pair.second - dt);
        if (next < 1e-7) next = 0;
        if (next != pair.second) { pair.second = next; updated = true; }
    }
    if (combat.dead) {
        double next = std::max(0.0, combat.revive_remaining - dt);
        if (next < 1e-7) next = 0;
        if (next != combat.revive_remaining) { combat.revive_remaining = next; updated = true; }
        entity.dx = entity.dy = 0;
    } else {
        double next = std::min(100.0, combat.mp + 6 * dt);
        if (next != combat.mp) { combat.mp = next; updated = true; }
    }
    if (updated) changed(combat);
}

void Scene::damage_player(Npc& npc, Entity& entity) {
    auto& combat = entity.role.combat;
    int damage = std::min(combat.hp, npc.damage);
    combat.hp -= damage; changed(combat);
    emit("damage", npc.id, entity.role.role_id, "npc_attack", damage, entity.role.x, entity.role.y);
    if (combat.hp == 0) {
        combat.dead = true; combat.revive_remaining = 5;
        entity.dx = entity.dy = 0;
        emit("death", npc.id, entity.role.role_id, "npc_attack", 0, entity.role.x, entity.role.y);
    }
}

void Scene::reset_npc(Npc& npc) {
    const auto& parameters = combat_config_["monsters"][npc.kind];
    npc.max_hp = parameters["max_hp"].get<int>();
    npc.damage = parameters["damage"].get<int>();
    npc.speed = parameters["move_speed"].get<double>();
    npc.attack_period = parameters["attack_interval_ms"].get<double>() / 1000;
    npc.respawn_period = parameters["respawn_ms"].get<double>() / 1000;
    npc.config_version = combat_config_["version"].get<std::string>();
    npc.x = npc.spawn_x; npc.y = npc.spawn_y; npc.hp = npc.max_hp;
    npc.target_id.clear(); npc.stun_remaining = npc.attack_remaining = npc.respawn_remaining = 0;
}

void Scene::step_npcs(double dt) {
    for (auto& pair : npcs_) {
        auto& npc = pair.second;
        if (npc.hp == 0) {
            npc.respawn_remaining = std::max(0.0, npc.respawn_remaining - dt);
            if (npc.respawn_remaining < 1e-7) {
                reset_npc(npc);
                emit("respawn", npc.id, npc.id, "", 0, npc.x, npc.y);
            }
            continue;
        }
        npc.stun_remaining = std::max(0.0, npc.stun_remaining - dt);
        npc.attack_remaining = std::max(0.0, npc.attack_remaining - dt);
        if (npc.target_id.empty()) continue;
        auto target = entities_.find(npc.target_id);
        if (target == entities_.end() || target->second.mode != Mode::Active ||
            target->second.role.combat.dead || !target->second.connected || target->second.lease_age > 3 ||
            std::hypot(npc.x - npc.spawn_x, npc.y - npc.spawn_y) > 24 ||
            std::hypot(npc.x - target->second.role.x, npc.y - target->second.role.y) > 24) {
            reset_npc(npc);
            emit("reset", npc.id, npc.id, "", 0, npc.x, npc.y);
            continue;
        }
        if (npc.stun_remaining > 1e-7) continue;
        auto& entity = target->second;
        double distance = std::hypot(entity.role.x - npc.x, entity.role.y - npc.y);
        if (distance > 2) {
            double travel = std::min(npc.speed * dt, distance - 1.5);
            const auto position = wall_move(npc.x, npc.y, npc.x + (entity.role.x - npc.x) * travel / distance,
                npc.y + (entity.role.y - npc.y) * travel / distance, 0.6);
            npc.x = position.first; npc.y = position.second;
        }
        const auto clear = wall_move(npc.x, npc.y, entity.role.x, entity.role.y, 0);
        if (std::hypot(entity.role.x - npc.x, entity.role.y - npc.y) <= 2 && npc.attack_remaining <= 1e-7 &&
            std::hypot(clear.first-entity.role.x, clear.second-entity.role.y)<1e-6) {
            damage_player(npc, entity);
            npc.attack_remaining = npc.attack_period;
        }
    }
}
} // namespace mmo
