#!/usr/bin/env python3
"""隔离完整账号→组房→Lua/ECS战斗→离开/重启链路，不读取任何部署配置。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import tempfile
import time
import urllib.error

from smoke import http
from test_native_stack import WebSocket, unique_ports

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    args = parser.parse_args()
    binaries = {"auth": args.build / "account/native-account", "backend": args.build / "game_service/native-game-service",
                "gateway": args.build / "gateway/native-gateway", "worker": args.build / "room/native-worker"}
    inherited = {k: v for k, v in os.environ.items() if not k.startswith(("MYSQL_", "AUTH_", "BACKEND_", "GATEWAY_", "WEBSOCKET_", "WS_", "SCENE_", "ROOM_", "WORKER_", "COMBAT_", "INTERNAL_TOKEN", "TLS_"))}
    with tempfile.TemporaryDirectory(prefix="mmo-room-stack-") as directory:
        work = Path(directory)
        scripts = work / "gameplay"
        shutil.copytree(ROOT / "scripts/gameplay", scripts)
        manifest = scripts / "manifest.json"
        ports = unique_ports(("auth", "backend", "worker", "tcp", "ws", "health"))
        urls = {k: f"http://127.0.0.1:{v}" for k, v in ports.items()}
        common = dict(inherited, INTERNAL_TOKEN=secrets.token_hex(32))
        envs = {
            "auth": dict(common, AUTH_DB_DRIVER="sqlite", AUTH_DB=str(work / "auth.db"), AUTH_LISTEN=f"127.0.0.1:{ports['auth']}"),
            "backend": dict(common, BACKEND_DB_DRIVER="sqlite", BACKEND_DB=str(work / "roles.db"),
                            BACKEND_LISTEN=f"127.0.0.1:{ports['backend']}", AUTH_URL=urls["auth"], ROOM_WORKER_URL=urls["worker"], SCENES="scene-skills=" + urls["worker"]),
            "worker": dict(common, SCENE_HOST="127.0.0.1", SCENE_PORT=str(ports["worker"]), SCENE_ID="scene-skills", SCENE_MODE="skill_lab", ROOM_SCRIPT_MANIFEST=str(manifest)),
            "gateway": dict(common, GATEWAY_LISTEN=f"127.0.0.1:{ports['tcp']}", WEBSOCKET_LISTEN=f"127.0.0.1:{ports['ws']}",
                            GATEWAY_HEALTH_LISTEN=f"127.0.0.1:{ports['health']}", WEBSOCKET_ALLOWED_ORIGINS="https://native-test.example",
                            BACKEND_URL=urls["backend"], GATEWAY_PROFILE="skill_lab", GATEWAY_LOGIN_SCENE="scene-skills")}
        children, logs, peers = {}, [], []

        def stop(name):
            child = children.pop(name, None)
            if child:
                child.terminate()
                try: child.wait(timeout=12)
                except subprocess.TimeoutExpired: child.kill(); child.wait(timeout=5)

        def start(name):
            log = (work / (name + ".log")).open("ab"); logs.append(log)
            child = subprocess.Popen([str(binaries[name].resolve())], cwd=ROOT, env=envs[name], stdout=log, stderr=log)
            children[name] = child
            for _ in range(150):
                if child.poll() is not None: raise AssertionError(name + " exited: " + (work / (name + ".log")).read_text(errors="replace"))
                try:
                    if http(urls["health" if name == "gateway" else name] + "/healthz", timeout=.3)[0] == 200: return
                except (OSError, urllib.error.URLError): pass
                time.sleep(.05)
            raise AssertionError(name + " health timed out")

        def peer():
            p = WebSocket(ports["ws"]); peers.append(p); return p

        def close(p):
            p.close(); peers.remove(p)

        def internal(path, body):
            code, result = http(urls["worker"] + path, body, common["INTERNAL_TOKEN"])
            assert code == 200, (path, code, result)
            return result

        def define(p):
            view = p.request("view")
            definition = {"type": "projectile", "damage": 9, "mp_cost": 3, "cooldown_ms": 100, "range": 20, "speed": 12, "radius": .2}
            result = p.request("skill_init", skill_id="shared-id", definition=definition, expected_definition_version=0, world_epoch=view["world_epoch"])
            assert result["accepted"], result
            return view, result["definition"]["definition_version"]

        def cast(p, view, version, seq, ok=True):
            return p.request("cast", ok=ok, skill_id="shared-id", definition_version=version,
                             world_epoch=view["world_epoch"], seq=seq, dx=1, dy=0)

        try:
            for name in ("auth", "worker", "backend", "gateway"): start(name)
            accounts, roles, players = [], [], []
            for index in range(3):
                p = peer(); players.append(p)
                account = p.request("account_register", name=f"RoomTest-{index}", password="synthetic-room-password-123!")
                accounts.append(account); roles.append(p.request("login", token=account["token"]))
            a, b, c = players
            code, reward = http(urls["backend"] + "/v1/claim", {"role_id": roles[0]["role_id"], "session_epoch": roles[0]["session_epoch"]}, common["INTERNAL_TOKEN"])
            assert code == 200 and reward["coins"] == 100
            room_a = a.request("room_create", request_id="create-a", capacity=2)
            assert a.request("room_create", request_id="create-a", capacity=2)["room_id"] == room_a["room_id"]
            a.request("room_create", ok=False, request_id="create-a", capacity=3)
            room_b = b.request("room_create", request_id="create-b", capacity=2)
            ar = a.request("room_join", room_id=room_a["room_id"], request_id="join-a")
            assert a.request("room_join", room_id=room_a["room_id"], request_id="join-a")["owner_epoch"] == ar["owner_epoch"]
            b.request("room_join", room_id=room_b["room_id"], request_id="join-b")
            c.request("room_join", room_id=room_a["room_id"], request_id="join-c")
            av, version_a = define(a)
            bv, version_b = define(b)
            assert av["world_epoch"] != bv["world_epoch"]
            assert len(av["players"]) == 2 and len(bv["players"]) == 1
            cast(a, av, version_a, 1, ok=False)
            a.request("room_ready", ready=True)
            a.request("room_start", ok=False)
            c.request("room_ready", ready=True)
            c.request("room_start", ok=False)
            a.request("room_start")
            b.request("room_ready", ready=True); b.request("room_start")
            c.request("move", seq=1, dx=0, dy=-1); time.sleep(.16); c.request("move", seq=2, dx=0, dy=0)
            assert cast(a, av, version_a, 1)["accepted"]
            assert b.request("view")["projectiles"] == []
            assert a.request("view")["self"]["coins"] == 100
            print("PASS password accounts, idempotent allocation/entry, ready/host gates, two real ECS rooms and isolated Lua projectile", flush=True)

            # 发布新规则只影响随后创建的房间；旧房仍可按原脚本施法。
            source = "return {plan=function(ctx) host.deny('new-room-policy') end}\n"
            (scripts / "skills_v2.lua").write_text(source)
            policy = {"version": "skills-v2", "file": "skills_v2.lua", "sha256": hashlib.sha256(source.encode()).hexdigest()}
            (scripts / "versions/skills-v2.json").write_text(json.dumps(policy))
            manifest.write_text(json.dumps(policy))
            internal("/v1/rooms/scripts/reload", {})
            room_v2 = b.request("room_create", request_id="create-v2", capacity=2)
            assert room_v2["script_version"] == "skills-v2"
            assert a.request("room_status")["script_version"] == "skills-v1"
            b.request("room_leave", request_id="leave-b")
            b.request("room_join", room_id=room_v2["room_id"], request_id="join-v2")
            denied_view, denied_version = define(b)
            b.request("room_ready", ready=True); b.request("room_start")
            before = b.request("view")["self"]["combat"]
            denied = cast(b, denied_view, denied_version, 1)
            after = b.request("view")
            assert denied["accepted"] is False
            assert after["self"]["combat"]["last_action_seq"] == before["last_action_seq"]
            assert after["self"]["combat"]["mp"] == before["mp"] and after["projectiles"] == []
            time.sleep(.12)
            assert cast(a, av, version_a, 2)["accepted"]
            print("PASS new-room-only script publication, old-room pinning and no partial MP/sequence/instance commit on Lua denial", flush=True)

            old_epoch = a.request("view")["world_epoch"]
            for p in list(peers): close(p)
            time.sleep(.4)
            for name in ("gateway", "backend", "worker"): stop(name)
            for name in ("worker", "backend", "gateway"): start(name)
            a, b, c = peer(), peer(), peer()
            restored = a.request("login", token=accounts[0]["token"])
            assert restored["role_id"] == roles[0]["role_id"] and restored["coins"] == 100 and restored["scene_id"] == room_a["room_id"]
            restored_world = a.request("view")
            assert restored_world["world_epoch"] != old_epoch and restored_world["projectiles"] == []
            assert a.request("room_status")["script_version"] == "skills-v1"
            b.request("login", token=accounts[1]["token"])
            assert b.request("room_status")["script_version"] == "skills-v2"
            c.request("login", token=accounts[2]["token"])
            print("PASS Backend/Worker/Gateway restart keeps original identity, coins, room routing and exact archived script versions", flush=True)

            a.request("room_finish")
            close(a); time.sleep(.2)
            a = peer()
            returned = a.request("login", token=accounts[0]["token"])
            assert returned["scene_id"] == "scene-skills" and returned["coins"] == 100 and returned["role_id"] == roles[0]["role_id"]
            assert a.request("view")["scene_id"] == "scene-skills"
            close(c); time.sleep(1.3)
            c = peer()
            returned_c = c.request("login", token=accounts[2]["token"])
            assert returned_c["scene_id"] == "scene-skills" and returned_c["role_id"] == roles[2]["role_id"]
            c.request("view")
            print("PASS finished live-room and collected-room login return safely to original lobby without resetting accounts/assets", flush=True)
        except Exception:
            for name in children:
                print("fixture", name, (work / (name + ".log")).read_text(errors="replace")[-2500:], flush=True)
            raise
        finally:
            for p in list(peers): close(p)
            for name in reversed(list(children)): stop(name)
            for log in logs: log.close()


if __name__ == "__main__":
    main()
