#include "room/worker.hpp"
#include <httplib.h>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }
double number(const char* key, const char* fallback, double low, double high) {
    auto value = native::env(key, fallback); std::size_t used = 0;
    const auto result = std::stod(value, &used);
    if (used != value.size() || !std::isfinite(result) || result < low || result > high) throw std::runtime_error(std::string("invalid environment: ") + key);
    return result;
}
std::size_t count(const char* key, const char* fallback, std::size_t low, std::size_t high) {
    auto result = number(key, fallback, static_cast<double>(low), static_cast<double>(high));
    if (std::floor(result) != result) throw std::runtime_error(std::string("integer environment required: ") + key);
    return static_cast<std::size_t>(result);
}
void reply(httplib::Response& response, int code, const native::Json& json) {
    response.status = code; response.set_content(json.dump(), "application/json; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
}
}
int main(int argc, char** argv) {
#ifndef _WIN32
    umask(0077);
#endif
    if (argc > 1 && std::string(argv[1]) == "--help") {
        std::cout << "native-worker: SCENE_ID/SCENE_HOST/SCENE_PORT/SCENE_MODE, ROOM_SCRIPT_MANIFEST, WORKER_MAX_ROOMS, WORKER_MAX_ROOM_PLAYERS, WORKER_MAX_PLAYERS\n"; return 0;
    }
    try {
        const auto token = native::env("INTERNAL_TOKEN");
        if (token.size() < 32 || token.size() > 256) throw std::runtime_error("INTERNAL_TOKEN must contain 32..256 bytes");
        native::room::Config config;
        config.scene.scene_id = native::env("SCENE_ID", "scene-1");
        if (config.scene.scene_id.empty() || config.scene.scene_id.size() > 128) throw std::runtime_error("invalid SCENE_ID");
        const auto mode = native::env("SCENE_MODE", "combat");
        if (mode != "combat" && mode != "movement" && mode != "abilities" && mode != "skill_lab" && mode != "elemental") throw std::runtime_error("invalid SCENE_MODE");
        config.scene.combat_enabled = mode == "combat" || mode == "abilities" || mode == "skill_lab";
        config.scene.abilities_enabled = mode == "abilities" || mode == "skill_lab";
        config.scene.skill_lab_enabled = mode == "skill_lab"; config.scene.elemental_enabled = mode == "elemental";
        config.scene.elemental_arena_id = native::env("ELEMENTAL_ARENA_ID", "scene-elemental-arena");
        if (config.scene.elemental_enabled && (config.scene.elemental_arena_id.empty() || config.scene.elemental_arena_id.size() > 64 || config.scene.elemental_arena_id == config.scene.scene_id)) throw std::runtime_error("invalid ELEMENTAL_ARENA_ID");
        const auto pvp = native::env("PVP_ENABLED", "true");
        if (pvp != "true" && pvp != "false") throw std::runtime_error("invalid PVP_ENABLED");
        config.scene.pvp_enabled = pvp == "true";
        config.scene.trace_movement = native::env("TRACE_MOVEMENT", "false") == "true";
        config.scene.tick_hz = static_cast<int>(count("TICK_HZ", "20", 1, 120));
        if (config.scene.elemental_enabled && config.scene.tick_hz != 20) throw std::runtime_error("elemental requires 20Hz");
        config.scene.aoi_radius = number("AOI_RADIUS", "30", 1, 1000);
        config.scene.move_speed = number("MOVE_SPEED", "6", .01, 100);
        config.scene.max_players = count("MAX_PLAYERS", "128", 1, 1024);
        config.script_manifest = native::env("ROOM_SCRIPT_MANIFEST", "scripts/gameplay/manifest.json");
        config.combat_config_file = native::env("COMBAT_CONFIG_FILE");
        if (!config.combat_config_file.empty()) config.scene.combat_config = native::room::read_json_file(config.combat_config_file);
        config.max_rooms = count("WORKER_MAX_ROOMS", "16", 1, 64);
        config.max_room_players = count("WORKER_MAX_ROOM_PLAYERS", "32", 1, 128);
        config.max_total_players = count("WORKER_MAX_PLAYERS", "512", 1, 8192);
        config.max_tombstones = count("WORKER_MAX_TOMBSTONES", "4096", 1, 65536);
        config.queue_limit = count("WORKER_QUEUE_LIMIT", "512", 1, 4096);
        config.room_queue_limit = count("WORKER_ROOM_QUEUE_LIMIT", "64", 1, 512);
        config.finished_retention_seconds = number("WORKER_FINISHED_RETENTION_SECONDS", "1", 0, 60);
        const auto host = native::env("SCENE_HOST", "127.0.0.1");
        const auto port = static_cast<int>(count("SCENE_PORT", "9001", 1, 65535));
        native::room::Runtime runtime(config);
        httplib::Server server;
        server.set_socket_options([](socket_t socket) {
#ifdef _WIN32
            if (!httplib::set_socket_opt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1)) throw std::runtime_error("cannot configure exclusive worker listener");
#else
            // 不启用httplib默认的SO_REUSEPORT，禁止两个独立世界随机分享同一个端口。
            if (!httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEADDR, 1)) throw std::runtime_error("cannot configure exclusive worker listener");
#endif
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(8, 8, 128); };
        server.set_payload_max_length(65536); server.set_read_timeout(5); server.set_write_timeout(5);
        server.set_keep_alive_max_count(1);
        server.Get("/healthz", [&](const httplib::Request&, httplib::Response& response) {
            auto health = runtime.health(); reply(response, health.value("ok", false) ? 200 : 503, health);
        });
        auto authenticated = [&](const httplib::Request& request, httplib::Response& response) {
            if (native::secure_equal(request.get_header_value("X-Internal-Token"), token)) return true;
            reply(response, 401, {{"error", "invalid internal token"}}); return false;
        };
        auto perform = [&](const std::string& path, native::Json body, httplib::Response& response) {
            if (path.size() >= 17 && path.compare(path.size() - 17, 17, "/v1/config/reload") == 0) {
                for (auto it = body.begin(); it != body.end(); ++it) if (it.key() != "expected_generation") throw native::Error(400, "reload only accepts expected_generation");
                if (config.combat_config_file.empty()) throw native::Error(409, "COMBAT_CONFIG_FILE is not configured");
                body["config"] = native::room::read_json_file(config.combat_config_file);
            }
            auto result = runtime.submit(path, std::move(body)); reply(response, result.status, result.body);
        };
        server.Post(R"(/.*)", [&](const httplib::Request& request, httplib::Response& response) {
            if (!authenticated(request, response)) return;
            try { perform(request.path, native::room::strict_object(request.body), response); }
            catch (const native::Error& error) { reply(response, error.status, {{"error", error.what()}}); }
            catch (...) { reply(response, 400, {{"error", "invalid worker request"}}); }
        });
        server.Get(R"(/rooms/[^/]+/healthz)", [&](const httplib::Request& request, httplib::Response& response) {
            auto result = runtime.submit(request.path, native::Json::object()); reply(response, result.status, result.body);
        });
        server.Get(R"(/v1/rooms/(list|status))", [&](const httplib::Request& request, httplib::Response& response) {
            if (!authenticated(request, response)) return;
            native::Json body = native::Json::object();
            if (request.path == "/v1/rooms/status" && request.has_param("room_id")) body["room_id"] = request.get_param_value("room_id");
            auto result = runtime.submit(request.path, std::move(body)); reply(response, result.status, result.body);
        });
        server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
            if (response.get_header_value("Content-Type").find("application/json") != 0) reply(response, response.status, {{"error", "worker HTTP request rejected"}});
        });
        if (!server.bind_to_port(host, port)) throw std::runtime_error("cannot bind worker listener");
        std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal);
        std::atomic<bool> done{false};
        std::thread monitor([&] {
            while (!done && !interrupted) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); if (!runtime.health().value("ok", false)) break; }
            if (!done) server.stop();
        });
        std::cout << "native-worker default_scene=" << config.scene.scene_id << " listen=" << host << ':' << port << " mode=" << mode << '\n' << std::flush;
        bool success = server.listen_after_bind(); done = true; monitor.join();
        return success ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << "native-worker startup failed: " << error.what() << '\n'; return 1; }
}
