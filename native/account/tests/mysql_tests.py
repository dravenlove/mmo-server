"""Run C++/Go Auth compatibility only against an explicitly isolated local MySQL.

Creates and drops its own mmo_native_test_<random> database and limited user.
Never reads MYSQL_*/AUTH_* deployment configuration.
"""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--db-test-binary", type=Path)
    parser.add_argument("--game-test-binary", type=Path)
    parser.add_argument("--host", default=os.environ.get("MMO_TEST_MYSQL_HOST", "127.0.0.1"), choices=("127.0.0.1", "localhost"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("MMO_TEST_MYSQL_PORT", "0")))
    parser.add_argument("--admin-user", default=os.environ.get("MMO_TEST_MYSQL_USER", "root"))
    parser.add_argument("--admin-password-file", type=Path)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("explicit isolated MySQL port is required")
    binary = args.binary.resolve(strict=True)
    password = args.admin_password_file.read_text().rstrip("\r\n") if args.admin_password_file else os.environ.get("MMO_TEST_MYSQL_PASSWORD", "")
    environment = {key: value for key, value in os.environ.items() if not key.startswith(("MYSQL_", "AUTH_"))}
    environment.update(NATIVE_ACCOUNT_BINARY=str(binary), MMO_TEST_MYSQL_HOST=args.host,
                       MMO_TEST_MYSQL_PORT=str(args.port), MMO_TEST_MYSQL_USER=args.admin_user,
                       MMO_TEST_MYSQL_PASSWORD=password)
    if args.db_test_binary:
        environment["NATIVE_DB_TEST_BINARY"] = str(args.db_test_binary.resolve(strict=True))
    if args.game_test_binary:
        environment["NATIVE_GAME_TEST_BINARY"] = str(args.game_test_binary.resolve(strict=True))
    root = Path(__file__).resolve().parents[3]
    result = subprocess.run(["go", "test", "./native/account/tests/go_compat", "-run", "^TestMySQLNativeAccountCompatibility$",
                             "-count=1", "-timeout", "90s", "-v"], cwd=root, env=environment)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
