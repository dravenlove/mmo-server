#pragma once

#include "common/support.hpp"
#include "script/script.hpp"
#include "scene.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace native::room {
struct Config {
    mmo::Config scene;
    std::string script_manifest = "scripts/gameplay/manifest.json";
    std::string combat_config_file;
    std::size_t max_rooms = 16, max_room_players = 32, max_total_players = 512;
    std::size_t max_tombstones = 4096, queue_limit = 512, room_queue_limit = 64;
    double finished_retention_seconds = 1;
};

class ScriptCatalog {
public:
    explicit ScriptCatalog(std::string manifest);
    std::shared_ptr<const script::Script> resolve(const std::string& version = "");
    Json reload(const Json& request);
    Json status() const;
private:
    std::string manifest_, root_, active_;
    std::map<std::string, std::shared_ptr<const script::Script>> versions_;
    std::uint64_t generation_ = 0;
    std::shared_ptr<const script::Script> read(const std::string& manifest, const std::string& expected = "") const;
};

// 只由Worker执行线程访问。每个实例持有独立Scene/ECS及固定脚本快照。
class RoomInstance {
public:
    RoomInstance(mmo::Config config, std::string host, std::string profile,
                 std::size_t capacity, bool legacy, std::shared_ptr<const script::Script> script);
    Json execute(const std::string& path, const Json& request);
    Json metadata(bool members = true) const;
    void step(double dt);
    bool retired(double retention) const;
    std::size_t occupants() const;
    std::size_t live_players() const { return scene_.player_count(); }
    const std::string& id() const { return config_.scene_id; }
    const std::string& host() const { return host_; }
    const std::string& profile() const { return profile_; }
    const std::string& script_version() const { return script_->version(); }
    std::size_t capacity() const { return capacity_; }
    bool legacy() const { return legacy_; }
private:
    struct Ready { std::int64_t owner = 0, session = 0; bool ready = false; std::string fingerprint; };
    mmo::Config config_;
    std::string host_, profile_, state_;
    std::size_t capacity_;
    bool legacy_;
    std::shared_ptr<const script::Script> script_;
    mmo::Scene scene_;
    std::map<std::string, Ready> ready_;
    std::uint64_t revision_ = 1;
    double finished_age_ = 0;
    void sync();
    Json checked_role(const Json& request, bool heartbeat = false);
    bool host_control(const Json& request);
};

class Manager {
public:
    explicit Manager(Config config);
    Json execute(const std::string& path, const Json& request);
    void step(double dt);
    Json health() const;
    static std::string queue_key(const std::string& path);
private:
    Config config_;
    ScriptCatalog scripts_;
    std::map<std::string, std::unique_ptr<RoomInstance>> rooms_;
    std::set<std::string> tombstones_;
    std::uint64_t worker_tick_ = 0;
    RoomInstance& find(const std::string& id);
    Json create(const Json& request);
    std::size_t occupants() const;
};

struct Result { int status = 200; Json body; };
class Runtime {
public:
    explicit Runtime(Config config);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Result submit(const std::string& path, Json request);
    Json health() const;
private:
    struct Command {
        std::string path, key;
        Json body;
        std::promise<Result> promise;
    };
    Config config_;
    Manager manager_;
    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<Command>> queue_;
    std::map<std::string, std::size_t> queued_;
    Json health_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    void run();
};

Json strict_object(const std::string& text);
Json read_json_file(const std::string& path, std::size_t maximum = 64 * 1024);
} // namespace native::room
