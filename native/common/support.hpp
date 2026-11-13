#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace native {
using Json = nlohmann::json;

struct Error : std::runtime_error {
    int status;
    Error(int code, const std::string& message) : std::runtime_error(message), status(code) {}
};

inline std::string env(const char* key, const std::string& fallback = {}) {
    const char* value = std::getenv(key);
    return value && *value ? value : fallback;
}

inline bool secure_equal(const std::string& a, const std::string& b) {
    return !a.empty() && a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

// 内部 HTTP 调用只走受控地址；不跟随重定向，不把鉴权头发送到其他主机。
inline Json http_post(const std::string& url, const std::string& path, const Json& body,
                      const std::string& token, int timeout_ms = 2000) {
    if ((url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) ||
        url.find_first_of("?#@") != std::string::npos)
        throw Error(502, "invalid internal service URL");
    httplib::Client client(url);
    client.set_connection_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
    client.set_read_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
    client.set_write_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
    client.set_max_timeout(std::chrono::milliseconds(timeout_ms));
    client.set_follow_location(false);
    client.set_payload_max_length(2 * 1024 * 1024);
    auto response = client.Post(path, {{"X-Internal-Token", token}}, body.dump(), "application/json");
    if (!response) throw Error(502, "service unavailable or timed out; retry with the same operation id");
    Json result;
    try { result = Json::parse(response->body); }
    catch (...) {
        throw Error(response->status >= 400 ? response->status : 502, "invalid service response");
    }
    if (response->status < 200 || response->status >= 300)
        throw Error(response->status, result.value("error", std::string("service response interrupted")));
    return result;
}
}  // namespace native
