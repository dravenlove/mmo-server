#include "room/worker.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace native::room {
namespace {
bool identifier(const std::string& value) {
    return !value.empty() && value.size() <= 64 && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == std::string::npos &&
           value.find_first_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == 0;
}
std::string text(const Json& object, const char* key, const std::string& fallback = "") {
    if (!object.contains(key)) return fallback;
    if (!object[key].is_string()) throw Error(400, std::string("invalid ") + key);
    return object[key].get<std::string>();
}
std::int64_t integer(const Json& object, const char* key, std::int64_t fallback = 0) {
    if (!object.contains(key)) return fallback;
    if (!object[key].is_number_integer() || (object[key].is_number_unsigned() && object[key].get<std::uint64_t>() > INT64_MAX))
        throw Error(400, std::string("invalid ") + key);
    return object[key].get<std::int64_t>();
}
void fields(const Json& object, std::initializer_list<const char*> allowed) {
    if (!object.is_object()) throw Error(400, "JSON object required");
    std::set<std::string> keys(allowed.begin(), allowed.end());
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!keys.count(it.key())) throw Error(400, "unknown room field");
}
std::string bytes(const std::filesystem::path& path, std::size_t maximum) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) throw Error(400, "trusted room file must be regular");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw Error(400, "cannot read trusted room file");
    std::string value(maximum + 1, '\0');
    file.read(value.data(), static_cast<std::streamsize>(value.size()));
    if (file.gcount() > static_cast<std::streamsize>(maximum)) throw Error(400, "trusted room file exceeds limit");
    if (file.bad() || (file.fail() && !file.eof())) throw Error(400, "cannot read trusted room file");
    value.resize(static_cast<std::size_t>(file.gcount()));
    return value;
}
std::filesystem::path within(const std::string& root, const std::filesystem::path& requested) {
    auto base = std::filesystem::canonical(root);
    auto candidate = std::filesystem::canonical(requested);
    auto a = base.begin(), b = candidate.begin();
    for (; a != base.end(); ++a, ++b)
        if (b == candidate.end() || *a != *b) throw Error(400, "script path escapes trusted root");
    if (b == candidate.end()) throw Error(400, "script source must be a file");
    return candidate;
}
mmo::Config room_config(mmo::Config config, const std::string& id, const std::string& profile, std::size_t capacity) {
    config.scene_id = id;
    config.max_players = capacity;
    config.max_tracked_roles = 1024;
    config.combat_enabled = profile != "movement";
    config.abilities_enabled = profile == "abilities" || profile == "skill_lab";
    config.skill_lab_enabled = profile == "skill_lab";
    config.elemental_enabled = false;
    return config;
}
std::string profile_of(const mmo::Config& config) {
    return config.skill_lab_enabled ? "skill_lab" : config.elemental_enabled ? "elemental" : config.abilities_enabled ? "abilities" : config.combat_enabled ? "combat" : "movement";
}
bool live(const Json& state) {
    return state.value("mode", std::string()) == "active" && state.value("connected", false) && !state.value("lease_expired", false);
}
}

Json strict_object(const std::string& raw) {
    std::vector<std::set<std::string>> stack;
    bool invalid = false;
    try {
        auto result = Json::parse(raw, [&](int depth, Json::parse_event_t event, Json& value) {
            if (depth > 16) invalid = true;
            if (event == Json::parse_event_t::object_start) stack.emplace_back();
            if (event == Json::parse_event_t::key && !stack.back().insert(value.get<std::string>()).second) invalid = true;
            if (event == Json::parse_event_t::object_end) stack.pop_back();
            return true;
        });
        if (!result.is_object() || invalid) throw Error(400, "JSON object without duplicate keys required");
        return result;
    } catch (const Json::exception&) { throw Error(400, "valid finite JSON object required"); }
}
Json read_json_file(const std::string& path, std::size_t maximum) { return strict_object(bytes(path, maximum)); }

ScriptCatalog::ScriptCatalog(std::string manifest) : manifest_(std::move(manifest)) {
    if (manifest_.empty()) throw Error(400, "ROOM_SCRIPT_MANIFEST is required");
    try { root_ = std::filesystem::canonical(std::filesystem::path(manifest_).parent_path().empty() ? "." : std::filesystem::path(manifest_).parent_path()).string(); }
    catch (...) { throw Error(400, "trusted script root is unavailable"); }
    reload(Json::object());
}
std::shared_ptr<const script::Script> ScriptCatalog::read(const std::string& manifest, const std::string& expected) const {
    try {
        auto info = strict_object(bytes(within(root_, manifest), 8192));
        fields(info, {"version", "file", "sha256"});
        auto version = text(info, "version"), file = text(info, "file"), digest = text(info, "sha256");
        if (!identifier(version) || (!expected.empty() && expected != version)) throw Error(400, "script version mismatch");
        if (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != std::string::npos || file.empty()) throw Error(400, "invalid script manifest");
        if (std::filesystem::path(file).is_absolute()) throw Error(400, "script source path must be relative");
        auto source = bytes(within(root_, std::filesystem::path(root_) / file), 64 * 1024);
        auto program = std::make_shared<const script::Script>(version, source, script::Limits{});
        if (program->sha256() != digest) throw Error(400, "script checksum mismatch");
        return program;
    } catch (const Error&) { throw; }
    catch (...) { throw Error(400, "trusted script validation failed"); }
}
std::shared_ptr<const script::Script> ScriptCatalog::resolve(const std::string& version) {
    const auto wanted = version.empty() ? active_ : version;
    if (!identifier(wanted)) throw Error(400, "invalid script version");
    if (auto found = versions_.find(wanted); found != versions_.end()) return found->second;
    if (versions_.size() >= 32) throw Error(503, "script version capacity exhausted");
    std::shared_ptr<const script::Script> candidate;
    try { candidate = read((std::filesystem::path(root_) / "versions" / (wanted + ".json")).string(), wanted); }
    catch (...) { throw Error(409, "pinned script version unavailable"); }
    versions_.emplace(wanted, candidate);
    return candidate;
}
Json ScriptCatalog::reload(const Json& request) {
    fields(request, {"expected_generation"});
    if (request.contains("expected_generation") && integer(request, "expected_generation") != static_cast<std::int64_t>(generation_)) throw Error(409, "script generation conflict");
    auto candidate = read(manifest_);
    auto old = versions_.find(candidate->version());
    if (old != versions_.end() && old->second->sha256() != candidate->sha256()) throw Error(409, "script version already has different source");
    if (old == versions_.end() && versions_.size() >= 32) throw Error(503, "script version capacity exhausted");
    const bool changed = active_ != candidate->version();
    if (changed && generation_ >= 9007199254740991ULL) throw Error(503, "script generation exhausted");
    // 完整源码、hash及sandbox验证通过后才改变active，旧Room持有原shared_ptr。
    if (old == versions_.end()) versions_.emplace(candidate->version(), candidate);
    active_ = candidate->version();
    if (changed) ++generation_;
    auto out = status(); out["changed"] = changed; return out;
}
Json ScriptCatalog::status() const {
    Json versions = Json::array();
    for (const auto& item : versions_) versions.push_back({{"version", item.first}, {"sha256", item.second->sha256()}});
    return {{"script_version", active_}, {"script_sha256", versions_.at(active_)->sha256()}, {"generation", generation_}, {"versions", versions}};
}

RoomInstance::RoomInstance(mmo::Config config, std::string host, std::string profile, std::size_t capacity,
                           bool legacy, std::shared_ptr<const script::Script> program)
    : config_(std::move(config)), host_(std::move(host)), profile_(std::move(profile)), state_(legacy ? "playing" : "lobby"),
      capacity_(capacity), legacy_(legacy), script_(std::move(program)), scene_([&] {
          config_.ability_planner = [program = script_](const Json& context) { return program->plan(context); };
          return config_;
      }()) {}
std::size_t RoomInstance::occupants() const { return scene_.role_count(); }
void RoomInstance::sync() {
    std::set<std::string> present;
    for (const auto& item : scene_.role_states(false)) {
        const auto& role = item.at("role");
        auto id = role.at("role_id").get<std::string>(); present.insert(id);
        auto& ready = ready_[id];
        const auto owner = role.at("owner_epoch").get<std::int64_t>(), session = role.at("session_epoch").get<std::int64_t>();
        const auto fingerprint = Json{{"owner", owner}, {"session", session}, {"name", role.at("name")}, {"mode", item.at("mode")},
                                      {"connected", item.at("connected")}, {"lease_expired", item.at("lease_expired")}}.dump();
        bool changed = ready.fingerprint != fingerprint;
        if (ready.owner != owner || ready.session != session) { ready.owner = owner; ready.session = session; ready.ready = false; changed = true; }
        if (!live(item) && ready.ready) { ready.ready = false; changed = true; }
        ready.fingerprint = fingerprint;
        if (changed) ++revision_;
    }
    for (auto it = ready_.begin(); it != ready_.end();) {
        if (!present.count(it->first)) { it = ready_.erase(it); ++revision_; } else ++it;
    }
}
Json RoomInstance::metadata(bool members) const {
    Json list = Json::array();
    if (members) for (const auto& item : scene_.role_states(false)) {
        const auto& r = item.at("role"); auto id = r.at("role_id").get<std::string>();
        auto ready = ready_.find(id);
        list.push_back({{"role_id", id}, {"name", r.at("name")}, {"owner_epoch", r.at("owner_epoch")}, {"session_epoch", r.at("session_epoch")},
                        {"mode", item.at("mode")}, {"connected", item.at("connected")}, {"lease_expired", item.at("lease_expired")},
                        {"ready", ready != ready_.end() && ready->second.ready && live(item)}});
    }
    Json result{{"room_id", id()}, {"scene_id", id()}, {"profile", profile_}, {"state", state_}, {"host_role_id", host_},
                {"capacity", capacity_}, {"member_count", occupants()}, {"script_version", script_->version()}, {"script_sha256", script_->sha256()},
                {"world_epoch", scene_.world_epoch()}, {"revision", revision_}, {"tick", scene_.tick()}, {"legacy", legacy_}};
    if (members) result["members"] = std::move(list);
    return result;
}
Json RoomInstance::checked_role(const Json& request, bool heartbeat) {
    auto current = scene_.role_state(text(request, "role_id"), false);
    if (!current) throw Error(404, "role not in scene");
    const auto owner = integer(request, "owner_epoch"), session = integer(request, "session_epoch");
    if (owner <= 0 || session <= 0) throw Error(400, "positive role reference required");
    const auto& role = current->at("role");
    if (role.at("owner_epoch") != owner || role.at("session_epoch") != session) throw Error(409, "stale role reference");
    if (current->at("mode") != "active" || !current->value("connected", false)) throw Error(409, "role is not active");
    if (heartbeat) scene_.execute("/v1/view", request);
    return role;
}
bool RoomInstance::host_control(const Json& request) {
    if (text(request, "role_id") != host_) throw Error(403, "room host required");
    if (request.contains("control_authorized") && !request["control_authorized"].is_boolean()) throw Error(400, "invalid room control flag");
    if (request.value("control_authorized", false)) {
        if (integer(request, "owner_epoch") <= 0 || integer(request, "session_epoch") <= 0) throw Error(400, "positive role reference required");
        return true;
    }
    checked_role(request, true); return true;
}
Json RoomInstance::execute(const std::string& path, const Json& incoming) {
    if (!legacy_ && incoming.contains("_room_world_epoch")) {
        if (!incoming.at("_room_world_epoch").is_string() || incoming.at("_room_world_epoch") != scene_.world_epoch())
            throw Error(409, "room world changed");
        auto clean = incoming;
        clean.erase("_room_world_epoch");
        // 私有控制面围栏仅移除此字段；客户端cast/skill_init的world_epoch仍由Scene校验。
        return execute(path, clean);
    }
    const auto& request = incoming;
    if (!legacy_ && (path == "/v1/config/apply" || path == "/v1/config/reload"))
        throw Error(409, "room combat configuration is pinned");
    if (path == "/healthz") {
        auto out = metadata(false); out["ok"] = true; out["players"] = scene_.player_count(); out["ecs"] = scene_.ecs_stats(); return out;
    }
    if (path == "/v1/status") return metadata();
    if (path == "/v1/final_roles") {
        fields(request, {});
        if (legacy_ || state_ != "finished") throw Error(409, "final snapshots require a finished room");
        return {{"roles", scene_.role_states()}, {"world_epoch", scene_.world_epoch()}};
    }
    if (path == "/v1/persisted") {
        // 这是Backend在事务提交后的内部ACK。任何不匹配都保持权威缓存，
        // 不能让ACK失败把已经提交的保存请求变成业务错误。
        try {
            fields(request, {"role"});
            if (legacy_ || state_ != "finished" || !request.contains("role") || !request["role"].is_object()) return {{"released", false}};
            const auto& saved = request["role"];
            const auto role_id = text(saved, "role_id");
            if (role_id.empty() || text(saved, "scene_id") != id()) return {{"released", false}};
            const auto current = scene_.role_state(role_id);
            if (!current) return {{"released", true}, {"already_absent", true}};
            if (current->at("mode") != "active" || (current->value("connected", false) && !current->value("lease_expired", false))) return {{"released", false}};
            const auto& role = current->at("role");
            for (const auto* field : {"role_id", "scene_id", "owner_epoch", "session_epoch", "name", "x", "y", "combat"})
                if (!saved.contains(field) || saved[field] != role.at(field)) return {{"released", false}};
            scene_.execute("/v1/remove", {{"role_id", role_id}, {"owner_epoch", role.at("owner_epoch")}, {"session_epoch", role.at("session_epoch")}});
            sync(); return {{"released", true}};
        } catch (...) { return {{"released", false}}; }
    }
    if (path == "/v1/ready") {
        fields(request, {"role_id", "owner_epoch", "session_epoch", "ready"});
        if (legacy_ || state_ != "lobby") throw Error(409, "room is not waiting for ready");
        if (!request.contains("ready") || !request["ready"].is_boolean()) throw Error(400, "ready boolean required");
        auto role = checked_role(request, true); sync();
        auto& flag = ready_.at(role.at("role_id").get<std::string>()).ready;
        if (flag != request.at("ready").get<bool>()) { flag = request.at("ready").get<bool>(); ++revision_; }
        return metadata();
    }
    if (path == "/v1/start") {
        fields(request, {"role_id", "owner_epoch", "session_epoch"});
        if (legacy_ || state_ == "finished") throw Error(409, "room cannot start");
        host_control(request); sync();
        if (state_ == "playing") return metadata();
        auto states = scene_.role_states(false);
        if (states.empty()) throw Error(409, "room has no participants");
        for (const auto& item : states) {
            auto id = item.at("role").at("role_id").get<std::string>();
            if (!live(item) || !ready_.at(id).ready) throw Error(409, "all participants must be active and ready");
        }
        state_ = "playing"; ++revision_; return metadata();
    }
    if (path == "/v1/finish") {
        fields(request, {"role_id", "owner_epoch", "session_epoch", "control_authorized"});
        if (legacy_) throw Error(409, "legacy room cannot finish");
        host_control(request);
        for (const auto& member : scene_.role_states(false))
            if (member.value("mode", std::string()) != "active") throw Error(409, "room has an ownership transition in progress");
        if (state_ != "finished") { scene_.stop_combat(); state_ = "finished"; finished_age_ = 0; ++revision_; }
        return metadata();
    }
    if (!legacy_ && (path == "/v1/enter" || path == "/v1/prepare")) {
        const auto& role = request.at("role");
        const auto role_id = text(role, "role_id");
        const auto existing = scene_.role_state(role_id, false);
        if (state_ == "finished" && !existing) throw Error(409, "room finished");
        if (!existing && occupants() >= capacity_) throw Error(409, "room is full");
    }
    if (!legacy_ && (path == "/v1/move" || path == "/v1/move_batch" || path == "/v1/cast" || path == "/v1/revive" || path == "/v1/elemental_cast")) {
        if (state_ != "playing") {
            checked_role(request);
            throw Error(409, state_ == "finished" ? "room finished" : "room not started");
        }
    }
    if (!legacy_ && state_ == "finished" && path == "/v1/skill_init") throw Error(409, "room finished");
    auto result = scene_.execute(path, request);
    if (path == "/v1/enter" || path == "/v1/prepare" || path == "/v1/activate" || path == "/v1/remove" || path == "/v1/disconnect" || path == "/v1/freeze" || path == "/v1/unfreeze") sync();
    if (path == "/v1/view") result["room"] = metadata(false);
    return result;
}
void RoomInstance::step(double dt) {
    if (state_ == "playing") scene_.step(dt);
    else {
        scene_.advance_leases(dt);
        if (state_ == "finished") finished_age_ += dt;
    }
    if (!legacy_) sync();
}
bool RoomInstance::retired(double retention) const {
    if (legacy_ || state_ != "finished" || finished_age_ < retention) return false;
    // disconnect只是取出最终快照，Backend的异步事务可能尚未完成。
    // 只有Coordinator保存后精确remove的空房间才可回收，不能按离线标志丢状态。
    return occupants() == 0;
}

Manager::Manager(Config config) : config_(std::move(config)), scripts_(config_.script_manifest) {
    if (!config_.max_rooms || config_.max_rooms > 64 || !config_.max_room_players || config_.max_room_players > 128 ||
        config_.max_total_players < config_.max_room_players || config_.max_total_players > 8192 || !config_.max_tombstones ||
        config_.scene.tick_hz < 1 || config_.scene.tick_hz > 120 || config_.finished_retention_seconds < 0 || config_.finished_retention_seconds > 60)
        throw Error(400, "invalid worker limits");
    auto room = std::make_unique<RoomInstance>(config_.scene, "", profile_of(config_.scene), config_.scene.max_players, true, scripts_.resolve());
    rooms_.emplace(config_.scene.scene_id, std::move(room));
}
RoomInstance& Manager::find(const std::string& id) {
    const auto found = rooms_.find(id);
    if (found == rooms_.end()) throw Error(404, tombstones_.count(id) ? "room retired" : "room not found");
    return *found->second;
}
std::size_t Manager::occupants() const {
    std::size_t total = 0; for (const auto& item : rooms_) total += item.second->occupants(); return total;
}
Json Manager::create(const Json& request) {
    fields(request, {"room_id", "host_role_id", "capacity", "profile", "script_version", "script_sha256"});
    auto id = text(request, "room_id"), host = text(request, "host_role_id"), profile = text(request, "profile", "skill_lab"), version = text(request, "script_version");
    const auto expected_hash = text(request, "script_sha256");
    if (!expected_hash.empty() && (expected_hash.size() != 64 || expected_hash.find_first_not_of("0123456789abcdef") != std::string::npos)) throw Error(400, "invalid pinned script checksum");
    const auto capacity = integer(request, "capacity", 4);
    if (!identifier(id) || host.empty() || host.size() > 128 || capacity < 1 || capacity > static_cast<std::int64_t>(config_.max_room_players) ||
        (profile != "movement" && profile != "combat" && profile != "abilities" && profile != "skill_lab")) throw Error(400, "invalid room configuration");
    if (const auto found = rooms_.find(id); found != rooms_.end()) {
        const auto& room = *found->second;
        if (room.legacy() || room.host() != host || room.profile() != profile || room.capacity() != static_cast<std::size_t>(capacity) || (!version.empty() && room.script_version() != version) ||
            (!expected_hash.empty() && room.metadata(false).at("script_sha256") != expected_hash)) throw Error(409, "room id already has different configuration");
        return room.metadata();
    }
    if (tombstones_.count(id)) throw Error(409, "room id cannot be reused");
    if (rooms_.size() - 1 >= config_.max_rooms || tombstones_.size() >= config_.max_tombstones) throw Error(503, "worker room capacity exhausted");
    auto program = scripts_.resolve(version);
    if (!expected_hash.empty() && program->sha256() != expected_hash) throw Error(409, "pinned script checksum mismatch");
    auto instance = std::make_unique<RoomInstance>(room_config(config_.scene, id, profile, static_cast<std::size_t>(capacity)), host, profile, static_cast<std::size_t>(capacity), false, program);
    auto out = instance->metadata(); rooms_.emplace(id, std::move(instance)); return out;
}
std::string Manager::queue_key(const std::string& path) {
    if (path.rfind("/rooms/", 0) == 0) {
        const auto end = path.find('/', 7);
        if (end != std::string::npos) return path.substr(7, end - 7);
    }
    return path.rfind("/v1/rooms/", 0) == 0 ? "@control" : "@legacy";
}
Json Manager::execute(const std::string& path, const Json& request) {
    if (!request.is_object()) throw Error(400, "JSON object required");
    if (path == "/v1/rooms/create") return create(request);
    if (path == "/v1/rooms/scripts/reload") return scripts_.reload(request);
    if (path == "/v1/rooms/scripts/status") { fields(request, {}); return scripts_.status(); }
    if (path == "/v1/rooms/list") {
        fields(request, {}); Json values = Json::array();
        for (const auto& item : rooms_) values.push_back(item.second->metadata(false));
        return {{"rooms", values}, {"max_rooms", config_.max_rooms}, {"retired_rooms", tombstones_.size()}};
    }
    if (path == "/v1/rooms/status") { fields(request, {"room_id"}); return find(text(request, "room_id")).metadata(); }
    std::string id = config_.scene.scene_id, operation = path;
    if (path.rfind("/rooms/", 0) == 0) {
        auto slash = path.find('/', 7);
        if (slash == std::string::npos) throw Error(404, "unknown room path");
        id = path.substr(7, slash - 7); operation = path.substr(slash);
        if (!identifier(id)) throw Error(400, "invalid room id");
    }
    auto& room = find(id);
    if ((operation == "/v1/enter" || operation == "/v1/prepare") && request.contains("role") && request["role"].is_object()) {
        const auto member = text(request["role"], "role_id");
        auto status = room.metadata();
        const bool exists = std::any_of(status["members"].begin(), status["members"].end(), [&](const Json& row) { return row["role_id"] == member; });
        if (!exists && occupants() >= config_.max_total_players) throw Error(503, "worker player capacity exhausted");
    }
    return room.execute(operation, request);
}
void Manager::step(double dt) {
    if (!std::isfinite(dt) || dt <= 0 || dt > 1) throw Error(400, "invalid worker step");
    ++worker_tick_;
    for (auto& item : rooms_) item.second->step(dt);
    for (auto it = rooms_.begin(); it != rooms_.end();) {
        if (it->second->retired(config_.finished_retention_seconds) && tombstones_.size() < config_.max_tombstones) { tombstones_.insert(it->first); it = rooms_.erase(it); }
        else ++it;
    }
}
Json Manager::health() const {
    auto result = rooms_.at(config_.scene.scene_id)->metadata(false);
    result["ok"] = true; result["service"] = "native-worker"; result["mode"] = profile_of(config_.scene);
    result["players"] = rooms_.at(config_.scene.scene_id)->live_players(); result["rooms"] = rooms_.size() - 1;
    result["worker_tick"] = worker_tick_; result["total_players"] = occupants(); result["retired_rooms"] = tombstones_.size();
    return result;
}

Runtime::Runtime(Config config) : config_(std::move(config)), manager_(config_), health_(manager_.health()) {
    if (!config_.queue_limit || config_.queue_limit > 4096 || !config_.room_queue_limit || config_.room_queue_limit > config_.queue_limit) throw Error(400, "invalid worker queue limits");
    thread_ = std::thread([this] { run(); });
}
Runtime::~Runtime() { stopping_.store(true); if (thread_.joinable()) thread_.join(); }
Result Runtime::submit(const std::string& path, Json request) {
    auto command = std::make_shared<Command>(); command->path = path; command->key = Manager::queue_key(path); command->body = std::move(request);
    auto future = command->promise.get_future();
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (stopping_) return {503, {{"error", "worker stopping"}}};
        if (queue_.size() >= config_.queue_limit || queued_[command->key] >= config_.room_queue_limit) return {503, {{"error", "worker command queue full"}}};
        ++queued_[command->key]; queue_.push_back(command);
    }
    if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) return {504, {{"error", "worker command timed out; outcome may be committed"}}};
    return future.get();
}
Json Runtime::health() const { std::lock_guard<std::mutex> guard(mutex_); return health_; }
void Runtime::run() {
    const auto period = std::chrono::nanoseconds(1000000000 / config_.scene.tick_hz);
    auto next = std::chrono::steady_clock::now();
    while (!stopping_) {
        next += period;
        std::vector<std::shared_ptr<Command>> batch;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            std::map<std::string, std::size_t> used;
            for (auto it = queue_.begin(); it != queue_.end() && batch.size() < 128;) {
                const auto limit = (*it)->key == "@legacy" ? 32u : (*it)->key == "@control" ? 16u : 8u;
                if (used[(*it)->key] >= limit) { ++it; continue; }
                ++used[(*it)->key];
                if (--queued_[(*it)->key] == 0) queued_.erase((*it)->key);
                batch.push_back(std::move(*it)); it = queue_.erase(it);
            }
        }
        for (auto& command : batch) {
            Result result;
            try { result.body = manager_.execute(command->path, command->body); }
            catch (const Error& error) { result = {error.status, {{"error", error.what()}}}; }
            catch (const mmo::Error& error) { result = {error.status, {{"error", error.what()}}}; }
            catch (const Json::exception&) { result = {400, {{"error", "invalid JSON fields"}}}; }
            catch (...) { result = {500, {{"error", "worker internal error"}}}; }
            command->promise.set_value(std::move(result));
        }
        try { manager_.step(1.0 / config_.scene.tick_hz); }
        catch (...) { stopping_.store(true); }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            health_ = manager_.health(); health_["queued_commands"] = queue_.size();
            if (stopping_) health_["ok"] = false;
        }
        auto now = std::chrono::steady_clock::now();
        if (now > next + period) next = now;
        std::this_thread::sleep_until(next);
    }
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto& command : queue_) command->promise.set_value({503, {{"error", "worker stopping"}}});
    queue_.clear(); queued_.clear();
}
} // namespace native::room
