#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>

namespace mmo {
using Json = nlohmann::json;

struct Error : std::runtime_error {
    int status;
    Error(int code, const std::string& message) : std::runtime_error(message), status(code) {}
};

struct Config {
    std::string scene_id = "scene-1";
    int tick_hz = 20;
    double aoi_radius = 30;
    double move_speed = 6;
    std::size_t max_players = 128;
    std::size_t max_tracked_roles = 8192;
};

struct Role {
    std::string role_id, name, scene_id;
    std::int64_t owner_epoch = 0, session_epoch = 0, coins = 0;
    double x = 0, y = 0;
    Json json() const;
};

// 只允许 Tick 线程调用；HTTP 线程通过外层命令队列访问。
class Scene {
public:
    explicit Scene(Config config);
    Json execute(const std::string& path, const Json& request);
    void step(double dt);
    std::size_t player_count() const { return entities_.size(); }
    std::uint64_t tick() const { return tick_; }

private:
    enum class Mode { Active, Frozen, Prepared };
    struct Entity {
        Role role;
        Mode mode = Mode::Active;
        std::string transfer_id;
        std::set<std::string> cancelled_transfers;
        std::int64_t last_seq = 0;
        double dx = 0, dy = 0, input_age = 1;
    };
    Config config_;
    std::map<std::string, Entity> entities_;
    // 墓碑不能按普通缓存淘汰：淘汰后可能接受迟到的旧入场请求。
    std::map<std::string, std::int64_t> removed_epochs_;
    std::uint64_t tick_ = 0;

    Role parse_role(const Json& request) const;
    Entity& matching(const Json& request, bool active_only = false);
    void admit(const Role& role);
    void ensure_tracked(const std::string& role_id);
    Json enter(const Json& request);
    Json prepare(const Json& request);
};
} // namespace mmo
