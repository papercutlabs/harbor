"""Offline PTY proof of PCL-1378: Option-/ Commands, no brand/connected row, chat terminal title, new-message bar A."""
import re
import os
import json
import sys
import tempfile
import time
from pathlib import Path
from native_smoke import Terminal, ROOT

def prompt_row(t):
    return next(y for y, row in enumerate(t.screen.display) if '›' in row)

def row_of(t, text):
    return next(y for y, row in enumerate(t.screen.display) if text in row)

def prose(t):
    return ' '.join(row.strip() for row in t.screen.display if row.strip())

def wait_for(t, text, timeout=6):
    end = time.monotonic() + timeout
    while text not in t.text and time.monotonic() < end: t.pump(.1)
    assert text in t.text, f'Missing {text!r}:\n{t.text}'

def capture(t, name):
    directory = os.environ.get('HARBOR_CHROME_EVIDENCE')
    if not directory:
        return
    out = Path(directory) / name
    out.parent.mkdir(parents=True, exist_ok=True)
    out.with_suffix('.txt').write_text(t.text)
    out.with_suffix('.raw').write_bytes(t.raw)
    cells = [[{k: getattr(t.screen.buffer[y][x], k) for k in ('data', 'fg', 'bg', 'bold', 'reverse')}
              for x in range(t.cols)] for y in range(t.rows)]
    out.with_suffix('.json').write_text(json.dumps({'title': t.screen.title, 'cells': cells}))

# Chrome and titles.
hostile = 'Studio 🌊\x1b]2;pwned\x07\x1b[2J\r\n\u009b31m end'
clean = 'Studio 🌊 ]2;pwned [2J 31m end'
t = Terminal(cols=56, rows=38, extra_env={'HARBOR_DEMO_STUDIO_NAME': hostile})
try:
    t.picker('Marina'); t.expect('Marina')
    assert 'HARBOR / TELEGRAM' not in t.text and 'connected' not in t.text.lower(), t.text
    assert 'Marina' in t.screen.display[0], t.screen.display[:3]  # the chat header now takes the reclaimed first row
    assert t.screen.display[2].strip() == '', t.screen.display[:4]  # one spacer, then history from row 3
    print('PASS brand/connected row removed; chat header and history reclaim its rows')
    capture(t, 'normal-56')
    assert t.screen.title == 'Marina', t.screen.title
    t.send('typing does not retitle'); t.send(b'\x1b[D\x1b[C')
    assert bytes(t.raw).count(b'\x1b]2;Marina\x07') == 1, bytes(t.raw).count(b'\x1b]2;Marina\x07')
    print('PASS terminal title is the current chat and is written once, not on every redraw')
    t.picker('Studio')
    assert t.screen.title == clean, repr(t.screen.title)
    raw = bytes(t.raw)
    assert ('\x1b]2;' + clean + '\x07').encode() in raw, raw[-400:]
    titles = set(re.findall(rb'\x1b\]2;([^\x07\x1b]*)\x07', raw))
    assert b'pwned' not in titles and all(x == clean.encode() for x in titles if x.startswith(b'Studio')), titles
    assert b'\x1b]2;pwned' not in raw
    print('PASS hostile provider name is sanitized; its escapes never reach the terminal and Unicode stays')
    t.picker('Marina'); assert t.screen.title == 'Marina', t.screen.title
    t.expect('typing does not retitle')
    print('PASS title follows switching back')
    # Plain slash stays text in an empty or nonempty draft; Option-/ in a picker inserts nothing.
    t.send(b'\x15'); t.send('/'); t.expect('› /'); assert '│ Commands' not in t.text
    t.send(b'\x1b/'); t.expect('│ Commands'); t.send(b'\x1b'); t.expect('› /')
    t.send(b'\x1b/'); capture(t, 'commands-56'); t.send(b'\x1b')
    t.send(b'\x0b'); t.expect('Find a conversation'); t.send(b'\x1b/'); t.send(b'\x1b[47;3u')
    assert '/' not in t.screen.display[row_of(t, 'Find a conversation') + 2], t.text
    t.send(b'\x1b'); t.send(b'\x15')
    print('PASS plain / literal (empty draft too); legacy and enhanced Option-/ open Commands, never filter text')
finally:
    t.close()

# A topic's terminal title must carry the topic, rather than only its parent group.
t = Terminal(cols=90, rows=38, extra_env={'HARBOR_DEMO_TOPICS': '1'})
try:
    t.picker('DEMO-421'); t.expect('Topic-specific history')
    assert t.screen.title == 'Team forum > Desk planning — DEMO-421', repr(t.screen.title)
    t.send('topic draft'); t.picker('Infrastructure'); t.expect('different topic')
    assert t.screen.title == 'Team forum > Infrastructure', repr(t.screen.title)
    t.picker('DEMO-421'); t.expect('topic draft')
    capture(t, 'topic-90')
    print('PASS terminal title identifies each forum topic and follows returning to its draft')
finally:
    t.close()

# Return from a Desk view retitles this terminal.
with tempfile.TemporaryDirectory(prefix='harbor-chrome-') as directory:
    commands = Path(directory) / 'commands'
    commands.write_text('\t'.join(['Quota', 'Existing Desk view', sys.executable, '-c',
                                   'import sys;print("\\x1b]2;Desk\\x07CHILD",flush=True);input()']) + '\n')
    t = Terminal(cols=56, rows=38, extra_env={'HARBOR_DESK_COMMANDS': str(commands)})
    try:
        t.picker('Marina'); t.send('desk draft')
        t.send(b'\x1b/'); t.expect('Desk · Quota'); t.send('q'); t.expect('CHILD'); assert t.screen.title == 'Desk'
        t.send(b'\r'); t.expect('Back from Desk'); t.pump(.4); t.expect('desk draft')
        assert t.screen.title == 'Marina', t.screen.title
        print('PASS returning from Desk restores the current chat title and draft')
    finally:
        t.close()

# Indicator A: count transitions, duplicates, position, Ctrl+G/click, narrow and wide.
for cols, rows, use_click in [(56, 38, False), (110, 40, True), (26, 30, False)]:
    t = Terminal(cols=cols, rows=rows, extra_env={'HARBOR_DEMO_INCOMING_DELAY_MS': '5000', 'HARBOR_DEMO_INCOMING_COUNT': '3'})
    try:
        if cols < 30:
            # The picker's full heading does not fit here; exercise its actual filter and selection.
            t.send(b'\x0b'); t.expect('search…'); t.send('Marina'); t.send(b'\r')
            assert t.screen.title == 'Marina' and 'Enter select' not in t.text, t.text
        else:
            t.picker('Marina')
        t.send('kept draft'); t.send(b'\x1b[5~')
        # The bottom timestamp uses an extra row; at 26 columns the image is above this viewport.
        if cols >= 30:
            t.expect('▀')  # settle the visible image before comparing reading positions
        before_prompt = prompt_row(t)
        main_x = 25 if cols >= 100 else 0
        history = [line[main_x:] for line in t.screen.display[:before_prompt - 2]]
        assert 'new message' not in t.text
        wait_for(t, '↓ 1 new')
        bar = row_of(t, '↓ 1 new')
        assert bar == prompt_row(t) - 2 == before_prompt - 2, (bar, before_prompt)  # directly above the composer surface
        # New activity may reorder the wide sidebar; compare the exact conversation viewport.
        current = [line[main_x:] for line in t.screen.display[:before_prompt - 2]]
        assert current == history, [(i, a, b) for i, (a, b) in enumerate(zip(history, current)) if a != b]
        cell = t.screen.buffer[bar][t.screen.display[bar].index('↓')]
        assert cell.bold and cell.bg not in ('default',), (cell.bg, cell.bold)
        if cols >= 30:
            assert '↓ 1 new message ' in t.screen.display[bar] and t.screen.display[bar].rstrip().endswith('Ctrl+G')
        wait_for(t, '↓ 3 new')  # same-provider-second IDs count; replayed IDs do not count twice
        t.pump(.8); assert '↓ 3 new' in t.text and '↓ 5' not in t.text and '↓ 4' not in t.text
        if cols >= 30: assert '↓ 3 new messages' in t.text
        assert 'Another live message 3.' not in prose(t) and 'kept draft' in t.text and prompt_row(t) == before_prompt
        capture(t, f'unseen-{cols}')
        if use_click: t.click_text('↓ 3 new')
        else: t.send(b'\x07')
        assert 'Another live message 3.' in prose(t), t.text
        t.expect('kept draft')
        assert 'new message' not in t.text and '↓ 3' not in t.text
        t.send(b'\x1b[5~'); t.pump(.3)
        assert 'new message' not in t.text and '↓ 3' not in t.text
        if cols >= 30: t.expect('↓ Latest · Ctrl+G')  # quiet footer only, no bright bar
        t.send(b'\x07'); t.pump(.2); assert 'Latest · Ctrl+G' not in t.text
        print(f'PASS indicator A at {cols} columns: 1 → 3 (same provider second; replays not counted), fixed reader/draft, '
              f'{"click" if use_click else "Ctrl+G"} clears, quiet Latest only when nothing new')
    finally:
        t.close()

# Pending attachment confirmation remains usable while Latest changes the history behind it.
for use_click in (False, True):
    t = Terminal(cols=56, rows=38, extra_env={'HARBOR_DEMO_INCOMING_DELAY_MS': '4500',
                                            'HARBOR_DEMO_INCOMING_COUNT': '3'})
    try:
        t.picker('Marina'); t.send('kept attachment caption'); t.send(b'\x1b[5~')
        fixture = str(ROOT / 'tests/harbor/fixtures/harbor.png')
        t.send('\x1b[200~' + fixture + '\x1b[201~', wait=.4)
        t.expect('Preview attachments'); wait_for(t, '↓ 3 new', timeout=8)
        capture(t, f'unseen-attachment-{"click" if use_click else "key"}-56')
        if use_click: t.click_text('↓ 3 new')
        else: t.send(b'\x07')
        t.expect('Preview attachments'); t.expect('harbor.png'); t.expect('kept attachment caption')
        assert '↓ 3 new' not in t.text and 'Sending attachments' not in t.text and 'Sent' not in t.text
        t.click_text('Send 1 attachment'); t.delivered('kept attachment caption')
        assert 'Preview attachments' not in t.text, t.text
        print(f'PASS {"click" if use_click else "Ctrl+G"} keeps pending attachment, caption and confirmation; '
              'the attachment sends only after explicit Send')
    finally:
        t.close()

# The bar occupies its own row above either reply or edit context. Jumping keeps the
# exact draft cursor and context; line deletion still stops at the hard newline.
for context, use_click in [('reply', False), ('edit', True)]:
    t = Terminal(cols=56, rows=38, extra_env={'HARBOR_DEMO_INCOMING_DELAY_MS': '5500',
                                            'HARBOR_DEMO_INCOMING_COUNT': '3'})
    try:
        t.picker('Marina'); t.send('normal draft before context')
        if context == 'reply':
            t.click_text('Keeping this one', button=2); t.click_text('Reply')
            label = 'Replying · '
            t.clear_draft()
        else:
            t.message_tool('Keeping this one', 'Edit message')
            label = 'Editing your message'
            t.clear_draft()
        t.expect(label)
        t.send('context head'); t.send(b'\x1b[13;2u'); t.send('context tail')
        for _ in range(4): t.send(b'\x1b[D', wait=.02)
        t.send(b'\x1b[5~')
        t.expect('▀')
        before = prompt_row(t)
        history = t.screen.display[:before - 3]
        wait_for(t, '↓ 3 new', timeout=8)
        bar = row_of(t, '↓ 3 new')
        assert bar == before - 3 and row_of(t, label) == bar + 1, t.text
        assert t.screen.display[:before - 3] == history, t.text
        t.expect('context head'); t.expect('context tail')
        capture(t, f'unseen-{context}-56')
        if use_click: t.click_text('↓ 3 new')
        else: t.send(b'\x07')
        t.expect('Another live message 3.'); t.expect(label)
        t.send('X'); t.expect('context Xtail')  # cursor before tail survived Latest
        t.send(b'\x1b[127;9u'); t.expect('context head')
        assert 'context Xtail' not in t.text and 'tail' in t.text, t.text
        t.expect(label)
        t.send(b'\x1b'); t.expect('normal draft before context') if context == 'edit' else t.expect('Reply canceled')
        print(f'PASS unseen bar above {context} context; {"click" if use_click else "Ctrl+G"} '
              'keeps draft/cursor/context; Cmd+Backspace keeps the other hard line and suffix')
    finally:
        t.close()
