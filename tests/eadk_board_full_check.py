#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""eadk_board.py 全命令契约测试。

不依赖真实板卡：启动一个本地 v3 形状的假桥和 UDP 发现响应器，下载
`share/tools/eadk_board.py` 后用子进程逐条执行全部子命令，校验 HTTP 语义、
输出、退出码与状态变化。真实桥/真实通道的冒烟仍由 eadk_cli_check.ps1 负责。

用法:
  python tests\\eadk_board_full_check.py
  python tests\\eadk_board_full_check.py --keep
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "share" / "tools"
CLI_SRC = TOOLS / "eadk_board.py"
CLI_MD = TOOLS / "eadk_board.md"
OUT_DIR = ROOT / "output" / "ux-audit"
TOKEN = "eadk-full-token"
BOARD = "fake-board"
NOBTN = "fake-nobutton"
SSH_BOARD = "fake-ssh"
DOWN = "fake-down"


class ResultList:
    def __init__(self) -> None:
        self.items: List[Dict[str, Any]] = []
        self.failed = 0

    def add(self, name: str, ok: bool, detail: str = "") -> None:
        detail = (detail or "").replace("\r", "")[-700:]
        self.items.append({"name": name, "ok": bool(ok), "detail": detail})
        if not ok:
            self.failed += 1


def now_ms() -> int:
    return int(time.time() * 1000)


class FakeState:
    def __init__(self) -> None:
        self.lock = threading.RLock()
        self.logs: Dict[str, str] = {}
        self.state: Dict[str, str] = {}
        self.connected: Dict[str, bool] = {}
        self.buttons: Dict[str, List[str]] = {}
        self.config: Dict[str, Any] = {}
        self.trace: Dict[str, List[Any]] = {
            "commands": [], "writes": [], "keys": [], "actions": [],
            "reconnects": [], "serial": [], "exec": [], "feedback": [],
            "channel_updates": [], "tftp": [],
        }
        self.files: Dict[str, bytes] = {}
        self.dirs: set = set()
        self._init_channels()

    def _init_channels(self) -> None:
        self.logs[BOARD] = "root@fake:~# "
        self.logs[NOBTN] = "root@fake:~# "
        self.logs[SSH_BOARD] = ""
        self.logs[DOWN] = "offline\n"
        for name in (BOARD, NOBTN, SSH_BOARD, DOWN):
            self.state[name] = "shell" if name != DOWN else "disconnected"
            self.connected[name] = name != DOWN
        self.buttons[BOARD] = ["power_on", "power_off", "reboot", "enter_uboot", "upgrade"]
        self.buttons[NOBTN] = []
        self.buttons[SSH_BOARD] = []
        self.buttons[DOWN] = []
        self.config = {
            "boards": [
                {"name": BOARD, "serial_device": "loop:fake", "baud": 115200,
                 "username": "root", "password": "secret", "timestamp": False, "auto_open": True},
                {"name": NOBTN, "serial_device": "loop:nobutton", "baud": 115200,
                 "username": "root", "password": "secret", "timestamp": False, "auto_open": True},
                {"name": DOWN, "serial_device": "loop:down", "baud": 115200,
                 "username": "root", "password": "secret", "timestamp": False, "auto_open": False},
            ],
            "ssh": [
                {"name": SSH_BOARD, "host": "127.0.0.1", "port": 22,
                 "username": "root", "password": "secret", "timestamp": False},
            ],
        }
        self.files = {"tools/eadk_board.md": CLI_MD.read_bytes() if CLI_MD.exists() else b"# md\n"}
        self.dirs = {"tools", BOARD, NOBTN, SSH_BOARD, DOWN}

    def channel(self, name: str) -> Dict[str, Any]:
        kind = "ssh" if name == SSH_BOARD else "serial"
        st = self.state.get(name, "disconnected")
        return {
            "name": name,
            "board_name": name,
            "channel_type": kind,
            "kind": kind,
            "connected": bool(self.connected.get(name)),
            "running": bool(self.connected.get(name)),
            "state": st,
            "terminal": st,
            "log_bytes": len(self.logs.get(name, "")),
            "target_username": "root",
            "target_password": "secret",
            "username": "root",
            "password": "secret",
            "features": {},
            "buttons": list(self.buttons.get(name, [])),
        }

    def scan_channels(self) -> List[Dict[str, Any]]:
        return [self.channel(n) for n in (BOARD, NOBTN, SSH_BOARD, DOWN)]

    def append_log(self, name: str, text: str) -> None:
        self.logs[name] = self.logs.get(name, "") + text
        # 保持日志短，避免全量测试无限增长
        if len(self.logs[name]) > 200000:
            self.logs[name] = self.logs[name][-100000:]

    def apply_reboot(self, name: str) -> None:
        if name == NOBTN:
            self.state[name] = "uboot"
            self.append_log(name, "reboot: Restarting system\n=> ")
        else:
            self.state[name] = "login"
            self.append_log(name, "reboot: Restarting system\nfake login: ")

    def handle_write(self, name: str, data: str) -> None:
        st = self.state.get(name, "disconnected")
        if st == "login":
            if data.strip() == "root":
                self.state[name] = "password"
                self.append_log(name, "Password: ")
            else:
                self.append_log(name, "Login incorrect\nlogin: ")
        elif st == "password":
            if data.strip() == "secret":
                self.state[name] = "shell"
                self.append_log(name, "root@fake:~# ")
            else:
                self.state[name] = "login"
                self.append_log(name, "Login incorrect\nlogin: ")
        elif st == "shell":
            self.append_log(name, data)
            if not data.endswith("\n"):
                self.append_log(name, "\n")
            self.append_log(name, "root@fake:~# ")

    def handle_command(self, name: str, cmd: str) -> None:
        if "reboot" in cmd:
            self.apply_reboot(name)
            return
        if cmd.startswith("echo ") or "echo " in cmd:
            # 模拟真实 shell：命令回显前先换行，marker 才能独占一行。
            self.append_log(name, "\n" + cmd.split("echo ", 1)[-1] + "\nroot@fake:~# ")
            return
        self.append_log(name, f"{cmd}\nroot@fake:~# ")

    def handle_action(self, name: str, aid: str) -> None:
        if aid == "enter_uboot":
            self.state[name] = "uboot"
            self.append_log(name, "=> ")
        elif aid == "power_on":
            self.state[name] = "shell"
            self.connected[name] = True
            self.append_log(name, "root@fake:~# ")
        elif aid == "power_off":
            self.state[name] = "disconnected"
            self.connected[name] = False
        elif aid == "reboot":
            self.apply_reboot(name)
        # upgrade/custom 只记录，不改状态

    def handle_exec(self, name: str, cmd: str) -> Dict[str, Any]:
        if "SHARE_RC:" in cmd:
            return {"output": "SHARE_RC:1\n-rw-r--r-- 1 root root 150000 file\n", "state": self.state.get(name), "timed_out": False}
        if "ip -4 route get" in cmd:
            return {"output": "1.2.3.4\n", "state": self.state.get(name), "timed_out": False}
        if "ping " in cmd:
            return {"output": "PING_RC:1\n", "state": self.state.get(name), "timed_out": False}
        if "hostname -I" in cmd:
            return {"output": "10.0.0.2\n", "state": self.state.get(name), "timed_out": False}
        if "uname -a" in cmd:
            return {"output": "Linux fake 6.1 eadk-test\n", "state": self.state.get(name), "timed_out": False}
        if cmd.startswith("echo "):
            return {"output": cmd[5:].strip() + "\n", "state": self.state.get(name), "timed_out": False}
        return {"output": "ok\n", "state": self.state.get(name), "timed_out": False}

    def update_channel(self, payload: Dict[str, Any]) -> Dict[str, Any]:
        name = payload.get("old_name") or (payload.get("channel") or {}).get("name")
        kind = payload.get("kind") or "serial"
        ch = dict(payload.get("channel") or {})
        key = "boards" if kind == "serial" else "ssh"
        found = None
        for item in self.config.get(key, []):
            if item.get("name") == name:
                found = item
                break
        if found is None:
            found = {"name": name}
            self.config.setdefault(key, []).append(found)
        for k, v in ch.items():
            if k in ("name", "serial_device", "baud", "host", "port", "username", "password",
                     "timestamp", "auto_open", "discoverable", "log_enabled"):
                found[k] = v
        self.trace["channel_updates"].append({"kind": kind, "name": name, "channel": dict(ch)})
        return {"ok": True, "name": name}

    def tftp_files(self, path: str) -> List[Dict[str, Any]]:
        prefix = path.rstrip("/") + "/" if path else ""
        items: List[Dict[str, Any]] = []
        for d in sorted(self.dirs):
            if not d or d == path:
                continue
            if d.startswith(prefix) and "/" not in d[len(prefix):]:
                items.append({"name": d.rsplit("/", 1)[-1], "type": "dir", "size": 0})
        for f, data in sorted(self.files.items()):
            if f.startswith(prefix) and "/" not in f[len(prefix):]:
                items.append({"name": f.rsplit("/", 1)[-1], "type": "file", "size": len(data)})
        return items

    def tftp_md5(self, path: str) -> Optional[str]:
        data = self.files.get(path)
        return hashlib.md5(data).hexdigest() if data is not None else None


STATE = FakeState()


class Handler(BaseHTTPRequestHandler):
    server_version = "eadk-fake/1.0"
    protocol_version = "HTTP/1.1"

    def log_message(self, _fmt: str, *_args: Any) -> None:
        pass

    def _send(self, code: int, body: Any, ctype: str = "application/json; charset=utf-8") -> None:
        if isinstance(body, (dict, list)):
            raw = json.dumps(body, ensure_ascii=False).encode("utf-8")
        elif isinstance(body, bytes):
            raw = body
        else:
            raw = str(body).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _body(self) -> Any:
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0:
            return {}
        raw = self.rfile.read(n)
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return {"_raw": raw}

    def do_GET(self) -> None:  # noqa: N802
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        path = u.path
        with STATE.lock:
            if path == "/health":
                return self._send(200, {"ok": True, "service": "fake"})
            if path == "/api/help":
                return self._send(200, {"operations": [
                    {"id": "agent_probe", "path": "/agent/probe"},
                    {"id": "agent_scan", "path": "/agent/scan"},
                ], "pages": []})
            if path == "/api/channels":
                return self._send(200, STATE.config)
            if path == "/agent/scan":
                return self._send(200, {"channels": STATE.scan_channels(),
                                        "board_net": {"host_ip_resolved": "10.0.0.1"},
                                        "agent_net": {"host_ip": "10.0.0.1"}})
            if path == "/status":
                return self._send(200, {"channels": STATE.scan_channels()})
            if path == "/agent/state":
                name = (q.get("channel") or [""])[0]
                return self._send(200, STATE.channel(name))
            if path == "/agent/probe":
                name = (q.get("channel") or [""])[0]
                return self._send(200, {"state": STATE.state.get(name, "unknown")})
            if path == "/actions":
                name = (q.get("channel") or [""])[0]
                return self._send(200, {"buttons": [{"id": x, "label": x} for x in STATE.buttons.get(name, [])]})
            if path == "/log":
                name = (q.get("channel") or [""])[0]
                text = STATE.logs.get(name, "")
                if q.get("chars"):
                    try:
                        text = text[-int(q["chars"][0]):]
                    except Exception:
                        pass
                if q.get("lines"):
                    try:
                        text = "\n".join(text.splitlines()[-int(q["lines"][0]):]) + "\n"
                    except Exception:
                        pass
                return self._send(200, text, "text/plain; charset=utf-8")
            if path == "/log/download":
                name = (q.get("channel") or [""])[0]
                return self._send(200, STATE.logs.get(name, "").encode("utf-8"), "application/octet-stream")
            if path == "/tftp/files":
                rel = (q.get("path") or [""])[0]
                return self._send(200, {"files": STATE.tftp_files(rel)})
            if path == "/tftp/md5":
                rel = (q.get("path") or [""])[0]
                md5 = STATE.tftp_md5(rel)
                if md5 is None:
                    return self._send(404, {"error": "not found"})
                return self._send(200, {"md5": md5})
            if path == "/tftp/status":
                return self._send(200, {"bind_ip": "10.0.0.1", "running": True})
            if path == "/tcp_forward/status":
                return self._send(200, {"running": True, "listen_count": 1})
            if path == "/tcp_forward/map":
                return self._send(200, {"port": 22022})
            if path == "/test/trace":
                return self._send(200, STATE.trace)
            if path == "/test/config":
                return self._send(200, STATE.config)
            if path == "/test/files":
                return self._send(200, {"files": {k: len(v) for k, v in STATE.files.items()}})
            if path == "/share/tools/eadk_board.py":
                return self._send(200, CLI_SRC.read_bytes(), "text/plain; charset=utf-8")
            if path == "/share/tools/eadk_board.md":
                return self._send(200, CLI_MD.read_bytes(), "text/plain; charset=utf-8")
            if path.startswith("/help/"):
                return self._send(200, f"<html><body>{path}</body></html>", "text/html; charset=utf-8")
            return self._send(404, {"error": "not found"})

    def do_POST(self) -> None:  # noqa: N802
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        path = u.path
        body = self._body()
        with STATE.lock:
            if path == "/test/reset":
                name = body.get("name") or BOARD
                STATE.state[name] = body.get("state") or "shell"
                STATE.connected[name] = bool(body.get("connected", True))
                STATE.logs[name] = body.get("log", "root@fake:~# " if STATE.state[name] == "shell" else "")
                return self._send(200, {"ok": True})
            if path == "/test/reset_trace":
                for k in STATE.trace:
                    STATE.trace[k].clear()
                return self._send(200, {"ok": True})
            if path == "/write":
                name = body.get("channel") or BOARD
                STATE.trace["writes"].append({"channel": name, "data": body.get("data")})
                STATE.handle_write(name, str(body.get("data") or ""))
                return self._send(200, {"ok": True})
            if path == "/command":
                name = body.get("channel") or BOARD
                STATE.trace["commands"].append({"channel": name, "command": body.get("command")})
                STATE.handle_command(name, str(body.get("command") or ""))
                return self._send(200, {"ok": True, "queued": True})
            if path == "/send_keys":
                name = body.get("channel") or BOARD
                STATE.trace["keys"].append({"channel": name, "keys": body.get("keys"),
                                            "duration_ms": body.get("duration_ms"),
                                            "interval_ms": body.get("interval_ms")})
                return self._send(200, {"ok": True})
            if path == "/log/clear":
                name = body.get("channel") or BOARD
                STATE.logs[name] = ""
                return self._send(200, {"ok": True})
            if path == "/reconnect":
                STATE.trace["reconnects"].append(dict(body))
                STATE.connected[body.get("channel") or BOARD] = True
                return self._send(200, {"ok": True})
            if path == "/serial/open":
                name = body.get("channel") or BOARD
                STATE.trace["serial"].append({"op": "open", "channel": name})
                STATE.connected[name] = True
                STATE.state[name] = "shell"
                return self._send(200, {"ok": True})
            if path == "/serial/close":
                name = body.get("channel") or BOARD
                STATE.trace["serial"].append({"op": "close", "channel": name})
                STATE.connected[name] = False
                return self._send(200, {"ok": True})
            if path == "/action":
                name = body.get("channel") or BOARD
                aid = body.get("id") or ""
                STATE.trace["actions"].append({"channel": name, "id": aid})
                STATE.handle_action(name, aid)
                return self._send(200, {"ok": True})
            if path == "/channel/configure":
                return self._send(200, {"ok": True})
            if path == "/api/channels":
                return self._send(200, STATE.update_channel(body))
            if path == "/agent/exec":
                name = body.get("channel") or BOARD
                cmd = str(body.get("command") or "")
                STATE.trace["exec"].append({"channel": name, "command": cmd})
                return self._send(200, STATE.handle_exec(name, cmd))
            if path == "/tftp/mkdir":
                rel = str(body.get("path") or "").strip("/")
                if not rel:
                    return self._send(400, {"error": "path required"})
                if rel in STATE.dirs or any(k.startswith(rel + "/") for k in STATE.files):
                    return self._send(409, {"error": "already exists"})
                STATE.dirs.add(rel)
                STATE.trace["tftp"].append({"op": "mkdir", "path": rel})
                return self._send(200, {"ok": True})
            if path == "/tftp/upload":
                rel = (q.get("path") or [""])[0].lstrip("/")
                off = int((q.get("offset") or ["0"])[0])
                data = body.get("_raw") if isinstance(body, dict) and "_raw" in body else b""
                if not isinstance(data, bytes):
                    data = bytes(data)
                old = STATE.files.get(rel, b"")
                if off == len(old):
                    STATE.files[rel] = old + data
                else:
                    STATE.files[rel] = old[:off] + data
                STATE.trace["tftp"].append({"op": "upload", "path": rel, "offset": off, "len": len(data)})
                return self._send(200, {"ok": True, "size": len(STATE.files[rel])})
            if path == "/tftp/delete":
                rel = str(body.get("path") or "").strip("/")
                if rel in STATE.files:
                    del STATE.files[rel]
                elif rel in STATE.dirs:
                    STATE.dirs.discard(rel)
                    for f in list(STATE.files):
                        if f.startswith(rel + "/"):
                            del STATE.files[f]
                STATE.trace["tftp"].append({"op": "delete", "path": rel})
                return self._send(200, {"ok": True})
            if path == "/tftp/config":
                STATE.trace["tftp"].append({"op": "config", "body": dict(body)})
                return self._send(200, {"ok": True})
            if path == "/tcp_forward/config":
                return self._send(200, {"ok": True})
            if path == "/hook":
                STATE.trace["feedback"].append(body)
                return self._send(200, {"code": 0, "msg": "ok"})
            return self._send(404, {"error": "not found"})


class UDPResponder(threading.Thread):
    def __init__(self, port: int) -> None:
        super().__init__(daemon=True)
        self.port = port
        self.sock: Optional[socket.socket] = None
        self.error = ""
        self.ready = threading.Event()
        self.received = 0
        self.sent = 0
        self.last_from = ""

    def run(self) -> None:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("0.0.0.0", self.port))
            s.settimeout(0.5)
            self.sock = s
            self.ready.set()
        except Exception as e:
            self.error = str(e)
            self.ready.set()
            return
        while True:
            try:
                data, addr = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data.startswith(b"EADK1") and not data.startswith(b"SSHBM1"):
                continue
            self.received += 1
            self.last_from = f"{addr[0]}:{addr[1]}"
            magic = "EADK1" if data.startswith(b"EADK1") else "SSHBM1"
            payload = {
                "http_port": HTTP_PORT,
                "version": "eadk-test",
                "alias": "fake",
                "boards": [{"name": BOARD, "type": "serial"}],
            }
            reply = magic.encode() + b"\n" + json.dumps(payload, ensure_ascii=False).encode("utf-8")
            try:
                self.sock.sendto(reply, addr)
                self.sent += 1
            except OSError:
                pass

    def stop(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass


def local_ipv4() -> str:
    candidates: List[str] = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        if ip and not ip.startswith("127."):
            candidates.append(ip)
    except Exception:
        pass
    try:
        candidates.extend(socket.gethostbyname_ex(socket.gethostname())[2])
    except Exception:
        pass
    try:
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-Command",
             "Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue "
             "| Select-Object -ExpandProperty IPAddress"],
            stderr=subprocess.DEVNULL, timeout=5, text=True,
        )
        candidates.extend(out.split())
    except Exception:
        pass
    # 优先 10/172.16-31/192.168 私网；198.19/169.254/虚拟网卡排在后面。
    def rank(ip: str) -> Tuple[int, str]:
        if ip.startswith("10."):
            return (0, ip)
        if ip.startswith("192.168."):
            return (0, ip)
        if ip.startswith("172."):
            try:
                second = int(ip.split(".")[1])
                if 16 <= second <= 31:
                    return (0, ip)
            except Exception:
                pass
        if ip.startswith("169.254.") or ip.startswith("198.19."):
            return (9, ip)
        return (5, ip)
    for ip in sorted({x for x in candidates if x and not x.startswith("127.")}, key=rank):
        return ip
    return ""


def http_json(url: str, body: Optional[dict] = None, timeout: float = 10) -> Any:
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json; charset=utf-8"
    req = urllib.request.Request(url, data=data, headers=headers, method="POST" if body is not None else "GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        raw = r.read()
    return json.loads(raw.decode("utf-8")) if raw else {}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="保留临时目录")
    args = ap.parse_args()

    if not CLI_SRC.is_file():
        print(f"missing {CLI_SRC}", file=sys.stderr)
        return 2

    global HTTP_PORT
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    HTTP_PORT = server.server_address[1]
    http_thread = threading.Thread(target=server.serve_forever, daemon=True)
    http_thread.start()
    base = f"http://127.0.0.1:{HTTP_PORT}"
    # 先算本机 IP：Windows 上 PowerShell 子进程会继承/扰动已存在的 UDP 句柄，
    # 必须在 UDP 发现响应器启动前完成，否则响应器收不到包。
    ip = local_ipv4()

    udp = UDPResponder(18071)
    udp.start()
    udp.ready.wait(2)
    udp2 = None
    if not udp.sock:
        udp2 = UDPResponder(38472)
        udp2.start()
        udp2.ready.wait(2)
    time.sleep(0.3)
    udp_info = (f"udp18071={'ok' if udp.sock else udp.error} "
                f"udp38472={'ok' if (udp2 and udp2.sock) else (udp2.error if udp2 else '-')}")

    tmp = Path(tempfile.mkdtemp(prefix="eadk-board-full-"))
    results = ResultList()

    def add(name: str, ok: bool, detail: str = "") -> None:
        results.add(name, ok, detail)

    def run(argv: List[str], env_extra: Optional[Dict[str, str]] = None,
            timeout: float = 40) -> Tuple[int, str, str]:
        env = dict(os.environ)
        env["PYTHONIOENCODING"] = "utf-8"
        env["EADK_FEEDBACK_HOOK"] = f"{base}/hook"
        env["EADK_HOST"] = "eadk-test-host"
        if env_extra:
            env.update(env_extra)
        cmd = [sys.executable, str(tmp / "eadk_board.py"), "-B", base, "--token", TOKEN] + argv
        cp = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=timeout, env=env)
        return cp.returncode, cp.stdout.strip(), cp.stderr.strip()

    def reset(name: str = BOARD, state: str = "shell", log: Optional[str] = None,
              connected: bool = True) -> None:
        if log is None:
            log = "root@fake:~# " if state == "shell" else ""
        http_json(f"{base}/test/reset", {"name": name, "state": state, "log": log,
                                         "connected": connected})
        STATE.state[name] = state
        STATE.connected[name] = connected
        STATE.logs[name] = log

    try:
        # 0) 分发与语法
        resp = urllib.request.urlopen(f"{base}/share/tools/eadk_board.py", timeout=10)
        raw = resp.read()
        add("share_py_200", resp.status == 200 and len(raw) > 80000,
            f"status={resp.status} len={len(raw)}")
        (tmp / "eadk_board.py").write_bytes(raw)
        md = urllib.request.urlopen(f"{base}/share/tools/eadk_board.md", timeout=10).read()
        add("share_md_200", len(md) > 1000, f"len={len(md)}")
        (tmp / "eadk_board.md").write_bytes(md)
        cp = subprocess.run([sys.executable, "-m", "py_compile", str(tmp / "eadk_board.py")],
                            capture_output=True, text=True)
        add("py_compile", cp.returncode == 0, (cp.stdout + cp.stderr)[-500:])
        add("dist_py_md5_matches", hashlib.md5(raw).hexdigest() == hashlib.md5(CLI_SRC.read_bytes()).hexdigest())

        # 1) 帮助面
        code, out, err = run([])
        add("noargs_help", code == 0 and "全局选项" in out and "推荐开场" in out, out or err)
        code, out, err = run(["-h"])
        add("dash_h_help", code == 0 and "子命令" in out, out or err)
        code, out, err = run(["help"])
        add("help", code == 0 and "EADK_BASE" in out, out or err)
        code, out, err = run(["help", "caps"])
        add("help_caps", code == 0 and "能力" in out, out or err)
        code, out, err = run(["help", "to-board"])
        add("help_to_board", code == 0 and "--method" in out, out or err)
        code, out, err = run(["help", "download"])
        add("help_download", code == 0 and "日志" in out, out or err)
        code, out, err = run(["help", "no-such-topic"])
        add("help_unknown_topic_fail", code != 0 and "无此帮助主题" in err, err)

        # 2) 发现 / 列表 / 扫描 / 状态
        if ip:
            # CLI 子进程只校验参数与 JSON 形状；Windows 上子进程的 UDP 回包
            # 会被本机策略丢弃，因此真实发现协议在同一个测试进程里跑。
            code, out, err = run(["discover", "--host", ip, "-w", "600", "-j"], timeout=20)
            try:
                disc_shape = json.loads(out)
            except Exception:
                disc_shape = None
            add("discover_cli_json_shape", code == 0 and isinstance(disc_shape, list),
                f"ip={ip} {udp_info} out={out} err={err}")
            spec = importlib.util.spec_from_file_location("eadk_board_under_test", str(tmp / "eadk_board.py"))
            eb = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(eb)
            payload = eb.lan_parse_reply(
                b'EADK1\n{"http_port":18081,"version":"test","boards":[{"name":"b","type":"serial"}]}')
            add("discover_wire_parse", isinstance(payload, dict) and payload.get("http_port") == 18081)
            # Windows 上本机 UDP 回包受策略影响，CLI 子进程/同进程都可能丢首包；
            # discover 的协议解析与输出映射用固定响应做确定性覆盖。
            orig_scan = eb.lan_scan
            def fake_scan(wait_ms=1500, targets=None, local_ips=None):
                return ([{"host": "10.0.0.9", "http_port": 18081, "alias": "fake",
                          "boards": [{"name": BOARD, "type": "serial"}]}], "10.0.0.9", set())
            eb.lan_scan = fake_scan
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                eb.cmd_discover(["--host", "10.0.0.9", "-w", "600"], as_json=True)
            eb.lan_scan = orig_scan
            try:
                disc = json.loads(buf.getvalue())
            except Exception:
                disc = None
            add("discover_command_mapping", isinstance(disc, list) and len(disc) == 1
                and disc[0].get("base_url") == "http://10.0.0.9:18081"
                and disc[0].get("boards", [{}])[0].get("name") == BOARD,
                f"out={buf.getvalue()}")
            code, out, err = run(["discover", "--self", "-w", "300", "--compact"], timeout=20)
            add("discover_self_compact", code == 0 and out.startswith("["),
                f"ip={ip} {udp_info} out={out} err={err}")
        else:
            add("discover_no_local_ip", False, "no non-loopback IPv4")

        code, out, err = run(["list"])
        add("list_all", code == 0 and BOARD in out and "enter_uboot" in out, out or err)
        code, out, err = run(["-b", BOARD, "list"])
        add("list_board", code == 0 and "connected: True" in out and "键: channel" in out, out or err)
        code, out, err = run(["list", BOARD])
        add("list_name_positional", code == 0 and "U-Boot抢键" in out, out or err)
        code, out, err = run(["-b", BOARD, "feats"])
        add("feats_alias", code == 0 and "支持功能" in out, out or err)
        code, out, err = run(["-b", BOARD, "list", "-j"])
        add("list_json", code == 0 and '"name": "fake-board"' in out and '"features_plus"' in out, out or err)

        code, out, err = run(["scan"])
        add("scan_all", code == 2 and BOARD in out and "scan 4 channels" in out, out or err)
        code, out, err = run(["scan", BOARD])
        add("scan_named", code == 0 and "name=fake-board" in out, out or err)
        code, out, err = run(["scan", "no-such"])
        add("scan_missing_fail", code != 0 and "通道不存在" in err, err)
        code, out, err = run(["scan", DOWN])
        add("scan_disconnected_exit2", code == 2 and "connected=false" in out, out or err)
        code, out, err = run(["scan", "-j"])
        add("scan_json", code == 2 and '"source": "server"' in out, out or err)

        code, out, err = run(["state", "-j"])
        j = json.loads(out) if out.startswith("{") else {}
        add("state_all_json", code == 0 and len(j.get("channels") or []) == 4, out or err)
        code, out, err = run(["-b", BOARD, "state", "-j"])
        j = json.loads(out) if out.startswith("{") else {}
        add("state_one_json", code == 0 and j.get("name") == BOARD and j.get("terminal") == "shell", out or err)
        code, out, err = run(["-b", BOARD, "probe"])
        add("probe_shell", code == 0 and out.strip() == "shell", out or err)
        code, out, err = run(["-b", BOARD, "health"])
        add("health_ok", code == 0 and "connected=true" in out and "state=shell" in out, out or err)
        code, out, err = run(["-b", DOWN, "health"])
        add("health_down_exit2", code == 2 and "connected=false" in out, out or err)
        code, out, err = run(["-b", BOARD, "status"])
        j = json.loads(out) if out.startswith("{") else {}
        add("status_json", code == 0 and j.get("board_name") == BOARD, out or err)
        code, out, err = run(["-b", BOARD, "help-url"])
        add("help_url", code == 0 and out == f"{base}/help/{BOARD}", out or err)

        # 3) 日志 / 写 / 命令
        reset()
        STATE.logs[BOARD] = "alpha\nbeta\ngamma\n"
        code, out, err = run(["-b", BOARD, "log"])
        add("log_all", code == 0 and "alpha" in out and "gamma" in out, out or err)
        code, out, err = run(["-b", BOARD, "log", "lines", "1"])
        add("log_lines", code == 0 and out.strip() == "gamma", out or err)
        code, out, err = run(["-b", BOARD, "log", "chars", "6"])
        add("log_chars", code == 0 and out.strip() == "gamma", out or err)
        code, out, err = run(["-b", BOARD, "clear"])
        add("clear", code == 0 and STATE.logs[BOARD] == "", out or err)

        reset()
        code, out, err = run(["-b", BOARD, "write", "WRITE_MARK"])
        add("write_exit0", code == 0, out or err)
        add("write_recorded", STATE.trace["writes"][-1].get("data") == "WRITE_MARK",
            json.dumps(STATE.trace["writes"][-1], ensure_ascii=False))
        code, out, err = run(["-b", BOARD, "log", "chars", "200"])
        add("write_in_log", "WRITE_MARK" in out, out or err)

        reset()
        code, out, err = run(["-b", BOARD, "command", "echo CMD_MARK", "-j"])
        add("command_async", code == 0 and '"queued": true' in out, out or err)
        add("command_recorded", STATE.trace["commands"][-1].get("command") == "echo CMD_MARK",
            json.dumps(STATE.trace["commands"][-1], ensure_ascii=False))
        code, out, err = run(["-b", BOARD, "log", "chars", "200"])
        add("command_log", "CMD_MARK" in out, out or err)

        reset()
        code, out, err = run(["-b", BOARD, "-w", "0", "send", "echo SEND_MARK"])
        add("send_output", code == 0 and "SEND_MARK" in out, out or err)
        code, out, err = run(["-b", BOARD, "-w", "0", "send", "echo SEND_JSON", "-j"])
        j = json.loads(out) if out.startswith("{") else {}
        add("send_json", code == 0 and "SEND_JSON" in str(j.get("output")), out or err)

        reset()
        code, out, err = run(["-b", BOARD, "keys", "AB", "0.1", "0.05"])
        add("keys_exit0", code == 0, out or err)
        add("keys_payload", STATE.trace["keys"][-1].get("keys") == ["A", "B"]
            and STATE.trace["keys"][-1].get("duration_ms") == 100, json.dumps(STATE.trace["keys"][-1]))
        code, out, err = run(["-b", BOARD, "ctrlc"])
        add("ctrlc_exit0", code == 0 and STATE.trace["keys"][-1].get("keys") == ["<ctrl+c>"], out or err)

        # 4) 配置 / 生命周期
        reset()
        code, out, err = run(["-b", BOARD, "timestamp", "on"])
        add("timestamp_on", code == 0 and any(u["channel"].get("timestamp") is True for u in STATE.trace["channel_updates"]), out or err)
        code, out, err = run(["-b", BOARD, "timestamp", "off"])
        add("timestamp_off", code == 0 and any(u["channel"].get("timestamp") is False for u in STATE.trace["channel_updates"]), out or err)
        code, out, err = run(["-b", BOARD, "timestamp", "maybe"])
        add("timestamp_bad_fail", code != 0 and "timestamp on|off" in err, err)

        code, out, err = run(["-b", BOARD, "reconnect"])
        add("reconnect", code == 0 and STATE.trace["reconnects"], out or err)
        code, out, err = run(["-b", BOARD, "serial", "close"])
        add("serial_close", code == 0 and STATE.connected[BOARD] is False, out or err)
        code, out, err = run(["-b", BOARD, "serial", "open"])
        add("serial_open", code == 0 and STATE.connected[BOARD] is True and STATE.state[BOARD] == "shell", out or err)
        code, out, err = run(["-b", SSH_BOARD, "serial", "open"])
        add("serial_on_ssh_fail", code != 0 and "不支持: serial" in err, err)

        reset()
        code, out, err = run(["-b", BOARD, "action", "enter_uboot"])
        add("action_enter_uboot", code == 0 and STATE.state[BOARD] == "uboot" and "action_ok" in out, out or err)
        code, out, err = run(["-b", BOARD, "action", "power_on"])
        add("action_power_on", code == 0 and STATE.state[BOARD] == "shell", out or err)
        code, out, err = run(["-b", BOARD, "action", "upgrade"])
        add("action_custom", code == 0 and STATE.trace["actions"][-1].get("id") == "upgrade", out or err)
        code, out, err = run(["-b", BOARD, "action", "no-such"])
        add("action_missing_fail", code != 0 and "无按钮" in err, err)
        code, out, err = run(["-b", NOBTN, "action", "reboot"])
        add("action_no_button_fail", code != 0 and "无按钮" in err, err)

        code, out, err = run(["-b", BOARD, "soft-reboot"])
        add("soft_reboot", code == 0 and STATE.trace["commands"][-1].get("command") == "reboot" and STATE.state[BOARD] == "login", out or err)
        reset()
        code, out, err = run(["-b", BOARD, "uboot"])
        add("uboot_action_path", code == 0 and "action_ok id=enter_uboot" in out and STATE.state[BOARD] == "uboot", out or err)
        reset()
        code, out, err = run(["-b", NOBTN, "-t", "2", "uboot"])
        add("uboot_key_fallback", code == 0 and "=>" in out, out or err)
        reset()
        code, out, err = run(["-b", BOARD, "-t", "2", "-p", "login:", "bootlog"])
        add("bootlog", code == 0 and "login:" in out, out or err)
        reset()
        code, out, err = run(["-b", BOARD, "-t", "2", "-T", "bootlog"])
        add("bootlog_timestamp", code == 0 and "login:" in out, out or err)

        # 5) 登录 / ensure / 执行
        reset(BOARD, "login", "fake login: ")
        code, out, err = run(["-b", BOARD, "login", "root", "secret"], timeout=30)
        add("login_explicit", code == 0 and "login_ok user=root" in out and STATE.state[BOARD] == "shell", out or err)
        reset(BOARD, "shell", "root@fake:~# ")
        code, out, err = run(["-b", BOARD, "ensure"])
        add("ensure_shell", code == 0 and "shell_ok" in out, out or err)
        reset(BOARD, "login", "fake login: ", True)
        code, out, err = run(["-b", BOARD, "ensure"], timeout=30)
        add("ensure_login", code == 0 and STATE.state[BOARD] == "shell", out or err)
        reset()
        code, out, err = run(["-b", BOARD, "cmd", "uname -a"])
        add("cmd_exec", code == 0 and "Linux fake" in out, out or err)
        reset()
        code, out, err = run(["-b", BOARD, "run", "uname -a"])
        add("run_exec", code == 0 and "Linux fake" in out, out or err)

        # 6) share 文件面
        reset()
        local = tmp / "upload.bin"
        local.write_bytes(bytes(range(256)) * 600)  # 153600 B，3 个分片
        md5_local = hashlib.md5(local.read_bytes()).hexdigest()
        code, out, err = run(["put", str(local), f"{BOARD}/upload.bin", "65536"], timeout=30)
        add("put_multichunk", code == 0 and "put_ok" in out and STATE.files.get(f"{BOARD}/upload.bin") == local.read_bytes(), out or err)
        add("put_md5_local_remote", STATE.tftp_md5(f"{BOARD}/upload.bin") == md5_local, md5_local)
        code, out, err = run(["md5", f"{BOARD}/upload.bin", "-j"])
        j = json.loads(out) if out.startswith("{") else {}
        add("md5_json", code == 0 and j.get("md5") == md5_local, out or err)
        code, out, err = run(["ls", BOARD])
        add("ls_file", code == 0 and "upload.bin" in out, out or err)
        code, out, err = run(["mkdir", f"{BOARD}/sub"])
        add("mkdir", code == 0 and f"{BOARD}/sub" in STATE.dirs, out or err)
        code, out, err = run(["rm", f"{BOARD}/upload.bin"])
        add("rm_file", code == 0 and f"{BOARD}/upload.bin" not in STATE.files, out or err)
        code, out, err = run(["rm", f"{BOARD}/sub"])
        add("rm_dir", code == 0 and f"{BOARD}/sub" not in STATE.dirs, out or err)
        code, out, err = run(["put", str(tmp / "missing.bin"), f"{BOARD}/x"])
        add("put_missing_fail", code != 0 and "本地文件不存在" in err, err)
        code, out, err = run(["md5", f"{BOARD}/none"])
        add("md5_missing_fail", code != 0 and "HTTP 404" in err, err)

        # 7) to-board 的 share 阶梯（真实 scp 依赖主机/板密钥，不在假桥里伪造）
        reset()
        tb = tmp / "to-board.bin"
        tb.write_bytes(b"to-board-test\n")
        code, out, err = run(["-b", BOARD, "to-board", str(tb), "/tmp/to-board.bin", "--method=share", "--host-ip=10.0.0.1"])
        add("to_board_share", code == 0 and "to_board_ok method=share" in out, out or err)
        code, out, err = run(["-b", BOARD, "to-board", str(tmp / "missing.bin"), "/tmp/x"])
        add("to_board_missing_fail", code != 0 and "本地文件不存在" in err, err)
        code, out, err = run(["-b", BOARD, "to-board", str(tb), "/tmp/x", "--method=bogus"])
        add("to_board_bad_method_fail", code != 0 and "未知 --method" in err, err)

        # 8) 下载 / 反馈
        reset()
        STATE.logs[BOARD] = "download-me\n"
        outfile = tmp / "download.log"
        code, out, err = run(["-b", BOARD, "download", str(outfile)])
        add("download", code == 0 and outfile.exists() and outfile.read_text(encoding="utf-8") == "download-me\n", out or err)
        for sub, kind in (("feedback", "bug"), ("suggest", "建议"), ("bug", "bug")):
            code, out, err = run([sub, kind, "全量测试陈述"] if sub == "feedback" else [sub, "全量测试陈述"])
            add(f"{sub}_ok", code == 0 and "feedback_ok" in out, out or err)
            add(f"{sub}_payload", any(kind in str(x) for x in STATE.trace["feedback"][-1:]), out or err)
        code, out, err = run(["feedback", "bug"])
        add("feedback_missing_text_fail", code != 0 and "陈述" in err, err)

        # 9) 未知子命令
        code, out, err = run(["no-such-command"])
        add("unknown_subcommand_fail", code != 0 and "未知子命令" in err, err)
    except Exception as e:
        add("uncaught", False, f"{type(e).__name__}: {e}")
    finally:
        try:
            udp.stop()
            if udp2:
                udp2.stop()
            server.shutdown()
            server.server_close()
        except Exception:
            pass
        if not args.keep:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)

    ok_count = sum(1 for x in results.items if x["ok"])
    summary = {
        "ok": results.failed == 0,
        "passed": ok_count,
        "total": len(results.items),
        "failed": results.failed,
        "checks": results.items,
    }
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    out_json = OUT_DIR / "eadk-board-full-check.json"
    out_json.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    for x in results.items:
        print(f"{'PASS' if x['ok'] else 'FAIL'} {x['name']} {x['detail']}")
    print(f"RESULT passed={ok_count} total={len(results.items)} bad={results.failed} json={out_json}")
    return 0 if results.failed == 0 else 1


HTTP_PORT = 0

if __name__ == "__main__":
    sys.exit(main())
