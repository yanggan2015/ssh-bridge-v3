# ssh_test_server.py — S2 自动化用的最小 SSH 服务器（paramiko）。
#
# 只做三件事：接受密码认证（或一律拒绝）、开 pty + shell、把收到的字节原样回显。
# 生命周期由 tests\step2_check.ps1 控制：起来 → 跑自检 → 杀掉（模拟拔网）→ 再起来。
#
# 用法：
#   python ssh_test_server.py --port 0 --mode echo --ready-file <path>
#   python ssh_test_server.py --port 22022 --mode reject --ready-file <path>
#
# 依赖：paramiko（已在本机 5.1.0 验证）。缺依赖时打印提示并以 2 退出。

import argparse
import os
import socket
import sys
import threading
import time

try:
    import paramiko
except ImportError:  # pragma: no cover
    sys.stderr.write("paramiko missing: pip install paramiko\n")
    sys.exit(2)

BANNER = b"SB3-TEST-READY\r\n"


class Server(paramiko.ServerInterface):
    def __init__(self, mode, shell_event):
        self.mode = mode
        self.shell_event = shell_event

    def check_auth_password(self, username, password):
        if self.mode == "reject":
            return paramiko.AUTH_FAILED
        return paramiko.AUTH_SUCCESSFUL

    def check_auth_none(self, username):
        return paramiko.AUTH_FAILED

    def get_allowed_auths(self, username):
        return "password"

    def check_channel_request(self, kind, chanid):
        if kind == "session":
            return paramiko.OPEN_SUCCEEDED
        return paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_pty_request(self, channel, term, width, height, pixelwidth,
                                  pixelheight, modes):
        return True

    def check_channel_shell_request(self, channel):
        self.shell_event.set()
        return True


def serve_connection(sock, addr, host_key, mode, banner_extra=b"", flood_lines=0):
    transport = None
    try:
        transport = paramiko.Transport(sock)
        transport.add_server_key(host_key)
        shell_event = threading.Event()
        transport.start_server(server=Server(mode, shell_event))
        if mode == "reject":
            time.sleep(1.0)
            return
        chan = transport.accept(20)
        if chan is None:
            return
        if not shell_event.wait(10):
            return
        chan.sendall(BANNER)
        if banner_extra:
            chan.sendall(banner_extra)
        for i in range(flood_lines):
            chan.sendall(("L%06d\r\n" % i).encode("ascii"))
        while True:
            data = chan.recv(4096)
            if not data:
                break
            chan.sendall(data)          # 原样回显，模拟设备侧回显
    except Exception as exc:  # noqa: BLE001
        sys.stderr.write("conn %s: %s\n" % (addr, exc))
    finally:
        try:
            if transport is not None:
                transport.close()
        except Exception:  # noqa: BLE001
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--mode", choices=["echo", "reject"], default="echo")
    ap.add_argument("--ready-file", default="")
    ap.add_argument("--banner-file", default="", help="SSH 建链后额外下发的内容（UTF-8，可含 ANSI）")
    ap.add_argument("--flood-lines", type=int, default=0, help="建链后连发多少行 L000000 形式的数据")
    args = ap.parse_args()

    banner_extra = b""
    if args.banner_file:
        with open(args.banner_file, "rb") as fh:
            banner_extra = fh.read()
    host_key = paramiko.RSAKey.generate(2048)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(16)
    port = srv.getsockname()[1]
    if args.ready_file:
        with open(args.ready_file, "w", encoding="utf-8") as fh:
            fh.write(str(port))
    sys.stdout.write("READY %d %s\n" % (port, args.mode))
    sys.stdout.flush()

    while True:
        try:
            sock, addr = srv.accept()
        except OSError:
            break
        threading.Thread(target=serve_connection,
                         args=(sock, addr, host_key, args.mode, banner_extra, args.flood_lines),
                         daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
