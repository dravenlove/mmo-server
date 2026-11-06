"""在真实网关连接中验收配置热更；仅由隔离的测试环境调用。"""
import copy
import time
import uuid

from combat_checks import mob, wait_for


def run_checks(new_peer, passed, admin, publish, rollback, checkpoint=None, restart_all=None):
    # admin 返回 (HTTP状态, JSON)，publish/rollback 必须更新固定启动文件。
    statuses = {}
    for scene in ('scene1', 'scene2'):
        assert admin(scene, 'status', {}, False)[0] == 401
        code, statuses[scene] = admin(scene, 'status', {})
        assert code == 200 and statuses[scene]['version'] == 'default-v1', statuses[scene]
    baseline = statuses['scene1']['config']
    assert baseline == statuses['scene2']['config']
    passed('config management requires internal authentication and reports both active versions')

    invalid = copy.deepcopy(baseline)
    invalid['version'] = 'invalid-negative'
    invalid['skills']['strike']['damage'] = -1
    for scene in statuses:
        old = statuses[scene]
        assert admin(scene, 'validate', {'config': invalid})[0] == 400
        assert admin(scene, 'reload', {'expected_generation': old['generation'] + 1})[0] == 409
        code, unchanged = admin(scene, 'status', {})
        assert code == 200 and unchanged == old, unchanged
    passed('invalid config and stale-generation reload leave active rules unchanged')

    # 使用长旧CD，避免把测试成功建立在机器必须足够快的假设上。
    long_rules = copy.deepcopy(baseline)
    long_rules['version'] = 'test-old-600s'
    long_rules['skills']['mend']['cooldown_ms'] = 600000
    publish(long_rules)
    hero = new_peer()
    auth = hero.request('register', name='reload-' + uuid.uuid4().hex[:12])
    role = hero.request('login', token=auth['token'])
    wait_for(hero, lambda v: mob(v)['alive'] and mob(v)['hp'] >= 60, timeout=12)
    hero.request('cast', seq=1, skill_id='charge', target_id='npc-1')
    original_mend = hero.request('cast', seq=2, skill_id='mend', target_id='')
    before = hero.request('view')
    assert before['self']['combat']['cooldowns']['mend'] > 500000

    demo = copy.deepcopy(baseline)
    demo['version'] = 'demo-v2'
    demo['skills']['strike']['damage'] = 36
    demo['skills']['mend'].update(cooldown_ms=5000, heal=60, mp_cost=20)
    demo['monsters']['wolf'].update(max_hp=120, damage=4, respawn_ms=3000)
    publish(demo)
    live = hero.request('view')
    assert live['config_version'] == 'demo-v2' and live['config_generation'] > before['config_generation']
    assert live['self']['role_id'] == role['role_id'] and live['self']['session_epoch'] == role['session_epoch']
    assert live['self']['combat']['cooldowns']['mend'] > 5000
    assert live['self']['combat']['cooldowns']['mend'] <= before['self']['combat']['cooldowns']['mend']
    assert live['self']['combat']['hp'] <= before['self']['combat']['hp']
    assert next(s for s in live['skills'] if s['id'] == 'mend')['cooldown_ms'] == 5000
    assert hero.request('cast', seq=2, skill_id='mend', target_id='') == original_mend
    passed('same TCP session sees new rules while existing cooldown and cached action remain intact')

    # 同版本同内容重试不增加代次；同版本换内容必须被拒绝。
    _, active = admin('scene1', 'status', {})
    _, repeated = admin('scene1', 'reload', {'expected_generation': active['generation']})
    assert not repeated['changed'] and repeated['generation'] == active['generation']
    conflict = copy.deepcopy(demo)
    conflict['skills']['strike']['damage'] = 37
    assert admin('scene1', 'validate', {'config': conflict})[0] == 409
    strike = hero.request('cast', seq=3, skill_id='strike', target_id='npc-1')
    assert strike['config_version'] == 'demo-v2' and strike['hits'][0]['damage'] == 36, strike
    assert hero.request('cast', seq=3, skill_id='strike', target_id='npc-1') == strike
    passed('new casts use new damage; reload is idempotent and conflicting version reuse is rejected')

    # 只攻击这一只怪物，避免依赖之前基础业务用例留下的其他怪物状态。
    seq = 3
    deadline = time.monotonic() + 12
    while mob(hero.request('view'))['alive']:
        assert time.monotonic() < deadline, 'monster was not defeated'
        time.sleep(.65)
        seq += 1
        hero.request('cast', seq=seq, skill_id='strike', target_id='npc-1')
    refreshed = wait_for(hero, lambda v: mob(v)['alive'], timeout=12)
    assert mob(refreshed)['max_hp'] == 120 and mob(refreshed)['config_version'] == 'demo-v2'
    passed('monster respawn adopts the new rule version and maximum HP')

    hero.close()
    hero = new_peer()
    hero.request('login', token=auth['token'])
    assert hero.request('view')['self']['combat']['cooldowns']['mend'] > 5000
    hero.request('transfer', target_scene_id='scene-2', request_id='reload-' + uuid.uuid4().hex)
    migrated = hero.request('view')
    assert migrated['config_version'] == 'demo-v2'
    assert migrated['self']['combat']['cooldowns']['mend'] > 5000
    passed('relogin and cross-scene transfer accept remaining cooldown longer than the new skill duration')

    if checkpoint is not None and restart_all is not None:
        checkpoint(migrated['self'])
        hero.close()
        restart_all()
        hero = new_peer()
        hero.request('login', token=auth['token'])
        restored = hero.request('view')
        assert restored['config_version'] == 'demo-v2'
        assert restored['self']['combat']['cooldowns']['mend'] > 0
        passed('service restart reloads the published file and restores compatible player combat state')

    previous_cd = hero.request('view')['self']['combat']['cooldowns']['mend']
    rollback()
    reverted = hero.request('view')
    assert reverted['config_version'] == 'test-old-600s'
    assert next(s for s in reverted['skills'] if s['id'] == 'strike')['damage'] == 18
    assert reverted['self']['combat']['cooldowns']['mend'] <= previous_cd
    publish(baseline)
    assert hero.request('view')['config_version'] == 'default-v1'
    passed('rollback restores previous rules without resetting cooldown; test restores the original config')
