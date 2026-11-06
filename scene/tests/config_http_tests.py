"""真实 HTTP 验证固定文件热更、鉴权、失败原子性和启动失败；仅清理自建进程。"""

from contextlib import contextmanager
from copy import deepcopy
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


@contextmanager
def running(binary, config_file=None):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    token = secrets.token_hex(32)
    env = dict(os.environ, INTERNAL_TOKEN=token, SCENE_ID="config-http",
               SCENE_HOST="127.0.0.1", SCENE_PORT=str(port))
    env.pop("COMBAT_CONFIG_FILE", None)
    if config_file is not None:
        env["COMBAT_CONFIG_FILE"] = str(config_file)
    process = subprocess.Popen([binary], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def request(path, data=None, authenticated=True, raw=None):
        body = raw if raw is not None else (json.dumps(data).encode() if data is not None else None)
        headers = {"Content-Type": "application/json"}
        if authenticated:
            headers["X-Internal-Token"] = token
        req = urllib.request.Request(f"http://127.0.0.1:{port}" + path, data=body, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=4) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    try:
        until = time.monotonic() + 10
        while True:
            if process.poll() is not None:
                raise RuntimeError("config scene exited during startup")
            try:
                assert request("/healthz", authenticated=False)[0] == 200
                break
            except (OSError, urllib.error.URLError):
                if time.monotonic() > until:
                    raise
                time.sleep(.03)
        yield request, process, env
    finally:
        process.terminate()
        try:
            output, _ = process.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            output, _ = process.communicate(timeout=3)
        if sys.exc_info()[0] is not None:
            print(output.decode(errors="replace"), file=sys.stderr)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with running(binary) as (request, _, _):
        code, initial = request("/v1/config/status", {})
        assert code == 200 and initial["generation"] == 1 and initial["version"] == "default-v1"
        defaults = initial["config"]
        assert request("/v1/config/reload", {})[0] == 409
        for operation in ("status", "reload", "apply", "validate"):
            assert request("/v1/config/" + operation, {}, authenticated=False)[0] == 401
        assert "config" not in request("/healthz", authenticated=False)[1]
        assert request("/v1/config/status", raw=b'{"extra":1,"extra":2}')[0] == 400

    with tempfile.TemporaryDirectory(prefix="mmo-config-http-") as directory:
        path = Path(directory) / "current.json"
        startup = deepcopy(defaults)
        startup["version"] = "file-start"
        path.write_text(json.dumps(startup), encoding="utf-8")
        with running(binary, path) as (request, process, env):
            assert request("/v1/config/status", {})[1]["config"] == startup
            role = dict(role_id="hero", name="Hero", scene_id="config-http",
                        owner_epoch=1, session_epoch=1, x=0, y=0, coins=0)
            ref = {key: role[key] for key in ("role_id", "owner_epoch", "session_epoch")}
            assert request("/v1/enter", {"role": role})[0] == 200
            charge = dict(ref, seq=1, skill_id="charge", target_id="npc-1")
            code, cached = request("/v1/cast", charge)
            assert code == 200 and cached["hits"][0]["damage"] == 24
            assert cached["config_version"] == "file-start"
            candidate = deepcopy(startup)
            candidate["version"] = "file-demo"
            candidate["skills"]["strike"].update(damage=40, range=20, cooldown_ms=50)
            candidate["skills"]["charge"]["cooldown_ms"] = 100
            candidate["monsters"]["wolf"]["max_hp"] = 200
            status_before = request("/v1/config/status", {})[1]
            code, validation = request("/v1/config/validate", dict(config=candidate, expected_generation=1))
            assert code == 200 and validation["would_change"] and validation["current_version"] == "file-start"
            assert request("/v1/config/status", {})[1] == status_before
            assert request("/v1/config/reload", dict(path="other.json"))[0] == 400
            # 通过替换文件模拟真实发布，保证服务重新打开目录中的新 inode。
            replacement = path.with_suffix(".next")
            replacement.write_text(json.dumps(candidate), encoding="utf-8")
            os.replace(replacement, path)
            code, applied = request("/v1/config/reload", dict(expected_generation=1))
            assert code == 200 and applied["version"] == "file-demo" and applied["generation"] == 2 and applied["changed"]
            assert request("/v1/cast", charge) == (200, cached), "hot reload lost successful action cache"
            code, view = request("/v1/view", ref)
            assert code == 200 and view["config_version"] == "file-demo" and view["config_generation"] == 2
            assert view["self"]["combat"]["last_action_seq"] == 1
            assert view["self"]["combat"]["cooldowns"]["charge"] > 100
            assert view["mobs"][0]["max_hp"] == 80 and view["mobs"][0]["config_version"] == "file-start"
            strike = dict(ref, seq=2, skill_id="strike", target_id="npc-1")
            code, hit = request("/v1/cast", strike)
            assert code == 200 and hit["hits"][0]["damage"] == 40 and hit["config_version"] == "file-demo"
            assert request("/v1/config/reload", dict(expected_generation=1))[0] == 409
            code, identical = request("/v1/config/reload", dict(expected_generation=2))
            assert code == 200 and identical["generation"] == 2 and not identical["changed"]

            stable = request("/v1/config/status", {})[1]
            for bad in (b'{bad', b'{}', b'[]', b'{"version":"x","version":"y"}', b'x' * 65537):
                path.write_bytes(bad)
                assert request("/v1/config/reload", {})[0] == 400
                assert request("/v1/config/status", {})[1] == stable
                assert request("/v1/cast", strike) == (200, hit)
            path.unlink()
            assert request("/v1/config/reload", {})[0] == 400
            assert request("/v1/config/status", {})[1] == stable
            # apply 仅用于内存调试，不写入文件；正式发布使用固定文件+reload。
            memory = deepcopy(candidate); memory["version"] = "memory-only"
            assert request("/v1/config/apply", dict(config=memory, expected_generation=2))[1]["generation"] == 3
            assert not path.exists()
            conflict = deepcopy(candidate); conflict["skills"]["strike"]["damage"] = 41
            assert request("/v1/config/validate", dict(config=conflict))[0] == 409
            path.write_text(json.dumps(startup), encoding="utf-8")
            code, rolled = request("/v1/config/reload", dict(expected_generation=3))
            assert code == 200 and rolled["version"] == "file-start" and rolled["generation"] == 4
            assert request("/v1/cast", strike) == (200, hit)
            assert process.poll() is None, "reload restarted the scene process"

        # 配置文件损坏、缺失、未知字段、非有限数、重复键或过大时启动必须失败。
        invalid_values = [b'{bad', b'{}', b'[]', b'{"schema_version":1,"schema_version":1}',
                          b'x' * 65537]
        nan = deepcopy(startup); nan["skills"]["strike"]["range"] = float("nan")
        invalid_values.append(json.dumps(nan).encode())
        wrong_schema = deepcopy(startup); wrong_schema["unknown"] = True
        invalid_values.append(json.dumps(wrong_schema).encode())
        for content in invalid_values:
            path.write_bytes(content)
            failed = subprocess.run([binary], env=env, capture_output=True, timeout=8)
            assert failed.returncode != 0, "invalid startup file accepted"
        path.unlink()
        assert subprocess.run([binary], env=env, capture_output=True, timeout=8).returncode != 0
    print("scene fixed-file config HTTP auth, reload, validation, CAS, rollback and startup checks passed")


if __name__ == "__main__":
    main()
