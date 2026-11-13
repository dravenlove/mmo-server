#!/usr/bin/env python3
"""在本机新建临时 MySQL 8 实例，验证 C++ 与 Go 账号/锁兼容，绝不读取部署配置。"""
import argparse
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    for command in ("mysqld", "mysql", "go"):
        if not shutil.which(command):
            parser.error("缺少隔离验证依赖：" + command)
    build = args.build.resolve()
    account = build / "account/native-account"
    database_test = build / "persistence/native-db-tests"
    assert account.is_file() and database_test.is_file()
    clean = {k: v for k, v in os.environ.items() if not k.startswith(("MYSQL_", "AUTH_", "BACKEND_", "MMO_TEST_MYSQL_"))}
    with tempfile.TemporaryDirectory(prefix="mmo-native-mysql-") as directory:
        work = Path(directory)
        data = work / "data"
        data.mkdir()
        mysql_socket = work / "mysql.sock"
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0)); port = listener.getsockname()[1]
        with (work / "mysqld.log").open("wb") as log:
            subprocess.run(["mysqld", "--no-defaults", "--initialize-insecure", "--user=" + __import__("getpass").getuser(),
                            "--datadir=" + str(data)], env=clean, stdout=log, stderr=log, check=True, timeout=60)
            process = subprocess.Popen(["mysqld", "--no-defaults", "--user=" + __import__("getpass").getuser(),
                                        "--datadir=" + str(data), "--socket=" + str(mysql_socket),
                                        "--pid-file=" + str(work / "mysqld.pid"), "--port=" + str(port),
                                        "--bind-address=127.0.0.1", "--mysqlx=OFF", "--skip-log-bin", "--secure-file-priv=NULL",
                                        "--innodb-buffer-pool-size=64M", "--max-connections=30"],
                                       env=clean, stdout=log, stderr=log)
            try:
                for _ in range(200):
                    if process.poll() is not None:
                        raise AssertionError("isolated MySQL failed: " + (work / "mysqld.log").read_text(errors="replace")[-2000:])
                    if mysql_socket.exists():
                        probe = subprocess.run(["mysql", "--no-defaults", "-uroot", "--socket=" + str(mysql_socket),
                                                "-Nse", "SELECT 1"], env=clean, capture_output=True)
                        if probe.returncode == 0:
                            break
                    time.sleep(.1)
                else:
                    raise AssertionError("isolated MySQL startup timed out")
                password = secrets.token_hex(24)
                credentials = work / "admin-password"
                credentials.write_text(password)
                credentials.chmod(0o600)
                subprocess.run(["mysql", "--no-defaults", "-uroot", "--socket=" + str(mysql_socket)],
                               input=f"ALTER USER 'root'@'localhost' IDENTIFIED BY '{password}';\n".encode(),
                               env=clean, capture_output=True, check=True)
                subprocess.run([sys.executable, str(root / "native/account/tests/mysql_tests.py"),
                                "--binary", str(account), "--db-test-binary", str(database_test),
                                "--game-test-binary", str(build / "game_service/native-game-tests"),
                                "--port", str(port), "--admin-password-file", str(credentials)],
                               cwd=root, env=clean, check=True, timeout=180)
                print("PASS isolated MySQL 8, C++/Go password compatibility, restricted privileges, writer locks and lock loss", flush=True)
            finally:
                process.terminate()
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=10)


if __name__ == "__main__":
    main()
