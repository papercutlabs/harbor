"""Read current chat through the real CLI and park duplicates without discarding composer state."""
from native_smoke import Terminal
from pathlib import Path
import contextlib, fcntl, json, os, re, shutil, subprocess, tempfile, time
def semantic_send_observation(requests,receipts,chat,text,target_pid,receipt_pid,records,expected_full_request,foreign_receipts=()):
    # The backend records the full wire identity and exact dispatched payload. Only the
    # addressed target writes this receipt file; other UIs cannot satisfy its low-ID join.
    if receipt_pid!=target_pid:return None
    candidates=[]
    for line in requests.splitlines():
        match=re.fullmatch(r'send chat=(\S+) text=(.*?) view=(\d+) index=\d+ request=(\d+) payload=([0-9A-Fa-f]+)',line)
        if not match or match[1]!=chat or match[2]!=text:continue
        full=int(match[4]);connection=full>>32;sequence=full&0xffffffff
        if not connection or not sequence or connection!=int(match[3]):continue
        # Decode just the public wire's first three counted UTF-8 strings: id, sender, text.
        payload=bytes.fromhex(match[5]);offset=0;fields=[]
        for _ in range(3):
            if offset+4>len(payload):break
            size=int.from_bytes(payload[offset:offset+4],'little');offset+=4
            if offset+size>len(payload):break
            fields.append(payload[offset:offset+size]);offset+=size
        if len(fields)!=3 or fields[2]!=text.encode():continue
        candidates.append((connection,sequence))
    if len(candidates)!=1:return None
    connection,sequence=candidates[0]
    if ((connection<<32)|sequence)!=expected_full_request:return None
    known={int(r['connection_id']) for r in records if r.get('pid')==target_pid and r.get('connection_id','0')!='0'}
    assert not known or known=={connection},('target connection disagrees',known,connection)
    sends=[r for r in records if r.get('event')=='request.sent' and r.get('pid')==target_pid and r.get('operation')=='message.send']
    assert all((r.get('connection_id'),r.get('request_id'))==(str(connection),str(sequence)) for r in sends),sends
    for sent in sends:
        results=[r for r in records if r.get('event')=='request.result.received' and r.get('pid')==target_pid and
            (r.get('connection_id'),r.get('request_id'))==(sent['connection_id'],sent['request_id'])]
        assert all(r.get('outcome')!='failed' for r in results),results
    success=r'\d+ receive send request='+str(sequence)+r' pending=0 success=1'
    if not any(re.fullmatch(success,line) for line in receipts.splitlines()):return None
    assert not any(re.fullmatch(success,line) for receipt in foreign_receipts for line in receipt.splitlines()),'semantic success reached another UI'
    return {'target_pid':target_pid,'connection_id':connection,'request_id':sequence}

exe=os.environ['HARBOR_EXECUTABLE']
with tempfile.TemporaryDirectory(prefix='hi-',dir='/tmp') as directory:
    config=Path(directory)/'config'; home=Path(directory)/'home';home.mkdir();(home/'.local/state').mkdir(parents=True)
    env=dict(HOME=str(home),XDG_CONFIG_HOME=str(home/'xdg'),XDG_STATE_HOME=str(home/'.local/state'),HARBOR_DEMO_TOPICS='1',HARBOR_DEMO_ROUTES='1')
    requests=home/'demo-requests.log';receipts=home/'target-native-events.log'
    foreign_recorders=(home/'backend-ui-native-events.log',home/'other-ui-native-events.log')
    env.update(HARBOR_DEMO_REQUEST_LOG=str(requests),HARBOR_TEST_EVENT_LOG='')
    windows=[]
    def control(window,surface,*flags):
        child=subprocess.Popen([exe,'-d',str(config),'--target-surface',surface,'--target-pid',str(window.pid),*flags],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=dict(os.environ,**env))
        if '--inspect-window' in flags:inspection_pids.append(child.pid)
        until=time.monotonic()+10
        while child.poll() is None and time.monotonic()<until:
            for view in windows:view.pump(.01)
        if child.poll() is None:child.kill();child.wait();raise AssertionError('control did not finish')
        out,err=child.communicate();return child.returncode,out,err
    def inspect(window,surface):
        code,out,err=control(window,surface,'--inspect-ui');assert code==0,err
        fields=out.rstrip('\n').split('\t');assert len(fields)==9,fields
        assert fields[:3]==['pcl.harbor.ui.v2',str(window.pid),surface]
        return fields
    samples=[];inspection_pids=[]
    def inspect_window(window,surface):
        code,out,err=control(window,surface,'--inspect-window');assert code==0,err
        fields=out.rstrip('\n').split('\t');assert len(fields)==10,fields
        assert fields[:3]==['pcl.harbor.window.v1',str(window.pid),surface]
        assert fields[3].startswith(str(window.pid)+'-') and fields[4].isdigit()
        assert fields[5].count('.')==2 and fields[6]
        assert all(not value or value.isdigit() for value in fields[7:9])
        assert fields[9] in ('picker','conversation')
        samples.append(out);return fields
    def retain_failure():
        # Preserve the owned fictional demo's actual evidence before profile cleanup.
        # On CI the bounded stdout summary survives even without an artifact directory.
        try:
            destination=Path(os.environ.get('HARBOR_EVIDENCE_DIR') or tempfile.mkdtemp(prefix='harbor-identity-failure-'))
            destination.mkdir(parents=True,exist_ok=True)
            journal=home/'.local/state/harbor/operational.jsonl'
            records=[]
            if journal.exists():
                shutil.copyfile(journal,destination/'operational-native.jsonl')
                for line in journal.read_text().splitlines():
                    try:records.append(json.loads(line))
                    except json.JSONDecodeError:pass # retain the original bytes, including a concurrent final write
            for recorder in (requests,receipts,*foreign_recorders):
                if recorder.exists():shutil.copyfile(recorder,destination/recorder.name)
            (destination/'window-observations.json').write_text(json.dumps(samples))
            views=[]
            for index,view in enumerate(windows):
                (destination/f'window-{index}.raw').write_bytes(view.raw)
                (destination/f'window-{index}.txt').write_text(view.text)
                views.append({'pid':view.pid,'screen':view.text})
            keys=('event','pid','ts_ms','run_id','operation','request_id','connection_id','outcome','type','cause')
            events=[{k:r[k] for k in keys if k in r} for r in records
                if r.get('pid') in [v.pid for v in windows] or r.get('event') in
                ('request.backend.received','request.result.produced','backend.shutdown')]
            summary={'evidence_directory':str(destination),'windows':views,'window_samples':samples,'events':events[-60:]}
            (destination/'failure-summary.json').write_text(json.dumps(summary))
            print('IDENTITY_FAILURE_EVIDENCE '+json.dumps(summary),flush=True)
        except Exception as evidence_error:
            print('IDENTITY_FAILURE_EVIDENCE_ERROR '+type(evidence_error).__name__,flush=True)
    try:
        backend_window=Terminal(cols=110,config_dir=config,extra_env=dict(env,CMUX_SURFACE_ID='IDENTITY-BACKEND',HARBOR_TEST_EVENT_LOG=str(foreign_recorders[0])));windows.append(backend_window)
        backend_window.picker('Saved Messages')
        first=Terminal(cols=110,config_dir=config,extra_env=dict(env,CMUX_SURFACE_ID='IDENTITY-ONE',HARBOR_TEST_EVENT_LOG=str(receipts)),args=['--open-telegram-chat','12345','--open-telegram-topic','421']);windows.append(first)
        second=Terminal(cols=110,config_dir=config,extra_env=dict(env,CMUX_SURFACE_ID='IDENTITY-TWO',HARBOR_TEST_EVENT_LOG=str(foreign_recorders[1])));windows.append(second)
        first.expect('Agent');first.send('Retained unique draft');initial=inspect(first,'IDENTITY-ONE')
        window_initial=inspect_window(first,'IDENTITY-ONE');other_window=inspect_window(second,'IDENTITY-TWO')
        assert window_initial[3]!=other_window[3], 'two native windows shared one runtime identity'
        assert initial[4].endswith('#421') and initial[6:8] == ['1','1'] and initial[8] == '0',initial
        first.picker('Doug');different=inspect(first,'IDENTITY-ONE');assert different[4].endswith('#550'),different
        window_different=inspect_window(first,'IDENTITY-ONE')
        assert window_initial[3:7]==window_different[3:7] and window_initial[8]!=window_different[8]
        assert different[4]!=initial[4], 'query reused original launch argv'
        first.picker('Agent');current=inspect(first,'IDENTITY-ONE');first.send(' changed')
        assert control(first,'IDENTITY-ONE','--park-ui',current[5])[0]!=0,'changed composer was parked from old evidence'
        first.expect('Retained unique draft changed');current=inspect(first,'IDENTITY-ONE');other=second.text
        assert control(first,'IDENTITY-ONE','--park-ui',current[5])[0]==0
        parked=inspect(first,'IDENTITY-ONE');assert parked[3:5]==['',''],parked
        assert inspect_window(first,'IDENTITY-ONE')[7:]==['','','picker']
        first.pump(.8);assert inspect(first,'IDENTITY-ONE')[4]=='','incoming chats reopened parked view'
        assert second.text==other,'another UI changed'
        first.picker('Agent');first.expect('Retained unique draft changed')
        reopened=inspect(first,'IDENTITY-ONE');assert reopened[4]==initial[4]
        assert inspect_window(first,'IDENTITY-ONE')[8]==window_initial[8]
        trace=home/'.local/state/harbor/operational.jsonl'
        # Optional contention mode proves delivery qualification does not depend on
        # the best-effort journal retaining this single send's records.
        with contextlib.ExitStack() as stack:
            if os.environ.get('HARBOR_IDENTITY_JOURNAL_LOCK')=='1':
                held=stack.enter_context(trace.open('a'));fcntl.flock(held,fcntl.LOCK_EX)
            first.send('\r') # This fictional payload is dispatched only by the addressed target.
            until=time.monotonic()+6;dispatched_request=None
            while time.monotonic()<until:
                records=[json.loads(line) for line in trace.read_text().splitlines()]
                backend_records=requests.read_text() if requests.exists() else ''
                # Freeze the actual backend identity for this target-only payload before
                # matching receipts; a foreign connection with the same low ID cannot replace it.
                if dispatched_request is None:
                    identities=re.findall(r'^send chat='+re.escape(reopened[4])+r' text=Retained unique draft changed view=\d+ index=\d+ request=(\d+) payload=',backend_records,re.M)
                    assert len(identities)<=1,identities
                    if identities:dispatched_request=int(identities[0])
                observation=semantic_send_observation(backend_records,
                    receipts.read_text() if receipts.exists() else '',reopened[4],
                    'Retained unique draft changed',first.pid,first.pid,records,dispatched_request,
                    [r.read_text() if r.exists() else '' for r in foreign_recorders])
                if observation:break
                first.pump(.03)
            else:raise AssertionError('semantic send did not retain its correlated native success')
        first.delivered('Retained unique draft changed')
        sent_state=inspect(first,'IDENTITY-ONE');assert sent_state[6]=='0',sent_state
        body_row=next(i for i,line in enumerate(first.screen.display) if 'Retained unique draft changed' in line and '›' not in line)
        assert any('\U000f012d' in line or ' read' in line for line in first.screen.display[body_row+1:body_row+3]),first.text
        print('NATIVE_SEMANTIC_SEND '+json.dumps(observation),flush=True)
        data=trace.read_text();assert 'Retained unique draft' not in data and ' changed' not in data
        assert all(r.get('schema')==1 and r.get('component')=='harbor' and isinstance(r.get('pid'),int) and r.get('run_id') for r in records),records
        assert any(json.loads(line).get('event')=='ui.chat.opened' for line in data.splitlines())
        assert not any(json.loads(line).get('pid') in inspection_pids for line in data.splitlines()),'operational reads wrote the journal and would trigger a reporting loop'
        evidence=os.environ.get('HARBOR_EVIDENCE_DIR')
        if evidence:
            destination=Path(evidence);destination.mkdir(parents=True,exist_ok=True)
            (destination/'window-observations.json').write_text(json.dumps(samples))
            (destination/'operational-native.jsonl').write_text(data)
            for recorder in (requests,receipts,*foreign_recorders):
                if recorder.exists():shutil.copyfile(recorder,destination/recorder.name)
        print('NATIVE_IDENTITY_SAMPLES '+json.dumps({'initial':initial,'switched':different,'parked':parked,'reopened':reopened}),flush=True)
        print('PASS actual CLI current identity differs from launch argv; conditional parking rejects draft races; all draft state and other UI preserved',flush=True)
    except BaseException:
        retain_failure()
        raise
    finally:
        try:
            for view in windows:view.close()
            if windows:windows[-1].wait_backend_shutdown()
            if windows:windows[-1].wait_backend_shutdown()
        except BaseException:
            retain_failure()
            raise
