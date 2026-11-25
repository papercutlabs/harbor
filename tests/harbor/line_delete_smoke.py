"""Cmd+Backspace / Ctrl+U delete only the current display row, through the real decoder."""
import unicodedata
from native_smoke import Terminal, ROOT

NEWLINE = b'\x1b[13;2u'
# Legacy terminals send Ctrl+U for Cmd+Backspace; enhanced ones report Super+Backspace.
KEYS = [b'\x15', b'\x1b[127;9u']


def draft(t, *lines):
    t.clear_draft(4)
    for i, line in enumerate(lines):
        if i:
            t.send(NEWLINE)
        t.send(line)


def row(t, text):
    shown = [unicodedata.normalize('NFC', r) for r in t.screen.display]
    return next((r for r in shown if unicodedata.normalize('NFC', text) in r), None)


t = Terminal(cols=50, rows=38)
try:
    t.picker('Studio')
    for key in KEYS:
        # End of the last line: earlier lines stay.
        draft(t, 'qalpha one', 'qbeta two', 'qgamma three')
        t.send(key)
        assert 'qgamma' not in t.text, (key, t.text)
        assert row(t, 'qalpha one') and row(t, 'qbeta two'), (key, t.text)
        # At a line start nothing is removed, not even the preceding newline.
        t.send(key)
        t.send('W')
        assert row(t, 'qbeta two') and 'W' not in row(t, 'qbeta two'), (key, t.text)
        assert row(t, 'qalpha one'), (key, t.text)

        # Middle of a line: text after the cursor and other lines stay; cursor at line start.
        draft(t, 'qkeep', 'qab cd tail')
        for _ in range(4):
            t.send(b'\x1bOD', wait=.02)
        t.send(key)
        t.send('Z')
        assert row(t, 'Ztail') and 'qab cd' not in t.text, (key, t.text)
        assert row(t, 'qkeep') and 'Z' not in row(t, 'qkeep'), (key, t.text)

        # First line and single-line drafts behave like before.
        draft(t, 'qonly line')
        t.send(key)
        assert 'qonly line' not in t.text, (key, t.text)

        # Unicode, combining and wide text on the current line only.
        draft(t, 'qkeep café', 'e\u0301 你好 🌊')
        t.send(key)
        assert '你好' not in t.text and '🌊' not in t.text, (key, t.text)
        assert row(t, 'qkeep café'), (key, t.text)

        # A wrapped logical line keeps preceding display rows and the hard line before it.
        long = 'qone two three four five six seven eight nine ten eleven twelve'
        draft(t, 'qhead', long)
        assert row(t, 'eleven twelve') and not row(t, long), t.text  # really wrapped
        t.send(key)
        assert 'qone two' in t.text and 'eleven twelve' not in t.text, (key, t.text)
        assert row(t, 'qhead'), (key, t.text)

    # Option+Backspace still deletes one word; plain Backspace one character.
    draft(t, 'qkeep', 'qalpha beta')
    t.send(b'\x1b[127;3u')
    assert row(t, 'qalpha') and 'beta' not in t.text and row(t, 'qkeep'), t.text
    t.send(b'\x7f'); t.send(b'\x7f'); t.send(b'\x7f')
    assert row(t, 'qalp') and not row(t, 'qalph') and row(t, 'qkeep'), t.text

    # Attachment preview caption: only the current caption line goes, preview stays.
    draft(t, 'qcaption head', 'qcaption tail ')
    fixture = str(ROOT / 'tests/harbor/fixtures/new-shelf.md')
    t.send('\x1b[200~' + fixture + '\x1b[201~', wait=.4)
    t.expect('Preview attachments')
    t.send(b'\x1b[127;9u')
    assert 'qcaption tail' not in t.text and 'qcaption head' in t.text, t.text
    assert 'Preview attachments' in t.text
    t.click_text('Send 1 attachment')
    t.expect('qcaption head')
    print('PASS Cmd+Backspace legacy Ctrl+U/Kitty Super+Backspace deletes only the current line: end, middle, '
          'line start, first line, Unicode, wrap; Option+Backspace, Backspace and captions unchanged')
finally:
    t.close()
