#!/usr/bin/env python3
"""Step15 Telnet 验收探针：握手、IAC 过滤、NAWS、回显、主动断线。"""
import os
import select
import socket
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 2323
LOG = sys.argv[2] if len(sys.argv) > 2 else "telnet_probe.log"
CTL = sys.argv[3] if len(sys.argv) > 3 else "probe.ctl"
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240


def log(text):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(text + "\n")


log("READY")
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT))
srv.listen(4)
srv.setblocking(False)
conn = None
state = 0
sb_buf = bytearray()
neg = []
msub = bytearray()
nawa = None


def send(data):
    if conn:
        conn.sendall(data)
        log("TX " + data.hex())


def reset_state():
    global state, sb_buf, msub, nawa
    state = 0
    sb_buf = bytearray()
    msub = bytearray()
    nawa = None


def handle_byte(b):
    global state, msub, nawa
    if state == 0:
        if b == IAC:
            state = 1
        elif b in (13, 10):
            pass
        else:
            send(bytes([b]))
    elif state == 1:
        if b == SB:
            state = 2
            msub = bytearray()
        elif b in (DO, DONT, WILL, WONT):
            state = 3
            neg.append(b)
        else:
            state = 0
    elif state == 2:
        if b == IAC:
            state = 4
        else:
            msub.append(b)
    elif state == 4:
        if b == SE:
            log("SB " + msub.hex())
            if len(msub) >= 2 and msub[0] == 31 and len(msub) >= 4:
                w = (msub[1] << 8) | msub[2]
                h = (msub[3] << 8) | (msub[4] if len(msub) > 4 else 0)
                nawa = (w, h)
                log(f"NAWS {w} {h}")
            state = 0
        elif b == IAC:
            msub.append(IAC)
        else:
            msub.append(b)
            state = 2
    elif state == 3:
        cmd = neg.pop() if neg else 0
        log(f"NEG {cmd} {b}")
        if cmd == WILL and b in (1, 3):
            send(bytes([IAC, DO, b]))
        if cmd == DO and b == 31:
            send(bytes([IAC, WILL, 31]))
            send(bytes([IAC, SB, 31, 0, 120, 0, 32, IAC, SE]))
        state = 0


def accept():
    global conn
    try:
        conn, addr = srv.accept()
    except BlockingIOError:
        return
    addr = addr[0] if addr else "?"
    log(f"ACCEPT {addr}")
    conn.setblocking(False)
    reset_state()
    send(b"TELNET READY\r\n")
    send(bytes([IAC, WILL, 1, IAC, WILL, 3]))
    send(bytes([IAC, SB, 99]) + b"foo" + bytes([IAC, SE]))
    send(b"SB-SAFE\r\n")


def read_conn():
    global conn
    try:
        data = conn.recv(4096)
    except BlockingIOError:
        return
    except OSError as e:
        log(f"ERR {e}")
        data = b""
    if not data:
        log("CLOSED")
        try:
            conn.close()
        except OSError:
            pass
        conn = None
        return
    log("RX " + data.hex())
    for b in data:
        handle_byte(b)


try:
    while True:
        if os.path.exists(CTL):
            try:
                cmd = open(CTL, encoding="utf-8").read().strip()
                os.remove(CTL)
            except OSError:
                cmd = ""
            if cmd == "close":
                if conn:
                    log("CTL_CLOSE")
                    try:
                        conn.close()
                    except OSError:
                        pass
                    conn = None
            elif cmd == "stop":
                break
        rlist = [srv] + ([conn] if conn else [])
        try:
            ready, _, _ = select.select(rlist, [], [], 0.2)
        except OSError:
            ready = []
        if srv in ready:
            accept()
        if conn and conn in ready:
            read_conn()
finally:
    if conn:
        conn.close()
    srv.close()
    log("STOP")
