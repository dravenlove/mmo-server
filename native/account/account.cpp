#include "account.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <crypt.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

namespace native::account {
namespace {
struct Rune { std::uint32_t value; std::size_t begin, end; };
std::vector<Rune> utf8(const std::string& text) {
    std::vector<Rune> result;
    for (std::size_t pos = 0; pos < text.size();) {
        const auto begin = pos;
        const auto lead = static_cast<unsigned char>(text[pos++]);
        std::uint32_t point;
        unsigned extra;
        if (lead < 0x80) { point = lead; extra = 0; }
        else if (lead >= 0xc2 && lead <= 0xdf) { point = lead & 0x1f; extra = 1; }
        else if (lead >= 0xe0 && lead <= 0xef) { point = lead & 0x0f; extra = 2; }
        else if (lead >= 0xf0 && lead <= 0xf4) { point = lead & 7; extra = 3; }
        else throw ApiError(400, "需要有效的UTF-8文本");
        for (unsigned n = 0; n < extra; ++n) {
            if (pos == text.size()) throw ApiError(400, "需要有效的UTF-8文本");
            auto next = static_cast<unsigned char>(text[pos++]);
            if ((next & 0xc0) != 0x80) throw ApiError(400, "需要有效的UTF-8文本");
            point = (point << 6) | (next & 0x3f);
        }
        if ((extra == 1 && point < 0x80) || (extra == 2 && point < 0x800) ||
            (extra == 3 && point < 0x10000) || point > 0x10ffff ||
            (point >= 0xd800 && point <= 0xdfff)) throw ApiError(400, "需要有效的UTF-8文本");
        result.push_back({point, begin, pos});
    }
    return result;
}
bool control(std::uint32_t point) { return point < 0x20 || (point >= 0x7f && point <= 0x9f); }
bool space(std::uint32_t point) {
    return (point >= 9 && point <= 13) || point == 0x20 || point == 0x85 || point == 0xa0 ||
        point == 0x1680 || (point >= 0x2000 && point <= 0x200a) || point == 0x2028 ||
        point == 0x2029 || point == 0x202f || point == 0x205f || point == 0x3000;
}
std::string random_hex(std::size_t bytes) {
    std::vector<unsigned char> raw(bytes);
    if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1)
        throw std::runtime_error("secure random source unavailable");
    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes * 2, '0');
    for (std::size_t n = 0; n < bytes; ++n) {
        result[2*n] = digits[raw[n] >> 4]; result[2*n+1] = digits[raw[n] & 15];
    }
    return result;
}
std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
void fields(const Json& body, std::initializer_list<const char*> allowed) {
    if (!body.is_object()) throw ApiError(400, "请求必须是JSON对象");
    std::set<std::string> names;
    for (auto key : allowed) names.emplace(key);
    for (auto it = body.begin(); it != body.end(); ++it) {
        if (!names.count(it.key()) || (!it->is_null() && !it->is_string()))
            throw ApiError(400, "请求JSON或字段无效");
    }
}
std::string string_field(const Json& body, const char* key) {
    const auto it = body.find(key);
    return it == body.end() || it->is_null() ? std::string() : it->get<std::string>();
}
class PasswordSlot {
public:
    explicit PasswordSlot(std::atomic<unsigned>& count) : count_(count) {
        unsigned value = count_.load();
        do {
            if (value >= 2) throw ApiError(429, "登录服务繁忙，请稍后重试");
        } while (!count_.compare_exchange_weak(value, value + 1));
    }
    ~PasswordSlot() { --count_; }
private:
    std::atomic<unsigned>& count_;
};
constexpr const char* dummy_hash = "$2a$10$N9qo8uLOickgx2ZMRZoMyeIjZAgcfl7p92ldGxad68LJZdL17lhWy";
db::Config account_database(db::Config config) { config.lock_prefix = "mmo-auth:"; return config; }
Config validated_config(Config config) {
    if (config.token.size() < 32) throw std::runtime_error("INTERNAL_TOKEN 至少需要32字符");
    auto trim_ascii = [](std::string value) {
        const auto begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) return std::string();
        return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
    };
    config.database.driver = trim_ascii(config.database.driver);
    config.database.host = trim_ascii(config.database.host);
    std::transform(config.database.driver.begin(), config.database.driver.end(), config.database.driver.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (config.database.driver.empty()) config.database.driver = "mysql";
    return config;
}
}

std::string validated_name(const std::string& original, bool trim_legacy) {
    auto runes = utf8(original);
    auto begin = runes.begin(), end = runes.end();
    if (trim_legacy) {
        while (begin != end && space(begin->value)) ++begin;
        while (begin != end && space((end-1)->value)) --end;
    }
    const auto count = static_cast<std::size_t>(end - begin);
    if (count == 0 || count > 24 || space(begin->value) || space((end-1)->value))
        throw ApiError(400, "账号名需要1到24个字符，且不能包含首尾空格");
    for (auto it = begin; it != end; ++it)
        if (control(it->value)) throw ApiError(400, "账号名不能包含控制字符");
    return original.substr(begin->begin, (end-1)->end - begin->begin);
}
void validate_password(const std::string& password) {
    if (password.size() < 8 || password.size() > 72)
        throw ApiError(400, "密码需要8到72字节的UTF-8文本");
    for (const auto& rune : utf8(password))
        if (control(rune.value)) throw ApiError(400, "密码不能包含控制字符");
}
bool equal_secret(const std::string& left, const std::string& right) {
    return left.size() == right.size() && CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}
std::string hash_token(const std::string& token) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> raw{};
    unsigned length = 0;
    if (EVP_Digest(token.data(), token.size(), raw.data(), &length, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 unavailable");
    constexpr char digits[] = "0123456789abcdef";
    std::string result(length * 2, '0');
    for (unsigned n = 0; n < length; ++n) {
        result[2*n] = digits[raw[n] >> 4]; result[2*n+1] = digits[raw[n] & 15];
    }
    return result;
}
std::string hash_password(const std::string& password) {
    validate_password(password);
    std::array<char, 16> random{};
    if (RAND_bytes(reinterpret_cast<unsigned char*>(random.data()), random.size()) != 1)
        throw std::runtime_error("secure random source unavailable");
    std::array<char, CRYPT_GENSALT_OUTPUT_SIZE> salt{};
    if (!crypt_gensalt_rn("$2b$", 10, random.data(), random.size(), salt.data(), salt.size()))
        throw std::runtime_error("bcrypt is unavailable in libcrypt");
    auto state = std::make_unique<crypt_data>();
    const char* encoded = crypt_r(password.c_str(), salt.data(), state.get());
    if (!encoded || encoded[0] == '*') throw std::runtime_error("bcrypt failed");
    return encoded;
}
bool verify_password(const std::string& encoded, const std::string& password) {
    if (encoded.size() != 60 || (encoded.compare(0, 4, "$2a$") != 0 &&
        encoded.compare(0, 4, "$2b$") != 0 && encoded.compare(0, 4, "$2y$") != 0)) return false;
    auto state = std::make_unique<crypt_data>();
    const char* result = crypt_r(password.c_str(), encoded.c_str(), state.get());
    return result && result[0] != '*' && equal_secret(encoded, result);
}
Json parse_request(const std::string& body) {
    if (body.empty() || body.size() > 64 * 1024) throw ApiError(400, "请求过大或读取失败");
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 16) throw ApiError(400, "请求JSON嵌套过深");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
            throw ApiError(400, "请求JSON含重复字段");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        if (keys.size() > 16) throw ApiError(400, "请求JSON嵌套过深");
        return true;
    };
    try {
        auto parsed = Json::parse(body, callback);
        if (!parsed.is_object()) throw ApiError(400, "请求必须是JSON对象");
        return parsed;
    } catch (const Json::exception&) { throw ApiError(400, "请求JSON或字段无效"); }
}

AccountService::AccountService(Config config)
    : config_(validated_config(std::move(config))), store_(account_database(config_.database)) {
    schema();
    import_legacy();
    // 启动时发现没有 bcrypt 支持立即失败，避免部署后才拒绝旧账号。
    if (!verify_password(hash_password("startup-bcrypt-check"), "startup-bcrypt-check"))
        throw std::runtime_error("bcrypt self-test failed");
}
void AccountService::schema() {
    if (store_.is_mysql()) {
        store_.exec("CREATE TABLE IF NOT EXISTS auth_accounts (role_id VARBINARY(128) PRIMARY KEY,name VARBINARY(96) NOT NULL UNIQUE,token_hash BINARY(64) NOT NULL UNIQUE) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
        store_.exec("CREATE TABLE IF NOT EXISTS auth_passwords (role_id VARBINARY(128) PRIMARY KEY,password_hash VARBINARY(128) NOT NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
        store_.exec("CREATE TABLE IF NOT EXISTS auth_login_tokens (token_hash BINARY(64) PRIMARY KEY,role_id VARBINARY(128) NOT NULL,created_ms BIGINT NOT NULL,expires_ms BIGINT NOT NULL,INDEX auth_login_tokens_role(role_id,created_ms),INDEX auth_login_tokens_expiry(expires_ms)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
        for (const auto* table : {"auth_accounts", "auth_passwords", "auth_login_tokens"}) {
            auto rows = store_.query("SELECT ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME=?", {std::string(table)});
            if (rows.size() != 1 || db::text(rows.front(), 0) != "InnoDB")
                throw std::runtime_error("账号表必须使用 InnoDB");
        }
    } else {
        store_.exec("CREATE TABLE IF NOT EXISTS auth_accounts (role_id TEXT PRIMARY KEY,name TEXT NOT NULL UNIQUE,token_hash TEXT NOT NULL UNIQUE)");
        store_.exec("CREATE TABLE IF NOT EXISTS auth_passwords (role_id TEXT PRIMARY KEY,password_hash TEXT NOT NULL)");
        store_.exec("CREATE TABLE IF NOT EXISTS auth_login_tokens (token_hash TEXT PRIMARY KEY,role_id TEXT NOT NULL,created_ms INTEGER NOT NULL,expires_ms INTEGER NOT NULL)");
        store_.exec("CREATE INDEX IF NOT EXISTS auth_login_tokens_role ON auth_login_tokens(role_id,created_ms)");
        store_.exec("CREATE INDEX IF NOT EXISTS auth_login_tokens_expiry ON auth_login_tokens(expires_ms)");
    }
}
void AccountService::import_legacy() {
    auto count = store_.query(store_.is_mysql() ?
        "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='roles'" :
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='roles'");
    if (db::integer(count.at(0), 0) == 0) return;
    auto roles = store_.query("SELECT role_id,name,token_hash FROM roles");
    db::Tx transaction(store_);
    for (const auto& role : roles) {
        db::Params values{db::text(role, 0), db::text(role, 1), db::text(role, 2)};
        auto exact = store_.query("SELECT COUNT(*) FROM auth_accounts WHERE role_id=? AND name=? AND token_hash=?", values);
        if (db::integer(exact.at(0), 0) == 1) continue;
        auto conflict = store_.query("SELECT COUNT(*) FROM auth_accounts WHERE role_id=? OR name=? OR token_hash=?", values);
        if (db::integer(conflict.at(0), 0) != 0) throw std::runtime_error("旧账号导入存在身份冲突；未覆盖现有账号");
        store_.exec("INSERT INTO auth_accounts(role_id,name,token_hash) VALUES(?,?,?)", values);
        ++imported_;
    }
    transaction.commit();
}
void AccountService::health() {
    std::lock_guard<std::mutex> lock(database_mutex_);
    store_.ping();
}
bool AccountService::allow(const std::string& key) {
    std::lock_guard<std::mutex> lock(limiter_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (now >= global_.until) global_ = {0, now + std::chrono::minutes(1)};
    if (names_.size() >= 1024) {
        for (auto it = names_.begin(); it != names_.end();) {
            if (now >= it->second.until) it = names_.erase(it); else ++it;
        }
    }
    auto hash = hash_token(key);
    auto it = names_.find(hash);
    Bucket bucket = it == names_.end() ? Bucket{} : it->second;
    if (now >= bucket.until) bucket = {0, now + std::chrono::minutes(1)};
    if (global_.count >= 60 || bucket.count >= 6 || (it == names_.end() && names_.size() >= 1024)) return false;
    ++global_.count; ++bucket.count; names_[hash] = bucket;
    return true;
}
Json AccountService::authenticate(const std::string& token) {
    if (token.size() != 64) throw ApiError(401, "登录token无效");
    auto rows = store_.query("SELECT role_id,name,token_hash FROM auth_accounts WHERE token_hash=?", {hash_token(token)});
    if (rows.empty()) rows = store_.query("SELECT a.role_id,a.name,a.token_hash FROM auth_login_tokens t JOIN auth_accounts a ON a.role_id=t.role_id WHERE t.token_hash=? AND t.expires_ms>?", {hash_token(token), now_ms()});
    if (rows.size() != 1) throw ApiError(401, "登录token无效或已过期");
    return {{"role_id", db::text(rows.front(), 0)}, {"name", db::text(rows.front(), 1)}, {"legacy_token_hash", db::text(rows.front(), 2)}};
}
Json AccountService::issue_ticket(const std::string& role, const std::string& name) {
    const auto now = now_ms(), expiry = now + std::int64_t(24 * 60 * 60 * 1000);
    const auto token = random_hex(32), hash = hash_token(token);
    auto older = store_.query("SELECT token_hash FROM auth_login_tokens WHERE role_id=? ORDER BY created_ms ASC,token_hash ASC", {role});
    if (older.size() >= 5) {
        store_.exec("UPDATE auth_login_tokens SET token_hash=?,created_ms=?,expires_ms=? WHERE token_hash=? AND role_id=?", {hash, now, expiry, db::text(older.front(), 0), role});
    } else store_.exec("INSERT INTO auth_login_tokens(token_hash,role_id,created_ms,expires_ms) VALUES(?,?,?,?)", {hash, role, now, expiry});
    return {{"role_id", role}, {"name", name}, {"token", token}, {"expires_at_unix_ms", expiry}};
}
Json AccountService::legacy_request(const std::string& operation, const Json& request) {
    std::lock_guard<std::mutex> lock(database_mutex_);
    if (operation == "status") {
        fields(request, {});
        const auto rows = store_.query("SELECT COUNT(*) FROM auth_accounts");
        return {{"ok", true}, {"accounts", db::integer(rows.at(0), 0)}, {"imported_legacy", imported_}};
    }
    if (operation == "authenticate") { fields(request, {"token"}); return authenticate(string_field(request, "token")); }
    fields(request, {"name"});
    auto name = validated_name(string_field(request, "name"), true);
    auto rows = store_.query("SELECT COUNT(*) FROM auth_accounts WHERE name=?", {name});
    if (db::integer(rows.at(0), 0)) throw ApiError(409, "名字已被注册；请使用原token登录");
    auto role = random_hex(16), token = random_hex(32);
    store_.exec("INSERT INTO auth_accounts(role_id,name,token_hash) VALUES(?,?,?)", {role, name, hash_token(token)});
    return {{"role_id", role}, {"token", token}};
}
Json AccountService::password_login(const std::string& name, const std::string& password) {
    db::Rows rows;
    {
        std::lock_guard<std::mutex> lock(database_mutex_);
        rows = store_.query("SELECT a.role_id,p.password_hash FROM auth_accounts a JOIN auth_passwords p ON p.role_id=a.role_id WHERE a.name=?", {name});
    }
    auto encoded = rows.empty() ? std::string(dummy_hash) : db::text(rows.front(), 1);
    if (!verify_password(encoded, password) || rows.empty()) throw ApiError(401, "账号或密码错误");
    std::lock_guard<std::mutex> lock(database_mutex_);
    db::Tx transaction(store_);
    auto ticket = issue_ticket(db::text(rows.front(), 0), name);
    transaction.commit();
    return ticket;
}
Json AccountService::password_request(const std::string& operation, const Json& request) {
    fields(request, {"name", "password", "token"});
    const auto name = string_field(request, "name"), token = string_field(request, "token"), password = string_field(request, "password");
    validate_password(password);
    if (operation == "account_bind") {
        if (!name.empty() || token.size() != 64) throw ApiError(400, "绑定密码需要原登录token，不能提交name");
    } else {
        if (!token.empty()) throw ApiError(400, "账号名登录不能提交token");
        validated_name(name);
    }
    if (!allow(operation == "account_bind" ? "bind:" + hash_token(token) : name))
        throw ApiError(429, "账号请求过于频繁，请稍后重试");
    PasswordSlot slot(password_work_);
    if (operation == "account_login") return password_login(name, password);
    const auto encoded = hash_password(password); // 密码CPU工作不占用数据库互斥锁。
    std::lock_guard<std::mutex> lock(database_mutex_);
    if (operation == "account_bind") {
        auto identity = authenticate(token);
        const auto role = identity.at("role_id").get<std::string>();
        auto exists = store_.query("SELECT COUNT(*) FROM auth_passwords WHERE role_id=?", {role});
        if (db::integer(exists.at(0), 0)) throw ApiError(409, "账号已设置密码，本接口不能覆盖已有密码");
        store_.exec("INSERT INTO auth_passwords(role_id,password_hash) VALUES(?,?)", {role, encoded});
        return {{"bound", true}, {"role_id", role}, {"name", identity.at("name")}};
    }
    db::Tx transaction(store_);
    auto exists = store_.query("SELECT COUNT(*) FROM auth_accounts WHERE name=?", {name});
    if (db::integer(exists.at(0), 0)) throw ApiError(409, "账号名已被注册");
    const auto role = random_hex(16);
    // 独立稳定身份哈希，不把临时票据写入原身份列。
    store_.exec("INSERT INTO auth_accounts(role_id,name,token_hash) VALUES(?,?,?)", {role, name, hash_token(random_hex(32))});
    store_.exec("INSERT INTO auth_passwords(role_id,password_hash) VALUES(?,?)", {role, encoded});
    auto ticket = issue_ticket(role, name);
    transaction.commit();
    return ticket;
}
Json AccountService::execute(const std::string& operation, const Json& request) {
    if (operation == "account_register" || operation == "account_login" || operation == "account_bind")
        return password_request(operation, request);
    if (operation == "register" || operation == "authenticate" || operation == "status")
        return legacy_request(operation, request);
    throw ApiError(404, "接口不存在");
}
}
