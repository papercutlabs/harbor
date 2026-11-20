"""Settings restarts two real PTYs and their shared backend using disposable demo profiles only.

HARBOR_OFFICE_RESTART_MODULE optionally exercises the compiled Office publisher too.
That integration overrides only update preparation; Office's normal buildUpdate has its own tests.
"""
import fcntl, json, os, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path
from native_smoke import Terminal, ROOT

with tempfile.TemporaryDirectory(prefix='harbor-restart-all-') as directory:
    home=Path(directory); config=home/'profile'
    commands=home/'.office/desk/harbor-desk-commands';commands.parent.mkdir(parents=True)
    env={'HOME':str(home),'HARBOR_DESK_COMMANDS':str(commands),'HARBOR_DESK_UPDATE_FILE':'','CMUX_SURFACE_ID':'','HARBOR_DEMO_RICH':'1'}
    binary=Path(os.environ.get('HARBOR_EXECUTABLE',ROOT/'build-harbor/bin/harbor')).resolve()
    target_binary=Path(os.environ.get('HARBOR_RESTART_TARGET',str(binary))).resolve()
    office=home/'office'; target=office/'dist/harbor-runtime/bin/harbor'
    target.parent.mkdir(parents=True); target.symlink_to(target_binary)
    module=os.environ.get('HARBOR_OFFICE_RESTART_MODULE')
    if module:
        child=home/'update.mjs'
        child.write_text(r'''import {restartHarbors} from MODULE;
import {updateRunner} from RUNNER;
import {readdirSync} from 'node:fs';
import {writeFileSync,existsSync} from 'node:fs';
if(JSON.stringify(process.argv.slice(2))!==JSON.stringify(['update','--restart-harbor']))throw Error('wrong Settings argv');
const home=process.env.HOME,office=home+'/office';
// Bind the global process census to the owned PTYs; targeted identity reads remain native.
const owned=new Set(readdirSync(home+'/.office/harbor-windows').filter(n=>/^[0-9]+-[a-f0-9]{32}$/.test(n)).map(n=>Number(n.split('-')[0])));
const run=async(file,args,cwd)=>{const result=await updateRunner(file,args,cwd);return args.includes('-U')?{...result,stdout:result.stdout.split('\n').filter(line=>owned.has(Number(line.trim().split(/\s+/)[0]))).join('\n')}:result;};
if(existsSync(home+'/fail-update'))await restartHarbors(home,office,{home,run,prepare:async()=>{throw Error('fixture preparation failed');},ready:()=>true});
try {await restartHarbors(home,office,{home,run,prepare:async()=>{throw Error('fixture preparation failed');},ready:()=>true});throw Error('missing failure');}catch(e){if(e.message!=='fixture preparation failed')throw e;}
const result=await restartHarbors(home,office,{home,run,prepare:async()=>null,ready:()=>true});
writeFileSync(home+'/publisher-result.json',JSON.stringify(result));'''.replace('MODULE',json.dumps(Path(module).resolve().as_uri())).replace('RUNNER',json.dumps(Path(module).resolve().with_name('update.js').as_uri())))
        runner=shutil.which('node');assert runner
    else:
        child=home/'update.py'
        child.write_text('''import os,sys
from pathlib import Path
assert sys.argv[1:]==['update','--restart-harbor']
home=Path(os.environ['HOME'])
if (home/'fail-update').exists():raise RuntimeError('fixture preparation failed')
target=(home/'office/dist/harbor-runtime/bin/harbor').resolve()
for record in (home/'.office/harbor-windows').iterdir():
    if record.suffix:continue
    request=Path(str(record)+'.restart');request.write_text('pcl.harbor.restart.v1\\t'+str(target)+'\\n');request.chmod(0o600)
''')
        runner=sys.executable
    # An old Desk command format must suffice; no new command row is required for Settings.
    commands.write_text('Quota\tProvider capacity\t'+runner+'\t'+str(child)+'\tdesk\t--view\tquota\n')
    def update_from_settings(t):
        t.send(b'\x0f')
        for _ in range(30):
            if 'Settings' in t.text:break
            t.send(b'\x1b[B',wait=.02)
        t.click_text('Settings')
        evidence=os.environ.get('HARBOR_RESTART_EVIDENCE')
        if evidence:
            out=Path(evidence);out.mkdir(parents=True,exist_ok=True)
            (out/'settings.txt').write_text(t.text)
            cells=[[{key:getattr(t.screen.buffer[y][x],key) for key in ('data','fg','bg','bold','reverse')} for x in range(t.cols)] for y in range(t.rows)]
            (out/'settings.json').write_text(json.dumps(cells))
        t.click_text('Update all windows')
    a=Terminal(config_dir=config,extra_env=env,executable=binary)
    b=Terminal(config_dir=config,extra_env=env,executable=binary)
    try:
        a.picker('Saved Messages'); a.send(b'\x1b[5~')
        note=re.search(r'Note \d+:',a.text).group()
        a.click_text(note,button=2);a.click_text('Reply');a.send('anchored draft')
        a.send(b'\x1b[D'*3)
        def history(t):
            reply=next(i for i,line in enumerate(t.screen.display) if 'Replying' in line)
            return [line.rstrip() for line in t.screen.display[4:reply]]
        before_history=history(a)
        before=[row.strip() for row in a.screen.display if 'Note ' in row]
        assert before and 'Note 35:' not in '\n'.join(before),a.text
        b.picker('Studio'); b.send('live draft')
        b.message_tool('Keep the details quiet','Edit')
        b.clear_draft();b.send('edited draft');b.expect('Editing your message')
        old_names=set((home/'.office/harbor-windows').iterdir())
        old_socket=(config/'session.sock').stat().st_ino
        failure=home/'fail-update';failure.touch()
        update_from_settings(a);a.expect('fixture preparation failed',timeout=5)
        a.expect('press any key to return to Harbor',timeout=5)
        assert set((home/'.office/harbor-windows').iterdir())==old_names
        assert (config/'session.sock').stat().st_ino==old_socket
        resumed=len(a.raw);a.send(b' ')
        end=time.monotonic()+5
        while b'\x1b]2;Saved Messages\x07' not in a.raw[resumed:] and time.monotonic()<end:a.pump(.05)
        assert b'\x1b]2;Saved Messages\x07' in a.raw[resumed:],'failed update did not return terminal ownership'
        a.expect('anchored draft');b.expect('edited draft')
        failure.unlink()
        a_start,b_start=len(a.raw),len(b.raw)
        update_from_settings(a)
        end=time.monotonic()+12
        while time.monotonic()<end:
            a.pump(.05);b.pump(.05)
            names=set((home/'.office/harbor-windows').iterdir())
            if len(names)==2 and not names.intersection(old_names):break
        else:raise AssertionError('both native registrations did not change after exec\nA:\n'+a.text+'\nB:\n'+b.text)
        assert (config/'session.sock').stat().st_ino!=old_socket,'old shared backend survived restart'
        end=time.monotonic()+5
        while time.monotonic()<end:
            a.pump(.05);b.pump(.05)
            if b'Saved Messages' in a.raw[a_start:] and b'Studio' in b.raw[b_start:]:break
        else:raise AssertionError('both updated chat views did not render')
        a.expect('anchored draft');a.expect('Replying');b.expect('edited draft');b.expect('Editing your message');b.expect('Studio')
        expected_version=subprocess.run([str(target_binary),'--version'],text=True,capture_output=True,check=True).stdout.split()[1]
        assert all(expected_version==record.read_text().split('\t')[2] for record in names)
        # A fresh backend may supply chat metadata/drafts before the anchored message page.
        end=time.monotonic()+5
        while time.monotonic()<end:
            a.pump(.05);b.pump(.05)
            after=[row.strip() for row in a.screen.display if 'Note ' in row]
            if after==before and history(a)==before_history:break
        else:raise AssertionError((before_history,history(a),a.text))
        assert a.screen.title=='Saved Messages' and b.screen.title=='Studio'
        a.send('!');a.expect('anchored dr!aft');b.send('!');b.expect('edited draft!')
        b.send(b'\x1b');b.expect('live draft')
        if module:assert json.loads((home/'publisher-result.json').read_text())=={'requested':2,'unsupported':0}
        print('PASS Settings preparation failure preserved both UIs; old Desk argv, two real native PTYs and replacement shared backend; independent chats, message/line position, draft cursor, reply/edit targets and pre-edit draft survived; both accept input',flush=True)
        # A reply/search/pin visit has a separate Return mark. Manual update keeps the visible
        # original and that mark; ordinary quit retains its existing return-before-visit policy.
        a.picker('Doug formatting');a.send('visit draft')
        def find_body(t,text):
            t.send(b'\x07')
            for _ in range(40):
                if any(line.strip().startswith(text) for line in t.screen.display):return
                t.send(b'\x1b[5~',wait=.05)
            raise AssertionError(('message body unavailable',text,t.text))
        find_body(a,'The code word');a.message_tool('The code word','Reveal spoiler')
        a.expect('The code word is harbor-42, keep it.')
        find_body(a,'That was the quote');a.message_tool('That was the quote','Expand quote')
        a.expect('second quoted line');a.send(b'\x07')
        def draft_history(t):
            prompt=next(i for i,line in enumerate(t.screen.display) if '› visit draft' in line)
            return [line.rstrip() for line in t.screen.display[3:prompt]]
        return_view=draft_history(a)
        a.send(b'\x0f');a.click_text('Search or pinned messages');a.click_text('Pinned messages')
        a.expect('Original plan',timeout=5);a.click_text('Original plan');a.expect('Showing the message',timeout=5)
        def original(t):
            return next(((i,line.rstrip()) for i,line in enumerate(t.screen.display) if line.strip().startswith('Original plan:')),None)
        end=time.monotonic()+5
        while original(a) is None and time.monotonic()<end:a.pump(.05)
        visited=original(a);assert visited,a.text
        fixture=str(ROOT/'tests/harbor/fixtures/harbor.png')
        b.send('\x1b[200~'+fixture+'\x1b[201~');b.expect('Preview attachments')
        old_names=set((home/'.office/harbor-windows').iterdir())
        b_name=next(path for path in old_names if path.name.startswith(str(b.pid)+'-'))
        old_socket=(config/'session.sock').stat().st_ino
        a_start,b_start=len(a.raw),len(b.raw)
        update_from_settings(a)
        end=time.monotonic()+5
        while time.monotonic()<end:
            a.pump(.05);b.pump(.05)
            if not any(path.exists() for path in old_names if path!=b_name):break
        else:raise AssertionError('initiating UI did not leave for the prepared version')
        assert b_name.exists() and 'Preview attachments' in b.text,'pending attachment was interrupted'
        b.click_text('Cancel')
        end=time.monotonic()+12
        while time.monotonic()<end:
            a.pump(.05);b.pump(.05)
            names=set((home/'.office/harbor-windows').iterdir())
            if len(names)==2 and not names.intersection(old_names) and original(a)==visited:break
        else:raise AssertionError(('visited position did not resume',visited,original(a),a.text,b.text))
        assert (config/'session.sock').stat().st_ino!=old_socket
        a.expect('visit draft');b.expect('live draft')
        a.send(b'\x0f');a.click_text('Return to previous position')
        end=time.monotonic()+5
        while time.monotonic()<end:
            a.pump(.05)
            if draft_history(a)==return_view:break
        else:raise AssertionError(('Return position changed',return_view,draft_history(a)))
        a.expect('visit draft')
        find_body(a,'The code word');a.expect('The code word is harbor-42, keep it.')
        find_body(a,'That was the quote');a.expect('second quoted line')
        print('PASS manual restart keeps a visited message at its line and Return action; quote expansion and spoiler reveal survive; pending attachment preview defers until cancelled without losing its draft',flush=True)
    finally:
        b.close();a.close()
        # Last-UI exit precedes backend cleanup. Wait for the backend's existing shutdown lock
        # after its socket is removed, so temporary state cannot race the backend's final writes.
        until=time.monotonic()+5
        while (config/'session.sock').exists() and time.monotonic()<until:time.sleep(.01)
        assert not (config/'session.sock').exists(),'owned demo backend did not stop'
        with (config/'session.lock').open('a') as lock:fcntl.flock(lock,fcntl.LOCK_EX)
