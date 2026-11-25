"""Rendered-row editing through native input: clicks, wraps, deletion and Unicode."""
import unicodedata
import tempfile
import time
from pathlib import Path
from native_smoke import Terminal, ROOT

KEYS = [b'\x15', b'\x1b[127;9u']  # legacy Ctrl+U and Kitty Super+Backspace
NEWLINE = b'\x1b[13;2u'


def click(t, x, y):
    t.send(f'\x1b[<0;{x+1};{y+1}M', wait=.03)
    t.send(f'\x1b[<0;{x+1};{y+1}m', wait=.03)


def reset(t, text):
    t.clear_draft(4)
    t.send('\x1b[200~' + text + '\x1b[201~')


def locate(t, text):
    text = unicodedata.normalize('NFC', text)
    deadline = time.monotonic() + 3
    while True:
        for y, row in enumerate(t.screen.display):
            row = unicodedata.normalize('NFC', row)
            if text in row:
                return row.index(text), y
        if time.monotonic() >= deadline:
            raise AssertionError((text, t.text))
        # A completed input write does not mean its rendered frame was read.
        t.pump(.02)


def expect(t, text):
    locate(t, text)


def expect_cursor(t, expected, context, hidden=False):
    # Input acceptance and the next painted frame are separate events. Harbor
    # draws at most once per 16 ms; observe the frame instead of assuming a delay.
    def matches():
        return t.screen.cursor.hidden == hidden and (hidden or
            (t.screen.cursor.x, t.screen.cursor.y) == expected)
    deadline = time.monotonic() + 3
    while not matches() and time.monotonic() < deadline:
        t.pump(.02)
    assert matches(), (context, expected, (t.screen.cursor.x, t.screen.cursor.y),
                       t.screen.cursor.hidden, t.text)


def check_newline_backspace():
    # Exercise actual paste decoding and enhanced Shift+Enter, rendering and saved source.
    # A hard newline is a separate edit unit, even though wcwidth gives it no cells.
    for text, typed in (('lorry crane deliveries inside', False),
                        ('different draft café', True),
                        ('combining e\u0301', False),
                        ('two blank rows\n', False)):
        for presses in (1, 2):
            with tempfile.TemporaryDirectory(prefix='harbor-newline-backspace-') as directory:
                t = Terminal(cols=112, rows=38, config_dir=directory)
                try:
                    t.picker('Studio')
                    t.send('\x1b[200~' + text + ('' if typed else '\n') + '\x1b[201~')
                    if typed:
                        t.send(NEWLINE)
                    x, y = t.screen.cursor.x, t.screen.cursor.y
                    assert not t.screen.display[y][x:].strip(), ('blank caret row', t.text)
                    if not text.endswith('\n'):
                        assert unicodedata.normalize('NFC', text.split('\n')[-1]) in unicodedata.normalize('NFC', t.screen.display[y-1]), ('previous row', t.text)
                    t.send(b'\x7f')
                    if not text.endswith('\n'):
                        shown = unicodedata.normalize('NFC', t.screen.display[t.screen.cursor.y])
                        last = unicodedata.normalize('NFC', text.split('\n')[-1])
                        assert last in shown and t.screen.cursor.x == shown.index(last) + len(last), ('caret beside last letter', t.text)
                    if presses == 2:
                        t.send(b'\x7f')
                    expected = text
                    if presses == 2:
                        expected = text[:-2] if text.endswith('e\u0301') else text[:-1]
                finally:
                    t.close()
                    t.wait_backend_shutdown()
                records = Path(directory, 'harbor-state').read_text().splitlines()
                active = next(row.split()[1] for row in records if row.startswith('active '))
                fields = next(row.split() for row in records if row.split()[0] == active)
                actual = bytes.fromhex(fields[1]).decode() if fields[1] != '-' else ''
                assert actual == expected, (text, typed, presses, actual, expected)
                assert int(fields[2]) == len(expected), (text, fields[2], len(expected))
    print('PASS native blank-row Backspace: pasted/typed newline; combining cluster; consecutive blank rows; next deletion')


def check_newline_navigation():
    # Hard newlines are insertion stops. They must not join a preceding glyph,
    # including a combining cluster or a tab, when moving in either direction.
    from wcwidth import wcwidth
    for prefix, last_unit in (('lorry crane deliveries inside', 1),
                              ('wide 界 e\u0301', 2), ('tab\t', 1), ('two blanks\n', 1)):
        with tempfile.TemporaryDirectory(prefix='harbor-newline-navigation-') as directory:
            t = Terminal(cols=112, rows=38, config_dir=directory)
            source = prefix + '\n'
            try:
                t.picker('Studio')
                t.send('\x1b[200~' + source + '\x1b[201~')
                first = unicodedata.normalize('NFC', source.split('\n', 1)[0].rstrip())
                deadline = time.monotonic() + 3
                while first not in unicodedata.normalize('NFC', t.text) and time.monotonic() < deadline:
                    t.pump(.02)
                base_x, base_y = locate(t, first)
                def position(offset):
                    before = source[:offset]
                    line = before.rsplit('\n', 1)[-1]
                    cells = sum(1 if ch == '\t' else max(0, wcwidth(ch)) for ch in line)
                    expect_cursor(t, (base_x + cells, base_y + before.count('\n')), (source, offset))
                position(len(source))
                for key, offset in ((b'\x1bOD', len(prefix)),
                                    (b'\x1bOC', len(source)),
                                    (b'\x1bOD', len(prefix)),
                                    (b'\x1bOD', len(prefix) - last_unit),
                                    (b'\x1bOC', len(prefix))):
                    t.send(key, wait=.03)
                    position(offset)
                if '\n' not in prefix:
                    # Reach the same edge from the beginning, one grapheme at a time.
                    t.send(b'\x01')
                    position(0)
                    stops = [i for i in range(1, len(prefix) + 1)
                             if i == len(prefix) or wcwidth(prefix[i]) != 0]
                    for offset in stops:
                        t.send(b'\x1bOC', wait=.01)
                        position(offset)
            finally:
                t.close()
                t.wait_backend_shutdown()
            records = Path(directory, 'harbor-state').read_text().splitlines()
            active = next(row.split()[1] for row in records if row.startswith('active '))
            fields = next(row.split() for row in records if row.split()[0] == active)
            assert bytes.fromhex(fields[1]).decode() == source, (source, fields)
            assert int(fields[2]) == len(prefix), (source, fields[2], len(prefix))
    print('PASS native newline cursor stops: both arrows, line edge, combining cluster, tab, blank rows; exact draft preserved')


def check_integrated_cursor():
    for strip_enabled in (True, False):
        with tempfile.TemporaryDirectory(prefix='harbor-cursor-frame-') as directory:
            strip = Path(directory, 'strip')
            strip.write_text('pcl.desk.strip.v1\t' + str(int(time.time() * 1000)) +
                             '\nDesk fixture\nCODEX fixture\nCLAUDE fixture\nGROK fixture\n')
            env = {'HARBOR_DESK_STRIP_FILE': str(strip) if strip_enabled else '',
                   'HARBOR_GRAPHICS': 'kitty'}
            t = Terminal(cols=112, rows=38, extra_env=env)
            try:
                t.picker('Studio')
                t.send('caret_frame_sentinel')
                t.send(b'\x0b'); t.send('St')
                x, y = locate(t, '› St')
                expect_cursor(t, (x + len('› St'), y), ('filter caret', strip_enabled))
                t.send(b'\x1b')
                x, y = locate(t, 'caret_frame_sentinel')
                expect_cursor(t, (x + len('caret_frame_sentinel'), y), ('composer caret', strip_enabled))
                t.send(b'\x0f'); t.expect('│ Commands')
                expect_cursor(t, None, ('noneditable menu caret', strip_enabled), hidden=True)
                t.send(b'\x1b')
                t.clear_draft()
                t.send('caption_frame')
                fixture = str(ROOT / 'tests/harbor/fixtures/new-shelf.md')
                t.send('\x1b[200~' + fixture + '\x1b[201~', wait=.4)
                t.expect('Caption · type to add'); t.send('X')
                x, y = locate(t, 'caption_frameX')
                expect_cursor(t, (x + len('caption_frameX'), y), ('caption caret', strip_enabled))
            finally:
                t.close()
    print('PASS actual frame cursor with strip on/off and Kitty graphics: composer, filter, caption, noneditable menu')


check_integrated_cursor()

check_newline_navigation()

check_newline_backspace()

for cols in (34, 50, 112):
    t = Terminal(cols=cols, rows=38)
    try:
        t.picker('Studio')
        # A sent message remains history, including when line-delete acts on an empty draft.
        t.send('sent_keep11044'); t.send(b'\r'); t.expect('sent_keep11044')
        for key in KEYS:
            # End and middle of a final hard row; newline and suffix survive.
            reset(t, 'hard_keep\nabcdef tail')
            for _ in range(4): t.send(b'\x1bOD', wait=.01)
            t.send(key); t.send('Z')
            expect(t, 'Ztail'); expect(t, 'hard_keep')
            # At the start of that row, the previous newline must remain.
            t.send(b'\x1bOD'); t.send(key); t.send('Q')
            expect(t, 'QZtail'); expect(t, 'hard_keep')

            # No-space wrap: row-end has the same source offset as the next row-start.
            # Clicking on or after the last glyph must insert *after* that glyph.
            reset(t, 'hard_keep\n' + 'abcdefghij' * 15)
            y = t.screen.cursor.y - 1
            line = t.screen.display[y].rstrip()
            start = next(i for i, ch in enumerate(line) if ch in 'abcdefghij')
            shown = line[start:]
            last = len(line) - 1
            click(t, last, y)
            assert t.screen.cursor.y == y, ('row-end affinity', cols, t.text)
            t.send('X')
            # X wraps, so observe it on the following row then remove it.
            expect(t, 'X'); t.send(b'\x7f')
            # Click after the same row end and delete only that visual row.
            click(t, min(t.cols - 3, last + 2), y)
            t.send(key); t.send('Y')
            # Earlier source is still present; deletion cannot reach the typed newline.
            assert 'abcdefghij' in t.text, (cols, key, t.text)
            assert 'Delete message' not in t.text

            # Word wrap at spaces has a distinct end source offset. Previous row survives.
            reset(t, 'head_keep\n' + 'one two three four five six seven eight nine ten ' * 4)
            y = t.screen.cursor.y
            final = t.screen.display[y].strip()
            previous = t.screen.display[y-1].strip()
            assert final and previous and final != previous, t.text
            t.send(key)
            expect(t, previous)
            t.send('row_end_marker')
            expect(t, 'row_end_marker')

            # Wide glyph and combining cluster at final row end, clicked on the glyph itself.
            reset(t, 'hard_keep\n界e\u0301🌊')
            x, y = locate(t, '界é🌊')
            click(t, x + 3, y)  # wave's first cell; its second cell is another valid row-end click
            t.send('X'); expect(t, '界é🌊X')
            t.send(key); t.send('Z'); expect(t, 'hard_keep'); expect(t, 'Z')
            assert '界é🌊' not in unicodedata.normalize('NFC', t.text)

        # Caption preview uses its own display width and the same click/edit behavior.
        reset(t, 'caption_keep\n界e\u0301🌊')
        fixture = str(ROOT / 'tests/harbor/fixtures/new-shelf.md')
        t.send('\x1b[200~' + fixture + '\x1b[201~', wait=.4)
        t.expect('Preview attachments')
        x, y = locate(t, '界é🌊')
        click(t, x + 4, y); t.send('X'); expect(t, '界é🌊X')
        t.send(KEYS[1]); expect(t, 'caption_keep')
        assert 'Preview attachments' in t.text and 'Delete message' not in t.text
        t.send(b'\x1b'); t.clear_draft(4); t.send(KEYS[0]); t.send(KEYS[1])
        t.expect('sent_keep11044')
        print(f'PASS composer display rows at {cols} columns: legacy/enhanced keys, hard/soft wraps, '
              'row-end click+insert/delete, Unicode, caption, sent message preserved')
    finally:
        t.close()

# Read the application's saved draft to verify every source character and newline,
# rather than inferring preservation from only the visible excerpt.
for cols in (34, 50, 112):
    for operation in ('backspace', 'legacy-row-delete', 'enhanced-row-delete'):
        with tempfile.TemporaryDirectory(prefix='harbor-composer-source-') as directory:
            t = Terminal(cols=cols, rows=38, config_dir=directory)
            try:
                t.picker('Studio')
                source = 'hard_keep\n' + 'abcdefghij' * 23 + 'TAIL_END'
                t.send('\x1b[200~' + source + '\x1b[201~')
                y = t.screen.cursor.y
                previous = t.screen.display[y-1].rstrip()
                x = next(i for i, ch in enumerate(previous) if ch in 'abcdefghij')
                final = t.screen.display[y][x:].strip()
                width = len(previous) - x
                boundary = len(source) - len(final)
                click(t, len(previous)-1, y-1)
                if operation == 'backspace':
                    t.send(b'\x7f')
                    expected = source[:boundary-1] + source[boundary:]
                    cursor = boundary-1
                else:
                    t.send(KEYS[operation == 'enhanced-row-delete'])
                    expected = source[:boundary-width] + source[boundary:]
                    cursor = boundary-width
                assert 'Delete message' not in t.text
            finally:
                t.close()
                t.wait_backend_shutdown()
            records = Path(directory, 'harbor-state').read_text().splitlines()
            active = next(row.split()[1] for row in records if row.startswith('active '))
            fields = next(row.split() for row in records if row.split()[0] == active)
            actual = bytes.fromhex(fields[1]).decode() if fields[1] != '-' else ''
            assert actual == expected, (cols, operation, actual, expected)
            assert int(fields[2]) == cursor, (cols, operation, fields[2], cursor)
        print(f'PASS exact saved draft at {cols} columns after row-end click + {operation}')

# Run the viewport regression in this existing hosted native composer lane.
from composer_wheel_smoke import check as check_composer_wheel
check_composer_wheel()
