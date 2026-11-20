"""Hosted current-source Harbor decoder/render proof for the warm owned child. No accounts or live cmux."""
import os, sys, tempfile, signal, time
from pathlib import Path
from native_smoke import Terminal
with tempfile.TemporaryDirectory(prefix='harbor-warm-') as directory:
    work=Path(directory); child=work/'host.py'; calls=work/'calls'
    child.write_text('''import fcntl,os,select,socket,sys,termios
s=socket.socket(fileno=int(os.environ['HARBOR_DESK_HOST_FD']));f=s.makefile('rwb',buffering=0)
count=0;view=''
while True:
 line=f.readline().decode().strip()
 if not line:break
 verb,ident,*rest=line.split('\\t')
 if verb=='prepare':view=rest[0];f.write(('ready\\t'+ident+'\\n').encode())
 elif verb=='cancel':f.write(('released\\t'+ident+'\\tok\\n').encode())
 elif verb=='yield':
  count+=1
  with open(os.environ['CALLS'],'a') as out:out.write('%d %d %s\\n'%(os.getpid(),count,view))
  if view=='help':f.write(b'released\\t999\\tok\\n');continue
  if view=='now':os._exit(4)
  mode=termios.tcgetattr(0);mode[3]&=~(termios.ICANON|termios.ECHO);mode[6][termios.VMIN]=1;mode[6][termios.VTIME]=0;termios.tcsetattr(0,termios.TCSANOW,mode)
  print('\\x1b[=0u\\x1b[2J\\x1b[HWARM OFFICE %d VISIT %d'%(os.getpid(),count),flush=True)
  data=b''
  while True:
   select.select([0],[],[]);key=os.read(0,1)
   if key==b'\\x03':break
   if key==b'\\x1b':
    seq=b''
    while select.select([0],[],[],.03)[0]:
     seq+=os.read(0,1)
     if seq.endswith((b'M',b'm')):break
    if seq.startswith(b'[<'):continue
    break
   data+=key
  with open(os.environ['CALLS'],'a') as out:out.write('KEYS '+repr(data)+'\\n')
  fcntl.fcntl(0,fcntl.F_SETFL,fcntl.fcntl(0,fcntl.F_GETFL)|os.O_NONBLOCK)
  f.write(('released\\t'+ident+'\\tok\\n').encode())
''')
    commands=work/'commands'
    commands.write_text('\n'.join('\t'.join([label,'Warm view',sys.executable,str(child),'desk','--view',view]) for label,view in [('Everything','everything'),('Quota','quota'),('Malformed','help'),('Death','now')])+'\n')
    t=Terminal(cols=72,rows=38,extra_env={'HARBOR_DESK_COMMANDS':str(commands),'HARBOR_DESK_WARM':'1','CALLS':str(calls)})
    def state():
        t.pump(2.2)
        return (Path(t.config)/'harbor-state').read_text()
    def enter(label):
        t.send(b'\x0f');t.expect('Desk · '+label);t.click_text('Desk · '+label)
    try:
        pid=t.pid;t.picker('Marina');t.send('draft / e q');t.send(b'\x1b[D');before=state()
        raw_at=len(t.raw)
        for n,label in enumerate(['Everything','Quota','Everything'],1):
            t.send(b'\x0f');t.expect('Desk · '+label);t.send('e' if label=='Everything' else 'q');t.expect('VISIT '+str(n));assert 'draft / e q' not in t.text
            t.resize(60+n,25+n);t.send(b'only-desk\x1b');t.expect('Back from Desk');t.resize(72,38)
            assert state()==before
        records=calls.read_text().splitlines();hosts={row.split()[0] for row in records if not row.startswith('KEYS')}
        assert len(hosts)==1,records
        assert [r for r in records if r.startswith('KEYS')]==["KEYS b'only-desk'"]*3,records
        assert b'\x1b[?1049l' not in t.raw[raw_at:] and b'\x1b[?1049h' not in t.raw[raw_at:],'alternate screen switched'
        # Reply/edit state belongs to the same live conversation, including the draft behind an edit.
        t.click_text('The boats are just starting',button=2);t.expect('Message actions');t.click_text('Reply');t.expect('Replying');reply=state()
        enter('Everything');t.expect('VISIT 4');t.send(b'\x1b');t.expect('Back from Desk');assert state()==reply
        t.send(b'\x1b');t.message_tool('Keeping this one','Edit message');t.expect('Editing your message')
        t.send(b'\x15');t.send('Unfinished warm edit');editing=state()
        enter('Quota');t.expect('VISIT 5');t.send(b'\x1b');t.expect('Back from Desk');assert state()==editing
        t.send(b'\x1b');t.expect('draft / e q')
        # The real Harbor reader and reading position survive a second set of transfers.
        t.send(b'\x1b');t.send(b'\x1b[A');reading=state()
        enter('Quota');t.expect('VISIT 6');t.send(b'\x03');t.expect('Back from Desk');assert state()==reading
        for label in ['Malformed','Death']:
            enter(label);t.expect('Desk ended unexpectedly');assert state()==reading
        assert t.pid==pid
        t.send(b'\x1b');t.send('!');t.expect('!')
        print('PASS warm child reused; alternate screen, draft/cursor/reading, resize, Ctrl+C, late reply and exact child death')
    finally:t.close()

    # An explicit signal to Harbor while the child owns input unwinds through ordinary cleanup.
    t=Terminal(cols=72,rows=38,extra_env={'HARBOR_DESK_COMMANDS':str(commands),'HARBOR_DESK_WARM':'1','CALLS':str(calls)})
    reaped=False
    try:
        t.picker('Marina');t.send('draft before termination');before=state()
        enter('Quota');t.expect('VISIT 1')
        child_pid=int([row for row in calls.read_text().splitlines() if not row.startswith('KEYS')][-1].split()[0])
        os.kill(t.pid,signal.SIGTERM);end=time.monotonic()+4
        while time.monotonic()<end:
            t.pump(.05);pid,status=os.waitpid(t.pid,os.WNOHANG)
            if pid:
                reaped=True;break
        assert reaped,'Harbor failed to unwind before its existing signal watchdog'
        assert os.WIFSIGNALED(status) and os.WTERMSIG(status)==signal.SIGTERM,status
        assert (Path(t.config)/'harbor-state').read_text()==before
        assert b'\x1b[?1049l' in t.raw,'ordinary terminal cleanup did not leave alternate screen'
        try:os.kill(child_pid,0)
        except ProcessLookupError:pass
        else:raise AssertionError('owned Office child survived Harbor termination')
        print('PASS explicit active Harbor termination reaps exact child and performs ordinary state/terminal cleanup')
    finally:
        if reaped:
            os.close(t.master)
            if t.dir:
                # The UI was already reaped above, so close() cannot perform its normal wait.
                t.wait_backend_shutdown()
                t.dir.cleanup()
        else:t.close()
