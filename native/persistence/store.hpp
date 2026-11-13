#pragma once
#include <cstdint>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace native::db {
using Value = std::variant<std::nullptr_t, std::int64_t, double, std::string>;
using Params = std::vector<Value>;
using Row = std::vector<std::optional<std::string>>;
using Rows = std::vector<Row>;
struct Config {
    std::string driver = "sqlite", path;
    std::string host, user, password, database, tls = "false";
    unsigned port = 3306;
    std::string lock_prefix = "mmo-backend:";
};
class Error : public std::runtime_error {
public:
    int code;
    Error(int number, const std::string& message) : std::runtime_error(message), code(number) {}
    bool unique() const { return code == 1062 || code == 1555 || code == 2067; }
};
class Store {
public:
    explicit Store(const Config& config);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    std::int64_t exec(const std::string& sql, const Params& params = {});
    Rows query(const std::string& sql, const Params& params = {});
    void ping();
    bool is_mysql() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class Tx {
public:
    explicit Tx(Store& store) : store_(store) { store_.exec("BEGIN"); }
    ~Tx() { if (!done_) { try { store_.exec("ROLLBACK"); } catch (...) {} } }
    Tx(const Tx&) = delete;
    Tx& operator=(const Tx&) = delete;
    void commit() { store_.exec("COMMIT"); done_ = true; }
private:
    Store& store_;
    bool done_ = false;
};
inline const std::string& text(const Row& row, std::size_t index) {
    if (index >= row.size() || !row[index]) throw Error(0, "database returned unexpected NULL");
    return *row[index];
}
inline std::int64_t integer(const Row& row, std::size_t index) {
    const auto& value = text(row, index);
    std::size_t used = 0;
    try { auto parsed = std::stoll(value, &used); if (used == value.size()) return parsed; } catch (...) {}
    throw Error(0, "database returned invalid signed integer");
}
inline double number(const Row& row, std::size_t index) {
    const auto& value = text(row, index);
    std::size_t used = 0;
    try { auto parsed = std::stod(value, &used); if (used == value.size() && std::isfinite(parsed)) return parsed; } catch (...) {}
    throw Error(0, "database returned invalid finite number");
}
}
