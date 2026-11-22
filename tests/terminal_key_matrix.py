#!/usr/bin/env python3
"""终端控制键/功能键真实矩阵（UI 桥 + 真实 SSH/串口回环）。

用法：
  python tests/terminal_key_matrix.py --base http://127.0.0.1:18096 \
      --loop loop-chars --ssh ssh-demo2 --tag K1
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request


class Client:
    def __init__(self, base, token=""):
        self.base = base.rstrip("/")
        self.token = token

    def _request(self, path, method="GET", payload=None):
        data = None
        headers = {}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        if payload is not None:
            data = json.dumps(payload, ensure_ascii=True).encode("utf-8")
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(self.base + path, data=data, headers=headers,
                                     method=method)
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def state(self):
        return self._request("/state")["state"]

    def action(self, control, action, text=None, value=None):
        payload = {"id": control, "action": action}
        if text is not None:
            payload["text"] = text
        if value is not None:
            payload["value"] = value
        result = self._request("/action", "POST", payload)
        if not result.get("ok"):
            raise RuntimeError("%s %s failed: %s" % (control, action, result))
        return result

    def select(self, channel):
        self.action("tree.channels", "select", text=channel)
        for _ in range(60):
            pane = self.state()["terminal"]["panes"][0]
            if pane.get("channel") == channel:
                return
            time.sleep(0.1)
        raise RuntimeError("channel did not become active: " + channel)

    def channel(self, name):
        for ch in self.state()["channels"]:
            if ch.get("name") == name:
                return ch
        raise RuntimeError("channel missing: " + name)

    def tail(self):
        return "\n".join(self.state()["terminal"]["panes"][0].get("tail", []))

    def send(self, text):
        return self.action("term.send", "set_text", text=text)

    def input(self, text, submit=True):
        return self.action("term.input", "set_text", text=text, value=1 if submit else 0)


failures = []


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + ((" : " + detail) if detail else ""))
    if not ok:
        failures.append(name)


def wait_until(predicate, timeout=2.5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.08)
    return False


def has_exact_line(text, marker):
    return any(line.strip() == marker for line in text.splitlines())


def last_line(client):
    """`/state` 的 tail 是最新行在前（bridge_host 从末尾倒序取 2 行）。"""
    lines = client.tail().splitlines()
    return lines[0] if lines else ""


def wait_prompt(client, timeout=1.5):
    return wait_until(lambda: last_line(client).rstrip().endswith(("#", "$", ">")), timeout)


def settle_prompt(client, timeout=3.0):
    """空回车把上一行强制提交/清空；低波特率板端靠这个拿到干净提示符。"""
    client.input("")
    return wait_prompt(client, timeout)


def loop_control_keys(client, channel):
    client.select(channel)
    cases = [
        ("ctrl-c", "\x03"),
        ("ctrl-a", "\x01"),
        ("tab", "\t"),
        ("esc", "\x1b"),
        ("up", "\x1b[A"),
        ("down", "\x1b[B"),
        ("home", "\x1b[H"),
        ("delete", "\x1b[3~"),
        ("f1", "\x1b[11~"),
        ("ctrl-right", "\x1b[1;5C"),
        ("backspace", "\x7f"),
    ]
    for name, payload in cases:
        before = client.channel(channel)
        sent = client.send(payload)
        n = len(payload.encode("utf-8"))
        ok = wait_until(lambda: client.channel(channel)["tx_bytes"] - before["tx_bytes"] == n,
                        1.2)
        after = client.channel(channel)
        echo_ok = after["rx_bytes"] - before["rx_bytes"] == n
        check("LOOP-KEY-" + name, ok and echo_ok and sent["result"]["sent"] == 1,
              "tx=%d rx=%d expect=%d" % (after["tx_bytes"] - before["tx_bytes"],
                                         after["rx_bytes"] - before["rx_bytes"], n))


def ssh_readline_keys(client, channel, tag):
    client.select(channel)
    for _ in range(3):
        client.send("\x03")
        time.sleep(0.12)
    client.send("\n")
    if not wait_prompt(client, 4.0):
        client.send("\n")
        wait_prompt(client, 4.0)
    time.sleep(0.8)

    # Tab 补全先做：干净提示符下最稳定；部分板端补全不加尾部空格，这里显式补一个
    client.send("ech")
    typed = wait_until(lambda: last_line(client).strip().endswith("ech"), 3.0)
    tab_marker = "TAB_%s_OK" % tag
    if typed:
        client.send("\t")
        expanded = wait_until(lambda: last_line(client).strip().endswith("echo"), 2.5)
    else:
        expanded = False
    check("SSH-KEY-tab-expand", expanded, "last=%r" % (last_line(client)))
    if expanded:
        client.send(" ")
        client.send(tab_marker)
        client.input("")
        ok = wait_until(lambda: has_exact_line(client.tail(), tab_marker), 3.0)
    else:
        ok = False
    check("SSH-KEY-tab-complete", ok, "marker=%s" % tab_marker)
    wait_prompt(client, 4.0)

    # Ctrl+C 必须打断正在运行的 sleep，而不是等它自然结束
    client.input("sleep 5")
    time.sleep(0.25)
    client.send("\x03")
    ctrl_marker = "KEYC_%s_OK" % tag
    client.input("printf '%s\\n'" % ctrl_marker)
    ok = wait_until(lambda: has_exact_line(client.tail(), ctrl_marker), 2.2)
    check("SSH-KEY-ctrl-c", ok, "marker=%s" % ctrl_marker)
    wait_prompt(client, 4.0)

    # 上方向键召回上一条命令；只检查命令行，不执行
    hist_marker = "HIST_%s_ONE" % tag
    client.input("printf '%s\\n'" % hist_marker)
    wait_until(lambda: has_exact_line(client.tail(), hist_marker), 2.5)
    client.send("\x1b[A")
    ok = wait_until(lambda: hist_marker in last_line(client), 1.5)
    check("SSH-KEY-up-history", ok, "last=%r" % (last_line(client)))
    client.send("\x03")
    client.send("\n")
    if not wait_prompt(client, 4.0):
        client.input("")
        wait_prompt(client, 4.0)

    # Backspace：先多打一个 Z，DEL 后补 K；只有退格生效才会输出精确标记行
    back_marker = "BACK_%s_K" % tag
    client.send("printf '%sZ" % back_marker[:-1])
    client.send("\x7f")
    client.send("%s\\n'" % back_marker[-1])
    client.input("")
    ok = wait_until(lambda: has_exact_line(client.tail(), back_marker), 2.5)
    check("SSH-KEY-backspace", ok, "marker=%s" % back_marker)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="http://127.0.0.1:18096")
    ap.add_argument("--token", default="")
    ap.add_argument("--loop", default="loop-chars")
    ap.add_argument("--ssh", default="ssh-demo2")
    ap.add_argument("--tag", default="K")
    ap.add_argument("--skip-loop", action="store_true")
    ap.add_argument("--skip-ssh", action="store_true")
    args = ap.parse_args()

    client = Client(args.base, args.token)
    print("BASE " + args.base)
    if not args.skip_loop:
        loop_control_keys(client, args.loop)
    if not args.skip_ssh:
        ssh_readline_keys(client, args.ssh, args.tag)
    print("RESULT: %s" % ("ALL PASS" if not failures else "FAIL " + ",".join(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (urllib.error.URLError, RuntimeError) as exc:
        print("ERROR: %s" % exc)
        sys.exit(1)
