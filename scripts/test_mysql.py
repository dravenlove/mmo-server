#!/usr/bin/env python3
"""在本机隔离 MySQL 容器中运行 Backend 测试，不连接实际部署的数据库。"""
import argparse
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', default='mysql:8.0', help='已经拉取到本机的 MySQL 镜像')
    args = parser.parse_args()
    name = 'mmo-mysql-test-' + secrets.token_hex(5)
    work = ROOT / 'output' / name
    work.mkdir(parents=True)
    # 特意包含连接串特殊字符，验证独立字段传递密码不会被误解析。
    password = 'test@:/?&=+$' + secrets.token_hex(20)
    envfile = work / 'mysql.env'
    envfile.write_text(f'MYSQL_ROOT_PASSWORD={password}\nMYSQL_ROOT_HOST=%\n', encoding='utf-8')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]

    def run(command, **kwargs):
        return subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                              encoding='utf-8', errors='replace', **kwargs)

    created = False
    try:
        result = run(['docker', 'run', '-d', '--pull', 'never', '--name', name,
                      '--env-file', str(envfile), '-p', f'127.0.0.1:{port}:3306', args.image,
                      '--max-connections=30', '--performance-schema=OFF',
                      '--innodb-buffer-pool-size=67108864'], timeout=45)
        if result.returncode:
            raise RuntimeError(result.stderr)
        created = True
        print('Started isolated MySQL test container:', name, flush=True)
        deadline = time.monotonic() + 100
        version = ''
        while time.monotonic() < deadline:
            result = run(['docker', 'exec', name, 'sh', '-c',
                          'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql --protocol=TCP -h127.0.0.1 -uroot -Nse "SELECT VERSION()"'], timeout=10)
            if result.returncode == 0:
                version = result.stdout.strip()
                break
            time.sleep(1)
        if not version:
            raise RuntimeError('MySQL startup timed out; see container.log')
        print('MySQL version:', version, flush=True)
        env = dict(os.environ, MMO_TEST_MYSQL_HOST='127.0.0.1', MMO_TEST_MYSQL_PORT=str(port),
                   MMO_TEST_MYSQL_USER='root', MMO_TEST_MYSQL_PASSWORD=password)
        command = ['go', 'test', './internal/backend', '-v', '-count=1', '-timeout', '180s']
        result = run(command, env=env, timeout=210)
        (work / 'go-test.log').write_text(result.stdout + result.stderr, encoding='utf-8')
        print(result.stdout, end='', flush=True)
        if result.returncode:
            print(result.stderr, end='', flush=True)
            raise RuntimeError('MySQL Backend tests failed')
        report = {'ok': True, 'mysql_version': version, 'image': args.image,
                  'passed': sum(line.startswith('--- PASS:') for line in result.stdout.splitlines())}
        (work / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print('PASS MySQL integration tests; report:', work / 'report.json', flush=True)
    finally:
        if created:
            logs = run(['docker', 'logs', name], timeout=15)
            (work / 'container.log').write_text(logs.stdout + logs.stderr, encoding='utf-8')
            # 仅清理本次新建的随机容器及其匿名测试卷。
            result = run(['docker', 'rm', '-f', '-v', name], timeout=30)
            if result.returncode:
                raise RuntimeError('Failed to remove isolated MySQL test container: ' + name)


if __name__ == '__main__':
    main()
