"""Private native startup/operation trace, using disposable accounts and real PTYs only.

Every native child receives an explicit HOME, XDG config/state home and --demo -d path.
A conflicting inherited config path and outside sentinel must survive success/failure cleanup.
No installed Harbor, real account, authentication flow or provider is selected.
"""
import errno
import fcntl
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import time
from native_smoke import Terminal

binary = Path(os.environ['HARBOR_EXECUTABLE']).resolve()
probe = Path(os.environ.get('HARBOR_TRACE_PROBE', binary.with_name('harbor-trace-probe'))).resolve()
startup = '1b9cc1a4-428d-4f51-a810-b411defab111'
secret = 'SECRET_MESSAGE_AUTHCODE_TOKEN_937411'


def wait(predicate, description):
    end = time.monotonic() + 6
    while time.monotonic() < end:
        if predicate(): return
        time.sleep(.03)
    raise AssertionError(description)


with tempfile.TemporaryDirectory(prefix='hbt-') as tmp:
    root = Path(tmp)
    home, state, config = root/'home', root/'state', root/'config'
    home.mkdir(); state.mkdir()
    unrelated = root/'unrelated'; unrelated.mkdir()
    sentinel = unrelated/'keep'; sentinel.write_text('unchanged')
    env = dict(os.environ, HOME=str(home), XDG_CONFIG_HOME=str(unrelated), XDG_STATE_HOME=str(state),
               OFFICE_DESK_STARTUP_ID=startup, SECRET_FIXTURE=secret, TERM='xterm-256color', LANG='en_US.UTF-8')
    trace = state/'harbor/operational.jsonl'

    def rows():
        return [json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []

    def call(args, **kwargs):
        return subprocess.run([str(binary), *args], env=env, capture_output=True, text=True, timeout=8, **kwargs)

    def events(name): return [r for r in rows() if r['event'] == name]

    # Early return precedes config directory and ordinary logging initialisation.
    result = call(['--version', secret])
    assert result.returncode == 0
    assert events('process.entry') and events('process.exit')[-1]['status'] == '0'
    assert all(r['startup_id'] == startup for r in rows())
    assert not (unrelated/'harbor').exists()
    assert trace.stat().st_mode & 0o777 == 0o600

    # Actual OS input captured by the real backend distinguishes a busy directory from access/path failure.
    config.mkdir()
    locked = os.open(config, os.O_RDONLY)
    try:
        fcntl.flock(locked, fcntl.LOCK_EX | fcntl.LOCK_NB)
        result = call(['--demo', '-d', str(config)])
        assert result.returncode == 1, result
        busy = events('directory.lock.failed')[-1]
        assert int(busy['errno']) in (errno.EWOULDBLOCK, errno.EAGAIN) and busy['operation'] == 'flock', busy
        assert busy['diagnostic'] in result.stderr, (busy, result.stderr)
    finally:
        os.close(locked)
    blocked = root/'file'; blocked.write_text('keep')
    result = call(['--demo', '-s', '-d', str(blocked/'child')])
    access = events('directory.lock.failed')[-1]
    assert result.returncode == 1 and int(access['errno']) == errno.ENOTDIR and access['operation'] == 'open', (result, access)
    assert access['diagnostic'] in result.stderr and 'another Harbor' not in result.stderr
    assert blocked.read_text() == 'keep'
    inaccessible = root/'inaccessible'; inaccessible.mkdir(); inaccessible.chmod(0)
    try:
        result = call(['--demo', '-s', '-d', str(inaccessible)])
        denied = events('directory.lock.failed')[-1]
        assert result.returncode == 1 and int(denied['errno']) == errno.EACCES and denied['operation'] == 'open', (result, denied)
        assert denied['diagnostic'] in result.stderr
    finally: inaccessible.chmod(0o700)

    # The actual Attach path reaches fork + failed exec, retaining the original OS reason.
    failed = root/'failed'; failed.mkdir()
    result = subprocess.run([str(probe), 'spawn', str(failed)], env=env, capture_output=True, timeout=8)
    assert result.returncode == 0, result
    assert int(events('backend.exec.failed')[-1]['errno']) == errno.ENOENT
    assert events('handshake.failed')[-1]['cause'] == 'frame_or_eof'

    windows = []
    try:
        a = Terminal(config_dir=config, extra_env=env); windows.append(a)
        b = Terminal(config_dir=config, extra_env=env); windows.append(b)
        a.picker('Marina')
        a.send(secret); a.send('\r')
        wait(lambda: any(r.get('outcome') == 'success' and r.get('request_id') != '0' for r in events('request.result.received')), 'send result absent')
        attached = events('handshake.ready')[-2:]
        assert len({r['connection_id'] for r in attached}) == 2, attached
        assert events('socket.connected') and events('backend.ready')
        assert any(r.get('profile_index') == '0' for r in events('request.backend.received'))
        produced = [r for r in events('request.result.produced') if r.get('outcome') == 'success' and r.get('request_id') != '0']
        received = [r for r in events('request.result.received') if r.get('outcome') == 'success' and r.get('request_id') != '0']
        assert any((p['request_id'], p['connection_id'], p['type']) == (r['request_id'], r['connection_id'], r['type']) for p in produced for r in received)
        os.kill(b.pid, signal.SIGTERM)
        b.close(); windows.remove(b)
        a.close(); windows.remove(a)
        wait(lambda: not (config/'session.sock').exists(), 'backend must finish after last owned window')
        assert events('backend.shutdown') and events('process.termination')
        assert events('ui.commands.open') or events('ui.chat_picker.open')
    finally:
        for window in windows: window.close()

    # Uncaught failures keep their normal signal outcome; exception bodies are never retained.
    def no_core(): resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    for mode, expected, event in [('exception', signal.SIGABRT, 'process.uncaught_exception'),
                                  ('signal', signal.SIGSEGV, 'process.signal')]:
        result = subprocess.run([str(probe), mode, str(root)], env=env, capture_output=True, timeout=8, preexec_fn=no_core)
        assert result.returncode == -expected, (mode, result)
        assert events(event), mode

    retained = trace.read_text()
    assert secret not in retained and 'SECRET_EXCEPTION_BODY' not in retained
    assert 'chatMessage' not in retained and 'quotedText' not in retained and 'SECRET_FIXTURE' not in retained
    evidence = os.environ.get('HARBOR_EVIDENCE_DIR')
    if evidence:
        Path(evidence).mkdir(parents=True, exist_ok=True)
        (Path(evidence)/'operational-native.jsonl').write_text(retained)
    # A nonregular or busy log must not block native entry; no stdout diagnostic pollution.
    trace.unlink(); trace.mkdir()
    result = call(['--version'])
    assert result.returncode == 0 and not result.stderr, result
    trace.rmdir()
    trace.write_text(''); trace.chmod(0o600)
    logfd = os.open(trace, os.O_WRONLY)
    try:
        fcntl.flock(logfd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        result = call(['--version'])
        assert result.returncode == 0 and not result.stderr
    finally: os.close(logfd)
    assert not trace.read_text(), 'contended log should be dropped instead of waiting'
    # Rotation leaves the next entry readable and bounds the main stream.
    trace.write_text('x' * (5 * 1024 * 1024 + 1))
    result = call(['--version'])
    assert result.returncode == 0 and events('process.entry')
    assert Path(str(trace)+'.1').exists() and trace.stat().st_size < 8192
    # Invalid correlation data (including arbitrary environment payloads) is replaced, never logged.
    env['OFFICE_DESK_STARTUP_ID'] = secret
    result = call(['--version'])
    assert result.returncode == 0
    assert events('process.entry')[-1]['startup_id'] != secret
    assert sentinel.read_text() == 'unchanged' and list(unrelated.iterdir()) == [sentinel]
    print('PASS operational trace: early native entry; real open/flock causes; attach/spawn failure; two-window request/result ownership; exit/signal; private bounded nonblocking log; no sensitive payloads')
