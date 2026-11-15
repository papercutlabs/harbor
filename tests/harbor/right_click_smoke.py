"""Right-click exposes only the primary actions (Reply, Forward, Copy text, plus Copy code and Go to replied
message when a message has them); every other action, including React…, stays in Message tools."""
import subprocess
import uuid
from native_smoke import Terminal

name='harbor-right-click-'+uuid.uuid4().hex
read="ObjC.import('AppKit'); function run(a) { var s=$.NSPasteboard.pasteboardWithName(a[0]).stringForType('public.utf8-plain-text'); return s.isNil() ? '' : ObjC.unwrap(s); }"
clear="ObjC.import('AppKit'); function run(a) { $.NSPasteboard.pasteboardWithName(a[0]).clearContents; }"
def menu(t,message):
    t.click_text(message)
    assert 'Message actions' not in t.text
    t.click_text(message,button=2)
    for label in ['Reply','Forward','Copy text']:t.expect(label)
    for label in ['Edit message','Delete message','React…','Open / play attachment','Open link','Open a link…',
                  'Reveal spoiler','Expand quote']:
        assert label not in t.text

t=Terminal(cols=50,rows=44,extra_env={'HARBOR_TEST_PASTEBOARD':name})
try:
    t.picker('Studio');t.send('Unsent draft')
    menu(t,'new layout is ready')
    t.snapshot('harbor-right-click-50')
    # After closing a right-click menu, left-clicking a different message must
    # clear whole-message focus rather than moving its highlight.
    t.send(b'\x1b')
    t.click_text('conversation should feel calm')
    assert 'MESSAGES' not in t.text
    y=next(y for y,row in enumerate(t.screen.display) if 'conversation should feel calm' in row)
    x=t.screen.display[y].index('conversation')
    actual=t.screen.buffer[y][x].bg
    # A different incoming message has its ordinary panel background.
    other=next(y for y,row in enumerate(t.screen.display) if 'new layout is ready' in row)
    xx=t.screen.display[other].index('new layout')
    assert actual==t.screen.buffer[other][xx].bg
    t.click_text('new layout is ready',button=2)
    t.expect('Message actions')
    t.click_text('Copy text');t.expect('Copied');t.expect('Unsent draft')
    copied=subprocess.check_output(['/usr/bin/osascript','-l','JavaScript','-e',read,name],text=True,timeout=5).removesuffix('\n')
    assert copied=='The new layout is ready to try.'
    menu(t,'new layout is ready');t.click_text('Reply');t.expect('Replying');t.expect('Unsent draft')
    t.send(b'\x1b');t.expect('Unsent draft')
    menu(t,'new layout is ready');t.click_text('Forward');t.expect('Forward to…')
    t.send('Kleya');t.send(b'\r');t.expect('Kleya');t.expect('new layout is ready')
    t.picker('Studio');t.expect('Unsent draft')
    menu(t,'Keep the details quiet')
    t.send(b'\x1b');t.send(b'\r');t.expect('Message actions')
    assert 'Edit message' not in t.text and 'React…' not in t.text
    # the absent labels are real actions: the full Message tools list for the same message has them
    t.send(b'\x1b');t.click_text('/ commands');t.click_text('More message tools');t.expect('Message tools')
    for label in ['Edit message','React…','Delete message']:t.expect(label)
    print('PASS left click stays quiet; right-click/Enter show only the primary actions; tools keep the rest; reply/forward/copy and drafts')
finally:
    t.close()
    subprocess.run(['/usr/bin/osascript','-l','JavaScript','-e',clear,name],capture_output=True,timeout=5)
