"""Ordinary link/file clicks through the real native input, layout and opener boundary.

Everything uses offline demo data, private Downloads/clipboard and a recording
opener. No browser, account, default viewer or user file is touched.
"""
import tempfile
import os
import select
import time
import unicodedata
from pathlib import Path
from native_smoke import Terminal

work = Path(tempfile.mkdtemp(prefix='harbor-click-'))
opened, clipboard, requests = (work / name for name in ('opened', 'clipboard', 'requests'))
opener = work / 'opener.sh'
events = work / 'open-events'
os.mkfifo(events, 0o600)
event_descriptor = os.open(events, os.O_RDWR | os.O_NONBLOCK)
opener.write_text('#!/bin/sh\nprintf "%s\\n" "$1" >> "' + str(opened) + '"\nprintf x > "' + str(events) + '"\n')
opener.chmod(0o755)

def opened_urls():
    return opened.read_text().splitlines() if opened.exists() else []

def expect_opened(count, expected):
    # The opener is an asynchronous native child. Wait for its output event, not a scheduler delay.
    until = time.monotonic() + 5
    while len(opened_urls()) < count + len(expected) and time.monotonic() < until:
        if select.select([event_descriptor], [], [], max(0, until-time.monotonic()))[0]:
            os.read(event_descriptor, 4096)
    assert opened_urls()[count:] == expected, opened_urls()

def point(t, text):
    for y in range(t.rows):
        for x in range(t.cols):
            if unicodedata.normalize('NFC', ''.join(t.screen.buffer[y][col].data for col in range(x, t.cols))).startswith(unicodedata.normalize('NFC', text)):
                return x, y
    raise AssertionError(f'Missing {text!r}:\n{t.text}')

def mouse(t, p, action):
    x, y = p
    t.send(f'\x1b[<{32 if action == "drag" else 0};{x+1};{y+1}{"m" if action == "up" else "M"}', wait=.12)

def click(t, p):
    mouse(t, p, 'down'); mouse(t, p, 'up'); t.pump(.2)

for cols in (42, 110):
    config = work / f'config-{cols}'
    downloads = work / f'Downloads-{cols}'
    config.mkdir(); downloads.mkdir()
    (config / 'version').write_text('31')
    (config / 'app.conf').write_text('downloads_dir=' + str(downloads) + '\n')
    # An existing destination must survive; a second click gets a fresh name.
    (downloads / 'new-shelf.md').write_text('existing user file')
    t = Terminal(cols=cols, rows=50, config_dir=config, extra_env={
        'HARBOR_DEMO_RICH': '1', 'HARBOR_DEMO_CLICKS': '1',
        'HARBOR_TEST_OPENER': str(opener), 'HARBOR_TEST_CLIPBOARD_FILE': str(clipboard),
        'HARBOR_DEMO_REQUEST_LOG': str(requests), 'XDG_STATE_HOME': str(work / f'state-{cols}')})
    try:
        t.picker('Doug formatting')
        # Picker closure precedes the asynchronous history response.
        t.expect('PcL')
        original_config = (config / 'app.conf').read_bytes()
        original_mtime = (config / 'app.conf').stat().st_mtime_ns
        # Native text_url words, an internal space, and a word on a wrapped row.
        for text in ('PcL', 'shelf', 'rewrite', 'draft', 'wrapped', 'words'):
            count = len(opened_urls())
            p = point(t, text)
            mouse(t, p, 'down')
            assert len(opened_urls()) == count, 'Press must wait for release to distinguish dragging'
            mouse(t, p, 'up'); t.pump(.2)
            expect_opened(count, ['https://example.com/shelf?view=full'])
        x, y = point(t, 'PcL')
        count = len(opened_urls()); click(t, (x+3, y))
        expect_opened(count, ['https://example.com/shelf?view=full'])
        for text in ('你', '好', 'café', 'é'):
            count = len(opened_urls()); p = point(t, text)
            # A wide glyph's second terminal column belongs to the same link.
            if text == '你': p = (p[0]+1, p[1])
            click(t, p)
            expect_opened(count, ['https://example.com/wide'])
        count = len(opened_urls())
        for text in ('Read', 'now.', '▒▒▒', 'Normal underlined'):
            click(t, point(t, text))
        assert len(opened_urls()) == count
        click(t, point(t, 'local link')); t.expect('cannot be opened safely')
        assert len(opened_urls()) == count
        # Start on the link and drag into another word: copy, no open.
        start, end = point(t, 'PcL'), point(t, 'words')
        mouse(t, start, 'down'); mouse(t, end, 'drag'); mouse(t, end, 'up')
        t.expect('Copied selection')
        assert 'PcL shelf rewrite draft with wrapped w' in clipboard.read_text(), clipboard.read_text()
        assert len(opened_urls()) == count
        t.send('draft stays')
        # Explicit click saves through the backend download action, never launches.
        t.click_text('new-shelf.md'); t.expect('Saved to ')
        expected = (Path(__file__).parent / 'fixtures/new-shelf.md').read_bytes()
        assert (downloads / 'new-shelf_1.md').read_bytes() == expected
        assert (downloads / 'new-shelf.md').read_text() == 'existing user file'
        assert len(opened_urls()) == count
        assert 'draft stays' in t.text
        assert 'attachment-download action=2' in requests.read_text(), requests.read_text()
        assert 'click to save' in t.text
        assert (config / 'app.conf').read_bytes() == original_config
        assert (config / 'app.conf').stat().st_mtime_ns == original_mtime
        t.click_text('new-shelf.md'); t.expect('Saved to ')
        assert (downloads / 'new-shelf_2.md').read_bytes() == expected
        assert len(opened_urls()) == count
        # Dragging on the file row must not save or open.
        x,y = point(t, 'new-shelf.md')
        mouse(t,(x,y),'down'); mouse(t,(x+3,y),'drag'); mouse(t,(x+3,y),'up')
        assert not (downloads / 'new-shelf_3.md').exists()
        assert len(opened_urls()) == count
        if cols == 110:
            # Keep the established keyboard/action flow for deliberately opening a file.
            # Keep the wide-glyph row outside the popup's background: pyte does
            # not clear an overwritten wide glyph's other cell like a terminal.
            t.resize(cols, 24); t.send(b'\x07')
            assert '你好' not in t.text
            t.send(b'\x1b'); t.send(b'\r'); t.expect('Message actions')
            t.click_text('More message tools'); t.expect('Message tools')
            t.expect('Open / play attachment'); t.send(b'\r'); t.pump(.3)
            expect_opened(count, [str(Path(__file__).parent.resolve() / 'fixtures/new-shelf.md')])
        t.snapshot(f'harbor-message-click-{cols}')
    finally:
        t.close()
os.close(event_descriptor)
print('PASS message clicks: wrapped native links, spaces, wide/combining glyphs, spoilers, URL refusal, drag, download, collision, explicit open')
