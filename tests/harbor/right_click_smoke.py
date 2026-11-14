"""Right-click exposes only Reply, Forward, Copy text; left drag keeps copying."""
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
    for label in ['Edit message','Delete message','React with a heart','Open / play attachment','Open link']:
        assert label not in t.text

t=Terminal(cols=50,rows=44,extra_env={'HARBOR_TEST_PASTEBOARD':name})
try:
    t.picker('Studio');t.send('Unsent draft')
    menu(t,'new layout is ready')
    t.snapshot('harbor-right-click-50')
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
    assert 'Edit message' not in t.text and 'React with a heart' not in t.text
    print('PASS left click stays quiet; right-click/Enter show exactly three actions; reply/forward/copy and drafts')
finally:
    t.close()
    subprocess.run(['/usr/bin/osascript','-l','JavaScript','-e',clear,name],capture_output=True,timeout=5)
