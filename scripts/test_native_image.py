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
        peer = None
        try:
            subprocess.run(command + ["up", "-d", "--no-build", "--pull", "never", "--wait", "--wait-timeout", "90"], check=True, timeout=120)
            peer = WebSocket(ws_port)
            account = peer.request("account_register", name="container-" + secrets.token_hex(4), password="synthetic-password-123!")
            role = peer.request("login", token=account["token"])
            assert role["role_id"] == account["role_id"]
            peer.request("move", seq=1, dx=1, dy=0)
            view = peer.request("view")
            definition = peer.request("skill_init", world_epoch=view["world_epoch"], skill_id="container-bolt",
                                      expected_definition_version=0, definition={"type": "projectile", "damage": 8,
                                      "mp_cost": 0, "cooldown_ms": 100, "range": 20, "speed": 12, "radius": .2})
            assert definition["accepted"]
            cast = peer.request("cast", seq=1, skill_id="container-bolt", world_epoch=view["world_epoch"],
                                definition_version=definition["definition"]["definition_version"], dx=1, dy=0)
            assert cast["accepted"]
            print("PASS non-root image: C++ WebSocket -> GameService -> Account/SQLite -> Scene, move and parameterized cast", flush=True)
        except Exception:
            subprocess.run(command + ["logs", "--tail", "40"], timeout=20)
            raise
        finally:
            if peer:
                peer.close()
            # 仅删除此函数刚创建的随机测试project及其卷，不使用真实部署project。
            subprocess.run(command + ["down", "--volumes", "--remove-orphans"], timeout=45, check=True)


if __name__ == "__main__":
    main()
