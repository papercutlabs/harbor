"""Native formatting, exact copies, reply/search/pin navigation, link and reaction choosers (PCL-1312).

Runs the real Harbor executable on the offline demo protocol (HARBOR_DEMO_RICH) in a PTY. Copies go
to a private file (HARBOR_TEST_CLIPBOARD_FILE), opened links to a recording opener, and backend
requests to HARBOR_DEMO_REQUEST_LOG, so no user clipboard, browser or Telegram chat is touched.
"""
import os, re, tempfile, time
from pathlib import Path
from native_smoke import Terminal

work = Path(tempfile.mkdtemp(prefix='harbor-rich-'))
clip, opened, requests, events = work / 'clipboard', work / 'opened', work / 'requests', work / 'events'
opener = work / 'opener.sh'
opener.write_text(f'#!/bin/sh\nprintf "%s\\n" "$1" >> {opened}\n')
opener.chmod(0o755)

def env(**extra):
    base = {'HARBOR_DEMO_RICH': '1', 'HARBOR_TEST_CLIPBOARD_FILE': str(clip), 'HARBOR_TEST_OPENER': str(opener),
            'HARBOR_DEMO_REQUEST_LOG': str(requests), 'HARBOR_TEST_EVENT_LOG': str(events)}
    base.update(extra)
    return base

def read(path):
    return path.read_text() if path.exists() else ''

def reset():
    for path in (clip, opened, requests, events):
        if path.exists(): path.unlink()

def open_rich(t):
    t.picker('Doug formatting'); t.expect('Doug formatting')

def visible(t, text, body=False):
    """Find actual rendered text using normal history paging at the current size."""
    def shown():
        if not body:
            return text in t.text
        main_x = 25 if t.cols >= 100 else 0
        return any((x := line.find(text)) >= main_x and not line[main_x:x].rstrip().endswith('│')
                   for line in t.screen.display)
    if shown():
        return
    # Start at the known latest end; scanning upward then downward from an arbitrary
    # position can fetch the unloaded original before the visit regression reaches it.
    t.send(b'\x07')
    if shown():
        return
    for _ in range(40):
        before = t.text
        t.send(b'\x1b[5~', wait=.2)
        if shown():
            return
        if t.text == before:
            break
    t.expect(text)

def locate(t, text):
    """The cell where text starts in a message body, not in a reply excerpt quoting it."""
    t.expect(text)
    main_x = 25 if t.cols >= 100 else 0
    for y, line in enumerate(t.screen.display):
        x = line.find(text)
        if x >= main_x and not line[main_x:x].rstrip().endswith('│'):
            return x, y
    raise AssertionError(f'No message body row for {text!r}:\n{t.text}')

def click_body(t, text, button=0):
    x, y = locate(t, text)
    t.send(f'\x1b[<{button};{x+2};{y+1}M', wait=.06)
    t.send(f'\x1b[<{button};{x+2};{y+1}m', wait=.1)

def actions(t, message):
    visible(t, message, body=True)
    click_body(t, message, button=2); t.expect('Message actions')

def tools(t, message, tool):
    visible(t, message, body=True)
    click_body(t, message, button=2); t.expect('Message actions')
    t.send(b'\x1b'); t.click_text('/ commands'); t.click_text('More message tools'); t.expect('Message tools')
    t.click_text(tool)

def cell(t, text):
    """Screen cell of text, matching cell data so combining strokes do not shift columns."""
    for y in range(t.rows):
        for x in range(t.cols):
            if ''.join(t.screen.buffer[y][col].data for col in range(x, t.cols)).startswith(text):
                return x, y
    raise AssertionError(f'No terminal cell for {text!r}:\n{t.text}')

def drag(t, a, b):
    (x, y), (xx, yy) = a, b
    t.send(f'\x1b[<0;{x+1};{y+1}M', wait=.05)
    t.send(f'\x1b[<32;{xx+1};{yy+1}M', wait=.1)
    t.send(f'\x1b[<0;{xx+1};{yy+1}m', wait=.4)
    t.expect('Copied selection')

def commands(t, entry):
    t.click_text('/ commands'); t.expect('Commands & shortcuts'); t.click_text(entry)

def gone(t, text, wait=.4):
    t.pump(wait); assert text not in t.text, f'Unexpected {text!r}:\n{t.text}'

for cols in (56, 110):
    reset()
    t = Terminal(cols=cols, rows=60, extra_env=env())
    try:
        open_rich(t)
        visible(t, '‹shell›'); t.expect('‹shell›'); t.expect('sh ~/Downloads/create-hive-next-repos.sh'); gone(t, '```')
        visible(t, 'The code word is ▒▒▒▒▒▒▒▒▒, keep it.'); t.expect('The code word is ▒▒▒▒▒▒▒▒▒, keep it.'); gone(t, 'harbor-42')
        visible(t, '│ first quoted line …'); t.expect('│ first quoted line …'); gone(t, 'second quoted line')
        visible(t, '**literal** stars and `ticks` stay as typed.'); t.expect('**literal** stars and `ticks` stay as typed.')
        visible(t, 'news'); t.expect('o\u0336l\u0336d\u0336')  # strike as a combining stroke overlay
        t.snapshot(f'harbor-rich-{cols}')
        actions(t, 'It should end with Done'); t.expect('Copy code'); t.snapshot(f'harbor-rich-actions-{cols}')
        t.click_text('Copy code'); t.expect('Copied code')
        assert read(clip) == 'sh ~/Downloads/create-hive-next-repos.sh', read(clip)
        actions(t, 'It should end with Done'); t.click_text('Copy text'); t.expect('Copied')
        assert read(clip) == ("It's in your Downloads now:\n\nsh ~/Downloads/create-hive-next-repos.sh\n\n"
                              "It should end with Done. Paste me the output."), read(clip)
        actions(t, 'Two steps'); t.click_text('Copy code…'); t.expect('2 lines'); t.send(b'\r'); t.expect('Copied code')
        assert read(clip) == 'cd ~/src\n\tmake **all** _x_\n', repr(read(clip))
        actions(t, 'Two steps'); t.click_text('Copy code…'); t.send(b'\x1b[B'); t.send(b'\r'); t.expect('Copied code')
        assert read(clip) == "printf '%s\\n' \"done\"", repr(read(clip))
        # a drag across the code header and its command copies the command, never the ‹shell› decor
        visible(t, '‹shell›'); start = cell(t, '‹shell›'); end = cell(t, 'repos.sh')
        drag(t, start, (end[0] + 7, end[1]))
        assert '‹' not in read(clip) and '›' not in read(clip) and '```' not in read(clip), repr(read(clip))
        assert 'sh ~/Downloads/create-hive-next-repos.s' in read(clip), repr(read(clip))
        # a reply excerpt quoting the spoiler message keeps it concealed
        visible(t, 'Got it, I will keep that word private.'); t.expect('│ The code word is ▒▒▒'); gone(t, 'harbor-42')
        visible(t, 'Last word: the deploy key is'); t.expect('Last word: the deploy key is ▒▒▒▒'); gone(t, 'swordfish')
        if cols == 56:
            # real wrapping at 56 columns keeps the bold range on both wrapped lines
            visible(t, 'Wrapped styling:'); x, y = cell(t, 'Wrapped styling:')
            assert 'narrow edge.' not in t.screen.display[y], f'did not wrap:\n{t.text}'
            bold = [r for r in (y, y + 1) if any(t.screen.buffer[r][c].bold for c in range(x, t.cols))]
            assert bold == [y, y + 1], (bold, t.text)
        else:
            # the picker preview of the newest message is literal native text with its spoiler concealed
            t.send(b'\x0b'); t.expect('Find a conversation'); t.send('Doug formatting')
            t.expect('Last word: the deploy key is ▒'); gone(t, 'swordfish'); t.send(b'\x1b')
        print(f'PASS {cols} cols: native formatting, strike, single/multiple code copies, whole-message copy, drag without decor, concealed excerpt/preview, wrapping')
    finally: t.close()

reset()
t = Terminal(cols=56, rows=60, extra_env=env())
try:
    open_rich(t)
    tools(t, 'Read the plan', 'Open a link…'); t.expect('Open a link')
    t.expect('https://example.com/plans/PCL-1312?view=full'); t.send(b'\r'); t.pump(.5)
    assert read(opened) == 'https://example.com/plans/PCL-1312?view=full\n', read(opened)
    tools(t, 'The code word', 'Reveal spoiler'); t.expect('The code word is harbor-42, keep it.')
    visible(t, 'Got it, I will keep'); assert '│ The code word is harbor' not in t.text, t.text  # excerpt stays concealed
    tools(t, 'That was the quote', 'Expand quote'); t.expect('│ second quoted line')
    t.send('draft stays put')
    actions(t, 'Replying to the original plan.'); t.click_text('Go to replied message')
    t.expect('Original plan: keep drafts unique and offline.', timeout=5)
    assert 'get-message chat=rich msg=' in read(requests), read(requests)
    commands(t, 'Return to previous position'); t.expect('Replying to the original plan.')
    t.expect('│ Original plan: keep drafts unique'); t.send(b'\x1b'); t.expect('draft stays put')
    actions(t, 'This replies to a message'); t.click_text('Go to replied message')
    t.expect('The original message is unavailable', timeout=16)
    print('PASS link chooser, spoiler reveal, quote toggle, unloaded reply visit/return with draft, unavailable original')
finally: t.close()

reset()
t = Terminal(cols=56, rows=60, extra_env=env(HARBOR_DEMO_FETCH_ERROR='1'))
try:
    open_rich(t)
    actions(t, 'This replies to a message'); t.click_text('Go to replied message')
    # an explicit failure is reported at once, well before the 12 s visit timeout
    t.expect('The original message is unavailable', timeout=3)
    # the failed visit leaves no stale mark behind: Return is not offered
    t.click_text('/ commands'); t.expect('Commands & shortcuts'); gone(t, 'Return to previous position'); t.send(b'\x1b')
    print('PASS explicit original failure is immediate and forgets the return mark')
finally: t.close()

reset()
t = Terminal(cols=56, rows=60, extra_env=env(HARBOR_DEMO_FETCH_DELAY_MS='1500'))
try:
    open_rich(t); t.send('rich draft')
    actions(t, 'Replying to the original plan.'); t.click_text('Go to replied message'); t.expect('Loading the original')
    # leaving while the original loads ends that visit: no later jump, no false "unavailable"
    t.picker('Kleya'); t.expect('One conversation.'); t.pump(2)
    t.picker('Doug formatting'); t.expect('rich draft')
    gone(t, 'The original message is unavailable', wait=13); gone(t, 'Showing the message', wait=.1)
    t.click_text('/ commands'); t.expect('Commands & shortcuts'); gone(t, 'Return to previous position'); t.send(b'\x1b')
    print('PASS a chat switch during a pending visit clears it, its mark and its late original')
finally: t.close()

# quitting while visiting saves the reading position the user left for the visit, not the visited spot
state = Path(tempfile.mkdtemp(prefix='harbor-rich-state-')) / 'config'
reset()
t = Terminal(cols=56, rows=40, config_dir=state, extra_env=env())
try:
    open_rich(t); t.expect('Last word: the deploy key is')
    visible(t, 'Replying to the original plan.')
    before_visit = [row.rstrip() for row in t.screen.display[7:-6]]
    actions(t, 'Replying to the original plan.'); t.click_text('Go to replied message')
    t.expect('Original plan: keep drafts unique and offline.', timeout=5); t.expect('Showing the message')
finally: t.close()
t = Terminal(cols=56, rows=40, config_dir=state, extra_env=env())
try:
    t.expect('Doug formatting'); t.expect('Replying to the original plan.')
    t.pump(1); body = 'Original plan: keep drafts unique and offline.'
    assert [row.rstrip() for row in t.screen.display[7:-6]] == before_visit, (before_visit, t.text)
    assert not any(body in l and not l[:l.find(body)].rstrip().endswith('│') for l in t.screen.display), t.text
    print('PASS restart after quitting during a visit resumes the pre-visit reading position')
finally: t.close()

reset()
t = Terminal(cols=56, rows=40, extra_env=env())
try:
    open_rich(t); t.send('keep this draft')
    commands(t, 'Search or pinned messages'); t.send('Filler 3 between'); t.send(b'\r')
    t.expect('Search results'); t.expect('Filler 3 between')
    assert 'search chat=rich query=Filler 3 between pinned=0' in read(requests), read(requests)
    t.click_text('Doug · Filler 3 between'); t.expect('Showing the message')
    commands(t, 'Return to previous position'); t.send(b'\x1b'); t.expect('keep this draft')
    commands(t, 'Search or pinned messages'); t.send('nothing like this'); t.send(b'\r')
    t.expect('No messages match “nothing like this”'); t.send(b'\x1b')
    commands(t, 'Search or pinned messages'); t.click_text('Pinned messages'); t.expect('Pinned messages'); t.expect('Original plan')
    t.click_text('Original plan'); t.expect('Showing the message')
    commands(t, 'Return to previous position'); t.send(b'\x1b'); t.expect('keep this draft')
    tools(t, 'The code word', 'React…'); t.expect('🔥')
    assert 'send-reaction' not in read(requests), 'opening the chooser sent a reaction'
    t.click_text('🔥'); t.expect('Reacted 🔥')
    assert 'emoji=🔥' in read(requests), read(requests)
    tools(t, 'Filler 60 between', 'React…'); t.expect('No reactions allowed here')
    print('PASS server search, no-match, pins, return with draft, permitted reactions')
finally: t.close()

for name, extra, check in [
        ('failure', {'HARBOR_DEMO_SEARCH_FAIL': '1'}, 'Search failed · Enter to retry'),
        ('no answer', {'HARBOR_DEMO_SEARCH_SILENT': '1'}, 'Search did not complete')]:
    t = Terminal(cols=56, rows=40, extra_env=env(**extra))
    try:
        open_rich(t); commands(t, 'Search or pinned messages'); t.send('plan'); t.send(b'\r')
        t.expect(check, timeout=25); gone(t, 'No messages match')
        print(f'PASS search {name} is not reported as no match')
    finally: t.close()

reset()
t = Terminal(cols=56, rows=40, extra_env=env(HARBOR_DEMO_SEARCH_DELAY_MS='1500', HARBOR_DEMO_REACTIONS_DELAY_MS='2500'))
try:
    open_rich(t)
    commands(t, 'Search or pinned messages'); t.expect('Search this conversation'); t.send('Filler')
    t.expect('Search for “Filler”'); t.send(b'\r'); t.expect('Searching Telegram…'); t.send(b'\x1b')
    commands(t, 'Search or pinned messages'); t.expect('Search this conversation'); t.send('nothing like this')
    t.expect('Search for “nothing like this”'); t.send(b'\r'); t.expect('Searching Telegram…')
    try:
        t.expect('No messages match “nothing like this”', timeout=5)
    except AssertionError as failure:
        # diagnostic for native replay: the demo's emit lines and Harbor's receive/defer/apply lines
        # place the lost reply at emit, receipt or acceptance; no wait is increased
        raise AssertionError(f'{failure}\n--- requests\n{read(requests)}--- events\n{read(events)}') from None
    gone(t, 'Filler 1 between'); t.send(b'\x1b')
    trace, tokens = read(events), re.findall(r'^search chat=rich .* token=(\d+)$', read(requests), re.M)
    assert len(tokens) == 2 and tokens[0] != tokens[1], read(requests)
    assert f'apply search token={tokens[1]} accepted' in trace, trace
    assert f'apply search token={tokens[0]} accepted' not in trace, trace
    tools(t, 'The code word', 'React…'); t.expect('Loading reactions…'); t.send(b'\x1b')
    tools(t, 'That was the quote', 'React…'); t.expect('🙏', timeout=6); gone(t, '🔥')
    t.click_text('🙏'); t.expect('Reacted 🙏')
    lines = read(requests).splitlines()
    asked = [l.split('msg=')[1] for l in lines if l.startswith('available-reactions')]
    sent = [l.split('msg=')[1].split()[0] for l in lines if l.startswith('send-reaction')]
    assert len(asked) == 2 and asked[0] != asked[1] and sent == [asked[1]], lines
    print('PASS stale search and reaction replies do not apply to the newer request or another message')
finally: t.close()

reset()
t = Terminal(cols=56, rows=40, extra_env=env(HARBOR_DEMO_TOPICS='1'))
try:
    t.picker('DEMO-421'); t.expect('Topic-specific history')
    commands(t, 'Search or pinned messages'); t.send('history'); t.send(b'\r'); t.expect('Topic-specific history')
    assert 'search chat=hive#421 query=history' in read(requests), read(requests)
    gone(t, 'Parent group history.')
    print('PASS search keeps the forum topic identity')
finally: t.close()
