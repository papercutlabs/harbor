"""Exact existing native UI routing, shared drafts, and actual Office warm return."""
from native_smoke import Terminal
from pathlib import Path
import hashlib, json, os, shutil, subprocess, sys, tempfile, time

exe=os.environ['HARBOR_EXECUTABLE']
with tempfile.TemporaryDirectory(prefix='hc-',dir='/tmp') as directory:
    root=Path(directory);config=root/'config';home=root/'home';home.mkdir();windows=[]
    env={'HOME':str(home),'XDG_CONFIG_HOME':str(root/'xdg'),'HARBOR_DEMO_TOPICS':'1','HARBOR_DEMO_ROUTES':'1'}
    first=second=None
    def request(topic='550',pid=None,surface='DESK-LEFT',chat='12345',profile=None):
        argv=[exe,'-d',str(config),'--target-surface',surface,'--target-pid',str(pid or first.pid),'--open-telegram-chat',chat]
        if topic:argv+=['--open-telegram-topic',topic]
        if profile:argv+=['--open-profile',profile]
        child=subprocess.Popen(argv,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=dict(os.environ,**env))
        end=time.monotonic()+40
        while child.poll() is None and time.monotonic()<end:
            for window in windows:window.pump(.02)
        if child.poll() is None:child.kill();child.wait();raise AssertionError('control did not finish')
        out,err=child.communicate();return child.returncode,out,err
    try:
        first=Terminal(cols=110,config_dir=config,extra_env=dict(env,CMUX_SURFACE_ID='DESK-LEFT'));windows.append(first)
        second=Terminal(cols=110,config_dir=config,extra_env=dict(env,CMUX_SURFACE_ID='OTHER-WINDOW'));windows.append(second)
        first.picker('Agent');first.send('Agent private draft')
        second.picker('Saved Messages');second.send('Other window draft');before=second.text
        assert request()[0]==0
        first.expect('Routed group > Doug');first.send('Doug private draft')
        assert second.text==before,'another UI changed'
        assert request('421')[0]==0;first.expect('Agent private draft')
        assert request()[0]==0;first.expect('Doug private draft')
        for kwargs in [{'pid':first.pid+1000000},{'surface':'MISSING-SURFACE'},{'profile':'Telegram_missing'}]:
            result=request(**kwargs);assert result[0]!=0,result
            first.expect('Doug private draft');assert second.text==before
        result=request('999');assert result[0]!=0 and 'unavailable' in result[2],result
        first.expect('previous chat and drafts kept');first.expect('Doug private draft')
        rows=subprocess.check_output(['/bin/ps','-eo','pid=,args='],text=True)
        assert len([r for r in rows.splitlines() if '--session-backend' in r and str(config) in r])==1
        print('PASS addressed UI/PID, two UI isolation, existing Doug and Agent drafts, stale/missing/account/unavailable refusal, one backend',flush=True)
        entry=os.environ.get('HARBOR_CONTROL_OFFICE_ENTRY');node=os.environ.get('HARBOR_CONTROL_NODE')
        if entry:
            assert node and Path(entry).is_file()
            node=str(Path(node).resolve());entry=str(Path(entry).resolve())
            deps=os.environ.get('HARBOR_CONTROL_OFFICE_DEPS')
            if deps:
                staged=root/'office';staged.mkdir();shutil.copytree(Path(entry).parent,staged/'dist')
                (staged/'package.json').write_text('{"type":"module"}')
                (staged/'node_modules').symlink_to(Path(deps).resolve(),target_is_directory=True)
                entry=str(staged/'dist/index.js')
            fixture=root/'bin';fixture.mkdir();own='00000000-0000-4000-8000-000000000003';workspace='00000000-0000-4000-8000-000000000001'
            tree=f'window 00000000-0000-4000-8000-000000000099\n└── workspace {workspace} "Isolated"\n    └── pane 00000000-0000-4000-8000-000000000002\n        └── surface {own} [terminal] "Desk" tty=fixture'
            for name,body in {'cmux':'print('+repr(tree)+')','tailscale':"print('127.0.0.1')",'ssh':'''print('{"version":1,"seats":[]}')'''}.items():
                path=fixture/name;path.write_text('#!'+sys.executable+'\n'+body+'\n');path.chmod(0o700)
            env.update(PATH=str(fixture)+':/usr/bin:/bin',OFFICE_SKIP_UPDATE='1',CMUX_WORKSPACE_ID=workspace,HARBOR_DESK_WARM='1')
            saved=home/'.office/desk';saved.mkdir(parents=True)
            identity=subprocess.check_output(['/bin/ps','-p',str(os.getpid()),'-o','lstart=,args=','-ww'],text=True).strip()
            (saved/'state.json').write_text(json.dumps({'version':1,'homes':{},'pins':{},'focus':False,'instance':{'pid':os.getpid(),'surface':own,'workspace':workspace,'identity':hashlib.sha256(identity.encode()).hexdigest()}}))
            commands=saved/'harbor-desk-commands';module=Path(entry).parent/'desk/harbor.js'
            script='import {deskCommandLines,writeDeskCommands} from '+json.dumps(module.as_uri())+';writeDeskCommands('+json.dumps(str(commands))+',deskCommandLines('+json.dumps(node)+','+json.dumps(entry)+'));'
            subprocess.run([node,'--input-type=module','-e',script],check=True);env['HARBOR_DESK_COMMANDS']=str(commands)
            # Restart only our first UI to install the actual Office child contract; same UI slot restores drafts.
            first.close();windows.remove(first)
            first=Terminal(cols=110,config_dir=config,extra_env=dict(env,CMUX_SURFACE_ID='DESK-LEFT'));windows.append(first)
            first.expect('Doug private draft')
            warm_pid=None
            for key,label in [('q','Quota'),('e','Everything')]:
                first.send(b'\x0f');first.expect('Desk · '+label);first.send(key)
                first.expect('QUOTA' if key=='q' else 'Sessions',timeout=12)
                first.pump(.4);assert 'Desk · '+label not in first.text
                children=[row.split(None,2) for row in subprocess.check_output(['/bin/ps','-eo','pid=,ppid=,args='],text=True).splitlines() if ' desk --view ' in row]
                children=[int(row[0]) for row in children if int(row[1])==first.pid]
                assert len(children)==1,children
                if warm_pid is None:warm_pid=children[0]
                assert children[0]==warm_pid,'warm child replaced'
                assert request()[0]==0
                os.kill(warm_pid,0)
                first.expect('Routed group > Doug');first.expect('Doug private draft');assert second.text==before
                assert request('421')[0]==0;first.expect('Agent private draft')
                assert request()[0]==0;first.expect('Doug private draft')
            first.close();windows.remove(first)
            first=Terminal(cols=110,config_dir=config,extra_env=dict(env,CMUX_SURFACE_ID='DESK-LEFT',HARBOR_DESK_WARM='0'));windows.append(first)
            first.send(b'\x0f');first.expect('Desk · Quota');first.send('q');first.expect('QUOTA',timeout=12);first.pump(.4)
            result=request();assert result[0]!=0 and 'legacy Desk view' in result[2],result
            first.send(b'\x1b');first.expect('Doug private draft');assert second.text==before
            print('PASS actual Office legacy view refuses addressed navigation until ordinary return',flush=True)
            print('PASS actual Office Quota and Everything active warm return into existing Doug with both drafts and other UI preserved',flush=True)
        else:print('SKIP actual Office warm return: explicit HARBOR_CONTROL_OFFICE_ENTRY and HARBOR_CONTROL_NODE not supplied',flush=True)
    except BaseException:
        for diagnostic in config.glob('harbor-desk-error-*'):
            print('DISPOSABLE OFFICE DIAGNOSTIC',diagnostic.name,diagnostic.read_text(),file=sys.stderr,flush=True)
        raise
    finally:
        for window in windows:window.close()
