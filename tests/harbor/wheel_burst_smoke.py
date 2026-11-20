"""Queued wheel gestures travel promptly, stop, and preserve precise reversals."""
import time
from native_smoke import Terminal

up = b'\x1b[<64;30;12M'
down = b'\x1b[<65;30;12M'
t = Terminal(cols=96, rows=38)
try:
    t.picker('Saved Messages')
    t.expect('Note 35:')
    # Compare positions too: a precise tick can move the same messages within a taller panel.
    notes = lambda: [(y, row.strip()) for y, row in enumerate(t.screen.display) if 'Note ' in row]
    latest = notes()
    t.send(up)
    precise = notes()
    assert precise != latest, 'one tick did not move history'
    t.send(down)
    assert notes() == latest, 'one reverse tick did not restore the exact position'
    t.send(up * 200)
    first = notes()
    assert first != latest, 'hard flick did not move history'
    t.pump(.4)
    assert notes() == first, 'history continued moving after the input queue drained'
    # A down-flick hits the newest boundary. An immediate one-tick reversal
    # must act on that boundary, rather than on an unclamped overshoot.
    started = time.monotonic()
    t.send(down * 200 + up + b'wheel-ended', wait=0)
    t.expect('wheel-ended', timeout=1)
    elapsed = time.monotonic() - started
    assert notes() == precise, 'one-tick reversal was lost after a down-flick'
    stopped = notes()
    t.pump(.4)
    assert notes() == stopped, 'scrolling continued after the following key arrived'
    print(f'PASS single ticks precise; 200-tick travel/stop; immediate reversal; following text visible in {elapsed:.3f}s')
finally:
    t.close()
