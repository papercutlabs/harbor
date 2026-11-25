"""Option+Backspace editing through real legacy and enhanced key sequences."""
from native_smoke import Terminal, ROOT

t = Terminal(cols=50)
try:
    t.picker('Studio')
    keys = [b'\x1b\x7f', b'\x1b\x08', b'\x1b[127;3u', b'\x1b[8;3u',
            b'\x1b[27;3;127~', b'\x1b[27;3;8~', b'\x17']
    for key in keys:
        t.send(b'\x15'); t.send('keep café 你好   '); t.send(key)
        row = next(row for row in t.screen.display if '›' in row)
        assert row.strip() == '› keep café', (key, row)
        t.send(key)
        row = next(row for row in t.screen.display if '›' in row)
        assert row.strip() == '› keep', (key, row)
        t.send(key); t.send(key)
        assert 'Message actions' not in t.text
    # Delete before cursor without touching the suffix.
    t.send('keep delete tail')
    for _ in range(5): t.send(b'\x1bOD', wait=.02)
    t.send(b'\x1b\x7f')
    assert any('› keep  tail' in row for row in t.screen.display)
    t.send(b'\x15'); t.send(b'\x05'); t.send(b'\x15')
    t.send('caption unwanted')
    fixture = str(ROOT / 'tests/harbor/fixtures/new-shelf.md')
    t.send('\x1b[200~' + fixture + '\x1b[201~', wait=.4)
    t.send(b'\x1b[127;3u')
    assert 'unwanted' not in t.text
    t.click_text('Send 1 attachment')
    t.expect('caption')
    assert 'unwanted' not in t.text
    print('PASS Option+Backspace legacy/CSI-u/xterm, Ctrl+W, Unicode, middle-of-draft, empty draft, caption')
finally:
    t.close()
