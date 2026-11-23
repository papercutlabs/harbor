"""Clear only the focused chat composer through the native terminal decoder."""
import tempfile

from native_smoke import Terminal


def expect_empty_composer(terminal, chat):
    assert chat in terminal.screen.display[0], terminal.text
    assert not terminal.screen.cursor.hidden, terminal.text
    line = terminal.screen.display[terminal.screen.cursor.y]
    assert line.strip() == '›', terminal.text
    assert terminal.screen.cursor.x == line.index('›') + 2, terminal.text


with tempfile.TemporaryDirectory(prefix='harbor-clear-draft-') as config:
    terminal = Terminal(cols=56, rows=38, config_dir=config)
    try:
        terminal.picker('Marina')
        terminal.send('Other chat keeps its draft')
        terminal.picker('Studio')
        terminal.send(b'\x03')
        expect_empty_composer(terminal, 'Studio')

        draft = '\n'.join(f'prompt-line-{index:02d} ' + ('wide café 界 ' * 5) for index in range(36))
        for clear in (b'\x03', b'\x1b[99;5u'):
            terminal.send('\x1b[200~' + draft + '\x1b[201~')
            terminal.expect('prompt-line-35')
            assert 'prompt-line-00' not in terminal.text, terminal.text
            terminal.send(b'\x01')
            terminal.expect('prompt-line-00')
            terminal.send(b'\x1bOC')
            terminal.send(clear)
            expect_empty_composer(terminal, 'Studio')
            terminal.send('Fresh text')
            terminal.expect('› Fresh text')
            terminal.send(clear)
            expect_empty_composer(terminal, 'Studio')

        terminal.send('Keep me outside the composer')
        terminal.send(b'\x0b')
        terminal.expect('Find a conversation')
        terminal.send(b'\x03')
        terminal.expect('Find a conversation')
        terminal.send(b'\x1b')
        terminal.expect('Keep me outside the composer')

        terminal.send(b'\x1b')
        terminal.expect('MESSAGES')
        terminal.send(b'\x03')
        terminal.expect('MESSAGES')
        terminal.expect('Keep me outside the composer')
        terminal.send(b'\x1b')
        terminal.send(b'\x03')
        expect_empty_composer(terminal, 'Studio')
        terminal.picker('Marina')
        terminal.expect('Other chat keeps its draft')
        terminal.picker('Studio')
        expect_empty_composer(terminal, 'Studio')
    finally:
        terminal.close()
        terminal.wait_backend_shutdown()

    terminal = Terminal(cols=56, rows=38, config_dir=config)
    try:
        terminal.expect('Studio')
        expect_empty_composer(terminal, 'Studio')
        terminal.picker('Marina')
        terminal.expect('Other chat keeps its draft')
    finally:
        terminal.close()
        terminal.wait_backend_shutdown()

print('PASS Ctrl+C clears the whole focused multiline composer and preserves focus, persisted state and other drafts')
