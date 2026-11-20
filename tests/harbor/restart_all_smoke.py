"""Two real PTYs restart in place through Office's actual request publisher, using disposable demo profiles only."""
import json, os, subprocess, tempfile, time
from pathlib import Path
from native_smoke import Terminal, ROOT

with tempfile.TemporaryDirectory(prefix='harbor-restart-all-') as directory:
    home=Path(directory); config=home/'profile'
    env={'HOME':str(home),'HARBOR_DESK_COMMANDS':'','HARBOR_DESK_UPDATE_FILE':'','CMUX_SURFACE_ID':''}
    binary=Path(os.environ.get('HARBOR_EXECUTABLE',ROOT/'build-harbor/bin/harbor')).resolve()
    office=home/'office'; target=office/'dist/harbor-runtime/bin/harbor'
    target_binary=Path(os.environ.get('HARBOR_RESTART_TARGET',str(binary))).resolve()
    target.parent.mkdir(parents=True); target.symlink_to(target_binary)
    a=Terminal(config_dir=config,extra_env=env,executable=binary)
    b=Terminal(config_dir=config,extra_env=env,executable=binary)
    try:
        a.picker('Saved Messages'); a.send(b'\x1b[5~'); a.send('anchored draft')
        before=[row.strip() for row in a.screen.display if 'Note ' in row]
        assert before and 'Note 35:' not in '\n'.join(before),a.text
        b.picker('Studio'); b.send('live draft')
        old_names=set((home/'.office/harbor-windows').iterdir())
        a_start,b_start=len(a.raw),len(b.raw)
        module=os.environ.get('HARBOR_OFFICE_RESTART_MODULE')
        script='''import {restartHarbors} from MODULE;
const home=process.argv[1];const office=home+'/office';
try {await restartHarbors(home,office,{home,prepare:async()=>{throw Error('fixture preparation failed');},ready:()=>true});throw Error('missing failure');}catch(e){if(e.message!=='fixture preparation failed')throw e;}
const result=await restartHarbors(home,office,{home,prepare:async()=>null,ready:()=>true});console.log(JSON.stringify(result));'''.replace('MODULE',json.dumps(Path(module).resolve().as_uri()) if module else '"unused"')
        if module:
            result=subprocess.run(['node','--input-type=module','-e',script,str(home)],text=True,capture_output=True,check=True)
            assert json.loads(result.stdout)['requested']==2,result.stdout
        else:
            for record in old_names:
                request=Path(str(record)+'.restart')
                request.write_text('pcl.harbor.restart.v1\t'+str(target_binary)+'\n');request.chmod(0o600)
        end=time.monotonic()+10
        while time.monotonic()<end:
            a.pump(.05);b.pump(.05)
            names=set((home/'.office/harbor-windows').iterdir())
            if len(names)==2 and not names.intersection(old_names):break
        else:raise AssertionError('both native registrations did not change after exec')
        # Registration happens before the asynchronous chat restore renders. Wait for both
        # new terminal titles, rather than treating the previous screen as the resumed UI.
        end=time.monotonic()+5
        while time.monotonic()<end:
            a.pump(.05);b.pump(.05)
            if b'Saved Messages' in a.raw[a_start:] and b'Studio' in b.raw[b_start:]:break
        else:raise AssertionError('both updated chat views did not render')
        a.expect('anchored draft');b.expect('live draft');b.expect('Studio')
        expected_version=subprocess.run([str(target_binary),'--version'],text=True,capture_output=True,check=True).stdout.split()[1]
        assert all(expected_version in record.read_text().split('\t')[2] for record in names)
        after=[row.strip() for row in a.screen.display if 'Note ' in row]
        assert after==before,(before,after)
        assert a.screen.title=='Saved Messages' and b.screen.title=='Studio'
        a.send('!');a.expect('anchored draft!');b.send('!');b.expect('live draft!')
        publisher='actual Office publisher' if module else 'native request format'
        print('PASS '+publisher+' restarted two real native PTYs in place; independent chats, message/line position and drafts survived; both accept input')
    finally:b.close();a.close()
