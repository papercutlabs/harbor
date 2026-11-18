"""The library-to-UI boundary preserves input semantics, not wire bytes."""
from native_smoke import Terminal

t=Terminal()
try:
    t.picker('Studio');t.send('abcd')
    for chunk in (b'\x1b',b'[',b'D'):t.send(chunk,wait=.02)
    t.send('X');t.expect('abcXd')
    t.send(b'\x1b[57350;1:3u');t.send('Y');t.expect('abcXYd')
    t.send(b'\x1b[99999z');t.send('!');t.expect('abcXY!d')
    t.send(b'\x1b[57350;1:2u');t.send('R');t.expect('abcXYR!d')
    assert '99999' not in t.text and '[D' not in t.text and '57350' not in t.text
    t.send(b'\x05');t.send(b'\x15')
    t.send('\x1b[200~literal [D and / Commands\nsecond line\x1b[201~')
    t.expect('literal [D and / Commands');t.expect('second line')
    assert 'Commands & shortcuts' not in t.text
    print('PASS fragmented keys, release/repeat, unknown reports, and literal multiline paste through UI')
finally:t.close()
