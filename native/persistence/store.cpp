#include "persistence/store.hpp"
#include <mysql.h>
#include <sqlite3.h>
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <type_traits>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace native::db {
struct Store::Impl {
    MYSQL* mysql = nullptr;
    sqlite3* sqlite = nullptr;
    bool poisoned = false;
    std::int64_t affected = 0;
#ifdef _WIN32
    HANDLE file_lock = INVALID_HANDLE_VALUE;
#else
    int file_lock = -1;
#endif
    ~Impl() {
        if (mysql) mysql_close(mysql);
        if (sqlite) sqlite3_close_v2(sqlite);
#ifdef _WIN32
        if (file_lock != INVALID_HANDLE_VALUE) CloseHandle(file_lock);
#else
        if (file_lock >= 0) close(file_lock);
#endif
    }
    [[noreturn]] void mysql_error(unsigned number = 0) {
        if (!number) number = mysql_errno(mysql);
        if (number >= 2000) poisoned = true;
        // 错误文本可能带SQL中的参数；只向日志暴露错误编号。
        throw Error(static_cast<int>(number), "MySQL operation failed (" + std::to_string(number) + ")");
    }
};
namespace {
std::string lock_name(const Config& c) {
    std::string db = c.database;
    std::transform(db.begin(), db.end(), db.begin(), [](unsigned char x) { return static_cast<char>(std::tolower(x)); });
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(db.data()), db.size(), hash);
    std::ostringstream out;
    out << c.lock_prefix << std::hex << std::setfill('0');
    for (int i = 0; i < 24; ++i) out << std::setw(2) << static_cast<unsigned>(hash[i]);
    return out.str();
}
struct SqliteStatement {
    sqlite3_stmt* value = nullptr;
    ~SqliteStatement() { if (value) sqlite3_finalize(value); }
};
struct MysqlStatement {
    MYSQL_STMT* value = nullptr;
    ~MysqlStatement() { if (value) mysql_stmt_close(value); }
};
void bind_sqlite(sqlite3* db, sqlite3_stmt* st, const Params& params) {
    if (sqlite3_bind_parameter_count(st) != static_cast<int>(params.size())) throw Error(0, "SQL parameter count mismatch");
    for (std::size_t i = 0; i < params.size(); ++i) {
        int code = SQLITE_OK;
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) code = sqlite3_bind_null(st, static_cast<int>(i + 1));
            else if constexpr (std::is_same_v<T, std::int64_t>) code = sqlite3_bind_int64(st, static_cast<int>(i + 1), v);
            else if constexpr (std::is_same_v<T, double>) code = sqlite3_bind_double(st, static_cast<int>(i + 1), v);
            else code = sqlite3_bind_text(st, static_cast<int>(i + 1), v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        }, params[i]);
        if (code != SQLITE_OK) throw Error(sqlite3_extended_errcode(db), "SQLite bind failed");
    }
}
}
Store::Store(const Config& c) : impl_(std::make_unique<Impl>()) {
    if (c.driver == "mysql") {
        if (c.host.empty() || c.user.empty() || c.database.empty() || c.port < 1 || c.port > 65535 ||
            c.host.find_first_of("/@?#\\ \t\r\n") != std::string::npos || (c.tls != "false" && c.tls != "true")) {
            throw Error(0, "invalid MySQL configuration");
        }
        if (std::any_of(c.database.begin(), c.database.end(), [](unsigned char value) { return value >= 128; })) {
            // Go使用Unicode小写；迁移器只接受ASCII库名，避免不同语言生成不同锁名。
            throw Error(0, "MySQL database name must be ASCII for cross-language writer-lock compatibility");
        }
        auto* m = impl_->mysql = mysql_init(nullptr);
        if (!m) throw Error(0, "MySQL initialization failed");
        unsigned timeout = 5;
        unsigned protocol = MYSQL_PROTOCOL_TCP;
        my_bool reconnect = 0;
        // 与旧 Go Net=tcp 一致，localhost 也必须尊重可配置的隧道端口。
        if (mysql_options(m, MYSQL_OPT_PROTOCOL, &protocol)) impl_->mysql_error();
        mysql_options(m, MYSQL_OPT_RECONNECT, &reconnect);
        mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        mysql_options(m, MYSQL_OPT_READ_TIMEOUT, &timeout);
        mysql_options(m, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
        mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        if (c.tls == "true") {
            my_bool verify = 1;
            my_bool enforce = 1;
            mysql_options(m, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);
            mysql_options(m, MYSQL_OPT_SSL_ENFORCE, &enforce);
            mysql_ssl_set(m, nullptr, nullptr, "/etc/ssl/certs/ca-certificates.crt", nullptr, nullptr);
        }
        if (!mysql_real_connect(m, c.host.c_str(), c.user.c_str(), c.password.c_str(), c.database.c_str(), c.port, nullptr, CLIENT_FOUND_ROWS)) impl_->mysql_error();
        auto versions = query("SELECT VERSION()");
        auto version = text(versions.at(0), 0);
        unsigned major = 0, minor = 0, patch = 0;
        std::string lower = version;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char x) { return static_cast<char>(std::tolower(x)); });
        if (std::sscanf(version.c_str(), "%u.%u.%u", &major, &minor, &patch) != 3 || lower.find("mariadb") != std::string::npos || major < 8 || (major == 8 && minor == 0 && patch < 16)) throw Error(0, "requires MySQL 8.0.16 or newer");
        auto locked = query("SELECT GET_LOCK(?,0)", {lock_name(c)});
        if (locked.empty() || !locked[0][0] || integer(locked[0], 0) != 1) throw Error(0, "database already owned by another writer");
    } else if (c.driver == "sqlite") {
        if (c.path.empty()) throw Error(0, "SQLite path is required");
        auto absolute = std::filesystem::absolute(c.path);
        std::filesystem::create_directories(absolute.parent_path());
        auto canonical = std::filesystem::weakly_canonical(absolute);
        // Auth和Backend写各自的表，允许共用兼容SQLite库；Backend沿用旧文件锁名。
        auto lock = canonical.string() + (c.lock_prefix == "mmo-auth:" ? ".auth.lock" : ".lock");
#ifdef _WIN32
        impl_->file_lock = CreateFileA(lock.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        OVERLAPPED overlapped{};
        if (impl_->file_lock == INVALID_HANDLE_VALUE || !LockFileEx(impl_->file_lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &overlapped)) throw Error(0, "SQLite database already owned by another writer");
#else
        impl_->file_lock = open(lock.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (impl_->file_lock < 0 || flock(impl_->file_lock, LOCK_EX | LOCK_NB) != 0) throw Error(0, "SQLite database already owned by another writer");
#endif
        if (sqlite3_open_v2(canonical.string().c_str(), &impl_->sqlite, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) throw Error(0, "SQLite open failed");
        sqlite3_extended_result_codes(impl_->sqlite, 1);
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA synchronous=FULL");
        exec("PRAGMA busy_timeout=5000");
        exec("PRAGMA foreign_keys=ON");
    } else throw Error(0, "database driver must be sqlite or mysql");
}
Store::~Store() = default;
bool Store::is_mysql() const { return impl_->mysql != nullptr; }
void Store::ping() {
    if (impl_->poisoned) throw Error(0, "database connection lost; restart required");
    if (is_mysql()) { if (mysql_ping(impl_->mysql)) impl_->mysql_error(); }
    else query("SELECT 1");
}

// MySQL和SQLite都使用真正的预处理参数，不把玩家输入拼接进SQL。
Rows Store::query(const std::string& sql, const Params& params) {
    if (impl_->poisoned) throw Error(0, "database connection lost; restart required");
    Rows rows;
    if (!is_mysql()) {
        SqliteStatement st;
        if (sqlite3_prepare_v2(impl_->sqlite, sql.c_str(), static_cast<int>(sql.size()), &st.value, nullptr) != SQLITE_OK) throw Error(sqlite3_extended_errcode(impl_->sqlite), "SQLite prepare failed");
        bind_sqlite(impl_->sqlite, st.value, params);
        int code;
        while ((code = sqlite3_step(st.value)) == SQLITE_ROW) {
            Row row;
            for (int column = 0; column < sqlite3_column_count(st.value); ++column) {
                if (sqlite3_column_type(st.value, column) == SQLITE_NULL) row.emplace_back(std::nullopt);
                else row.emplace_back(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st.value, column)), static_cast<std::size_t>(sqlite3_column_bytes(st.value, column))));
            }
            rows.push_back(std::move(row));
        }
        if (code != SQLITE_DONE) throw Error(sqlite3_extended_errcode(impl_->sqlite), "SQLite operation failed");
        return rows;
    }
    if (params.empty()) {
        // 不带外部参数的固定语句使用COM_QUERY；KILL等运维语句不支持服务端预处理。
        if (mysql_real_query(impl_->mysql, sql.data(), static_cast<unsigned long>(sql.size()))) impl_->mysql_error();
        impl_->affected = static_cast<std::int64_t>(mysql_affected_rows(impl_->mysql));
        auto* raw = mysql_store_result(impl_->mysql);
        if (!raw) {
            if (mysql_field_count(impl_->mysql)) impl_->mysql_error();
            return rows;
        }
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(raw, mysql_free_result);
        const unsigned count = mysql_num_fields(raw);
        while (auto values = mysql_fetch_row(raw)) {
            auto* lengths = mysql_fetch_lengths(raw);
            Row row;
            for (unsigned i = 0; i < count; ++i) {
                if (!values[i]) row.emplace_back(std::nullopt);
                else {
                    if (lengths[i] > 1024 * 1024) throw Error(0, "database result too large");
                    row.emplace_back(std::string(values[i], lengths[i]));
                }
            }
            rows.push_back(std::move(row));
        }
        if (mysql_errno(impl_->mysql)) impl_->mysql_error();
        return rows;
    }
    MysqlStatement st;
    st.value = mysql_stmt_init(impl_->mysql);
    if (!st.value) throw Error(0, "MySQL statement initialization failed");
    auto fail = [&]() { impl_->mysql_error(mysql_stmt_errno(st.value)); };
    if (mysql_stmt_prepare(st.value, sql.data(), static_cast<unsigned long>(sql.size()))) fail();
    if (mysql_stmt_param_count(st.value) != params.size()) throw Error(0, "SQL parameter count mismatch");
    std::vector<MYSQL_BIND> bindings(params.size());
    std::vector<unsigned long> lengths(params.size());
    std::vector<my_bool> nulls(params.size());
    for (std::size_t i = 0; i < params.size(); ++i) {
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) { bindings[i].buffer_type = MYSQL_TYPE_NULL; nulls[i] = 1; bindings[i].is_null = &nulls[i]; }
            else if constexpr (std::is_same_v<T, std::int64_t>) { bindings[i].buffer_type = MYSQL_TYPE_LONGLONG; bindings[i].buffer = const_cast<std::int64_t*>(&v); }
            else if constexpr (std::is_same_v<T, double>) { bindings[i].buffer_type = MYSQL_TYPE_DOUBLE; bindings[i].buffer = const_cast<double*>(&v); }
            else { bindings[i].buffer_type = MYSQL_TYPE_STRING; bindings[i].buffer = const_cast<char*>(v.data()); lengths[i] = static_cast<unsigned long>(v.size()); bindings[i].buffer_length = lengths[i]; bindings[i].length = &lengths[i]; }
        }, params[i]);
    }
    if (!params.empty() && mysql_stmt_bind_param(st.value, bindings.data())) fail();
    my_bool update_lengths = 1;
    mysql_stmt_attr_set(st.value, STMT_ATTR_UPDATE_MAX_LENGTH, &update_lengths);
    if (mysql_stmt_execute(st.value)) fail();
    impl_->affected = static_cast<std::int64_t>(mysql_stmt_affected_rows(st.value));
    if (mysql_stmt_field_count(st.value) == 0) return rows;
    if (mysql_stmt_store_result(st.value)) fail();
    auto* metadata = mysql_stmt_result_metadata(st.value);
    if (!metadata) fail();
    std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> meta(metadata, mysql_free_result);
    const unsigned count = mysql_num_fields(metadata);
    auto* fields = mysql_fetch_fields(metadata);
    std::vector<std::vector<char>> buffers(count);
    std::vector<MYSQL_BIND> outputs(count);
    std::vector<unsigned long> result_lengths(count);
    std::vector<my_bool> result_nulls(count), errors(count);
    for (unsigned i = 0; i < count; ++i) {
        if (fields[i].max_length > 1024 * 1024) throw Error(0, "database result too large");
        buffers[i].resize(std::max<unsigned long>(fields[i].max_length, 1));
        outputs[i].buffer_type = MYSQL_TYPE_STRING;
        outputs[i].buffer = buffers[i].data();
        outputs[i].buffer_length = static_cast<unsigned long>(buffers[i].size());
        outputs[i].length = &result_lengths[i];
        outputs[i].is_null = &result_nulls[i];
        outputs[i].error = &errors[i];
    }
    if (mysql_stmt_bind_result(st.value, outputs.data())) fail();
    int code;
    while ((code = mysql_stmt_fetch(st.value)) != MYSQL_NO_DATA) {
        if (code != 0) fail();
        Row row;
        for (unsigned i = 0; i < count; ++i) {
            if (result_nulls[i]) row.emplace_back(std::nullopt);
            else if (errors[i] || result_lengths[i] > buffers[i].size()) throw Error(0, "database result truncated");
            else row.emplace_back(std::string(buffers[i].data(), result_lengths[i]));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}
std::int64_t Store::exec(const std::string& sql, const Params& params) {
    query(sql, params);
    if (!is_mysql()) return sqlite3_changes64(impl_->sqlite);
    // mysql_affected_rows反映最近已执行的语句（CLIENT_FOUND_ROWS返回匹配行）。
    return impl_->affected;
}
}
