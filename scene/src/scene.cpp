#include "scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
        if (remaining > milliseconds(skill.cooldown)) throw Error(400, "invalid skill cooldown");
        combat.cooldowns[skill.id] = static_cast<double>(remaining) / 1000;
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
        {"respawn_remaining_ms", milliseconds(respawn_remaining)}, {"target_id", target_id}};
}

Scene::Scene(Config config) : config_(std::move(config)) {
    const std::array<std::pair<double, double>, 4> spawns{{{-8, 4}, {10, 4}, {6, -10}, {18, 16}}};
    for (std::size_t i = 0; i < spawns.size(); ++i) {
        Npc npc;
        npc.id = "npc-" + std::to_string(i + 1);
        npc.name = i == 3 ? "试炼守卫" : "荒野狼";
        npc.kind = i == 3 ? "elite" : "wolf";
        npc.x = npc.spawn_x = spawns[i].first;
        npc.y = npc.spawn_y = spawns[i].second;
        if (i == 3) {
            npc.hp = npc.max_hp = 240; npc.damage = 14;
            npc.speed = 2.4; npc.attack_period = 1.8;
        }
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
    if (role.scene_id != config_.scene_id) throw Error(409, "scene mismatch");
    return role;
}

void Scene::ensure_tracked(const std::string& role_id) {
    if (!removed_epochs_.count(role_id)) {
        if (removed_epochs_.size() >= config_.max_tracked_roles) throw Error(503, "scene identity capacity exhausted");
        removed_epochs_.emplace(role_id, 0);
    }
}

void Scene::admit(const Role& role) {
    ensure_tracked(role.role_id);
    if (role.owner_epoch <= removed_epochs_.at(role.role_id)) throw Error(409, "removed owner epoch");
    if (entities_.size() >= config_.max_players) throw Error(503, "scene player capacity exhausted");
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
    if (active_only && entity.mode != Mode::Active) throw Error(409, "role is not active");
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
        if (role.session_epoch > entity.role.session_epoch) {
            entity.role.session_epoch = role.session_epoch;
            entity.role.coins = role.coins;
            entity.role.name = role.name;
            entity.role.combat.action_session = role.session_epoch;
            entity.role.combat.last_action_seq = 0;
            changed(entity.role.combat);
            entity.last_action_key.clear();
            entity.last_action_result = Json();
            entity.last_seq = 0;
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

Json Scene::execute(const std::string& path, const Json& request) {
    if (!request.is_object()) throw Error(400, "JSON object required");
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
        if (it != entities_.end()) entities_.erase(it);
        return {{"accepted", true}};
    }

    auto& entity = matching(request, path == "/v1/move" || path == "/v1/view" || path == "/v1/cast" || path == "/v1/revive");
    if (path == "/v1/cast") return cast(entity, request);
    if (path == "/v1/revive") return revive(entity, request);
    if (path == "/v1/move") {
        if (entity.role.combat.dead) throw Error(409, "dead players cannot move; revive first");
        auto seq = integer_field(request, "seq");
        double dx = real_field(request, "dx", 1), dy = real_field(request, "dy", 1);
        if (seq <= entity.last_seq) throw Error(409, "input sequence must increase");
        double length = std::hypot(dx, dy);
        if (length > 1) { dx /= length; dy /= length; }
        entity.last_seq = seq;
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
        step_combat(entity, dt);
        // 输入失联后最多继续移动 250ms，避免网关断开留下无限移动角色。
        double elapsed = std::min(dt, std::max(0.0, 0.25 - entity.input_age));
        entity.role.x = std::clamp(entity.role.x + entity.dx * config_.move_speed * elapsed, -100.0, 100.0);
        entity.role.y = std::clamp(entity.role.y + entity.dy * config_.move_speed * elapsed, -100.0, 100.0);
        entity.input_age = std::min(1.0, entity.input_age + dt);
    }
    step_npcs(dt);
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
    for (const auto& skill : skills) catalog.push_back(skill.json());
    Json result{{"self", entity.role.json()}, {"players", players}, {"tick", tick_},
        {"scene_id", config_.scene_id}, {"mobs", mobs}, {"skills", catalog}, {"events", events}};
    // 为自身和元数据预留空间；极端长名字、满员 AOI 也不超过客户端 64KiB 帧上限。
    std::size_t used = result.dump(-1, ' ', true).size() + entity.role.json(false).dump(-1, ' ', true).size() + 1;
    std::size_t visible_count = 0;
    bool truncated = false;
    for (const auto& pair : entities_) {
        const auto& other = pair.second;
        if (other.mode != Mode::Active || !visible(other.role.x, other.role.y)) continue;
        ++visible_count;
        auto summary = other.role.json(false);
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
    const std::string& skill, int value, double x, double y) {
    events_.push_back({{"id", ++next_event_id_}, {"type", type}, {"source_id", source},
        {"target_id", target}, {"skill_id", skill}, {"value", value}, {"x", x}, {"y", y}, {"tick", tick_}});
    while (events_.size() > 32) events_.pop_front();
}

void Scene::damage_npc(Entity& entity, Npc& npc, const std::string& skill, int amount, Json& hits) {
    int damage = std::min(npc.hp, amount);
    npc.hp -= damage;
    npc.target_id = entity.role.role_id;
    emit("damage", entity.role.role_id, npc.id, skill, damage, npc.x, npc.y);
    if (npc.hp == 0) {
        npc.target_id.clear();
        npc.respawn_remaining = 8;
        emit("death", entity.role.role_id, npc.id, skill, 0, npc.x, npc.y);
    }
    hits.push_back({{"target_id", npc.id}, {"damage", damage}, {"hp", npc.hp},
        {"max_hp", npc.max_hp}, {"dead", npc.hp == 0}});
}

Json Scene::cast(Entity& entity, const Json& request) {
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
    const auto& skill = *found;
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
        {"hits", hits}, {"healed", healed}, {"role", entity.role.json()}, {"tick", tick_}};
    entity.last_action_key = key;
    entity.last_action_result = result;
    return result;
}

Json Scene::revive(Entity& entity, const Json& request) {
    auto seq = integer_field(request, "seq");
    auto& combat = entity.role.combat;
    if (seq <= combat.last_action_seq) {
        if (seq == combat.last_action_seq && entity.last_action_key == "revive" && !entity.last_action_result.is_null())
            return entity.last_action_result;
        throw Error(409, "old action sequence; action was not repeated");
    }
    if (!combat.dead) throw Error(409, "player is not dead");
    if (combat.revive_remaining > 1e-7) throw Error(409, "revive is not ready");
    combat.hp = 180; combat.mp = 100; combat.dead = false; combat.revive_remaining = 0;
    combat.last_action_seq = seq; changed(combat);
    entity.role.x = entity.role.y = 0;
    entity.dx = entity.dy = 0; entity.input_age = 1; entity.lease_age = 0;
    emit("revive", entity.role.role_id, entity.role.role_id, "", 180, 0, 0);
    Json result{{"accepted", true}, {"seq", seq}, {"role", entity.role.json()}, {"tick", tick_}};
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
            target->second.role.combat.dead || target->second.lease_age > 3 ||
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
            npc.x += (entity.role.x - npc.x) * travel / distance;
            npc.y += (entity.role.y - npc.y) * travel / distance;
        }
        if (std::hypot(entity.role.x - npc.x, entity.role.y - npc.y) <= 2 && npc.attack_remaining <= 1e-7) {
            damage_player(npc, entity);
            npc.attack_remaining = npc.attack_period;
        }
    }
}
} // namespace mmo
