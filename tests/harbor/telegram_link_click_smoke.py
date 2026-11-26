"""Native message clicks reach the Duet bridge; declined actions retain the platform opener."""
from native_smoke import Terminal
from pathlib import Path
import json, os, select, sys, tempfile, time

with tempfile.TemporaryDirectory(prefix='duet-link-click-') as directory:
    root=Path(directory)
    events=root/'events'; os.mkfifo(events,0o600); event=os.open(events,os.O_RDWR|os.O_NONBLOCK)
    bridge=root/'bridge'; requests=root/'requests'; platform=root/'platform'; opener=root/'opener'
    bridge.write_text('#!'+sys.executable+'\nimport json,sys\np=json.loads(sys.argv[3])\nwith open('+repr(str(requests))+',"a") as f:f.write(json.dumps([sys.argv[1:3],p])+"\\n")\nprint(json.dumps({"accepted":"/c/" in p["url"]}))\nwith open('+repr(str(events))+',"w") as f:f.write("x")\n')
    bridge.chmod(0o700)
    opener.write_text('#!/bin/sh\nprintf "%s\\n" "$1" >> "'+str(platform)+'"\nprintf x > "'+str(events)+'"\n');opener.chmod(0o700)
    def wait(path, count):
        until=time.monotonic()+5
        while (not path.exists() or len(path.read_text().splitlines())<count) and time.monotonic()<until:
            if select.select([event],[],[],max(0,until-time.monotonic()))[0]:os.read(event,4096)
        assert path.exists() and len(path.read_text().splitlines())>=count,path
    urls=['https://t.me/c/12345/421/100','tg://proxy?server=example.test','https://t.me/share/url?url=https://example.test']
    try:
        for index,url in enumerate(urls):
            t=Terminal(cols=110,config_dir=root/str(index),extra_env={'HOME':str(root),'HARBOR_DEMO_TOPICS':'1','HARBOR_DEMO_ROUTES':'1','HARBOR_DEMO_LINKS':'1','HARBOR_DEMO_LINK_URL':url,'DUET_HARBOR_NATIVE_LINKS':'1','CMUX_BUNDLED_CLI_PATH':str(bridge),'CMUX_SURFACE_ID':'CLICK-SOURCE','HARBOR_TEST_OPENER':str(opener)},args=['--open-telegram-chat','12345','--open-telegram-topic','550'])
            try:
                t.expect('linked Desk');t.click_text('linked Desk');wait(requests,index+1)
                rows=[json.loads(line) for line in requests.read_text().splitlines()]
                assert rows[-1]==[['rpc','duet.open-link'],{'url':url,'surface_id':'CLICK-SOURCE'}],rows
                if index:wait(platform,index)
                else:assert not platform.exists(),'accepted route also opened externally'
            finally:t.close()
        assert platform.read_text().splitlines()==urls[1:]
    finally:os.close(event)
print('PASS ordinary message-link click bridge, source identity, accepted route once, proxy and share actions use their existing platform opener')
