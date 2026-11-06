#pragma once

#include <cstdint>
#include <deque>
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
    Json combat_config; // 未设置时使用内置数值；启动文件由 HTTP 外层读取。
};

struct Role {
    struct Combat {
        int hp = 180;
        double mp = 100;
        bool dead = false;
        double revive_remaining = 0;
        std::int64_t revision = 1, last_action_seq = 0, action_session = 0;
        std::map<std::string, double> cooldowns{{"strike", 0}, {"whirlwind", 0}, {"charge", 0}, {"mend", 0}};
        Json json(bool full = true) const;
    } combat;
    std::string role_id, name, scene_id;
    std::int64_t owner_epoch = 0, session_epoch = 0, coins = 0;
    double x = 0, y = 0;
    Json json(bool full_combat = true) const;
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
        double lease_age = 0;
        std::string last_action_key;
        Json last_action_result;
    };
    struct Npc {
        std::string id, name, kind, target_id;
        double x = 0, y = 0, spawn_x = 0, spawn_y = 0;
        int hp = 80, max_hp = 80, damage = 8;
        double speed = 3.2, attack_period = 1.3, attack_remaining = 0;
        double respawn_remaining = 0, stun_remaining = 0;
        double respawn_period = 8;
        std::string config_version;
        Json json() const;
    };
    Config config_;
    std::map<std::string, Entity> entities_;
    // 墓碑不能按普通缓存淘汰：淘汰后可能接受迟到的旧入场请求。
    std::map<std::string, std::int64_t> removed_epochs_;
    std::uint64_t tick_ = 0;
    std::map<std::string, Npc> npcs_;
    std::deque<Json> events_;
    std::uint64_t next_event_id_ = 0;
    Json combat_config_;
    std::map<std::string, Json> config_history_;
    std::uint64_t config_generation_ = 1, config_applied_tick_ = 0;

    Role parse_role(const Json& request) const;
    Entity& matching(const Json& request, bool active_only = false);
    void admit(const Role& role);
    void ensure_tracked(const std::string& role_id);
    Json enter(const Json& request);
    Json prepare(const Json& request);
    Json view(Entity& entity);
    Json cast(Entity& entity, const Json& request);
    Json revive(Entity& entity, const Json& request);
    Json config_status() const;
    Json reload_config(const Json& request, bool validate_only = false);
    void emit(const std::string& type, const std::string& source, const std::string& target,
        const std::string& skill, int value, double x, double y);
    void damage_npc(Entity& entity, Npc& npc, const std::string& skill, int amount, Json& hits);
    void damage_player(Npc& npc, Entity& entity);
    void reset_npc(Npc& npc);
    void step_combat(Entity& entity, double dt);
    void step_npcs(double dt);
};
} // namespace mmo
