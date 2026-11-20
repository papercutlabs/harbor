"""Read current chat through the real CLI and park duplicates without discarding composer state."""
from native_smoke import Terminal
from pathlib import Path
import json, os, subprocess, tempfile, time
exe=os.environ['HARBOR_EXECUTABLE']
with tempfile.TemporaryDirectory(prefix='hi-',dir='/tmp') as directory:
    config=Path(directory)/'config'; home=Path(directory)/'home';home.mkdir()
    env=dict(HOME=str(home),XDG_CONFIG_HOME=str(home/'xdg'),HARBOR_DEMO_TOPICS='1',HARBOR_DEMO_ROUTES='1')
    windows=[]
    def control(window,surface,*flags):
        child=subprocess.Popen([exe,'-d',str(config),'--target-surface',surface,'--target-pid',str(window.pid),*flags],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=dict(os.environ,**env))
        until=time.monotonic()+10
        while child.poll() is None and time.monotonic()<until:
            for view in windows:view.pump(.01)
        if child.poll() is None:child.kill();child.wait();raise AssertionError('control did not finish')
        out,err=child.communicate();return child.returncode,out,err
    def inspect(window,surface):
        code,out,err=control(window,surface,'--inspect-ui');assert code==0,err
        fields=out.rstrip('\n').split('\t');assert len(fields)==8,fields
        assert fields[:3]==['pcl.harbor.ui.v1',str(window.pid),surface]
        return fields
    try:
        backend_window=Terminal(cols=110,config_dir=config,extra_env=dict(env,CMUX_SURFACE_ID='IDENTITY-BACKEND'));windows.append(backend_window)
        backend_window.picker('Saved Messages')
        first=Terminal(cols=110,config_dir=config,extra_env=dict(env,CMUX_SURFACE_ID='IDENTITY-ONE'),args=['--open-telegram-chat','12345','--open-telegram-topic','421']);windows.append(first)
        second=Terminal(cols=110,config_dir=config,extra_env=dict(env,CMUX_SURFACE_ID='IDENTITY-TWO'));windows.append(second)
        first.expect('Agent');first.send('Retained unique draft');initial=inspect(first,'IDENTITY-ONE')
        assert initial[4].endswith('#421') and initial[6:] == ['1','1'],initial
        first.picker('Doug');different=inspect(first,'IDENTITY-ONE');assert different[4].endswith('#550'),different
        assert different[4]!=initial[4], 'query reused original launch argv'
        first.picker('Agent');current=inspect(first,'IDENTITY-ONE');first.send(' changed')
        assert control(first,'IDENTITY-ONE','--park-ui',current[5])[0]!=0,'changed composer was parked from old evidence'
        first.expect('Retained unique draft changed');current=inspect(first,'IDENTITY-ONE');other=second.text
        assert control(first,'IDENTITY-ONE','--park-ui',current[5])[0]==0
        parked=inspect(first,'IDENTITY-ONE');assert parked[3:5]==['',''],parked
        first.pump(.8);assert inspect(first,'IDENTITY-ONE')[4]=='','incoming chats reopened parked view'
        assert second.text==other,'another UI changed'
        first.picker('Agent');first.expect('Retained unique draft changed')
        reopened=inspect(first,'IDENTITY-ONE');assert reopened[4]==initial[4]
        print('NATIVE_IDENTITY_SAMPLES '+json.dumps({'initial':initial,'switched':different,'parked':parked,'reopened':reopened}),flush=True)
        print('PASS actual CLI current identity differs from launch argv; conditional parking rejects draft races; all draft state and other UI preserved',flush=True)
    finally:
        for view in windows:view.close()
