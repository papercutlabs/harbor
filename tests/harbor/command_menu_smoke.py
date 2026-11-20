"""Offline native Commands/help/About receiving, including a changed serving owner and old command files."""
import os
import subprocess
import tempfile
from pathlib import Path
from native_smoke import Terminal

with tempfile.TemporaryDirectory(prefix='harbor-command-menu-') as directory:
    root = Path(directory)
    commands = root/'commands'
    revision = '1a2b3c4d' * 5
    def write(rev=revision, pid=os.getpid()):
        commands.write_text(f'# pcl.desk.owner.v1\t{pid} {rev}\n' + '\n'.join(
            '\t'.join([label, hint, '/usr/bin/true']) for label, hint in [
                ('Quota', 'q capacity'), ('Everything', 'e sessions'), ('Focus', 'f toggle'),
                ('Focus status', 'Decision and history'),
                ('Desk controls', 'Old front page'), ('Desk keys…', 'Every Desk control and key')]) + '\n')
    write()
    t = Terminal(cols=84, rows=48, extra_env={'HARBOR_DESK_COMMANDS':str(commands)})
    try:
        t.picker('Marina'); t.send('kept menu draft')
        t.send(b'\x0f'); t.expect('│ Commands')
        for label in ['Desk · Quota', 'Desk · Everything', 'Desk · Focus', 'Desk · Focus status',
                      'Search or pinned messages', 'Chats', 'Jump to latest messages',
                      'Settings', 'Keyboard help', 'About', 'Quit Harbor']:
            t.expect(label)
        for removed in ['Desk controls', 'Desk keys', 'More message tools', 'Refresh conversation',
                        'Browse messages', 'Attach files', 'Paste clipboard', 'Pause / resume voice',
                        'Stop playback', 'Searches Telegram', 'Recover a failed send', 'Update Harbor']:
            assert removed not in t.text, (removed, t.text)
        menu = t.text
        t.click_text('Keyboard help'); t.expect('Keyboard help'); t.expect('Desk · Desk keys…')
        t.expect('Ctrl+R'); t.expect('Ctrl+T'); t.expect('Space pauses / resumes')
        help_screen = t.text
        t.send(b'\x1b'); t.expect('kept menu draft')
        t.send(b'\x0f'); t.click_text('About'); t.expect('Serving Desk revision'); t.expect(revision)
        reported = subprocess.check_output([os.environ.get('HARBOR_EXECUTABLE', str(Path(__file__).resolve().parents[2]/'build-harbor/bin/harbor')), '--version'],text=True).splitlines()[0].split()[1]
        t.expect(reported)
        about = t.text
        t.send(b'\x1b'); t.expect('kept menu draft')
        # A surviving native UI follows the current writer, without a native restart.
        newer = '9e8d7c6b' * 5
        write(newer)
        t.send(b'\x0f'); t.click_text('About'); t.expect(newer); assert revision not in t.text
        t.send(b'\x1b'); write(newer, 2147483647)
        t.send(b'\x0f'); t.click_text('About'); t.expect('unknown'); assert newer not in t.text
        t.send(b'\x1b'); t.expect('kept menu draft')
        # Message tools moved to the message itself.
        t.click_text('Keeping this one', button=2); t.click_text('More message tools'); t.expect('Message tools')
        t.send(b'\x1b'); t.expect('kept menu draft')
        evidence = os.environ.get('HARBOR_COMMAND_MENU_EVIDENCE')
        if evidence:
            out = Path(evidence); out.mkdir(parents=True, exist_ok=True)
            for name, text in [('commands',menu),('help',help_screen),('about',about)]: (out/(name+'.txt')).write_text(text)
            (out/'native.raw').write_bytes(t.raw)
    finally: t.close()
    t = Terminal(cols=84, rows=48, extra_env={'HOME':str(root/'standalone'), 'HARBOR_DESK_COMMANDS':str(root/'missing')})
    try:
        t.send(b'\x0f'); t.expect('│ Commands'); assert 'Desk ·' not in t.text
        t.click_text('About'); t.expect('Serving Desk revision'); t.expect('unknown')
    finally: t.close()
print('PASS short Commands, separate help/context tools, running version, live owner revision changes, dead owner/standalone unknown, draft retention')
