#include "persistence/store.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
std::string env(const char* name, const std::string& fallback = {}) { auto* value = std::getenv(name); return value && *value ? value : fallback; }
void check(bool valid, const char* reason) { if (!valid) throw std::runtime_error(reason); }
template<class F> void refuses(F action, const char* reason) { bool failed = false; try { action(); } catch (const native::db::Error&) { failed = true; } check(failed, reason); }
void mysql() {
    native::db::Config config;
    config.driver = "mysql";
    config.host = env("MMO_TEST_MYSQL_HOST");
    config.port = static_cast<unsigned>(std::stoul(env("MMO_TEST_MYSQL_PORT", "3306")));
    config.user = env("MMO_TEST_MYSQL_USER");
    config.password = env("MMO_TEST_MYSQL_PASSWORD");
    config.database = env("MMO_TEST_MYSQL_DATABASE");
    // 测试仅允许明确命名的隔离数据库，不能误连已有业务库。
    if (config.database.rfind("mmo_native_test_", 0) != 0) throw std::runtime_error("MySQL tests require isolated mmo_native_test_ database");
    native::db::Store owner(config);
    owner.exec("CREATE TABLE IF NOT EXISTS native_db_probe (id BIGINT PRIMARY KEY,value BIGINT NOT NULL) ENGINE=InnoDB");
    const auto id = static_cast<std::int64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    owner.exec("INSERT INTO native_db_probe(id,value) VALUES(?,?)", {id, std::int64_t{4}});
    check(owner.exec("UPDATE native_db_probe SET value=? WHERE id=?", {std::int64_t{4}, id}) == 1, "CLIENT_FOUND_ROWS did not preserve unchanged checkpoint matching semantics");
    refuses([&] { native::db::Store duplicate(config); }, "second writer acquired existing Go-compatible lock");
    auto command_config = config; command_config.lock_prefix = "mmo-dbtest:";
    native::db::Store command(command_config);
    const auto connection_id = native::db::integer(owner.query("SELECT CONNECTION_ID()").at(0), 0);
    command.exec("KILL CONNECTION " + std::to_string(connection_id));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto replacement_config = config;
    replacement_config.host = "localhost";
    native::db::Store replacement(replacement_config);
    refuses([&] { owner.exec("UPDATE native_db_probe SET value=? WHERE id=?", {std::int64_t{99}, id}); }, "lost owner reconnected and wrote without lock");
    refuses([&] { owner.ping(); }, "poisoned owner health recovered without process restart");
    refuses([&] { owner.exec("UPDATE native_db_probe SET value=? WHERE id=?", {std::int64_t{99}, id}); }, "poisoned owner resumed writing");
    check(native::db::integer(replacement.query("SELECT value FROM native_db_probe WHERE id=?", {id}).at(0), 0) == 4, "old owner changed data after lock loss");
    check(replacement.exec("UPDATE native_db_probe SET value=? WHERE id=?", {std::int64_t{7}, id}) == 1, "replacement writer failed");
}
void sqlite() {
    const auto dir = std::filesystem::temp_directory_path() / ("mmo-native-db-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    native::db::Config config; config.path = (dir / "test.db").string();
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{dir};
    {
        native::db::Store store(config);
        store.exec("CREATE TABLE probe (id INTEGER PRIMARY KEY,value TEXT NOT NULL UNIQUE)");
        store.exec("INSERT INTO probe(id,value) VALUES(?,?)", {std::int64_t{1}, std::string("a\0b", 3)});
        check(native::db::text(store.query("SELECT value FROM probe WHERE id=?", {std::int64_t{1}}).at(0), 0) == std::string("a\0b", 3), "binary-safe prepared parameters changed");
        check(store.exec("UPDATE probe SET value=? WHERE id=?", {std::string("a\0b", 3), std::int64_t{1}}) == 1, "SQLite unchanged row count changed");
        refuses([&] { native::db::Store duplicate(config); }, "duplicate SQLite writer admitted");
        { native::db::Tx tx(store); store.exec("INSERT INTO probe(id,value) VALUES(?,?)", {std::int64_t{2}, std::string("rollback")}); }
        check(native::db::integer(store.query("SELECT COUNT(*) FROM probe WHERE id=2").at(0), 0) == 0, "transaction did not roll back");
        auto auth_config = config; auth_config.lock_prefix = "mmo-auth:"; native::db::Store auth(auth_config); auth.ping();
    }
    { native::db::Store reopened(config); check(native::db::integer(reopened.query("SELECT COUNT(*) FROM probe").at(0), 0) == 1, "data lost on reopen"); }
}
}
int main(int argc, char** argv) {
    try { if (argc > 1 && std::string(argv[1]) == "--mysql") mysql(); else sqlite(); std::cout << "native database invariants passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << "native database test failed: " << error.what() << '\n'; return 1; }
}
