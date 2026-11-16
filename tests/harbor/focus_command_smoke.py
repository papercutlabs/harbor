"""Real offline Harbor keys -> Office command producer -> persisted Focus consumer.

Set OFFICE_DIST to the paired Office build. The owner is this live fixture process,
registered by its real ps start/argv identity; no live Desk, seat or account is used.
"""
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path
from native_smoke import Terminal

DIST = Path(os.environ['OFFICE_DIST']).resolve()
NODE = os.environ.get('OFFICE_NODE', 'node')
with tempfile.TemporaryDirectory(prefix='harbor-focus-') as directory:
    home = Path(directory)
    state = home / '.office/desk/state.json'
    state.parent.mkdir(parents=True)
    identity = subprocess.check_output(['ps', '-p', str(os.getpid()), '-o', 'lstart=,args=', '-ww'], text=True).strip()
    initial = {'version': 1, 'homes': {'kept': {'surface': 'fictional'}}, 'pins': {}, 'focus': False,
               'instance': {'pid': os.getpid(), 'surface': '00000000-0000-4000-8000-000000000001',
                            'identity': hashlib.sha256(identity.encode()).hexdigest()}}
    state.write_text(json.dumps(initial))
    commands = state.parent / 'harbor-desk-commands'
    # Consume the actual Office producer, never a hand-built approximation of its argv.
    producer = 'import {deskCommandLines} from '+json.dumps((DIST/'desk/harbor.js').as_uri())+'; console.log(deskCommandLines(process.execPath, process.argv[1]));'
    commands.write_text(subprocess.check_output([NODE, '--input-type=module', '-e', producer, str(DIST/'index.js')], text=True))
    env = {'HOME': str(home), 'HARBOR_DESK_COMMANDS': str(commands), 'OFFICE_SKIP_UPDATE': '1',
           'CMUX_WORKSPACE_ID': '00000000-0000-4000-8000-000000000002',
           'CMUX_SURFACE_ID': '00000000-0000-4000-8000-000000000003'}
    t = Terminal(cols=90, rows=38, extra_env=env)
    try:
        t.picker('Marina')
        t.send('draft f e q')
        t.pump(2.2)  # real Harbor persistence interval
        saved = (Path(t.config)/'harbor-state').read_text()
        pid = t.pid
        for opener, on in [(b'\x0f', True), (b'\x1b[111;9u', False)]:
            t.send(opener); t.expect('Commands & shortcuts'); t.expect('f · Focus '+('off' if on else 'on'))
            t.send('f'); t.expect('f · Focus '+('on' if on else 'off')); t.expect('draft f e q')
            actual = json.loads(state.read_text())
            assert actual == {**initial, 'focus': on}, actual
            assert '--view\tfocus' in commands.read_text()
            assert t.pid == pid
            t.pump(2.2)
            assert (Path(t.config)/'harbor-state').read_text() == saved
        # Select a reading position, then toggle on and off through the same actual key path.
        t.send(b'\x1b'); t.send(b'\x1b[A'); t.pump(2.2)
        reading = (Path(t.config)/'harbor-state').read_text()
        for on in [True, False]:
            t.send(b'\x0f'); t.send('f'); t.expect('f · Focus '+('on' if on else 'off')); t.pump(2.2)
            assert (Path(t.config)/'harbor-state').read_text() == reading
            assert json.loads(state.read_text()) == {**initial, 'focus': on}
        # Same PID with a mismatching native start/argv identity is refused and cannot toggle.
        invalid = {**initial, 'instance': {**initial['instance'], 'identity': 'wrong'}}
        state.write_text(json.dumps(invalid)); before = state.read_bytes()
        t.send(b'\x0f'); t.send('f'); t.expect('No running Desk owns this layout')
        assert state.read_bytes() == before
        t.send('x'); t.expect('Desk exited 1'); t.pump(2.2)
        assert (Path(t.config)/'harbor-state').read_text() == reading
        evidence = os.environ.get('HARBOR_FOCUS_EVIDENCE')
        if evidence:
            out = Path(evidence); out.mkdir(parents=True, exist_ok=True)
            (out/'focus.raw').write_bytes(t.raw)
            (out/'returned.txt').write_text(t.text)
            (out/'commands').write_bytes(commands.read_bytes())
        print('PASS real Ctrl/Command-O f -> Office producer argv -> native owner check -> saved on/off -> Harbor notice; draft, reading and PID retained; mismatched owner refused')
    finally:
        t.close()
