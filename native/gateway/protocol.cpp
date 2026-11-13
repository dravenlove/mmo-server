#include "gateway/protocol.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>
#include <regex>

namespace mmo::gateway {
namespace {
std::string env(const char* name, std::string fallback = {}) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}
int integer_env(const char* name, int fallback) {
    auto text = env(name);
    if (text.empty()) return fallback;
    std::size_t end = 0;
    int value;
    try { value = std::stoi(text, &end); } catch (...) { throw Failure(std::string(name) + " must be an integer"); }
    if (end != text.size()) throw Failure(std::string(name) + " must be an integer");
    return value;
}
bool valid_base(const std::string& text) {
    return std::regex_match(text, std::regex(R"(^https?://(\[[0-9A-Fa-f:]+\]|[A-Za-z0-9._-]+)(:[0-9]{1,5})?(/[A-Za-z0-9_./-]*)?$)"));
}
bool string_field(const Json& in, const char* field) {
    return !in.contains(field) || in[field].is_null() || in[field].is_string();
}
bool numeric_field(const Json& in, const char* field, bool integral) {
    if (!in.contains(field) || in[field].is_null()) return true;
    const auto& value = in[field];
    if (integral) return value.is_number_integer() && !(value.is_number_unsigned() && value.get<std::uint64_t>() > INT64_MAX);
    return value.is_number() && std::isfinite(value.get<double>());
}
std::uint64_t revision(const Json& role) {
    if (!role.is_object() || !role.contains("combat") || !role["combat"].is_object()) return 0;
    return role["combat"].value("revision", std::uint64_t(0));
}
}
Config Config::environment() {
    Config c;
    if (const char* value=std::getenv("GATEWAY_LISTEN")) c.tcp_listen=value;
    if (const char* value=std::getenv("WEBSOCKET_LISTEN")) c.ws_listen=value;
    else if (const char* value=std::getenv("WS_LISTEN")) c.ws_listen=value;
    c.health_listen = env("GATEWAY_HEALTH_LISTEN", c.health_listen);
    c.backend_url = env("BACKEND_URL", c.backend_url);
    c.token = env("INTERNAL_TOKEN");
    c.profile = env("GATEWAY_PROFILE", c.profile);
    c.login_scene = env("GATEWAY_LOGIN_SCENE"); c.arena_scene = env("GATEWAY_ARENA_SCENE");
    c.state_hz = integer_env("GATEWAY_STATE_HZ", 0);
    c.max_connections = integer_env("GATEWAY_MAX_CONNECTIONS", 128);
    std::string origins = env("WEBSOCKET_ALLOWED_ORIGINS", env("WS_ALLOWED_ORIGINS"));
    std::replace(origins.begin(), origins.end(), ';', ',');
    std::stringstream list(origins);
    for (std::string origin; std::getline(list, origin, ',');) {
        auto start = origin.find_first_not_of(" \r\n\t"), end = origin.find_last_not_of(" \r\n\t");
        if (start != std::string::npos) c.origins.push_back(origin.substr(start, end - start + 1));
    }
    if (!env("TLS_CERT_FILE").empty() || !env("TLS_KEY_FILE").empty())
        throw Failure("native-gateway requires TLS termination at the trusted ingress; direct TCP TLS is not configured");
    c.validate(); return c;
}
void Config::validate() {
    static const std::set<std::string> profiles{"full", "movement", "abilities", "elemental", "skill_lab"};
    if (!profiles.count(profile)) throw Failure("unknown GATEWAY_PROFILE");
    if (state_hz == 0) state_hz = profile == "movement" || profile == "skill_lab" ? 20 : 5;
    if (state_hz < 1 || state_hz > 20) throw Failure("GATEWAY_STATE_HZ must be 1..20");
    if (max_connections < 1 || max_connections > 256) throw Failure("GATEWAY_MAX_CONNECTIONS must be 1..256");
    if (token.size() < 32) throw Failure("INTERNAL_TOKEN must contain at least 32 bytes");
    if (!valid_base(backend_url)) throw Failure("BACKEND_URL must be an HTTP(S) service address without credentials, query or fragment");
    if (login_scene.size() > 64 || arena_scene.size() > 64) throw Failure("scene id is too long");
    if (profile == "elemental" && (login_scene.empty() || arena_scene.empty() || login_scene == arena_scene))
        throw Failure("elemental profile requires distinct login and arena scenes");
    if (!ws_listen.empty() && origins.empty()) throw Failure("WEBSOCKET_ALLOWED_ORIGINS requires at least one exact browser origin");
    for (const auto& origin : origins) {
        if (!valid_base(origin) || origin.find('/', origin.find("://") + 3) != std::string::npos)
            throw Failure("allowed origins must be exact HTTP(S) origins without a path");
    }
}
std::string Config::protocol() const {
    if (profile == "abilities") return "webgl-combat-v1";
    if (profile == "elemental") return "webgl-elemental-v1";
    if (profile == "skill_lab") return "webgl-skill-lab-v1";
    return profile == "movement" ? "webgl-movement-v1" : "tcp-json-v1";
}
Input parse_input(const std::string& text, const std::string& profile) {
    if (text.size() > max_message_bytes) throw Failure("message exceeds 65535 bytes");
    Input result;
    // 限制嵌套并拒绝重复键，避免凭据/身份字段在不同解析器之间产生歧义。
    std::vector<std::set<std::string>> keys;
    bool duplicate = false;
    auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 32) throw Failure("JSON nesting exceeds limit");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.empty() && !keys.back().insert(parsed.get<std::string>()).second) duplicate = true;
        if (event == Json::parse_event_t::object_end && !keys.empty()) keys.pop_back();
        return true;
    };
    try { result.data = Json::parse(text, callback); } catch (const Failure&) { throw; } catch (...) { throw Failure("invalid JSON request"); }
    auto& in = result.data;
    if (duplicate || !in.is_object() || !in.contains("id") || !in["id"].is_string() || in["id"].get_ref<const std::string&>().empty() || in["id"].get_ref<const std::string&>().size() > 64 || !in.contains("op") || !in["op"].is_string() || in["op"].get_ref<const std::string&>().empty() || in["op"].get_ref<const std::string&>().size() > 24) throw Failure("invalid request");
    const auto op = in["op"].get<std::string>();
    static const std::set<std::string> fields{"id","op","name","token","password","seq","dx","dy","target_scene_id","request_id","skill_id","target_id","x","y","scene_id","owner_epoch","element","definition","definition_version","expected_definition_version","world_epoch"};
    bool valid = true;
    for (auto it = in.begin(); it != in.end(); ++it) if (!fields.count(it.key())) valid = false;
    for (const auto* key : {"name","token","password","target_scene_id","request_id","skill_id","target_id","scene_id","element","world_epoch"}) valid = valid && string_field(in,key);
    for (const auto* key : {"seq","owner_epoch"}) valid = valid && numeric_field(in,key,true);
    for (const auto* key : {"definition_version","expected_definition_version"}) {
        if (in.contains(key) && !in[key].is_null())
            valid = valid && in[key].is_number_integer() && (in[key].is_number_unsigned() || in[key].get<std::int64_t>() >= 0);
    }
    for (const auto* key : {"dx","dy","x","y"}) valid = valid && numeric_field(in,key,false);
    if (!valid) {
        if (op != "move" && op != "cast") throw Failure("invalid JSON request");
        result.error = "invalid input fields"; return result;
    }
    if (op == "move" || (op == "cast" && (profile == "skill_lab" || in.value("skill_id", std::string()) == "arc_bolt" || in.value("skill_id",std::string()) == "stone_wall"))) {
        for (const auto* key : {"seq","dx","dy"})
            if ((!in.contains(key) && (profile != "full" || op == "cast")) || (in.contains(key) && in[key].is_null())) result.error = "seq, dx and dy must be numbers";
    }
    // Go 将 null 标量解码为零值；保持旧协议的非必填字段语义。
    for (auto it = in.begin(); it != in.end();) {
        if (it.value().is_null() && it.key() != "definition") it = in.erase(it); else ++it;
    }
    return result;
}
bool same_role(const Json& a, const Json& b) {
    if (!a.is_object() || !b.is_object()) return false;
    for (const auto* key : {"role_id","scene_id","owner_epoch","session_epoch"}) if (!a.contains(key) || !b.contains(key) || a[key] != b[key]) return false;
    return true;
}
Json reference(const Json& role) { return {{"role_id",role.at("role_id")},{"owner_epoch",role.at("owner_epoch")},{"session_epoch",role.at("session_epoch")}}; }
Json client_role(Json role,const std::string& profile) {
    if (profile == "movement" || profile == "elemental") role.erase("combat");
    return role;
}
Json client_view(Json view,const std::string& profile) {
    if (!view.is_object()) throw Failure("invalid service response");
    if (profile == "movement") {
        view["self"] = client_role(view.at("self"),profile);
        if (view.contains("players") && view["players"].is_array()) for (auto& player : view["players"]) player = client_role(player,profile);
        for (const auto* key : {"mobs","skills","events","projectiles","walls","world_epoch","ability_rules_version","pvp_enabled","projectiles_truncated","events_truncated","event_cursor","features","battle"}) view.erase(key);
    }
    if (profile == "elemental" || (profile != "movement" && view.contains("battle") && view["battle"].is_object())) {
        view["self"].erase("combat");
        if (view.contains("players") && view["players"].is_array()) for (auto& player : view["players"]) player.erase("combat");
        if (view.contains("battle") && view["battle"].is_object() && view["battle"].contains("player")) {
            const auto& p = view["battle"]["player"];
            if (p.is_object() && p.value("id",std::string()) == view["self"].value("role_id",std::string())) {
                view["self"]["x"] = p.value("x",0.0); view["self"]["y"] = p.value("y",0.0);
                view["self"]["combat"] = {{"dead",p.value("hp",0.0) <= 0}};
                if (view.contains("players") && view["players"].is_array()) for (auto& player : view["players"]) if (player.value("role_id",std::string()) == p.value("id",std::string())) player = view["self"];
            }
        }
    }
    return view;
}
bool fresh_view(const Json& previous,const Json& next) {
    if (!next.is_object() || !next.contains("self")) return false;
    if (previous.is_null() || !same_role(previous.value("self",Json()),next["self"])) return true;
    if (previous.value("world_epoch",std::string()) != next.value("world_epoch",std::string()) && !previous.value("world_epoch",std::string()).empty() && !next.value("world_epoch",std::string()).empty()) return true;
    if (next.value("tick",std::uint64_t(0)) < previous.value("tick",std::uint64_t(0))) return false;
    return revision(next["self"]) >= revision(previous["self"]);
}
void overlay_balance(Json& view,const Json& role) {
    view.at("self")["coins"] = role.at("coins");
    if (view.contains("players") && view["players"].is_array()) for (auto& p : view["players"]) if (p.value("role_id",std::string()) == role.value("role_id",std::string())) p["coins"] = role.at("coins");
}
Json action_snapshot(Json& output,const Json& route,const Json& previous) {
    if (!output.is_object() || !output.contains("role") || !same_role(output["role"],route["role"])) return previous;
    auto role = output["role"]; role["coins"] = route["role"]["coins"]; output["role"] = role;
    if (previous.is_object() && same_role(previous.value("self",Json()),role)) {
        if (revision(previous["self"]) > revision(role)) return previous;
        auto view = previous; view["self"] = role;
        if (view.contains("players") && view["players"].is_array()) for (auto& p : view["players"]) if (p.value("role_id",std::string()) == role.value("role_id",std::string())) p = role;
        return view;
    }
    return {{"self",role},{"scene_id",role.at("scene_id")}};
}
Json success(const Json& input,Json data) {
    Json out{{"op",input.value("op",std::string("error"))},{"ok",true}};
    if (input.contains("id")) out["id"] = input["id"];
    if (!data.is_null()) out["data"] = std::move(data);
    return out;
}
Json rejected(const Json& input,const Failure& error) {
    Json out{{"op",input.value("op",std::string("error"))},{"ok",false},{"error",error.what()}};
    if (input.contains("id")) out["id"] = input["id"];
    if (error.status) out["status"] = error.status;
    return out;
}
bool permitted(const std::string& profile,const std::string& operation) {
    if (profile == "full") return true;
    if (operation == "register" || operation == "login" || operation == "account_register" || operation == "account_login" || operation == "account_bind" || operation == "view" || operation == "ping" || operation == "move") return true;
    if (profile == "abilities" || profile == "skill_lab") return operation == "cast" || operation == "revive" || (profile == "skill_lab" && operation == "skill_init");
    if (profile == "elemental") return operation == "cast" || operation == "enter_arena" || operation == "leave_arena" || operation == "restart_battle";
    return false;
}
}
