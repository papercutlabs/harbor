"""Exercise Duet's staged attachment strip through real Harbor PTY input."""
import os
from pathlib import Path
import shutil
import tempfile
import time

from native_smoke import ROOT, Terminal


def click(terminal, x, y):
    terminal.send(f'\x1b[<0;{x + 1};{y + 1}M', wait=.03)
    terminal.send(f'\x1b[<0;{x + 1};{y + 1}m')


def locate(terminal, text):
    terminal.expect(text)
    return next((line.index(text), y) for y, line in enumerate(terminal.screen.display) if text in line)


def paste_path(terminal, path):
    terminal.send(f'\x1b[200~{path}\x1b[201~')


with tempfile.TemporaryDirectory(prefix='duet-attach-', dir='/tmp') as directory:
    root = Path(directory)
    first, second, third = (root / name for name in ('first.png', 'second.pdf', 'third.txt'))
    shutil.copyfile(ROOT / 'tests/harbor/fixtures/harbor.png', first)
    second.write_bytes(b'%PDF-1.3\n%%EOF\n')
    third.write_text('Quick Look fixture\n')
    opens = root / 'opens'
    requests = root / 'requests'
    opener = root / 'opener'
    opener.write_text('#!/bin/sh\nprintf "%s|%s\\n" "$1" "$2" >> "$OPEN_RECORD"\n')
    opener.chmod(0o700)
    terminal = Terminal(cols=56, rows=38, config_dir=root / 'profile', extra_env={
        'DUET_HARBOR_DRAFT_ATTACHMENTS': '1',
        'DUET_HARBOR_NATIVE_PICKER': '0',
        'HARBOR_TEST_OPENER': str(opener),
        'HARBOR_DEMO_REQUEST_LOG': str(requests),
        'OPEN_RECORD': str(opens),
    })

    def require_open(path, viewer, count):
        deadline = time.monotonic() + 5
        lines = []
        while len(lines) < count and time.monotonic() < deadline:
            lines = opens.read_text().splitlines() if opens.exists() else []
            if len(lines) < count:
                terminal.pump(.05)
        assert len(lines) == count and lines[-1] == f'{path}|{viewer}', lines

    try:
        terminal.picker('Studio')
        terminal.send('Draft preserved')
        terminal.send(b'\x1bOD')
        before = (terminal.screen.cursor.x, terminal.screen.cursor.y)
        paste_path(terminal, first)
        paste_path(terminal, second)
        x, y = locate(terminal, 'first.png')
        assert y < locate(terminal, '› Draft preserved')[1], terminal.text
        assert 'Preview attachments' not in terminal.text and not opens.exists()
        deadline = time.monotonic() + 5
        while '▀' not in terminal.screen.display[y - 2][x:x + 16] and time.monotonic() < deadline:
            terminal.pump(.05)
        assert '▀' in terminal.screen.display[y - 2][x:x + 16], terminal.text
        evidence = os.environ.get('HARBOR_DRAFT_ATTACHMENT_EVIDENCE')
        if evidence:
            evidence = Path(evidence)
            evidence.mkdir(parents=True, exist_ok=True)
            (evidence / 'above-composer.txt').write_text(terminal.text)
            (evidence / 'above-composer.raw').write_bytes(terminal.raw)
        click(terminal, x + 4, y - 2)
        require_open(first, 'preview', 1)
        click(terminal, *locate(terminal, 'second.pdf'))
        require_open(second, 'preview', 2)
        assert before == (terminal.screen.cursor.x, terminal.screen.cursor.y) and '› Draft preserved' in terminal.text

        paste_path(terminal, third)
        click(terminal, terminal.cols - 3, y)
        click(terminal, *locate(terminal, 'third.txt'))
        require_open(third, 'quick-look', 3)
        x, y = locate(terminal, 'third.txt')
        click(terminal, x + 15, y)
        assert 'third.txt' not in terminal.text and third.is_file(), terminal.text

        terminal.picker('Marina')
        assert 'first.png' not in terminal.text and 'second.pdf' not in terminal.text
        terminal.picker('Studio')
        terminal.expect('first.png')
        terminal.expect('second.pdf')
        terminal.expect('› Draft preserved')
        assert not requests.exists() or 'send chat=' not in requests.read_text()
        terminal.send(b'\r')
        terminal.delivered('Draft preserved')
        assert requests.read_text().count('send chat=') == 2, requests.read_text()
        assert first.is_file() and second.is_file()
        if evidence:
            (evidence / 'opens.txt').write_text(opens.read_text())
        print('PASS native staged thumbnails, exact Preview and Quick Look targets, caret, chat isolation, removal, explicit send')
    finally:
        terminal.close()
        terminal.wait_backend_shutdown()
