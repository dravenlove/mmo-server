"""C++ Auth 黑盒兼容回归；仅使用临时 SQLite 数据，不读取线上 env。"""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

KEY = "synthetic-cpp-account-test-internal-key-32-plus"
LEGACY = "a" * 64
PASSWORD = "联调-password-123!"
GO_HASH = "$2a$10$5cAfhnaXy5oiH5LG6mKdOuWxwHHEi/ZtbjFYAj2SJyRJ4f/3J600G"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Auth:
    def __init__(self, binary, path):
        self.path = path
        self.port = free_port()
        clean = {k: v for k, v in os.environ.items() if not k.startswith(("MYSQL_", "AUTH_", "INTERNAL_TOKEN"))}
        clean.update(INTERNAL_TOKEN=KEY, AUTH_DB_DRIVER="sqlite", AUTH_DB=str(path), AUTH_LISTEN=f"127.0.0.1:{self.port}")
        self.process = subprocess.Popen([binary], env=clean, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(150):
            if self.process.poll() is not None:
                raise AssertionError("isolated C++ Auth did not start")
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{self.port}/healthz", timeout=.2) as response:
                    assert response.status == 200
                return
            except (OSError, urllib.error.URLError):
                time.sleep(.04)
        self.stop()
        raise AssertionError("isolated Auth startup timed out")

    def stop(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)

    def post(self, operation, body, authorized=True, raw=False):
        payload = body.encode() if raw else json.dumps(body, ensure_ascii=False).encode()
        headers = {"Content-Type": "application/json"}
        if authorized:
            headers["X-Internal-Token"] = KEY
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}/v1/{operation}", data=payload, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            return error.code, json.load(error)


def main(binary):
    with tempfile.TemporaryDirectory(prefix="mmo-native-account-") as temp:
        path = Path(temp) / "accounts.db"
        with sqlite3.connect(path) as connection:
            connection.execute("CREATE TABLE roles(role_id TEXT,name TEXT,token_hash TEXT,coins INTEGER)")
            connection.execute("INSERT INTO roles VALUES(?,?,?,77)", ("1" * 32, "原角色", hashlib.sha256(LEGACY.encode()).hexdigest()))
        service = Auth(binary, path)
        try:
            assert service.post("authenticate", {"token": LEGACY})[1]["role_id"] == "1" * 32
            assert service.post("register", {"name": "denied"}, False)[0] == 401
            for bad in (None, [], {"name": "\n"}, {"name": "x" * 25}, {"name": "x", "extra": "x"}):
                assert service.post("register", bad)[0] == 400
            assert service.post("register", '{"name":"one","name":"two"}', raw=True)[0] == 400
            assert service.post("register", {"name": "x" * 70000})[0] == 400
            header_request = urllib.request.Request(
                f"http://127.0.0.1:{service.port}/healthz",
                headers={"X-One": "x" * 8200, "X-Two": "x" * 8200})
            try:
                urllib.request.urlopen(header_request, timeout=3)
                raise AssertionError("aggregate header limit was not enforced")
            except urllib.error.HTTPError as error:
                assert error.code == 431
            with concurrent.futures.ThreadPoolExecutor(max_workers=20) as executor:
                statuses = list(executor.map(lambda _: service.post("register", {"name": "unique"})[0], range(20)))
            assert statuses.count(200) == 1 and statuses.count(409) == 19
            status, created = service.post("account_register", {"name": "密码角色", "password": PASSWORD})
            assert status == 200 and len(created["token"]) == 64 and created["expires_at_unix_ms"] > time.time() * 1000
            with sqlite3.connect(path) as connection:
                stable = connection.execute("SELECT token_hash FROM auth_accounts WHERE role_id=?", (created["role_id"],)).fetchone()[0]
                assert stable != hashlib.sha256(created["token"].encode()).hexdigest()
                # Go 原哈希在新 C++ 进程上验证，不能强制用户重设密码。
                connection.execute("UPDATE auth_passwords SET password_hash=? WHERE role_id=?", (GO_HASH, created["role_id"]))
            tickets = [created["token"]]
            for _ in range(5):
                code, logged = service.post("account_login", {"name": "密码角色", "password": PASSWORD})
                assert code == 200 and logged["role_id"] == created["role_id"]
                tickets.append(logged["token"])
            assert service.post("authenticate", {"token": tickets[0]})[0] == 401
            assert service.post("authenticate", {"token": tickets[-1]})[0] == 200
            assert service.post("account_login", {"name": "密码角色", "password": PASSWORD})[0] == 429
            with sqlite3.connect(path) as connection:
                assert connection.execute("SELECT COUNT(*) FROM auth_login_tokens WHERE role_id=?", (created["role_id"],)).fetchone()[0] == 5
                connection.execute("UPDATE auth_login_tokens SET expires_ms=0 WHERE token_hash=?", (hashlib.sha256(tickets[1].encode()).hexdigest(),))
                assert connection.execute("SELECT coins FROM roles").fetchone()[0] == 77
            assert service.post("authenticate", {"token": tickets[1]})[0] == 401
            assert service.post("account_bind", {"token": LEGACY, "password": PASSWORD})[0] == 200
            assert service.post("account_bind", {"token": LEGACY, "password": "cannot-overwrite"})[0] == 409
            assert service.post("account_login", {"name": "原角色", "password": PASSWORD})[0] == 200
            assert service.post("authenticate", {"token": LEGACY})[0] == 200
            missing = service.post("account_login", {"name": "不存在", "password": "wrong-password"})
            wrong = service.post("account_login", {"name": "原角色", "password": "wrong-password"})
            assert missing == wrong and missing[0] == 401
            for password in ("short", "x" * 73, "12345678\n", "12345678\x00"):
                assert service.post("account_register", {"name": "invalid", "password": password})[0] == 400
            with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
                statuses = list(executor.map(lambda n: service.post("account_login", {"name": f"cpu-busy-{n}", "password": "wrong-password"})[0], range(12)))
            assert set(statuses) <= {401, 429} and 429 in statuses
            # 固定连接独占锁阻止第二个 C++ 写入者进入相同 SQLite 文件。
            duplicate_env = dict(os.environ, INTERNAL_TOKEN=KEY, AUTH_DB_DRIVER="sqlite", AUTH_DB=str(path), AUTH_LISTEN=f"127.0.0.1:{free_port()}")
            duplicate = subprocess.run([binary], env=duplicate_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
            assert duplicate.returncode != 0
        finally:
            service.stop()
        service = Auth(binary, path)
        try:
            assert service.post("authenticate", {"token": LEGACY})[0] == 200
            assert service.post("authenticate", {"token": tickets[-1]})[0] == 200
            assert service.post("account_login", {"name": "密码角色", "password": PASSWORD})[0] == 200
            assert service.post("status", {})[1]["imported_legacy"] == 0
        finally:
            service.stop()
        # 导入冲突整批回滚，不覆盖旧账号。
        conflict = Path(temp) / "conflict.db"
        with sqlite3.connect(conflict) as connection:
            connection.executescript("CREATE TABLE roles(role_id TEXT,name TEXT,token_hash TEXT); CREATE TABLE auth_accounts(role_id TEXT PRIMARY KEY,name TEXT UNIQUE,token_hash TEXT UNIQUE); INSERT INTO roles VALUES('legacy','same','legacy-hash'); INSERT INTO auth_accounts VALUES('other','same','original-hash');")
        failed = subprocess.run([binary], env=dict(os.environ, INTERNAL_TOKEN=KEY, AUTH_DB_DRIVER="sqlite", AUTH_DB=str(conflict)), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
        assert failed.returncode != 0
        with sqlite3.connect(conflict) as connection:
            assert connection.execute("SELECT token_hash FROM auth_accounts").fetchone()[0] == "original-hash"
        print("C++ Auth HTTP: legacy import, password/hash compatibility, five-ticket reuse, expiry, bind, limits, ownership and restart passed")


if __name__ == "__main__":
    main(sys.argv[1])
