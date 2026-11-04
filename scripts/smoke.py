#!/usr/bin/env python3
"""启动独立原生进程，验证跨进程协议与持久化；仅使用 Python 标准库。"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parent.parent


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def http(url, body=None, key=None, timeout=25):
    headers = {"Content-Type": "application/json"}
    if key:
        headers["X-Internal-Token"] = key
    req = urllib.request.Request(url, data=None if body is None else json.dumps(body).encode(), headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as exc:
        return exc.code, json.loads(exc.read())


class Peer:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=25)
        self.reader = self.sock.makefile("rb")
        self.counter = 0

    def request(self, op, ok=True, **fields):
        self.counter += 1
        ident = str(self.counter)
        self.sock.sendall((json.dumps(dict(id=ident, op=op, **fields)) + "\n").encode())
        while True:
            line = self.reader.readline()
            if not line:
                raise AssertionError(f"connection closed while awaiting {op}")
            reply = json.loads(line)
            if reply.get("id") == ident:
                assert reply["ok"] is ok, (op, reply)
                return reply.get("data") if ok else reply

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.reader.close()
        self.sock.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", default=str(ROOT / "bin"))
    args = parser.parse_args()
    suffix = ".exe" if os.name == "nt" else ""
    binaries = {name: Path(args.bin_dir) / (name + suffix) for name in ("backend", "gateway", "scene-server", "client")}
    for path in binaries.values():
        if not path.is_file():
            raise SystemExit(f"先运行构建脚本，缺少 {path}")
    work = ROOT / "output" / ("smoke-" + time.strftime("%Y%m%d-%H%M%S"))
    work.mkdir(parents=True, exist_ok=False)
    ports = {name: free_port() for name in ("backend", "gateway", "health", "scene-1", "scene-2")}
    assert len(set(ports.values())) == len(ports), "端口分配冲突，请重试"
    urls = {name: f"http://127.0.0.1:{port}" for name, port in ports.items()}
    key = secrets.token_hex(32)
    common = dict(os.environ, INTERNAL_TOKEN=key)
    configs = {
        "scene-1": dict(common, SCENE_ID="scene-1", SCENE_HOST="127.0.0.1", SCENE_PORT=str(ports["scene-1"])),
        "scene-2": dict(common, SCENE_ID="scene-2", SCENE_HOST="127.0.0.1", SCENE_PORT=str(ports["scene-2"])),
        "backend": dict(common, BACKEND_LISTEN=f"127.0.0.1:{ports['backend']}", BACKEND_DB=str(work / "mmo.db"), SCENES=",".join(f"{name}={urls[name]}" for name in ("scene-1", "scene-2"))),
        "gateway": dict(common, GATEWAY_LISTEN=f"127.0.0.1:{ports['gateway']}", GATEWAY_HEALTH_LISTEN=f"127.0.0.1:{ports['health']}", BACKEND_URL=urls["backend"]),
    }
    processes, logs, peers, checks = {}, [], [], []

    def passed(name):
        checks.append(name)
        print("PASS", name, flush=True)

    def stop(name):
        process = processes.pop(name, None)
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)

    def start(name):
        log = (work / f"{name}-{time.time_ns()}.log").open("wb")
        logs.append(log)
        binary = binaries["scene-server" if name.startswith("scene-") else name]
        processes[name] = subprocess.Popen([str(binary)], cwd=ROOT, env=configs[name], stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        health = urls["health" if name == "gateway" else name] + "/healthz"
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if processes[name].poll() is not None:
                raise AssertionError(f"{name} 启动失败，查看 {work}")
            try:
                if http(health, timeout=1)[0] == 200:
                    return
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(0.1)
        raise AssertionError(f"{name} 健康检查超时")

    def peer():
        p = Peer(ports["gateway"])
        peers.append(p)
        return p

    def reference(role):
        return {k: role[k] for k in ("role_id", "owner_epoch", "session_epoch")}

    try:
        for name in ("scene-1", "scene-2", "backend", "gateway"):
            start(name)
        passed("four independent processes become healthy")
        assert http(urls["backend"] + "/v1/register", {"name": "intruder"})[0] in (401, 403)
        assert http(urls["scene-1"] + "/v1/enter", {"role": {}})[0] in (401, 403)
        passed("internal services reject missing credentials")
        alice, bob = peer(), peer()
        alice.request("move", ok=False, seq=1, dx=1, dy=0)
        auth = alice.request("register", name="Alice")
        bob_auth = bob.request("register", name="Bob")
        alice.request("register", ok=False, name="Alice")
        alice_role = alice.request("login", token=auth["token"])
        bob_role = bob.request("login", token=bob_auth["token"])
        passed("registration, unique names, token login and auth gating")
        view = alice.request("view")
        assert {p["name"] for p in view["players"]} == {"Alice", "Bob"}, view
        passed("AOI includes nearby players across two TCP clients")
        alice.request("move", seq=1, dx=1, dy=0)
        time.sleep(0.4)
        moved = alice.request("view")["self"]
        assert moved["x"] > alice_role["x"] + 0.3, moved
        time.sleep(0.35)
        stopped = alice.request("view")["self"]
        assert abs(stopped["x"] - moved["x"]) < 0.35, (moved, stopped)
        alice.request("move", ok=False, seq=1, dx=1, dy=0)
        alice.request("move", ok=False, seq=2, dx=9, dy=0)
        passed("authoritative movement, stale sequence rejection, bounds and automatic stop")
        assert alice.request("claim") == {"coins": 100, "granted": True}
        assert alice.request("claim") == {"coins": 100, "granted": False}
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda _: http(urls["backend"] + "/v1/claim", {"role_id": bob_role["role_id"], "session_epoch": bob_role["session_epoch"]}, key), range(8)))
        assert all(code == 200 and data["coins"] == 100 for code, data in results), results
        assert sum(data["granted"] for _, data in results) == 1, results
        passed("sequential and concurrent rewards are credited exactly once")
        old = alice_role.copy()
        alice_role = alice.request("transfer", target_scene_id="scene-2", request_id="smoke-transfer-1")
        assert alice_role["scene_id"] == "scene-2" and alice_role["owner_epoch"] > old["owner_epoch"]
        again = alice.request("transfer", target_scene_id="scene-2", request_id="smoke-transfer-1")
        assert again["owner_epoch"] == alice_role["owner_epoch"], again
        assert alice.request("view")["scene_id"] == "scene-2"
        assert {p["name"] for p in bob.request("view")["players"]} == {"Bob"}
        code, _ = http(urls["scene-1"] + "/v1/move", dict(reference(old), seq=10, dx=1, dy=0), key)
        assert code in (404, 409), code
        alice.request("transfer", ok=False, target_scene_id="unknown", request_id="smoke-invalid")
        passed("cross-scene ownership transfer, retry idempotency and stale owner fencing")
        alice_role = alice.request("transfer", target_scene_id="scene-1", request_id="smoke-transfer-2")
        alice.request("transfer", ok=False, target_scene_id="scene-2", request_id="smoke-transfer-2")
        passed("return transfer and rejection of changed idempotent payload")
        replacement = peer()
        old = alice_role.copy()
        alice_role = replacement.request("login", token=auth["token"])
        assert alice_role["session_epoch"] > old["session_epoch"]
        code, _ = http(urls["scene-1"] + "/v1/move", dict(reference(old), seq=20, dx=1, dy=0), key)
        assert code == 409, code
        passed("new login invalidates old scene session")
        alice.close(); peers.remove(alice)
        alice = replacement
        stop("scene-2")
        alice.request("transfer", ok=False, target_scene_id="scene-2", request_id="smoke-unavailable")
        start("scene-2")
        # 失败迁移恢复后，新的迁移必须使用未使用过的代次。
        time.sleep(0.4)
        alice_role = alice.request("transfer", target_scene_id="scene-2", request_id="smoke-recovered")
        assert alice_role["scene_id"] == "scene-2"
        passed("unavailable target fails safely and later transfer recovers")
        time.sleep(2.4)
        saved = alice.request("view")["self"]
        for p in list(peers):
            p.close()
        peers.clear()
        time.sleep(0.4)
        stop("gateway"); start("gateway")
        alice = peer(); current = alice.request("login", token=auth["token"])
        assert current["coins"] == 100 and current["scene_id"] == saved["scene_id"], current
        passed("gateway restart preserves identity and scene routing")
        alice.close();peers.remove(alice);time.sleep(0.3)
        stop("backend");stop("scene-2")
        start("scene-2");start("backend")
        alice = peer();current = alice.request("login", token=auth["token"])
        view = alice.request("view")
        assert current["coins"] == 100 and current["scene_id"] == "scene-2", current
        assert abs(view["self"]["x"] - saved["x"]) < 0.01, (view, saved)
        assert alice.request("claim")["granted"] is False
        passed("backend and scene restart restore SQLite state without duplicate reward")
        extra_env = dict(configs["backend"], BACKEND_LISTEN=f"127.0.0.1:{free_port()}")
        extra = subprocess.run([str(binaries["backend"])], env=extra_env, cwd=ROOT, capture_output=True, timeout=8, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        assert extra.returncode != 0, "two backends opened the same SQLite database"
        passed("SQLite database rejects a second backend writer process")
        commands = work / "client-regression.jsonl"
        commands.write_text('\n'.join(json.dumps(cmd) for cmd in [
            {"op": "move", "seq": 100, "dx": 1, "dy": 0},
            {"op": "move", "dx": 0, "dy": 1},
            {"op": "transfer", "target_scene_id": "scene-2"},
            {"op": "claim"}, {"op": "view"}]) + '\n', encoding="utf-8")
        cli = subprocess.run([str(binaries["client"]), "-addr", f"127.0.0.1:{ports['gateway']}", "-name", "Charlie", "-token-file", str(work / "charlie-token.json"), "-script", str(commands)], capture_output=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        assert cli.returncode == 0, cli.stderr.decode("utf-8", errors="replace")
        assert b"request_id=client-" in cli.stdout
        (work / "client.log").write_bytes(cli.stdout + cli.stderr)
        passed("real CLI preserves explicit input sequence and exposes transfer retry id")
        failed_cli = subprocess.run([str(binaries["client"]), "-addr", f"127.0.0.1:{ports['gateway']}", "-name", "Unregistered", "-token-file", str(work / "missing-dir" / "token.json"), "-script", str(commands)], capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        assert failed_cli.returncode != 0
        assert alice.request("register", name="Unregistered")["token"]
        passed("unwritable credential path does not consume an account name")
        report = {"ok": True, "checks": checks, "platform": sys.platform, "count": len(checks)}
        (work / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"PASS {len(checks)} checks; report: {work / 'report.json'}", flush=True)
    finally:
        for p in peers:
            try:
                p.close()
            except OSError:
                pass
        for name in list(processes):
            stop(name)
        for log in logs:
            log.close()


if __name__ == "__main__":
    main()
