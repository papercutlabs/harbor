"""Numbered items and headings get display-only gaps; text, wrapping, selection and copy stay exact."""
import subprocess
import uuid
from native_smoke import Terminal

name = 'harbor-list-spacing-test-' + uuid.uuid4().hex
read = ("ObjC.import('AppKit'); function run(a) { var s=$.NSPasteboard.pasteboardWithName(a[0])"
        ".stringForType('public.utf8-plain-text'); return s.isNil() ? '' : ObjC.unwrap(s); }")
clear = "ObjC.import('AppKit'); function run(a) { $.NSPasteboard.pasteboardWithName(a[0]).clearContents; }"


def clipboard():
    return subprocess.check_output(['/usr/bin/osascript', '-l', 'JavaScript', '-e', read, name],
                                   text=True, timeout=5).removesuffix('\n')


def point(t, text):
    y = next(y for y, line in enumerate(t.screen.display) if text in line)
    for x in range(t.cols):
        if ''.join(t.screen.buffer[y][col].data for col in range(x, t.cols)).startswith(text):
            return x, y
    raise AssertionError('No terminal cell for ' + text)


def drag(t, a, b):
    (x, y), (xx, yy) = a, b
    t.send(f'\x1b[<0;{x+1};{y+1}M', wait=.05)
    t.send(f'\x1b[<32;{xx+1};{yy+1}M', wait=.1)
    t.send(f'\x1b[<0;{xx+1};{yy+1}m', wait=.4)
    t.expect('Copied selection')

source = ('## Steps\n1. First step wraps across the narrow terminal width\n2. Second step\n\n'
          '```\n1. code stays\n2. together\n```\n> 3. quoted stays\n> 4. together too\nDone.')
shown = ('Steps\n1. First step wraps across the narrow terminal width\n2. Second step\n\n'
         '1. code stays\n2. together\n│ 3. quoted stays\n│ 4. together too\nDone.')
copied = shown.replace('│ ', '')


def row(t, text):
    return point(t, text)[1]


def empty(t, y, x):
    return not t.screen.display[y][x:].strip()


t = Terminal(cols=40, rows=44, extra_env={'HARBOR_TEST_PASTEBOARD': name})
try:
    t.picker('Studio')
    t.send('\x1b[200~' + source + '\x1b[201~'); t.send(b'\r'); t.expect('Done.')
    x, heading = point(t, 'Steps')
    first, second = row(t, '1. First'), row(t, '2. Second')
    # one gap after the heading, wrapped item rows together, one gap before the next item
    assert empty(t, heading + 1, x) and first == heading + 2, t.text
    assert second - first >= 3 and empty(t, second - 1, x), t.text
    assert all(not empty(t, y, x) for y in range(first, second - 1)), t.text
    # the existing paragraph gap is not doubled, and code and quotes keep their rows together
    code = row(t, '1. code stays')
    assert code == second + 2 and empty(t, second + 1, x), t.text
    assert row(t, '2. together') == code + 1 and row(t, '3. quoted') == code + 2, t.text
    assert row(t, '4. together too') == code + 3 and row(t, 'Done.') == code + 4, t.text
    t.snapshot('harbor-list-spacing-40')
    # a selection across spacer rows copies only the sender's characters
    ex, ey = point(t, 'Second step')
    drag(t, (x, heading), (ex + len('Second step') - 1, ey))
    assert clipboard() == copied.split('\n\n')[0], repr(clipboard())
    t.click_text('Done.', button=2); t.expect('Message actions'); t.click_text('Copy text'); t.expect('Copied')
    assert clipboard() == copied, repr(clipboard())
    # redraws keep the message where it was
    t.send('x'); t.send(b'\x7f')
    assert row(t, 'Steps') == heading and row(t, '2. Second') == second, t.text
    t.resize(56, 44); t.expect('Done.'); t.resize(40, 44); t.expect('Done.')
    assert row(t, '2. Second') - row(t, 'Steps') == second - heading, t.text
    print('PASS list and heading spacing: wrapped items, existing gaps, code and quotes, exact selection and copy, redraw')
finally:
    t.close()
    subprocess.run(['/usr/bin/osascript', '-l', 'JavaScript', '-e', clear, name], capture_output=True, timeout=5)
