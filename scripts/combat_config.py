#!/usr/bin/env python3
"""通过场景容器的内部接口管理战斗数值；只使用 Python 标准库。"""
import argparse
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DIRECTORY = ROOT / "deploy" / "combat-config"
DEFAULT_CONFIG = ROOT / "configs" / "combat.default.json"
CONTAINER_CONFIG = "/app/config/current.json"
SERVICES = ("scene1", "scene2")
MAX_CONFIG_BYTES = 65536
# 路径是程序常量；令牌只在容器内展开，不读取宿主机令牌、不把令牌放进参数。
RPC_COMMANDS = {
    operation: 'curl --silent --show-error --noproxy "*" --connect-timeout 3 --max-time 10 '
    '--request POST --header "Content-Type: application/json" '
    '--header "X-Internal-Token: $INTERNAL_TOKEN" --data-binary @- '
    '--write-out "\\n%{http_code}" '
    '"http://127.0.0.1:${SCENE_PORT}/v1/config/' + operation + '"'
    for operation in ("status", "validate", "reload")
}


class ToolError(Exception):
    def __init__(self, message, recovery=None):
        super().__init__(message)
        self.recovery = recovery


@dataclass(frozen=True)
class Paths:
    env_file: Path
    compose_file: Path
    directory: Path
    project_name: str = ""


def encode_json(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2,
                       allow_nan=False) + "\n").encode("utf-8")


def decode_config(data):
    if len(data) > MAX_CONFIG_BYTES:
        raise ToolError("配置文件超过 64 KiB。")

    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ToolError("配置包含重复 JSON 字段。")
            result[key] = value
        return result

    try:
        value = json.loads(data.decode("utf-8-sig"), object_pairs_hook=unique_pairs,
                           parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
    except (UnicodeError, ValueError) as error:
        raise ToolError("配置必须为有效的 UTF-8 JSON，不能含 NaN/Infinity。") from error
    if not isinstance(value, dict) or not isinstance(value.get("version"), str) or not value["version"]:
        raise ToolError("配置必须为对象，并包含非空字符串 version；完整 schema 由两个场景校验。")
    return value


def read_config(path):
    if path.is_symlink() or not path.is_file():
        raise ToolError(f"配置文件不存在、不是普通文件或是符号链接：{path}")
    with path.open("rb") as stream:
        data = stream.read(MAX_CONFIG_BYTES + 1)
    return decode_config(data), data


def sync_directory(directory):
    # Windows 不支持通过普通目录句柄 fsync；文件自身仍先 flush/fsync 再 replace。
    if os.name != "nt":
        descriptor = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)


def atomic_write(path, data, mode=0o644):
    descriptor, temporary = tempfile.mkstemp(prefix="." + path.name + ".", dir=path.parent)
    try:
        os.chmod(temporary, mode)
        with os.fdopen(descriptor, "wb") as stream:
            descriptor = None
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        sync_directory(path.parent)
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if os.path.exists(temporary):
            os.unlink(temporary)


@contextmanager
def exclusive_lock(directory):
    path = directory / ".combat-config.lock"
    content = encode_json({"pid": os.getpid(), "owner": secrets.token_hex(16)})
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError as error:
        raise ToolError(f"配置目录已锁定：{path}；请确认没有正在运行的工具，不会自动删除已有锁。") from error
    identity = os.fstat(descriptor)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        yield
    finally:
        # 只移除本次建立的锁；外部替换过的锁留给操作者检查。
        try:
            current = path.lstat()
            if (current.st_dev, current.st_ino) == (identity.st_dev, identity.st_ino) and not path.is_symlink() and path.read_bytes() == content:
                path.unlink()
        except FileNotFoundError:
            pass


def env_settings(raw):
    settings = {}
    for index, line in enumerate(raw.splitlines(keepends=True)):
        inspected = line[3:] if index == 0 and line.startswith(b"\xef\xbb\xbf") else line
        match = re.match(rb"^[ \t]*(?:export[ \t]+)?(COMBAT_CONFIG_FILE|COMBAT_CONFIG_DIR)[ \t]*=(.*?)(?:\r?\n)?$", inspected)
        if not match:
            continue
        key = match.group(1).decode("ascii")
        if key in settings:
            raise ToolError(f"env 中 {key} 重复，请先人工核对。")
        try:
            value = match.group(2).decode("utf-8").strip()
        except UnicodeError as error:
            raise ToolError(f"env 中 {key} 不是 UTF-8 路径。") from error
        if value.startswith(("'", '"')):
            closing = value.find(value[0], 1)
            if closing < 0 or (value[closing + 1:].strip() and not value[closing + 1:].strip().startswith("#")):
                raise ToolError(f"env 中 {key} 的引号格式不受支持，请先人工核对。")
            value = value[1:closing]
        else:
            value = "" if value.startswith("#") else re.split(r"\s+#", value, maxsplit=1)[0].strip()
        if "$" in value or "\x00" in value:
            raise ToolError(f"env 中 {key} 使用动态展开，请改成明确路径后核对。")
        settings[key] = (value, index)
    return settings


def check_environment(paths, initializing=False):
    if paths.env_file.is_symlink() or not paths.env_file.is_file():
        raise ToolError("实际 env 文件必须先存在；本工具不会从模板创建残缺 env。")
    if not paths.compose_file.is_file():
        raise ToolError(f"Compose 文件不存在：{paths.compose_file}")
    raw = paths.env_file.read_bytes()
    settings = env_settings(raw)
    config_file = settings.get("COMBAT_CONFIG_FILE", ("", None))[0]
    if config_file not in ("", CONTAINER_CONFIG):
        raise ToolError("COMBAT_CONFIG_FILE 与固定容器路径不一致，请先人工核对；未修改 env。")
    configured_directory = settings.get("COMBAT_CONFIG_DIR", ("", None))[0]
    mounted_directory = Path(configured_directory) if configured_directory else Path("./combat-config")
    if not mounted_directory.is_absolute():
        mounted_directory = paths.compose_file.parent / mounted_directory
    if mounted_directory.resolve() != paths.directory.resolve():
        raise ToolError("COMBAT_CONFIG_DIR 与 --directory 不一致，请先人工核对；未修改 env。")
    if config_file:
        return raw, None
    if not initializing or paths.directory.resolve() != DEFAULT_DIRECTORY.resolve():
        raise ToolError("COMBAT_CONFIG_FILE 为空：默认目录请先运行 init；自定义目录须人工设为 /app/config/current.json。")
    lines = raw.splitlines(keepends=True)
    newline = b"\r\n" if b"\r\n" in raw else b"\n"
    replacement = b"COMBAT_CONFIG_FILE=" + CONTAINER_CONFIG.encode("ascii") + newline
    index = settings.get("COMBAT_CONFIG_FILE", ("", None))[1]
    if index is None:
        if lines and not lines[-1].endswith((b"\n", b"\r")):
            lines[-1] += newline
        lines.append(replacement)
    else:
        lines[index] = replacement
    return raw, b"".join(lines)


class DockerRPC:
    def __init__(self, paths):
        self.paths = paths

    def __call__(self, service, operation, payload):
        if service not in SERVICES or operation not in RPC_COMMANDS:
            raise ToolError("不支持的内部配置操作。")
        command = ["docker", "compose", "--env-file", str(self.paths.env_file),
                   "--file", str(self.paths.compose_file)]
        if self.paths.project_name:
            command += ["--project-name", self.paths.project_name]
        command += ["exec", "-T", service, "sh", "-c", RPC_COMMANDS[operation]]
        try:
            result = subprocess.run(command, input=encode_json(payload), capture_output=True, timeout=25)
        except subprocess.TimeoutExpired as error:
            raise ToolError(f"{service} {operation} 超时，操作结果可能未知。") from error
        except OSError as error:
            raise ToolError("无法启动 Docker CLI，请检查 Docker 与当前用户权限。") from error
        # 不回显 Docker/curl stderr，它可能包含 Compose 展开的 env 内容。
        if result.returncode:
            raise ToolError(f"{service} {operation} 的 Docker/curl 命令失败（退出码 {result.returncode}）。")
        try:
            body, code = result.stdout.rsplit(b"\n", 1)
            status = int(code.strip())
            response = json.loads(body)
        except (ValueError, TypeError) as error:
            raise ToolError(f"{service} {operation} 返回无效 HTTP/JSON 响应。") from error
        if not 200 <= status < 300:
            raise ToolError(f"{service} {operation} 返回 HTTP {status}；检查配置、代次或接口支持情况。")
        if not isinstance(response, dict):
            raise ToolError(f"{service} {operation} 返回了非对象响应。")
        return response


def state_fields(response):
    config = response.get("config")
    version = response.get("version")
    generation, tick = response.get("generation"), response.get("applied_tick")
    if (not isinstance(config, dict) or not isinstance(version, str) or not version or config.get("version") != version or
            type(generation) is not int or generation < 1 or type(tick) is not int or tick < 0):
        raise ToolError("场景配置响应缺少有效 config/version/generation/applied_tick。")
    return response


class Manager:
    def __init__(self, paths, rpc=None):
        self.paths = paths
        self.rpc = rpc or DockerRPC(paths)
        self.current = paths.directory / "current.json"
        self.previous = paths.directory / "previous.json"

    def run(self, command, source=None):
        raw, replacement = check_environment(self.paths, initializing=command == "init")
        if self.paths.directory.is_symlink():
            raise ToolError("配置目录不能是符号链接。")
        if command == "init":
            self.paths.directory.mkdir(parents=True, exist_ok=True, mode=0o755)
            os.chmod(self.paths.directory, 0o755)
        elif not self.paths.directory.is_dir():
            raise ToolError("配置目录不存在，请先运行 init。")
        with exclusive_lock(self.paths.directory):
            if command == "init":
                created = not self.current.exists()
                if created:
                    config, _ = read_config(DEFAULT_CONFIG)
                    atomic_write(self.current, encode_json(config))
                else:
                    config, _ = read_config(self.current)
                backup = None
                if replacement is not None:
                    if self.paths.env_file.read_bytes() != raw:
                        raise ToolError("env 在初始化期间发生变化，请重新核对；未覆盖。")
                    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
                    backup = self.paths.env_file.with_name(self.paths.env_file.name + "." + stamp + "." + secrets.token_hex(3) + ".bak.env")
                    descriptor = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                    with os.fdopen(descriptor, "wb") as stream:
                        stream.write(raw)
                        stream.flush()
                        os.fsync(stream.fileno())
                    os.chmod(backup, 0o600)
                    atomic_write(self.paths.env_file, replacement, 0o600)
                return {"created": created, "version": config["version"], "directory": str(self.paths.directory),
                        "env_updated": replacement is not None, "env_backup": str(backup) if backup else None,
                        "restart_required": replacement is not None}
            if command == "status":
                config, _ = read_config(self.current)
                nodes = self.statuses()
                return {"directory": str(self.paths.directory), "current_version": config["version"], "nodes": nodes,
                        "consistent": all(node["config"] == config for node in nodes.values())}
            if command == "publish":
                config, _ = read_config(Path(source))
            elif command == "rollback":
                config, _ = read_config(self.previous)
            else:
                raise ToolError("不支持的命令。")
            return self.publish(config)

    def statuses(self):
        return {service: state_fields(self.rpc(service, "status", {})) for service in SERVICES}

    def publish(self, candidate):
        old, old_bytes = read_config(self.current)
        if self.previous.is_symlink():
            raise ToolError("previous.json 不能是符号链接。")
        previous_bytes = self.previous.read_bytes() if self.previous.exists() else None
        before = self.statuses()
        if any(node["config"] != old for node in before.values()):
            raise ToolError("两个场景的生效配置必须先与 current.json 一致；请运行 status 人工核对。")
        normalized = []
        for service in SERVICES:
            initial = before[service]
            checked = state_fields(self.rpc(service, "validate", {"config": candidate, "expected_generation": initial["generation"]}))
            if (checked.get("valid") is not True or checked.get("current_version") != initial["version"] or
                    checked["generation"] != initial["generation"] or type(checked.get("would_change")) is not bool or
                    checked["would_change"] != (checked["config"] != initial["config"])):
                raise ToolError(f"{service} validate 响应与准备阶段不一致。")
            normalized.append(checked["config"])
        if normalized[0] != normalized[1]:
            raise ToolError("两个场景对配置的规范化结果不一致；未写入文件。")
        desired = normalized[0]
        if desired == old:
            after = self.statuses()
            if self.current.read_bytes() != old_bytes or any(node["config"] != old or node["generation"] != before[service]["generation"] for service, node in after.items()):
                raise ToolError("幂等发布校验期间发生并发修改；未写入文件或 reload。")
            return {"changed": False, "version": old["version"], "nodes": after}
        desired_bytes, saved_previous = encode_json(desired), encode_json(old)
        try:
            if self.current.read_bytes() != old_bytes:
                raise ToolError("current.json 在准备阶段发生变化，拒绝覆盖。")
            atomic_write(self.previous, saved_previous)
            atomic_write(self.current, desired_bytes)
            for service in SERVICES:
                expected = before[service]["generation"]
                result = state_fields(self.rpc(service, "reload", {"expected_generation": expected}))
                if result["config"] != desired or result["generation"] != expected + 1 or result.get("changed") is not True:
                    raise ToolError(f"{service} reload 未确认预期配置与代次。")
            after = self.statuses()
            if any(node["config"] != desired or node["generation"] != before[service]["generation"] + 1 for service, node in after.items()):
                raise ToolError("发布后的状态校验失败，可能有并发配置操作。")
            return {"changed": True, "version": desired["version"], "nodes": after}
        except (Exception, KeyboardInterrupt) as error:
            recovery = self.recover(old, old_bytes, previous_bytes, desired, desired_bytes, saved_previous, before)
            raise ToolError(f"发布失败：{error}", recovery) from error

    @staticmethod
    def restore_file(path, owned, original):
        if path.is_symlink():
            return False
        present = path.read_bytes() if path.exists() else None
        if present == original:
            return True
        if present != owned:
            return False
        if original is None:
            path.unlink()
            sync_directory(path.parent)
        else:
            atomic_write(path, original)
        return True

    def recover(self, old, old_bytes, previous_bytes, desired, desired_bytes, saved_previous, before):
        result = {"current_restored": False, "previous_restored": False, "nodes": {}}
        for key, path, owned, original in (("current_restored", self.current, desired_bytes, old_bytes),
                                           ("previous_restored", self.previous, saved_previous, previous_bytes)):
            try:
                result[key] = self.restore_file(path, owned, original)
            except OSError:
                pass
        for service in SERVICES:
            try:
                current = state_fields(self.rpc(service, "status", {}))
                if current["config"] == old:
                    result["nodes"][service] = {"restored": True, "generation": current["generation"], "compensated": False}
                    continue
                expected = before[service]["generation"] + 1
                if not result["current_restored"]:
                    raise ToolError("旧文件未恢复，禁止发出补偿 reload。")
                if current["config"] != desired or current["generation"] != expected:
                    raise ToolError("节点存在未知或并发版本，未覆盖；需要人工恢复。")
                response = state_fields(self.rpc(service, "reload", {"expected_generation": expected}))
                confirmed = state_fields(self.rpc(service, "status", {}))
                if response["config"] != old or confirmed["config"] != old or confirmed["generation"] != response["generation"]:
                    raise ToolError("补偿后的配置未得到确认，需要人工恢复。")
                result["nodes"][service] = {"restored": True, "generation": confirmed["generation"], "compensated": True}
            except (Exception, KeyboardInterrupt) as error:
                result["nodes"][service] = {"restored": False, "error": str(error)}
        result["manual_recovery_required"] = not (result["current_restored"] and result["previous_restored"] and
                                                     all(node["restored"] for node in result["nodes"].values()))
        return result


class JsonArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        raise ToolError(message)


def make_parser():
    parser = JsonArgumentParser(description=__doc__)
    parser.add_argument("--env-file", type=Path, default=ROOT / "deploy" / "scene-host.env")
    parser.add_argument("--compose-file", type=Path, default=ROOT / "deploy" / "scene-host.compose.yaml")
    parser.add_argument("--directory", type=Path, default=DEFAULT_DIRECTORY)
    parser.add_argument("--project-name", default="")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("init", help="创建配置目录，并仅为默认目录填空 COMBAT_CONFIG_FILE")
    commands.add_parser("status", help="读取两个场景的实际配置")
    publish = commands.add_parser("publish", help="校验并发布完整数值配置")
    publish.add_argument("file", type=Path)
    commands.add_parser("rollback", help="将 previous.json 作为一次新发布回切")
    return parser


def main(argv=None):
    args = None
    try:
        args = make_parser().parse_args(argv)
        paths = Paths(args.env_file.absolute(), args.compose_file.absolute(), args.directory.absolute(), args.project_name)
        result = Manager(paths).run(args.command, getattr(args, "file", None))
        output = {"ok": True, "command": args.command, **result}
        code = 0
    except (ToolError, OSError, ValueError, KeyboardInterrupt) as error:
        output = {"ok": False, "command": getattr(args, "command", None), "error": str(error) or "操作被中断。"}
        if isinstance(error, ToolError) and error.recovery is not None:
            output["recovery"] = error.recovery
            output["manual_recovery_required"] = error.recovery["manual_recovery_required"]
        code = 1
    print(json.dumps(output, ensure_ascii=False, allow_nan=False))
    return code


if __name__ == "__main__":
    sys.exit(main())
