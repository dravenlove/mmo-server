#!/usr/bin/env python3
"""启动隔离的原生战斗场景/Backend/Gateway，用真实 WebSocket 验证技能。"""
import json, os, secrets, subprocess, time
from pathlib import Path
from smoke import ROOT, free_port, http

def main():
    work=ROOT/'output'/('skill-lab-smoke-'+time.strftime('%Y%m%d-%H%M%S'));work.mkdir(parents=True)
    ports={key:free_port() for key in ('scene','backend','gateway','health')}
    assert len(set(ports.values()))==4
    common=dict(os.environ,INTERNAL_TOKEN=secrets.token_hex(32))
    configs={
        'scene-server':dict(common,SCENE_HOST='127.0.0.1',SCENE_PORT=str(ports['scene']),SCENE_ID='scene-1',SCENE_MODE='skill_lab'),
        'backend':dict(common,BACKEND_LISTEN=f"127.0.0.1:{ports['backend']}",BACKEND_DB_DRIVER='sqlite',BACKEND_DB=str(work/'mmo.db'),SCENES=f"scene-1=http://127.0.0.1:{ports['scene']}",AUTH_URL=''),
        'gateway':dict(common,GATEWAY_LISTEN=f"127.0.0.1:{ports['gateway']}",GATEWAY_HEALTH_LISTEN=f"127.0.0.1:{ports['health']}",BACKEND_URL=f"http://127.0.0.1:{ports['backend']}",GATEWAY_PROFILE='skill_lab',GATEWAY_LOGIN_SCENE='scene-1')}
    children=[];logs=[]
    try:
        for name,env in configs.items():
            path=ROOT/'bin'/(name+('.exe' if os.name=='nt' else ''))
            log=(work/(name+'.log')).open('wb');logs.append(log)
            child=subprocess.Popen([str(path)],env=env,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0);children.append(child)
            health=ports['scene' if name=='scene-server' else 'health' if name=='gateway' else 'backend']
            for attempt in range(100):
                if child.poll() is not None:raise RuntimeError(name+' exited; see isolated log')
                try:
                    assert http(f'http://127.0.0.1:{health}/healthz')[0]==200
                    break
                except Exception:time.sleep(.1)
            else:raise RuntimeError(name+' health timed out')
        env=dict(os.environ,MMO_TEST_SKILL_LAB_GATEWAY=f"127.0.0.1:{ports['gateway']}")
        result=subprocess.run(['go','test','./internal/wsbridge','-run','^TestSkillLabIntegration$','-count=1','-v'],env=env,cwd=ROOT,text=True,capture_output=True,timeout=90)
        (work/'protocol.log').write_text(result.stdout+result.stderr,encoding='utf-8')
        print(result.stdout)
        report={'ok':result.returncode==0,'scope':'isolated native processes and real WebSocket protocol; no Unity build','report_directory':str(work)}
        (work/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report))
        if result.returncode:raise RuntimeError('skill lab protocol failed; see isolated log')
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
                try:child.wait(timeout=8)
                except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)
        for log in logs:log.close()

if __name__=='__main__':main()
