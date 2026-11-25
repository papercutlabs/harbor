"""Use the standalone native composer with real clipboard and PTY paste events."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time
import uuid

from PIL import Image

from native_smoke import ROOT, Terminal


def paste_paths(terminal, *paths):
    terminal.send('\x1b[200~' + shlex.join(map(str, paths)) + '\x1b[201~')


def click_chip(terminal, symbol, marker):
    terminal.expect(marker)
    row, line = next((y, line) for y, line in enumerate(terminal.screen.display) if marker in line)
    column = line.index(symbol, line.index(marker))
    terminal.send(f'\x1b[<0;{column + 1};{row + 1}M', wait=.06)
    terminal.send(f'\x1b[<0;{column + 1};{row + 1}m')


def decoded_sends(path):
    sends = []
    for line in path.read_text().splitlines() if path.exists() else []:
        match = re.fullmatch(r'send chat=(\S+) text=.* view=\d+ index=\d+ request=\d+ payload=([0-9a-fA-F]+)', line)
        if match is None:
            continue
        payload = bytes.fromhex(match[2])
        fields, offset = [], 0
        # The native demo logs the exact ChatMessage received by the backend.
        for _ in range(10):
            size = int.from_bytes(payload[offset:offset + 4], 'little')
            fields.append(payload[offset + 4:offset + 4 + size].decode())
            offset += 4 + size
        file_path = bytes.fromhex(fields[9].split(',')[2]).decode()
        sends.append((match[1], fields[2], file_path))
    return sends


def inspect(terminal, env):
    binary = os.environ.get('HARBOR_EXECUTABLE', ROOT / 'build-harbor/bin/harbor')
    args = [str(binary), '-d', terminal.config, '--target-surface', env['CMUX_SURFACE_ID'],
            '--target-pid', str(terminal.pid), '--inspect-ui']
    with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                          env=os.environ | env) as player:
        deadline = time.monotonic() + 8
        while player.poll() is None and time.monotonic() < deadline:
            terminal.pump(.05)
        if player.poll() is None:
            player.kill()
        output, error = player.communicate()
    assert player.returncode == 0, error
    return output.strip().split('\t')


def main():
    mac = sys.platform == 'darwin'
    board = 'harbor-photo-' + uuid.uuid4().hex
    applescript = """ObjC.import('AppKit'); function run(a) {
        var board=$.NSPasteboard.pasteboardWithName(a[0]); board.clearContents;
        if (a[1]) return board.setDataForType($.NSData.dataWithContentsOfFile(a[1]), 'public.png');
    }"""

    def pasteboard(*args):
        return subprocess.check_output(['/usr/bin/osascript', '-l', 'JavaScript', '-e', applescript, board,
                                        *map(str, args)], text=True, timeout=10).strip()

    with tempfile.TemporaryDirectory(prefix='harbor-inline-', dir='/tmp') as temporary:
        root = Path(temporary).resolve()
        home = root / 'home'
        home.mkdir()
        config, requests, opened = root / 'cfg', root / 'requests', root / 'opened'
        first, second, discarded = (root / name for name in ('first photo.png', 'second.png', 'discarded.png'))
        for file, color in zip((first, second, discarded), ('red', 'green', 'blue')):
            Image.new('RGB', (18, 20), color=color).save(file)
        opener = root / 'open.sh'
        opener.write_text('#!/bin/sh\nprintf "%s|%s\\n" "$1" "$2" >> ' + shlex.quote(str(opened)) + '\n')
        opener.chmod(0o700)
        env = dict(HOME=str(home), XDG_STATE_HOME=str(home / 'state'), HARBOR_DEMO_REQUEST_LOG=str(requests),
                   HARBOR_TEST_OPENER=str(opener), HARBOR_TEST_PASTEBOARD=board, CMUX_SURFACE_ID='INLINE-PHOTO')
        terminal = Terminal(cols=56, rows=34, config_dir=config, extra_env=env)
        try:
            terminal.picker('Studio')
            terminal.send('First last')
            terminal.send(b'\x01\x1b[C\x1b[C\x1b[C\x1b[C\x1b[C\x1b[C')
            if mac:
                assert pasteboard(first) == 'true'
                terminal.send(b'\x16')
            else:
                paste_paths(terminal, first)
            terminal.expect('[Image #1]')
            assert 'Preview attachments' not in terminal.text and not opened.exists()
            assert not decoded_sends(requests)
            terminal.send('middle ')
            terminal.expect('First middle last')

            paste_paths(terminal, second, discarded)
            terminal.expect('[Image #3]')
            terminal.resize(28, 24)
            click_chip(terminal, '→', '[Image #1]')
            terminal.expect('[Image #2]')
            click_chip(terminal, '→', '[Image #2]')
            terminal.expect('[Image #3]')
            terminal.resize(56, 34)
            discarded.unlink()
            terminal.send(b'\r')
            terminal.expect('An attachment is unavailable')
            assert not decoded_sends(requests)
            click_chip(terminal, '×', '[Image #3]')
            assert '[Image #3]' not in terminal.text

            terminal.picker('Kleya')
            assert '[Image #' not in terminal.text and 'First middle last' not in terminal.text
            terminal.send('other draft')
            terminal.picker('Studio')
            terminal.expect('[Image #1]')
            terminal.expect('[Image #2]')
            terminal.expect('First middle last')
            click_chip(terminal, '[Image #1]', '[Image #1]')
            deadline = time.monotonic() + 5
            while not opened.exists() and time.monotonic() < deadline:
                terminal.pump(.1)
            preview, mode = opened.read_text().strip().rsplit('|', 1)
            assert mode == 'preview' and Path(preview).read_bytes() == first.read_bytes(), preview
            assert int(inspect(terminal, env)[8]) > 0
            assert not (home / 'Downloads').exists()
            assert not decoded_sends(requests)
            evidence = os.environ.get('HARBOR_EVIDENCE_DIR')
            if evidence:
                Path(evidence, 'inline-photo-composer.txt').write_text(terminal.text)

            terminal.resize(56, 50)
            terminal.send(b'\r')
            terminal.delivered('First middle last')
            assert decoded_sends(requests) == [('studio', 'First middle last', preview), ('studio', '', str(second))]
            assert '[Image #' not in terminal.text
            terminal.picker('Kleya')
            terminal.expect('other draft')
            terminal.send(b'\x03')
            paste_paths(terminal, second)
            terminal.expect('[Image #1]')
            assert inspect(terminal, env)[6] == '1'
            terminal.send(b'\r')
            terminal.delivered('second.png')
            assert decoded_sends(requests)[2:] == [('kleya', '', str(second))]
            print('PASS staged photos preserve the caption and caret, preview exact bytes, and send only on Enter in the original chat')
            print('PASS multiple paths, narrow overflow, single removal and unavailable-image refusal without a partial send')
            if mac:
                print('PASS Ctrl+V reads PNG from a real private macOS pasteboard')
        finally:
            terminal.close()
            terminal.wait_backend_shutdown()
            if mac:
                pasteboard()


if __name__ == '__main__':
    main()
