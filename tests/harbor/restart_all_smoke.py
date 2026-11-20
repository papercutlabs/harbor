"""Settings restarts two real PTYs and their shared backend using disposable demo profiles only.

HARBOR_OFFICE_RESTART_MODULE optionally exercises the compiled Office publisher too.
That integration overrides only update preparation; Office's normal buildUpdate has its own tests.
"""
import json, os, shutil, subprocess, sys, tempfile, time
from pathlib import Path
from native_smoke import Terminal, ROOT

with tempfile.TemporaryDirectory(prefix='harbor-restart-all-') as directory:
    home=Path(directory); config=home/'profile'
    commands=home/'.office/desk/harbor-desk-commands';commands.parent.mkdir(parents=True)
    env={'HOME':str(home),'HARBOR_DESK_COMMANDS':str(commands),'HARBOR_DESK_UPDATE_FILE':'','CMUX_SURFACE_ID':''}
    binary=Path(os.environ.get('HARBOR_EXECUTABLE',ROOT/'build-harbor/bin/harbor')).resolve()
    target_binary=Path(os.environ.get('HARBOR_RESTART_TARGET',str(binary))).resolve()
    office=home/'office'; target=office/'dist/harbor-runtime/bin/harbor'
    target.parent.mkdir(parents=True); target.symlink_to(target_binary)
    module=os.environ.get('HARBOR_OFFICE_RESTART_MODULE')
    if module:
        child=home/'update.mjs'
        child.write_text('''import {restartHarbors} from MODULE;
import {writeFileSync} from 'node:fs';
if(JSON.stringify(process.argv.slice(2))!==JSON.stringify(['update','--restart-harbor']))throw Error('wrong Settings argv');
const home=process.env.HOME,office=home+'/office';
try {await restartHarbors(home,office,{home,prepare:async()=>{throw Error('fixture preparation failed');},ready:()=>true});throw Error('missing failure');}catch(e){if(e.message!=='fixture preparation failed')throw e;}
const result=await restartHarbors(home,office,{home,prepare:async()=>null,ready:()=>true});
writeFileSync(home+'/publisher-result.json',JSON.stringify(result));'''.replace('MODULE',json.dumps(Path(module).resolve().as_uri())))
        runner=shutil.which('node');assert runner
    else:
        child=home/'update.py'
        child.write_text('''import os,sys
from pathlib import Path
assert sys.argv[1:]==['update','--restart-harbor']
home=Path(os.environ['HOME']);target=(home/'office/dist/harbor-runtime/bin/harbor').resolve()
for record in (home/'.office/harbor-windows').iterdir():
    if record.suffix:continue
    request=Path(str(record)+'.restart');request.write_text('pcl.harbor.restart.v1\\t'+str(target)+'\\n');request.chmod(0o600)
''')
        runner=sys.executable
    # An old Desk command format must suffice; no new command row is required for Settings.
    commands.write_text('Quota\tProvider capacity\t'+runner+'\t'+str(child)+'\tdesk\t--view\tquota\n')
    a=Terminal(config_dir=config,extra_env=env,executable=binary)
    b=Terminal(config_dir=config,extra_env=env,executable=binary)
    try:
        a.picker('Saved Messages'); a.send(b'\x1b[5~'); a.send('anchored draft')
        before=[row.strip() for row in a.screen.display if 'Note ' in row]
        assert before and 'Note 35:' not in '\n'.join(before),a.text
        b.picker('Studio'); b.send('live draft')
        old_names=set((home/'.office/harbor-windows').iterdir())
        old_socket=(config/'session.sock').stat().st_ino
        a_start,b_start=len(a.raw),len(b.raw)
        a.send(b'\x0f')
        for _ in range(30):
            if 'Settings' in a.text:break
            a.send(b'\x1b[B',wait=.02)
        a.click_text('Settings');a.click_text('Update all windows')
        end=time.monotonic()+12
        while time.monotonic()<end:
            a.pump(.05);b.pump(.05)
            names=set((home/'.office/harbor-windows').iterdir())
            if len(names)==2 and not names.intersection(old_names):break
        else:raise AssertionError('both native registrations did not change after exec')
        assert (config/'session.sock').stat().st_ino!=old_socket,'old shared backend survived restart'
        end=time.monotonic()+5
        while time.monotonic()<end:
            a.pump(.05);b.pump(.05)
            if b'Saved Messages' in a.raw[a_start:] and b'Studio' in b.raw[b_start:]:break
        else:raise AssertionError('both updated chat views did not render')
        a.expect('anchored draft');b.expect('live draft');b.expect('Studio')
        expected_version=subprocess.run([str(target_binary),'--version'],text=True,capture_output=True,check=True).stdout.split()[1]
        assert all(expected_version==record.read_text().split('\t')[2] for record in names)
        after=[row.strip() for row in a.screen.display if 'Note ' in row]
        assert after==before,(before,after)
        assert a.screen.title=='Saved Messages' and b.screen.title=='Studio'
        a.send('!');a.expect('anchored draft!');b.send('!');b.expect('live draft!')
        if module:assert json.loads((home/'publisher-result.json').read_text())=={'requested':2,'unsupported':0}
        print('PASS Settings action, old Desk argv, two real native PTYs and replacement shared backend; independent chats, message/line position and drafts survived; both accept input')
    finally:b.close();a.close()
