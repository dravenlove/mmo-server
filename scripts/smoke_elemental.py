#!/usr/bin/env python3
"""隔离原生试炼场：真实 WebSocket、归属切换和协议快照，不访问线上。"""
import json,os,secrets,subprocess,time,threading,urllib.request,urllib.error
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from smoke import ROOT,free_port,http,Peer

def main():
    work=ROOT/'output'/('elemental-smoke-'+time.strftime('%Y%m%d-%H%M%S'));work.mkdir()
    ports={key:free_port() for key in ('scene','backend','gateway','health','proxy')};assert len(set(ports.values()))==5
    common=dict(os.environ,INTERNAL_TOKEN=secrets.token_hex(32),TLS_CERT_FILE='',TLS_KEY_FILE='',COMBAT_CONFIG_FILE='')
    scene=f"http://127.0.0.1:{ports['proxy']}"
    faults={};fault_lock=threading.Lock()
    class Proxy(BaseHTTPRequestHandler):
        def do_POST(self):
            data=self.rfile.read(int(self.headers['Content-Length']))
            req=urllib.request.Request(f"http://127.0.0.1:{ports['scene']}"+self.path,data=data,headers={'Content-Type':'application/json','X-Internal-Token':common['INTERNAL_TOKEN']})
            try:
                with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(req,timeout=5) as response:status,body=response.status,response.read()
            except urllib.error.HTTPError as e:status,body=e.code,e.read()
            except (urllib.error.URLError,TimeoutError,ConnectionError):status,body=502,b'{"error":"fixture upstream unavailable"}'
            with fault_lock:
                if status==200 and faults.get(self.path,0)>0:
                    faults[self.path]-=1;status=503;body=b'{"error":"fixture response dropped after execution"}'
            try:
                self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
            except (BrokenPipeError,ConnectionError):pass
        def log_message(self,*args):pass
    proxy=ThreadingHTTPServer(('127.0.0.1',ports['proxy']),Proxy);threading.Thread(target=proxy.serve_forever,daemon=True).start()
    configs={
        'scene-server':dict(common,SCENE_HOST='127.0.0.1',SCENE_PORT=str(ports['scene']),SCENE_ID='scene-elemental-world',SCENE_MODE='elemental',ELEMENTAL_ARENA_ID='scene-elemental-arena',TICK_HZ='20'),
        'backend':dict(common,BACKEND_LISTEN=f"127.0.0.1:{ports['backend']}",BACKEND_DB_DRIVER='sqlite',BACKEND_DB=str(work/'mmo.db'),SCENES=f'scene-elemental-world={scene},scene-elemental-arena={scene}',AUTH_URL=''),
        'gateway':dict(common,GATEWAY_LISTEN=f"127.0.0.1:{ports['gateway']}",GATEWAY_HEALTH_LISTEN=f"127.0.0.1:{ports['health']}",BACKEND_URL=f"http://127.0.0.1:{ports['backend']}",GATEWAY_PROFILE='elemental',GATEWAY_LOGIN_SCENE='scene-elemental-world',GATEWAY_ARENA_SCENE='scene-elemental-arena')}
    children=[];logs=[];peers=[]
    try:
        for name,env in configs.items():
            binary=ROOT/'bin'/(name+('.exe' if os.name=='nt' else ''));log=(work/(name+'.log')).open('wb');logs.append(log)
            child=subprocess.Popen([str(binary)],env=env,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0);children.append(child)
            health=ports['scene' if name=='scene-server' else 'health' if name=='gateway' else 'backend']
            for _ in range(100):
                if child.poll() is not None:raise RuntimeError(name+' exited; inspect fixture log')
                try:
                    assert http(f'http://127.0.0.1:{health}/healthz')[0]==200
                    break
                except Exception:time.sleep(.1)
            else:raise RuntimeError(name+' did not become healthy')
        frames=work/'frames.jsonl'
        env=dict(os.environ,MMO_TEST_ELEMENTAL_GATEWAY=f"127.0.0.1:{ports['gateway']}",MMO_TEST_ELEMENTAL_FRAMES=str(frames))
        r=subprocess.run(['go','test','./internal/wsbridge','-run','^TestElementalWorldIntegration$','-count=1','-v','-timeout','90s'],cwd=ROOT,env=env,capture_output=True,text=True,timeout=100)
        (work/'protocol.log').write_text(r.stdout+r.stderr,encoding='utf-8');print(r.stdout)
        if r.returncode:raise RuntimeError('elemental protocol checks failed')
        p=Peer(ports['gateway']);peers.append(p);auth=p.request('register',name='elemental-fault-check');p.request('login',token=auth['token'])
        view=p.request('view');context=lambda v:dict(scene_id=v['scene_id'],owner_epoch=v['self']['owner_epoch'])
        with fault_lock:faults['/v1/prepare']=1
        p.request('enter_arena',ok=False,**context(view))
        rolled_back=p.request('view');assert rolled_back['self']['owner_epoch']==view['self']['owner_epoch'] and 'battle' not in rolled_back
        with fault_lock:faults['/v1/prepare']=1;faults['/v1/unfreeze']=1
        p.request('enter_arena',ok=False,**context(rolled_back));p.close();peers.remove(p);time.sleep(.3)
        p=Peer(ports['gateway']);peers.append(p);p.request('login',token=auth['token']);rolled_back=p.request('view')
        assert 'battle' not in rolled_back and rolled_back['self']['session_epoch']>view['self']['session_epoch']
        p.request('enter_arena',**context(rolled_back));arena=p.request('view')
        with fault_lock:faults['/v1/activate']=1
        p.request('restart_battle',ok=False,**context(arena))
        time.sleep(1.5)
        restored=p.request('view');assert restored['self']['owner_epoch']>arena['self']['owner_epoch'] and restored['self']['session_epoch']==arena['self']['session_epoch'] and restored['battle']['battle_id']!=arena['battle']['battle_id']
        p.request('leave_arena',**context(restored));final=p.request('view');assert 'battle' not in final and final['self']['x']==view['self']['x'] and final['self']['y']==view['self']['y']
        print('PASS dropped prepare safely aborts; dropped activation recovers committed owner and restores return position')
        report={'ok':True,'scope':'isolated native elemental authority, WebSocket and lost-response recovery; no Unity build','fault_checks':['prepare response lost after execution','unfreeze response lost followed by client disconnect and relogin','activate response lost after commit'],'frames':str(frames),'directory':str(work)}
        (work/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report))
    finally:
        for p in peers:p.close()
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
                try:child.wait(timeout=8)
                except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)
        for log in logs:log.close()
        proxy.shutdown();proxy.server_close()

if __name__=='__main__':main()
