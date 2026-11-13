#!/usr/bin/env python3
"""在临时源码副本中复用旧验收脚本，所有游戏服务使用 C++ 二进制。"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--scene", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="mmo-native-legacy-") as directory:
        work = Path(directory)
        for folder in ("cmd", "internal", "scripts", "configs"):
            shutil.copytree(root / folder, work / folder, ignore=shutil.ignore_patterns("__pycache__"))
        for filename in ("go.mod", "go.sum"):
            shutil.copy2(root / filename, work / filename)
        (work / "bin").mkdir()
        (work / "output").mkdir()
        for source, filename in ((args.build / "game_service/native-game-service", "backend"),
                                 (args.build / "gateway/native-gateway", "gateway"), (args.scene, "scene-server")):
            shutil.copy2(source, work / "bin" / filename)
        environment = {k: v for k, v in os.environ.items() if not k.startswith(
            ("MYSQL_", "AUTH_", "BACKEND_", "GATEWAY_", "WEBSOCKET_", "WS_", "SCENE_", "INTERNAL_TOKEN", "TLS_"))}
        subprocess.run(["go", "build", "-o", "bin/client", "./cmd/client"], cwd=work, env=environment, check=True, timeout=120)
        for script in ("smoke.py", "smoke_abilities.py", "smoke_elemental.py", "smoke_skill_lab.py"):
            result = subprocess.run([sys.executable, str(work / "scripts" / script)], cwd=work,
                                    env=environment, capture_output=True, text=True, timeout=180)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                # 只打印合成夹具日志；绝不读取真实部署目录或 env。
                for log in sorted((work / "output").rglob("*.log")):
                    print("fixture", str(log.relative_to(work)), flush=True)
                    print(log.read_text(errors="replace")[-8000:], flush=True)
                raise AssertionError(script + " failed: " + result.stderr[-3000:])
        print("PASS legacy protocol suites with C++ Gateway/Backend/Scene and Go diagnostic CLI", flush=True)


if __name__ == "__main__":
    main()
