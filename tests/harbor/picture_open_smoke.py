"""Visible-photo auto download, delayed single-click Preview, separate save, and Focus event.

Real native PTY/render/input/request/control paths; offline Telegram fixture only.
A private recording opener replaces the final OS dispatch. No account or app opens.
"""
from native_smoke import Terminal
from pathlib import Path
import json, os, subprocess, tempfile, time

def check(mode):
    with tempfile.TemporaryDirectory(prefix='harbor-picture-', dir='/tmp') as directory:
        root=Path(directory);home=root/'home';home.mkdir();config=root/'config';config.mkdir()
        downloads=root/'Downloads';downloads.mkdir()
        (config/'version').write_text('31')
        (config/'app.conf').write_text('downloads_dir='+str(downloads)+'\n')
        requests=root/'requests';opened=root/'opened';opener=root/'open.sh'
        opener.write_text('#!/bin/sh\nprintf "%s|%s\\n" "$1" "$2" >> "'+str(opened)+'"\n');opener.chmod(0o700)
        env=dict(HOME=str(home),XDG_CONFIG_HOME=str(home/'xdg'),HARBOR_DEMO_PICTURES=mode,
                 HARBOR_DEMO_PICTURE_DELAY_MS='3000',HARBOR_DEMO_REQUEST_LOG=str(requests),
                 HARBOR_TEST_OPENER=str(opener),CMUX_SURFACE_ID='PICTURE-TEST')
        t=Terminal(cols=56,rows=30,config_dir=config,extra_env=env)
        def log():return requests.read_text() if requests.exists() else ''
        def opens():return opened.read_text().splitlines() if opened.exists() else []
        samples=[]
        def inspect(window=False):
            child=subprocess.Popen([os.environ['HARBOR_EXECUTABLE'],'-d',str(config),'--target-surface','PICTURE-TEST','--target-pid',str(t.pid),'--inspect-window' if window else '--inspect-ui'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=dict(os.environ,**env))
            until=time.monotonic()+8
            while child.poll() is None and time.monotonic()<until:t.pump(.01)
            if child.poll() is None:child.kill();child.wait();raise AssertionError('inspect timed out')
            out,err=child.communicate();assert child.returncode==0,err
            samples.append(out)
            fields=out.rstrip('\n').split('\t');assert len(fields)==(10 if window else 9) and fields[0]==('pcl.harbor.window.v1' if window else 'pcl.harbor.ui.v2'),fields
            return fields
        try:
            if 'photo-shown' not in t.text:t.picker('Studio')
            t.expect('photo-shown');t.send('draft stays');t.expect('downloading')
            assert 'picture-download photo-shown action=0' in log(),log()
            assert 'photo-offscreen' not in log(),log()+'\n'+t.text
            assert not opens() and not list(downloads.iterdir()),'automatic download opened or saved'
            before_open=inspect();assert before_open[8]=='0'
            y=next(y for y,line in enumerate(t.screen.display) if '[Photo]' in line)
            # Thumbnail rows are reserved even while its original download finishes.
            t.send(f'\x1b[<0;9;{y+2}M',wait=.03);t.send(f'\x1b[<0;9;{y+2}m',wait=.03)
            t.expect('Opening picture when download completes')
            deadline=time.monotonic()+6
            while not opens() and time.monotonic()<deadline:t.pump(.1)
            assert len(opens())==1 and opens()[0].endswith('harbor.png|preview'),opens()
            assert log().count('picture-download photo-shown')==1,log()
            assert not list(downloads.iterdir())
            first=inspect();assert int(first[8])>0 and first[6]=='1',first
            assert inspect(True)[9]=='external-preview'
            t.expect('draft stays')
            t.send(b'\x1b[I');late_focus=inspect()
            assert late_focus[8]==first[8],late_focus
            t.send(b'\x05');after=inspect()
            assert after[8]=='0' and first[5]!=after[5],after
            assert inspect(True)[9]=='conversation'
            assert 'draft stays' in t.text and 'Message actions' not in t.text
            # A downloaded filename is still the explicit Save action.
            t.click_text('harbor.png');t.expect('Saved to ')
            assert len(opens())==1 and (downloads/'harbor.png').is_file()
            # Clicking the same picture again produces a new Focus event.
            y=next(y for y,line in enumerate(t.screen.display) if 'harbor.png' in line)
            t.send(f'\x1b[<0;9;{y+2}M',wait=.03);t.send(f'\x1b[<0;9;{y+2}m',wait=.1)
            second=inspect();assert int(second[8])>int(first[8]),second
            t.send('!');assert inspect()[8]=='0'
            assert 'draft stays!' in t.text and len(opens())==2
            print('PREVIEW_NATIVE_SAMPLES '+json.dumps({'mode':mode,'raw':samples}),flush=True)
            print('PASS visible largest-photo metadata path auto-downloads once; offscreen/chat scope; pending-click Preview; no auto-open/save; explicit save; fresh Focus token and late focus-in ignored; deliberate input return; draft preserved')
        finally:
            t.close()
            t.wait_backend_shutdown()

for mode in ("1", "legacy"):check(mode)
