"""原Go Backend SQLite库关闭后，由C++接管；凭据仅存在临时文件中。"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--go", default="go")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    binary = str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory(prefix="mmo-native-legacy-wrapper-") as parent:
        fixture = Path(parent) / "mmo-native-legacy-fixture"
        subprocess.run(
            [args.go, "run", "./native/game_service/tests/go_compat/legacy_fixture.go", "-output-dir", str(fixture)],
            cwd=root,
            check=True,
            timeout=90,
        )
        subprocess.run([binary, "--legacy-fixture", str(fixture / "fixture.json")], check=True, timeout=30)
    print("Go to C++ legacy Backend handover passed")


if __name__ == "__main__":
    main()
