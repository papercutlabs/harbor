"""Option word movement through the real decoder and composer in offline Harbor."""
from pathlib import Path
import sys
import tempfile
import unicodedata
from native_smoke import Terminal


def composer(t, expected):
    # pyte combines a base and combining accent into its displayed NFC character.
    shown = unicodedata.normalize('NFC', t.text)
    assert unicodedata.normalize('NFC', expected) in shown, (expected, t.text)


def reset(t, text=''):
    t.send(b'\x05\x15')  # End, then delete back to start: independent of previous cursor.
    if text:
        t.send(text)


with tempfile.TemporaryDirectory(prefix='harbor-word-navigation-') as directory:
    work = Path(directory)
    child = work / 'view.py'
    child.write_text('import sys\nprint("WORD VIEW", flush=True)\nsys.stdin.readline()\n')
    commands = work / 'commands'
    commands.write_text('\t'.join(['Words', 'Offline view', sys.executable, str(child)]) + '\n')
    t = Terminal(cols=76, extra_env={'HARBOR_DESK_COMMANDS': str(commands)})
    try:
        t.picker('Studio')
        pairs = [
            (b'\x1bb', b'\x1bf'),  # legacy macOS Option-arrows
            (b'\x1b[1;3D', b'\x1b[1;3C'),  # modified CSI arrows
            (b'\x1b[57350;3u', b'\x1b[57351;3u'),  # enhanced arrows
            (b'\x1b[57350;3:2u', b'\x1b[57351;3:2u'),  # enhanced repeat
            (b'\x1b[98;3u', b'\x1b[102;3u'),  # enhanced Alt-b/f
            (b'\x1b[27;3;98~', b'\x1b[27;3;102~'),
        ]
        for left, right in pairs:
            # Empty/start/end are stable, punctuation belongs to the same word.
            reset(t)
            t.send(left + right + left)
            t.send('one  café 你好!')
            t.send(left)
            t.send('|')
            composer(t, 'one  café |你好!')
            t.send(b'\x01')
            t.send(left * 2 + right)
            t.send('^')
            composer(t, 'one^  café |你好!')
            t.send(b'\x05' + right * 2)
            t.send('$')
            composer(t, 'one^  café |你好!$')
            assert 'Message actions' not in t.text

        # Combining accents and wide characters remain intact; plain arrows still move one display character.
        reset(t, 'one cafe\u0301 界界')
        t.send(b'\x1bb\x1bb')
        t.send('|')
        composer(t, 'one |cafe\u0301 界界')
        t.send(b'\x1b[C')
        t.send('^')
        composer(t, 'one |c^afe\u0301 界界')
        # Paste remains literal; a word jump crosses spaces/newlines without inserting escape bytes.
        reset(t)
        t.send('\x1b[200~one  \n  two\x1b[201~')
        t.send(b'\x1bb\x1bb')
        t.send('|')
        composer(t, '|one')
        t.send(b'\x1bf\x1bf')
        t.send('$')
        composer(t, 'two$')

        # Other modified arrows keep their existing one-character movement, never word movement.
        for arrow in (b'\x1b[1;5D', b'\x1b[1;7D', b'\x1b[57350;9u', b'\x1b[57350;4u'):
            reset(t, 'one two')
            t.send(arrow)
            t.send('|')
            composer(t, 'one tw|o')
        reset(t, 'one two')
        t.send(b'\x1b[98;7u')  # Ctrl+Alt+b remains an unbound command, never Alt+b.
        t.send('|')
        composer(t, 'one two|')

        # Word movement from message browsing returns focus to the retained composer.
        reset(t, 'one two')
        t.send(b'\x1b')
        t.send(b'\x1bb')
        t.send('|')
        composer(t, 'one |two')
        reset(t, 'one two|')

        # Pickers must not turn internal word-navigation actions into printable filter characters.
        t.send(b'\x0b')
        t.expect('Find a conversation')
        t.send('Studio')
        t.send(b'\x1bb\x1bf\x1b[1;3D\x1b[1;3C')
        t.send(b'\r')
        assert 'Find a conversation' not in t.text, t.text
        composer(t, 'one two|')

        # A current cursor survives the actual terminal-owner handoff and remains editable on return.
        reset(t, 'one two three')
        t.send(b'\x1bb')
        t.send(b'\x0f')
        t.expect('Desk · Words')
        t.send(b'\x1bb\x1bf')  # Commands ignores composer movement and still selects the same entry.
        t.send(b'\r')
        t.expect('WORD VIEW')
        saved = (Path(t.config) / 'harbor-state').read_text()
        row = next(row.split(' ') for row in saved.splitlines()
                   if not row.startswith('active ') and row.split(' ')[1].lower() == 'one two three'.encode().hex())
        assert row[2] == '8', row
        t.send(b'\r')
        t.expect('Back from Desk')
        t.send('|')
        composer(t, 'one two |three')
        t.send(b'\x1bf')
        t.send('$')
        composer(t, 'one two |three$')

        # Attachment captions share composer word navigation, including legacy Alt-b/f.
        reset(t, 'caption two')
        fixture = str(Path(__file__).resolve().parent / 'fixtures/harbor.png')
        t.send('\x1b[200~' + fixture + '\x1b[201~', wait=.4)
        t.expect('Send 1 attachment')
        t.send(b'\x1bb')
        t.send('|')
        composer(t, 'caption |two')
        t.send(b'\x1bf')
        t.send('$')
        composer(t, 'caption |two$')
        print('PASS word arrows/Alt-b/f, boundaries, whitespace, punctuation, Unicode, modifiers, paste, menus, caption and Desk cursor retention')
    finally:
        t.close()
