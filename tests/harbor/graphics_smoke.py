"""Inspect real terminal graphics payloads for aspect fit and scroll cropping."""
import base64
import io
import re
import tempfile
from pathlib import Path
from PIL import Image
from native_smoke import Terminal

def graphics(raw):
    active = None
    payload = []
    for match in re.finditer(rb'\x1b_G(.*?)\x1b\\', bytes(raw), re.S):
        control, _, chunk = match[1].partition(b';')
        fields = dict(item.split(b'=', 1) for item in control.split(b',') if b'=' in item)
        if fields.get(b'a') == b't':
            active = fields[b'i']; payload = []
        if chunk and active:
            payload.append(chunk)
            if fields.get(b'm', b'0') == b'0':
                yield 'image', Image.open(io.BytesIO(base64.b64decode(b''.join(payload)))).convert('RGB')
                active = None
        if fields.get(b'a') == b'p':
            yield 'placement', fields

with tempfile.TemporaryDirectory(prefix='harbor-portrait-') as directory:
    portrait = Path(directory) / 'portrait.png'
    Image.new('RGB', (100, 400), (255, 0, 0)).save(portrait)
    t = Terminal(cols=50, rows=38, extra_env={'HARBOR_GRAPHICS': 'kitty'})
    try:
        t.picker('Studio')
        t.send('\x1b[200~' + str(portrait) + '\x1b[201~', wait=1.5)
        portraits = []
        for kind, item in graphics(t.raw):
            if kind != 'image': continue
            mask = item.point(lambda _: 0).convert('L')
            mask.putdata([255 if r > 240 and g < 10 and b < 10 else 0 for r,g,b in item.getdata()])
            bounds = mask.getbbox()
            if bounds:
                x0,y0,x1,y1 = bounds
                assert abs((y1-y0)/(x1-x0) - 4) < .05
                assert x0 > item.width // 4 and x1 < item.width * 3 // 4
                portraits.append(item)
        assert portraits, 'No letterboxed portrait PNG was transmitted'
        t.send(b'\x1b'); t.picker('Marina')
        for _ in range(16):
            t.send(b'\x1b[<64;20;10M', wait=.1)
        placements = [item for kind,item in graphics(t.raw) if kind == 'placement']
        assert any(int(p[b'y']) > 0 for p in placements), 'No cropped scrolling placement observed'
        for p in placements:
            assert int(p[b'h']) > 0 and int(p[b'w']) > 0
            assert int(p[b'r']) > 0 and int(p[b'c']) > 0
        print('PASS portrait preserves aspect with letterboxing; scrolling crops source pixels')
    finally:
        t.close()
