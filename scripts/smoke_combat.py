#!/usr/bin/env python3
"""在独立本机四进程中验收战斗，不接触真实部署和数据库。"""
import argparse
import json
import os
from pathlib import Path
import secrets
import subprocess
import time
import urllib.error
import urllib.request

from smoke import Peer, free_port
from combat_checks import run_checks

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, default=ROOT / 'bin')
    args = parser.parse_args()
    suffix = '.exe' if os.name == 'nt' else ''
    binaries = {name: args.bin_dir.resolve() / (name + suffix) for name in ('backend', 'gateway', 'scene-server')}
    for file in binaries.values():
        if not file.is_file():
            raise SystemExit('先构建服务端，缺少 ' + str(file))
    work = ROOT / 'output' / ('combat-check-' + secrets.token_hex(5))
    work.mkdir(parents=True)
    ports = {name: free_port() for name in ('backend', 'gateway', 'health', 'scene-1', 'scene-2')}
    assert len(set(ports.values())) == len(ports)
    key = secrets.token_hex(32)
    env = dict(os.environ, INTERNAL_TOKEN=key, BACKEND_DB_DRIVER='sqlite', TLS_CERT_FILE='', TLS_KEY_FILE='')
    for field in tuple(env):
        if field.startswith('MYSQL_'):
            env.pop(field)
    configs = {
        'scene-1': dict(env, SCENE_ID='scene-1', SCENE_HOST='127.0.0.1', SCENE_PORT=str(ports['scene-1'])),
        'scene-2': dict(env, SCENE_ID='scene-2', SCENE_HOST='127.0.0.1', SCENE_PORT=str(ports['scene-2'])),
        'backend': dict(env, BACKEND_LISTEN=f'127.0.0.1:{ports["backend"]}', BACKEND_DB=str(work / 'combat.db'),
                        SCENES=','.join(f'{name}=http://127.0.0.1:{ports[name]}' for name in ('scene-1', 'scene-2'))),
        'gateway': dict(env, GATEWAY_LISTEN=f'127.0.0.1:{ports["gateway"]}',
                        GATEWAY_HEALTH_LISTEN=f'127.0.0.1:{ports["health"]}', BACKEND_URL=f'http://127.0.0.1:{ports["backend"]}')
    }
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    processes, logs, peers, checks = {}, [], [], []

    def http(url, body=None):
        headers = {'X-Internal-Token': key, 'Content-Type': 'application/json'}
        request = urllib.request.Request(url, None if body is None else json.dumps(body).encode(), headers)
        with opener.open(request, timeout=25) as response:
            return json.load(response)

    def start(name):
        log = (work / (name + '.log')).open('ab')
        logs.append(log)
        binary = binaries['scene-server' if name.startswith('scene-') else name]
        processes[name] = subprocess.Popen([str(binary)], cwd=ROOT, env=configs[name], stdout=log,
                                           stderr=subprocess.STDOUT,
                                           creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        endpoint = 'health' if name == 'gateway' else name
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            if processes[name].poll() is not None:
                raise RuntimeError(name + ' 启动失败，请查看本次日志。')
            try:
                if http(f'http://127.0.0.1:{ports[endpoint]}/healthz')['ok']:
                    return
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(.1)
        raise TimeoutError(name + ' 健康检查超时。')

    def stop(name):
        process = processes.get(name)
        if process is None or process.poll() is not None:
            return
        process.terminate()
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)

    def peer():
        value = Peer(ports['gateway'])
        peers.append(value)
        return value

    def passed(name):
        checks.append(name)
        print('PASS', name, flush=True)

    def checkpoint(role):
        assert http(f'http://127.0.0.1:{ports["backend"]}/v1/checkpoint', {'role': role})['ok']

    def restart():
        for name in ('gateway', 'backend', 'scene-1', 'scene-2'):
            stop(name)
        for name in ('scene-1', 'scene-2', 'backend', 'gateway'):
            start(name)

    error = None
    try:
        for name in ('scene-1', 'scene-2', 'backend', 'gateway'):
            start(name)
        run_checks(peer, passed, checkpoint, restart)
    except Exception as exc:
        error = str(exc)
        print('FAIL', error, flush=True)
    finally:
        for value in peers:
            try:
                value.close()
            except OSError:
                pass
        for name in ('gateway', 'backend', 'scene-1', 'scene-2'):
            stop(name)
        for log in logs:
            log.close()
        report = {'ok': error is None, 'checks': checks, 'count': len(checks), 'error': error,
                  'external_services_contacted': False, 'services_stopped': all(p.poll() is not None for p in processes.values())}
        (work / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        print('Report:', work / 'report.json', flush=True)
    if error is not None:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
