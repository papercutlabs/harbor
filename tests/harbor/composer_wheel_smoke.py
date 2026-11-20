"""Browse wrapped drafts without moving the caret, text or conversation history."""
import tempfile
from pathlib import Path
from native_smoke import Terminal


def wheel(t, down=False, ticks=1, x=None, y=None):
    x = t.cols // 2 if x is None else x
    y = t.rows - 5 if y is None else y
    t.send((f'\x1b[<{65 if down else 64};{x+1};{y+1}M' * ticks).encode())


def view(t):
    y = next(y for y, row in enumerate(t.screen.display) if '›' in row)
    x = (25 if t.cols >= 100 else 0) + 6
    width = t.cols - (25 if t.cols >= 100 else 0) - 11
    rows = t.rows - 3 - y
    return y, [t.screen.display[y+i][x:x+width].rstrip() for i in range(rows)]


def history_rows(t):
    y, _ = view(t)
    return [(i, row.rstrip()) for i, row in enumerate(t.screen.display[:y-1]) if i >= 3 and row.strip()]


def saved(config):
    records = Path(config, 'harbor-state').read_text().splitlines()
    active = next(row.split()[1] for row in records if row.startswith('active '))
    fields = next(row.split() for row in records if row.split()[0] == active)
    return bytes.fromhex(fields[1]).decode(), int(fields[2]), fields[4]


def check():
    for cols, rows in ((34, 38), (72, 38), (112, 38), (50, 16)):
        with tempfile.TemporaryDirectory(prefix='harbor-composer-wheel-') as config:
            t = Terminal(cols=cols, rows=rows, config_dir=config)
            try:
                t.picker('Saved Messages'); t.expect('Note 35:')
                empty_y, empty = view(t)
                assert len(empty) == 2, ('minimum height', t.text)
                width = cols - (25 if cols >= 100 else 0) - 11
                t.send('\x1b[200~' + 'a' * (width * 2 + 4) + '\x1b[201~')
                assert view(t)[0] == empty_y - 1, ('growth from wrapped text', t.text)
                t.clear_draft()
                source = ''.join(f'R{i:02d}' + 'a' * (width - 3) for i in range(14)) + 'END'
                t.send('\x1b[200~' + source + '\x1b[201~')
                cap = max(2, min(5, rows // 4))
                y, bottom = view(t)
                assert len(bottom) == cap, ('height cap', cap, t.text)
                assert bottom[0].startswith(f'R{15-cap:02d}') and bottom[-1] == 'END', bottom
                history = history_rows(t)
                assert history, t.text
                wheel(t)
                assert view(t)[1][0].startswith(f'R{12-cap:02d}'), ('one wheel tick', t.text)
                assert history_rows(t) == history, 'draft wheel moved history'
                wheel(t, ticks=200)
                assert view(t)[1][0].startswith('R00'), ('top clamp', t.text)
                assert t.screen.cursor.hidden, 'offscreen caret must not move onto visible draft text'
                top = view(t)
                t.pump(.25)
                assert view(t) == top, 'redraw reset the draft viewport to the caret'
                wheel(t, down=True, ticks=200)
                assert view(t)[1] == bottom, ('bottom clamp', t.text)
                wheel(t)
                assert view(t)[1][0].startswith(f'R{12-cap:02d}'), 'reversal after bottom clamp'
                assert history_rows(t) == history, 'draft scrolling moved history'
                # The whole composer surface consumes wheel input even if no movement is possible.
                wheel(t, ticks=200, y=y-1)
                wheel(t, ticks=200, y=rows-3)
                assert view(t)[1][0].startswith('R00') and history_rows(t) == history
            finally:
                t.close()
            actual, cursor, follow = saved(config)
            assert actual == source and cursor == len(source), ('wheel edited draft/caret', actual, cursor)
            assert follow == '1', 'draft wheel disabled following conversation history'
        print(f'PASS composer wheel {cols}x{rows}: wrapped growth/cap; both boundaries; reversal; '
              'redraw holds viewport; exact saved text/caret; history and follow unchanged', flush=True)

    with tempfile.TemporaryDirectory(prefix='harbor-composer-follow-') as config:
        t = Terminal(cols=72, rows=38, config_dir=config)
        try:
            t.picker('Saved Messages'); t.expect('Note 35:')
            source = '\n'.join(f'ROW{i:02d} 界e\u0301' for i in range(15))
            t.send('\x1b[200~' + source + '\x1b[201~')
            wheel(t, ticks=200)
            assert view(t)[1][0].startswith('ROW00')
            t.send('X')
            assert view(t)[1][-1].endswith('X') and not t.screen.cursor.hidden, 'typing did not follow caret'
            wheel(t, ticks=200)
            t.send(b'\x1bOD')
            assert view(t)[1][-1].startswith('ROW14') and not t.screen.cursor.hidden, 'cursor movement did not follow'
            wheel(t, ticks=200)
            t.resize(34, 30)
            assert view(t)[1][-1].startswith('ROW14') and not t.screen.cursor.hidden, 'resize did not follow caret'
            wheel(t, ticks=200)
            y, _ = view(t)
            t.send(f'\x1b[<0;9;{y+1}M')
            t.send(f'\x1b[<0;9;{y+1}m')
            t.send('Z')
            source = source[:2] + 'Z' + source[2:] + 'X'
            assert 'ROZW00' in view(t)[1][0], ('click in scrolled draft', t.text)
            # A wheel event over history keeps the draft viewport and still moves the history.
            before = history_rows(t)
            wheel(t, ticks=2, x=15, y=10)
            assert history_rows(t) != before, 'wheel outside composer stopped scrolling history'
            assert 'ROZW00' in view(t)[1][0], 'history wheel moved composer viewport'
            t.picker('Studio'); t.picker('Saved Messages')
            assert 'ROZW00' in view(t)[1][0], 'conversation switch lost the caret row'
        finally:
            t.close()
        actual, cursor, _ = saved(config)
        assert actual == source and cursor == 3, ('follow/edit changed source unexpectedly', actual, cursor, source)
    print('PASS Unicode draft: typing, arrows, resize and clicking resume caret following; '
          'outside wheel scrolls history; chat switch preserves exact draft/caret', flush=True)


if __name__ == '__main__':
    check()
