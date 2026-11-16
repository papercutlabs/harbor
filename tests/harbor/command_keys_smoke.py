"""Offline real-decoder/PTY proof of Desk command accelerators; no live accounts."""
import json
import os
import sys
import tempfile
from pathlib import Path
from native_smoke import Terminal

with tempfile.TemporaryDirectory(prefix='harbor-command-keys-') as directory:
    work = Path(directory)
    calls = work / 'calls.jsonl'
    child = work / 'child.py'
    child.write_text('import json,sys\nfrom pathlib import Path\np=Path(sys.argv[1])\nwith p.open("a") as f: f.write(json.dumps(sys.argv[2:])+"\\n")\nprint("COMMAND CHILD", flush=True)\ninput()\n')
    commands = work / 'commands'
    literal = '$(touch NO_SHELL); `id`'
    commands.write_text('\n'.join('\t'.join([label, 'Existing Desk view', sys.executable, str(child), str(calls), label, literal]) for label in ['Quota', 'Everything']) + '\n')

    def recorded():
        return [json.loads(line) for line in calls.read_text().splitlines()] if calls.exists() else []

    def state(t):
        t.pump(2.2)  # production persistence interval; compare the real stored draft and reading state
        return (Path(t.config) / 'harbor-state').read_text()

    t = Terminal(cols=72, rows=38, extra_env={'HARBOR_DESK_COMMANDS': str(commands)})
    try:
        pid = t.pid
        t.picker('Marina')
        t.send('literal e q')
        t.expect('literal e q')
        assert not recorded()
        before = state(t)
        # Exact Super-O is the Kitty CSI-u Command key. Unbound modifiers stay unbound.
        # Ctrl-containing combinations retain the existing generic Ctrl-key dispatch.
        for wire in [b'\x1b[111;10u', b'\x1b[111;11u', b'\x1b[111;33u']:
            t.send(wire)
            assert 'Commands & shortcuts' not in t.text
        assert state(t) == before
        t.send(b'\x1b[111;9u'); t.expect('Commands & shortcuts')
        t.expect('e · Existing Desk view'); t.expect('q · Existing Desk view')
        t.send(b'\x1b'); t.expect('literal e q')
        assert state(t) == before
        for opener, key, label in [(b'\x0f', b'e', 'Everything'), (b'\x0f', b'q', 'Quota'),
                                    (b'\x1b[111;9u', b'e', 'Everything'), (b'\x1b[111;9u', b'q', 'Quota')]:
            old = recorded()
            t.send(opener); t.expect('Commands & shortcuts'); t.send(key); t.expect('COMMAND CHILD')
            assert recorded() == old + [[label, literal]], recorded()
            t.send(b'\r'); t.expect('Back from Desk'); t.expect('literal e q')
            assert t.pid == pid and state(t) == before
        # Reading selection also survives the same shortcut handoff.
        t.send(b'\x1b'); t.send(b'\x1b[A')
        reading = state(t)
        t.send(b'\x0f'); t.send(b'q'); t.expect('COMMAND CHILD'); t.send(b'\r')
        t.expect('Back from Desk')
        assert state(t) == reading and t.pid == pid
        # Picker letters remain filter text rather than launching either command.
        old = recorded()
        t.send(b'\x0b'); t.expect('Find a conversation'); t.send('eq')
        assert recorded() == old
        t.send(b'\x1b')
        output = os.environ.get('HARBOR_COMMAND_KEYS_EVIDENCE')
        if output:
            Path(output).mkdir(parents=True, exist_ok=True)
            (Path(output) / 'commands.raw').write_bytes(t.raw)
            (Path(output) / 'returned.txt').write_text(t.text)
            (Path(output) / 'calls.json').write_text(json.dumps(recorded(), indent=2))
        print('PASS literal e/q, exact Super-O, Ctrl-O, fixed argv once, Esc, draft, reading state, same process and picker isolation')
    finally:
        t.close()
    # A standalone Harbor must not fabricate Desk commands for either letter.
    t = Terminal(cols=72, rows=38, extra_env={'HARBOR_DESK_COMMANDS': ''})
    try:
        old = recorded()
        t.send(b'\x0f'); t.expect('Commands & shortcuts'); t.send('eq')
        assert 'Commands & shortcuts' in t.text and 'Desk ·' not in t.text
        assert recorded() == old
        t.send(b'\x1b')
        print('PASS standalone missing Desk entries are harmless')
    finally:
        t.close()
