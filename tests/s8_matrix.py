import argparse
import json
import socket
import time
import urllib.request

import paramiko

parser = argparse.ArgumentParser()
parser.add_argument('--port', type=int, default=2223)
parser.add_argument('--web', type=int, default=18091)
parser.add_argument('--token', default='s8pw')
parser.add_argument('--config', required=True)
args = parser.parse_args()

rows = []
def check(tag, name, ok, detail=''):
    rows.append((tag, name, bool(ok), detail))
    print(f'{tag:<6} {name:<38} {"PASS" if ok else "FAIL"} {detail}', flush=True)

def tcp_open(port):
    try:
        s = socket.create_connection(('127.0.0.1', port), timeout=2)
        s.close()
        return True
    except OSError:
        return False

def auth_none(user):
    s = socket.create_connection(('127.0.0.1', args.port), timeout=5)
    t = paramiko.Transport(s)
    t.start_client(timeout=5)
    try:
        t.auth_none(user)
        return True
    except Exception:
        return False
    finally:
        t.close()

def auth_password(user, password):
    s = socket.create_connection(('127.0.0.1', args.port), timeout=5)
    t = paramiko.Transport(s)
    t.start_client(timeout=5)
    try:
        t.auth_password(user, password)
        return True
    except Exception:
        return False
    finally:
        t.close()

def ssh_client(user, password=None):
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect('127.0.0.1', args.port, username=user, password=password,
              timeout=8, allow_agent=False, look_for_keys=False)
    return c

def shell_none(user):
    s = socket.create_connection(('127.0.0.1', args.port), timeout=5)
    t = paramiko.Transport(s)
    t.start_client(timeout=5)
    t.auth_none(user)
    ch = t.open_session(timeout=5)
    ch.get_pty(term='xterm')
    ch.invoke_shell()
    return t, ch

def read_for(ch, seconds):
    end = time.time() + seconds
    data = b''
    while time.time() < end:
        while ch.recv_ready():
            data += ch.recv(4096)
        time.sleep(0.05)
    return data

def save_config():
    body = open(args.config, 'rb').read()
    req = urllib.request.Request(
        f'http://127.0.0.1:{args.web}/api/config/save', data=body, method='POST',
        headers={'Authorization': 'Bearer ' + args.token, 'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=8) as r:
        return json.loads(r.read().decode('utf-8'))

def inbound_state():
    req = urllib.request.Request(f'http://127.0.0.1:{args.web}/status',
                                 headers={'Authorization': 'Bearer ' + args.token})
    with urllib.request.urlopen(req, timeout=5) as r:
        j = json.loads(r.read().decode('utf-8'))
    return j.get('services', {}).get('inbound', {})

check('S8M-1', '空密码通道 none 认证', auth_none('nonech'))
check('S8M-2', '默认密码通道拒绝 none', not auth_none('inherit'))
check('S8M-3', '默认密码继承 defpw', auth_password('inherit', 'defpw'))
check('S8M-4', '默认密码拒绝错误密码', not auth_password('inherit', 'badpw'))
check('S8M-5', 'SSH 出站通道用自身密码', auth_password('sshin', 'sshpw'))
check('S8M-6', 'SSH 出站通道拒绝默认密码', not auth_password('sshin', 'defpw'))
check('S8M-7', '未知用户名被拒', not auth_none('ghost'))

t1, ch1 = shell_none('nonech')
t2, ch2 = shell_none('nonech')
time.sleep(0.5)
ch1.send('S8M_A\r')
ch2.send('S8M_B\r')
out1 = read_for(ch1, 2.0)
out2 = read_for(ch2, 2.0)
check('S8M-8', '同通道双会话共享 A', b'S8M_A' in out1, f'bytes={len(out1)}')
check('S8M-9', '同通道双会话共享 B', b'S8M_B' in out2, f'bytes={len(out2)}')
st = inbound_state()
check('S8M-10', '活动会话计数为 2', st.get('sessions') == 2, f'sessions={st.get("sessions")}')

cfg = json.load(open(args.config, encoding='utf-8'))
cfg['inbound_ssh']['enable'] = False
open(args.config, 'w', encoding='utf-8', newline='').write(json.dumps(cfg, ensure_ascii=False, indent=2))
res = save_config()
time.sleep(2.0)
closed = bool(ch1.closed)
if not closed:
    try:
        ch1.send('AFTER_OFF\r')
    except Exception:
        closed = True
    end = time.time() + 2.0
    while time.time() < end and not closed:
        if ch1.recv_ready():
            if ch1.recv(4096) == b'':
                closed = True
        time.sleep(0.05)
st = inbound_state()
check('S8M-11', '关闭配置后端口释放', not tcp_open(args.port), f'port={args.port}')
check('S8M-12', '关闭配置后会话清零', st.get('sessions') == 0 and not st.get('on'),
      f'on={st.get("on")} sessions={st.get("sessions")}')
check('S8M-13', '活动会话收到断开', bool(closed), f'closed={closed}')

cfg['inbound_ssh']['enable'] = True
open(args.config, 'w', encoding='utf-8', newline='').write(json.dumps(cfg, ensure_ascii=False, indent=2))
save_config()
time.sleep(2.0)
check('S8M-14', '重新开启后端口恢复', tcp_open(args.port))
t3, ch3 = shell_none('nonech')
time.sleep(0.4)
ch3.send('S8M_REOPEN\r')
out3 = read_for(ch3, 1.5)
check('S8M-15', '重新开启后仍可回显', b'S8M_REOPEN' in out3, f'bytes={len(out3)}')
ch3.close(); t3.close()
ch1.close(); t1.close(); ch2.close(); t2.close()
time.sleep(1.0)
check('S8M-16', '全部关闭后会话清零', inbound_state().get('sessions') == 0,
      f'sessions={inbound_state().get("sessions")}')

failed = [r for r in rows if not r[2]]
print('RESULT: ALL PASS' if not failed else f'RESULT: FAIL ({len(failed)} 项)')
raise SystemExit(0 if not failed else 1)
