"""Native PTY typing proof with isolated demo accounts; no Telegram sends or credentials."""
import json
import os
import time
import re
import shlex
import tempfile
from contextlib import contextmanager
from pathlib import Path
from native_smoke import Terminal


def bubble_row(t):
    return next((y for y, row in enumerate(t.screen.display) if re.search(r'[●•] [●•] [●•]', row)), None)


def bubble(t):
    return bubble_row(t) is not None


def wait(t, present, seconds=3):
    end = time.monotonic() + seconds
    while bubble(t) != present and time.monotonic() < end:
        t.pump(.04)
    assert bubble(t) == present, t.text


def capture(t, name):
    if not os.environ.get('HARBOR_TYPING_EVIDENCE'):
        return
    out = Path(os.environ['HARBOR_TYPING_EVIDENCE']) / name
    out.parent.mkdir(parents=True, exist_ok=True)
    out.with_suffix('.txt').write_text(t.text)
    out.with_suffix('.raw').write_bytes(t.raw)
    out.with_suffix('.json').write_text(json.dumps({'cols': t.cols, 'rows': t.rows,
      'cells': [[{k: getattr(t.screen.buffer[y][x], k) for k in ('data', 'fg', 'bg', 'bold')}
                 for x in range(t.cols)] for y in range(t.rows)]}))


@contextmanager
def typing_fixture(mode, cols=56, motion=False, extra_env=None):
    # The control file, event log and optional executable wrapper outlive the last UI and its
    # owned backend. Terminal.close waits for its internally owned profile before deleting it.
    with tempfile.TemporaryDirectory(prefix='harbor-typing-control-') as directory:
        root = Path(directory)
        control, events = root / 'control', root / 'events'
        control.write_text('')
        binary = Path(os.environ.get('HARBOR_EXECUTABLE', Path(__file__).resolve().parents[2] / 'build-harbor/bin/harbor'))
        executable = binary
        if motion:
            executable = root / 'harbor'
            executable.write_text('#!/bin/sh\nunset HARBOR_REDUCED_MOTION\nexec ' + shlex.quote(str(binary)) + ' "$@"\n')
            executable.chmod(0o700)
        env = {'HARBOR_DEMO_TYPING_CONTROL': str(control),
               'HARBOR_DEMO_REQUEST_LOG': str(events), 'HARBOR_GRAPHICS': 'blocks'}
        env.update(extra_env or {})
        t = Terminal(cols=cols, rows=38, executable=executable, extra_env=env)
        count = 0

        def emit(action):
            nonlocal count
            count += 1
            with control.open('a') as stream:
                stream.write(action + '\n')
            ack = f'typing-control {count} {action} at-ms='
            deadline = time.monotonic() + 3
            while not events.exists() or ack not in events.read_text():
                assert time.monotonic() < deadline, f'Provider did not accept {action}:\n{t.text}'
                t.pump(.02)

        try:
            yield t, emit
        finally:
            t.close()
            if os.environ.get('HARBOR_TYPING_EVIDENCE') and events.exists():
                target = Path(os.environ['HARBOR_TYPING_EVIDENCE']) / f'{mode}-{cols}.events.log'
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(events.read_text())


for mode, cols in [('stop', 56), ('reply', 110), ('expire', 56), ('coalesced', 56), ('delayed', 56), ('disconnect', 56)]:
    with typing_fixture(mode, cols, motion=True) as (t, emit):
        t.picker('Marina')
        t.expect('Keeping this one')
        t.send('unsent draft stays here')
        t.expect('unsent draft stays here')
        message_y = next(y for y, row in enumerate(t.screen.display) if 'Keeping this one' in row)
        before = next(y for y, row in enumerate(t.screen.display) if '›' in row)
        assert not bubble(t), t.text
        emit('start')
        wait(t, True)
        if mode == 'delayed':
            # Reproduce a late observer. The fixture must remain active until the observer's
            # explicit stop, rather than ending on a clock that began before UI readiness.
            t.pump(2)
            assert bubble(t), t.text
            emit('renew')
        y = bubble_row(t)
        assert y < before and before == next(y for y, row in enumerate(t.screen.display) if '›' in row)
        assert 'is typing' not in t.text and 'unsent draft stays here' in t.text
        dot_x = re.search(r'[●•] [●•] [●•]', t.screen.display[y]).start()
        assert t.screen.display[y - 1][25 if cols >= 100 else 0:].strip() == '', t.text
        assert t.screen.buffer[y-1][dot_x].bg != t.screen.buffer[y][dot_x].bg
        frames, glyphs = set(), set()
        for i in range(9):
            glyphs.add(t.screen.display[y].strip())
            frames.add(tuple(t.screen.buffer[y][x].bold for x in range(t.cols) if t.screen.buffer[y][x].data in '●•'))
            if i == 0:
                capture(t, f'{mode}-{cols}')
            t.pump(.09)
        assert len(frames) >= 2 and len(glyphs) >= 2, (frames, glyphs)
        if mode in ('coalesced', 'delayed'):
            # The provider cannot stop during this observation. The elapsed interval still
            # crosses the old receiver timeout; reintroducing that timeout must fail here.
            observations = []
            started, renewal = time.monotonic(), 4
            span = 6.7 if mode == 'coalesced' else 9.4
            while time.monotonic() - started < span:
                elapsed = time.monotonic() - started
                if mode == 'delayed' and elapsed >= renewal:
                    emit('renew')
                    renewal += 4
                assert bubble(t), t.text
                assert next(y for y, row in enumerate(t.screen.display) if 'Keeping this one' in row) == message_y
                assert next(y for y, row in enumerate(t.screen.display) if '›' in row) == before
                observations.append({'elapsed': round(elapsed, 3), 'message_y': message_y,
                    'composer_y': before, 'typing_y': bubble_row(t), 'dots': t.screen.display[bubble_row(t)].strip()})
                if len(observations) % 8 == 0:
                    capture(t, f'{mode}-frame-{len(observations):03}')
                t.pump(.12)
            if os.environ.get('HARBOR_TYPING_EVIDENCE'):
                (Path(os.environ['HARBOR_TYPING_EVIDENCE']) / f'{mode}-sequence.json').write_text(json.dumps(observations, indent=2))
        if mode == 'reply':
            emit('reply')
            t.expect('The active reply is complete.')
            wait(t, False, seconds=.5)
        elif mode == 'disconnect':
            emit('disconnect')
            t.expect('Connection failed')
            wait(t, False, seconds=.5)
        elif mode == 'expire':
            # Arm an unrenewed provider action only after animation is observed. The demo
            # emits its real stop after the same 5.5s expiry interval as TDLib.
            emit('expire')
            wait(t, False, seconds=8)
        else:
            emit('stop')
            wait(t, False, seconds=.5)
        if mode != 'reply':
            assert next(y for y, row in enumerate(t.screen.display) if 'Keeping this one' in row) == message_y
        assert before == next(y for y, row in enumerate(t.screen.display) if '›' in row)
        t.expect('unsent draft stays here')
        print(f'PASS {mode}: provider acknowledgement, animation, stable history/composer/draft and real clearing at {cols} columns', flush=True)

with typing_fixture('history') as (t, emit):
    t.picker('Marina')
    t.expect('Keeping this one')
    t.send('draft during history')
    emit('start')
    wait(t, True)
    y = bubble_row(t)
    assert all(t.screen.buffer[y][x].bold for x in range(t.cols) if t.screen.buffer[y][x].data in '●•')
    t.send(b'\x1b[5~')
    assert not bubble(t), t.text
    history = t.screen.display[4:20]
    t.pump(.5)
    assert history == t.screen.display[4:20]
    capture(t, 'history-hidden')
    t.send(b'\x07')
    wait(t, True)
    t.picker('Studio')
    t.expect('Keep the details quiet')
    assert not bubble(t), t.text
    t.picker('Marina')
    t.expect('draft during history')
    wait(t, True)
    emit('stop')
    wait(t, False, seconds=.5)
    print('PASS history: fixed reading position, latest restores activity, other chat has none, draft survives', flush=True)

with typing_fixture('topic', extra_env={'HARBOR_DEMO_TYPING_CHAT': 'hive#421', 'HARBOR_DEMO_TOPICS': '1'}) as (t, emit):
    t.picker('DEMO-421')
    t.expect('Topic-specific history')
    emit('start')
    wait(t, True)
    t.picker('Infrastructure')
    t.expect('different topic')
    assert not bubble(t), t.text
    t.picker('DEMO-421')
    t.expect('Topic-specific history')
    wait(t, True)
    capture(t, 'topic-scoped')
    emit('stop')
    wait(t, False, seconds=.5)
    print('PASS topic: exact forum topic receives activity, sibling does not', flush=True)
