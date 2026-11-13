#!/usr/bin/env python3
"""用已构建镜像验证完整 C++ 容器链路，资源全部位于随机测试 project。"""
import argparse
import json
import os
from pathlib import Path
import secrets
import subprocess
import tempfile

from smoke import free_port
from test_native_stack import WebSocket


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    project = "mmo-native-image-test-" + secrets.token_hex(4)
    environment = dict(os.environ, INTERNAL_TOKEN=secrets.token_hex(32), NATIVE_ALLOWED_ORIGINS="https://native-test.example")
    with tempfile.TemporaryDirectory(prefix="mmo-native-image-") as directory:
        config = Path(directory) / "compose.json"
        raw = subprocess.run(["docker", "compose", "-f", str(root / "deploy/native-local.compose.yaml"),
                              "config", "--format", "json"], cwd=root, env=environment,
                             text=True, capture_output=True, check=True, timeout=30)
        content = json.loads(raw.stdout)
        content.pop("name", None)
        ws_port = free_port()
        for name, service in content["services"].items():
            service.pop("build", None)
            service["image"] = args.image
            service["restart"] = "no"
            service["ports"] = [{"target": 7001, "published": str(ws_port), "host_ip": "127.0.0.1", "protocol": "tcp"}] if name == "gateway" else []
        # config --format json会将Compose默认网络/卷名固化为原project名，测试必须隔离。
        for key, value in content.get("volumes", {}).items():
            value["name"] = project + "-" + key
        for key, value in content.get("networks", {}).items():
            value["name"] = project + "-" + key
        config.write_text(json.dumps(content))
        config.chmod(0o600)
        command = ["docker", "compose", "-p", project, "-f", str(config)]
        peers = []

        def connect():
            client = WebSocket(ws_port)
            peers.append(client)
            return client

        def define(client, skill_id):
            view = client.request("view")
            result = client.request("skill_init", world_epoch=view["world_epoch"], skill_id=skill_id,
                                    expected_definition_version=0, definition={"type": "projectile", "damage": 8,
                                    "mp_cost": 0, "cooldown_ms": 100, "range": 20, "speed": 12, "radius": .2})
            assert result["accepted"]
            return view, result["definition"]["definition_version"]

        def cast(client, view, version, skill_id, seq, ok=True):
            return client.request("cast", ok=ok, seq=seq, skill_id=skill_id, world_epoch=view["world_epoch"],
                                  definition_version=version, dx=1, dy=0)

        try:
            subprocess.run(command + ["up", "-d", "--no-build", "--pull", "never", "--wait", "--wait-timeout", "90"], check=True, timeout=120)
            peer = connect()
            account = peer.request("account_register", name="container-" + secrets.token_hex(4), password="synthetic-password-123!")
            role = peer.request("login", token=account["token"])
            assert role["role_id"] == account["role_id"]
            peer.request("move", seq=1, dx=1, dy=0)
            view, version = define(peer, "container-bolt")
            assert cast(peer, view, version, "container-bolt", 1)["accepted"]
            print("PASS non-root image: C++ WebSocket -> GameService -> Account/SQLite -> default Scene, move and parameterized cast", flush=True)

            guest = connect()
            guest_account = guest.request("account_register", name="container-guest-" + secrets.token_hex(4), password="synthetic-password-123!")
            guest_role = guest.request("login", token=guest_account["token"])
            room = peer.request("room_create", request_id="container-create", capacity=2)
            assert peer.request("room_create", request_id="container-create", capacity=2)["room_id"] == room["room_id"]
            manifest = json.loads((root / "scripts/gameplay/manifest.json").read_text())
            assert room["script_version"] == manifest["version"] and room["script_sha256"] == manifest["sha256"], "image did not package the expected Lua version"
            joined = peer.request("room_join", room_id=room["room_id"], request_id="container-host-join")
            guest_joined = guest.request("room_join", room_id=room["room_id"], request_id="container-guest-join")
            assert joined["role_id"] == role["role_id"] and joined["owner_epoch"] > role["owner_epoch"] and joined["coins"] == role["coins"]
            assert guest_joined["role_id"] == guest_role["role_id"]
            room_view, room_version = define(peer, "container-room-bolt")
            assert room_view["room"]["room_id"] == room["room_id"] and room_view["room"]["state"] == "lobby"
            assert {p["role_id"] for p in room_view["players"]} == {role["role_id"], guest_role["role_id"]}
            cast(peer, room_view, room_version, "container-room-bolt", 2, ok=False)
            peer.request("room_ready", ready=True)
            peer.request("room_start", ok=False)  # 客人尚未准备。
            guest.request("room_ready", ready=True)
            guest.request("room_start", ok=False)  # 非房主不能开局。
            assert peer.request("room_start")["state"] == "playing"
            room_cast = cast(peer, room_view, room_version, "container-room-bolt", 2)
            assert room_cast["accepted"] and room_cast["projectile"]["id"]
            assert peer.request("view")["room"]["state"] == "playing"
            guest_returned = guest.request("room_leave", room_id=room["room_id"], request_id="container-guest-leave")
            assert guest_returned["role_id"] == guest_role["role_id"] and guest_returned["scene_id"] == "scene-skills"
            assert len(peer.request("view")["players"]) == 1
            assert peer.request("room_finish")["state"] == "finished"
            cast(peer, room_view, room_version, "container-room-bolt", 3, ok=False)
            # finished房间可能已由session轮询送回大厅；省略旧room_id兼容两种合法时序。
            returned = peer.request("room_leave", request_id="container-host-leave")
            assert returned["role_id"] == role["role_id"] and returned["coins"] == role["coins"] and returned["scene_id"] == "scene-skills"
            assert peer.request("view")["scene_id"] == "scene-skills"
            peer.close(); peers.remove(peer)
            peer = connect()
            restored = peer.request("login", token=account["token"])
            assert restored["role_id"] == role["role_id"] and restored["coins"] == role["coins"] and restored["scene_id"] == "scene-skills"
            print("PASS non-root room image: two real accounts, idempotent allocation, join/ready/host gates, packaged Lua/ECS projectile, leave/finish and original-token identity", flush=True)
        except Exception:
            subprocess.run(command + ["logs", "--tail", "40"], timeout=20)
            raise
        finally:
            for peer in peers:
                peer.close()
            # 仅删除此函数刚创建的随机测试project及其卷，不使用真实部署project。
            subprocess.run(command + ["down", "--volumes", "--remove-orphans"], timeout=45, check=True)


if __name__ == "__main__":
    main()
