"""Desk entries in the Commands menu run on Harbor's own terminal and give it back with every draft intact.

Offline only: `harbor --demo -d <fresh dir>` with a fictional Desk child. Like the other PTY checks, pyte strips
Kitty graphics, so this proves menu, key, mode and state handling, not graphics or cmux behaviour.
"""
import os, stat, sys, tempfile
from pathlib import Path
from native_smoke import Terminal

work = Path(tempfile.mkdtemp(prefix='harbor-desk-'))
child = work/'desk_child.py'
child.write_text('''import fcntl, os, sys, termios, signal
mode = sys.argv[1]
cooked = termios.tcgetattr(0)[3] & termios.ICANON != 0
size = os.get_terminal_size(0)
print("\\x1b[2J\\x1b[HDESK CHILD", "COOKED=%d" % cooked, "SIZE=%dx%d" % (size.columns, size.lines))
print("ARGV=" + repr(sys.argv[2:])); sys.stdout.flush()
if mode == "fail": sys.exit(3)
if mode == "nonblock":  # as a Node child can: O_NONBLOCK on the shared terminal description, then failure
    fcntl.fcntl(0, fcntl.F_SETFL, fcntl.fcntl(0, fcntl.F_GETFL) | os.O_NONBLOCK); sys.exit(4)
if mode == "interrupt":
    print("WAITING"); sys.stdout.flush(); signal.pause()
print("Press Enter"); sys.stdout.flush(); print("GOT=" + repr(sys.stdin.readline())); sys.stdout.flush()
import time; time.sleep(.3)
''')
commands = work/'desk-commands'
py = sys.executable
literal = '$(touch PWNED); echo `id` > PWNED2'
commands.write_text('\n'.join('\t'.join(row) for row in [
    ['Quota', 'Provider capacity', py, str(child), 'ok', literal],
    ['Broken', 'Exits 3', py, str(child), 'fail'],
    ['Stuck', 'Waits for Ctrl+C', py, str(child), 'interrupt'],
    ['Nonblock', 'Exits 4 non-blocking', py, str(child), 'nonblock'],
    ['Missing', 'No such program', str(work/'absent')],
]) + '\n')

def state(t, settle=2.3):
    t.pump(settle)  # Harbor writes harbor-state every 2 s and again when it hands the terminal over
    rows = (Path(t.config)/'harbor-state').read_text().split('\n')
    return sorted(r for r in rows if r.strip())

def kept(rows):
    """Every saved field that does not depend on width: chat, draft, cursor, follow, selection, reply, edit and the
    pre-edit draft/cursor/reply. The row-based scroll offset re-wraps with width, so it is compared only when not following."""
    out = []
    for row in rows:
        f = row.split(' ')
        if f[0] == 'active':
            out.append(tuple(f)); continue
        out.append((f[0], f[1], f[2], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[3] if f[4] == '0' else None))
    return out

def draft_saved(t, text):
    return any(not r.startswith('active ') and r.split(' ')[1].lower() == text.encode().hex() for r in state(t))

def run(t, label):
    t.send(b'\x0f'); t.expect('Desk · ' + label); t.click_text('Desk · ' + label)  # mouse press, then its release

def held(t):
    """The failure pause outlasts the trailing release of the click that chose the entry and a late mouse report: only a
    new key ends it. Harbor has not repainted (no status line) until then."""
    t.pump(.6)
    before = t.text
    assert 'press any key to return to Harbor' in ' '.join(before.split()), before
    t.send(b'\x1b[<0;3;3m', wait=.4)  # a stray SGR release is not a key
    t.send(b'\x1b[<35;4;3M', wait=.4)  # nor is motion
    assert t.text == before, t.text

t = Terminal(cols=56, rows=38, extra_env={'HARBOR_DESK_COMMANDS': str(commands)})
try:
    pid = t.pid
    t.picker('Marina')  # / acts only in a selected conversation (existing behaviour)
    # Empty composer: / opens Commands with the Desk section; Esc leaves the draft empty.
    t.send('/'); t.expect('Desk · Quota'); t.send(b'\x1b')
    # Nonempty draft: /, e and q are literal; Ctrl+O opens the same menu without touching the draft.
    t.send('a/b e q /x'); t.expect('a/b e q /x')
    t.send(b'\x0f'); t.expect('Desk · Quota')
    assert draft_saved(t, 'a/b e q /x'), state(t, 0)  # the draft sits under the menu overlay; its saved bytes are exact
    t.send(b'\x1b'); t.expect('a/b e q /x')
    t.send(b'\x1b[111;5u'); t.expect('Desk · Quota'); t.send(b'\x1b')  # CSI-u Ctrl+O, as cmux/Ghostty send it
    before = state(t)
    run(t, 'Quota'); t.expect('DESK CHILD')
    assert state(t, 0) == before  # saved at handoff
    assert 'COOKED=1' in t.text, t.text
    assert repr([literal]) in t.text, t.text  # argv element arrives whole; no shell ran
    assert not (work/'PWNED').exists() and not (Path(t.config).parent/'PWNED').exists()
    t.resize(100, 38); t.expect('Press Enter')
    # Harbor's input reader is stopped while the child runs: every typed byte reaches the child, none the draft.
    t.send(b'deskline\r'); t.expect("GOT='deskline\\n'"); t.expect('Back from Desk')
    t.resize(56, 38); t.expect('a/b e q /x')
    assert state(t) == before  # same width as before: every field, scroll included, is exact
    t.resize(48, 38); t.expect('a/b e q /x')
    assert kept(state(t)) == kept(before)  # a narrower width re-wraps only the scroll offset of a following view
    t.resize(56, 38); t.expect('a/b e q /x')
    run(t, 'Broken'); t.pump(.3); assert 'press any key to return to Harbor' in ' '.join(t.text.split()), t.text; held(t)
    assert 'DESK CHILD' in t.text and 'Desk exited 3' in t.text, t.text  # the child's own output is still readable
    t.send('z'); t.expect('Desk exited 3 · draft kept'); t.expect('a/b e q /x')
    run(t, 'Nonblock'); t.pump(.3); assert 'press any key to return to Harbor' in ' '.join(t.text.split()), t.text; held(t)
    t.send('z'); t.expect('Desk exited 4 · draft kept'); t.expect('a/b e q /x')
    run(t, 'Stuck'); t.expect('WAITING'); t.send(b'\x03'); t.expect('Desk interrupted (signal 2)'); t.expect('a/b e q /x')
    assert 'press any key' not in t.text, t.text  # Ctrl+C returns at once
    run(t, 'Missing'); t.pump(.3); assert 'press any key to return to Harbor' in ' '.join(t.text.split()), t.text; held(t); t.send('z'); t.expect('a/b e q /x'); assert 'press any key to return to Harbor' not in ' '.join(t.text.split()), t.text
    assert state(t) == before and t.pid == pid
    # Mouse reporting and bracketed paste came back: typing after a click still lands in the draft.
    t.send('!'); t.expect('!')
    print('PASS Desk menu from empty and nonempty drafts; argv child on the same terminal; failure pause, Ctrl+C, missing program and resize keep every draft')
finally:
    t.close()

# Ordinary Harbor: without HARBOR_DESK_COMMANDS there is no Desk section, and Ctrl+O still only opens Commands.
t = Terminal(cols=56, rows=38)
try:
    t.send('/'); t.expect('Quit Harbor'); assert 'Desk ·' not in t.text, t.text
    print('PASS ordinary Harbor shows no Desk entries')
finally:
    t.close()
