"""Formatting colors survive wrapping; short chats sit above the composer."""
from native_smoke import Terminal

t = Terminal(cols=50, rows=50)
try:
    t.picker('Kleya')
    def assert_bottom():
        y = next(y for y,row in enumerate(t.screen.display) if 'reach.' in row)
        prompt = next(y for y,row in enumerate(t.screen.display) if '›' in row)
        assert prompt - y == 5, (y, prompt, t.text)
    assert_bottom()
    t.picker('Studio'); t.picker('Kleya'); assert_bottom()
    t.resize(50, 38); assert_bottom()
    t.send('*Bold emphasis* and `green code` and _italic_.'); t.send(b'\r')
    t.expect('Bold emphasis and green code and italic.')
    assert '*Bold emphasis*' not in t.text
    row = next(y for y,r in enumerate(t.screen.display) if 'Bold emphasis' in r)
    line = t.screen.display[row]
    bold = t.screen.buffer[row][line.index('Bold')]
    code = t.screen.buffer[row][line.index('green')]
    plain = t.screen.buffer[row][line.index('and')]
    assert bold.bold and bold.fg != plain.fg
    assert code.fg != plain.fg and code.fg != bold.fg
    t.send('A plain prefix that will wrap *a blue phrase across the next line* with ordinary words after.'); t.send(b'\r')
    t.expect('phrase across')
    assert '*a blue' not in t.text
    assert 'Bold emphasis' in t.text
    assert 'Everything else within reach.' in t.text
    # The colored range continues across wrapped rows.
    for word in ['blue', 'phrase', 'across', 'next', 'line']:
        y = next(y for y,r in enumerate(t.screen.display) if word in r and 'prefix' in '\n'.join(t.screen.display[max(0,y-3):y+1]))
        x = t.screen.display[y].index(word)
        assert t.screen.buffer[y][x].bold, word
    t.snapshot('harbor-compact-formatting-50')
    t.send(b'\x1b'); t.send(b'\r'); t.expect('Message actions')
    t.send(b'\x1b')
    # Preserve older history while switching, including after rich rendering.
    t.picker('Saved Messages'); t.send(b'\x1b[5~')
    previous = t.screen.display[6:15]
    t.picker('Kleya'); t.picker('Saved Messages')
    assert t.screen.display[6:15] == previous
    print('PASS bottom anchored on chat switch/resize; native formatting colors and wrapping; saved reading position')
finally:
    t.close()
