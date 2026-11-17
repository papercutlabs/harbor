"""Regression checks for narrow layout, modal dismissal, and modified Return."""
from native_smoke import Terminal, ROOT

t = Terminal(cols=50, rows=38)
try:
    t.picker('Studio')
    t.send('first'); t.send(b'\x1b\r'); t.send('second')
    assert 'Message actions' not in t.text
    first = next(y for y, row in enumerate(t.screen.display) if 'first' in row)
    second = next(y for y, row in enumerate(t.screen.display) if 'second' in row)
    assert second == first + 1
    assert sum(row.count('›') for row in t.screen.display) == 1
    t.clear_draft(2)
    # Enter a draft whose final word would cross the right edge.
    t.send('one two three four five six seven eight nine ten')
    t.expect('eight nine ten')
    assert 'one two three four five six seven' in t.text
    assert t.screen.cursor.x == 6 + len('eight nine ten')
    t.send(b'\x1bOA')
    assert t.screen.cursor.y == first
    t.send(b'\x1bOB')
    assert t.screen.cursor.y == first + 1
    t.snapshot('harbor-refined-compose-50')
    t.send(b'\x15'); t.send(b'\x1b'); t.send(b'\r'); t.expect('Message actions')
    t.snapshot('harbor-refined-actions-50')
    # One outside click dismisses, consumes the event, and does not reopen actions.
    t.send(b'\x1b[<0;2;25M', wait=.06)
    t.send(b'\x1b[<0;2;25m', wait=.1)
    assert 'Message actions' not in t.text
    t.send(b'\x1b[13;2u')
    assert 'COMPOSE' in t.text
    assert 'Message actions' not in t.text
    t.send(b'\x15')
    fixture = str(ROOT / 'tests/harbor/fixtures/harbor.png')
    t.send('existing caption ')
    t.send('\x1b[200~' + fixture + '\x1b[201~', wait=.4)
    t.expect('Caption · type to add')
    t.send('edited in preview')
    t.snapshot('harbor-refined-caption-50')
    t.click_text('Send 1 attachment')
    t.expect('existing caption edited in preview')
    assert 'Preview attachments' not in t.text
    # Caption is part of the sent attachment, and the composer is cleared.
    assert all('existing caption' not in row for row in t.screen.display[-7:])
    print('PASS editable preview caption is delivered with attachment')
    print('PASS legacy and CSI-u modified Return, word wrapping, mapped cursor, single prompt, outside dismissal')
finally:
    t.close()
