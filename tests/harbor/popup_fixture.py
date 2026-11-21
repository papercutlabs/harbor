"""Observe the native popup and its option before dispatching a fixture click."""
import time

def frame(t):
    for y,line in enumerate(t.screen.display):
        if '╭' not in line or '╮' not in line:continue
        left,right=line.index('╭'),line.index('╮')
        bottom=next((z for z in range(y+1,t.rows) if t.screen.display[z][left:left+1]=='╰'),None)
        if bottom is not None:
            return left,y,right,bottom,t.screen.display[y+1][left+1:right].strip()

def wait(t,predicate,label):
    end=time.monotonic()+3
    while not predicate() and time.monotonic()<end:t.pump(.05)
    assert predicate(),label+'\n'+t.text

def option(t,title,label):
    bounds=frame(t)
    if not bounds or bounds[4]!=title:return
    left,top,right,bottom,_=bounds
    for y in range(top+3,bottom):
        row=t.screen.display[y][left+1:right]
        if row.strip().removeprefix('›').strip()==label:
            return left+1+row.index(label),y

def click(t,title,label):
    wait(t,lambda:option(t,title,label) is not None,f'{title} option {label}')
    x,y=option(t,title,label)
    t.send(f'\x1b[<0;{x+2};{y+1}M',wait=.06)
    t.send(f'\x1b[<0;{x+2};{y+1}m',wait=.1)

def closed(t):
    wait(t,lambda:frame(t) is None,'Popup closed')

def settings(t):
    t.send(b'\x0f');click(t,'Commands','Settings')
    wait(t,lambda:frame(t) is not None and frame(t)[4]=='Settings','Settings rendered')

def choose(t,field,value):
    settings(t);click(t,'Settings',field);click(t,'Appearance',value);closed(t)
