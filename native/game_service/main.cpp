#include "game_service/service.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }
unsigned port_number(const std::string& value) {
    std::size_t used = 0;
    const auto number = std::stoul(value, &used);
    if (used != value.size() || number < 1 || number > 65535) throw std::runtime_error("port must be 1..65535");
    return static_cast<unsigned>(number);
}
std::string password() {
    const auto file = native::env("MYSQL_PASSWORD_FILE");
    if (file.empty()) return native::env("MYSQL_PASSWORD");
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read MYSQL_PASSWORD_FILE");
    std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
    return value;
}
native::Json parse(const std::string& body) {
    std::vector<std::set<std::string>> objects;
    bool invalid = false;
    auto j = native::Json::parse(body, [&](int depth, native::Json::parse_event_t event, native::Json& value) {
        if (depth > 32) invalid = true;
        if (event == native::Json::parse_event_t::object_start) objects.emplace_back();
        else if (event == native::Json::parse_event_t::key) { if (!objects.back().insert(value.get<std::string>()).second) invalid = true; }
        else if (event == native::Json::parse_event_t::object_end) objects.pop_back();
        return true;
    });
    if (invalid || !j.is_object()) throw native::Error(400, "请求必须是无重复字段的JSON对象");
    return j;
}
void write(httplib::Response& response, int status, const native::Json& body) { response.status = status; response.set_content(body.dump(), "application/json; charset=utf-8"); }
}
int main(int argc, char** argv) {
    try {
        native::game::Config config;
        config.token = native::env("INTERNAL_TOKEN");
        config.auth_url = native::env("AUTH_URL");
        config.database.driver = native::env("BACKEND_DB_DRIVER", "sqlite");
        config.database.path = native::env("BACKEND_DB", "data/mmo.db");
        config.database.host = native::env("MYSQL_HOST");
        config.database.port = port_number(native::env("MYSQL_PORT", "3306"));
        config.database.user = native::env("MYSQL_USER");
        config.database.password = password();
        config.database.database = native::env("MYSQL_DATABASE");
        config.database.tls = native::env("MYSQL_TLS", "false");
        for (int i = 1; i < argc; ++i) {
            std::string flag = argv[i];
            if (i + 1 == argc) throw std::runtime_error("flag requires value");
            std::string value = argv[++i];
            if (flag == "-db-driver" || flag == "--db-driver") config.database.driver = value;
            else if (flag == "-db-path" || flag == "--db-path") config.database.path = value;
            else if (flag == "-mysql-host" || flag == "--mysql-host") config.database.host = value;
            else if (flag == "-mysql-port" || flag == "--mysql-port") config.database.port = port_number(value);
            else if (flag == "-role" && value == "game") {}
            else throw std::runtime_error("unknown flag");
        }
        native::game::parse_scenes(config, native::env("SCENES", "scene-1=http://127.0.0.1:9001,scene-2=http://127.0.0.1:9002"));
        native::game::Service service(config);
        try { service.recover(); }
        catch (const std::exception&) { std::cerr << "some migrations await scene recovery\n"; }
        httplib::Server server;
        server.new_task_queue = [] { return new httplib::ThreadPool(8, 8, 64, 0); };
        server.set_payload_max_length(65536);
        server.set_read_timeout(5, 0);
        server.set_write_timeout(20, 0);
        server.set_keep_alive_max_count(1);
        server.set_pre_routing_handler([](const httplib::Request& request, httplib::Response& response) {
            std::size_t size = 0;
            for (const auto& header : request.headers) size += header.first.size() + header.second.size() + 4;
            if (size > 16384) { write(response, 431, {{"error", "HTTP headers too large"}}); return httplib::Server::HandlerResponse::Handled; }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        const std::set<std::string> ops{"register", "login", "session", "checkpoint", "transfer", "claim", "account_register", "account_login", "account_bind"};
        server.Get("/healthz", [&](const httplib::Request&, httplib::Response& response) {
            try { service.health(); write(response, 200, {{"ok", true}, {"service", "backend"}, {"implementation", "cpp"}}); }
            catch (...) { write(response, 503, {{"error", "数据库不可用"}}); }
        });
        server.Post(R"(/v1/(.*))", [&](const httplib::Request& request, httplib::Response& response) {
            if (!native::secure_equal(request.get_header_value("X-Internal-Token"), config.token)) { write(response, 401, {{"error", "内部服务凭据无效"}}); return; }
            const auto operation = request.matches[1].str();
            if (!ops.count(operation)) { write(response, 404, {{"error", "接口不存在"}}); return; }
            try { write(response, 200, service.execute(operation, parse(request.body))); }
            catch (const native::Error& error) { write(response, error.status, {{"error", error.what()}}); }
            catch (const native::Json::exception&) { write(response, 400, {{"error", "请求 JSON 或字段无效"}}); }
            catch (const std::exception&) { write(response, 500, {{"error", "服务内部错误"}}); std::cerr << "game service operation failed\n"; }
        });
        server.set_error_handler([](const httplib::Request&, httplib::Response& response) { if (response.body.empty()) write(response, response.status, {{"error", "接口或HTTP方法无效"}}); });
        auto listen = native::env("BACKEND_LISTEN", "127.0.0.1:8081");
        auto colon = listen.rfind(':');
        if (colon == std::string::npos) throw std::runtime_error("invalid BACKEND_LISTEN");
        auto host = listen.substr(0, colon);
        if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
        if (host.empty()) host = "0.0.0.0";
        auto port = port_number(listen.substr(colon + 1));
        if (port < 1 || port > 65535 || !server.bind_to_port(host, port)) throw std::runtime_error("cannot bind backend listener");
        std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
        std::atomic<bool> done{false};
        std::thread monitor([&] {
            unsigned ticks = 0;
            while (!done && !interrupted) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (config.database.driver == "mysql" && ++ticks % 50 == 0) {
                    try { service.health(); } catch (...) { interrupted = 1; }
                }
            }
            if (interrupted) server.stop();
        });
        std::cerr << "native game service listening on " << listen << '\n';
        const bool success = server.listen_after_bind(); done = true; monitor.join();
        return success ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << "native game service startup failed: " << error.what() << '\n'; return 1; }
}
