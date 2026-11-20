"""Reply quote clicks use the native jump/fetch/return route in disposable demos.
No principal profiles, network accounts, clipboard or external opener are used.
"""
import json
import tempfile
import time
from pathlib import Path
from native_smoke import Terminal

work=Path(tempfile.mkdtemp(prefix='harbor-reply-click-'))

def frame(t):
    for y,line in enumerate(t.screen.display):
        if '╭' in line and '╮' in line:
            left,right=line.index('╭'),line.index('╮')
            bottom=next((yy for yy in range(y+1,t.rows) if t.screen.display[yy][left:left+1]=='╰'),None)
            if bottom is not None:return left,y,right,bottom
    return None

def wait(t,predicate,description):
    deadline=time.monotonic()+3
    while not predicate() and time.monotonic()<deadline:t.pump(.05)
    assert predicate(),description+'\n'+t.text

def menu_text(t,text):
    bounds=frame(t)
    if bounds:
        left,top,right,bottom=bounds
        for y in range(top+1,bottom):
            x=t.screen.display[y].find(text,left+1,right)
            if x>=0:return x,y
    return None

def menu_click(t,text):
    wait(t,lambda:menu_text(t,text) is not None,'Menu item '+text)
    x,y=menu_text(t,text)
    mouse(t,(x+1,y),'down');mouse(t,(x+1,y),'up')

def ready(t):
    wait(t,lambda:frame(t) is None,'Popup must finish closing before reading history')

def settings(t, field, value):
    t.send(b'\x0f');menu_click(t,'Settings')
    wait(t,lambda:menu_text(t,'Quoted replies') is not None,'Settings must be rendered')
    menu_click(t,field)
    wait(t,lambda:menu_text(t,'Appearance') is not None,'Appearance must be rendered')
    menu_click(t,value);ready(t)
    prefs=Path(t.config,'harbor-appearance').read_text()
    key,values=('quote',('Plain','Left rule','Shaded')) if field=='Quoted replies' else ('messages',('Panels','Plain'))
    assert f'{key} {values.index(value)}\n' in prefs,prefs

def body(t,text):
    t.expect(text)
    main=25 if t.cols>=100 else 0
    for y,line in enumerate(t.screen.display):
        content=line[main:]
        if content.strip()==text:
            return main+next(x for x,c in enumerate(content) if not c.isspace()),y
    raise AssertionError(f'No body for {text!r}:\n{t.text}')

def quote(t,text,style='Plain'):
    _,y=body(t,text)
    line=t.screen.display[y-1]
    assert ':' in line or 'original' in line.lower(),line
    x=next(x for x,c in enumerate(line) if not c.isspace())
    if style=='Shaded':
        # The right part of the shaded quote row belongs to the quote too.
        x=t.cols-6
    return x,y-1

def mouse(t,p,action):
    x,y=p
    t.send(f'\x1b[<{32 if action=="drag" else 0};{x+1};{y+1}{"m" if action=="up" else "M"}',wait=.1)

def visits(t,state):
    path=state/'harbor/operational.jsonl'
    return sum(r['event']=='ui.reply.visit' and r['pid']==t.pid
               for r in (json.loads(x) for x in path.read_text().splitlines()))

def history(t):
    ready(t)
    prompt=next(y for y,line in enumerate(t.screen.display) if '›' in line)
    return t.screen.display[3:prompt-2]

def return_back(t):
    t.send(b'\x0f');menu_click(t,'Return to previous position');ready(t)

def jump(t,source,target,draft,state,style='Plain'):
    p=quote(t,source,style)
    before=history(t);cursor=(t.screen.cursor.x,t.screen.cursor.y)
    main=25 if t.cols>=100 else 0
    previous_target_bg=next((t.screen.buffer[y][main+next(x for x,c in enumerate(line[main:]) if not c.isspace())].bg
                             for y,line in enumerate(t.screen.display) if line[main:].strip()==target),None)
    count=visits(t,state)
    mouse(t,p,'down');assert visits(t,state)==count,'Jump must wait for release'
    mouse(t,p,'up');t.expect('Showing the message')
    tx,ty=body(t,target)
    if previous_target_bg is not None:
        assert t.screen.buffer[ty][tx].bg != previous_target_bg,'Original must be selected'
    assert visits(t,state)>count
    t.expect(draft)
    return_back(t);body(t,source);t.expect(draft)
    assert history(t)==before,(before,history(t))
    assert (t.screen.cursor.x,t.screen.cursor.y)==cursor

for cols in (52,110):
    state=work/f'state-{cols}';requests=work/f'requests-{cols}'
    t=Terminal(cols=cols,rows=85,extra_env={
        'HARBOR_DEMO_RICH':'1','HARBOR_DEMO_REPLIES':'1',
        'HARBOR_DEMO_FETCH_DELAY_MS':'150','HARBOR_DEMO_REQUEST_LOG':str(requests),
        'HARBOR_TEST_CLIPBOARD_FILE':str(work/f'clipboard-{cols}'),
        'XDG_STATE_HOME':str(state)})
    try:
        t.picker('Studio');t.send('Studio draft retained')
        t.picker('Doug formatting');t.expect('Cross-chat received reply.')
        # Generate an outgoing reply through the actual Reply + Send route.
        t.click_text('Cross-chat received reply.',button=2);t.click_text('Reply')
        t.send('Outgoing clickable reply.');t.send(b'\r');t.delivered('Outgoing clickable reply.')
        draft='Source draft retained'
        t.send(draft);t.send(b'\x02\x02\x02')
        for message_style in ('Panels','Plain'):
            settings(t,'Messages',message_style)
            for quote_style in ('Plain','Left rule','Shaded'):
                settings(t,'Quoted replies',quote_style)
                print(f'CASE {cols} {message_style} {quote_style}',flush=True)
                # Inspect older history, then return to this exact row and caret.
                t.send(b'\x07')
                t.send(b'\x1b[<64;20;20M')
                p=quote(t,'First received reply.',quote_style)
                count=visits(t,state)
                mouse(t,p,'down');mouse(t,(p[0]+1,p[1]),'drag');mouse(t,(p[0]+1,p[1]),'up')
                assert visits(t,state)==count,'A quote drag must not jump'
                # Clicking the reply body itself keeps its ordinary text behavior.
                p=body(t,'First received reply.')
                mouse(t,p,'down');mouse(t,p,'up')
                assert visits(t,state)==count
                jump(t,'First received reply.','Older parent outside the loaded page.',draft,state,quote_style)
                jump(t,'Outgoing clickable reply.','Cross-chat received reply.',draft,state,quote_style)
        # Cross-chat metadata must select the original, not a same-id local collision.
        before=history(t);p=quote(t,'Cross-chat received reply.')
        mouse(t,p,'down');mouse(t,p,'up');t.expect('Showing the message')
        body(t,'Correct cross-chat original.');t.expect('Studio draft retained')
        return_back(t);body(t,'Cross-chat received reply.');t.expect(draft)
        assert history(t)==before
        # Unknown/deleted/inaccessible originals give feedback without a jump or Return mark.
        for text in ('Legacy reply with lost chat metadata.','Reply to inaccessible original.','Reply to deleted original.'):
            before=history(t);p=quote(t,text)
            mouse(t,p,'down');mouse(t,p,'up');t.expect('original message is unavailable')
            assert history(t)==before and draft in t.text
            t.send(b'\x0f');assert 'Return to previous position' not in t.text;t.send(b'\x1b')
        assert any(line.startswith('get-message ') and 'preview=1' not in line
                   for line in requests.read_text().splitlines()),'Unloaded original must use the existing exact fetch'
        t.snapshot(f'harbor-reply-click-{cols}')
        print(f'PASS {cols} columns: incoming/outgoing quotes, all three quote and both message presentations, release-only click, drag/body negative controls, exact history/caret/drafts, cross-chat identity, unavailable originals',flush=True)
    finally:t.close()

# Click while the existing deduplicated preview read is pending; its answer completes the jump.
for cancel in (False,True):
    state=work/f'pending-{cancel}'
    t=Terminal(cols=52,rows=85,extra_env={
        'HARBOR_DEMO_RICH':'1','HARBOR_DEMO_REPLIES':'1',
        'HARBOR_DEMO_FETCH_DELAY_MS':'3000','XDG_STATE_HOME':str(state),
        'HARBOR_TEST_CLIPBOARD_FILE':str(work/f'pending-clipboard-{cancel}')})
    try:
        t.picker('Doug formatting');t.expect('First received reply.');t.send('Pending draft stays')
        p=quote(t,'First received reply.');mouse(t,p,'down');mouse(t,p,'up')
        t.expect('Loading the original message')
        if cancel:
            t.picker('Kleya');t.picker('Doug formatting');t.expect('First received reply.')
            t.expect('Doug: Older parent',timeout=5)
            assert 'Showing the message' not in t.text
            t.send(b'\x0f');assert 'Return to previous position' not in t.text;t.send(b'\x1b')
        else:
            t.expect('Showing the message',timeout=8);body(t,'Older parent outside the loaded page.')
            return_back(t);body(t,'First received reply.')
        t.expect('Pending draft stays')
        print('PASS pending original '+('cancelled on chat switch' if cancel else 'fetched and visited from the quote click'),flush=True)
    finally:t.close()
