#pragma once
#include "persistence/store.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace native::account {
using Json = nlohmann::json;
class ApiError : public std::runtime_error {
public:
    int status;
    ApiError(int code, const std::string& message) : std::runtime_error(message), status(code) {}
};

// 与旧 Go Auth 相同的 UTF-8、空白及控制字符语义。
std::string validated_name(const std::string& name, bool trim_legacy = false);
void validate_password(const std::string& password);
std::string hash_token(const std::string& token);
std::string hash_password(const std::string& password);
bool verify_password(const std::string& encoded, const std::string& password);
bool equal_secret(const std::string& left, const std::string& right);
Json parse_request(const std::string& body);

struct Config {
    std::string token;
    db::Config database;
};

class AccountService {
public:
    explicit AccountService(Config config);
    Json execute(const std::string& operation, const Json& request);
    void health();
private:
    struct Bucket {
        unsigned count = 0;
        std::chrono::steady_clock::time_point until{};
    };
    Config config_;
    db::Store store_;
    std::mutex database_mutex_, limiter_mutex_;
    std::atomic<unsigned> password_work_{0};
    Bucket global_;
    std::unordered_map<std::string, Bucket> names_;
    std::int64_t imported_ = 0;

    void schema();
    void import_legacy();
    bool allow(const std::string& key);
    Json authenticate(const std::string& token);
    Json issue_ticket(const std::string& role, const std::string& name);
    Json legacy_request(const std::string& operation, const Json& request);
    Json password_request(const std::string& operation, const Json& request);
    Json password_login(const std::string& name, const std::string& password);
};
}
