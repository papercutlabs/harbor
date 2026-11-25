"""Composer clicks use visual layout positions, including wrap and Unicode."""
from native_smoke import Terminal

def click(t,x,y):
    t.send(f'\x1b[<0;{x+1};{y+1}M',wait=.05)
    t.send(f'\x1b[<0;{x+1};{y+1}m',wait=.1)

t=Terminal(cols=50,rows=38)
try:
    t.picker('Studio');t.send('hello world')
    y=t.screen.cursor.y
    click(t,8,y);t.send('X');t.expect('heXllo world')
    t.clear_draft();t.send('one two three four five six seven eight nine ten')
    y=next(y for y,row in enumerate(t.screen.display) if 'eight nine ten' in row)
    click(t,12,y);t.send('X');t.expect('eight Xnine ten')
    t.clear_draft();t.send('界e\u0301abc')
    y=t.screen.cursor.y
    click(t,8,y);t.send('X');t.expect('界Xéabc')
    click(t,10,y);t.send('Y');t.expect('界XéYabc')
    # A click after the last glyph goes to end, not the next visual line.
    click(t,43,y);t.send('Z');t.expect('YabcZ')
    t.clear_draft()
    content='\n'.join('line '+str(i) for i in range(1,11))
    t.send('\x1b[200~'+content+'\x1b[201~')
    y=next(y for y,row in enumerate(t.screen.display) if 'line 6' in row)
    click(t,8,y);t.send('X');t.expect('liXne 6')
    t.clear_draft(10);t.send('caption ')
    fixture=str(__import__('native_smoke').ROOT/'tests/harbor/fixtures/new-shelf.md')
    t.send('\x1b[200~'+fixture+'\x1b[201~',wait=.4)
    t.expect('Caption · type to add')
    y=next(y for y,row in enumerate(t.screen.display) if 'caption ' in row and 'Caption' not in row)
    x=t.screen.display[y].index('caption ')+3
    click(t,x,y);t.send('X');t.expect('capXtion ')
    assert 'Preview attachments' in t.text
    print('PASS composer click placement: wrapped rows, wide/combining text, end-of-line')
finally:t.close()
