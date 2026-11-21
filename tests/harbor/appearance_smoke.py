"""Exercise saved appearance choices through the real Commands menu, on offline data only."""
from pathlib import Path
import tempfile
from native_smoke import Terminal
from popup_fixture import choose,click,closed,settings,wait,frame

def row(t, text):
    return next(i for i, line in enumerate(t.screen.display) if text in line)

with tempfile.TemporaryDirectory(prefix='harbor-appearance-') as directory:
    config = Path(directory) / 'config'
    t = Terminal(cols=70, rows=46, config_dir=config)
    try:
        t.picker('Marina')
        assert 'Ctrl+/ Commands' in t.screen.display[0]
        assert 'COMPOSE' not in t.text
        t.send('kept appearance draft')
        for label in ['Plain', 'Left rule', 'Shaded']:
            choose(t, 'Quoted replies', label)
            assert 'kept appearance draft' in t.text
            settings(t);click(t,'Settings','Quoted replies')
            wait(t,lambda:frame(t) is not None and frame(t)[4]=='Appearance','Appearance rendered')
            assert 'Selected' in t.screen.display[row(t, label)+1]
            t.send(b'\x1b');closed(t)
        for label in ['Top right', 'Bottom right']:
            choose(t, 'Timestamps', label)
            assert 'kept appearance draft' in t.text
        for label in ['Plain', 'Panels']:
            choose(t, 'Messages', label)
        # Each treatment is reachable, immediately applied, and keeps the existing key binding.
        hints = ['Chat title', 'Composer divider', 'Above prompt right', 'Above prompt left',
                 'Empty prompt', 'Footer keycap', 'Contracts while typing', 'Prompt key only',
                 'Between quota and chat']
        for i, label in enumerate(hints):
            choose(t, 'Commands hint', label)
            if label == 'Empty prompt':
                assert 'Ctrl+/' not in t.text
                t.send(b'\x15'); t.expect('Ctrl+/ Commands'); t.send('kept appearance draft')
            else: t.expect('Ctrl+/')
            assert 'kept appearance draft' in t.text
            assert f'commands {i}\n' in (config/'harbor-appearance').read_text()
        choose(t, 'Quoted replies', 'Shaded')
        choose(t, 'Messages', 'Plain')
        choose(t, 'Timestamps', 'Top right')
        choose(t, 'Commands hint', 'Prompt key only')
        assert 'Ctrl+/ Commands' not in t.text and 'Ctrl+/' in t.text
    finally:
        t.close()
        t.wait_backend_shutdown()
    assert (config/'harbor-appearance').stat().st_mode & 0o077 == 0
    t = Terminal(cols=70, rows=46, config_dir=config)
    try:
        t.expect('kept appearance draft')
        assert 'Ctrl+/ Commands' not in t.text
        settings(t)
        for value in ['Shaded', 'Plain', 'Top right', 'Prompt key only']: t.expect(value)
    finally:
        t.close()
        t.wait_backend_shutdown()
print('PASS all saved appearance choices, key binding, draft preservation and reload')

# Same preference reaches both incoming metadata and an actual outgoing reply.
for appearance in ['Plain', 'Left rule', 'Shaded']:
    t = Terminal(cols=90, rows=100, extra_env={'HARBOR_DEMO_RICH':'1', 'HARBOR_DEMO_REPLIES':'1'})
    try:
        t.picker('Doug formatting')
        choose(t, 'Quoted replies', appearance)
        t.expect('Doug: Older parent outside the loaded page.')
        incoming = row(t, 'Doug: Older parent outside the loaded page.')
        line = t.screen.display[incoming]
        assert ('│ Doug:' in line) == (appearance == 'Left rule'), line
        pos = line.index('Doug:')
        bg = t.screen.buffer[incoming][pos].bg
        adjacent = t.screen.buffer[incoming+2][pos].bg
        assert (bg != adjacent) == (appearance == 'Shaded'), (appearance, bg, adjacent)
        t.click_text('Cross-chat received reply.', button=2);click(t,'Message actions','Reply');closed(t)
        t.send('Outgoing appearance reply'); t.send(b'\r'); t.delivered('Outgoing appearance reply')
        outgoing = row(t, 'Outgoing appearance reply')
        quote = t.screen.display[outgoing-2]
        assert 'Doug:' in quote and ('│ Doug:' in quote) == (appearance == 'Left rule'), quote
        bg = t.screen.buffer[outgoing-2][quote.index('Doug:')].bg
        body = t.screen.buffer[outgoing][t.screen.display[outgoing].index('Outgoing')].bg
        assert (bg != body) == (appearance == 'Shaded'), (appearance, bg, body)
        stamp_rows = [y for y in range(outgoing+1, min(outgoing+4,t.rows))
                      if any(ch.isdigit() for ch in t.screen.display[y])]
        assert stamp_rows, t.text
    finally: t.close()
print('PASS all three quote treatments in both directions and bottom timestamps')
