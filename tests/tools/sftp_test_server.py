#!/usr/bin/env python3
"""Minimal password-auth SSH server with a real SFTP subsystem for UI tests."""
import argparse
import os
import socket
import stat
import threading
import time

import paramiko
from paramiko import SFTPAttributes, SFTPHandle, SFTP_OK, SFTP_FAILURE, SFTP_NO_SUCH_FILE
from paramiko import SFTPServer, SFTPServerInterface


def _attrs(path, filename=None):
    a = SFTPAttributes.from_stat(os.stat(path))
    a.filename = filename if filename is not None else os.path.basename(path)
    return a


class LocalHandle(SFTPHandle):
    def stat(self):
        try:
            f = self.readfile or self.writefile
            return SFTPAttributes.from_stat(os.fstat(f.fileno()))
        except OSError:
            return SFTP_FAILURE


class RootedSFTPServer(SFTPServerInterface):
    ROOT = "."

    def _real(self, path):
        rel = self.canonicalize(path).lstrip("/")
        return os.path.join(self.ROOT, rel)

    def list_folder(self, path):
        real = self._real(path)
        try:
            out = []
            for name in sorted(os.listdir(real)):
                full = os.path.join(real, name)
                try:
                    out.append(_attrs(full, name))
                except OSError:
                    pass
            return out
        except FileNotFoundError:
            return SFTP_NO_SUCH_FILE
        except OSError:
            return SFTP_FAILURE

    def stat(self, path):
        try:
            return _attrs(self._real(path))
        except FileNotFoundError:
            return SFTP_NO_SUCH_FILE
        except OSError:
            return SFTP_FAILURE

    def lstat(self, path):
        return self.stat(path)

    def open(self, path, flags, attr):
        real = self._real(path)
        try:
            if flags & os.O_WRONLY:
                if flags & os.O_APPEND:
                    mode = "ab"
                elif flags & os.O_TRUNC:
                    mode = "wb"
                elif os.path.exists(real):
                    mode = "r+b"
                else:
                    mode = "w+b"
            elif flags & os.O_RDWR:
                mode = "r+b" if os.path.exists(real) else "w+b"
            else:
                mode = "rb"
            f = open(real, mode)
        except FileNotFoundError:
            return SFTP_NO_SUCH_FILE
        except OSError:
            return SFTP_FAILURE
        h = LocalHandle(flags)
        h.readfile = f
        h.writefile = f
        return h

    def remove(self, path):
        try:
            os.remove(self._real(path))
            return SFTP_OK
        except FileNotFoundError:
            return SFTP_NO_SUCH_FILE
        except OSError:
            return SFTP_FAILURE

    def rename(self, oldpath, newpath):
        try:
            os.rename(self._real(oldpath), self._real(newpath))
            return SFTP_OK
        except FileNotFoundError:
            return SFTP_NO_SUCH_FILE
        except OSError:
            return SFTP_FAILURE

    def mkdir(self, path, attr):
        try:
            os.mkdir(self._real(path))
            return SFTP_OK
        except FileExistsError:
            return SFTP_FAILURE
        except OSError:
            return SFTP_FAILURE

    def rmdir(self, path):
        try:
            os.rmdir(self._real(path))
            return SFTP_OK
        except FileNotFoundError:
            return SFTP_NO_SUCH_FILE
        except OSError:
            return SFTP_FAILURE

    def chattr(self, path, attr):
        real = self._real(path)
        try:
            if attr.st_mode is not None:
                os.chmod(real, attr.st_mode)
            return SFTP_OK
        except OSError:
            return SFTP_FAILURE

    def canonicalize(self, path):
        if not path:
            return "/"
        parts = []
        for seg in path.replace("\\", "/").split("/"):
            if seg in ("", "."):
                continue
            if seg == "..":
                if parts:
                    parts.pop()
            else:
                parts.append(seg)
        return "/" + "/".join(parts)

    def readlink(self, path):
        try:
            return os.readlink(self._real(path))
        except OSError:
            return SFTP_NO_SUCH_FILE

    def symlink(self, target_path, path):
        return SFTP_FAILURE


class AuthServer(paramiko.ServerInterface):
    def __init__(self, user, password):
        self.user = user
        self.password = password

    def check_auth_password(self, username, password):
        if username == self.user and password == self.password:
            return paramiko.AUTH_SUCCESSFUL
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
        return True


def serve_client(client, host_key, args):
    t = paramiko.Transport(client)
    t.add_server_key(host_key)
    t.set_subsystem_handler("sftp", SFTPServer, RootedSFTPServer)
    try:
        t.start_server(server=AuthServer(args.user, args.password))
        chan = t.accept(20)
        if chan is None:
            t.close()
            return
        while t.is_active():
            time.sleep(0.2)
    except Exception as exc:
        print("client error:", exc, flush=True)
    finally:
        try:
            t.close()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=22023)
    ap.add_argument("--root", required=True)
    ap.add_argument("--user", default="root")
    ap.add_argument("--password", default="test")
    ap.add_argument("--ready-file", default="")
    args = ap.parse_args()
    RootedSFTPServer.ROOT = os.path.abspath(args.root)
    os.makedirs(args.root, exist_ok=True)
    host_key = paramiko.RSAKey.generate(2048)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(20)
    if args.ready_file:
        with open(args.ready_file, "w", encoding="utf-8") as f:
            f.write("ready\n")
    print(f"SFTP test server on 127.0.0.1:{args.port} root={RootedSFTPServer.ROOT}", flush=True)
    while True:
        client, peer = srv.accept()
        print(f"accept {peer}", flush=True)
        threading.Thread(target=serve_client, args=(client, host_key, args), daemon=True).start()


if __name__ == "__main__":
    main()
