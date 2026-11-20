"""Replace pending IDs without collapsing legitimate repeated messages."""
from native_smoke import Terminal

t=Terminal(extra_env={'HARBOR_DEMO_SEND_ID_REPLACEMENT':'1'})
try:
    t.picker('Studio')
    for count in (1,2):
        t.send('A deliberately repeated message');t.send(b'\r');t.delivered('A deliberately repeated message')
        assert t.text.count('A deliberately repeated message')==count,t.text
    t.picker('Marina');t.picker('Studio')
    assert t.text.count('A deliberately repeated message')==2,t.text
    print('PASS temporary ID replacement; legitimate repeated sends remain distinct')
finally:t.close()
