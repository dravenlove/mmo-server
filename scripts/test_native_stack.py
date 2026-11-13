#!/usr/bin/env python3
"""真实 C++ 网关/账号/Backend/Scene 联调；只创建临时目录和随机端口。"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

from smoke import free_port, http


def unique_ports(names):
    ports = {}
    for name in names:
        for _ in range(100):
            candidate = free_port()
            if candidate not in ports.values():
                ports[name] = candidate
                break
        else:
            raise AssertionError("could not allocate distinct fixture ports")
    return ports


class WebSocketClosed(Exception):
    def __init__(self, code):
        self.code = code


class WebSocket:
    def __init__(self, port, origin="https://native-test.example", path="/ws", accepted=True):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.socket.settimeout(5)
        self.reader = self.socket.makefile("rb")
        key = base64.b64encode(secrets.token_bytes(16)).decode()
        self.socket.sendall((f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
                            f"Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                            f"Sec-WebSocket-Version: 13\r\nOrigin: {origin}\r\n\r\n").encode())
        status = self.reader.readline().decode()
        headers = {}
        while True:
            line = self.reader.readline()
            if line == b"\r\n":
                break
            if not line:
                raise AssertionError("truncated handshake")
            k, v = line.decode().split(":", 1)
            headers[k.lower()] = v.strip()
        if accepted:
            assert " 101 " in status, status
            expected = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            assert headers["sec-websocket-accept"] == expected
        else:
            assert " 101 " not in status, "unapproved browser origin/query accepted"
        self.counter = 0

    def frame(self, payload, opcode=1):
        mask = secrets.token_bytes(4)
        size = len(payload)
        head = bytes([0x80 | opcode])
        head += bytes([0x80 | size]) if size < 126 else bytes([0x80 | 126]) + struct.pack("!H", size)
        self.socket.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def receive(self):
        # Beast可把一条较大的文本消息拆成text+continuation帧；JSON边界是FIN而非首帧。
        fragments = []
        total = 0
        while True:
            head = self.reader.read(2)
            if len(head) != 2:
                raise AssertionError("WebSocket closed unexpectedly")
            flags, size = head
            assert not flags & 0x70, "unexpected extensions on non-negotiated connection"
            assert not size & 0x80, "server frame must not be masked"
            size &= 127
            if size == 126:
                size = struct.unpack("!H", self.reader.read(2))[0]
            elif size == 127:
                size = struct.unpack("!Q", self.reader.read(8))[0]
            assert size <= 2 * 1024 * 1024
            data = self.reader.read(size)
            assert len(data) == size
            opcode = flags & 15
            if opcode in (8, 9, 10):
                assert flags & 0x80 and size <= 125, "invalid control frame"
                if opcode == 9:
                    self.frame(data, 10)
                elif opcode == 8:
                    raise WebSocketClosed(struct.unpack("!H", data[:2])[0] if len(data) >= 2 else 1005)
                continue
            assert opcode == (0 if fragments else 1), ("unexpected server opcode", opcode)
            fragments.append(data)
            total += size
            assert total <= 2 * 1024 * 1024
            if flags & 0x80:
                return json.loads(b"".join(fragments))

    def request(self, op, ok=True, **fields):
        self.counter += 1
        ident = str(self.counter)
        self.frame(json.dumps(dict(id=ident, op=op, **fields), ensure_ascii=False).encode())
        for _ in range(250):
            response = self.receive()
            if response.get("id") == ident:
                assert response.get("ok") is ok, (op, response)
                return response.get("data") if ok else response
        raise AssertionError("too many states without acknowledgement")

    def close(self):
        self.reader.close()
        self.socket.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--scene", required=True, type=Path)
    args = parser.parse_args()
    binaries = {"auth": args.build / "account/native-account",
                "backend": args.build / "game_service/native-game-service",
                "gateway": args.build / "gateway/native-gateway", "scene": args.scene}
    for binary in binaries.values():
        assert binary.is_file(), str(binary)
    inherited = {k: v for k, v in os.environ.items() if not k.startswith(
        ("MYSQL_", "AUTH_", "BACKEND_", "GATEWAY_", "WEBSOCKET_", "WS_", "SCENE_", "INTERNAL_TOKEN", "TLS_"))}
    checks = []
    with tempfile.TemporaryDirectory(prefix="mmo-native-stack-") as directory:
        work = Path(directory)
        common = dict(inherited, INTERNAL_TOKEN=secrets.token_hex(32))
        ports = unique_ports(("auth", "backend", "scene", "tcp", "ws", "health"))
        urls = {key: f"http://127.0.0.1:{port}" for key, port in ports.items()}
        children, logs, peers = {}, [], []

        def stop(name):
            child = children.pop(name, None)
            if child:
                child.terminate()
                try:
                    child.wait(timeout=12)
                except subprocess.TimeoutExpired:
                    child.kill(); child.wait(timeout=5)

        def start(name, env):
            log = (work / (name + ".log")).open("ab")
            logs.append(log)
            child = subprocess.Popen([str(binaries[name].resolve())], env=env, stdout=log, stderr=log)
            children[name] = child
            url = urls["health" if name == "gateway" else name] + "/healthz"
            for _ in range(160):
                if child.poll() is not None:
                    raise AssertionError(name + " exited: " + (work / (name + ".log")).read_text(errors="replace"))
                try:
                    if http(url, timeout=.3)[0] == 200:
                        return
                except (OSError, urllib.error.URLError):
                    pass
                time.sleep(.05)
            raise AssertionError(name + " health timed out")

        def passed(label):
            checks.append(label)
            print("PASS", label, flush=True)

        try:
            start("auth", dict(common, AUTH_DB_DRIVER="sqlite", AUTH_DB=str(work / "auth.db"),
                               AUTH_LISTEN=f"127.0.0.1:{ports['auth']}"))
            for profile in ("movement", "abilities", "elemental", "skill_lab"):
                mode = {"movement": "movement", "abilities": "abilities", "elemental": "elemental", "skill_lab": "skill_lab"}[profile]
                scene_id = "scene-" + profile
                start("scene", dict(common, SCENE_HOST="127.0.0.1", SCENE_PORT=str(ports["scene"]),
                                    SCENE_ID=scene_id, SCENE_MODE=mode, ELEMENTAL_ARENA_ID="scene-arena"))
                start("backend", dict(common, BACKEND_DB_DRIVER="sqlite", BACKEND_DB=str(work / (profile + ".db")),
                                      BACKEND_LISTEN=f"127.0.0.1:{ports['backend']}", AUTH_URL=urls["auth"],
                                      SCENES=f"{scene_id}={urls['scene']},scene-arena={urls['scene']}"))
                gateway_env = dict(common, GATEWAY_LISTEN=f"127.0.0.1:{ports['tcp']}",
                                   WEBSOCKET_LISTEN=f"127.0.0.1:{ports['ws']}",
                                   WEBSOCKET_ALLOWED_ORIGINS="https://native-test.example",
                                   GATEWAY_HEALTH_LISTEN=f"127.0.0.1:{ports['health']}", BACKEND_URL=urls["backend"],
                                   GATEWAY_PROFILE=profile, GATEWAY_LOGIN_SCENE=scene_id,
                                   GATEWAY_ARENA_SCENE="scene-arena")
                start("gateway", gateway_env)
                for origin, path in (("https://evil.example", "/ws"), ("https://native-test.example", "/ws?token=forbidden")):
                    bad = WebSocket(ports["ws"], origin, path, accepted=False); bad.close()
                first = WebSocket(ports["ws"]); peers.append(first)
                first.request("ping")
                account = first.request("account_register", name="Native-" + profile, password="synthetic-Password-123!")
                first.request("account_login", ok=False, name="Native-" + profile, password="wrong-password-123")
                ticket = first.request("account_login", name="Native-" + profile, password="synthetic-Password-123!")
                role = first.request("login", token=ticket["token"])
                assert role["role_id"] == account["role_id"]
                view = first.request("view")
                assert view["self"]["role_id"] == role["role_id"]
                rtts = []
                for seq in range(1, 9):
                    before = time.monotonic()
                    moved = first.request("move", seq=seq, dx=1, dy=0)
                    assert moved.get("accepted")
                    rtts.append((time.monotonic() - before) * 1000)
                    time.sleep(.05)
                assert max(rtts) < 1000, (profile, rtts)
                if profile == "skill_lab":
                    view = first.request("view")
                    initialized = first.request("skill_init", skill_id="custom-native-bolt", world_epoch=view["world_epoch"],
                                                expected_definition_version=0, definition={"type": "projectile", "name": "Native bolt",
                                                "damage": 15, "mp_cost": 3, "cooldown_ms": 100, "range": 20, "speed": 12, "radius": .2})
                    assert initialized["accepted"]
                    cast = first.request("cast", seq=1, skill_id="custom-native-bolt", world_epoch=view["world_epoch"],
                                         definition_version=initialized["definition"]["definition_version"], dx=1, dy=0)
                    assert cast["accepted"], cast
                second = WebSocket(ports["ws"]); peers.append(second)
                replaced = second.request("login", token=ticket["token"])
                assert replaced["session_epoch"] > role["session_epoch"]
                try:
                    first.request("move", ok=False, seq=20, dx=0, dy=1)
                except WebSocketClosed as closed:
                    assert closed.code in (1000, 1008), closed.code
                old_reference = {key: role[key] for key in ("role_id", "owner_epoch", "session_epoch")}
                assert http(urls["scene"] + "/v1/move", dict(old_reference, seq=21, dx=0, dy=1), common["INTERNAL_TOKEN"])[0] == 409
                first.close(); peers.remove(first)
                second.close(); peers.remove(second)
                stop("gateway")
                start("gateway", gateway_env)
                reconnect = WebSocket(ports["ws"]); peers.append(reconnect)
                restored = reconnect.request("login", token=ticket["token"])
                assert restored["role_id"] == role["role_id"]
                reconnect.close(); peers.remove(reconnect)
                passed(f"{profile}: direct C++ WebSocket, Origin, accounts, movement, fencing and restart; local move max={max(rtts):.1f}ms")
                stop("gateway"); stop("backend"); stop("scene")
        finally:
            for peer in peers:
                peer.close()
            for name in reversed(list(children)):
                stop(name)
            for log in logs:
                log.close()
        print(json.dumps({"ok": True, "checks": checks, "scope": "isolated local C++ processes; excludes VPS and Unity visual acceptance"}, ensure_ascii=False))


if __name__ == "__main__":
    main()
