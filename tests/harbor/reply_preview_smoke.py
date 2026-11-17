"""Received reply metadata and unloaded originals through the actual offline PTY renderer."""
import os
import tempfile
from pathlib import Path
from native_smoke import Terminal

with tempfile.TemporaryDirectory(prefix='harbor-reply-proof-') as directory:
    requests = Path(directory) / 'requests'
    t = Terminal(cols=90, rows=85, extra_env={
        'HARBOR_DEMO_RICH': '1', 'HARBOR_DEMO_REPLIES': '1',
        'HARBOR_DEMO_FETCH_DELAY_MS': '150', 'HARBOR_DEMO_REQUEST_LOG': str(requests)})
    try:
        t.picker('Doug formatting')
        t.expect('│ Doug: Older parent outside the loaded page.')
        t.expect('Correct cross-chat original.')
        t.expect('Original author:')
        t.expect('public quote')
        assert 'secret words' not in t.text, t.text
        assert 'Reply to a message' not in t.text, t.text
        # Unknown legacy target must not quote the same-id local message.
        lines = t.screen.display
        legacy = next(i for i,line in enumerate(lines) if 'Legacy reply with lost' in line)
        assert 'Original message unavailable' in lines[legacy-1], t.text
        for body in ['Reply to inaccessible original.', 'Reply to deleted original.']:
            row = next(i for i,line in enumerate(lines) if body in line)
            assert 'Original message unavailable' in lines[row-1], t.text
        # The original was fetched only for the preview: no visible historical insertion or jump.
        assert not any('Older parent outside' in line and '│' not in line for line in lines), t.text
        assert 'Showing the message' not in t.text
        t.send('Draft stays here')
        t.pump(.3)
        log = requests.read_text()
        reads = [line for line in log.splitlines() if 'preview=1' in line]
        assert len(reads) == len(set(reads)) == 4, log
        assert sum('chat=studio' in line for line in reads) == 1, log
        # Redraws and a narrow layout neither re-fetch nor reveal hidden quote text.
        t.resize(52, 85)
        t.pump(.3)
        assert requests.read_text() == log
        assert 'Draft stays here' in t.text and 'secret words' not in t.text
        t.click_text('Cross-chat received reply.', button=2)
        t.expect('Message actions')
        t.click_text('Go to replied message')
        t.expect('Showing the message')
        t.expect('Correct cross-chat original.')
        t.click_text('/ commands')
        t.click_text('Return to previous position')
        t.expect('Cross-chat received reply.')
        t.expect('Draft stays here')
        output = os.environ.get('HARBOR_REPLY_EVIDENCE')
        if output:
            Path(output).write_text(t.text + '\nREQUESTS\n' + log)
        print('PASS received reply previews: unloaded/deduplicated exact read, author, cross-chat collision, '
              'unknown legacy metadata, supplied spoiler quote, deleted/inaccessible parent, stable draft/narrow redraw')
    finally:
        t.close()

# A pending navigation intent ends on leaving, even when the user returns before the reply arrives.
t = Terminal(cols=90, rows=85, extra_env={
    'HARBOR_DEMO_RICH': '1', 'HARBOR_DEMO_REPLIES': '1', 'HARBOR_DEMO_FETCH_DELAY_MS': '3000'})
try:
    t.picker('Doug formatting')
    t.click_text('First received reply.', button=2)
    t.click_text('Go to replied message')
    t.expect('Loading the original message')
    t.picker('Kleya')
    t.picker('Doug formatting')
    t.pump(3.2)
    assert 'Showing the message' not in t.text, t.text
    t.expect('First received reply.')
    t.click_text('/ commands')
    assert 'Return to previous position' not in t.text, t.text
    print('PASS leaving and returning cancels a pending preview jump without a stale return mark')
finally:
    t.close()
