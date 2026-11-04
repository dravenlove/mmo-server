#include "scene.hpp"
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

using mmo::Json;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void fails(int status, const std::function<void()>& action) {
    try { action(); }
    catch (const mmo::Error& e) { check(e.status == status, "unexpected error status"); return; }
    throw std::runtime_error("expected rejection");
}
Json role(std::string id = "alice", int owner = 1, int session = 1, double x = 0) {
    return {{"role_id", id}, {"name", id}, {"scene_id", "scene-1"}, {"owner_epoch", owner},
            {"session_epoch", session}, {"x", x}, {"y", 0}, {"coins", 0}};
}
Json ref(std::string id = "alice", int owner = 1, int session = 1) {
    return {{"role_id", id}, {"owner_epoch", owner}, {"session_epoch", session}};
}
Json transfer(Json r, const char* id) { r["transfer_id"] = id; return r; }

int main() {
    try {
        mmo::Scene scene({});
        scene.execute("/v1/enter", {{"role", role()}});
        scene.execute("/v1/enter", {{"role", role("bob", 1, 1, 10)}});
        scene.execute("/v1/enter", {{"role", role("far", 1, 1, 90)}});
        auto visible = scene.execute("/v1/view", ref())["players"];
        check(visible.size() == 2 && visible[0]["role_id"] == "alice" && visible[1]["role_id"] == "bob", "AOI or ordering incorrect");
        auto move = ref(); move.update({{"seq", 1}, {"dx", 1}, {"dy", 1}});
        scene.execute("/v1/move", move);
        scene.step(0.1);
        scene.step(0.1);
        scene.step(0.1);
        auto position = scene.execute("/v1/view", ref())["self"];
        check(std::abs(std::hypot(position["x"].get<double>(), position["y"].get<double>()) - 1.5) < 1e-9, "normalized movement or input expiry incorrect");
        scene.step(0.5);
        check(scene.execute("/v1/view", ref())["self"]["x"] == position["x"], "expired input continued moving");
        fails(409, [&] { scene.execute("/v1/move", move); });

        auto session2 = role("alice", 1, 2, 99);
        auto updated = scene.execute("/v1/enter", {{"role", session2}})["role"];
        check(updated["x"] == position["x"], "relogin overwrote live position");
        fails(409, [&] { scene.execute("/v1/view", ref()); });
        auto current = ref("alice", 1, 2);
        auto frozen = transfer(current, "transfer-a");
        scene.execute("/v1/freeze", frozen);
        scene.execute("/v1/freeze", frozen);
        fails(409, [&] { scene.execute("/v1/view", current); });
        fails(409, [&] { scene.execute("/v1/enter", {{"role", role("alice", 1, 3)}}); });
        fails(409, [&] { scene.execute("/v1/unfreeze", transfer(current, "wrong")); });
        scene.execute("/v1/unfreeze", frozen);
        scene.execute("/v1/unfreeze", frozen);
        fails(409, [&] { scene.execute("/v1/freeze", frozen); });

        // unfreeze 可能先于超时的 freeze 抵达；取消标记阻止之后再冻结。
        auto cancelled = transfer(current, "cancelled-before-freeze");
        scene.execute("/v1/unfreeze", cancelled);
        fails(409, [&] { scene.execute("/v1/freeze", cancelled); });

        scene.execute("/v1/remove", current);
        fails(409, [&] { scene.execute("/v1/enter", {{"role", role("alice", 1, 9)}}); });
        auto prepared = Json{{"role", role("alice", 3, 2)}, {"transfer_id", "new-transfer"}};
        scene.execute("/v1/prepare", prepared);
        scene.execute("/v1/prepare", prepared);
        auto next = transfer(ref("alice", 3, 2), "new-transfer");
        fails(409, [&] { scene.execute("/v1/view", next); });
        fails(409, [&] { scene.execute("/v1/enter", {{"role", role("alice", 3, 3)}}); });
        scene.execute("/v1/activate", next);
        scene.execute("/v1/activate", next);
        fails(409, [&] { scene.execute("/v1/remove", current); });
        check(scene.execute("/v1/view", next)["self"]["owner_epoch"] == 3, "stale remove deleted new owner");

        // 即使对象不存在，remove 也须阻止迟到的 prepare。
        scene.execute("/v1/remove", transfer(ref("delayed", 8), "delayed-transfer"));
        fails(409, [&] { scene.execute("/v1/prepare", {{"role", role("delayed", 8)}, {"transfer_id", "delayed-transfer"}}); });
        scene.execute("/v1/prepare", {{"role", role("delayed", 9)}, {"transfer_id", "fresh-transfer"}});

        auto invalid = ref("alice", 3, 2); invalid.update({{"seq", 2}, {"dx", 2}, {"dy", 0}});
        fails(400, [&] { scene.execute("/v1/move", invalid); });
        invalid["dx"] = std::numeric_limits<double>::infinity();
        fails(400, [&] { scene.execute("/v1/move", invalid); });
        invalid["dx"] = 0; invalid["seq"] = 1.5;
        fails(400, [&] { scene.execute("/v1/move", invalid); });
        invalid["seq"] = std::numeric_limits<std::uint64_t>::max();
        fails(400, [&] { scene.execute("/v1/move", invalid); });

        mmo::Config small; small.max_players = 1;
        mmo::Scene limited(small);
        limited.execute("/v1/enter", {{"role", role()}});
        fails(503, [&] { limited.execute("/v1/enter", {{"role", role("full")}}); });

        // 取消记录达到上限后保持角色可用，重连后才能开始新的迁移。
        for (int i = 0; i < 256; ++i) {
            auto cancel = ref(); cancel["transfer_id"] = "cancel-" + std::to_string(i);
            limited.execute("/v1/unfreeze", cancel);
        }
        fails(503, [&] { limited.execute("/v1/freeze", transfer(ref(), "overflow")); });
        limited.execute("/v1/unfreeze", transfer(ref(), "overflow"));
        check(limited.execute("/v1/view", ref())["self"]["role_id"] == "alice", "history bound stranded active role");
        limited.execute("/v1/enter", {{"role", role("alice", 1, 2)}});
        limited.execute("/v1/freeze", transfer(ref("alice", 1, 2), "new-session-transfer"));
        std::cout << "scene state invariants passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "scene test failure: " << e.what() << '\n';
        return 1;
    }
}
