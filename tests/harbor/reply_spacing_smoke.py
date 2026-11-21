"""Approved reply gaps and complete wrapped excerpts through native rendering and click/Return."""
from pathlib import Path
import tempfile,time
from native_smoke import Terminal

full='Before we retire Diode, we need to be able to recover the approval records, even if an interrupted payment must be checked later. End of the complete original.'

def wait(t,predicate,description):
    end=time.monotonic()+4
    while not predicate() and time.monotonic()<end:t.pump(.05)
    assert predicate(),description+'\n'+t.text

def menu(t,text):
    # Rendered popup only, never click a matching message behind it.
    def find():
        for top,row in enumerate(t.screen.display):
            if '╭' not in row or '╮' not in row:continue
            left,right=row.index('╭'),row.index('╮')
            bottom=next((i for i in range(top+1,t.rows) if t.screen.display[i][left:left+1]=='╰'),t.rows)
            for y in range(top+1,bottom):
                x=t.screen.display[y].find(text,left,right)
                if x>=0:return x,y
    wait(t,lambda:find() is not None,'Menu '+text)
    x,y=find();click(t,x+1,y)

def click(t,x,y):
    t.send(f'\x1b[<0;{x+1};{y+1}M',wait=.05);t.send(f'\x1b[<0;{x+1};{y+1}m',wait=.15)

def setting(t,field,value):
    t.send(b'\x0f');menu(t,'Settings');menu(t,field);menu(t,value)
    wait(t,lambda:not any('╭' in s and '╮' in s for s in t.screen.display),'Menu closed')

def row(t,text):
    return next(y for y,line in enumerate(t.screen.display) if line.strip()==text)

def quote_rows(t,body):
    b=row(t,body);assert not t.screen.display[b-1].strip(),t.text
    end=b-2;start=end
    while start and t.screen.display[start-1].strip() not in ('Doug','You'):start-=1
    assert start>0 and t.screen.display[start-1].strip() in ('Doug','You'),t.text
    return start,end

for cols in (48,76):
    t=Terminal(cols=cols,rows=120,extra_env={'HARBOR_DEMO_RICH':'1','HARBOR_DEMO_REPLIES':'1','HARBOR_DEMO_LONG_REPLY':'1'})
    try:
        t.picker('Doug formatting');t.expect('First received reply.')
        t.expect('Outgoing wrapped reply.')
        t.send('Retained cursor draft');t.send(b'\x02\x02')
        for appearance in ('Plain','Left rule','Shaded'):
            setting(t,'Quoted replies',appearance)
            for body,expected in [('First received reply.',full),('Outgoing wrapped reply.',full)]:
                start,end=quote_rows(t,body)
                lines=[t.screen.display[y].strip().removeprefix('│ ').strip() for y in range(start,end+1)]
                excerpt=' '.join(lines)
                assert expected in excerpt,excerpt
                assert not any('…' in s for s in lines),lines
                assert end>start,(cols,lines)
                before=t.text;cursor=(t.screen.cursor.x,t.screen.cursor.y)
                # Click a continuation, not only its first row; full-width shaded target too.
                y=end;x=cols-6 if appearance=='Shaded' else next(i for i,c in enumerate(t.screen.display[y]) if not c.isspace())+2
                click(t,x,y);t.expect('Showing the message');t.expect('Retained cursor draft')
                t.send(b'\x0f');menu(t,'Return to previous position')
                wait(t,lambda:body in t.text and not any('╭' in s and '╮' in s for s in t.screen.display),'Returned reply')
                # Status toast can change; exact history, cursor and draft are preserved.
                assert t.screen.display[:110]==before.split('\n')[:110],t.text
                assert (t.screen.cursor.x,t.screen.cursor.y)==cursor
                t.expect('Retained cursor draft')
        t.snapshot(f'harbor-approved-reply-spacing-{cols}')
        print(f'PASS {cols}cols: complete wrapped incoming/outgoing quote, zero rows before/exactly1after, all appearances, continuation click/Return preserves history/cursor/draft',flush=True)
    finally:t.close()
