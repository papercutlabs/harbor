"""Short history batches, bounded picker, and opt-in restart into an installed release."""
import os
from pathlib import Path
import shutil
import tempfile
from native_smoke import Terminal, ROOT

t = Terminal(cols=50, rows=50, extra_env={'HARBOR_DEMO_PARTIAL_HISTORY':'1'})
try:
    t.picker('Studio')
    t.expect('new layout is ready')
    t.expect('conversation should feel calm')
    assert 'Keep the details quiet' in t.text
    t.picker('Marina'); t.picker('Studio'); t.expect('new layout is ready')
    t.send(b'\x0b')
    t.expect('Find a conversation')
    ys=[y for y,row in enumerate(t.screen.display) if 'Find a conversation' in row or 'Esc close' in row]
    assert max(ys)-min(ys) < 18
    search=next(y for y,row in enumerate(t.screen.display) if 'search…' in row)
    chosen=next(y for y,row in enumerate(t.screen.display) if '› ' in row and 'search…' not in row)
    assert chosen >= search + 2
    assert not any(row.strip() == 'unread' for row in t.screen.display)
    t.snapshot('harbor-compact-picker-50')
    print('PASS initial short batch automatically loads earlier history; bounded and separated chat picker')
finally:
    t.close()

with tempfile.TemporaryDirectory(prefix='harbor-update-test-') as directory:
    root=Path(directory)
    release=root/'releases/test-build'
    (release/'bin').mkdir(parents=True)
    (release/'share/harbor').mkdir(parents=True)
    (release/'share/harbor/version').write_text('9.0.0\n')
    executable=Path(os.environ.get('HARBOR_EXECUTABLE',ROOT/'build-harbor/bin/harbor'))
    shutil.copy2(executable,release/'bin/harbor')
    shutil.copytree(executable.resolve().parent.parent/'lib',release/'lib')
    launcher=root/'harbor'
    launcher.symlink_to(executable.resolve())
    t=Terminal(cols=56,rows=38,extra_env={'HARBOR_TEST_UPDATE_LAUNCHER':str(launcher)})
    try:
        t.picker('Studio'); t.send('Draft survives the update')
        assert 'Harbor 9.0.0 ready' not in t.text
        newer = root/'next-harbor'
        newer.symlink_to(release/'bin/harbor')
        os.replace(newer,launcher)
        t.expect('Harbor 9.0.0 ready',timeout=5)
        t.click_text('Harbor 9.0.0 ready');t.expect('Update ready')
        assert 'Restart into' not in t.text and 'Your draft and reading' not in t.text
        assert 'Keep using this version' not in t.text
        t.click_text('Later');t.expect('Draft survives the update')
        t.click_text('Harbor 9.0.0 ready');t.click_text('Restart')
        t.pump(1.5)
        assert 'unable to acquire lock' not in t.text
        assert 'Could not restart' not in t.text
        # /proc is unavailable on macOS; the visible banner disappears because
        # the new process resolves its own executable to the selected release.
        assert 'Harbor 9.0.0 ready' not in t.text
        t.picker('Studio');t.expect('Draft survives the update')
        assert os.waitpid(t.pid,os.WNOHANG)[0] == 0
        print('PASS update suggestion, Later, actual exec restart with same PID/config, and draft restoration')
    finally:
        t.close()
