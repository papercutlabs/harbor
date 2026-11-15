"""Topics, independent histories/drafts, pin-first ordering, timestamps, and arrows."""
import re
from native_smoke import Terminal

def selected(t):
    return next(row.strip(' │') for row in t.screen.display if row.lstrip(' │').startswith('› ') and 'search…' not in row and row.strip()!='›')

def exact_parent(t):
    t.send(b'\x0b'); t.expect('Find a conversation');t.send('Team forum')
    search=next(y for y,row in enumerate(t.screen.display) if '› Team forum' in row)
    y=next(y for y,row in enumerate(t.screen.display) if y>search+1 and row.lstrip(' │›').startswith('Team forum ') and '>' not in row)
    t.send(f'\x1b[<0;8;{y+1}M',wait=.06);t.send(f'\x1b[<0;8;{y+1}m',wait=.1)
    t.expect('Parent group history.')

t=Terminal(cols=56,rows=42,extra_env={'HARBOR_DEMO_TOPICS':'1'})
try:
    t.send(b'\x0b');t.expect('Find a conversation')
    first=selected(t)
    assert first.startswith('› Studio'),t.text # old but pinned, before the newer topic
    assert re.search(r'\s{2,}(\d\d:\d\d|\d\d/\d\d|[A-Za-z]{3})$',first)
    for down,up in [(b'\x1b[B',b'\x1b[A'),(b'\x1bOB',b'\x1bOA'),
                    (b'\x1b[57353u',b'\x1b[57352u'),(b'\x1b[57353;1u',b'\x1b[57352;1u')]:
        t.send(down);assert selected(t)!=first
        assert 'Desk planning' in selected(t)
        t.send(up);assert selected(t)==first
    t.snapshot('harbor-topics-picker-56')
    t.resize(110,42)
    t.expect('Keep the details')
    studio=next(row for row in t.screen.display if '› Studio' in row)
    assert 'Keep the details' in studio,studio
    t.snapshot('harbor-topics-picker-wide')
    t.send(b'\x1b');t.resize(56,42)
    t.picker('DEMO-421');t.expect('Topic-specific history')
    assert 'Parent group history.' not in t.text and 'different topic' not in t.text
    t.send('Draft for Desk topic')
    t.picker('Infrastructure');t.expect('different topic')
    assert 'Topic-specific history' not in t.text
    t.send('Sibling topic draft')
    t.picker('DEMO-421');t.expect('Draft for Desk topic');t.send(b'\r');t.expect('Sent')
    exact_parent(t)
    assert 'Draft for Desk topic' not in t.text and 'Topic-specific history' not in t.text
    t.picker('Infrastructure');t.expect('Sibling topic draft')
    assert 'Draft for Desk topic' not in t.text
    t.picker('DEMO-421');t.expect('Draft for Desk topic')
    t.send(b'\x0b');t.expect('Find a conversation')
    assert selected(t).startswith('› Studio') # sending does not outrank a pinned chat
    print('PASS forum search/history/send/draft isolation; pins retained; clock/optional preview; CSI/SS3/CSI-u arrow navigation')
finally:t.close()

# A live update may reorder recent chats while the picker is open. Keep the
# selected conversation, not the numeric row that used to contain it.
t=Terminal(cols=56,rows=42,extra_env={'HARBOR_DEMO_TOPICS':'1','HARBOR_DEMO_INCOMING_DELAY_MS':'2200'})
try:
    t.send(b'\x0b');t.expect('Find a conversation');t.send(b'\x1b[B')
    before=selected(t)
    assert 'Desk planning' in before
    t.pump(2.3)
    assert 'Desk planning' in selected(t),t.text
    assert 'Marina' in t.text
    print('PASS picker selection survives an incoming-message reorder')
finally:t.close()
