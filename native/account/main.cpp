#include "account.hpp"
#include <httplib.h>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
std::atomic<bool> stopping{false};
void signal_stop(int) { stopping.store(true); }
std::string env(const char* key, const char* fallback = "") {
    const auto* value = std::getenv(key);
    return value && *value ? value : fallback;
}
std::string password() {
    const auto file = env("MYSQL_PASSWORD_FILE");
    if (file.empty()) return env("MYSQL_PASSWORD");
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read MYSQL_PASSWORD_FILE");
    std::string result((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}
unsigned port(const std::string& value) {
    std::size_t used = 0;
    auto result = std::stoul(value, &used);
    if (used != value.size() || result < 1 || result > 65535) throw std::runtime_error("invalid port");
    return static_cast<unsigned>(result);
}
void respond(httplib::Response& response, int status, const native::account::Json& body) {
    response.status = status;
    response.set_content(body.dump(), "application/json; charset=utf-8");
}
}

int main(int argc, char** argv) {
#ifndef _WIN32
    umask(0077);
#endif
    // 容器由原 Auth 镜像切换时保留 -role auth 也可启动。
    if (argc > 1 && !(argc == 3 && std::string(argv[1]) == "-role" && std::string(argv[2]) == "auth")) {
        std::cerr << "usage: native-account [-role auth]\n";
        return 2;
    }
    try {
        native::account::Config config;
        config.token = env("INTERNAL_TOKEN");
        config.database.driver = env("AUTH_DB_DRIVER", "mysql");
        config.database.path = env("AUTH_DB", "data/auth.db");
        config.database.host = env("MYSQL_HOST");
        config.database.port = port(env("MYSQL_PORT", "3306"));
        config.database.user = env("MYSQL_USER");
        config.database.password = password();
        config.database.database = env("MYSQL_DATABASE");
        config.database.tls = env("MYSQL_TLS", "false");
        native::account::AccountService service(config);
        httplib::Server server;
        server.new_task_queue = [] { return new httplib::ThreadPool(8, 8, 64); };
        server.set_payload_max_length(64 * 1024);
        server.set_read_timeout(5, 0);
        server.set_write_timeout(10, 0);
        server.set_keep_alive_timeout(30);
        server.set_keep_alive_max_count(1);
        const std::vector<std::string> operations = {"register", "authenticate", "status", "account_register", "account_login", "account_bind"};
        server.set_pre_routing_handler([&](const httplib::Request& request, httplib::Response& response) {
            std::size_t header_bytes = request.method.size() + request.target.size() + request.version.size() + 4;
            for (const auto& header : request.headers) header_bytes += header.first.size() + header.second.size() + 4;
            if (header_bytes > 16 * 1024) {
                respond(response, 431, {{"error", "请求头过大"}});
                return httplib::Server::HandlerResponse::Handled;
            }
            bool known = request.path == "/healthz";
            for (const auto& op : operations) known = known || request.path == "/v1/" + op;
            if (known && ((request.path == "/healthz" && request.method != "GET") ||
                (request.path != "/healthz" && request.method != "POST"))) {
                respond(response, 405, {{"error", "请求方法无效"}});
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server.Get("/healthz", [&](const httplib::Request&, httplib::Response& response) {
            try {
                service.health();
                respond(response, 200, {{"ok", true}, {"service", "auth"}});
            } catch (...) { respond(response, 503, {{"error", "鉴权数据库不可用"}}); }
        });
        for (const auto& operation : operations) {
            server.Post("/v1/" + operation, [&, operation](const httplib::Request& request, httplib::Response& response) {
                if (!native::account::equal_secret(request.get_header_value("X-Internal-Token"), config.token)) {
                    respond(response, 401, {{"error", "内部服务凭据无效"}});
                    return;
                }
                try {
                    auto result = service.execute(operation, native::account::parse_request(request.body));
                    respond(response, 200, result);
                } catch (const native::account::ApiError& error) {
                    respond(response, error.status, {{"error", error.what()}});
                } catch (...) {
                    // SQL、密码、token 及驱动错误不进入公网响应或请求日志。
                    respond(response, 503, {{"error", "鉴权服务暂不可用"}});
                }
            });
        }
        server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
            if (response.status == 413) respond(response, 400, {{"error", "请求过大或读取失败"}});
            if (response.body.empty()) respond(response, response.status, {{"error", "接口不存在或请求无效"}});
        });
        auto listen = env("AUTH_LISTEN", "127.0.0.1:8082");
        const auto delimiter = listen.rfind(':');
        if (delimiter == std::string::npos) throw std::runtime_error("invalid AUTH_LISTEN");
        auto host = listen.substr(0, delimiter);
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
        const auto listen_port = port(listen.substr(delimiter + 1));
        if (!server.bind_to_port(host, listen_port)) throw std::runtime_error("cannot bind AUTH_LISTEN");
        std::signal(SIGINT, signal_stop);
        std::signal(SIGTERM, signal_stop);
        std::atomic<bool> unhealthy{false};
        std::thread supervisor([&] {
            unsigned ticks = 0;
            while (!stopping.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (++ticks < 50) continue;
                ticks = 0;
                try { service.health(); }
                catch (...) { unhealthy.store(true); stopping.store(true); }
            }
            server.stop();
        });
        std::cout << "C++ AccountService listening=" << listen << "\n";
        server.listen_after_bind();
        stopping.store(true);
        supervisor.join();
        return unhealthy.load() ? 1 : 0;
    } catch (...) {
        std::cerr << "AccountService startup failed; check database configuration, exclusive ownership and bcrypt support\n";
        return 1;
    }
}
