"""启动临时 SceneHost，验证实际 HTTP 入口；结束时只清理自己启动的进程。"""

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


def main():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    token = secrets.token_hex(32)
    env = dict(os.environ, INTERNAL_TOKEN=token, SCENE_ID="http-test",
               SCENE_HOST="127.0.0.1", SCENE_PORT=str(port))
    process = subprocess.Popen([str(Path(sys.argv[1]).resolve())], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    base = f"http://127.0.0.1:{port}"

    def request(path, data=None, authenticated=True, raw=None):
        body = raw if raw is not None else (json.dumps(data).encode() if data is not None else None)
        headers = {"Content-Type": "application/json"}
        if authenticated:
            headers["X-Internal-Token"] = token
        req = urllib.request.Request(base + path, data=body, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=4) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    try:
        until = time.monotonic() + 10
        while True:
            if process.poll() is not None:
                raise RuntimeError("server exited during startup")
            try:
                status, health = request("/healthz", authenticated=False)
                assert status == 200 and health["scene_id"] == "http-test"
                break
            except (OSError, urllib.error.URLError):
                if time.monotonic() > until:
                    raise
                time.sleep(0.05)
        # 同端口启动第二个进程必须失败，避免多个独立场景内存被随机路由。
        duplicate = subprocess.Popen([str(Path(sys.argv[1]).resolve())], env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            duplicate_output, _ = duplicate.communicate(timeout=5)
            assert duplicate.returncode != 0, "duplicate scene listener unexpectedly succeeded"
            assert b"could not bind scene address" in duplicate_output, duplicate_output
        finally:
            if duplicate.poll() is None:
                duplicate.kill()
                duplicate.communicate(timeout=3)
        assert request("/v1/enter", {}, authenticated=False)[0] == 401
        assert request("/v1/enter", raw=b"{bad")[0] == 400
        assert request("/v1/enter", raw=b"x" * (65536 + 1))[0] == 413
        assert request("/not-found")[0] == 404
        role = dict(role_id="http-player", name="Player", scene_id="http-test",
                    owner_epoch=1, session_epoch=1, x=0, y=0, coins=0)
        status, result = request("/v1/enter", {"role": role})
        assert status == 200 and result["role"]["role_id"] == role["role_id"]
        ref = {key: role[key] for key in ("role_id", "owner_epoch", "session_epoch")}
        assert request("/v1/move", dict(ref, seq=1, dx=1, dy=0))[0] == 200
        time.sleep(0.12)
        status, view = request("/v1/view", ref)
        assert status == 200 and view["self"]["x"] > 0
        assert request("/v1/move", dict(ref, seq=1, dx=1, dy=0))[0] == 409
        assert request("/v1/freeze", dict(ref, transfer_id="http-transfer"))[0] == 200
        assert request("/v1/view", ref)[0] == 409
        assert request("/v1/unfreeze", dict(ref, transfer_id="http-transfer"))[0] == 200
        assert request("/v1/remove", ref)[0] == 200
        assert request("/v1/enter", {"role": role})[0] == 409
        print("scene HTTP lifecycle passed")
    finally:
        process.terminate()
        try:
            output, _ = process.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            output, _ = process.communicate(timeout=3)
        if sys.exc_info()[0] is not None:
            print(output.decode(errors="replace"), file=sys.stderr)


if __name__ == "__main__":
    main()
