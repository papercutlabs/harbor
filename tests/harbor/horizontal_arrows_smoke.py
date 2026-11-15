"""Horizontal arrows move the cursor without inserting their escape bytes."""
from native_smoke import Terminal

t=Terminal(cols=56,rows=38)
try:
    t.picker('Studio')
    for left,right in [(b'\x1b[D',b'\x1b[C'),(b'\x1bOD',b'\x1bOC'),
                       (b'\x1b[1;1D',b'\x1b[1;1C'),
                       (b'\x1b[57350u',b'\x1b[57351u'),
                       (b'\x1b[57350;1u',b'\x1b[57351;1u')]:
        t.send(b'\x15');t.send('abcd')
        t.send(left*2);t.send('X');t.send(right);t.send('Y')
        assert 'abXcYd' in t.text,(left,t.text)
        assert 'Message actions' not in t.text
    print('PASS CSI/SS3/CSI-u left and right arrows edit without escape text')
finally:t.close()
