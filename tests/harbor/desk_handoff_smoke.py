"""Desk entries in the Commands menu run on Harbor's own terminal and give it back with every draft intact.

Offline only: `harbor --demo -d <fresh dir>` with a fictional Desk child. Like the other PTY checks, pyte strips
Kitty graphics, so this proves menu, key, mode and state handling, not graphics or cmux behaviour.
"""
import os, stat, sys, tempfile
from pathlib import Path
from native_smoke import Terminal

work = Path(tempfile.mkdtemp(prefix='harbor-desk-'))
child = work/'desk_child.py'
child.write_text('''import os, sys, termios, signal
mode = sys.argv[1]
cooked = termios.tcgetattr(0)[3] & termios.ICANON != 0
size = os.get_terminal_size(0)
print("\\x1b[2J\\x1b[HDESK CHILD", "COOKED=%d" % cooked, "SIZE=%dx%d" % (size.columns, size.lines))
print("ARGV=" + repr(sys.argv[2:])); sys.stdout.flush()
if mode == "fail": sys.exit(3)
if mode == "interrupt":
    print("WAITING"); sys.stdout.flush(); signal.pause()
print("Press Enter"); sys.stdout.flush(); sys.stdin.readline()
''')
commands = work/'desk-commands'
py = sys.executable
literal = '$(touch PWNED); echo `id` > PWNED2'
commands.write_text('\n'.join('\t'.join(row) for row in [
    ['Quota', 'Provider capacity', py, str(child), 'ok', literal],
    ['Broken', 'Exits 3', py, str(child), 'fail'],
    ['Stuck', 'Waits for Ctrl+C', py, str(child), 'interrupt'],
    ['Missing', 'No such program', str(work/'absent')],
]) + '\n')

def state(t, settle=2.3):
    t.pump(settle)  # Harbor writes harbor-state every 2 s and again when it hands the terminal over
    rows = (Path(t.config)/'harbor-state').read_text().split('\n')
    return sorted(r for r in rows if r.strip())

def run(t, label):
    t.send(b'\x0f'); t.expect('Desk · ' + label); t.click_text('Desk · ' + label)

t = Terminal(cols=56, rows=38, extra_env={'HARBOR_DESK_COMMANDS': str(commands)})
try:
    pid = t.pid
    # Empty composer: / opens Commands with the Desk section; Esc leaves the draft empty.
    t.send('/'); t.expect('Desk · Quota'); t.send(b'\x1b')
    # Nonempty draft: /, e and q are literal; Ctrl+O opens the same menu without touching the draft.
    t.send('a/b e q /x'); t.expect('a/b e q /x')
    t.send(b'\x0f'); t.expect('Desk · Quota'); assert 'a/b e q /x' in t.text
    t.send(b'\x1b'); t.expect('a/b e q /x')
    t.send(b'\x1b[111;5u'); t.expect('Desk · Quota'); t.send(b'\x1b')  # CSI-u Ctrl+O, as cmux/Ghostty send it
    before = state(t)
    run(t, 'Quota'); t.expect('DESK CHILD')
    assert state(t, 0) == before  # saved at handoff
    assert 'COOKED=1' in t.text, t.text
    assert repr([literal]) in t.text, t.text  # argv element arrives whole; no shell ran
    assert not (work/'PWNED').exists() and not (Path(t.config).parent/'PWNED').exists()
    t.resize(100, 38); t.expect('Press Enter'); t.send(b'\r'); t.expect('Back from Desk')
    t.resize(48, 38); t.expect('a/b e q /x')
    assert state(t) == before
    run(t, 'Broken'); t.expect('Desk exited 3'); t.expect('a/b e q /x')
    run(t, 'Stuck'); t.expect('WAITING'); t.send(b'\x03'); t.expect('Desk interrupted (signal 2)'); t.expect('a/b e q /x')
    run(t, 'Missing'); t.expect('Desk could not start')
    assert state(t) == before and t.pid == pid
    # Mouse reporting and bracketed paste came back: typing after a click still lands in the draft.
    t.send('!'); t.expect('!')
    print('PASS Desk menu from empty and nonempty drafts; argv child on the same terminal; failure, Ctrl+C, missing program and resize keep every draft')
finally:
    t.close()

# Ordinary Harbor: without HARBOR_DESK_COMMANDS there is no Desk section, and Ctrl+O still only opens Commands.
t = Terminal(cols=56, rows=38)
try:
    t.send('/'); t.expect('Quit Harbor'); assert 'Desk ·' not in t.text, t.text
    print('PASS ordinary Harbor shows no Desk entries')
finally:
    t.close()
