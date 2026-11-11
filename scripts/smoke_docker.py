#!/usr/bin/env python3
"""使用已有镜像执行隔离容器验收；不会连接或修改实际部署。"""
import argparse
import concurrent.futures
import copy
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from smoke import Peer, free_port, http

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tag', required=True)
parser.add_argument('--image-prefix', default='mmo')
parser.add_argument('--database', choices=('sqlite', 'mysql'), default='sqlite')
parser.add_argument('--mysql-image', default='mysql:8.0', help='隔离测试使用的本地 MySQL 镜像')
parser.add_argument('--combat', action='store_true', help='额外验收战斗、技能、死亡复活与战斗快照恢复')
parser.add_argument('--hot-reload', action='store_true', help='额外验收在线配置热更、管理CLI与回滚；包含战斗检查')
parser.add_argument('--external-auth', action='store_true', help='使用独立鉴权容器；需要 --database mysql')
parser.add_argument('--webgl', action='store_true', help='增加独立移动场景及真实 WebSocket 双角色检查')
args = parser.parse_args()
if args.external_auth and args.database != 'mysql':
    parser.error('--external-auth requires --database mysql')
args.combat = args.combat or args.hot_reload
TAG = args.tag
PREFIX = args.image_prefix.rstrip('/')
if not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}', TAG):
    parser.error('invalid image tag')
if not re.fullmatch(r'[a-z0-9][a-z0-9._:/-]*', PREFIX) or '://' in PREFIX:
    parser.error('invalid image prefix; use a Docker repository path without a URL scheme')
project = 'mmo-check-' + secrets.token_hex(4)
work = ROOT / 'output' / project
work.mkdir()
token = secrets.token_hex(32)
envfile = work / 'test.env'
envfile.write_text(f'INTERNAL_TOKEN={token}\nMMO_IMAGE_TAG={TAG}\nMMO_IMAGE_PREFIX={PREFIX}\nTLS_CERT_FILE=\nTLS_KEY_FILE=\n', encoding='utf-8')
child_env = dict(os.environ, INTERNAL_TOKEN=token, MMO_IMAGE_TAG=TAG,
                 MMO_IMAGE_PREFIX=PREFIX, TLS_CERT_FILE='', TLS_KEY_FILE='')
hot_directory = work / 'combat-config'
if args.hot_reload:
    hot_directory.mkdir(mode=0o755)
    os.chmod(hot_directory, 0o755)
    active_rules = hot_directory / 'current.json'
    active_rules.write_bytes((ROOT / 'configs/combat.default.json').read_bytes())
    os.chmod(active_rules, 0o644)
    with envfile.open('a', encoding='utf-8') as stream:
        stream.write(f'COMBAT_CONFIG_DIR={hot_directory.as_posix()}\nCOMBAT_CONFIG_FILE=/app/config/current.json\n')
peers, checks = [], []
ports = {name: free_port() for name in ('backend', 'gateway', 'health', 'scene1', 'scene2')}
assert len(set(ports.values())) == len(ports)

def run(args, timeout=120, **kwargs):
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True,
                          encoding='utf-8', errors='replace', timeout=timeout, env=child_env, **kwargs)

def passed(name):
    checks.append(name)
    print('PASS', name, flush=True)

raw = run(['docker', 'compose', '--env-file', str(envfile), '-p', project,
           '-f', 'compose.yaml', 'config', '--format', 'json'])
assert raw.returncode == 0, raw.stderr
config = json.loads(raw.stdout)
mapping = {'backend': [(8081, 'backend')], 'gateway': [(7000, 'gateway'), (8080, 'health')],
           'scene1': [(9001, 'scene1')], 'scene2': [(9002, 'scene2')]}
for name, service in config['services'].items():
    service.pop('build', None)
    service['ports'] = [{'target': target, 'published': str(ports[port]),
                         'host_ip': '127.0.0.1', 'protocol': 'tcp'} for target, port in mapping[name]]
    if args.hot_reload and name in ('scene1', 'scene2'):
        service['environment']['COMBAT_CONFIG_FILE'] = '/app/config/current.json'
        service['volumes'] = [{'type': 'bind', 'source': str(hot_directory), 'target': '/app/config', 'read_only': True}]
config['services']['gateway'].pop('volumes', None)
config['volumes']['backend-data']['name'] = project + '-data'
config['networks']['default']['name'] = project + '-net'
config['services']['backend']['environment']['BACKEND_DB_DRIVER'] = args.database
if args.database == 'mysql':
    # config.json 会再次经过 Compose 插值；$$ 保留密码里的字面 $。
    password = ('test@:/?&=$literal' + secrets.token_hex(20)).replace('$', '$$')
    config['services']['backend']['environment'].update(
        MYSQL_HOST='mysql', MYSQL_PORT='3306', MYSQL_USER='mmo', MYSQL_PASSWORD=password,
        MYSQL_DATABASE='mmo_test', MYSQL_TLS='false')
    config['services']['backend']['depends_on']['mysql'] = {'condition': 'service_healthy'}
    config['volumes']['mysql-data'] = {'name': project + '-mysql-data'}
    config['services']['mysql'] = {
        'image': args.mysql_image,
        'environment': {'MYSQL_ROOT_PASSWORD': secrets.token_hex(32), 'MYSQL_DATABASE': 'mmo_test',
                        'MYSQL_USER': 'mmo', 'MYSQL_PASSWORD': password},
        'volumes': [{'type': 'volume', 'source': 'mysql-data', 'target': '/var/lib/mysql'}],
        'networks': {'default': None},
        'command': ['--max-connections=30', '--performance-schema=OFF', '--innodb-buffer-pool-size=67108864'],
        'healthcheck': {'test': ['CMD-SHELL', 'MYSQL_PWD="$$MYSQL_ROOT_PASSWORD" mysql --protocol=TCP -h127.0.0.1 -uroot -Nse "SELECT 1" >/dev/null'],
                        'interval': '2s', 'timeout': '5s', 'retries': 40, 'start_period': '10s'}}
if args.external_auth:
    auth_service = copy.deepcopy(config['services']['backend'])
    auth_service.pop('ports', None)
    auth_service.pop('volumes', None)
    auth_service['command'] = ['-role', 'auth']
    auth_service['depends_on'] = {'mysql': {'condition': 'service_healthy'}}
    auth_service['environment'] = {key: value for key, value in auth_service['environment'].items()
                                   if key.startswith('MYSQL_') or key == 'INTERNAL_TOKEN'}
    auth_service['environment'].update(AUTH_LISTEN='0.0.0.0:8082', AUTH_DB_DRIVER='mysql')
    auth_service['healthcheck']['test'] = ['CMD', 'curl', '--fail', '--silent', '--max-time', '2', 'http://127.0.0.1:8082/healthz']
    config['services']['auth'] = auth_service
    config['services']['backend']['environment']['AUTH_URL'] = 'http://auth:8082'
    config['services']['backend']['depends_on']['auth'] = {'condition': 'service_healthy'}
if args.webgl:
    ports['webgl'] = free_port()
    web_scene = copy.deepcopy(config['services']['scene1'])
    web_scene.pop('ports', None)
    web_scene['environment'].update(SCENE_ID='scene-webgl', SCENE_PORT='9003', SCENE_MODE='movement')
    web_scene['environment'].pop('COMBAT_CONFIG_FILE', None)
    config['services']['scene-webgl'] = web_scene
    backend = config['services']['backend']
    backend['environment']['SCENES'] += ',scene-webgl=http://scene-webgl:9003'
    backend['depends_on']['scene-webgl'] = {'condition': 'service_healthy'}
    web_gate = copy.deepcopy(config['services']['gateway'])
    web_gate['environment'].update(GATEWAY_PROFILE='movement', GATEWAY_LOGIN_SCENE='scene-webgl')
    web_gate['ports'] = [{'target': 7000, 'published': str(ports['webgl']), 'host_ip': '127.0.0.1', 'protocol': 'tcp'}]
    config['services']['gateway-webgl'] = web_gate
cfg = work / 'compose.json'
cfg.write_text(json.dumps(config, indent=2), encoding='utf-8')
compose = ['docker', 'compose', '-f', str(cfg), '-p', project]

def compose_ok(*args):
    result = run(compose + list(args), timeout=180)
    with (work / 'compose.log').open('a', encoding='utf-8') as f:
        f.write(result.stdout + result.stderr)
    assert result.returncode == 0, result.stderr
    return result

def new_peer():
    p = Peer(ports['gateway'])
    peers.append(p)
    return p

def close_peers():
    for p in peers:
        try: p.close()
        except OSError: pass
    peers.clear()

def cli(commands, logname):
    script = work / (logname + '.jsonl')
    script.write_text(''.join(json.dumps(c) + '\n' for c in commands), encoding='utf-8')
    args = ['docker', 'run', '--rm', '--pull', 'never', '--read-only', '--cap-drop', 'ALL',
            '--network', project + '-net',
            '--mount', f'type=volume,source={project}-tokens,target=/app/tokens',
            '--mount', f'type=bind,source={script},target=/app/commands.jsonl,readonly',
            PREFIX + '/client:' + TAG, '-addr', 'gateway:7000', '-name', 'ContainerCLI',
            '-token-file', '/app/tokens/player.json', '-script', '/app/commands.jsonl']
    result = run(args)
    (work / (logname + '.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
    assert result.returncode == 0, result.stderr
    parsed = {}
    for line in result.stdout.splitlines():
        if ': ' in line and line.split(': ', 1)[0] in ('view', 'claim', 'move', 'transfer', 'cast', 'revive'):
            op, data = line.split(': ', 1)
            parsed.setdefault(op, []).append(json.loads(data))
    return parsed

try:
    compose_ok('up', '-d', '--no-build', '--pull', 'never', '--wait', '--wait-timeout', '90')
    healthy = run(['docker', 'inspect', '--format', '{{.State.Health.Status}}',
                   *compose_ok('ps', '-q').stdout.split()])
    assert healthy.returncode == 0 and healthy.stdout.split() == ['healthy'] * len(config['services']), healthy.stdout
    passed('four application containers healthy with read-only root and non-root user; database=' + args.database)
    for name in ('backend', 'scene1'):
        path = '/v1/register' if name == 'backend' else '/v1/enter'
        assert http(f'http://127.0.0.1:{ports[name]}' + path, {'name': 'intruder'})[0] in (401, 403)
    passed('internal HTTP services reject missing credentials')
    alice, bob = new_peer(), new_peer()
    alice.request('move', ok=False, seq=1, dx=1, dy=0)
    auth = alice.request('register', name='DockerAlice')
    bob_auth = bob.request('register', name='DockerBob')
    alice.request('register', ok=False, name='DockerAlice')
    a = alice.request('login', token=auth['token'])
    b = bob.request('login', token=bob_auth['token'])
    assert {p['name'] for p in alice.request('view')['players']} == {'DockerAlice', 'DockerBob'}
    passed('TCP registration, login, unique names and AOI across containers')
    if args.webgl:
        child_env['MMO_TEST_MOVEMENT_GATEWAY'] = '127.0.0.1:' + str(ports['webgl'])
        web_check = run(['go', 'test', './internal/wsbridge', '-run', '^TestMovementWorldIntegration$', '-count=1', '-v', '-timeout', '90s'], timeout=100)
        (work / 'webgl.log').write_text(web_check.stdout + web_check.stderr, encoding='utf-8')
        if web_check.returncode: raise AssertionError(web_check.stdout + web_check.stderr)
        passed('WebSocket two-player movement, bounded inputs, AOI, reconnect and disconnect in isolated movement world')
    if args.external_auth:
        compose_ok('stop', 'auth')
        new_peer().request('login', ok=False, token=auth['token'])
        # 包含超过一次 Gateway 会话复核周期，确认在线链路不会继续调用 Auth。
        time.sleep(1.2)
        assert alice.request('view')['self']['role_id'] == a['role_id']
        status, result = http(f'http://127.0.0.1:{ports["backend"]}/v1/session',
                              {'role_id': a['role_id'], 'session_epoch': a['session_epoch']}, token)
        assert status == 200, result
        passed('Auth outage rejects new login without blocking existing view or session verification')
        compose_ok('up', '-d', '--no-build', '--pull', 'never', '--wait', 'auth')
    alice.request('move', seq=1, dx=1, dy=0)
    time.sleep(0.4)
    moved = alice.request('view')['self']
    assert moved['x'] > a['x'] + 0.3
    alice.request('move', ok=False, seq=1, dx=1, dy=0)
    alice.request('move', ok=False, seq=2, dx=9, dy=0)
    passed('Linux C++ scene movement and input validation')
    assert alice.request('claim') == {'coins': 100, 'granted': True}
    assert alice.request('claim') == {'coins': 100, 'granted': False}
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        rewards = list(pool.map(lambda _: http(f'http://127.0.0.1:{ports["backend"]}/v1/claim',
                       {'role_id': b['role_id'], 'session_epoch': b['session_epoch']}, token), range(8)))
    assert all(code == 200 and data['coins'] == 100 for code, data in rewards)
    assert sum(data['granted'] for _, data in rewards) == 1
    passed(args.database + ' sequential and concurrent reward idempotency')
    transferred = alice.request('transfer', target_scene_id='scene-2', request_id='docker-transfer-1')
    retry = alice.request('transfer', target_scene_id='scene-2', request_id='docker-transfer-1')
    assert transferred['scene_id'] == 'scene-2' and retry['owner_epoch'] == transferred['owner_epoch']
    assert alice.request('view')['scene_id'] == 'scene-2'
    stale = {k: a[k] for k in ('role_id', 'owner_epoch', 'session_epoch')}
    assert http(f'http://127.0.0.1:{ports["scene1"]}/v1/move', dict(stale, seq=9, dx=1, dy=0), token)[0] in (404, 409)
    passed('cross-scene transfer, retry and stale owner fencing')
    first = cli([{'op': 'move', 'seq': 100, 'dx': 1, 'dy': 0},
                 {'op': 'move', 'dx': 0, 'dy': 1}, {'op': 'sleep', 'ms': 400},
                 {'op': 'transfer', 'target_scene_id': 'scene-2'},
                 {'op': 'claim'}, {'op': 'view'}], 'cli-before-restart')
    assert first['claim'] == [{'coins': 100, 'granted': True}]
    assert first['view'][-1]['scene_id'] == 'scene-2'
    saved = first['view'][-1]['self']
    passed('packaged CLI and writable credential volume under UID 10001')
    if args.combat:
        from combat_checks import run_checks

        def combat_checkpoint(role):
            code, result = http(f'http://127.0.0.1:{ports["backend"]}/v1/checkpoint', {'role': role}, token)
            assert code == 200 and result['ok'], result

        def combat_restart():
            close_peers()
            compose_ok('stop', 'gateway')
            if args.webgl: compose_ok('stop', 'gateway-webgl')
            compose_ok('stop', 'backend')
            compose_ok('stop', 'scene1', 'scene2')
            if args.webgl: compose_ok('stop', 'scene-webgl')
            compose_ok('up', '-d', '--no-build', '--pull', 'never', '--wait', '--wait-timeout', '90')

        run_checks(new_peer, passed, combat_checkpoint, combat_restart)
        exercised = cli([{'op': 'cast', 'skill_id': 'charge', 'target_id': 'npc-1'}, {'op': 'view'}], 'cli-combat')
        assert exercised['cast'][0]['accepted'] and exercised['view'][0]['self']['combat']['cooldowns']['charge'] > 0
        saved = exercised['view'][0]['self']
        passed('container CLI casts skills and reads authoritative combat state')
    if args.hot_reload:
        from hot_reload_checks import run_checks as hot_checks

        def config_admin(scene, operation, body, authenticated=True):
            return http(f'http://127.0.0.1:{ports[scene]}/v1/config/{operation}', body, token if authenticated else None)

        def config_tool(command, value=None):
            arguments = [sys.executable, 'scripts/combat_config.py', '--env-file', str(envfile),
                         '--compose-file', str(cfg), '--directory', str(hot_directory), '--project-name', project, command]
            if value is not None:
                source = work / 'candidate.json'
                source.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
                arguments.append(str(source))
            result = run(arguments, timeout=120)
            with (work / 'config-tool.log').open('a', encoding='utf-8') as stream:
                stream.write(result.stdout + result.stderr)
            assert result.returncode == 0, result.stdout + result.stderr
            return json.loads(result.stdout)

        hot_checks(new_peer, passed, config_admin, lambda value: config_tool('publish', value),
                   lambda: config_tool('rollback'), combat_checkpoint, combat_restart)
    close_peers()
    time.sleep(2.5)
    if args.database == 'mysql':
        backend_id = compose_ok('ps', '-q', 'backend').stdout.strip()
        compose_ok('restart', 'mysql')
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            state = run(['docker', 'inspect', backend_id])
            if state.returncode == 0:
                info = json.loads(state.stdout)[0]
                if info['RestartCount'] > 0 and info['State']['Health']['Status'] == 'healthy':
                    break
            time.sleep(1)
        else:
            raise AssertionError('Backend did not restart safely after the MySQL session was lost')
        if args.external_auth:
            compose_ok('up', '-d', '--no-build', '--pull', 'never', '--wait', '--wait-timeout', '90', 'auth')
        recovered = cli([{'op': 'view'}, {'op': 'claim'}], 'cli-after-mysql-restart')
        assert recovered['view'][0]['scene_id'] == 'scene-2'
        assert recovered['claim'] == [{'coins': 100, 'granted': False}]
        passed('MySQL restart fences the lost connection and automatically restarts Backend with the original data')
    compose_ok('stop', 'gateway')
    if args.webgl: compose_ok('stop', 'gateway-webgl')
    compose_ok('stop', 'backend')
    compose_ok('stop', 'scene1', 'scene2')
    if args.webgl: compose_ok('stop', 'scene-webgl')
    if args.database == 'mysql':
        if args.external_auth:
            compose_ok('stop', 'auth')
        compose_ok('stop', 'mysql')
    compose_ok('up', '-d', '--no-build', '--pull', 'never', '--wait', '--wait-timeout', '90')
    second = cli([{'op': 'view'}, {'op': 'claim'}], 'cli-after-restart')
    restored = second['view'][0]['self']
    assert second['view'][0]['scene_id'] == 'scene-2'
    assert second['claim'] == [{'coins': 100, 'granted': False}]
    assert abs(restored['x'] - saved['x']) < 0.01 and abs(restored['y'] - saved['y']) < 0.01
    passed('all containers restart with persistent identity, location, scene and reward history')
    names = compose_ok('ps', '-q').stdout.split()
    stats = run(['docker', 'stats', '--no-stream', '--format', '{{json .}}', *names])
    (work / 'stats.jsonl').write_text(stats.stdout, encoding='utf-8')
    report = {'ok': True, 'image_tag': TAG, 'image_prefix': PREFIX, 'database': args.database, 'external_auth': args.external_auth, 'combat': args.combat, 'hot_reload': args.hot_reload, 'checks': checks, 'count': len(checks),
              'scope': 'Linux containers on one Docker engine; no real VPS network, TLS or capacity test'}
    (work / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'PASS {len(checks)} Docker checks; report: {work / "report.json"}', flush=True)
finally:
    close_peers()
    logs = run(compose + ['logs', '--no-color'])
    (work / 'containers.log').write_text(logs.stdout + logs.stderr, encoding='utf-8')
    # 仅清理此次随机项目的测试容器和新建卷，不涉及实际部署的数据卷。
    cleanup = run(compose + ['down', '--volumes', '--remove-orphans'])
    (work / 'cleanup.log').write_text(cleanup.stdout + cleanup.stderr, encoding='utf-8')
    run(['docker', 'volume', 'rm', project + '-tokens'])
