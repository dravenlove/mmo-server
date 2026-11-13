#include "game_service/service.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <future>
#include <atomic>
#include <cstdlib>
#include <sstream>
#include <fstream>

using native::Json;
namespace {
using I = std::int64_t;
bool mysql_tests = false;
std::string env(const char* key, const std::string& fallback = {}) { const auto* value = std::getenv(key); return value && *value ? value : fallback; }
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void status(int expected, F action) {
    try { action(); throw std::runtime_error("expected request failure"); }
    catch (const native::Error& e) { if (e.status != expected) throw; }
}
struct Player { Json role; std::string state, transfer; };
struct FakeScene {
    httplib::Server server;
    std::thread thread;
    std::mutex mutex;
    std::map<std::string, Player> players;
    std::map<std::string, I> tombstones;
    std::map<std::string, int> before, after;
    std::string url;
    explicit FakeScene(const std::string& token) {
        server.new_task_queue = [] { return new httplib::ThreadPool(4, 4, 16); };
        server.set_keep_alive_max_count(1);
        server.Post(R"(/v1/(.*))", [this, token](const httplib::Request& request, httplib::Response& response) {
            if (request.get_header_value("X-Internal-Token") != token) { response.status = 401; return; }
            std::lock_guard<std::mutex> guard(mutex);
            const auto op = request.matches[1].str();
            if (before[op] > 0) { --before[op]; response.status = 503; return; }
            const auto data = Json::parse(request.body);
            const Json role = data.value("role", Json::object());
            const auto ref = op == "enter" || op == "prepare" ? role : data;
            const auto id = ref.value("role_id", std::string{});
            auto it = players.find(id);
            bool exists = it != players.end();
            Player player = exists ? it->second : Player{};
            auto same = [&] { return exists && player.role["owner_epoch"] == ref["owner_epoch"] && player.role["session_epoch"] == ref["session_epoch"]; };
            auto transfer = data.value("transfer_id", std::string{});
            Json result{{"accepted", true}};
            if (op == "enter") {
                if (exists) {
                    if (player.state != "active" || player.role["owner_epoch"] != ref["owner_epoch"] || player.role["session_epoch"].get<I>() > ref["session_epoch"].get<I>()) { response.status = 409; return; }
                    player.role["session_epoch"] = ref["session_epoch"]; player.role["coins"] = ref["coins"];
                } else {
                    if (tombstones[id] >= ref["owner_epoch"].get<I>()) { response.status = 409; return; }
                    player = {role, "active", {}};
                }
                players[id] = player; result = {{"role", player.role}};
            } else if (op == "prepare") {
                if (exists) { if (!same() || player.transfer != transfer) { response.status = 409; return; } }
                else { if (tombstones[id] >= ref["owner_epoch"].get<I>()) { response.status = 409; return; } player = {role, "prepared", transfer}; players[id] = player; }
                result = {{"role", player.role}};
            } else if (op == "freeze") {
                if (!exists) { response.status = 404; return; }
                if (!same() || (player.state == "frozen" && player.transfer != transfer)) { response.status = 409; return; }
                player.state = "frozen"; player.transfer = transfer; players[id] = player; result = {{"role", player.role}};
            } else if (op == "unfreeze") {
                if (!exists) { response.status = 404; return; }
                if (!same() || (player.state == "frozen" && player.transfer != transfer)) { response.status = 409; return; }
                player.state = "active"; players[id] = player;
            } else if (op == "activate") {
                if (!exists) { response.status = 404; return; }
                if (!same() || player.transfer != transfer) { response.status = 409; return; }
                player.state = "active"; players[id] = player; result = {{"role", player.role}};
            } else if (op == "remove") {
                if (exists && !same()) { response.status = 409; return; }
                players.erase(id); tombstones[id] = std::max(tombstones[id], ref["owner_epoch"].get<I>());
            } else { response.status = 404; return; }
            if (after[op] > 0) { --after[op]; response.status = 503; return; }
            response.set_content(result.dump(), "application/json");
        });
        const int port = server.bind_to_any_port("127.0.0.1");
        if (port < 1) throw std::runtime_error("test bind failed");
        url = "http://127.0.0.1:" + std::to_string(port);
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~FakeScene() { server.stop(); thread.join(); }
    void fail(const std::string& op, bool applied, int count = 1) { std::lock_guard<std::mutex> guard(mutex); (applied ? after : before)[op] = count; }
    void progress(const std::string& id, double x) { std::lock_guard<std::mutex> guard(mutex); players.at(id).role["x"] = x; }
};
Json combat(I hp, I revision, I session = 1) {
    return {{"hp", hp}, {"max_hp", 180}, {"mp", 37}, {"max_mp", 100}, {"dead", hp == 0}, {"revive_remaining_ms", 0}, {"revision", revision}, {"last_action_seq", 4}, {"action_session", session}, {"cooldowns", {{"strike", 500}, {"whirlwind", 2500}, {"charge", 6000}, {"mend", 11000}}}};
}
Json transfer_request(const Json& role, const std::string& request_id, const std::string& target = "scene-2") {
    return {{"role_id", role["role_id"]}, {"owner_epoch", role["owner_epoch"]}, {"session_epoch", role["session_epoch"]}, {"target_scene_id", target}, {"request_id", request_id}};
}
struct Fixture {
    std::string token = "native-test-internal-token-at-least-32-characters";
    FakeScene first{token}, second{token};
    native::game::Config config;
    std::unique_ptr<native::game::Service> service;
    std::filesystem::path directory;
    std::string suffix;
    Fixture() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        std::ostringstream hex; hex << std::hex << stamp;
        suffix = hex.str();
        directory = std::filesystem::temp_directory_path() / ("mmo-native-game-" + suffix);
        config.token = token; config.database.path = (directory / "test.db").string(); config.default_scene = "scene-1";
        if (mysql_tests) {
            config.database.driver = "mysql";
            config.database.host = env("MMO_TEST_MYSQL_HOST");
            config.database.port = static_cast<unsigned>(std::stoul(env("MMO_TEST_MYSQL_PORT", "3306")));
            config.database.user = env("MMO_TEST_MYSQL_USER");
            config.database.password = env("MMO_TEST_MYSQL_PASSWORD");
            config.database.database = env("MMO_TEST_MYSQL_DATABASE");
            if (config.database.database.rfind("mmo_native_test_", 0) != 0) throw std::runtime_error("Game MySQL tests require isolated mmo_native_test_ database");
        }
        config.scenes = {{"scene-1", first.url}, {"scene-2", second.url}};
        service = std::make_unique<native::game::Service>(config);
    }
    ~Fixture() { service.reset(); std::filesystem::remove_all(directory); }
    Json login(const std::string& name) {
        const auto account = service->execute("register", {{"name", name.substr(0, 12) + "-" + suffix.substr(suffix.size() - 8)}});
        return service->execute("login", {{"token", account["token"]}})["role"];
    }
    void restart() { service.reset(); service = std::make_unique<native::game::Service>(config); }
};
void account_and_checkpoint() {
    Fixture f;
    const auto name = "玩家-" + f.suffix.substr(f.suffix.size() - 8);
    auto account = f.service->execute("register", {{"name", "  " + name + "  "}});
    auto r = f.service->execute("login", {{"token", account["token"]}})["role"];
    check(r["name"] == name, "legacy trim changed");
    status(409, [&] { f.service->execute("register", {{"name", name}}); });
    const auto case_name = "Case-" + f.suffix.substr(f.suffix.size() - 8);
    const auto lower_name = "case-" + f.suffix.substr(f.suffix.size() - 8);
    f.service->execute("register", {{"name", case_name}});
    f.service->execute("register", {{"name", lower_name}});
    status(400, [&] { f.service->execute("register", {{"name", "x"}, {"extra", true}}); });
    status(401, [&] { f.service->execute("login", {{"token", std::string(64, 'x')}}); });
    const Json request{{"role_id", r["role_id"]}, {"session_epoch", r["session_epoch"]}};
    check(f.service->execute("claim", request)["granted"] == true, "first claim refused");
    check(f.service->execute("claim", request) == Json({{"granted", false}, {"coins", 100}}), "claim duplicated");
    r["x"] = 4; r["coins"] = 999; r["combat"] = combat(71, 8);
    f.service->execute("checkpoint", {{"role", r}});
    f.service->execute("checkpoint", {{"role", r}});
    auto older = r; older["x"] = 99; older["combat"] = combat(180, 7);
    status(409, [&] { f.service->execute("checkpoint", {{"role", older}}); });
    auto stored = f.service->execute("session", request)["role"];
    check(stored["x"] == 4 && stored["combat"]["hp"] == 71 && stored["coins"] == 100, "checkpoint/rollback asset invariant failed");
    f.restart();
    stored = f.service->execute("session", request)["role"];
    check(stored["combat"]["revision"] == 8, "combat lost on restart");
    auto next = f.service->execute("login", {{"token", account["token"]}})["role"];
    check(next["session_epoch"] == 2, "session epoch not advanced");
    status(409, [&] { f.service->execute("checkpoint", {{"role", r}}); });
    status(409, [&] { f.service->execute("session", request); });
    bool locked = false;
    try { native::game::Service duplicate(f.config); } catch (const native::db::Error&) { locked = true; }
    check(locked, "second SQLite writer admitted");
    auto incomplete = combat(180, 1); incomplete["cooldowns"]["arc_bolt"] = 0;
    status(400, [&] { native::game::combat_revision(incomplete); });
}
void migration_paths() {
    Fixture f;
    auto r = f.login("transfer-success"); auto req = transfer_request(r, "success");
    f.first.progress(r["role_id"], 8);
    auto moved = f.service->execute("transfer", req)["role"];
    check(moved["scene_id"] == "scene-2" && moved["owner_epoch"] == 2 && moved["x"] == 8, "transfer lost ownership/snapshot");
    check(f.service->execute("transfer", req)["role"] == moved, "transfer duplicate not idempotent");
    auto mismatched = req; mismatched["target_scene_id"] = "scene-1";
    status(409, [&] { f.service->execute("transfer", mismatched); });
    status(409, [&] { f.service->execute("checkpoint", {{"role", r}}); });

    auto aborted = f.login("transfer-abort");
    f.second.fail("prepare", true);
    auto abort_req = transfer_request(aborted, "abort-after-prepare");
    status(503, [&] { f.service->execute("transfer", abort_req); });
    auto session = f.service->execute("session", {{"role_id", aborted["role_id"]}, {"session_epoch", aborted["session_epoch"]}})["role"];
    check(session["scene_id"] == "scene-1" && session["owner_epoch"] == 1, "aborted ownership changed");
    status(409, [&] { f.service->execute("transfer", abort_req); });
    auto after = f.service->execute("transfer", transfer_request(session, "after-abort"))["role"];
    check(after["owner_epoch"] == 3, "aborted epoch reused");

    auto pending = f.login("transfer-committed"); auto pending_req = transfer_request(pending, "commit-activate-timeout");
    f.second.fail("activate", true);
    status(503, [&] { f.service->execute("transfer", pending_req); });
    status(409, [&] { f.service->execute("checkpoint", {{"role", pending}}); });
    f.second.progress(pending["role_id"], 19);
    f.restart(); f.service->recover();
    auto recovered = f.service->execute("transfer", pending_req)["role"];
    check(recovered["scene_id"] == "scene-2" && recovered["owner_epoch"] == 2 && recovered["x"] == 19, "commit recovery regressed live snapshot");
}
void external_auth_identity_and_lock_boundary() {
    Fixture f;
    httplib::Server auth;
    auth.new_task_queue = [] { return new httplib::ThreadPool(4, 4, 16); };
    auth.set_keep_alive_max_count(1);
    const std::string id = std::string(32 - f.suffix.size(), '0') + f.suffix;
    const std::string stable = id + id, token = id + id;
    const std::string name = "external-" + f.suffix.substr(f.suffix.size() - 8);
    std::atomic<bool> entered{false};
    auth.Post("/v1/authenticate", [&](const httplib::Request&, httplib::Response& response) { response.set_content(Json({{"role_id", id}, {"name", name}, {"legacy_token_hash", stable}}).dump(), "application/json"); });
    auth.Post("/v1/account_login", [&](const httplib::Request&, httplib::Response& response) {
        entered = true; std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto expiry = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 86400000;
        response.set_content(Json({{"role_id", id}, {"name", name}, {"token", token}, {"expires_at_unix_ms", expiry}}).dump(), "application/json");
    });
    const int port = auth.bind_to_any_port("127.0.0.1");
    std::thread thread([&] { auth.listen_after_bind(); }); auth.wait_until_ready();
    struct Stop { httplib::Server& server; std::thread& thread; ~Stop() { server.stop(); thread.join(); } } stop{auth, thread};
    f.config.auth_url = "http://127.0.0.1:" + std::to_string(port); f.restart();
    auto r = f.service->execute("login", {{"token", token}})["role"];
    check(r["role_id"] == id, "external identity changed");
    auto pending = std::async(std::launch::async, [&] { return f.service->execute("account_login", {{"name", name}, {"password", "not-a-real-password"}}); });
    for (int i = 0; i < 100 && !entered; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(entered, "auth request did not enter");
    const auto begin = std::chrono::steady_clock::now();
    f.service->execute("checkpoint", {{"role", r}});
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    pending.get();
    check(elapsed < std::chrono::milliseconds(250), "slow authentication held game state lock");
    f.service.reset();
    {
        native::db::Store store(f.config.database);
        const auto stored = store.query("SELECT token_hash FROM roles WHERE role_id=?", {id});
        check(native::db::text(stored.at(0), 0) == stable, "temporary ticket replaced stable identity hash");
    }
}
void legacy_fixture(const std::string& filename) {
    std::ifstream input(filename);
    if (!input) throw std::runtime_error("cannot read private legacy test fixture");
    Json data; input >> data;
    const auto path = data.at("database").get<std::string>();
    if (std::filesystem::path(path).parent_path().filename().string().rfind("mmo-native-legacy-", 0) != 0) throw std::runtime_error("legacy fixture must use isolated temporary database");
    Fixture f;
    f.service.reset(); f.config.database.path = path;
    f.service = std::make_unique<native::game::Service>(f.config);
    f.service->recover();
    const auto r = f.service->execute("login", {{"token", data.at("token")}})["role"];
    check(r["role_id"] == data.at("role_id") && r["name"] == "legacy-native-player", "legacy identity changed");
    check(r["scene_id"] == "scene-2" && r["owner_epoch"] == 2 && r["session_epoch"] == 2, "legacy committed migration/session did not recover");
    check(r["coins"] == 100 && r["x"] == 7 && r["y"] == 9, "legacy assets/position changed");
    check(r["combat"]["hp"] == 71 && r["combat"]["revision"] == 8 && r["combat"]["cooldowns"]["mend"] == 11000, "legacy combat snapshot changed");
    const auto claim = f.service->execute("claim", {{"role_id", r["role_id"]}, {"session_epoch", r["session_epoch"]}});
    check(claim["granted"] == false && claim["coins"] == 100, "legacy reward duplicated");
}
}
int main(int argc, char** argv) {
    mysql_tests = argc > 1 && std::string(argv[1]) == "--mysql";
    try {
        if (argc == 3 && std::string(argv[1]) == "--legacy-fixture") legacy_fixture(argv[2]);
        else { account_and_checkpoint(); migration_paths(); external_auth_identity_and_lock_boundary(); }
        std::cout << "native game invariants passed\n"; return 0;
    }
    catch (const std::exception& e) { std::cerr << "native game tests failed: " << e.what() << '\n'; return 1; }
}
