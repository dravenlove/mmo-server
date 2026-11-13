#pragma once
#include "common/support.hpp"
#include "persistence/store.hpp"
#include <map>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <chrono>

namespace native::game {
struct Config {
    std::string token, auth_url, default_scene, room_worker_url;
    std::map<std::string, std::string> scenes;
    db::Config database;
};
class Service {
public:
    explicit Service(Config config);
    ~Service();
    Json execute(const std::string& operation, const Json& request);
    void health();
    void recover();
private:
    struct Migration {
        std::string role_id, request_id, transfer_id, target_scene_id, status;
        Json source, target;
    };
    Config config_;
    db::Store db_;
    std::mutex mutex_;
    struct PersistedAck { std::string url; Json role; unsigned attempts = 0; std::chrono::steady_clock::time_point due{}; };
    std::mutex ack_mutex_;
    std::condition_variable ack_wake_;
    std::deque<PersistedAck> ack_queue_;
    bool ack_stopping_ = false;
    std::thread ack_thread_;
    Json role(const std::string& id);
    Json validate_session(const Json& req);
    Json route(const Json& role);
    Json scene(const std::string& id, const std::string& operation, const Json& request);
    Json auth(const std::string& operation, const Json& request);
    Json identity(const std::string& operation, const Json& request);
    Json account(const std::string& operation, const Json& request);
    Json login_role(const std::string& id);
    void load_combat(Json& role);
    void save_combat(const Json& role);
    void save_position(const Json& role);
    Json register_local(const Json& request);
    Json login_local(const Json& request);
    Json session(const Json& request);
    Json checkpoint(const Json& request);
    Json claim(const Json& request);
    Json transfer(const Json& request);
    void room_schema();
    Json room_request(const std::string& operation, const Json& request);
    Json room_record(const std::string& id);
    Json ensure_room(const std::string& id);
    void restore_room_routes();
    bool room_scene(const std::string& id) const;
    void recover_finished_room(Json& role, const std::string& recovery_key = {});
    void start_room_acks();
    void queue_room_ack(const Json& role);
    void persist_finished_members(const std::string& room_id);
    void bind_room_world(Json& role, const Json& metadata, bool rotate_session);
    Json drain_rooms(const Json& request);
    std::optional<Migration> migration(const std::string& role_id, const std::optional<std::string>& request_id = {});
    Migration begin_migration(const Json& role, const Json& request);
    void commit_migration(Migration& migration);
    void abort_migration(Migration& migration);
    void finish_committed(Migration& migration);
    void recover_migration(Migration& migration);
    void recover_role(const std::string& role_id);
    void set_status(Migration& migration, const std::string& status);
    [[noreturn]] void cancel_after_failure(Migration& migration, const std::string& message);
};
std::int64_t combat_revision(const Json& combat);
void parse_scenes(Config& config, const std::string& raw);
}
