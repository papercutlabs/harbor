"""Native PTY typing proof with isolated demo accounts; no Telegram sends or credentials."""
import json
import os
import time
import re
import shlex
import tempfile
from pathlib import Path
from native_smoke import Terminal

DOTS = '● ● ●'
def bubble_row(t):
    return next((y for y, row in enumerate(t.screen.display) if re.search(r'[●•] [●•] [●•]', row)), None)
def bubble(t): return bubble_row(t) is not None


def wait(t, present, seconds=14):
    end = time.monotonic() + seconds
    while bubble(t) != present and time.monotonic() < end:
        t.pump(.08)
    assert bubble(t) == present, t.text

def capture(t, name):
    if not os.environ.get('HARBOR_TYPING_EVIDENCE'): return
    out = Path(os.environ['HARBOR_TYPING_EVIDENCE']) / name
    out.parent.mkdir(parents=True, exist_ok=True)
    out.with_suffix('.txt').write_text(t.text)
    out.with_suffix('.raw').write_bytes(t.raw)
    out.with_suffix('.json').write_text(json.dumps({'cols': t.cols, 'rows': t.rows,
      'cells': [[{k: getattr(t.screen.buffer[y][x], k) for k in ('data', 'fg', 'bg', 'bold')}
                 for x in range(t.cols)] for y in range(t.rows)]}))

# Terminal's other smoke tests opt out of motion. Unset that preference for this animation proof.
wrapper_dir = tempfile.TemporaryDirectory(prefix='harbor-typing-motion-')
wrapper = Path(wrapper_dir.name) / 'harbor'
binary = Path(os.environ.get('HARBOR_EXECUTABLE', Path(__file__).resolve().parents[2] / 'build-harbor/bin/harbor'))
wrapper.write_text('#!/bin/sh\nunset HARBOR_REDUCED_MOTION\nexec ' + shlex.quote(str(binary)) + ' "$@"\n')
wrapper.chmod(0o700)

for mode, cols in [('stop', 56), ('reply', 110), ('expire', 56), ('coalesced', 56), ('delayed', 56), ('disconnect', 56)]:
    t = Terminal(cols=cols, rows=38, executable=wrapper, extra_env={'HARBOR_DEMO_TYPING': mode,
                 'HARBOR_GRAPHICS': 'blocks'})
    try:
        t.picker('Marina'); t.send('unsent draft stays here')
        message_y = next(y for y, row in enumerate(t.screen.display) if 'Keeping this one' in row)
        before = next(y for y, row in enumerate(t.screen.display) if '›' in row)
        assert not bubble(t), t.text
        wait(t, True)
        y = bubble_row(t)
        assert y < before and before == next(y for y, row in enumerate(t.screen.display) if '›' in row)
        assert 'is typing' not in t.text and 'unsent draft stays here' in t.text
        # A full base-colored row separates the filled last-message margin and the bubble.
        dot_x = re.search(r'[●•] [●•] [●•]', t.screen.display[y]).start()
        assert t.screen.display[y - 1][25 if cols >= 100 else 0:].strip() == '', t.text
        assert t.screen.buffer[y-1][dot_x].bg != t.screen.buffer[y][dot_x].bg
        frames = set()
        glyphs = set()
        for i in range(9):
            glyphs.add(t.screen.display[y].strip())
            frames.add(tuple(t.screen.buffer[y][x].bold for x in range(t.cols) if t.screen.buffer[y][x].data in '●•'))
            if i == 0: capture(t, f'{mode}-{cols}')
            t.pump(.09)
        assert len(frames) >= 2 and len(glyphs) >= 2, (frames, glyphs)
        if mode in ('coalesced', 'delayed'):
            # Read real native frames through the former six-second cutoff, every phase and
            # delayed action renewals. Any missing bubble or shifted message is a failure.
            observations = []
            started = time.monotonic()
            span = 6.7 if mode == 'coalesced' else 9.4
            while time.monotonic() - started < span:
                assert bubble(t), t.text
                assert next(y for y, row in enumerate(t.screen.display) if 'Keeping this one' in row) == message_y
                assert next(y for y, row in enumerate(t.screen.display) if '›' in row) == before
                observations.append({'elapsed': round(time.monotonic()-started, 3),
                    'message_y': message_y, 'composer_y': before, 'typing_y': bubble_row(t),
                    'dots': t.screen.display[bubble_row(t)].strip()})
                if len(observations) % 8 == 0: capture(t, f'{mode}-frame-{len(observations):03}')
                t.pump(.12)
            if os.environ.get('HARBOR_TYPING_EVIDENCE'):
                (Path(os.environ['HARBOR_TYPING_EVIDENCE']) / f'{mode}-sequence.json').write_text(json.dumps(observations, indent=2))
        if mode == 'reply':
            t.expect('The active reply is complete.', timeout=8)
            wait(t, False, seconds=.5)
        elif mode == 'disconnect':
            t.expect('Connection failed', timeout=8)
            wait(t, False, seconds=.5)
        else:
            wait(t, False)
        if mode != 'reply':
            assert next(y for y, row in enumerate(t.screen.display) if 'Keeping this one' in row) == message_y
        if mode == 'reply': t.expect('The active reply is complete.')
        assert before == next(y for y, row in enumerate(t.screen.display) if '›' in row)
        t.expect('unsent draft stays here')
        print(f'PASS {mode}: actual service event, animation, latest bubble, stable composer/draft and clearing at {cols} columns')
    finally: t.close()

# A bubble belongs at latest, never in scrolled history or another chat. Switching back retains activity.
t = Terminal(cols=56, rows=38, extra_env={'HARBOR_DEMO_TYPING': 'hold'})
try:
    t.picker('Marina'); t.send('draft during history'); wait(t, True)
    y = bubble_row(t)
    assert all(t.screen.buffer[y][x].bold for x in range(t.cols) if t.screen.buffer[y][x].data in '●•')
    t.send(b'\x1b[5~'); assert not bubble(t), t.text
    history = t.screen.display[4:20]
    t.pump(.5); assert history == t.screen.display[4:20]
    capture(t, 'history-hidden')
    t.send(b'\x07'); wait(t, True)
    t.picker('Studio'); assert not bubble(t), t.text
    t.picker('Marina'); wait(t, True); t.expect('draft during history')
    print('PASS history stays still and hides bubble; latest restores it; other chat has none; draft survives')
finally: t.close()

# Topic notifications use their exact conversation identity, never the parent or a sibling topic.
t = Terminal(cols=56, rows=38, extra_env={'HARBOR_DEMO_TYPING': 'hold',
    'HARBOR_DEMO_TYPING_CHAT': 'hive#421', 'HARBOR_DEMO_TOPICS': '1'})
try:
    t.picker('DEMO-421'); wait(t, True)
    t.picker('Infrastructure'); assert not bubble(t), t.text
    t.picker('DEMO-421'); wait(t, True)
    capture(t, 'topic-scoped')
    print('PASS exact forum topic receives activity; sibling does not')
finally: t.close()

wrapper_dir.cleanup()
