#include "game_service/service.hpp"
#include <algorithm>

namespace native::game {
Service::~Service() {
    { std::lock_guard<std::mutex> lock(ack_mutex_); ack_stopping_ = true; ack_queue_.clear(); }
    ack_wake_.notify_all();
    if (ack_thread_.joinable()) ack_thread_.join();
}

void Service::start_room_acks() {
    ack_thread_ = std::thread([this] {
        for (;;) {
            PersistedAck request;
            {
                std::unique_lock<std::mutex> lock(ack_mutex_);
                for (;;) {
                    if (ack_stopping_) return;
                    if (ack_queue_.empty()) { ack_wake_.wait(lock); continue; }
                    auto next = std::min_element(ack_queue_.begin(), ack_queue_.end(), [](const auto& a, const auto& b) { return a.due < b.due; });
                    if (next->due > std::chrono::steady_clock::now()) { ack_wake_.wait_until(lock, next->due); continue; }
                    request = std::move(*next); ack_queue_.erase(next); break;
                }
            }
            bool released = false;
            try {
                released = http_post(request.url, "/v1/persisted", {{"role", request.role}}, config_.token, 250).value("released", false);
            } catch (...) { /* 确认丢失只保留权威缓存，不影响已经成功的SQL提交。 */ }
            if (!released && request.attempts++ == 0) {
                request.due = std::chrono::steady_clock::now() + std::chrono::seconds(4);
                std::lock_guard<std::mutex> lock(ack_mutex_);
                const bool newer = std::any_of(ack_queue_.begin(), ack_queue_.end(), [&](const auto& q) {
                    return q.role["role_id"] == request.role["role_id"] && q.role["owner_epoch"] == request.role["owner_epoch"] && q.role["session_epoch"] == request.role["session_epoch"];
                });
                if (!ack_stopping_ && !newer && ack_queue_.size() < 64) ack_queue_.push_back(std::move(request));
            }
        }
    });
}

void Service::queue_room_ack(const Json& r) {
    const auto id = r.at("scene_id").get<std::string>();
    if (!room_scene(id)) return;
    const auto rows = db_.query("SELECT status FROM rooms_directory WHERE room_id=?", {id});
    if (rows.empty() || db::text(rows[0], 0) != "FINISHED") return;
    PersistedAck ack{config_.scenes.at(id), r, 0, std::chrono::steady_clock::now()};
    {
        std::lock_guard<std::mutex> lock(ack_mutex_);
        for (auto i = ack_queue_.begin(); i != ack_queue_.end();) {
            if (i->role["role_id"] == r["role_id"] && i->role["owner_epoch"] == r["owner_epoch"] && i->role["session_epoch"] == r["session_epoch"]) i = ack_queue_.erase(i);
            else ++i;
        }
        if (ack_queue_.size() >= 64) return; // 有界队列满则保留Scene缓存，不能提前回收。
        ack_queue_.push_back(std::move(ack));
    }
    ack_wake_.notify_one();
}

void Service::persist_finished_members(const std::string& id) {
    Json snapshots;
    try { snapshots = http_post(config_.scenes.at(id), "/v1/final_roles", Json::object(), config_.token, 1000); }
    catch (...) { return; } // 失联时缓存保留，下次status/drain/重连可继续保存。
    if (!snapshots.is_object() || !snapshots.contains("roles") || !snapshots["roles"].is_array() || snapshots["roles"].size() > 32) return;
    for (const auto& entry : snapshots["roles"]) {
        if (entry.value("mode", std::string()) != "active" || !entry.contains("role")) continue;
        const auto& r = entry["role"];
        if (r.value("scene_id", std::string()) != id) continue;
        try { checkpoint({{"role", r}}); } catch (...) { /* 归属不匹配或保存失败均不发送回收确认。 */ }
    }
}
} // namespace native::game
