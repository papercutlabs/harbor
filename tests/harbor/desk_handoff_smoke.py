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
    t.picker('Marina')
    # Empty composer: Option-/ opens Commands with the Desk section; Esc leaves the draft empty.
    t.send(b'\x1b/'); t.expect('Desk · Quota'); t.send(b'\x1b')
    # Nonempty draft: /, e and q are literal; Ctrl+O opens the same menu without touching the draft.
    t.send('a/b e q /x'); t.expect('a/b e q /x')
    t.send(b'\x0f'); t.expect('Desk · Quota')
    assert draft_saved(t, 'a/b e q /x'), state(t, 0)  # the draft sits under the menu overlay; its saved bytes are exact
    t.send(b'\x1b'); t.expect('a/b e q /x')
    t.send(b'\x1b[111;5u'); t.expect('Desk · Quota'); t.send(b'\x1b')  # CSI-u Ctrl+O, as cmux/Ghostty send it
    before = state(t)
    # Use the keyboard here so the cooked child receives only the tested line, without a late mouse release.
    t.send(b'\x0f'); t.expect('Desk · Quota'); t.send(b'\r'); t.expect('DESK CHILD')
    assert state(t, 0) == before  # saved at handoff
    assert 'COOKED=1' in t.text, t.text
    assert repr([literal]) in t.text, t.text  # argv element arrives whole; no shell ran
    assert not (work/'PWNED').exists() and not (Path(t.config).parent/'PWNED').exists()
    t.resize(100, 38); t.expect('Press Enter')
    # Harbor's input reader is stopped while the child runs: every typed byte reaches the child, none the draft.
    t.send(b'deskline\r'); t.expect('Back from Desk')
    # The child output is transient: validate retained terminal bytes after Harbor has repainted.
    assert b"GOT='deskline\\n'" in t.raw, [line for line in bytes(t.raw).splitlines() if b"GOT=" in line]
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

# An already-running standalone Harbor discovers a later Desk installation without losing its draft.
home = work/'ordinary-home'
home.mkdir()
t = Terminal(cols=56, rows=38, extra_env={'HOME': str(home), 'HARBOR_DESK_COMMANDS': ''})
try:
    t.send(b'\x1b/'); t.expect('Quit Harbor'); assert 'Desk ·' not in t.text, t.text
    t.send(b'\x1b'); t.picker('Marina'); t.send('standalone draft')
    installed = home/'.office/desk/harbor-desk-commands'
    installed.parent.mkdir(parents=True)
    installed.write_text(commands.read_text())
    t.send(b'\x0f'); t.expect('Desk · Quota'); t.send(b'\r'); t.expect('DESK CHILD')
    t.send(b'\r'); t.expect('Back from Desk'); t.expect('standalone draft')
    print('PASS standalone Harbor discovers installed Desk commands on menu open and preserves its draft')
finally:
    t.close()

# Explicit host configuration wins even when installed commands exist; a missing explicit file stays missing.
other = work/'explicit-commands'
other.write_text('\t'.join(['Custom', 'Explicit host', py, str(child), 'ok']) + '\n')
t = Terminal(cols=56, rows=38, extra_env={'HOME': str(home), 'HARBOR_DESK_COMMANDS': str(other)})
try:
    t.send(b'\x0f'); t.expect('Desk · Custom'); assert 'Desk · Quota' not in t.text, t.text
    other.unlink()
    t.send(b'\x1b'); t.send(b'\x0f'); t.expect('Quit Harbor'); assert 'Desk ·' not in t.text, t.text
    print('PASS explicit Desk commands take precedence and never fall back when their file is absent')
finally:
    t.close()

# Four-row inline strip: shared backend, per-window geometry and bounded untrusted input.
# Each UI and backend owns one disposable config; inherited selectors cannot select a live account.
import time
import json
with tempfile.TemporaryDirectory(prefix='harbor-inline-strip-', dir='/tmp') as directory:
    work = Path(directory)
    config = work/'config'
    strip = work/'strip'
    sentinel = work/'sentinel'
    sentinel.write_text('keep')
    commands = work/'commands'
    commands.write_text('\n'.join('\t'.join([label, 'Strip return fixture', sys.executable, '-c',
        'print("INLINE CHILD ' + label + '",flush=True);input()'])
        for label in ['Everything', 'Quota']) + '\n')
    env = {'HOME': str(work/'home'), 'XDG_CONFIG_HOME': str(work/'xdg'),
           'HARBOR_DESK_COMMANDS': str(commands), 'HARBOR_DESK_WARM': '0'}
    rows = ['Desk CPU 18% MEM 6% DISK 90%', 'CODEX 2.4/4 ▁▃▅ 0.017/h reset 19h',
            'CLAUDE 2.2/6 ▁▂▆ 0.023/h reset 14h', 'GROK 1.0/1 ▇▇▇ 0.000/h reset 6d']
    def write_rows(values=rows, stamp=None, raw=None):
        stamp = int(time.time()*1000) if stamp is None else stamp
        data = ('pcl.desk.strip.v1\t' + str(stamp) + '\n' + '\n'.join(values) + '\n').encode() if raw is None else raw
        temporary = strip.with_suffix('.next')
        temporary.write_bytes(data)
        temporary.replace(strip)
    def header(t, expected):
        t.pump(.4)
        actual = [line.rstrip() for line in t.screen.display[:4]]
        assert actual == expected, (actual, expected, t.text)
    def unknown(t):
        header(t, ['Desk  CPU ?  MEM ?  DISK ?', 'CODEX  unavailable', 'CLAUDE unavailable', 'GROK   unavailable'])
    def capture(t, name):
        if os.environ.get('HARBOR_STRIP_EVIDENCE'):
            out = Path(os.environ['HARBOR_STRIP_EVIDENCE'])/name
            out.parent.mkdir(parents=True, exist_ok=True)
            out.with_suffix('.txt').write_text(t.text)
            out.with_suffix('.raw').write_bytes(t.raw)
            out.with_suffix('.json').write_text(json.dumps({'title': t.screen.title, 'cursor': [t.screen.cursor.x, t.screen.cursor.y],
                'cells': [[{k:getattr(t.screen.buffer[y][x], k) for k in ('data','fg','bg','bold')}
                           for x in range(t.cols)] for y in range(t.rows)]}))
    write_rows()
    a = Terminal(cols=80, rows=38, config_dir=config, extra_env=dict(env, HARBOR_DESK_STRIP_FILE=str(strip)))
    b = None
    try:
        a.picker('Saved Messages'); a.expect('Note 35:'); header(a, rows)
        assert 'Saved Messages' in a.screen.display[4] and a.screen.cursor.y >= 4, a.text
        a.send(b'\x1b[5~'); a.send('draft / e q'); a.send(b'\x1b[D')
        before = state(a)
        note_rows = [row for row in a.screen.display[7:] if 'Note ' in row]
        capture(a, 'strip-80')
        # The sibling attaches to the exact same backend but has no strip opt-in.
        b = Terminal(cols=80, rows=38, config_dir=config, extra_env=dict(env, HARBOR_DESK_STRIP_FILE=''))
        b.picker('Marina'); b.send('sibling draft'); b.pump(.3)
        assert 'Marina' in b.screen.display[0] and 'CODEX' not in b.text and 'CPU' not in b.text, b.text
        changed = [rows[0].replace('18%', '23%')] + rows[1:]
        write_rows(changed); header(a, changed)  # idle file refresh, no input to a
        b.pump(.3); assert 'CPU' not in b.text and 'sibling draft' in b.text, b.text
        assert [row for row in a.screen.display[7:] if 'Note ' in row] == note_rows
        assert state(a) == before, 'refresh changed draft/cursor/reading position'
        # Each rejection must clear previously visible valid values, and recover without input.
        cases = [b'wrong\n' + b'x\n'*4, b'x'*16385,
                 ('pcl.desk.strip.v1\t0\n' + '\n'.join(rows) + '\n').encode(),
                 ('pcl.desk.strip.v1\t' + str(int(time.time()*1000)+60000) + '\n' + '\n'.join(rows) + '\n').encode(),
                 ('pcl.desk.strip.v1\t' + str(int(time.time()*1000)) + '\n' + '\n'.join(rows[:3]) + '\n').encode(),
                 ('pcl.desk.strip.v1\t' + str(int(time.time()*1000)) + '\n' + '\n'.join(rows) + '\nextra\n').encode()]
        for invalid in ['bad\x1b[2J', 'bad\x00', 'bad\t', 'bad\u009b31m']:
            cases.append(('pcl.desk.strip.v1\t' + str(int(time.time()*1000)) + '\n' + '\n'.join([invalid]+rows[1:]) + '\n').encode())
        cases.append(b'pcl.desk.strip.v1\t' + str(int(time.time()*1000)).encode() + b'\ninvalid\xff\nrow2\nrow3\nrow4\n')
        for invalid in cases:
            write_rows(raw=invalid); unknown(a); write_rows(); header(a, rows)
        strip.unlink(); unknown(a); write_rows(); header(a, rows)
        # Cross the real 90-second freshness boundary without waiting for a production interval.
        write_rows(stamp=int(time.time()*1000)-88800); header(a, rows); a.pump(1.0); unknown(a)
        capture(a, 'strip-unavailable-80'); write_rows(); header(a, rows)
        for cols, height in [(110, 38), (26, 22), (18, 8), (1, 2), (80, 38)]:
            a.resize(cols, height); a.pump(.3)
            # Physical terminals shorter than four rows show only the rows that exist.
            assert a.screen.display[0].startswith('Desk'[:cols]), a.text
            assert a.screen.cursor.y >= min(4, height - 1) or a.screen.cursor.hidden, (cols, height, a.screen.cursor)
            if cols == 110:
                header(a, rows); assert 'CONVERSATIONS' in a.screen.display[4], a.text
            capture(a, 'strip-' + str(cols) + 'x' + str(height))
        a.expect('draft / e q'); header(a, rows)
        assert state(a) == before, 'resize changed draft/cursor/reading position'
        for key, label in [('e','Everything'), ('q','Quota')]:
            a.send(b'\x0f'); a.expect('Desk · ' + label); header(a, rows)
            a.send(key); a.expect('INLINE CHILD ' + label); a.send(b'\r'); a.expect('Back from Desk')
            header(a, rows); a.expect('draft / e q')
            assert state(a) == before, label + ' return changed saved state'
        a.send('X'); a.expect('draft / e Xq')  # exact nonempty draft cursor survived both handoffs
        assert a.screen.cursor.y >= 4 and sentinel.read_text() == 'keep'
        assert 'sibling draft' in b.text and 'CPU' not in b.text
        print('PASS inline strip: exactly four full-width rows, idle refresh/expiry, malformed/missing/oversize/control/UTF-8/future rejection, tiny resize recovery, independent shared-session sibling, draft/cursor/scroll and Ctrl+O E/Q return')
    finally:
        if b: b.close()
        a.close()
        assert sentinel.read_text() == 'keep'
