#!/usr/bin/env python3
"""离线验证实际 Compose 合并结果，防止旧 Go build 被保留或原数据卷/TLS 被改写。"""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parent.parent
    environment = dict(os.environ, INTERNAL_TOKEN="synthetic-compose-validation-key-0000",
                       MMO_NATIVE_IMAGE_TAG="cpp-core-config-test", MMO_IMAGE_TAG="legacy-test",
                       MMO_ROOM_IMAGE_TAG="cpp-room-config-test", MMO_SKILL_LAB_IMAGE_TAG="legacy-skills-test",
                       MMO_IMAGE_PREFIX="ghcr.io/dravenlove/mmo-server", AUTH_URL="http://auth.example:8082",
                       NATIVE_ALLOWED_ORIGINS="https://native-test.example", MYSQL_HOST="db.example",
                       MYSQL_USER="synthetic", MYSQL_PASSWORD="synthetic", MYSQL_DATABASE="synthetic",
                       BACKEND_VOLUME_NAME="synthetic-preserved-data",
                       ROOM_GAMEPLAY_DIR=str(root / "scripts/gameplay"), WORKER_MAX_ROOMS="12",
                       WORKER_MAX_ROOM_PLAYERS="24", WORKER_MAX_PLAYERS="256",
                       TLS_CERT_FILE="/certs/existing.pem", TLS_KEY_FILE="/certs/existing.key")
    environment.pop("MMO_NATIVE_IMAGE_NAME", None)
    with tempfile.TemporaryDirectory(prefix="mmo-compose-check-") as directory:
        empty_env = Path(directory) / "empty.env"
        empty_env.write_text("")
        merged = {}

        def read_compose(files, env=environment):
            command = ["docker", "compose", "--env-file", str(empty_env)]
            for file in files:
                command += ["-f", str(root / "deploy" / file)]
            command += ["config", "--format", "json"]
            return json.loads(subprocess.run(command, env=env, capture_output=True, text=True, check=True, timeout=30).stdout)

        for role in ("game", "auth"):
            config = read_compose([f"{role}-host.compose.yaml", f"native-{role}.override.yaml"])
            merged[role] = config
            names = ["auth"] if role == "auth" else ["backend", "gateway", "gateway-webgl", "gateway-combat", "gateway-elemental", "gateway-skills"]
            for name in names:
                service = config["services"][name]
                assert service["image"] == "ghcr.io/dravenlove/mmo-server/backend:cpp-core-config-test", name
                assert service["entrypoint"][0].startswith("/usr/local/bin/native-"), name
                assert not service.get("build"), "old Go build retained: " + name
            if role == "game":
                assert config["services"]["scene1"]["image"] == "ghcr.io/dravenlove/mmo-server/scene:legacy-test"
                assert config["volumes"]["backend-data"]["name"] == "synthetic-preserved-data"
                assert config["services"]["gateway"]["environment"]["TLS_CERT_FILE"] == "/certs/existing.pem"
                for name, port in (("gateway-webgl", 7001), ("gateway-combat", 7004), ("gateway-elemental", 7006), ("gateway-skills", 7008)):
                    assert any(int(p["target"]) == 7001 and int(p["published"]) == port and p["host_ip"] == "127.0.0.1"
                               for p in config["services"][name]["ports"]), name
            print("PASS", role, "native image/entrypoints, no legacy build, preserved scenes/volume/TLS and loopback WS ports", flush=True)
        room = read_compose(["game-host.compose.yaml", "native-game.override.yaml", "native-room-game.override.yaml"])
        previous = merged["game"]
        assert set(room["services"]) == set(previous["services"]), "room layer unexpectedly added or removed a service"
        image = "ghcr.io/dravenlove/mmo-server/backend:cpp-room-config-test"
        core = ("backend", "gateway", "gateway-webgl", "gateway-combat", "gateway-elemental", "gateway-skills")
        for name in core:
            expected = copy.deepcopy(previous["services"][name])
            expected["image"] = image
            if name == "backend":
                expected["environment"]["ROOM_WORKER_URL"] = "http://scene-skills:9006"
            assert room["services"][name] == expected, "room core changed settings beyond its image/Worker URL: " + name
            assert not room["services"][name].get("build"), "room layer retained a source build: " + name
        for name in ("scene1", "scene2", "scene-webgl", "scene-combat", "scene-elemental"):
            assert room["services"][name] == previous["services"][name], "room layer changed an existing Scene: " + name
        worker = room["services"]["scene-skills"]
        expected = copy.deepcopy(previous["services"]["scene-skills"])
        expected.pop("build", None)
        expected["image"] = image
        expected["entrypoint"] = ["/usr/local/bin/native-worker"]
        expected["command"] = []
        expected["environment"].update(ROOM_SCRIPT_MANIFEST="/app/gameplay/manifest.json", WORKER_MAX_ROOMS="12",
                                       WORKER_MAX_ROOM_PLAYERS="24", WORKER_MAX_PLAYERS="256")
        scripts = [mount for mount in worker.get("volumes", []) if mount["target"] == "/app/gameplay"]
        assert len(scripts) == 1 and scripts[0]["type"] == "bind" and scripts[0]["read_only"] is True
        assert Path(scripts[0]["source"]).resolve() == (root / "scripts/gameplay").resolve(), "ROOM_GAMEPLAY_DIR was not honored"
        retained = [mount for mount in worker.get("volumes", []) if mount["target"] != "/app/gameplay"]
        assert retained == expected.get("volumes", []), "room worker changed the existing combat-config mount"
        expected["volumes"] = worker["volumes"]
        assert worker == expected, "room worker changed old profile/ports/runtime settings"
        for key in ("volumes", "networks"):
            assert room.get(key) == previous.get(key), "room layer changed persistent volumes or networks"
        auth = read_compose(["auth-host.compose.yaml", "native-auth.override.yaml"],
                            dict(environment, MMO_ROOM_IMAGE_TAG="unrelated-room-tag"))
        assert auth == merged["auth"], "room image tag changed separate Auth configuration"
        assert room["services"]["backend"]["environment"]["AUTH_URL"] == "http://auth.example:8082"
        print("PASS room third layer: matched core image, Worker URL/read-only Lua scripts, preserved five Scenes, Auth, data, TLS and ports", flush=True)
        config = read_compose(["native-local.compose.yaml"])
        for service in config["services"].values():
            mounts = service["tmpfs"]
            assert len(mounts) == 1 and mounts[0].startswith("/tmp:") and "mode=1777" in mounts[0], "tmpfs options split into invalid mount paths"
        print("PASS native local tmpfs is one absolute mount including its options", flush=True)


if __name__ == "__main__":
    main()
