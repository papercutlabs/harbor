"""Compile and exercise the production owned-child protocol without building Harbor or touching a real session."""
import os, subprocess, sys, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='harbor-owned-view-') as tmp:
    work = Path(tmp)
    host = work/'host.py'
    host.write_text('''import os, socket, sys, time, signal
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
  if mode=='before': sys.stderr.write('ORIGINAL-STARTUP-'+'x'*8192+'\\n'); sys.stderr.flush(); sys.exit(7)
  if mode=='reject': f.write(('rejected\\t'+ident+'\\n').encode()); continue
  if mode=='cancel': time.sleep(.05)
  f.write(('ready\\t'+ident+'\\n').encode())
 elif verb=='cancel': f.write(('released\\t'+ident+'\\tok\\n').encode())
 elif verb=='yield':
  if mode=='exit': sys.exit(3)
  if mode=='signal': os.kill(os.getpid(),signal.SIGTERM)
  if mode=='terminate': Path(os.environ['PID_FILE']+'.active').write_text('active'); time.sleep(20)
  if mode=='failed': sys.stderr.write('ORIGINAL-ACTIVATION-CAUSE\\n');sys.stderr.flush();f.write(('released\\t'+ident+'\\tfailed\\n').encode());continue
  if mode=='malformed': f.write(b'not-release\\n'); time.sleep(20)
  elif mode=='late': f.write(b'released\\t999\\tok\\n'); time.sleep(20)
  elif mode=='eof': s.shutdown(socket.SHUT_RDWR); time.sleep(20)
  else: f.write(('released\\t'+ident+'\\tok\\n').encode())
''')
    driver = work/'driver.cpp'
    driver.write_text('''#include "deskhost.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
static volatile sig_atomic_t exited=0;
int main(int, char** argv) {
 signal(SIGCHLD, [](int){exited=1;});
 HarborDeskHost host;
 std::vector<std::string> args{argv[1],argv[2],"desk","--view","everything"};
 const std::string mode=getenv("MODE");
 if(mode=="missing" || mode=="permission" || mode=="spawn") args[0]=getenv("FAIL_EXE");
 for(int i=0;i<3;i++) {
  const auto ready=host.Prepare(args,[&]{if(mode=="before") {while(!exited) usleep(1000);} return mode=="cancel" && access(getenv("PID_FILE"),F_OK)==0;});
  if(mode=="reject") { assert(ready==HarborDeskHost::Prepared::Rejected); break; }
  if(mode=="before" || mode=="missing" || mode=="permission" || mode=="spawn") { assert(ready==HarborDeskHost::Prepared::Failed); break; }
  if(mode=="cancel") { assert(ready==HarborDeskHost::Prepared::Cancelled); break; }
  assert(ready==HarborDeskHost::Prepared::Ready);
  const bool ok=host.Visit([&]{if(mode=="exit" || mode=="signal") {while(!exited) usleep(1000);} return mode=="terminate" && access((std::string(getenv("PID_FILE"))+".active").c_str(),F_OK)==0;}); assert(ok==(mode=="ok")); if(!ok) break;
 }
 host.Stop();
 std::ifstream file(host.Diagnostics()); std::stringstream bytes; bytes << file.rdbuf(); const auto original=bytes.str();
 assert(!host.Diagnostics().empty());
 if(mode=="before") {assert(original.find("ORIGINAL-STARTUP-"+std::string(8192,'x'))!=std::string::npos);assert(original.find("exited 7")!=std::string::npos);}
 if(mode=="exit") assert(original.find("exited 3")!=std::string::npos);
 if(mode=="signal") assert(original.find("signal 15")!=std::string::npos);
 if(mode=="terminate") assert(original.find("Harbor termination requested")!=std::string::npos && original.find("signal 9")!=std::string::npos);
 if(mode=="failed") assert(original.find("ORIGINAL-ACTIVATION-CAUSE")!=std::string::npos);
 if(mode=="missing" || mode=="permission") assert(original.find("access "+args[0])!=std::string::npos);
 if(mode=="spawn") assert(original.find("posix_spawn "+args[0])!=std::string::npos);
 if(mode!="ok") assert(!host.Error().empty());
 unlink(host.Diagnostics().c_str());
 std::cout<<"PASS "<<mode<<"\\n";
}
''')
    binary=work/'driver'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-I'+str(root/'src/harbor'),str(driver),'-o',str(binary)],check=True)
    for mode in ['ok','reject','before','cancel','exit','signal','terminate','failed','malformed','late','eof','missing','permission','spawn']:
        pidfile=work/('pid-'+mode)
        fail=work/('exe-'+mode)
        if mode in ['permission','spawn']:fail.write_text('not an executable format');fail.chmod(0o700 if mode=='spawn' else 0o600)
        env={**os.environ,'HARBOR_DESK_WARM':'1','MODE':mode,'PID_FILE':str(pidfile),'FAIL_EXE':str(fail)}
        subprocess.run([str(binary),sys.executable,str(host)],env=env,check=True,timeout=5)
        if mode in ['missing','permission','spawn']:continue
        pid=int(pidfile.read_text())
        try: os.kill(pid,0)
        except ProcessLookupError: pass
        else: raise AssertionError(f'{mode}: owned child {pid} survived protocol return')
