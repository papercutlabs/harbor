"""Exercise Harbor through a real pseudoterminal, using only its offline protocol."""
import codecs
import base64
import hashlib
import io
import re
import subprocess
from PIL import Image
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import tempfile
import termios
import time
import pyte

ROOT = Path(__file__).resolve().parents[2]

class HarborScreen(pyte.Screen):
    # ncurses uses CSI S/T to move bottom-aligned history efficiently. pyte
    # omits these standard operations; emulate them inside the scroll margins.
    def scroll_up(self, count=1):
        x, y = self.cursor.x, self.cursor.y
        self.cursor.y = self.margins.bottom if self.margins else self.lines - 1
        for _ in range(min(count or 1, self.lines)): self.index()
        self.cursor.x, self.cursor.y = x, y
    def scroll_down(self, count=1):
        x, y = self.cursor.x, self.cursor.y
        self.cursor.y = self.margins.top if self.margins else 0
        for _ in range(min(count or 1, self.lines)): self.reverse_index()
        self.cursor.x, self.cursor.y = x, y

class HarborStream(pyte.ByteStream):
    csi = dict(pyte.ByteStream.csi, S='scroll_up', T='scroll_down')
    events = pyte.ByteStream.events | {'scroll_up', 'scroll_down'}

class Terminal:
    def __init__(self, cols=56, rows=38, config_dir=None, extra_env=None, executable=None):
        self.dir = tempfile.TemporaryDirectory(prefix='harbor-native-') if config_dir is None else None
        self.config = str(config_dir) if config_dir is not None else self.dir.name + '/config'
        self.cols, self.rows = cols, rows
        self.screen = HarborScreen(cols, rows)
        self.stream = HarborStream(self.screen)
        self.raw = bytearray()
        self.vt_pending = b''
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
        self.pid = os.fork()
        if self.pid == 0:
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            for i in range(3): os.dup2(slave, i)
            os.close(self.master)
            os.environ.update(TERM='xterm-256color', LANG='en_US.UTF-8',
                HARBOR_DEMO_FIXTURES=str(ROOT/'tests/harbor/fixtures'), HARBOR_REDUCED_MOTION='1', HARBOR_GRAPHICS='blocks', HARBOR_TEST_MUTE='1')
            os.environ.update(extra_env or {})
            exe = Path(executable or os.environ.get('HARBOR_EXECUTABLE', ROOT/'build-harbor/bin/harbor'))
            os.execv(str(exe), [str(exe), '--demo', '-d', self.config])
        os.close(slave)
        self.pump(.6)
        try: self.expect('⌥/ commands', timeout=10)
        except Exception:
            self.close()
            raise

    @property
    def text(self): return '\n'.join(self.screen.display)
    def pump(self, duration=.15):
        deadline=time.monotonic()+duration
        while time.monotonic()<deadline:
            if select.select([self.master], [], [], .02)[0]:
                try: data=os.read(self.master, 262144)
                except OSError: break
                if not data: break
                self.raw.extend(data)
                # pyte does not implement Kitty APCs; a terminal without graphics ignores them.
                self.vt_pending += data
                while self.vt_pending:
                    start=self.vt_pending.find(b'\x1b_')
                    if start<0:
                        keep=1 if self.vt_pending.endswith(b'\x1b') else 0
                        visible=self.vt_pending[:-keep] if keep else self.vt_pending
                        if visible:self.stream.feed(visible)
                        self.vt_pending=self.vt_pending[-keep:] if keep else b''
                        break
                    if start:self.stream.feed(self.vt_pending[:start])
                    apc_end=self.vt_pending.find(b'\x1b\\',start+2)
                    if apc_end<0:
                        self.vt_pending=self.vt_pending[start:]
                        break
                    self.vt_pending=self.vt_pending[apc_end+2:]
        return self.text
    def send(self, data, wait=.15):
        os.write(self.master, data.encode() if isinstance(data, str) else data)
        return self.pump(wait)
    def expect(self, text, timeout=3):
        end=time.monotonic()+timeout
        while text not in self.text and time.monotonic()<end: self.pump(.1)
        assert text in self.text, f'Missing {text!r}:\n{self.text}'
    def clear_draft(self, lines=1):
        # Ctrl+U deletes only the current hard line; Backspace then joins the previous one.
        self.send(b'\x05')
        for _ in range(lines-1): self.send(b'\x15');self.send(b'\x7f')
        self.send(b'\x15')
    def picker(self, name):
        self.send(b'\x0b');self.expect('Find a conversation');self.send(name);self.send(b'\r')
        deadline=time.monotonic()+3
        while 'Find a conversation' in self.text and time.monotonic()<deadline: self.pump(.1)
        assert 'Find a conversation' not in self.text, 'Chat picker did not select its result'
    def click_text(self, text, button=0):
        self.expect(text)
        y=next(i for i,line in enumerate(self.screen.display) if text in line)
        x=self.screen.display[y].index(text)+1
        self.send(f'\x1b[<{button};{x+1};{y+1}M',wait=.06)
        self.send(f'\x1b[<{button};{x+1};{y+1}m',wait=.1)
    def message_tool(self, message, tool):
        self.click_text(message, button=2)
        self.expect('Message actions')
        self.send(b'\x1b')
        self.click_text('/ commands')
        self.click_text('More message tools')
        self.expect('Message tools')
        self.click_text(tool)
    def snapshot(self, name):
        out=ROOT/'design'/name
        out.parent.mkdir(parents=True, exist_ok=True)
        out.with_suffix('.txt').write_text(self.text)
        out.with_suffix('.raw').write_bytes(self.raw)
        cells=[[{k:getattr(self.screen.buffer[y][x], k) for k in ('data','fg','bg','bold','reverse')} for x in range(self.cols)] for y in range(self.rows)]
        out.with_suffix('.json').write_text(json.dumps(cells))
    def resize(self, cols, rows):
        self.cols,self.rows=cols,rows
        self.screen.resize(rows,cols)
        fcntl.ioctl(self.master,termios.TIOCSWINSZ,struct.pack('HHHH',rows,cols,0,0))
        os.kill(self.pid,signal.SIGWINCH);self.pump(.25)
    def close(self):
        try:self.send(b'\x11',.1)
        except OSError:pass
        exited = False
        # Keep draining owned PTY output while ncurses restores the terminal; on macOS
        # endwin's TCSADRAIN can otherwise wait on this very fixture's unread bytes.
        for _ in range(25):
            result,_=os.waitpid(self.pid,os.WNOHANG)
            if result:
                exited = True
                break
            self.pump(.02)
        if not exited:
            os.kill(self.pid,signal.SIGTERM)
            for _ in range(50):
                result,_=os.waitpid(self.pid,os.WNOHANG)
                if result:
                    exited = True
                    break
                self.pump(.02)
        if not exited:
            os.kill(self.pid,signal.SIGKILL)
            os.waitpid(self.pid,0)
        os.close(self.master)
        if self.dir: self.dir.cleanup()

if __name__=='__main__':
    passed=[]
    def check(name):
        passed.append(name)
        print('PASS',name,flush=True)
    with tempfile.TemporaryDirectory(prefix='harbor-state-test-') as temp:
      config=Path(temp)/'config'
      t=Terminal(config_dir=config)
      try:
        t.expect('⌥/ commands');t.picker('Marina');assert 'HARBOR / TELEGRAM' not in t.text and 'connected' not in t.text;check('Native startup and chat search')
        t.send('Hello from Harbor');t.send(b'\r');t.expect('Sent');check('Enter sends through Protocol')
        t.send(b'\x1b');t.expect('MESSAGES');t.send(b'\r');t.expect('Message actions');check('Escape and Enter open message actions')
        t.send(b'\x1b');t.send('Draft kept');t.expect('Draft kept')
        t.picker('Studio');t.send('Studio draft');t.picker('Marina');t.expect('Draft kept');check('Drafts survive switching')
        t.send(b'\x15');t.send('/');t.expect('› /');assert 'Commands & shortcuts' not in t.text;t.send(b'\x15');check('Slash is literal in an empty composer')
        t.send(b'\x1b/');t.expect('Commands & shortcuts');t.send(b'\x1b');assert '› /' not in t.text;check('Option-/ opens commands from an empty composer')
        t.send('literal/');t.expect('literal/');assert 'Commands & shortcuts' not in t.text;t.send(b'\x1b[47;3u');t.expect('Commands & shortcuts');t.send(b'\x1b');t.expect('literal/');assert 'literal//' not in t.text;t.send(b'\x15');check('Slash is literal in nonempty drafts and enhanced Option-/ keeps them')
        t.send('line one');t.send(b'\x1b[13;2u');t.send('line two');t.expect('line two');t.expect('line one');check('CSI-u Shift+Enter inserts newline')
        t.clear_draft(2);t.send('Unicode: 你好 🌊 ǚ ɚ');t.expect('你好');t.expect('ɚ');check('Unicode and special-key code points remain distinct')
        t.send(b'\x1b[107;5u');t.expect('Find a conversation');t.send(b'\x1b');check('Enhanced Ctrl+K finds chats')
        t.send(b'\x1b[107;3u');t.expect('Find a conversation');t.send(b'\x1b');check('Enhanced Option+K finds chats')
        t.send(b'\x15');t.send('Persistent draft')
        t.picker('Saved Messages');t.send(b'\x1b[5~');t.pump(.2);saved_screen=t.screen.display[6:15]
        t.picker('Studio');t.picker('Saved Messages');assert t.screen.display[6:15]==saved_screen;check('Reading position survives switching')
        t.picker('Marina');t.expect('Persistent draft');t.send(b'\x15')
        # Use the real mouse-reporting path to select the voice message.
        t.send(b'\x1b[5~');t.message_tool('Voice · voice.wav','Open / play attachment');t.expect('playing',timeout=5);check('Mouse actions start native voice playback')
        t.send(' ');t.expect('paused');t.send(' ');t.expect('playing');check('Playback pauses and resumes in-app')
        t.picker('Studio');check('Switching away stops playback')
        t.picker('Marina');t.send(b'\x1b');t.send('caption')
        fixture=str(ROOT/'tests/harbor/fixtures/harbor.png')
        t.send('\x1b[200~'+fixture+'\x1b[201~',wait=.4);t.expect('Preview attachments');t.expect('harbor.png');t.click_text('Cancel');t.expect('caption');check('Dropped file preview cancels without losing draft')
        t.send('\x1b[200~'+fixture+'\x1b[201~',wait=.4);t.expect('Preview attachments');t.click_text('Send 1 attachment');t.expect('Sent');check('Attachment sends only after confirmation')
        t.send('Persistent final draft');t.resize(110,42);t.expect('CONVERSATIONS');t.snapshot('harbor-native-wide');check('Wide layout shows chat list')
        t.resize(50,38);assert 'CONVERSATIONS' not in t.text;t.expect('Persistent final draft');t.snapshot('harbor-native-narrow');check('50-column layout uses full conversation width')
        t.resize(28,15);t.expect('commands');t.resize(56,38);check('Small terminal resize does not crash')
      finally:t.close()
      state=config/'harbor-state';assert state.exists();assert state.stat().st_mode & 0o077 == 0
      t=Terminal(config_dir=config)
      try:t.picker('Marina');t.expect('Persistent final draft');check('Drafts persist across restart in a private state file')
      finally:t.close()
    with tempfile.TemporaryDirectory(prefix='harbor-edit-test-') as temp:
      edit_config=Path(temp)/'config'
      t=Terminal(config_dir=edit_config)
      try:
        t.picker('Marina');t.send('Unsent draft sentinel')
        t.message_tool('Keeping this one','Edit message');t.expect('Editing your message')
        t.send(b'\x15');t.send('An edit to cancel');t.send(b'\x1b');t.expect('Unsent draft sentinel')
        t.send('!');t.expect('Unsent draft sentinel!');check('Canceling an edit restores the existing draft and cursor')
        t.message_tool('Keeping this one','Edit message');t.send(b'\x15');t.send('Revised original');t.send(b'\r')
        t.expect('Revised original');t.expect('Unsent draft sentinel!');check('Saving an edit restores the unsent draft')
        t.message_tool('Revised original','Edit message');t.send(b'\x15');t.send('Unfinished edit')
      finally:t.close()
      t=Terminal(config_dir=edit_config)
      try:
        t.picker('Marina');t.expect('Unfinished edit');t.expect('Editing your message');t.send(b'\x1b');t.expect('Unsent draft sentinel!')
        check('Restart during editing preserves both the edit and original draft')
      finally:t.close()
    t=Terminal(extra_env={'HARBOR_DEMO_FAIL_EDIT':'1'})
    try:
      t.picker('Marina');t.send('Keep this normal draft');t.message_tool('Keeping this one','Edit message');t.send(b'\x15');t.send('An edit that fails');t.send(b'\r')
      t.expect('Send failed');t.expect('Keep this normal draft');t.click_text('/ commands');t.click_text('Recover a failed send')
      t.expect('Editing your message');t.expect('An edit that fails');t.send(b'\x1b');t.expect('Keep this normal draft')
      check('Failed edit recovery retains edit mode and the original unsent draft')
    finally:t.close()
    t=Terminal(extra_env={'HARBOR_DEMO_FAIL_SEND':'1'})
    try:
      t.picker('Marina');t.send('Recover this failed send');t.send(b'\r');t.expect('Send failed');t.expect('Recover this failed send');check('Failed send restores unsent text')
    finally:t.close()
    t=Terminal(extra_env={'HARBOR_DEMO_INCOMING_DELAY_MS':'2200'})
    try:
      t.picker('Marina');t.send(b'\x1b');t.send(b'\x1b');t.send(b'\x07');t.pump(1.7)
      t.expect('A live incoming reply.');check('Incoming reply follows after explicitly choosing Latest')
    finally:t.close()
    t=Terminal(extra_env={'HARBOR_DEMO_INCOMING_DELAY_MS':'2200'})
    try:
      t.picker('Marina');t.send(b'\x1b[5~');t.pump(1.7)
      assert 'A live incoming reply.' not in t.text
      t.expect('↓ 1 new message');t.send(b'\x07');t.expect('A live incoming reply.');assert 'new message' not in t.text
      check('Older reading position stays put with a visible new-message action')
    finally:t.close()
    with tempfile.TemporaryDirectory(prefix='harbor-opener-test-') as directory:
      opener=Path(directory)/'opener'
      opener.write_text('#!/usr/bin/env python3\nimport json,sys\nfrom pathlib import Path\nPath(__file__).with_suffix(".json").write_text(json.dumps(sys.argv[1:]))\n')
      opener.chmod(0o700)
      t=Terminal(extra_env={'HARBOR_TEST_OPENER':str(opener)})
      try:
        t.picker('Marina');t.pump(.5);assert '▀' in t.text
        t.message_tool('▷ morning.mp4','Open / play attachment')
        result=opener.with_suffix('.json');deadline=time.monotonic()+3
        while not result.exists() and time.monotonic()<deadline:t.pump(.1)
        assert json.loads(result.read_text())==[str(ROOT/'tests/harbor/fixtures/morning.mp4')]
        check('Video thumbnail action launches an external opener with the exact media path')
      finally:t.close()
    t=Terminal(extra_env={'HARBOR_ICONS':'nerd'})
    try:
      t.picker('Marina');t.expect('\U000f012d');assert '✓✓' not in t.text
      check('Read receipt uses one integrated double-check glyph')
    finally:t.close()
    t=Terminal(extra_env={'HARBOR_GRAPHICS':'kitty'})
    try:
      t.picker('Marina');t.pump(1.2)
      transfers={};active=None;payload=[];decoded=0
      for match in re.finditer(rb'\x1b_G(.*?)\x1b\\',bytes(t.raw),re.S):
        control,_,chunk=match[1].partition(b';')
        fields=dict(item.split(b'=',1) for item in control.split(b',') if b'=' in item)
        if fields.get(b'a')==b't': active=fields[b'i'];payload=[]
        if chunk and active:
          payload.append(chunk)
          if fields.get(b'm',b'0')==b'0':
            png=base64.b64decode(b''.join(payload));image=Image.open(io.BytesIO(png));image.verify();decoded+=1;active=None
        if fields.get(b'a')==b'p':
          assert fields.get(b'C')==b'1' and fields.get(b'q')==b'2'
          assert int(fields[b'c'])>0 and int(fields[b'r'])>0
      assert decoded>0,'No complete PNG transmitted to the terminal'
      check('Kitty graphics encodes valid PNG chunks and bounded placements')
    finally:t.close()
    with tempfile.TemporaryDirectory(prefix='harbor-config-guard-') as directory:
      sentinel=Path(directory)/'keep.txt';sentinel.write_text('preserve me')
      exe=os.environ.get('HARBOR_EXECUTABLE',str(ROOT/'build-harbor/bin/harbor'))
      result=subprocess.run([exe,'--demo','-s','-d',directory],capture_output=True,text=True,timeout=3)
      assert result.returncode==1 and sentinel.read_text()=='preserve me'
      check('Setup refuses to overwrite an unrelated directory')
    executable=Path(os.environ.get('HARBOR_EXECUTABLE',ROOT/'build-harbor/bin/harbor'))
    (ROOT/'design/harbor-native-validation.json').write_text(json.dumps({'passed':passed,'count':len(passed),'executable':str(executable),'sha256':hashlib.sha256(executable.read_bytes()).hexdigest(),'scope':'Real native executable through PTY using offline Protocol. No live Telegram messages sent.'},indent=2))
    print(str(len(passed))+' native checks passed')
