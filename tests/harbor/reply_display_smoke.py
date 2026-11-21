"""Compact/full reply quotes and recognizable composer context in disposable native demos."""
from pathlib import Path
import sys,tempfile,time
from native_smoke import Terminal

env={'HARBOR_DEMO_RICH':'1','HARBOR_DEMO_REPLIES':'1','HARBOR_DEMO_LONG_REPLY':'1',
     'HARBOR_DEMO_REPLY_CONTEXT':'1'}
opaque='32363035373131333630'

def wait(t,predicate,label):
    end=time.monotonic()+4
    while not predicate() and time.monotonic()<end:t.pump(.05)
    assert predicate(),label+'\n'+t.text

def choose(t,field,value):
    t.send(b'\x0f');t.click_text('Settings');t.click_text(field);t.click_text(value)
    wait(t,lambda:not any('╭' in s and '╮' in s for s in t.screen.display),'Closed setting')

def quote_rows(t,body):
    b=next(y for y,line in enumerate(t.screen.display) if line.strip()==body)
    assert not t.screen.display[b-1].strip(),t.text
    end=b-2;start=end
    while start and t.screen.display[start-1].strip() not in ('Doug','You'):start-=1
    assert start and t.screen.display[start-1].strip() in ('Doug','You'),t.text
    return start,end

def banner(t):
    return next((s.strip() for s in t.screen.display if s.strip().startswith('Replying · ')),'')

if '--context-only' not in sys.argv:
  for cols in (48,76):
    with tempfile.TemporaryDirectory(prefix='harbor-reply-display-') as directory:
      config=Path(directory)/'config'
      bootstrap=Terminal(cols=cols,rows=120,config_dir=config,extra_env=env)
      bootstrap.close();bootstrap.wait_backend_shutdown()
      # Existing profiles without the new key also use the compact default.
      (config/'harbor-appearance').write_text('quote 1\nmessages 0\ntimestamp 1\ncommands 0\n')
      t=Terminal(cols=cols,rows=120,config_dir=config,extra_env=env)
      try:
        t.picker('Doug formatting');t.expect('Outgoing wrapped reply.')
        for body in ('First received reply.','Outgoing wrapped reply.'):
            start,end=quote_rows(t,body)
            assert start==end and t.screen.display[start].rstrip().endswith('…'),t.text
        t.send('kept compact draft');t.send(b'\x1b[D'*2)
        cursor=(t.screen.cursor.x,t.screen.cursor.y)
        for appearance in ('Plain','Left rule','Shaded'):
            choose(t,'Quoted replies',appearance)
            for length in ('Full quote','Single line'):
                choose(t,'Quote length',length)
                prefs=(config/'harbor-appearance').read_text()
                assert f'quote {("Plain","Left rule","Shaded").index(appearance)}\n' in prefs,prefs
                assert f'quote_length {int(length=="Full quote")}\n' in prefs,prefs
                for body in ('First received reply.','Outgoing wrapped reply.'):
                    start,end=quote_rows(t,body)
                    lines=t.screen.display[start:end+1]
                    assert (end>start)==(length=='Full quote'),lines
                    assert any('…' in s for s in lines)==(length=='Single line'),lines
                    if length=='Full quote':assert 'the complete original.' in lines[-1],lines
                    assert ('│ ' in lines[0])==(appearance=='Left rule'),lines
                    if length=='Single line':
                        before=t.screen.display[:110];point=(t.screen.cursor.x,t.screen.cursor.y)
                        x=cols-6 if appearance=='Shaded' else next(i for i,ch in enumerate(lines[0]) if not ch.isspace())+2
                        t.send(f'\x1b[<0;{x+1};{end+1}M',wait=.05);t.send(f'\x1b[<0;{x+1};{end+1}m')
                        t.expect('Showing the message');t.send(b'\x0f');t.click_text('Return to previous position')
                        wait(t,lambda:body in t.text and not any('╭' in s and '╮' in s for s in t.screen.display),'Return')
                        assert t.screen.display[:110]==before,t.text
                        assert (t.screen.cursor.x,t.screen.cursor.y)==point
        assert (t.screen.cursor.x,t.screen.cursor.y)==cursor
        t.expect('kept compact draft');t.snapshot(f'harbor-single-line-quotes-{cols}')
        choose(t,'Quote length','Full quote')
      finally:t.close();t.wait_backend_shutdown()
      for length in ('Full quote','Single line'):
        t=Terminal(cols=cols,rows=120,config_dir=config,extra_env=env)
        try:
            t.expect('kept compact draft')
            t.send(b'\x0f');t.click_text('Settings');t.expect(length);t.expect('Shaded');t.send(b'\x1b')
            start,end=quote_rows(t,'Outgoing wrapped reply.')
            assert (end>start)==(length=='Full quote'),t.text
            choose(t,'Quote length','Single line')
        finally:t.close();t.wait_backend_shutdown()
      print(f'PASS {cols}cols: compact default for existing profile; both lengths persist independently of all appearances; exact spacing, ellipsis, original/Return and draft/cursor',flush=True)

if '--length-only' not in sys.argv:
  with tempfile.TemporaryDirectory(prefix='harbor-reply-context-') as directory:
    config=Path(directory)/'config'
    t=Terminal(cols=60,rows=120,config_dir=config,extra_env=env)
    try:
        t.picker('Doug formatting');t.send('recognizable draft');t.send(b'\x1b[D'*2)
        point=(t.screen.cursor.x,t.screen.cursor.y)
        for body,expected in [('First received reply.','Doug: First received reply.'),
                              ('Outgoing wrapped reply.','You: Outgoing wrapped reply.'),
                              ('Unresolved sender keeps this source recognizable.','Unknown sender: Unresolved')]:
            t.click_text(body,button=2);t.click_text('Reply')
            wait(t,lambda:expected in banner(t),'Recognizable reply context')
            assert opaque not in banner(t),banner(t)
            assert (t.screen.cursor.x,t.screen.cursor.y)==point
            t.expect('recognizable draft');t.send(b'\x1b')
        # The original-message resolver also supplies a concise, ellipsized long source.
        start,end=quote_rows(t,'First received reply.')
        t.click_text(t.screen.display[start].strip());t.expect('Showing the message')
        t.click_text('Before we retire Diode',button=2);t.click_text('Reply')
        wait(t,lambda:'Doug: Before we retire' in banner(t),'Resolved original context')
        assert banner(t).endswith('…'),banner(t)
        t.snapshot('harbor-recognizable-reply-context')
    finally:t.close();t.wait_backend_shutdown()
    # Restored drafts can retain a reply whose original has become unavailable.
    state=config/'harbor-state';rows=[]
    for line in state.read_text().splitlines():
        fields=line.split()
        if len(fields)>=8 and fields[0] not in ('active','recover') and fields[6]!='-':
            fields[6]=opaque.encode().hex();line=' '.join(fields)
        rows.append(line)
    state.write_text('\n'.join(rows)+'\n')
    t=Terminal(cols=60,rows=120,config_dir=config,extra_env=env)
    try:
        t.expect('recognizable draft')
        wait(t,lambda:'Original message unavailable' in banner(t),'Unavailable restored original')
        assert opaque not in banner(t),banner(t)
        t.send('X');t.expect('recognizable draXft')
    finally:t.close();t.wait_backend_shutdown()
    print('PASS composer reply context: known author, You, unresolved sender, long ellipsis and unavailable restored original; no opaque IDs; exact draft/cursor retained',flush=True)
