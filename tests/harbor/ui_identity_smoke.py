"""Read current chat through the real CLI and park duplicates without discarding composer state."""
from native_smoke import Terminal
from pathlib import Path
import json, os, subprocess, tempfile, time
exe=os.environ['HARBOR_EXECUTABLE']
with tempfile.TemporaryDirectory(prefix='hi-',dir='/tmp') as directory:
    config=Path(directory)/'config'; home=Path(directory)/'home';home.mkdir();(home/'.local/state').mkdir(parents=True)
    env=dict(HOME=str(home),XDG_CONFIG_HOME=str(home/'xdg'),XDG_STATE_HOME=str(home/'.local/state'),HARBOR_DEMO_TOPICS='1',HARBOR_DEMO_ROUTES='1')
    windows=[]
    def control(window,surface,*flags):
        child=subprocess.Popen([exe,'-d',str(config),'--target-surface',surface,'--target-pid',str(window.pid),*flags],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=dict(os.environ,**env))
        if '--inspect-window' in flags:inspection_pids.append(child.pid)
        until=time.monotonic()+10
        while child.poll() is None and time.monotonic()<until:
            for view in windows:view.pump(.01)
        if child.poll() is None:child.kill();child.wait();raise AssertionError('control did not finish')
        out,err=child.communicate();return child.returncode,out,err
    def inspect(window,surface):
        code,out,err=control(window,surface,'--inspect-ui');assert code==0,err
        fields=out.rstrip('\n').split('\t');assert len(fields)==9,fields
        assert fields[:3]==['pcl.harbor.ui.v2',str(window.pid),surface]
        return fields
    samples=[];inspection_pids=[]
    def inspect_window(window,surface):
        code,out,err=control(window,surface,'--inspect-window');assert code==0,err
        fields=out.rstrip('\n').split('\t');assert len(fields)==10,fields
        assert fields[:3]==['pcl.harbor.window.v1',str(window.pid),surface]
        assert fields[3].startswith(str(window.pid)+'-') and fields[4].isdigit()
        assert fields[5].count('.')==2 and fields[6]
        assert all(not value or value.isdigit() for value in fields[7:9])
        assert fields[9] in ('picker','conversation')
        samples.append(out);return fields
    try:
        backend_window=Terminal(cols=110,config_dir=config,extra_env=dict(env,CMUX_SURFACE_ID='IDENTITY-BACKEND'));windows.append(backend_window)
        backend_window.picker('Saved Messages')
        first=Terminal(cols=110,config_dir=config,extra_env=dict(env,CMUX_SURFACE_ID='IDENTITY-ONE'),args=['--open-telegram-chat','12345','--open-telegram-topic','421']);windows.append(first)
        second=Terminal(cols=110,config_dir=config,extra_env=dict(env,CMUX_SURFACE_ID='IDENTITY-TWO'));windows.append(second)
        first.expect('Agent');first.send('Retained unique draft');initial=inspect(first,'IDENTITY-ONE')
        window_initial=inspect_window(first,'IDENTITY-ONE');other_window=inspect_window(second,'IDENTITY-TWO')
        assert window_initial[3]!=other_window[3], 'two native windows shared one runtime identity'
        assert initial[4].endswith('#421') and initial[6:8] == ['1','1'] and initial[8] == '0',initial
        first.picker('Doug');different=inspect(first,'IDENTITY-ONE');assert different[4].endswith('#550'),different
        window_different=inspect_window(first,'IDENTITY-ONE')
        assert window_initial[3:7]==window_different[3:7] and window_initial[8]!=window_different[8]
        assert different[4]!=initial[4], 'query reused original launch argv'
        first.picker('Agent');current=inspect(first,'IDENTITY-ONE');first.send(' changed')
        assert control(first,'IDENTITY-ONE','--park-ui',current[5])[0]!=0,'changed composer was parked from old evidence'
        first.expect('Retained unique draft changed');current=inspect(first,'IDENTITY-ONE');other=second.text
        assert control(first,'IDENTITY-ONE','--park-ui',current[5])[0]==0
        parked=inspect(first,'IDENTITY-ONE');assert parked[3:5]==['',''],parked
        assert inspect_window(first,'IDENTITY-ONE')[7:]==['','','picker']
        first.pump(.8);assert inspect(first,'IDENTITY-ONE')[4]=='','incoming chats reopened parked view'
        assert second.text==other,'another UI changed'
        first.picker('Agent');first.expect('Retained unique draft changed')
        reopened=inspect(first,'IDENTITY-ONE');assert reopened[4]==initial[4]
        assert inspect_window(first,'IDENTITY-ONE')[8]==window_initial[8]
        trace=home/'.local/state/harbor/operational.jsonl'
        first.send('\r') # Offline protocol only: qualify a real semantic send and its native result.
        until=time.monotonic()+6
        while time.monotonic()<until:
            records=[json.loads(line) for line in trace.read_text().splitlines()]
            sends=[r for r in records if r.get('event')=='request.sent' and r.get('pid')==first.pid and r.get('operation')=='message.send']
            if any(any(r.get('event')=='request.result.received' and r.get('pid')==first.pid and r.get('outcome')=='success' and (r.get('request_id'),r.get('connection_id'))==(s.get('request_id'),s.get('connection_id')) for r in records) for s in sends):break
            first.pump(.03)
        else:raise AssertionError('semantic send did not retain its correlated native success')
        data=trace.read_text();assert 'Retained unique draft' not in data and ' changed' not in data
        assert any(json.loads(line).get('event')=='ui.chat.opened' for line in data.splitlines())
        assert not any(json.loads(line).get('pid') in inspection_pids for line in data.splitlines()),'operational reads wrote the journal and would trigger a reporting loop'
        evidence=os.environ.get('HARBOR_EVIDENCE_DIR')
        if evidence:
            destination=Path(evidence);destination.mkdir(parents=True,exist_ok=True)
            (destination/'window-observations.json').write_text(json.dumps(samples))
            (destination/'operational-native.jsonl').write_text(data)
        print('NATIVE_IDENTITY_SAMPLES '+json.dumps({'initial':initial,'switched':different,'parked':parked,'reopened':reopened}),flush=True)
        print('PASS actual CLI current identity differs from launch argv; conditional parking rejects draft races; all draft state and other UI preserved',flush=True)
    finally:
        for view in windows:view.close()
        if windows:windows[-1].wait_backend_shutdown()
