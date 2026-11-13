#!/usr/bin/env python3
"""离线验证实际 Compose 合并结果，防止旧 Go build 被保留或原数据卷/TLS 被改写。"""
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parent.parent
    environment = dict(os.environ, INTERNAL_TOKEN="synthetic-compose-validation-key-0000",
                       MMO_NATIVE_IMAGE_TAG="cpp-core-config-test", MMO_IMAGE_TAG="legacy-test",
                       MMO_IMAGE_PREFIX="ghcr.io/dravenlove/mmo-server", AUTH_URL="http://auth.example:8082",
                       NATIVE_ALLOWED_ORIGINS="https://native-test.example", MYSQL_HOST="db.example",
                       MYSQL_USER="synthetic", MYSQL_PASSWORD="synthetic", MYSQL_DATABASE="synthetic",
                       BACKEND_VOLUME_NAME="synthetic-preserved-data",
                       TLS_CERT_FILE="/certs/existing.pem", TLS_KEY_FILE="/certs/existing.key")
    environment.pop("MMO_NATIVE_IMAGE_NAME", None)
    with tempfile.TemporaryDirectory(prefix="mmo-compose-check-") as directory:
        empty_env = Path(directory) / "empty.env"
        empty_env.write_text("")
        for role in ("game", "auth"):
            command = ["docker", "compose", "--env-file", str(empty_env),
                       "-f", str(root / f"deploy/{role}-host.compose.yaml"),
                       "-f", str(root / f"deploy/native-{role}.override.yaml"), "config", "--format", "json"]
            raw = subprocess.run(command, env=environment, capture_output=True, text=True, check=True, timeout=30)
            config = json.loads(raw.stdout)
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
        local = ["docker", "compose", "--env-file", str(empty_env), "-f", str(root / "deploy/native-local.compose.yaml"), "config", "--format", "json"]
        config = json.loads(subprocess.run(local, env=environment, capture_output=True, text=True, check=True, timeout=30).stdout)
        for service in config["services"].values():
            mounts = service["tmpfs"]
            assert len(mounts) == 1 and mounts[0].startswith("/tmp:") and "mode=1777" in mounts[0], "tmpfs options split into invalid mount paths"
        print("PASS native local tmpfs is one absolute mount including its options", flush=True)


if __name__ == "__main__":
    main()
