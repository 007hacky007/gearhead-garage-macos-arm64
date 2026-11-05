#!/usr/bin/env python3
"""Development driver: run the game with a control FIFO and a script of input commands.
Usage: drive.py SCRIPT [seconds]. Script lines: 'wait S', or any control command
(mouse X Y BUTTON DOWN WHEEL, key SCAN VK DOWN, text CHAR, capture, click X Y, type TEXT)."""
import os, subprocess, sys, time, tempfile
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
script = open(sys.argv[1]).read().splitlines()
limit = sys.argv[2] if len(sys.argv) > 2 else '60'
fifo = tempfile.mktemp(prefix='ghg-control-')
os.mkfifo(fifo)
env = dict(os.environ, GEARHEAD_CONTROL=fifo, GEARHEAD_TIMEOUT=limit, GEARHEAD_FULLSCREEN='0',
           GEARHEAD_SAVE_DIR=os.path.join(root, 'out/saves'))
os.makedirs(os.path.join(root, 'out/saves'), exist_ok=True)
log = open(os.path.join(root, 'out/run.log'), 'w')
p = subprocess.Popen([os.path.join(root, 'out/gearhead'), 'out/linked.exe', 'local/game'], cwd=root, env=env, stdout=log, stderr=log)
KEYS = {ch: (ord(ch.upper()), sc) for ch, sc in zip('qwertyuiopasdfghjklzxcvbnm', [16,17,18,19,20,21,22,23,24,25,30,31,32,33,34,35,36,37,38,44,45,46,47,48,49,50])}
KEYS[' '] = (32, 57)
f = open(fifo, 'w')
def send(s):
    f.write(s + '\n'); f.flush()
try:
    for line in script:
        line = line.strip()
        if not line or line.startswith('#'): continue
        if p.poll() is not None: break
        w = line.split()
        if w[0] == 'wait': time.sleep(float(w[1]))
        elif w[0] == 'click':
            send(f'mouse {w[1]} {w[2]} -1 0 0'); time.sleep(0.15)
            send(f'mouse {w[1]} {w[2]} 0 1 0'); time.sleep(0.15)
            send(f'mouse {w[1]} {w[2]} 0 0 0'); time.sleep(0.3)
        elif w[0] == 'type':
            for ch in line[5:]:
                vk, scan = KEYS.get(ch.lower(), (0, 0))
                if vk: send(f'key {scan} {vk} 1')
                send(f'text {ord(ch)}'); time.sleep(0.05)
                if vk: send(f'key {scan} {vk} 0')
                time.sleep(0.05)
        elif w[0] == 'enter':
            send('key 28 13 1'); time.sleep(0.1); send('key 28 13 0')
        else: send(line)
    p.wait()
finally:
    if p.poll() is None: p.terminate()
    os.unlink(fifo)
print('exit', p.returncode)
