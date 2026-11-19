"""Safe Office update: pure production clock/state gates and one real two-minute native exit.

Only disposable --demo state is used. The test never overrides the production idle interval.
The sibling owns an independent draft on the same backend and must remain usable throughout.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from native_smoke import Terminal, ROOT

with tempfile.TemporaryDirectory(prefix='harbor-desk-update-', dir='/tmp') as directory:
    work = Path(directory)
    unit = work/'desk-update-test'
    subprocess.run(['c++', '-std=c++17', '-I' + str(ROOT/'src/harbor'),
                    str(ROOT/'tests/harbor/desk_update_test.cpp'), '-o', str(unit)], check=True)
    subprocess.run([str(unit), str(work)], check=True)
    config = work/'config'
    request = work/'update'
    sentinel = work/'sentinel'
    sentinel.write_text('keep')
    env = {'HOME': str(work/'home'), 'XDG_CONFIG_HOME': str(work/'xdg'),
           'HARBOR_DESK_STRIP_FILE': '', 'HARBOR_DESK_UPDATE_FILE': '', 'CMUX_SURFACE_ID': ''}
    surface = 'surface:owned-update-fixture'
    opted = dict(env, CMUX_SURFACE_ID=surface, HARBOR_DESK_UPDATE_FILE=str(request))
    stable = config/('harbor-state.surface-' + surface.encode().hex().upper())
    revision = 'a'*40
    wanted = f'pcl.desk.update.v1\t{os.getpid()}\t{revision}\n'
    ack = wanted.replace('pcl.desk.update.v1', 'pcl.desk.updated.v1')
    def write_request(data=wanted):
        temp = request.with_suffix('.next')
        temp.write_text(data)
        temp.chmod(0o600)
        temp.replace(request)
    def saved(t):
        t.pump(2.2)
        return stable.read_text()
    def active(state):
        line = next(line for line in state.splitlines() if line.startswith('active '))
        return bytes.fromhex(line[7:]).decode()
    windows = []
    reaped = False
    a = None
    try:
        # Migrate only the numeric slot held by this UI, without writing back to it.
        legacy = Terminal(config_dir=config, extra_env=env); windows.append(legacy)
        legacy.picker('Studio'); legacy.send('migration draft'); legacy.close(); windows.remove(legacy)
        numeric = (config/'harbor-state').read_bytes()
        a = Terminal(config_dir=config, extra_env=opted); windows.append(a)
        a.expect('migration draft'); assert a.screen.title == 'Studio', a.text
        migrated = saved(a)
        assert stable.exists() and (config/'harbor-state').read_bytes() == numeric
        assert active(migrated).endswith('\tstudio'), migrated
        write_request()
        a.send(b'\x1b[200~ still drafting\x1b[201~'); a.pump(.5)
        assert request.read_text() == wanted and 'still drafting' in a.text
        # A normal user quit never acknowledges the request, including an outstanding draft.
        a.close(); windows.remove(a); a = None
        assert request.read_text() == wanted, request.read_text()
        a = Terminal(config_dir=config, extra_env=opted); windows.append(a)
        a.expect('migration draft still drafting'); assert a.screen.title == 'Studio', a.text
        a.send(b'\x15'); a.send(b'\x07')  # empty draft and explicit latest; these are real input
        last_input_at = time.monotonic()
        b = Terminal(config_dir=config, extra_env=env); windows.append(b)
        b.picker('Marina'); b.send('independent sibling draft')
        write_request('pcl.desk.update.v1\t1\t' + revision + '\n')
        a.pump(.4); assert request.read_text().startswith('pcl.desk.update.v1\t1\t')
        write_request()
        # This is the real public interval, not a shortened fixture clock. No input is sent to a.
        end = last_input_at + 128
        while time.monotonic() < end:
            a.pump(.1); b.pump(.02)
            pid, status = os.waitpid(a.pid, os.WNOHANG)
            if pid:
                reaped = True
                assert os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, status
                break
        assert reaped, 'safe native UI did not exit after the production idle interval'
        elapsed = time.monotonic() - last_input_at
        assert elapsed >= 119.8, elapsed
        assert request.read_text() == ack, request.read_text()
        persisted = stable.read_text()
        assert active(persisted).endswith('\tstudio'), persisted
        assert 'independent sibling draft' in b.text and b.screen.title == 'Marina', b.text
        os.close(a.master); windows.remove(a); a = None
        # Occupy the freed numeric slot before reopening the same surface. Its stable state wins.
        c = Terminal(config_dir=config, extra_env=env); windows.append(c)
        c.picker('Saved Messages'); c.send('different numeric slot state'); c.pump(2.2)
        resumed = Terminal(config_dir=config, extra_env=opted); windows.append(resumed)
        resumed.expect('Studio')
        assert resumed.screen.title == 'Studio', resumed.text
        assert 'different numeric slot state' not in resumed.text and 'migration draft' not in resumed.text
        b.send('!'); b.expect('independent sibling draft!')
        assert sentinel.read_text() == 'keep'
        if os.environ.get('HARBOR_STRIP_EVIDENCE'):
            out = Path(os.environ['HARBOR_STRIP_EVIDENCE'])/'safe-update.json'
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(json.dumps({'elapsedIdleSeconds': elapsed, 'ack': ack,
                'stableState': persisted, 'numericSlotMigratedReadOnly': True,
                'resumedTitleAfterNumericSlotOccupied': resumed.screen.title,
                'siblingTitle': b.screen.title, 'siblingDraftKept': True}, indent=2) + '\n')
        print('PASS real 120000ms safe update: draft blocks request acknowledgment; user quit leaves request; '
              'exact atomic ack and normal UI-only exit; selected chat resumes despite occupied numeric slot; '
              'shared-backend sibling and draft survive', flush=True)
    finally:
        for window in reversed(windows):
            if window is a and reaped:
                os.close(window.master)
            else:
                window.close()
        assert sentinel.read_text() == 'keep'
