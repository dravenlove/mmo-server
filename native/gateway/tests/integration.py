"""真实 C++ TCP/WS 入口契约与慢 I/O 隔离；全程仅使用内存假服务。"""
import base64
import hashlib
import http.client
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SECRET = "native-gateway-isolated-test-key-0001"
ORIGIN = "https://client.example.test"


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Fixture:
    def __init__(self):
        self.lock = threading.Lock()
        self.role = dict(role_id="controlled-player", name="controlled", scene_id="scene-test",
                         owner_epoch=3, session_epoch=7, x=0., y=0., coins=91,
                         combat=dict(revision=1, hp=180, dead=False))
        self.delays = {}
        self.calls = []
        self.saved = []
        self.disconnects = []
        self.session_status = 200
        self.changing = False
        self.statuses = {}
        self.overrides = {}
        self.transfer_error_commits = False
        self.scene = self.server("scene")
        self.backend = self.server("backend")

    def server(self, kind):
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_):
                pass

            def do_GET(self):
                self.respond(200, {"ok": True})

            def respond(self, status, data):
                wire = json.dumps(data).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(wire)))
                self.end_headers()
                try:
                    self.wfile.write(wire)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                assert self.headers.get("X-Internal-Token") == SECRET
                with fixture.lock:
                    fixture.calls.append((kind, self.path, body))
                    role = json.loads(json.dumps(fixture.role))
                    delay = fixture.delays.get(self.path, 0)
                    if kind == "backend" and self.path == "/v1/transfer":
                        fixture.changing = True
                if delay:
                    time.sleep(delay)
                if self.path in fixture.statuses:
                    if kind == "backend" and self.path == "/v1/transfer" and fixture.transfer_error_commits:
                        with fixture.lock:
                            fixture.role["scene_id"] = body["target_scene_id"]
                            fixture.role["owner_epoch"] += 1
                            fixture.changing = False
                    self.respond(fixture.statuses[self.path], {"error": "isolated fault injection"})
                    return
                if self.path in fixture.overrides:
                    self.respond(200, fixture.overrides[self.path])
                    return
                if kind == "backend":
                    if self.path in ("/v1/login", "/v1/session"):
                        if self.path == "/v1/session" and fixture.session_status != 200:
                            self.respond(fixture.session_status, {"error": "stale session"})
                        else:
                            self.respond(200, {"role": role, "scene_url": fixture.scene_url})
                    elif self.path in ("/v1/account_register", "/v1/account_login"):
                        assert body == {"name": "controlled", "password": "test-password"}
                        self.respond(200, {"token": "a" * 64, "role_id": role["role_id"], "expires_at_unix_ms": int(time.time()*1000)+86400000})
                    elif self.path == "/v1/account_bind":
                        assert body == {"token": "old-token", "password": "test-password"}
                        self.respond(409, {"error": "already bound"})
                    elif self.path == "/v1/register":
                        self.respond(200, {"token": "old-token", "role_id": role["role_id"]})
                    elif self.path == "/v1/checkpoint":
                        with fixture.lock:
                            fixture.saved.append(body["role"])
                        self.respond(200, {"ok": True})
                    elif self.path == "/v1/claim":
                        self.respond(200, {"coins": 101, "granted": True})
                    elif self.path == "/v1/transfer":
                        with fixture.lock:
                            fixture.role["scene_id"] = body["target_scene_id"]
                            fixture.role["owner_epoch"] += 1
                            fixture.changing = False
                            role = json.loads(json.dumps(fixture.role))
                        self.respond(200, {"role": role, "scene_url": fixture.scene_url})
                    else:
                        self.respond(404, {"error": "unknown backend route"})
                else:
                    assert body["role_id"] == role["role_id"]
                    if body["owner_epoch"] != role["owner_epoch"] or body["session_epoch"] != role["session_epoch"]:
                        self.respond(409, {"error": "stale owner or session"})
                        return
                    if self.path == "/v1/view":
                        if fixture.changing:
                            self.respond(409, {"error": "role is not active"})
                            return
                        role["coins"] = -100  # Backend 才是金币的事实来源。
                        self.respond(200, {"self": role, "players": [role], "scene_id": role["scene_id"],
                                           "tick": int(time.monotonic()*20), "world_epoch": "test-epoch",
                                           "event_cursor": 9007199254740993,
                                           "events": [{"id": 9007199254740993, "type": "test"}],
                                           "projectiles": [], "walls": [], "skill_cooldowns": {}})
                    elif self.path == "/v1/move":
                        with fixture.lock:
                            fixture.role["x"] += body["dx"]
                        self.respond(200, {"accepted": True, "seq": body["seq"]})
                    elif self.path == "/v1/cast":
                        with fixture.lock:
                            fixture.role["combat"]["revision"] += 1
                            role = json.loads(json.dumps(fixture.role))
                        self.respond(200, {"accepted": True, "role": role, "instance_id": "projectile-test"})
                    elif self.path == "/v1/skill_init":
                        assert body["definition"]["type"] == "projectile"
                        self.respond(200, {"accepted": True, "definition_version": 1, "skill_id": body["skill_id"]})
                    elif self.path == "/v1/disconnect":
                        with fixture.lock:
                            fixture.disconnects.append(body)
                            role = json.loads(json.dumps(fixture.role))
                        self.respond(200, {"role": role})
                    else:
                        self.respond(200, {"accepted": True})

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        return server

    @property
    def scene_url(self):
        return f"http://127.0.0.1:{self.scene.server_port}"

    @property
    def backend_url(self):
        return f"http://127.0.0.1:{self.backend.server_port}"

    def close(self):
        self.scene.shutdown()
        self.backend.shutdown()


def recv_exact(sock, length):
    result = b""
    while len(result) < length:
        piece = sock.recv(length-len(result))
        if not piece:
            raise EOFError("connection closed")
        result += piece
    return result


class Client:
    def __init__(self, number, ws=False, origin=ORIGIN, path="/ws"):
        self.sock = socket.create_connection(("127.0.0.1", number), 3)
        self.sock.settimeout(5)
        self.ws = ws
        self.incoming = b""
        self.events = []
        if ws:
            key = base64.b64encode(os.urandom(16)).decode()
            request = f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{number}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nOrigin: {origin}\r\n\r\n"
            self.sock.sendall(request.encode())
            headers = b""
            while not headers.endswith(b"\r\n\r\n"):
                headers += recv_exact(self.sock, 1)
            self.status = int(headers.split(b" ")[1])
            if self.status == 101:
                expected = base64.b64encode(hashlib.sha1((key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
                assert expected.lower() in headers.lower()

    def frame(self, wire, opcode=1):
        mask = os.urandom(4)
        size = len(wire)
        first = bytes([0x80 | opcode])
        if size < 126:
            first += bytes([0x80 | size])
        elif size < 65536:
            first += bytes([0x80 | 126])+struct.pack("!H", size)
        else:
            first += bytes([0x80 | 127])+struct.pack("!Q", size)
        self.sock.sendall(first+mask+bytes(byte ^ mask[i % 4] for i, byte in enumerate(wire)))

    def send(self, data):
        wire = json.dumps(data, separators=(",", ":")).encode()
        if self.ws:
            self.frame(wire)
        else:
            self.sock.sendall(wire+b"\n")

    def read(self):
        if self.ws:
            while True:
                first, second = recv_exact(self.sock, 2)
                opcode, size = first & 15, second & 127
                if size == 126:
                    size = struct.unpack("!H", recv_exact(self.sock, 2))[0]
                elif size == 127:
                    size = struct.unpack("!Q", recv_exact(self.sock, 8))[0]
                wire = recv_exact(self.sock, size)
                if opcode == 9:
                    self.frame(wire, 10)
                    continue
                if opcode == 8:
                    self.close_code = struct.unpack("!H", wire[:2])[0] if len(wire) >= 2 else 1005
                    raise EOFError("websocket closed")
                assert opcode == 1 and first & 0x80
                return json.loads(wire)
        while b"\n" not in self.incoming:
            piece = self.sock.recv(65536)
            if not piece:
                raise EOFError("TCP closed")
            self.incoming += piece
        line, self.incoming = self.incoming.split(b"\n", 1)
        return json.loads(line)

    def exchange(self, op, **fields):
        request = dict(id=f"test-{time.monotonic_ns()}", op=op, **fields)
        self.send(request)
        for _ in range(200):
            reply = self.read()
            if reply.get("id") == request["id"]:
                return reply
            self.events.append(reply)
        raise AssertionError("request acknowledgement missing")

    def close(self):
        self.sock.close()


class Gateway:
    def __init__(self, executable, fixture, profile, state_hz=20, login_scene=None):
        self.tcp, self.ws, self.health = port(), port(), port()
        env = {**os.environ, "INTERNAL_TOKEN": SECRET, "BACKEND_URL": fixture.backend_url,
               "GATEWAY_LISTEN": f"127.0.0.1:{self.tcp}", "WEBSOCKET_LISTEN": f"127.0.0.1:{self.ws}",
               "GATEWAY_HEALTH_LISTEN": f"127.0.0.1:{self.health}", "GATEWAY_PROFILE": profile,
               "GATEWAY_STATE_HZ": str(state_hz), "WEBSOCKET_ALLOWED_ORIGINS": ORIGIN,
               "GATEWAY_LOGIN_SCENE": "scene-test" if profile == "elemental" else "",
               "GATEWAY_ARENA_SCENE": "scene-arena" if profile == "elemental" else ""}
        if login_scene is not None:
            env["GATEWAY_LOGIN_SCENE"] = login_scene
        env.pop("TLS_CERT_FILE", None)
        env.pop("TLS_KEY_FILE", None)
        self.process = subprocess.Popen([executable], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(100):
            if self.process.poll() is not None:
                raise AssertionError("gateway exited: "+self.process.stderr.read().decode())
            try:
                conn = http.client.HTTPConnection("127.0.0.1", self.health, timeout=.2)
                conn.request("GET", "/healthz")
                reply = conn.getresponse()
                assert reply.status == 200
                self.metadata = json.loads(reply.read())
                conn.close()
                return
            except OSError:
                time.sleep(.03)
        raise AssertionError("gateway startup timeout")

    def close(self):
        self.process.send_signal(signal.SIGTERM)
        try:
            _, errors = self.process.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.kill()
            _, errors = self.process.communicate()
            raise AssertionError("gateway did not drain: "+errors.decode())
        assert self.process.returncode == 0, errors.decode()


def wait_until(predicate, message, seconds=5):
    deadline = time.monotonic()+seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.03)
    raise AssertionError(message)


def main(executable):
    checks = 0
    for profile in ("movement", "abilities", "elemental", "skill_lab", "full"):
        fixture = Fixture()
        gateway = Gateway(executable, fixture, profile)
        try:
            assert gateway.metadata["implementation"] == "cpp"
            for origin, path, status in (("https://evil.example.test", "/ws", 403), (ORIGIN, "/ws?token=hidden", 400), (ORIGIN, "/other", 404)):
                client = Client(gateway.ws, ws=True, origin=origin, path=path)
                assert client.status == status
                client.close()
                checks += 1
            for websocket in (False, True):
                client = Client(gateway.ws if websocket else gateway.tcp, ws=websocket)
                try:
                    assert client.exchange("ping")["data"]["account_protocol"] == "account-password-v1"
                    assert client.exchange("account_login", name="controlled", password="test-password")["ok"]
                    conflict = client.exchange("account_bind", token="old-token", password="test-password")
                    assert not conflict["ok"] and conflict["status"] == 409
                    assert not client.exchange("move", seq=1, dx=1, dy=0)["ok"]
                    assert client.exchange("login", token="old-token")["ok"]
                    assert not client.exchange("account_login", name="controlled", password="test-password")["ok"]
                    bad = client.exchange("move", seq=1, dx=1, dy=0, role_id="victim")
                    assert not bad["ok"]
                    fixture.delays["/v1/view"] = .65
                    # 状态查询占用期间，方向输入仍须及时执行，不能被阻塞请求串行化。
                    time.sleep(.1)
                    started = time.monotonic()
                    move = client.exchange("move", seq=2, dx=1, dy=0)
                    assert move["ok"] and time.monotonic()-started < .5
                    fixture.delays["/v1/view"] = 0
                    view = client.exchange("view")
                    assert view["ok"] and view["data"]["self"]["coins"] == 91
                    if profile == "movement":
                        assert "combat" not in view["data"]["self"] and "projectiles" not in view["data"]
                    if profile == "skill_lab":
                        assert client.exchange("skill_init", skill_id="client-bolt", definition={"type": "projectile"}, world_epoch="test-epoch")["ok"]
                        assert client.exchange("cast", seq=3, skill_id="client-bolt", dx=1, dy=0, definition_version=1, world_epoch="test-epoch")["ok"]
                    if profile in ("abilities", "full"):
                        assert client.exchange("cast", seq=3, skill_id="strike", target_id="npc-1")["ok"]
                    checks += 9
                finally:
                    expected = len(fixture.disconnects)+1
                    client.close()
                wait_until(lambda: len(fixture.disconnects) >= expected, "final disconnect missing")
                wait_until(lambda: fixture.saved and fixture.saved[-1]["x"] == fixture.role["x"], "final authoritative position was not saved")
            for payload, opcode, code in ((b"{}", 2, 1003), (b"not-json", 1, 1007), (b" "*65536, 1, 1009)):
                client = Client(gateway.ws, ws=True)
                client.frame(payload, opcode)
                try:
                    client.read()
                    raise AssertionError("invalid WebSocket frame was accepted")
                except EOFError:
                    assert client.close_code == code, (client.close_code, code)
                finally:
                    client.close()
                checks += 1
            if profile == "full":
                client = Client(gateway.ws, ws=True)
                assert client.exchange("login", token="old-token")["ok"]
                fixture.delays["/v1/transfer"] = .35
                transferred = client.exchange("transfer", target_scene_id="scene-next", request_id="stable-transfer-id")
                assert transferred["ok"] and transferred["data"]["scene_id"] == "scene-next"
                assert all(event.get("ok") for event in client.events), "view/session polled a frozen source during ownership transition"
                assert client.exchange("view")["ok"]
                client.close()
                fixture.delays["/v1/transfer"] = 0
                checks += 3
            # 服务端保留 uint64 事件游标；自动推送只发一次同一可靠连接已交付事件。
            if profile == "skill_lab":
                client = Client(gateway.ws, ws=True)
                assert client.exchange("login", token="old-token")["ok"]
                states = []
                while len(states) < 3:
                    value = client.read()
                    if value.get("op") == "state" and value.get("ok"):
                        states.append(value["data"])
                assert states[0]["event_cursor"] == 9007199254740993
                assert len(states[0]["events"]) == 1 and states[1]["events"] == []
                # 查询超过一个 50ms 周期时，将等待的节拍合并并立即续查，不能额外空等一个周期。
                fixture.delays["/v1/view"] = .065
                times = []
                while len(times) < 10:
                    value = client.read()
                    if value.get("op") == "state" and value.get("ok"):
                        times.append(time.monotonic())
                assert times[-1]-times[1] < .72, "state stream lost coalesced tick demand"
                fixture.delays["/v1/view"] = .3
                time.sleep(.08)
                cast = client.exchange("cast", seq=50, skill_id="client-bolt", dx=1, dy=0,
                                       definition_version=1, world_epoch="test-epoch")
                current_revision = cast["data"]["role"]["combat"]["revision"]
                observed = 0
                while observed < 3:
                    value = client.read()
                    if value.get("op") == "state" and value.get("ok"):
                        assert value["data"]["self"]["combat"]["revision"] >= current_revision, "old in-flight view overwrote action snapshot"
                        observed += 1
                fixture.delays["/v1/view"] = 0
                fixture.session_status = 409
                wait_until(lambda: any(x[1] == "/v1/session" for x in fixture.calls), "session refresh missing")
                deadline = time.monotonic()+5
                closed = False
                while time.monotonic() < deadline:
                    try:
                        client.read()
                    except (EOFError, OSError):
                        closed = True
                        break
                assert closed, "stale session remained connected"
                client.close()
                checks += 3
        finally:
            gateway.close()
            fixture.close()
    # 显式慢查询与自动查询并发：旧快照既不能回显给用户，也不能覆盖断开保存的备份。
    fixture = Fixture()
    fixture.delays["/v1/view"] = 1.7
    gateway = Gateway(executable, fixture, "abilities", state_hz=1)
    client = Client(gateway.ws, ws=True)
    try:
        assert client.exchange("login", token="old-token")["ok"]
        client.send(dict(id="explicit-old", op="view"))
        wait_until(lambda: any(call[1] == "/v1/view" for call in fixture.calls), "slow explicit view did not start")
        with fixture.lock:
            fixture.role["combat"] = dict(revision=9, hp=99, dead=False)
            fixture.delays["/v1/view"] = 0
        seen_new = False
        while True:
            value = client.read()
            if value.get("op") == "state" and value.get("ok"):
                seen_new = value["data"]["self"]["combat"]["revision"] == 9
                if seen_new:
                    fixture.delays["/v1/view"] = 3
            if value.get("id") == "explicit-old":
                assert seen_new, "new automatic view did not overtake explicit view"
                assert value["ok"] and value["data"]["self"]["combat"]["revision"] == 9, "stale explicit view was shown after a newer state"
                break
        fixture.statuses["/v1/disconnect"] = 503
        previous_saves = len(fixture.saved)
        client.close()
        wait_until(lambda: len(fixture.saved) > previous_saves, "disconnect failure did not save last known authoritative state")
        assert fixture.saved[-1]["combat"] == dict(revision=9, hp=99, dead=False), "disconnect fallback persisted stale explicit view"
        checks += 3
    finally:
        client.close()
        gateway.close()
        fixture.close()
    fixture = Fixture()
    fixture.role["scene_id"] = "scene-arena"
    fixture.statuses["/v1/view"] = 503
    gateway = Gateway(executable, fixture, "elemental")
    client = Client(gateway.ws, ws=True)
    try:
        failed = client.exchange("login", token="old-token")
        assert not failed["ok"] and failed["status"] == 503
        retry = client.exchange("login", token="old-token")
        assert not retry["ok"] and "already logged in" in retry["error"]
        assert len([call for call in fixture.calls if call[:2] == ("backend", "/v1/login")]) == 1
        client.close()
        wait_until(lambda: bool(fixture.disconnects), "successful arena entry leaked after initial-view failure")
        wait_until(lambda: bool(fixture.saved), "partially successful login did not save final authoritative state")
        assert fixture.disconnects[-1] == dict(role_id="controlled-player", owner_epoch=3, session_epoch=7)
        checks += 3
    finally:
        client.close()
        gateway.close()
        fixture.close()
    for committed in (False, True):
        fixture = Fixture()
        fixture.role["scene_id"] = "scene-origin"
        fixture.statuses["/v1/transfer"] = 503
        fixture.transfer_error_commits = committed
        gateway = Gateway(executable, fixture, "movement", login_scene="scene-test")
        client = Client(gateway.ws, ws=True)
        try:
            failed = client.exchange("login", token="old-token")
            assert not failed["ok"] and failed["status"] == 503
            retry = client.exchange("login", token="old-token")
            assert not retry["ok"] and "already logged in" in retry["error"]
            assert len([call for call in fixture.calls if call[:2] == ("backend", "/v1/login")]) == 1
            client.close()
            wait_until(lambda: any(call[1] == "/v1/disconnect" for call in fixture.calls), "forced-entry transfer failure discarded established login")
            cleanup = [call[2] for call in fixture.calls if call[1] == "/v1/disconnect"][-1]
            assert cleanup == dict(role_id="controlled-player", owner_epoch=3, session_epoch=7)
            if committed:
                assert fixture.role["owner_epoch"] == 4 and fixture.role["scene_id"] == "scene-test"
                assert not fixture.disconnects and not fixture.saved, "uncertain transfer cleanup touched a newer owner"
            else:
                wait_until(lambda: bool(fixture.saved), "forced-entry failure did not save final source state")
                assert fixture.disconnects[-1] == cleanup
            checks += 3
        finally:
            client.close()
            gateway.close()
            fixture.close()
    fixture = Fixture()
    fixture.role["scene_id"] = "scene-arena"
    fixture.overrides["/v1/view"] = []
    gateway = Gateway(executable, fixture, "elemental")
    client = Client(gateway.ws, ws=True)
    try:
        failed = client.exchange("login", token="old-token")
        assert not failed["ok"] and failed["error"] == "invalid service response"
        client.close()
        wait_until(lambda: bool(fixture.disconnects) and bool(fixture.saved), "malformed initial view lost established arena session")
        checks += 3
    finally:
        client.close()
        gateway.close()
        fixture.close()
    print(f"native gateway integration passed ({checks} checks; 5 profiles, TCP and direct C++ WebSocket)")


if __name__ == "__main__":
    main(sys.argv[1])
