#include "scene.hpp"
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <future>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }

std::string environment(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value ? value : fallback;
}

double number_env(const char* name, const char* fallback, double low, double high) {
    auto value = environment(name, fallback);
    std::size_t used = 0;
    double parsed;
    try { parsed = std::stod(value, &used); }
    catch (...) { throw std::runtime_error(std::string("invalid environment: ") + name); }
    if (used != value.size() || !std::isfinite(parsed) || parsed < low || parsed > high)
        throw std::runtime_error(std::string("invalid environment: ") + name);
    return parsed;
}

int integer_env(const char* name, const char* fallback, int low, int high) {
    auto value = number_env(name, fallback, low, high);
    if (std::floor(value) != value) throw std::runtime_error(std::string("integer required: ") + name);
    return static_cast<int>(value);
}

bool token_matches(const std::string& actual, const std::string& expected) {
    if (actual.size() != expected.size()) return false;
    unsigned char different = 0;
    for (std::size_t i = 0; i < expected.size(); ++i)
        different |= static_cast<unsigned char>(actual[i] ^ expected[i]);
    return different == 0;
}

mmo::Json strict_json(const std::string& text) {
    std::vector<std::set<std::string>> objects;
    bool invalid = false;
    try {
        auto parsed = mmo::Json::parse(text, [&](int depth, mmo::Json::parse_event_t event, mmo::Json& value) {
            if (depth > 16) invalid = true;
            if (event == mmo::Json::parse_event_t::object_start) objects.emplace_back();
            else if (event == mmo::Json::parse_event_t::key && !objects.back().insert(value.get<std::string>()).second)
                invalid = true;
            else if (event == mmo::Json::parse_event_t::object_end) objects.pop_back();
            return true;
        });
        if (invalid || !parsed.is_object()) throw mmo::Error(400, "JSON object without duplicate keys required");
        return parsed;
    } catch (const mmo::Json::exception&) {
        throw mmo::Error(400, "valid finite JSON object required");
    }
}

mmo::Json read_combat_config(const std::string& path) {
    if (path.empty()) throw mmo::Error(409, "COMBAT_CONFIG_FILE is not configured");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw mmo::Error(400, "cannot open COMBAT_CONFIG_FILE");
    std::string bytes(64 * 1024 + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto size = input.gcount();
    if (size > 64 * 1024) throw mmo::Error(400, "COMBAT_CONFIG_FILE exceeds 64KiB");
    if (input.bad() || (input.fail() && !input.eof())) throw mmo::Error(400, "cannot read COMBAT_CONFIG_FILE");
    bytes.resize(static_cast<std::size_t>(size));
    return strict_json(bytes);
}

struct Result { int status; mmo::Json body; };
struct Command {
    std::string path;
    mmo::Json request;
    std::promise<Result> promise;
};

class Runtime {
public:
    explicit Runtime(mmo::Config config) : config_(std::move(config)), scene_(config_), thread_([this] { run(); }) {}
    ~Runtime() { stop_.store(true); thread_.join(); }

    Result submit(const std::string& path, mmo::Json request) {
        auto command = std::make_shared<Command>();
        command->path = path;
        command->request = std::move(request);
        auto response = command->promise.get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= 512) return {503, {{"error", "scene command queue full"}}};
            queue_.push_back(command);
        }
        if (response.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
            return {504, {{"error", "scene command timed out; outcome may be committed"}}};
        return response.get();
    }

    mmo::Json health() const {
        return {{"ok", true}, {"scene_id", config_.scene_id},
                {"tick", tick_.load()}, {"players", players_.load()}};
    }

private:
    mmo::Config config_;
    mmo::Scene scene_;
    std::mutex mutex_;
    std::deque<std::shared_ptr<Command>> queue_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> tick_{0};
    std::atomic<std::size_t> players_{0};
    std::thread thread_;

    void run() {
        const auto period = std::chrono::nanoseconds(1000000000 / config_.tick_hz);
        auto next = std::chrono::steady_clock::now();
        while (!stop_.load()) {
            next += period;
            std::deque<std::shared_ptr<Command>> batch;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (std::size_t count = 0; count < 128 && !queue_.empty(); ++count) {
                    batch.push_back(std::move(queue_.front()));
                    queue_.pop_front();
                }
            }
            for (auto& command : batch) {
                Result result;
                try { result = {200, scene_.execute(command->path, command->request)}; }
                catch (const mmo::Error& e) { result = {e.status, {{"error", e.what()}}}; }
                catch (const mmo::Json::exception&) { result = {400, {{"error", "invalid JSON fields"}}}; }
                catch (const std::exception& e) {
                    std::cerr << "scene command failed: " << e.what() << '\n';
                    result = {500, {{"error", "scene internal error"}}};
                }
                command->promise.set_value(std::move(result));
            }
            scene_.step(1.0 / config_.tick_hz);
            tick_.store(scene_.tick());
            players_.store(scene_.player_count());
            auto now = std::chrono::steady_clock::now();
            // 过载时让模拟减速，不进行无上限追帧。
            if (now > next + period) next = now;
            std::this_thread::sleep_until(next);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& command : queue_)
            command->promise.set_value({503, {{"error", "scene stopping"}}});
        queue_.clear();
    }
};

void reply(httplib::Response& response, int status, const mmo::Json& body) {
    response.status = status;
    response.set_content(body.dump(), "application/json; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        std::cout << "scene-server: configure INTERNAL_TOKEN, SCENE_ID, SCENE_HOST, SCENE_PORT, TICK_HZ, AOI_RADIUS, MOVE_SPEED, COMBAT_CONFIG_FILE\n";
        return 0;
    }
    try {
        auto token = environment("INTERNAL_TOKEN", "");
        if (token.size() < 32 || token.size() > 256) throw std::runtime_error("INTERNAL_TOKEN must contain 32..256 characters");
        mmo::Config config;
        config.scene_id = environment("SCENE_ID", "scene-1");
        if (config.scene_id.empty() || config.scene_id.size() > 128) throw std::runtime_error("invalid SCENE_ID");
        config.tick_hz = integer_env("TICK_HZ", "20", 1, 120);
        config.aoi_radius = number_env("AOI_RADIUS", "30", 1, 1000);
        config.move_speed = number_env("MOVE_SPEED", "6", 0.01, 100);
        const auto combat_config_path = environment("COMBAT_CONFIG_FILE", "");
        if (!combat_config_path.empty()) config.combat_config = read_combat_config(combat_config_path);
        const auto host = environment("SCENE_HOST", "127.0.0.1");
        const auto port = integer_env("SCENE_PORT", "9001", 1, 65535);

        httplib::Server server;
        // 场景状态只存在于一个进程，禁止多个实例共享同一监听端口。
        server.set_socket_options([](socket_t socket) {
#ifdef _WIN32
            if (!httplib::set_socket_opt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1))
                throw std::runtime_error("could not enable exclusive scene listener");
#else
            // 允许退出后的端口重绑定，但不启用 SO_REUSEPORT 的并行监听。
            if (!httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEADDR, 1))
                throw std::runtime_error("could not configure scene listener");
#endif
        });
        server.set_payload_max_length(64 * 1024);
        server.set_read_timeout(5);
        server.set_write_timeout(5);
        server.set_keep_alive_timeout(5);
        // HTTP 连接线程和待处理连接也有上限。
        server.new_task_queue = [] { return new httplib::ThreadPool(8, 8, 128); };
        Runtime runtime(config);
        server.Get("/healthz", [&](const httplib::Request&, httplib::Response& response) {
            reply(response, 200, runtime.health());
        });

        for (const auto* path : {"/v1/enter", "/v1/move", "/v1/view", "/v1/cast", "/v1/revive", "/v1/freeze", "/v1/unfreeze", "/v1/prepare", "/v1/activate", "/v1/remove",
                "/v1/config/status", "/v1/config/reload", "/v1/config/apply", "/v1/config/validate"}) {
            server.Post(path, [&, path](const httplib::Request& request, httplib::Response& response) {
                if (!token_matches(request.get_header_value("X-Internal-Token"), token)) {
                    reply(response, 401, {{"error", "invalid internal token"}});
                    return;
                }
                try {
                    auto body = strict_json(request.body);
                    if (std::string(path) == "/v1/config/reload") {
                        for (auto it = body.begin(); it != body.end(); ++it)
                            if (it.key() != "expected_generation") throw mmo::Error(400, "reload only accepts expected_generation");
                        body["config"] = read_combat_config(combat_config_path);
                    }
                    auto result = runtime.submit(path, std::move(body));
                    reply(response, result.status, result.body);
                } catch (const mmo::Error& error) {
                    reply(response, error.status, {{"error", error.what()}});
                }
            });
        }
        server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
            if (response.get_header_value("Content-Type").find("application/json") != 0)
                reply(response, response.status, {{"error", "HTTP request rejected"}});
        });
        if (!server.bind_to_port(host, port)) throw std::runtime_error("could not bind scene address");
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        std::atomic<bool> done{false};
        std::thread shutdown([&] {
            while (!done.load()) {
                if (interrupted) { server.stop(); return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        std::cout << "scene-server scene=" << config.scene_id << " listen=" << host << ':' << port
                  << " tick_hz=" << config.tick_hz << '\n' << std::flush;
        bool clean = server.listen_after_bind();
        done.store(true);
        shutdown.join();
        return clean ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "scene-server: " << e.what() << '\n';
        return 1;
    }
}
