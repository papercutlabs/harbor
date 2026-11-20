"""Real shipped guard, effect process, inherited lock, responsive native UI; no OS app effects."""
from native_smoke import Terminal,ROOT
from pathlib import Path
import fcntl,json,os,select,signal,struct,subprocess,sys,tempfile,time
exe=os.environ['HARBOR_EXECUTABLE']
def frame(args):
    parts=[struct.pack('<I',len(args))]
    for arg in args:
        b=arg.encode();parts.extend([struct.pack('<I',len(b)),b])
    body=b''.join(parts);return struct.pack('<I',len(body))+body

def until(check,message,seconds=5,pump=None):
    deadline=time.monotonic()+seconds
    while not check():
        assert time.monotonic()<deadline,message
        if pump:pump(.02)
        else:time.sleep(.01)

with tempfile.TemporaryDirectory(prefix='harbor-preview-guard-',dir='/tmp') as directory:
    root=Path(directory);home=root/'home';home.mkdir();case=root/'effect';case.mkdir();timeline=root/'timeline'
    effect=root/'effect.py';effect.write_text('#!'+sys.executable+'\n'+'''import os,pathlib,sys,time
p=pathlib.Path(os.environ['HARBOR_TEST_EFFECT_DIR']);p.mkdir(exist_ok=True)
(p/'started').write_text(str(os.getpid()))
with open(os.environ['HARBOR_TEST_TIMELINE'],'a') as f:f.write('effect-start\\n')
mode=os.environ.get('HARBOR_TEST_EFFECT_MODE','wait')
if mode=='wait':
 while not (p/'release').exists():time.sleep(.01)
if mode=='timeout':time.sleep(30)
with open(os.environ['HARBOR_TEST_TIMELINE'],'a') as f:f.write('effect-end\\n')
if mode=='error':
 sys.stdout.write('out:'+('A'*5000));sys.stderr.write('err:'+('B'*5000));sys.exit(37)
print('effect-ok')
''');effect.chmod(0o700)
    opened=root/'opened';opener=root/'open.py';opener.write_text('#!'+sys.executable+'\n'+'''import os,pathlib,sys
with open(os.environ['HARBOR_TEST_TIMELINE'],'a') as f:f.write('image-open\\n')
with open(os.environ['HARBOR_TEST_OPENED'],'a') as f:f.write(sys.argv[1]+'\\n')
''');opener.chmod(0o700)
    inherited={k:v for k,v in os.environ.items() if k in {'PATH','LANG','LC_ALL','LC_CTYPE','TERM','TMPDIR'}}
    env=dict(inherited,HARBOR_EXECUTABLE=exe,PYTHONDONTWRITEBYTECODE='1',TERM='xterm-256color',LANG='en_US.UTF-8',HOME=str(home),XDG_CONFIG_HOME=str(root/'xdg'),XDG_STATE_HOME=str(root/'state'),
        HARBOR_DEMO_FIXTURES=str(ROOT/'tests/harbor/fixtures'),HARBOR_TEST_FOCUS_EFFECT=str(effect),
        HARBOR_TEST_EFFECT_DIR=str(case),HARBOR_TEST_TIMELINE=str(timeline),HARBOR_TEST_OPENED=str(opened),
        HARBOR_TEST_OPENER=str(opener),HARBOR_DEMO_PICTURES='1',HARBOR_DEMO_PICTURE_DELAY_MS='0',CMUX_SURFACE_ID='GUARD-UI')
    office_driver=os.environ.get('HARBOR_TEST_OFFICE_GUARD_DRIVER');office_dist=os.environ.get('HARBOR_TEST_OFFICE_DIST');office_node=os.environ.get('HARBOR_TEST_NODE')
    os.environ.clear();os.environ.update(env)
    if office_driver:
        assert office_dist,'Office paired qualification needs its actual dist'
        os.environ['HARBOR_TEST_OFFICE_GUARD_DRIVER']=office_driver;os.environ['HARBOR_TEST_OFFICE_DIST']=office_dist
        if office_node:os.environ['HARBOR_TEST_NODE']=office_node
    children=[];t=None;effect_pid=None
    def start(extra=None,office=False):
        command=[exe,'--focus-guard']
        selected=dict(env,**(extra or {}))
        if office:
            command=[os.environ.get('HARBOR_TEST_NODE','node'),os.environ['HARBOR_TEST_OFFICE_GUARD_DRIVER'],os.environ['HARBOR_TEST_OFFICE_DIST']]
            selected['HARBOR_DEMO_FIXTURES']='1'
        child=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=selected)
        child.office=office
        children.append(child)
        assert select.select([child.stdout],[],[],5)[0],'guard readiness missing'
        first=child.stdout.readline()
        if office and first==b'OFFICE_GUARD_READY\n':first=b'READY\n'
        return child,first
    def request(child,args=('/usr/bin/osascript','-e','return "fixture"')):
        child.stdin.write(b'RUN\n' if child.office else frame(args));child.stdin.close();child.stdin=None
    def inspect():
        child=subprocess.Popen([exe,'-d',str(root/'config'),'--target-surface','GUARD-UI','--target-pid',str(t.pid),'--inspect-ui'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env)
        until(lambda:child.poll() is not None,'inspection blocked behind Focus guard',pump=t.pump)
        out,err=child.communicate();assert child.returncode==0,err
        fields=out.rstrip('\n').split('\t');assert fields[0]=='pcl.harbor.ui.v2';return fields
    def free():
        with (home/'.office/desk/preview-focus.lock').open('a+') as lock:
            try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);return True
            except BlockingIOError:return False
    def busy():
        child,first=start();out,err=child.communicate(timeout=3)
        assert first==b'BUSY\n' and child.returncode==75,(first,out,err,child.returncode)
    try:
        early=root/'early';(early/'home').mkdir(parents=True)
        probe,first=start({'HOME':str(early/'home'),'XDG_CONFIG_HOME':str(early/'xdg'),'XDG_STATE_HOME':str(early/'logs'),'HARBOR_TEST_EFFECT_DIR':str(root/'early-effect'),'HARBOR_TEST_EFFECT_MODE':'error','HARBOR_TEST_TIMELINE':str(root/'early-timeline')});assert first==b'READY\n'
        request(probe);out,err=probe.communicate(timeout=5);assert probe.returncode==37
        assert not (early/'xdg').exists() and not (early/'logs').exists(),'helper initialized profile or operational logs'
        early_files=[str(p.relative_to(early)) for p in early.rglob('*') if p.is_file()]
        assert early_files==['home/.office/desk/preview-focus.lock'],early_files
        t=Terminal(cols=56,rows=30,config_dir=root/'config',extra_env=env)
        if 'photo-shown' not in t.text:t.picker('Studio')
        t.expect('harbor.png');t.send('guard draft')
        holder,first=start(office=bool(office_driver));assert first==b'READY\n',first
        assert inspect()[8]=='0'
        busy()
        y=next(y for y,line in enumerate(t.screen.display) if 'harbor.png' in line)
        t.send(f'\x1b[<0;9;{y+2}M',wait=.03);t.send(f'\x1b[<0;9;{y+2}m',wait=.03)
        t.expect('Opening picture');t.send('x');t.expect('guard draftx')
        queued=inspect();assert queued[7:9]==['0','0'] and not opened.exists(),'open escaped the held EX guard or queued action was reported ready'
        t.send(b'\x0f');t.expect('Commands');t.send(b'\x1b')
        request(holder);until(lambda:(case/'started').exists(),'effect did not start');effect_pid=int((case/'started').read_text())
        busy()
        # Kill the holder (actual Node with the integration hook) while the effect remains in flight.
        holder.kill();holder.wait(timeout=3);busy()
        assert not opened.exists() and inspect()[8]=='0','holder death released an active effect'
        (case/'release').touch()
        until(lambda:opened.exists(),'queued picture did not resume after effect ended',pump=t.pump)
        events=timeline.read_text().splitlines();assert events.index('effect-end')<events.index('image-open'),events
        first_open=inspect();assert int(first_open[8])>0
        t.send(b'\x1b[I');assert inspect()[8]==first_open[8],'late programmatic focus-in revoked a fresh explicit open'
        t.send(b'\x05');assert inspect()[8]=='0','deliberate return did not revoke Preview'
        print('PASS real EX/SH ordering, BUSY, effect survives holder death, responsive draft/menu/inspection, late-I negative, deliberate input',flush=True)
        if office_driver:
            # A stale positive packet cannot force activation after deliberate input revoked it.
            y=next(y for y,line in enumerate(t.screen.display) if 'harbor.png' in line)
            t.send(f'\x1b[<0;9;{y+2}M',wait=.03);t.send(f'\x1b[<0;9;{y+2}m',wait=.1)
            granted=inspect();assert int(granted[8])>0
            passive=root/'passive-effect';passive.mkdir();(passive/'release').touch()
            result=root/'passive-result.json'
            holder,first=start({'HARBOR_TEST_OFFICE_GUARD_MODE':'passive-return',
                'HARBOR_TEST_OFFICE_NATIVE_PID':str(t.pid),'HARBOR_TEST_OFFICE_NATIVE_SURFACE':'GUARD-UI',
                'HARBOR_TEST_OFFICE_NATIVE_PROFILE':str(root/'config'),'HARBOR_TEST_OFFICE_PASSIVE_RESULT':str(result),
                'HARBOR_TEST_EFFECT_DIR':str(passive)},office=True)
            assert first==b'READY\n'
            t.send(b'\x05');assert inspect()[8]=='0'
            request(holder);out,err=holder.communicate(timeout=6)
            assert holder.returncode==0 and b'OFFICE_PASSIVE_NO_ACTIVATION\n' in out,(holder.returncode,out,err)
            assert inspect()[8]=='0'
            native=json.loads(Path(str(result)+'.native.json').read_text())
            print('PASSIVE_RETURN_EVIDENCE '+json.dumps({'branch':json.loads(result.read_text()),'native':native}),flush=True)
            print('PASS actual Office stale grant1 effect activates neither app after native deliberate return0',flush=True)
        # Settings' normal update exec keeps the PID/surface while replacing native state.
        # A surviving Office allowance must distinguish the next explicit open.
        y=next(y for y,line in enumerate(t.screen.display) if 'harbor.png' in line)
        t.send(f'\x1b[<0;9;{y+2}M',wait=.03);t.send(f'\x1b[<0;9;{y+2}m',wait=.1)
        pre_exec=inspect();assert int(pre_exec[8])>0
        restart_holder=None
        if office_driver:
            restart_effect=root/'restart-effect';restart_effect.mkdir();(restart_effect/'release').touch()
            restart_result=root/'restart-result.json'
            restart_holder,first=start({'HARBOR_TEST_OFFICE_GUARD_MODE':'restart-event',
                'HARBOR_TEST_OFFICE_NATIVE_PID':str(t.pid),'HARBOR_TEST_OFFICE_NATIVE_SURFACE':'GUARD-UI',
                'HARBOR_TEST_OFFICE_NATIVE_PROFILE':str(root/'config'),'HARBOR_TEST_OFFICE_RESTART_RESULT':str(restart_result),
                'HARBOR_TEST_EFFECT_DIR':str(restart_effect)},office=True)
            assert first==b'READY\n'
        registry=home/'.office/harbor-windows'
        original=set(registry.iterdir());assert len(original)==1,original
        registration=next(iter(original))
        restart=Path(str(registration)+'.restart')
        restart.write_text('pcl.harbor.restart.v1\t'+str(Path(exe).resolve())+'\n');restart.chmod(0o600)
        until(lambda:bool(set(registry.iterdir())) and not set(registry.iterdir()).intersection(original),
              'same-PID exec did not replace its native registration',seconds=12,pump=t.pump)
        restored=[]
        def resumed():
            restored[:]=inspect()
            return restored[1:5]==pre_exec[1:5]
        until(resumed,'same-PID exec did not restore the original conversation',seconds=12,pump=t.pump)
        post_reset=list(restored);assert post_reset[8]=='0',post_reset
        t.expect('harbor.png')
        y=next(y for y,line in enumerate(t.screen.display) if 'harbor.png' in line)
        t.send(f'\x1b[<0;9;{y+2}M',wait=.03);t.send(f'\x1b[<0;9;{y+2}m',wait=.1)
        post_exec=inspect();assert int(post_exec[8])>int(pre_exec[8]),(pre_exec,post_exec)
        print('PREVIEW_RESTART_SAMPLES '+json.dumps({'before': '\t'.join(pre_exec)+'\n',
            'reset':'\t'.join(post_reset)+'\n','after':'\t'.join(post_exec)+'\n'}),flush=True)
        if restart_holder:
            request(restart_holder);out,err=restart_holder.communicate(timeout=8)
            assert restart_holder.returncode==0 and b'OFFICE_RESTART_EVENT_FRESH\n' in out,(restart_holder.returncode,out,err)
            print('PREVIEW_RESTART_OFFICE '+restart_result.read_text(),flush=True)
        print('PASS same-PID exec resets active allowance and next explicit open has a greater event ID',flush=True)
        # EOF before a request is harmless and releases without an effect.
        holder,first=start();assert first==b'READY\n';holder.stdin.close();holder.stdin=None
        out,err=holder.communicate(timeout=3);assert holder.returncode==0 and out==b'' and err==b''
        # Complete child stdout/stderr/status remain intact.
        error_case=root/'error';error_case.mkdir()
        holder,first=start({'HARBOR_TEST_EFFECT_DIR':str(error_case),'HARBOR_TEST_EFFECT_MODE':'error'});assert first==b'READY\n';request(holder)
        out,err=holder.communicate(timeout=5);assert holder.returncode==37 and out==b'out:'+b'A'*5000 and err==b'err:'+b'B'*5000,(holder.returncode,len(out),len(err))
        # Invalid executable and payload tail cause no effect.
        for body in [frame(['/bin/false']),frame(['/usr/bin/osascript'])[:-1]]:
            holder,first=start();assert first==b'READY\n';holder.stdin.write(body);holder.stdin.close();holder.stdin=None
            out,err=holder.communicate(timeout=3);assert holder.returncode==2 and err,(holder.returncode,out,err)
        timeout_case=root/'timeout';timeout_case.mkdir()
        holder,first=start({'HARBOR_TEST_EFFECT_DIR':str(timeout_case),'HARBOR_TEST_EFFECT_MODE':'timeout','HARBOR_TEST_FOCUS_LIMIT_MS':'2000'});assert first==b'READY\n';request(holder)
        out,err=holder.communicate(timeout=6);assert holder.returncode==124 and b'execution bound' in err,(holder.returncode,out,err,(timeout_case/'started').read_text() if (timeout_case/'started').exists() else 'effect-not-started')
        holder,first=start();assert first==b'READY\n','timeout retained the guard';holder.stdin.close();holder.stdin=None;holder.communicate(timeout=3)
        # A real caller process dies after sending its frame. The holder drains the live effect.
        caller_case=root/'caller-death';caller_case.mkdir()
        driver_code="""import os,pathlib,struct,subprocess,sys
p=pathlib.Path(sys.argv[2]);h=subprocess.Popen([sys.argv[1],'--focus-guard'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
assert h.stdout.readline()==b'READY\\n'
a=[b'/usr/bin/osascript',b'-e',b'return fixture'];b=struct.pack('<I',len(a))+b''.join(struct.pack('<I',len(x))+x for x in a)
h.stdin.write(struct.pack('<I',len(b))+b);h.stdin.flush();(p/'caller-ready').touch();sys.stdin.buffer.read()
"""
        driver=subprocess.Popen([sys.executable,'-c',driver_code,exe,str(caller_case)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=dict(env,HARBOR_TEST_EFFECT_DIR=str(caller_case)));children.append(driver)
        until(lambda:(caller_case/'started').exists(),'caller effect did not start')
        driver.kill();driver.wait(timeout=3);busy();assert not free()
        (caller_case/'release').touch();until(free,'caller death left the completed effect guard held')
        # Killing the holder also cannot defeat the effect's inherited kernel resource bound.
        orphan_case=root/'orphan-timeout';orphan_case.mkdir()
        holder,first=start({'HARBOR_TEST_EFFECT_DIR':str(orphan_case),'HARBOR_TEST_EFFECT_MODE':'timeout','HARBOR_TEST_FOCUS_LIMIT_MS':'2000'});assert first==b'READY\n';request(holder)
        until(lambda:(orphan_case/'started').exists(),'orphan timeout effect did not start')
        holder.kill();holder.wait(timeout=3);assert not free()
        until(free,'orphan effect outlived its kernel execution bound',seconds=6)
        assert not (home/'.config/harbor').exists(),'guard helper initialized a live profile'
        print('PASS EOF cleanup, exact child outputs/status, invalid request, bounded effect and lock release',flush=True)
    finally:
        for name in ('effect','caller-death','error','timeout','orphan-timeout'):
            folder=root/name
            if folder.exists():(folder/'release').touch()
        for child in children:
            if child.poll() is None:child.kill();child.wait()
        if (home/'.office/desk/preview-focus.lock').exists():until(free,'owned guard still held during fixture cleanup',seconds=6)
        if t:t.close();t.wait_backend_shutdown()
