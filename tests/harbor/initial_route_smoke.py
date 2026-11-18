"""Initial exact Telegram chat/topic belongs only to its new native UI."""
from native_smoke import Terminal
from pathlib import Path
import os
import subprocess
import tempfile

exe=os.environ['HARBOR_EXECUTABLE']
with tempfile.TemporaryDirectory(prefix='harbor-route-') as directory:
    unit=Path(directory)/'initial-route-test'
    root=Path(__file__).resolve().parents[2]
    subprocess.run(['c++','-std=c++17','-I',str(root/'src/harbor'),str(Path(__file__).with_suffix('.cpp').with_name('initial_route_test.cpp')),'-o',str(unit)],check=True)
    subprocess.run([str(unit)],check=True)
    config=Path(directory)/'config'
    env={'HOME':directory,'XDG_CONFIG_HOME':directory,'HARBOR_DEMO_TOPICS':'1','HARBOR_DEMO_ROUTES':'1'}
    first=Terminal(cols=90,config_dir=config,extra_env=env)
    second=None
    try:
        first.picker('Saved Messages');first.send('first private draft')
        before=first.text
        second=Terminal(cols=90,config_dir=config,extra_env=env,args=['--open-telegram-chat','12345','--open-telegram-topic','421'])
        second.expect('Routed group > Agent');second.expect('Topic-specific history')
        second.send('second private draft')
        first.pump(.3)
        assert first.text==before, 'Opening another UI changed the existing window'
        owned=subprocess.run(['ps','-eo','pid=,args='],capture_output=True,text=True,check=True).stdout
        assert len([line for line in owned.splitlines() if '--session-backend' in line and str(config) in line])==1
        second.close();second=None
        # Same free slot previously saved another topic/draft. Explicit route wins over it.
        second=Terminal(cols=90,config_dir=config,extra_env=env,args=['--open-telegram-chat','12345','--open-telegram-topic','550','--open-profile','HarborDemo_local'])
        second.expect('Routed group > Doug');second.expect('A different topic')
        assert 'second private draft' not in second.text
        second.close();second=None
        second=Terminal(cols=90,config_dir=config,extra_env=env,args=['--open-telegram-chat','12345'])
        second.expect('Routed group');second.expect('Parent group history.')
        assert 'Topic-specific history' not in second.text
        second.close();second=None
        second=Terminal(cols=90,config_dir=config,extra_env=env,args=['--open-telegram-chat','12345','--open-telegram-topic','999'])
        second.expect('Opening requested conversation')
        assert 'Parent group history.' not in second.text and 'Note 35:' not in second.text
        # This wait establishes the actual bounded remote-discovery failure, not a shortened clock.
        second.expect('Requested conversation unavailable.',timeout=34)
        assert 'No other chat was opened.' in second.text
        second.send(b'\x12');second.expect('Opening requested conversation')
        second.picker('Saved Messages');second.expect('Note 35:')
        first.pump(.3);assert first.text==before
    finally:
        if second: second.close()
        first.close()
    for args in [ ['--open-telegram-chat','0'], ['--open-telegram-chat','9223372036854775808'],
                  ['--open-telegram-chat','1x'], ['--open-telegram-topic','421'],
                  ['--open-telegram-chat','12345','--open-telegram-topic','0'],
                  ['--open-telegram-chat','12345','--open-telegram-topic','2147483648'],
                  ['--open-telegram-chat','12345','--open-profile','missing'] ]:
        result=subprocess.run([exe,'--demo','-d',str(config),*args],env={**os.environ,**env},capture_output=True,text=True,timeout=8)
        assert result.returncode!=0, args
        assert 'No other conversation was opened.' in result.stderr or '--open-' in result.stderr, (args,result.stderr)
print('PASS exact initial chat/topic, profile refusal, one backend, independent windows/drafts, visible unavailable route and manual recovery')
