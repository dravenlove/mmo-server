#include "scene.hpp"

#include <algorithm>
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
} // namespace

Json Role::json() const {
    return {{"role_id", role_id}, {"name", name}, {"scene_id", scene_id},
            {"owner_epoch", owner_epoch}, {"session_epoch", session_epoch},
            {"x", x}, {"y", y}, {"coins", coins}};
}

Scene::Scene(Config config) : config_(std::move(config)) {}

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
            entity.last_seq = 0;
            entity.dx = entity.dy = 0;
            entity.input_age = 1;
            entity.cancelled_transfers.clear();
            entity.transfer_id.clear();
        }
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

    auto& entity = matching(request, path == "/v1/move" || path == "/v1/view");
    if (path == "/v1/move") {
        auto seq = integer_field(request, "seq");
        double dx = real_field(request, "dx", 1), dy = real_field(request, "dy", 1);
        if (seq <= entity.last_seq) throw Error(409, "input sequence must increase");
        double length = std::hypot(dx, dy);
        if (length > 1) { dx /= length; dy /= length; }
        entity.last_seq = seq;
        entity.dx = dx;
        entity.dy = dy;
        entity.input_age = 0;
        return {{"accepted", true}};
    }
    if (path == "/v1/view") {
        Json players = Json::array();
        const double radius_squared = config_.aoi_radius * config_.aoi_radius;
        for (const auto& pair : entities_) {
            const auto& other = pair.second;
            double dx = other.role.x - entity.role.x, dy = other.role.y - entity.role.y;
            if (other.mode == Mode::Active && dx * dx + dy * dy <= radius_squared)
                players.push_back(other.role.json());
        }
        return {{"self", entity.role.json()}, {"players", players}, {"tick", tick_}, {"scene_id", config_.scene_id}};
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
        return {{"accepted", true}};
    }
    if (path == "/v1/activate") {
        auto transfer = string_field(request, "transfer_id");
        if (entity.transfer_id != transfer || entity.mode == Mode::Frozen || entity.cancelled_transfers.count(transfer))
            throw Error(409, "transfer conflict");
        entity.mode = Mode::Active;
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
        // 输入失联后最多继续移动 250ms，避免网关断开留下无限移动角色。
        double elapsed = std::min(dt, std::max(0.0, 0.25 - entity.input_age));
        entity.role.x = std::clamp(entity.role.x + entity.dx * config_.move_speed * elapsed, -100.0, 100.0);
        entity.role.y = std::clamp(entity.role.y + entity.dy * config_.move_speed * elapsed, -100.0, 100.0);
        entity.input_age = std::min(1.0, entity.input_age + dt);
    }
}
} // namespace mmo
