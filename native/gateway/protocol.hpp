#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace mmo::gateway {
using Json = nlohmann::json;
constexpr std::size_t max_message_bytes = 65535;
struct Failure : std::runtime_error {
    int status;
    explicit Failure(std::string message, int code = 0) : std::runtime_error(std::move(message)), status(code) {}
};
struct Config {
    std::string tcp_listen = "127.0.0.1:7000";
    std::string ws_listen;
    std::string health_listen = "127.0.0.1:8080";
    std::string backend_url = "http://127.0.0.1:8081";
    std::string token, profile = "full", login_scene, arena_scene;
    std::vector<std::string> origins;
    int state_hz = 0;
    int max_connections = 128;
    static Config environment();
    void validate();
    std::string protocol() const;
};
struct Input {
    Json data;
    Json received_route;
    std::string error;
    std::chrono::steady_clock::time_point received = std::chrono::steady_clock::now();
};
Input parse_input(const std::string& text, const std::string& profile);
bool same_role(const Json& a, const Json& b);
Json reference(const Json& role);
Json client_role(Json role, const std::string& profile);
Json client_view(Json view, const std::string& profile);
bool fresh_view(const Json& previous, const Json& next);
void overlay_balance(Json& view, const Json& role);
Json action_snapshot(Json& output, const Json& route, const Json& previous);
Json success(const Json& input, Json data);
Json rejected(const Json& input, const Failure& error);
bool permitted(const std::string& profile, const std::string& operation);
std::string sha256_prefix(const std::string& value);
}
