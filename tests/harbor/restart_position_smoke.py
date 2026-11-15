"""Restart restores the active chat and a message-anchored reading position."""
import tempfile
from pathlib import Path
from native_smoke import Terminal

with tempfile.TemporaryDirectory(prefix='harbor-position-') as directory:
    config=Path(directory)/'config'
    t=Terminal(cols=56,rows=38,config_dir=config)
    try:
        t.picker('Saved Messages');t.expect('Note 35:')
        t.send(b'\x1b[5~');t.pump(.4)
        before=[row.strip() for row in t.screen.display if 'Note ' in row]
        assert before and 'Note 35:' not in '\n'.join(before)
    finally:t.close()
    t=Terminal(cols=56,rows=38,config_dir=config,extra_env={'HARBOR_DEMO_PARTIAL_HISTORY':'1'})
    try:
        t.expect(before[0]);t.pump(.5)
        after=[row.strip() for row in t.screen.display if 'Note ' in row]
        assert after==before,(before,after)
        assert 'Saved Messages' in t.text
        t.send(b'\x07');t.expect('Note 35:')
    finally:t.close()
    t=Terminal(cols=56,rows=38,config_dir=config)
    try:
        t.expect('Saved Messages');t.expect('Note 35:')
    finally:t.close()
print('PASS active chat, anchored scroll after partial history, and following latest survive restart')
