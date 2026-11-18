"""Two independent Harbor windows on one shared offline session, through real pseudoterminals.

Every window runs `harbor --demo -d <throwaway>`; HOME and XDG_CONFIG_HOME point into the same
throwaway directory so no real profile can be selected. A sentinel file outside the config
directory must survive every step, including failure cleanup.
"""
import os
import json
import signal
import shutil
import struct
import sys
import termios
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from native_smoke import Terminal


def backends(config):
    out = subprocess.run(['ps', '-eo', 'pid=,args='], capture_output=True, text=True).stdout
    return [line for line in out.splitlines() if '--session-backend' in line and str(config) in line]


def wait(predicate, what, timeout=8):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(.05)
    raise AssertionError(f'timed out: {what}')


def orderly_pending_checks(root):
    # Freeze + save crosses the real Ui::Run/Cleanup boundary. A final failure can
    # be either not received yet or already removed from Connection's pending map
    # and queued behind mouse deferral. Each case owns a backend and disposable home.
    for mode in ('close', 'signal', 'restart', 'queued-final'):
        case = root / ('pending-' + mode)
        case.mkdir()
        config, log, trace = case / 'config', case / 'requests.log', case / 'origin-events.log'
        caption = 'unknown caption ' + mode + ' 你好'
        fixture = Path(__file__).resolve().parent / 'fixtures/harbor.png'
        env = {'HOME': str(case / 'home'), 'XDG_CONFIG_HOME': str(case / 'xdg'),
               'HARBOR_DEMO_REQUEST_LOG': str(log), 'HARBOR_DEMO_SEND_DELAY_MS': '10000',
               'HARBOR_DEMO_FAIL_SEND_INDEX': '1'}
        if mode == 'restart':
            native_version = subprocess.check_output([os.environ['HARBOR_EXECUTABLE'], '--version'],
                                                     env=dict(os.environ, **env), text=True).split()[1]
            major, minor, patch = map(int, native_version.split('.'))
            target = case / f'releases/{major}.{minor}.{patch + 1}/bin/harbor'
            target.parent.mkdir(parents=True)
            shutil.copy2(os.environ['HARBOR_EXECUTABLE'], target)
            env['HARBOR_TEST_UPDATE_LAUNCHER'] = str(target)
        windows = []
        try:
            a = Terminal(config_dir=config, extra_env=dict(env, HARBOR_TEST_EVENT_LOG=str(trace)))
            windows.append(a)
            b = Terminal(config_dir=config, extra_env=env); windows.append(b)
            backend = backends(config)
            assert len(backend) == 1, backend
            a.picker('Marina')
            a.click_text('Keeping this one', button=2); a.click_text('Reply')
            a.send(caption)
            a.send('\x1b[200~' + str(fixture) + '\x1b[201~', .3)
            a.expect('Preview attachments'); a.click_text('Send 1 attachment')
            wait(lambda: log.exists() and 'send chat=' in log.read_text(), 'original request')
            assert 'Send failed' not in a.text, a.text
            if mode == 'queued-final':
                a.send(b'\x1b[<0;20;10M') # defer event application, keep reader active
                wait(lambda: 'defer events: mouse down' in trace.read_text(), 'UI defers events')
                wait(lambda: ' pending=0 success=0' in trace.read_text(), 'final failure queued to UI', 15)
                assert 'Send failed' not in a.text, a.text
            if mode == 'restart':
                a.send(b'\x0f'); a.expect('Commands & shortcuts')
                a.click_text('Update Harbor'); a.expect('Restart')
                a.click_text('Restart')
                wait(lambda: str(target) in subprocess.run(['ps', '-p', str(a.pid), '-o', 'args='],
                     capture_output=True, text=True).stdout, 'same UI process execs replacement')
                a.expect(caption)
            else:
                if mode == 'signal': os.kill(a.pid, signal.SIGTERM)
                a.close(); windows.remove(a)
            state = (config / 'harbor-state').read_text()
            recovery = [bytes.fromhex(row.split()[2]) for row in state.splitlines() if row.startswith('recover ')]
            assert len(recovery) == 1, (mode, state)
            requests = [row for row in log.read_text().splitlines() if row.startswith('send ')]
            assert len(requests) == 1, requests
            original = bytes.fromhex(requests[0].split(' payload=')[1])
            assert recovery == [original], 'full original payload changed at final save'
            # Verify that the equality check carries body, quote and actual attachment bytes.
            fields, offset = [], 0
            for _ in range(12):
                size = struct.unpack_from('<I', original, offset)[0]; offset += 4
                fields.append(original[offset:offset + size]); offset += size
            assert fields[2] == caption.encode() and fields[3], fields
            assert bytes.fromhex(fields[9].decode().split(',')[2]) == str(fixture).encode(), fields[9]
            assert backends(config) == backend, backends(config)
            if mode != 'restart':
                a = Terminal(config_dir=config, extra_env=env); windows.append(a)
                a.expect(caption) # slot zero was freed and recovered; never replay
            request_id = requests[0].split(' request=')[1].split()[0]
            wait(lambda: 'emit send request=' + request_id + ' success=0' in log.read_text(),
                 'late final failure after origin departure', 15)
            a.pump(.3); b.pump(.3)
            assert 'Send failed' not in a.text and 'Send failed' not in b.text, (a.text, b.text)
            assert sum(row.startswith('send ') for row in log.read_text().splitlines()) == 1
            # Final failure was addressed to a departed connection, never its slot replacement.
            a.close(); windows.remove(a)
            saved = (config / 'harbor-state').read_text()
            assert [bytes.fromhex(row.split()[2]) for row in saved.splitlines()
                    if row.startswith('recover ')] == [original]
            b.picker('Studio'); b.expect('Studio') # survivor remains usable after the late failure
            b.send('survivor after ' + mode); b.send(bytes([13])); b.expect('survivor after ' + mode)
            wait(lambda: sum(row.startswith('send ') for row in log.read_text().splitlines()) == 2,
                 'survivor still sends through same backend')
            if os.environ.get('HARBOR_EVIDENCE_DIR'):
                evidence = Path(os.environ['HARBOR_EVIDENCE_DIR']) / 'pending-close-evidence.json'
                records = json.loads(evidence.read_text()) if evidence.exists() else {}
                records[mode] = {'originalPayload': original.hex(), 'savedPayload': recovery[0].hex(),
                                 'originalRequestId': request_id, 'originalOutboundCount': 1,
                                 'totalAfterSurvivorSend': 2, 'backend': backend,
                                 'originEvents': trace.read_text()}
                evidence.write_text(json.dumps(records, indent=2) + chr(10))
            print('PASS ' + mode + ': full body/quote/attachment recovery before final save; '
                  'slot restart, late failure isolation, one original outbound request, survivor healthy', flush=True)
        finally:
            for window in windows: window.close()
            wait(lambda: not backends(config), 'owned backend exits before fixture cleanup')


if os.environ.get('HARBOR_TEST_PENDING_CLOSE_ONLY'):
    with tempfile.TemporaryDirectory(prefix='harbor-close-pending-', dir='/tmp') as directory:
        orderly_pending_checks(Path(directory))
    sys.exit(0)


with tempfile.TemporaryDirectory(prefix='harbor-multiwindow-', dir='/tmp') as directory:
    root = Path(directory)
    config = root / 'config'
    log = root / 'requests.log'
    sentinel = root / 'sentinel'
    sentinel.write_text('keep')
    env = {'HOME': str(root / 'home'), 'XDG_CONFIG_HOME': str(root / 'xdg'),
           'HARBOR_DEMO_REQUEST_LOG': str(log), 'HARBOR_DEMO_FAIL_SEND_INDEX': '2'}
    windows = []
    try:
        # Explicit -d owns selection, even with conflicting inherited defaults; unknown
        # user content must be refused without replacement or deletion.
        unknown_format = root / 'unknown'
        unknown_format.mkdir()
        (unknown_format / 'keep').write_text('existing user content')
        exe = os.environ['HARBOR_EXECUTABLE']
        refused = subprocess.run([exe, '--demo', '-d', str(unknown_format)],
                                 env=dict(os.environ, **env), capture_output=True, text=True, timeout=8)
        assert refused.returncode != 0 and 'unknown format' in refused.stderr, refused
        assert (unknown_format / 'keep').read_text() == 'existing user content'
        assert not (unknown_format / 'version').exists()
        print('PASS unknown configuration preserved; explicit selection stays isolated')

        # Simultaneous startup: one backend.
        started = [None, None]
        def start(i):
            started[i] = Terminal(config_dir=config, extra_env=env)
        threads = [threading.Thread(target=start, args=(i,)) for i in range(2)]
        for t in threads: t.start()
        for t in threads: t.join()
        a, b = started
        assert a and b, 'a window failed to start'
        windows += [a, b]
        assert len(backends(config)) == 1, backends(config)
        print('PASS simultaneous start: one backend')

        # Independent navigation.
        a.picker('Saved Messages'); a.expect('Note 35:')
        b.picker('Studio')
        a.pump(.4)
        assert 'Saved Messages' in a.text and 'Note 35:' in a.text, a.text
        print('PASS independent chats')

        # Same text from both windows; the session fails only the second send.
        a.picker('Marina'); b.picker('Marina')
        a.send('ok'); a.send(b'\r'); a.expect('Sent')
        b.send('ok'); b.send(b'\r'); b.expect('Send failed')
        a.pump(.4)
        assert 'Send failed' not in a.text, a.text
        sends = [line for line in log.read_text().splitlines() if line.startswith('send ')]
        assert len(sends) == 2 and len({line.split(' view=')[1].split()[0] for line in sends}) == 2, sends
        print('PASS same-text ownership: only the failing window restores its draft')

        # A send from B appears in A.
        b.send('from window b'); b.send(b'\r'); b.expect('Sent')
        a.expect('from window b')
        print('PASS shared live updates')

        # Distinct drafts persist per slot.
        a.send('draft in a', .3); b.send('draft in b', .3)
        a.close(); windows.remove(a)
        b.close(); windows.remove(b)
        wait(lambda: not backends(config), 'backend exit after last window')
        assert not (config / 'session.sock').exists()
        states = {p.name for p in config.glob('harbor-state*') if not p.name.endswith('.lock')}
        assert {'harbor-state', 'harbor-state.1'} <= states, states
        print('PASS last window closes the backend; per-view state slots written')

        env.pop('HARBOR_DEMO_FAIL_SEND_INDEX')
        a = Terminal(config_dir=config, extra_env=env); windows.append(a)
        b = Terminal(config_dir=config, extra_env=env); windows.append(b)
        a.pump(.6); b.pump(.6)
        # Slots are taken lowest-free first, so either window may own either slot; each draft
        # must come back in exactly one window.
        for draft in ('draft in a', 'draft in b'):
            assert (draft in a.text) != (draft in b.text), (draft, a.text, b.text)
        print('PASS drafts restored per slot after restart')

        # Close one window: the survivor keeps sending.
        a.close(); windows.remove(a)
        b.send(' survivor'); b.send(b'\r'); b.expect('Sent')
        assert len(backends(config)) == 1
        print('PASS survivor sends after the other window closes')

        # SIGKILL of an ordinary view releases its slot; a connected survivor still sends.
        a = Terminal(config_dir=config, extra_env=env); windows.append(a)
        os.kill(a.pid, signal.SIGKILL); os.waitpid(a.pid, 0)
        os.close(a.master); windows.remove(a)
        b.send(' after peer kill'); b.send(b'\r'); b.expect('Sent')
        assert len(backends(config)) == 1
        print('PASS SIGKILL of one view leaves the other usable')

        # Backend loss: the window leaves ncurses with a session-ended message and no replay.
        pid = int(backends(config)[0].split()[0])
        os.kill(pid, 9)
        end = time.monotonic() + 8
        while time.monotonic() < end and 'session ended' not in bytes(b.raw).decode('utf-8', 'replace'):
            b.pump(.1)
        assert 'session ended' in bytes(b.raw).decode('utf-8', 'replace')
        b.close(); windows.remove(b)
        print('PASS backend loss ends the window cleanly')

        # Sign-in through terminal lending: the backend prompts on the lending window's terminal
        # and returns it afterwards. Fixed nonsecret demo code only.
        auth_config = root / 'auth-config'
        auth_env = dict(env, HARBOR_DEMO_AUTH='1')
        auth_env.pop('HARBOR_DEMO_FAIL_SEND_INDEX', None)
        a = Terminal(config_dir=auth_config, extra_env=auth_env); windows.append(a)
        a.expect('Enter authentication code', timeout=8)
        a.send('12345\r', .5)
        a.expect('Ctrl+/ Commands', timeout=8)
        wait(lambda: 'auth read=1 accepted=1' in log.read_text(), 'auth line read by backend')
        pid = int(backends(auth_config)[0].split()[0])
        if Path(f'/proc/{pid}/fd/0').exists():
            assert os.readlink(f'/proc/{pid}/fd/0') == '/dev/null'
            assert os.readlink(f'/proc/{pid}/fd/1') == '/dev/null'
        if sys.platform == 'darwin':
            detached = subprocess.run(['/usr/sbin/lsof', '-a', '-p', str(pid), '-d', '0,1', '-Fn'],
                                      capture_output=True, text=True, timeout=8)
            assert detached.returncode == 0 and detached.stdout.count('n/dev/null') == 2, detached
        a.picker('Marina'); a.send('after sign-in'); a.send(b'\r'); a.expect('Sent')
        a.close(); windows.remove(a)
        print('PASS sign-in prompt on the lending terminal, terminal returned')
        # The supported process signal handler also reaches the backend loop and saves views.
        signal_config = root / 'sig'
        a = Terminal(config_dir=signal_config, extra_env=env); windows.append(a)
        pid = int(backends(signal_config)[0].split()[0]); os.kill(pid, signal.SIGTERM)
        end = time.monotonic() + 8
        while time.monotonic() < end and b'session stopped' not in a.raw: a.pump(.1)
        assert b'session stopped' in a.raw, bytes(a.raw)
        wait(lambda: not backends(signal_config), 'backend responds to SIGTERM')
        a.close(); windows.remove(a)
        print('PASS backend SIGTERM saves views and releases native ownership')

        # Kill the lender while getline is waiting. Keep its PTY master open: terminal
        # hangup cannot accidentally supply the EOF this test requires from cancellation.
        death_config = root / 'auth-death'
        death_log = root / 'auth-death.log'
        death_env = dict(auth_env, HARBOR_DEMO_REQUEST_LOG=str(death_log), HARBOR_DEMO_AUTH_PASSWORD='1')
        a = Terminal(config_dir=death_config, extra_env=death_env); windows.append(a)
        a.expect('Enter authentication code', timeout=8)
        b = Terminal(config_dir=death_config, extra_env=death_env); windows.append(b)
        pid = int(backends(death_config)[0].split()[0])
        evidence = os.environ.get('HARBOR_EVIDENCE_DIR')
        if sys.platform == 'darwin' and evidence:
            sample = Path(evidence) / 'blocked-auth.sample.txt'
            sampled = subprocess.run(['/usr/bin/sample', str(pid), '1', '1', '-file', str(sample)],
                                     capture_output=True, text=True, timeout=15)
            assert sampled.returncode == 0, sampled.stderr
            trace = sample.read_text()
            assert 'underflow' in trace and 'poll' in trace, trace
        assert not (termios.tcgetattr(a.master)[3] & termios.ECHO), 'native password read did not disable echo'
        os.kill(a.pid, signal.SIGKILL); os.waitpid(a.pid, 0)
        windows.remove(a)
        try:
            wait(lambda: death_log.exists() and 'auth read=0 accepted=0' in death_log.read_text(),
                 'blocked native getline cancelled on lender SIGKILL')
            wait(lambda: not backends(death_config), 'last clients close after sign-in owner dies')
            end = time.monotonic() + 8
            while time.monotonic() < end and b"sign-in terminal closed" not in b.raw:
                b.pump(.1)
            assert b"sign-in terminal closed" in b.raw, bytes(b.raw)
        finally:
            os.close(a.master)
        b.close(); windows.remove(b)
        print('PASS blocked native auth read cancelled on lender SIGKILL with PTY master still open; survivor receives exact session failure')

        orderly_pending_checks(root)

        # Lose the backend after local acceptance but before two final send outcomes.
        unknown_config = root / 'unknown-config'
        unknown_log = root / 'unknown.log'
        unknown_env = dict(env, HARBOR_DEMO_REQUEST_LOG=str(unknown_log), HARBOR_DEMO_SEND_DELAY_MS='20000')
        unknown_env.pop('HARBOR_DEMO_FAIL_SEND_INDEX', None)
        a = Terminal(config_dir=unknown_config, extra_env=unknown_env); windows.append(a)
        a.picker('Marina')
        for text in ('unknown one', 'unknown two'):
            a.send(text); a.send(b'\r')
        wait(lambda: unknown_log.exists() and len([line for line in unknown_log.read_text().splitlines()
                                                  if line.startswith('send ')]) == 2, 'two locally accepted sends')
        pid = int(backends(unknown_config)[0].split()[0]); os.kill(pid, signal.SIGKILL)
        def ended():
            a.pump(.1)
            return b'session ended' in a.raw
        wait(ended, 'window leaves after backend loss')
        a.close(); windows.remove(a)
        state = (unknown_config / 'harbor-state').read_text()
        recoveries = [bytes.fromhex(line.split()[2]) for line in state.splitlines() if line.startswith('recover ')]
        assert len(recoveries) == 2 and any(b'unknown one' in value for value in recoveries) and any(b'unknown two' in value for value in recoveries), state
        a = Terminal(config_dir=unknown_config, extra_env=unknown_env); windows.append(a)
        a.expect('unknown one')
        assert len([line for line in unknown_log.read_text().splitlines() if line.startswith('send ')]) == 2
        a.close(); windows.remove(a)
        print('PASS backend EOF preserves both unknown locally accepted sends across restart without replay')
    finally:
        for w in windows:
            try: w.close()
            except Exception: pass
        assert sentinel.read_text() == 'keep'
print('PASS multiwindow')
