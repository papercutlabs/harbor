"""Offline PTY proof that popup option titles, descriptions and the selected row are visually distinct."""
import os
import sys
import time
from pathlib import Path

MARKER = '›'


def find_dialog(cells):
    """Return (top, left, width) of the dialog drawn with ╭…╮; cells are rows of (char, fg, bg, bold)."""
    for y, row in enumerate(cells):
        chars = [c[0] for c in row]
        if '╭' in chars:
            left = chars.index('╭')
            return y, left, chars.index('╮', left) - left + 1
    raise AssertionError('No dialog on screen')


def check_options(cells, options, selected, compact=False, surfaces=True):
    """Assert the hierarchy for every visible (label, hint) option; selected is the chosen label.

    surfaces=False is for terminals without 256 colours, where every pair falls back to the
    default background and the selection arrow alone distinguishes the selected option."""
    top, left, width = find_dialog(cells)
    right = left + width - 1
    text = lambda y, x, n: ''.join(c[0] for c in cells[y][x:x + n])
    seen = 0
    plain_title = plain_hint = selected_bg = plain_bg = None
    for label, hint in options:
        rows = [y for y in range(top + 1, len(cells)) if text(y, left + 2, len(label)) == label]
        if not rows:
            continue
        y = rows[0]
        seen += 1
        is_selected = label == selected
        title = cells[y][left + 2:left + 2 + min(len(label), width - 4)]
        assert all(c[3] for c in title if c[0] != ' '), f'title {label!r} is not bold'
        assert (cells[y][left + 1][0] == MARKER) == is_selected, f'selection marker wrong on {label!r}'
        inner = {c[2] for c in cells[y][left + 1:right]}
        assert len(inner) == 1, f'row {label!r} is not one surface: {inner}'
        if is_selected:
            selected_bg = inner.pop()
        else:
            plain_bg = inner.pop()
            plain_title = cells[y][left + 2][1]
        assert cells[y][right][0] == '│', f'title {label!r} overran the border'
        if compact or not hint:
            continue
        assert text(y + 1, left + 2, 2) == '  ', f'description of {label!r} is not indented'
        shown = hint[:width - 6]
        assert text(y + 1, left + 4, len(shown)) == shown, f'description of {label!r} missing'
        body = cells[y + 1][left + 4:left + 4 + len(shown)]
        assert not any(c[3] for c in body), f'description of {label!r} is bold'
        assert cells[y + 1][left + 1][0] == ' ', f'description of {label!r} has a second selection marker'
        assert cells[y + 1][right][0] == '│', f'description of {label!r} overran the border'
        assert not surfaces or body[0][1] != title[0][1], f'description of {label!r} is not muted'
        assert all(c[2] == cells[y][left + 2][2] for c in body), f'description of {label!r} lost the row surface'
        if not is_selected:
            plain_hint = body[0][1]
    assert seen >= 2, f'expected at least two visible options, saw {seen}'
    assert selected_bg is not None and plain_bg is not None, 'need a selected and an unselected option'
    assert not surfaces or selected_bg != plain_bg, 'selected row has the same surface as unselected rows'
    return plain_title, plain_hint


def pyte_cells(screen):
    return [[(screen.buffer[y][x].data, screen.buffer[y][x].fg, screen.buffer[y][x].bg, screen.buffer[y][x].bold)
             for x in range(screen.columns)] for y in range(screen.lines)]


def expect_options(terminal, options, selected, **kwargs):
    # Navigation and redraw are asynchronous. A fixed send() pump may still
    # contain the previous selection on a loaded runner. Keep the same visual
    # assertions and wait for their actual rendered result, with a deadline.
    deadline = time.monotonic() + 3
    while True:
        try:
            return check_options(pyte_cells(terminal.screen), options, selected, **kwargs)
        except AssertionError:
            if time.monotonic() >= deadline:
                raise
            terminal.pump(.02)


if __name__ == '__main__':
    from native_smoke import Terminal
    commands = Path(os.environ.get('TMPDIR', '/tmp')) / f'harbor-popup-{os.getpid()}'
    commands.write_text('\n'.join('\t'.join([label, 'Existing Desk view', sys.executable, '-c', 'pass'])
                                  for label in ['Everything', 'Quota']) + '\n')
    options = [('Desk · Everything', 'e · Existing Desk view'), ('Desk · Quota', 'q · Existing Desk view'),
               ('More message tools', 'For the selected message'), ('Chats', 'Ctrl+K'),
               ('Jump to latest messages', 'Ctrl+G')]
    try:
        for cols, rows in [(36, 38), (48, 38), (100, 38), (48, 16)]:
            t = Terminal(cols=cols, rows=rows, extra_env={'HARBOR_DESK_COMMANDS': str(commands)})
            try:
                t.send(b'\x0f'); t.expect('Commands & shortcuts')
                title, hint = expect_options(t, options, 'Desk · Everything')
                assert title != hint, f'{cols}x{rows}: description colour equals title colour'
                t.send(b'\x1b[B'); t.send(b'\x1b[B')
                expect_options(t, options, 'More message tools')
                t.send(b'\x1b')
                print(f'PASS {cols}x{rows} bold titles, muted indented descriptions, marked selected surface')
            finally:
                t.close()
        # Exercise ncurses' actual non-256-colour fallback: hierarchy must survive
        # without distinct selected/background surfaces or a muted colour pair.
        t = Terminal(cols=48, rows=38, extra_env={'TERM': 'xterm', 'HARBOR_DESK_COMMANDS': str(commands)})
        try:
            t.send(b'\x0f'); t.expect('Commands & shortcuts')
            expect_options(t, options, 'Desk · Everything', surfaces=False)
            t.send(b'\x1b[B'); t.send(b'\x1b[B')
            expect_options(t, options, 'More message tools', surfaces=False)
            print('PASS xterm 8-colour fallback retains title/hint weights, indent and title-only selection marker')
        finally:
            t.close()
        # Exercise the actual mouse hit target, compact menu navigation and picker.
        t = Terminal(cols=48, rows=38, extra_env={'HARBOR_DESK_COMMANDS': str(commands)})
        try:
            t.picker('Marina')
            t.send(b'\x0f'); t.click_text('More message tools'); t.expect('Message tools')
            # Compact options still have bold titles and a single full-width selected surface.
            cells = pyte_cells(t.screen)
            top, left, width = find_dialog(cells)
            labels = [('Edit message', ''), ('React…', ''), ('Delete message', '')]
            expect_options(t, labels, 'Edit message', compact=True)
            t.send(b'\x1b[B')
            expect_options(t, labels, 'React…', compact=True)
            t.send(b'\x1b'); t.send(b'\x0b'); t.expect('Find a conversation')
            cells = pyte_cells(t.screen)
            top, left, width = find_dialog(cells)
            # Picker keeps its existing arrow at left+2; no second gutter arrow is added.
            row = top + 5
            assert cells[row][left + 1][0] == ' '
            assert cells[row][left + 2][0] == MARKER
            assert all(c[3] for c in cells[row][left + 4:left + 10] if c[0] != ' ')
            assert all(c[3] for c in cells[row + 1][left + 4:left + 10] if c[0] != ' ')
            for r in [row, row + 1]:
                metadata = cells[r][left + width - 9:left + width - 2]
                assert not any(c[3] for c in metadata), 'picker metadata is bold'
                assert metadata[-1][1] != cells[r][left + 4][1], 'picker metadata is not muted'
            selected_bg = cells[row][left + 2][2]
            assert selected_bg != cells[row + 1][left + 2][2]
            t.send(b'\x1b[B')
            deadline = time.monotonic() + 3
            while t.screen.buffer[row + 1][left + 2].data != MARKER and time.monotonic() < deadline:
                t.pump(.02)
            moved = pyte_cells(t.screen)
            assert moved[row + 1][left + 2][0] == MARKER
            assert moved[row][left + 2][0] == ' '
            t.send(b'\x1b'); t.send(b'\x0b'); t.send('Marina'); t.expect('Marina')
            t.click_text('Marina')
            deadline = time.monotonic() + 3
            while 'Find a conversation' in t.text and time.monotonic() < deadline:
                t.pump(.02)
            assert 'Find a conversation' not in t.text
            print('PASS real menu title hit target, compact navigation, retained picker arrow/navigation and picker click')
        finally:
            t.close()
        t = Terminal(cols=100, rows=38)
        try:
            t.send(b'\x0b'); t.expect('Find a conversation')
            cells = pyte_cells(t.screen)
            top, left, width = find_dialog(cells)
            row = top + 5
            for r, label in [(row, 'Saved Messages'), (row + 1, 'Marina')]:
                assert ''.join(c[0] for c in cells[r][left + 4:left + 4 + len(label)]) == label
                preview = cells[r][left + 6 + len(label):left + 13 + len(label)]
                assert any(c[0] != ' ' for c in preview), 'missing picker preview'
                assert not any(c[3] for c in preview), 'picker preview is bold'
                assert preview[0][1] != cells[r][left + 4][1], 'picker preview is not muted'
                assert all(c[2] == cells[r][left + 4][2] for c in preview), 'picker preview lost the row surface'
            print('PASS real wide picker has muted non-bold previews on selected and unselected surfaces')
        finally:
            t.close()
    finally:
        commands.unlink()
