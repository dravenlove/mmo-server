#!/usr/bin/env python3
"""隔离启动四个本机服务，验收桌面客户端协议核心；不连接真实部署。"""
import argparse
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", type=Path, default=ROOT / "bin")
    parser.add_argument("--configuration", default="Release", choices=("Release", "Debug"))
    parser.add_argument("--keep-running", action="store_true", help="验收通过后保留隔离服务供 GUI 联调，回车后清理")
    args = parser.parse_args()
    if os.name != "nt":
        raise SystemExit("桌面客户端的 DPAPI 验收须在 Windows 运行。")
    suffix = ".exe"
    binaries = {name: args.bin_dir.resolve() / (name + suffix)
                for name in ("backend", "gateway", "scene-server")}
    for path in binaries.values():
        if not path.is_file():
            raise SystemExit(f"缺少本地服务 {path}，先运行 scripts/build.ps1。")
    work = ROOT / "output" / ("desktop-check-" + time.strftime("%Y%m%d-%H%M%S") + "-" + secrets.token_hex(3))
    work.mkdir(parents=True)
    listeners = []
    ports = {}
    for name in ("backend", "gateway", "health", "scene-1", "scene-2"):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        ports[name] = listener.getsockname()[1]
        listeners.append(listener)
    for listener in listeners:
        listener.close()

    common = dict(os.environ, INTERNAL_TOKEN=secrets.token_hex(32))
    # 明确覆盖运行模式，避免继承开发机上的任何真实数据库或 TLS 配置。
    common.update(BACKEND_DB_DRIVER="sqlite", TLS_CERT_FILE="", TLS_KEY_FILE="", COMBAT_CONFIG_FILE="")
    for name in tuple(common):
        if name.startswith("MYSQL_"):
            common.pop(name)
    urls = {name: f"http://127.0.0.1:{port}" for name, port in ports.items()}
    configs = {
        "scene-1": dict(common, SCENE_ID="scene-1", SCENE_HOST="127.0.0.1", SCENE_PORT=str(ports["scene-1"])),
        "scene-2": dict(common, SCENE_ID="scene-2", SCENE_HOST="127.0.0.1", SCENE_PORT=str(ports["scene-2"])),
        "backend": dict(common, BACKEND_LISTEN=f"127.0.0.1:{ports['backend']}",
                        BACKEND_DB=str(work / "test.db"),
                        SCENES=",".join(f"{name}={urls[name]}" for name in ("scene-1", "scene-2"))),
        "gateway": dict(common, GATEWAY_LISTEN=f"127.0.0.1:{ports['gateway']}",
                        GATEWAY_HEALTH_LISTEN=f"127.0.0.1:{ports['health']}", BACKEND_URL=urls["backend"]),
    }
    processes = {}
    logs = []
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    report = work / "report.json"
    failure = None
    exit_code = 1
    try:
        env = dict(os.environ, DOTNET_NOLOGO="1", DOTNET_CLI_TELEMETRY_OPTOUT="1")
        project = ROOT / "desktop/MMO.Client.Checks/MMO.Client.Checks.csproj"
        build = subprocess.run(["dotnet", "build", str(project), "--configuration", args.configuration,
                                "--nologo", "--verbosity", "minimal", "--tl:off"],
                               cwd=ROOT, env=env, timeout=90)
        if build.returncode:
            raise RuntimeError("桌面检查工程编译失败。")
        for name in ("scene-1", "scene-2", "backend", "gateway"):
            log = (work / (name + ".log")).open("wb")
            logs.append(log)
            binary = binaries["scene-server" if name.startswith("scene-") else name]
            processes[name] = subprocess.Popen([str(binary)], cwd=ROOT, env=configs[name],
                                               stdout=log, stderr=subprocess.STDOUT,
                                               creationflags=subprocess.CREATE_NO_WINDOW)
            deadline = time.monotonic() + 20
            health = urls["health" if name == "gateway" else name] + "/healthz"
            while time.monotonic() < deadline:
                if processes[name].poll() is not None:
                    raise RuntimeError(f"{name} 启动失败，查看本次独立目录中的日志。")
                try:
                    with opener.open(health, timeout=1) as response:
                        if response.status == 200:
                            break
                except (OSError, urllib.error.URLError):
                    pass
                time.sleep(.1)
            else:
                raise RuntimeError(f"{name} 健康检查超时。")
        runner = project.parent / "bin" / args.configuration / "net10.0-windows/MMO.Client.Checks.dll"
        command = ["dotnet", str(runner), "--addr", f"127.0.0.1:{ports['gateway']}",
                   "--work", str(work), "--report", str(report)]
        completed = subprocess.run(command, cwd=ROOT, env=env, timeout=150)
        exit_code = completed.returncode
        if args.keep_running and exit_code == 0:
            fixture = {"address": f"127.0.0.1:{ports['gateway']}", "ports": ports,
                       "process_ids": {name: process.pid for name, process in processes.items()},
                       "external_services_contacted": False}
            (work / "fixture.json").write_text(json.dumps(fixture, indent=2) + "\n", encoding="utf-8")
            print("Fixture:", work / "fixture.json", flush=True)
            print("GUI endpoint:", fixture["address"], flush=True)
            input("隔离服务保留运行；按回车停止并清理本次进程。\n")
    except Exception as error:
        failure = str(error)
        exit_code = 1
        print("FAIL", failure, flush=True)
    finally:
        # 仅停止本次脚本新建的进程，保留测试报告与独立数据库供排查。
        for process in reversed(list(processes.values())):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        for log in logs:
            log.close()
        result = json.loads(report.read_text(encoding="utf-8")) if report.is_file() else {
            "success": False, "checks": [], "errors": []}
        if failure:
            result["errors"].append(failure)
            result["success"] = False
        result["fixture"] = {"ports": ports, "services_stopped": all(p.poll() is not None for p in processes.values()),
                             "database": "isolated SQLite", "external_services_contacted": False}
        report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print("Report:", report, flush=True)
    raise SystemExit(exit_code)


if __name__ == "__main__":
    main()
