"""Exact message targets through the shared native control, preserving independent drafts."""
from native_smoke import Terminal
from pathlib import Path
import os, subprocess, tempfile, time
exe = os.environ['HARBOR_EXECUTABLE']
with tempfile.TemporaryDirectory(prefix='duet-links-') as directory:
    config = Path(directory)/'config'
    env = dict(HOME=directory, HARBOR_DEMO_TOPICS='1', HARBOR_DEMO_ROUTES='1', HARBOR_DEMO_LINKS='1', HARBOR_DEMO_ROUTE_CHAT_ID='-10012345')
    first = Terminal(cols=100, config_dir=config, extra_env=dict(env, CMUX_SURFACE_ID='LINK-FIRST'), args=['--open-telegram-chat','-10012345','--open-telegram-topic','421'])
    second = None
    def command(*arguments):
        child = subprocess.Popen([exe,'--demo','-d',str(config),*arguments], env=dict(os.environ, **env), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        end = time.monotonic()+20
        while child.poll() is None and time.monotonic()<end:
            first.pump(.01)
            if second: second.pump(.01)
        if child.poll() is None: child.kill(); raise AssertionError('native command did not finish')
        out, err = child.communicate()
        return child.returncode, out, err
    try:
        first.expect('Later message 130'); first.send('first draft untouched')
        second = Terminal(cols=100, config_dir=config, extra_env=dict(env, CMUX_SURFACE_ID='LINK-SECOND'), args=['--open-telegram-chat','-10012345','--open-telegram-topic','550'])
        second.send('second draft untouched'); before = second.text
        result = command('--inspect-ui','--target-surface','LINK-FIRST')
        assert result[0] == 0, result
        fields = result[1].strip().split('\t')
        assert fields[1] == str(first.pid) and fields[2] == 'LINK-FIRST', fields
        result = command('--target-surface','LINK-FIRST','--target-pid',str(first.pid),'--open-telegram-chat','-10012345','--open-telegram-topic','421','--open-telegram-message','100')
        assert result[0] == 0, result
        first.expect('Topic-specific history'); first.expect('Showing the message'); first.expect('first draft untouched')
        assert second.text == before, 'other window changed'
        assert 'Later message 130' not in first.text, 'message route stayed at latest history'
        third = Terminal(cols=100, config_dir=config, extra_env=env, args=['--open-telegram-chat','-10012345','--open-telegram-topic','421','--open-telegram-message','100'])
        try:
            third.expect('Topic-specific history'); third.expect('Showing the message')
            assert 'Later message 130' not in third.text
        finally: third.close()
        result = command('--resolve-telegram-link','https://t.me/duet_demo/100','--resolve-profile','HarborDemo_local')
        assert result[0] == 0 and 'pcl.harbor.link.v1\t-10012345\t421\t100' in result[1], result
        result = command('--resolve-telegram-username','duet_demo')
        assert result[0] == 0 and 'pcl.harbor.link.v1\t-10012345\t\t' in result[1], result
        result = command('--target-surface','LINK-FIRST','--open-telegram-chat','-10012345','--open-telegram-message','100')
        assert result[0] != 0, 'mutation accepted without exact inspected PID'
        result = command('--target-surface','LINK-FIRST','--target-pid',str(first.pid+1000000),'--open-telegram-chat','-10012345','--open-telegram-message','100')
        assert result[0] != 0, 'mutation accepted stale PID'
        first.expect('first draft untouched'); assert second.text == before
        print('PASS inspected native identity, exact older message, public provider route, chat-only route, both drafts, other window unchanged, no-PID/stale-PID refusal')
    finally:
        if second: second.close()
        first.close()
