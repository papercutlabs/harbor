"""Compile and exercise the production owned-child protocol without building Harbor or touching a real session."""
import os, subprocess, sys, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='harbor-owned-view-') as tmp:
    work = Path(tmp)
    host = work/'host.py'
    host.write_text('''import os, socket, sys, time
from pathlib import Path
Path(os.environ['PID_FILE']).write_text(str(os.getpid()))
s = socket.socket(fileno=int(os.environ['HARBOR_DESK_HOST_FD']))
f = s.makefile('rwb', buffering=0)
mode = os.environ['MODE']
while True:
 line=f.readline().decode().strip()
 if not line: break
 verb, ident, *rest=line.split('\\t')
 if verb=='prepare':
  if mode=='before': sys.exit(7)
  if mode=='reject': f.write(('rejected\\t'+ident+'\\n').encode()); continue
  if mode=='cancel': time.sleep(.05)
  f.write(('ready\\t'+ident+'\\n').encode())
 elif verb=='cancel': f.write(('released\\t'+ident+'\\tok\\n').encode())
 elif verb=='yield':
  if mode=='exit': sys.exit(3)
  if mode=='malformed': f.write(b'not-release\\n'); time.sleep(20)
  elif mode=='late': f.write(b'released\\t999\\tok\\n'); time.sleep(20)
  elif mode=='eof': s.shutdown(socket.SHUT_RDWR); time.sleep(20)
  else: f.write(('released\\t'+ident+'\\tok\\n').encode())
''')
    driver = work/'driver.cpp'
    driver.write_text('''#include "deskhost.h"
#include <cassert>
#include <iostream>
int main(int, char** argv) {
 HarborDeskHost host;
 std::vector<std::string> args{argv[1],argv[2],"desk","--view","everything"};
 const std::string mode=getenv("MODE");
 for(int i=0;i<3;i++) {
  const auto ready=host.Prepare(args,[&]{return mode=="cancel" && access(getenv("PID_FILE"),F_OK)==0;});
  if(mode=="reject") { assert(ready==HarborDeskHost::Prepared::Rejected); break; }
  if(mode=="before") { assert(ready==HarborDeskHost::Prepared::Failed); break; }
  if(mode=="cancel") { assert(ready==HarborDeskHost::Prepared::Cancelled); break; }
  assert(ready==HarborDeskHost::Prepared::Ready);
  const bool ok=host.Visit(); assert(ok==(mode=="ok")); if(!ok) break;
 }
 host.Stop();
 std::cout<<"PASS "<<mode<<"\\n";
}
''')
    binary=work/'driver'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-I'+str(root/'src/harbor'),str(driver),'-o',str(binary)],check=True)
    for mode in ['ok','reject','before','cancel','exit','malformed','late','eof']:
        pidfile=work/('pid-'+mode)
        env={**os.environ,'HARBOR_DESK_WARM':'1','MODE':mode,'PID_FILE':str(pidfile)}
        subprocess.run([str(binary),sys.executable,str(host)],env=env,check=True,timeout=5)
        pid=int(pidfile.read_text())
        try: os.kill(pid,0)
        except ProcessLookupError: pass
        else: raise AssertionError(f'{mode}: owned child {pid} survived protocol return')
