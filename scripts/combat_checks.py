"""真实网关协议上的战斗验收；仅由隔离测试 fixture 调用。"""
import time
import uuid


def wait_for(peer, predicate, timeout=8):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = peer.request('view')
        if predicate(last):
            return last
        time.sleep(.15)
    raise AssertionError('combat condition timed out: ' + str(last))


def mob(view, ident='npc-1'):
    return next(item for item in view['mobs'] if item['id'] == ident)


def run_checks(new_peer, passed, checkpoint=None, restart_all=None):
    hero = new_peer()
    auth = hero.request('register', name='combat-' + uuid.uuid4().hex[:12])
    initial = hero.request('login', token=auth['token'])
    view = hero.request('view')
    state = view['self']['combat']
    assert state['hp'] == state['max_hp'] == 180
    assert state['mp'] == state['max_mp'] == 100
    assert {skill['id'] for skill in view['skills']} == {'strike', 'whirlwind', 'charge', 'mend'}
    assert len(view['mobs']) >= 3 and mob(view)['alive']
    passed('combat catalog, initial HP/MP and authoritative PvE mobs')

    hero.request('cast', ok=False, seq=1, skill_id='invented', target_id='npc-1')
    hero.request('cast', ok=False, seq=1, skill_id='strike', target_id='npc-4')
    hero.request('cast', ok=False, seq=1, skill_id='charge', target_id='missing-target')
    clean = hero.request('view')
    assert clean['self']['combat']['mp'] == 100 and mob(clean)['hp'] == 80
    passed('invalid skills, targets and range do not apply damage or consume mana')

    charge = hero.request('cast', seq=1, skill_id='charge', target_id='npc-1')
    duplicate = hero.request('cast', seq=1, skill_id='charge', target_id='npc-1')
    assert charge == duplicate
    after_charge = hero.request('view')
    assert mob(after_charge)['hp'] == 56
    assert after_charge['self']['combat']['cooldowns']['charge'] > 0
    hero.request('cast', ok=False, seq=2, skill_id='charge', target_id='npc-1')
    hero.request('cast', ok=False, seq=1, skill_id='mend', target_id='')
    assert mob(hero.request('view'))['hp'] == 56
    passed('server cooldown and duplicate action fencing prevent repeated damage')

    hero.request('cast', seq=2, skill_id='strike', target_id='npc-1')
    hero.request('cast', seq=2, skill_id='strike', target_id='npc-1')
    hero.request('cast', ok=False, seq=3, skill_id='strike', target_id='npc-1')
    hero.request('cast', seq=3, skill_id='whirlwind', target_id='')
    assert mob(hero.request('view'))['hp'] == 6
    wounded = wait_for(hero, lambda item: item['self']['combat']['hp'] < 180, timeout=7)
    healed = hero.request('cast', seq=4, skill_id='mend', target_id='')
    assert healed['healed'] > 0
    assert hero.request('view')['self']['combat']['hp'] > wounded['self']['combat']['hp']
    hero.request('cast', seq=5, skill_id='strike', target_id='npc-1')
    corpse = hero.request('view')
    assert mob(corpse)['hp'] == 0 and not mob(corpse)['alive']
    assert any(event['type'] == 'damage' for event in corpse['events'])
    hero.request('cast', ok=False, seq=6, skill_id='strike', target_id='npc-1')
    passed('NPC counterattack, healing, AoE, hit events and monster death')

    wait_for(hero, lambda item: mob(item)['alive'] and mob(item)['hp'] == 80, timeout=11)
    passed('dead monsters respawn through the scene tick')

    # 刷新一个仍在冷却中的快照，确认重连和迁移不能刷新技能或法力。
    wait_for(hero, lambda item: item['self']['combat']['cooldowns']['mend'] == 0, timeout=15)
    hero.request('cast', seq=6, skill_id='strike', target_id='npc-1')
    wait_for(hero, lambda item: item['self']['combat']['hp'] < 180, timeout=7)
    hero.request('cast', seq=7, skill_id='mend', target_id='')
    before = hero.request('view')['self']
    assert before['combat']['cooldowns']['mend'] > 0
    hero.close()
    hero = new_peer()
    relogged = hero.request('login', token=auth['token'])
    resumed = hero.request('view')['self']
    assert relogged['session_epoch'] > initial['session_epoch']
    assert resumed['combat']['cooldowns']['mend'] > 0
    assert resumed['combat']['mp'] < 100
    passed('relogin renews the session while preserving live HP, mana and cooldown')

    transfer = 'combat-transfer-' + uuid.uuid4().hex
    moved = hero.request('transfer', target_scene_id='scene-2', request_id=transfer)
    retry = hero.request('transfer', target_scene_id='scene-2', request_id=transfer)
    assert moved['owner_epoch'] == retry['owner_epoch']
    target = hero.request('view')['self']
    assert target['scene_id'] == 'scene-2'
    assert target['combat']['cooldowns']['mend'] > 0 and target['combat']['mp'] < 100
    passed('cross-scene commit and retry retain combat state without cooldown refresh')

    if checkpoint is not None and restart_all is not None:
        checkpoint(target)
        hero.close()
        restart_all()
        hero = new_peer()
        restored = hero.request('login', token=auth['token'])
        restored = hero.request('view')['self']
        assert restored['scene_id'] == 'scene-2'
        assert restored['combat']['cooldowns']['mend'] > 0
        assert restored['combat']['mp'] < 100
        passed('checkpoint and all-service restart restore persisted combat state')

    # 新角色挑战精英，走完整死亡、拒绝行动、等待与复活流程。
    mortal = new_peer()
    mortal_auth = mortal.request('register', name='mortal-' + uuid.uuid4().hex[:12])
    mortal.request('login', token=mortal_auth['token'])
    seq = 20
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        mortal.request('move', seq=seq, dx=1, dy=1)
        seq += 1
        time.sleep(.1)
    mortal.request('move', seq=seq, dx=0, dy=0)
    mortal.request('cast', seq=seq + 1, skill_id='charge', target_id='npc-4')
    dead = wait_for(mortal, lambda item: item['self']['combat']['dead'], timeout=36)
    assert dead['self']['combat']['hp'] == 0
    mortal.request('move', ok=False, seq=seq + 2, dx=1, dy=0)
    mortal.request('cast', ok=False, seq=seq + 3, skill_id='mend', target_id='')
    mortal.request('revive', ok=False, seq=seq + 4)
    wait_for(mortal, lambda item: item['self']['combat']['revive_remaining_ms'] == 0, timeout=8)
    result = mortal.request('revive', seq=seq + 5)
    duplicate = mortal.request('revive', seq=seq + 5)
    assert result == duplicate
    alive = mortal.request('view')['self']
    assert not alive['combat']['dead'] and alive['combat']['hp'] == 180
    assert alive['combat']['mp'] == 100 and abs(alive['x']) < .001 and abs(alive['y']) < .001
    passed('death fences movement/casts and timed authoritative revival is idempotent')
    mortal.close()
    hero.close()
