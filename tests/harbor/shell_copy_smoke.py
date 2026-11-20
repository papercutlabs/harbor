"""Copy code through native pointer hit testing and the real Clipboard boundary.

Only disposable demo data, a private clipboard file and a recording opener are
used. Commands remain text; the recording opener also catches unintended opens.
"""
import tempfile
import unicodedata
from pathlib import Path
from native_smoke import Terminal

work = Path(tempfile.mkdtemp(prefix='harbor-shell-copy-'))
clipboard, opened = work/'clipboard', work/'opened'
opener = work/'opener.sh'
opener.write_text('#!/bin/sh\nprintf "%s\\n" "$1" >> "'+str(opened)+'"\n')
opener.chmod(0o755)
short = "printf 'short'"
multiline = "printf '%s\\n' 'first line deliberately crosses a narrow pane'\n\tprintf '你好 é'\nprintf 'tail'\n"

def point(t, text):
    for y in range(t.rows):
        for x in range(t.cols):
            value = ''.join(t.screen.buffer[y][col].data for col in range(x,t.cols))
            if unicodedata.normalize('NFC',value).startswith(unicodedata.normalize('NFC',text)):
                return x,y
    raise AssertionError(f'Missing {text!r}:\n{t.text}')

def visible(t, text):
    t.send(b'\x07')
    for _ in range(8):
        if text in t.text: return
        t.send(b'\x1b[5~')
    t.expect(text)

def icon(t, label, index=0):
    visible(t,label)
    _,top = point(t,label)
    found = [(x,y) for y in range(top+1,t.rows) for x in range(t.cols)
             if t.screen.buffer[y][x].data == '⧉']
    assert len(found)>index,(label,found,t.text)
    return found[index]

def mouse(t, p, action):
    x,y=p
    t.send(f'\x1b[<{32 if action=="drag" else 0};{x+1};{y+1}{"m" if action=="up" else "M"}',wait=.1)

def copy(t, p, expected):
    clipboard.write_text('unchanged until release')
    mouse(t,p,'down')
    assert clipboard.read_text()=='unchanged until release'
    mouse(t,p,'up');t.expect('Copied code')
    assert clipboard.read_text()==expected,repr(clipboard.read_text())
    assert 'Message actions' not in t.text
    assert not opened.exists(),'Copy must not open a file, command or URL'

for cols in (32,56,110):
    t=Terminal(cols=cols,rows=50,extra_env={
        'HARBOR_DEMO_RICH':'1','HARBOR_DEMO_CODE_COPY':'1',
        'HARBOR_TEST_CLIPBOARD_FILE':str(clipboard),'HARBOR_TEST_OPENER':str(opener),
        'XDG_STATE_HOME':str(work/f'state-{cols}')})
    try:
        t.picker('Doug formatting');t.expect('Concealed code:')
        t.send('draft stays')
        p=icon(t,'Short command:');x,y=point(t,short)
        assert p[1]==y and p[0]>x+len(short),t.text
        assert '‹shell›' not in t.text
        copy(t,p,short)
        t.snapshot(f'harbor-shell-copy-short-{cols}')
        # Drag from the icon into the command: selection, not the block-copy action.
        mouse(t,p,'down');mouse(t,(x+7,y),'drag');mouse(t,(x+7,y),'up')
        t.expect('Copied selection')
        assert clipboard.read_text()==short[7:],repr(clipboard.read_text())
        assert '⧉' not in clipboard.read_text()
        # Ordinary command drag still copies without the icon or any UI prompt.
        mouse(t,(x,y),'down');mouse(t,(x+len(short)-1,y),'drag');mouse(t,(x+len(short)-1,y),'up')
        t.expect('Copied selection');assert clipboard.read_text()==short
        p=icon(t,'Multiline command:')
        copy(t,p,multiline)
        t.snapshot(f'harbor-shell-copy-multiline-{cols}')
        assert 'draft stays' in t.text
        # The icon never covers the first source glyph, even when the first line wraps.
        _,label_y=point(t,'Multiline command:')
        first_y=next(y for y in range(label_y+1,t.rows) if "printf '%s" in t.screen.display[y])
        if cols==32: assert p[1]==first_y-1,t.text
        p=icon(t,'Two code blocks:');copy(t,p,'echo first\n')
        p=icon(t,'Two code blocks:',1);copy(t,p,'echo second')
        p=icon(t,'Concealed code:');copy(t,p,"printf '▒▒▒▒▒▒'")
        assert 'secret' not in clipboard.read_text()
        assert 'draft stays' in t.text
        t.snapshot(f'harbor-shell-copy-{cols}')
    finally:
        t.close()
print('PASS shell copy at32/56/110cols: exact block, multiline/trailing newline/tab/Unicode, independent blocks, release-only click, drag, draft, concealed spoiler, no opener')
