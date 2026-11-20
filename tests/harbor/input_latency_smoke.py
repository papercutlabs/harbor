"""A slow history frame must leave a gap for queued keys and drag reports."""
import fcntl
import tempfile
import time
from pathlib import Path
from native_smoke import Terminal

# More than one 16 ms frame of native rich-text/layout work on ordinary hosts.
# The history comes through DemoProtocol's usual 50-message response.
def until(t, predicate, timeout=2):
    started = time.monotonic()
    while not predicate() and time.monotonic() - started < timeout:
        t.pump(.005)
    return time.monotonic() - started

def point(t, text):
    y = next(y for y, row in enumerate(t.screen.display) if text in row)
    return t.screen.display[y].index(text), y

# A burst has an exact source string, including Unicode and deletion, so merely
# seeing its last few characters cannot hide missing/reordered earlier input.
burst = 'begin-' + 'abcdefghijklmnop' * 20 + '-你好-café-🙂'
expected = burst[:-1] + 'X-end-of-burst'

# Keep an explicit directory so the ordinary cleanup save can be inspected.
with tempfile.TemporaryDirectory(prefix='harbor-input-latency-') as directory:
    config = Path(directory)/'config'
    clip = Path(directory)/'selection'
    t = Terminal(cols=72, rows=62, config_dir=config,
                 extra_env={'HARBOR_DEMO_LONG_HISTORY':'1', 'HARBOR_TEST_CLIPBOARD_FILE':str(clip)})
    try:
        t.picker('Studio');t.expect('selection-sentinel')
        x,y = point(t,'selection-sentinel')
        wire = f'\x1b[<0;{x+1};{y+1}M'
        wire += ''.join(f'\x1b[<32;{x+1+(i%8)};{y+1}M' for i in range(160))
        wire += f'\x1b[<0;{x+8};{y+1}m'
        t.send(wire, wait=0)
        selected = until(t, clip.exists)
        assert clip.exists(), f'Queued selection release blocked beyond 2s: {selected:.3f}s'
        assert clip.read_text() == 'selectio', repr(clip.read_text())
        # Escape clears the selection; literal input and a special key retain order.
        t.send(b'\x1b', wait=.15)
        t.send(burst + '\x7f' + 'X-end-of-burst', wait=0)
        elapsed = until(t, lambda: 'X-end-of-burst' in t.text)
        assert 'X-end-of-burst' in t.text, f'Queued typing blocked beyond 2s: {elapsed:.3f}s'
    finally:
        t.close()
        # This test owns the explicit config. The detached demo backend holds this
        # lock until its final writes finish, so temp cleanup cannot race it.
        end = time.monotonic() + 5
        while True:
            with (config/'session.lock').open() as lock:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    if not (config/'session.sock').exists(): break
                except BlockingIOError:
                    pass
            assert time.monotonic() < end, 'Owned demo backend did not finish shutdown'
            time.sleep(.01)
    drafts = [bytes.fromhex(line.split()[1]).decode() for line in (config/'harbor-state').read_text().splitlines()
              if line and not line.startswith('active ') and line.split()[1] != '-']
    assert drafts == [expected], repr(drafts)
    print(f'PASS long history: typing {elapsed:.3f}s; 160 drag reports + exact copy {selected:.3f}s; persisted Unicode draft exact')
