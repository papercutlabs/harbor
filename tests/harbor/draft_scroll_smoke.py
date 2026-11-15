"""Draft editing does not change following or the reading position."""
from native_smoke import Terminal

t=Terminal(cols=56,rows=38)
try:
    t.picker('Saved Messages');t.expect('Note 35:');t.send(b'\x1b[5~')
    before=[row.strip() for row in t.screen.display if 'Note ' in row]
    assert before and 'Note 35:' not in t.text
    t.send('draft while reading history')
    assert [row.strip() for row in t.screen.display if 'Note ' in row]==before
    t.send(b'\x1b[13;2u');t.send('second line')
    assert 'Note 35:' not in t.text
    t.send(b'\x1b[200~ and pasted text\x1b[201~')
    assert 'Note 35:' not in t.text
    t.picker('Studio');t.picker('Saved Messages')
    t.expect(before[0]);t.expect('draft while reading history')
    t.send(b'\x07');t.expect('Note 35:');t.expect('draft while reading history')
finally:t.close()

t=Terminal(extra_env={'HARBOR_DEMO_INCOMING_DELAY_MS':'3500'})
try:
    t.picker('Marina');t.send(b'\x1b[5~');t.send('draft while a reply arrives')
    t.pump(3)
    assert 'A live incoming reply.' not in t.text
    t.expect('new · Ctrl+G');t.expect('draft while a reply arrives')
    t.send(b'\x07');t.expect('A live incoming reply.');t.expect('draft while a reply arrives')
finally:t.close()
print('PASS typing/newline/paste preserve history; incoming reply keeps unread action; Latest is explicit')
