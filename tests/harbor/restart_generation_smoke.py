"""Real owned backends: wait for the old lifetime, accept a replacement despite inode alias.

The alias is deterministic: the exported old dev:ino observation is set equal to the
replacement socket. Socket objects, generation publishers and lifetime locks are native.
HARBOR_LEGACY_EXECUTABLE additionally qualifies an actual installed pre-generation binary.
"""
import json, os, re, subprocess, tempfile
from pathlib import Path
from native_smoke import Terminal, ROOT

binary=Path(os.environ.get('HARBOR_EXECUTABLE',ROOT/'build-harbor/bin/harbor')).resolve()
legacy=os.environ.get('HARBOR_LEGACY_EXECUTABLE')
receipts=[]

class WaitingWindow(Terminal):
    def __init__(self, **arguments):
        self.awaiting=True
        super().__init__(**arguments)
        self.awaiting=False
    def expect(self, needle, timeout=10):
        return super().expect('Updating Harbor: waiting' if self.awaiting and needle=='Ctrl+/' else needle,timeout)

def identity(config):
    info=(config/'session.sock').stat()
    return str(info.st_dev)+':'+str(info.st_ino)

def generation(config):
    fields=(config/'session.generation').read_text().rstrip('\n').split('\t')
    assert len(fields)==3 and fields[0]=='pcl.harbor.session-generation.v1' and re.fullmatch('[a-f0-9]{32}',fields[1])
    assert fields[2]==identity(config)
    return fields[1]

def backend(config):
    lines=subprocess.check_output(['ps','-A','-o','pid=,args=','-ww'],text=True).splitlines()
    found=[int(row.split()[0]) for row in lines if '--session-backend ' in row and ('-d '+str(config)) in row]
    assert len(found)==1,found
    return found[0]

with tempfile.TemporaryDirectory(prefix='harbor-generation-') as directory:
    home=Path(directory);config=home/'profile';windows=[]
    env={'HOME':str(home),'HARBOR_RESTART_OLD_SOCKET':'','HARBOR_RESTART_OLD_GENERATION':'','HARBOR_RESTART_STATE_ID':''}
    try:
        a=Terminal(config_dir=config,extra_env=env,executable=binary);windows.append(a)
        a.picker('Studio');a.send('old-generation draft');old_generation=generation(config);old_backend=backend(config)
        b=WaitingWindow(config_dir=config,extra_env=dict(env,HARBOR_RESTART_OLD_SOCKET=identity(config),HARBOR_RESTART_OLD_GENERATION=old_generation),executable=binary);windows.append(b)
        a.expect('old-generation draft');assert backend(config)==old_backend
        # b has not attached: a's ordinary close is sufficient to retire the exact old backend.
        a.close();windows.remove(a);b.expect('Ctrl+/');b.expect('old-generation draft')
        new_generation=generation(config);new_backend=backend(config)
        assert new_generation!=old_generation and new_backend!=old_backend
        alias=identity(config)
        c=Terminal(config_dir=config,extra_env=dict(env,HARBOR_RESTART_OLD_SOCKET=alias,HARBOR_RESTART_OLD_GENERATION=old_generation),executable=binary);windows.append(c)
        c.picker('Kleya');c.send('replacement-generation draft');b.expect('old-generation draft');assert backend(config)==new_backend
        # Existing 0.1.26 exporters have no generation field. A live new publisher proves
        # this socket belongs to the replacement, even when the old inode observation aliases.
        d=Terminal(config_dir=config,extra_env=dict(env,HARBOR_RESTART_OLD_SOCKET=alias),executable=binary);windows.append(d)
        assert backend(config)==new_backend
        receipts.append({'case':'same-version lifetime wait and deterministic socket alias','old_backend':old_backend,'new_backend':new_backend,'old_generation':old_generation,'new_generation':new_generation,'alias_identity':alias,'generation_aware_and_legacy_aliases':'accepted','drafts_preserved':True})
        print('PASS exact live old generation blocks; its exit releases the waiter; replacement generation and legacy bootstrap accept an aliased socket; other UI/drafts retained',flush=True)
    finally:
        for window in reversed(windows):window.close()
        if windows:windows[-1].wait_backend_shutdown()
    if legacy:
        config=home/'legacy';windows=[]
        try:
            a=Terminal(config_dir=config,extra_env=env,executable=Path(legacy).resolve());windows.append(a)
            a.picker('Studio');a.send('legacy-backend draft');old_backend=backend(config)
            assert not (config/'session.generation').exists()
            b=WaitingWindow(config_dir=config,extra_env=dict(env,HARBOR_RESTART_OLD_SOCKET=identity(config)),executable=binary);windows.append(b)
            a.send(' retained');a.expect('legacy-backend draft retained');assert backend(config)==old_backend
            a.close();windows.remove(a);b.expect('Ctrl+/');b.expect('legacy-backend draft retained')
            assert backend(config)!=old_backend and generation(config)
            receipts.append({'case':'actual legacy backend bootstrap','legacy_binary':str(Path(legacy).resolve()),'old_backend':old_backend,'new_backend':backend(config),'draft_preserved':True})
            print('PASS actual legacy backend remains usable and blocks replacement until its last UI exits; new window restores its draft',flush=True)
        finally:
            for window in reversed(windows):window.close()
            if windows:windows[-1].wait_backend_shutdown()
    else:print('SKIP actual legacy binary bootstrap: HARBOR_LEGACY_EXECUTABLE not supplied; deterministic legacy-format alias exercised above',flush=True)
print('RESTART_GENERATION_RECEIPTS '+json.dumps(receipts),flush=True)
