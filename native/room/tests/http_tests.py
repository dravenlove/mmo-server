"""隔离Worker HTTP兼容、生命周期、归属与配置入口；不读取真实部署env。"""
import concurrent.futures
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


KEY = "synthetic-room-worker-key-32-bytes-or-more"


def port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def main():
    binary, manifest = map(lambda value: str(Path(value).resolve()), sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix="mmo-worker-http-") as directory:
        work = Path(directory)
        listen = port()
        environment = {k: v for k, v in os.environ.items() if not k.startswith(("SCENE_", "WORKER_", "ROOM_", "INTERNAL_TOKEN", "COMBAT_", "PVP_", "TICK_HZ", "ELEMENTAL_"))}
        environment.update(INTERNAL_TOKEN=KEY, SCENE_ID="legacy", SCENE_MODE="skill_lab", SCENE_HOST="127.0.0.1",
                           SCENE_PORT=str(listen), ROOM_SCRIPT_MANIFEST=manifest, WORKER_FINISHED_RETENTION_SECONDS="0.1")
        log = (work / "worker.log").open("wb")
        process = subprocess.Popen([binary], env=environment, stdout=log, stderr=log)

        def call(path, body=None, authenticated=True, raw=None):
            headers = {"Content-Type": "application/json"}
            if authenticated:
                headers["X-Internal-Token"] = KEY
            request = urllib.request.Request(f"http://127.0.0.1:{listen}" + path,
                                             data=raw if raw is not None else None if body is None else json.dumps(body).encode(), headers=headers)
            try:
                with urllib.request.urlopen(request, timeout=5) as response:
                    return response.status, json.load(response)
            except urllib.error.HTTPError as error:
                return error.code, json.load(error)

        def ref(name="host", session=1):
            return dict(role_id=name, owner_epoch=1, session_epoch=session)

        def role(name, room):
            return dict(ref(name), name=name, scene_id=room, x=0, y=0, coins=25)

        try:
            for _ in range(150):
                if process.poll() is not None:
                    raise AssertionError("worker failed to start")
                try:
                    code, health = call("/healthz", authenticated=False)
                    if code == 200:
                        assert health["scene_id"] == "legacy" and health["mode"] == "skill_lab"
                        break
                except (OSError, urllib.error.URLError):
                    pass
                time.sleep(.05)
            else:
                raise AssertionError("worker health timed out")
            duplicate = subprocess.run([binary], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=8)
            assert duplicate.returncode != 0, "two workers shared one listener and split authoritative worlds"
            assert call("/v1/rooms/create", {}, authenticated=False)[0] == 401
            assert call("/v1/rooms/list", authenticated=False)[0] == 401
            assert call("/v1/rooms/create", raw=b'{"room_id":"a","room_id":"b"}')[0] == 400
            assert call("/v1/rooms/scripts/reload", {"source": "return {}"})[0] == 400
            assert call("/v1/config/status", {})[0] == 200
            assert call("/v1/config/reload", {})[0] == 409
            code, metadata = call("/v1/rooms/create", dict(room_id="http-room", host_role_id="host", capacity=2, profile="skill_lab"))
            assert code == 200 and metadata["state"] == "lobby"
            base = "/rooms/http-room"
            assert call(base + "/healthz", authenticated=False)[0] == 200
            assert call(base + "/v1/enter", dict(role=role("host", "http-room")))[0] == 200
            assert call(base + "/v1/move", dict(ref(), seq=1, dx=1, dy=0))[0] == 409
            assert call(base + "/v1/ready", dict(ref(), ready=True))[0] == 200
            assert call(base + "/v1/start", ref())[1]["state"] == "playing"
            assert call(base + "/v1/move", dict(ref(), seq=1, dx=1, dy=0))[0] == 200
            time.sleep(.1)
            view = call(base + "/v1/view", ref())[1]
            assert view["self"]["x"] > 0 and view["room"]["room_id"] == "http-room"
            assert call(base + "/v1/view", ref(session=2))[0] == 409
            # 房间结束不能越过已有prepare/freeze，防止半途迁移被回收。
            pending = dict(ref("pending"), name="pending", scene_id="http-room", x=4, y=4, coins=0)
            assert call(base + "/v1/prepare", dict(role=pending, transfer_id="synthetic-transfer"))[0] == 200
            assert call(base + "/v1/finish", dict(ref(), control_authorized=True))[0] == 409
            assert call(base + "/v1/remove", dict(ref("pending"), transfer_id="synthetic-transfer"))[0] == 200
            assert call(base + "/v1/finish", ref())[1]["state"] == "finished"
            assert call(base + "/v1/enter", dict(role=role("new-player", "http-room")))[0] == 409
            # 已在房中的账号能以新session重连，用于取最终快照和离开。
            returning = role("host", "http-room"); returning["session_epoch"] = 2
            assert call(base + "/v1/enter", dict(role=returning))[0] == 200
            assert call(base + "/v1/view", ref(session=2))[0] == 200
            assert call(base + "/v1/disconnect", ref(session=2))[0] == 200
            time.sleep(.2)
            assert call("/v1/rooms/status", dict(room_id="http-room"))[0] == 200, "unacknowledged final snapshot was discarded"
            assert call(base + "/v1/remove", ref(session=2))[0] == 200
            for _ in range(40):
                if call("/v1/rooms/status", dict(room_id="http-room"))[0] == 404:
                    break
                time.sleep(.05)
            else:
                raise AssertionError("finished disconnected room was not reclaimed")
            assert call("/v1/rooms/create", dict(room_id="http-room", host_role_id="host", capacity=2, profile="skill_lab"))[0] == 409
            assert call("/healthz")[1]["rooms"] == 0
            print("PASS worker HTTP: old endpoints, strict JSON, rooms lifecycle, migration fence, finished reconnect, tombstones", flush=True)
        finally:
            process.terminate()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=5)
            log.close()
            if process.returncode not in (0, -15):
                print((work / "worker.log").read_text(errors="replace"), file=sys.stderr)


if __name__ == "__main__":
    main()
