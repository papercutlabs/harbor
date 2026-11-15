"""Mouse drag selects and copies on release to a private system pasteboard."""
import subprocess
import uuid
from native_smoke import Terminal

name = 'harbor-selection-test-' + uuid.uuid4().hex
read = "ObjC.import('AppKit'); function run(a) { var s=$.NSPasteboard.pasteboardWithName(a[0]).stringForType('public.utf8-plain-text'); return s.isNil() ? '' : ObjC.unwrap(s); }"
clear = "ObjC.import('AppKit'); function run(a) { $.NSPasteboard.pasteboardWithName(a[0]).clearContents; }"
def clipboard():
    return subprocess.check_output(['/usr/bin/osascript','-l','JavaScript','-e',read,name],text=True,timeout=5).removesuffix('\n')
def point(t,text):
    y = next(y for y,row in enumerate(t.screen.display) if text in row)
    for x in range(t.cols):
        if ''.join(t.screen.buffer[y][col].data for col in range(x,t.cols)).startswith(text):
            return x,y
    raise AssertionError('No terminal cell for '+text)

def drag(t,a,b):
    x,y=a; xx,yy=b
    t.send(f'\x1b[<0;{x+1};{y+1}M',wait=.05)
    assert 'Message actions' not in t.text
    t.send(f'\x1b[<32;{xx+1};{yy+1}M',wait=.1)
    assert 'Message actions' not in t.text
    t.send(f'\x1b[<0;{xx+1};{yy+1}m',wait=.4)
    t.expect('Copied selection')
    assert 'Message actions' not in t.text

t=Terminal(cols=50,rows=44,extra_env={'HARBOR_TEST_PASTEBOARD':name})
try:
    t.picker('Studio')
    x,y=point(t,'new layout')
    drag(t,(x,y),(x+len('new layout')-1,y))
    assert clipboard()=='new layout',repr(clipboard())
    drag(t,(x+len('new layout')-1,y),(x,y))
    assert clipboard()=='new layout'
    t.send('draft retained')
    x,y=point(t,'new layout');drag(t,(x,y),(x+2,y))
    assert clipboard()=='new'
    assert 'draft retained' in t.text
    t.send(b'\x1b')
    assert 'MESSAGES' not in t.text
    t.send(b'\x15')
    source='*Select this phrase* across a deliberately wrapped message, without adding terminal line breaks.\nNext paragraph stays separate.'
    t.send('\x1b[200~'+source+'\x1b[201~');t.send(b'\r')
    t.expect('Next paragraph stays separate.')
    a=point(t,'Select this phrase')
    x,y=point(t,'separate.')
    drag(t,a,(x+len('separate.')-1,y))
    assert clipboard()==source.replace('*',''),repr(clipboard())
    t.snapshot('harbor-drag-copy-50')
    t.send('你好 café e\u0301 🙂 final');t.send(b'\r');t.expect('final')
    y=next(y for y,row in enumerate(t.screen.display) if '你好' in row)
    x=next(x for x in range(t.cols) if t.screen.buffer[y][x].data=='你')
    drag(t,(x+1,y),(x+3,y))
    assert clipboard()=='你好',repr(clipboard())
    x=max(x for x in range(t.cols) if t.screen.buffer[y][x].data in ('e\u0301','é'))
    drag(t,(x,y),(x+1,y))
    assert clipboard()=='e\u0301 ',repr(clipboard())
    a=point(t,'Select this phrase')
    x,y=point(t,'final')
    drag(t,a,(x+len('final')-1,y))
    assert clipboard()==source.replace('*','')+'\n\n你好 café e\u0301 🙂 final',repr(clipboard())
    # Exact TDLib getMarkdownText shape: the closing fence follows the command directly.
    command='sh ~/Downloads/create-hive-next-repos.sh'
    block="It's in your Downloads now:\n\n```shell\n"+command+"```\n\nIt should end with Done."
    shown="It's in your Downloads now:\n\n"+command+"\n\nIt should end with Done."
    t.send('\x1b[200~'+block+'\x1b[201~');t.send(b'\r');t.expect('It should end with Done.')
    assert '```' not in t.text,t.text
    x,y=point(t,command);code=t.screen.buffer[y][x]
    px,py=point(t,'It should end');prose=t.screen.buffer[py][px]
    bx,by=point(t,"It's in your");before=t.screen.buffer[by][bx]
    assert code.fg!=prose.fg and prose.fg==before.fg,(code.fg,prose.fg,before.fg)
    drag(t,(x,y),(x+len(command)-1,y))
    assert clipboard()==command,repr(clipboard())
    t.click_text('It should end',button=2);t.expect('Message actions');t.click_text('Copy text');t.expect('Copied')
    assert clipboard()==shown,repr(clipboard())
    t.snapshot('harbor-code-block-copy-50')
    # Left-click does not open actions; right-click does.
    t.send(b'\x1b');t.click_text('final');assert 'Message actions' not in t.text
    t.click_text('final',button=2);t.expect('Message actions')
    print('PASS drag auto-copy, reverse selection, TDLib code block display and exact copy, no action popup, wrapping/newlines, Unicode, draft preservation, right-click actions')
finally:
    t.close()
    subprocess.run(['/usr/bin/osascript','-l','JavaScript','-e',clear,name],capture_output=True,timeout=5)
