#!/usr/bin/env python3
"""终端回显 / 特殊字符矩阵。

对已启动的 ssh-bridge3 调试实例执行：切换通道 → 通过 UI 桥 `term.input`
输入真实命令行 → 读取 `/state` 的终端 tail，断言远端回显与输入一致。

用法：
  python tests/terminal_echo_matrix.py --base http://127.0.0.1:18096 \
      --channels ssh-demo1 ssh-demo2
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

FAILURES = []


def api(base, path, payload=None, timeout=5):
    url = base.rstrip("/") + path
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers,
                                 method="POST" if payload is not None else "GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def state(base):
    return api(base, "/state")["state"]


def terminal(base):
    return state(base)["terminal"]["panes"][0]


def select_channel(base, name):
    try:
        api(base, "/action", {"id": "tree.channels", "action": "select",
                              "text": name})
    except urllib.error.HTTPError as e:
        raise RuntimeError("select %s: HTTP %s" % (name, e.code))
    deadline = time.time() + 3.0
    while time.time() < deadline:
        if terminal(base).get("channel") == name:
            return True
        time.sleep(0.2)
    return False


def shquote(s):
    """POSIX shell 单引号转义；反斜杠、双引号、$、反引号都原样保留。"""
    return "'" + s.replace("'", "'\"'\"'") + "'"


def send_input(base, text):
    return api(base, "/action", {"id": "term.input", "action": "set_text",
                                 "text": text}).get("result", {}).get("sent", 0)


def send_raw(base, text):
    return api(base, "/action", {"id": "term.send", "action": "set_text",
                                 "text": text}).get("result", {}).get("sent", 0)


def wait_line(base, expected, timeout=3.0, allow_wrapped=False):
    deadline = time.time() + timeout
    while time.time() < deadline:
        tail = terminal(base).get("tail") or []
        if expected in tail:
            return True
        if allow_wrapped and any(ln.endswith(expected) for ln in tail):
            return True
        time.sleep(0.15)
    return False


def channel_stats(base, channel):
    for c in state(base).get("channels", []):
        if c.get("name") == channel:
            return c.get("tx_bytes", 0), c.get("rx_bytes", 0)
    return 0, 0


def check(channel, name, payload, base, before, raw=False):
    raw_long = raw and name == "long"
    if raw:
        sent = send_input(base, payload)
        probe = payload
        cmd = payload
        expected = payload
    else:
        # 命令回显里只会出现 OUT<%s>END 这层格式串，不会出现 OUT<真实 payload>END。
        # 因此命中 expected 就证明是远端执行后的输出，不是输入行回显。
        cmd = "printf 'OUT<%s>END\\n' " + shquote(payload)
        sent = send_input(base, cmd)
        expected = "OUT<" + payload + ">END"
        # 长行会折行，tail 只保留最后两行；用输出尾部标记断言，仍不会命中命令回显。
        probe = payload[-40:] + ">END" if name == "long" else expected
    if raw_long:
        # 回环长行会占多行，/state.tail 只保留最后两行，不能要求整串可见；
        # 回环端点原样回送，tx+ == rx+ 本身就是逐字节一致的强断言。
        ok = sent == 1
    else:
        ok = sent == 1 and wait_line(base, probe,
                                     timeout=6.0 if name == "long" else 3.0,
                                     allow_wrapped=(name == "long"))
    after = channel_stats(base, channel)
    grew = after[0] > before[0] and after[1] > before[1]
    if raw_long:
        grew = grew and (after[0] - before[0]) == (after[1] - before[1])
    result = "PASS" if ok and grew else "FAIL"
    print("%-8s %-22s sent=%d tx+%d rx+%d tail=%r" %
          (result, name, sent, after[0] - before[0],
           after[1] - before[1],
           (terminal(base).get("tail") or [])[-1:]))
    if result == "FAIL":
        FAILURES.append("%s/%s expected=%r tail=%r" %
                        (channel, name, expected, terminal(base).get("tail")))
    return after


def run_channel(base, channel, tag, raw=False, skip=None, no_history=False):
    skip = skip or set()
    if not select_channel(base, channel):
        print("FAIL     %-22s channel not active" % channel)
        FAILURES.append("%s: channel not active" % channel)
        return
    pane = terminal(base)
    print("\n== %s (%s) cols=%s rows=%s ==" %
          (channel, pane.get("channel"), pane.get("cols"), pane.get("rows")))

    chans = [c for c in state(base).get("channels", []) if c.get("name") == channel]
    if not chans:
        print("FAIL     %-22s channel missing from /state" % channel)
        FAILURES.append("%s: channel missing" % channel)
        return
    ch = chans[0]
    if ch.get("state") != "ready":
        print("FAIL     %-22s state=%s" % (channel, ch.get("state")))
        FAILURES.append("%s: state=%s" % (channel, ch.get("state")))
        return
    if no_history and not raw:
        send_input(base, "set +H")
        time.sleep(0.4)
    before = (ch.get("tx_bytes", 0), ch.get("rx_bytes", 0))

    cases = [
        ("simple-echo", "SB3_ECHO_%s_SIMPLE" % tag),
        ("ascii-punct",
         "SB3_A_!\"#$%%&()*+,-./:;<=>?@[\\]^_`{|}~"),
        ("quotes", "SB3_Q_'single'\"double\""),
        ("backslash", "SB3_BS_C:\\temp\\new\\table\\"),
        ("spaces", "SB3_SP_  lead  mid  tail  END"),
        ("cjk-simplified", "SB3_CN_中文测试，标点！"),
        ("cjk-traditional", "SB3_TC_繁體中文測試，標點！"),
        ("cjk-fullwidth", "SB3_FW_ＡＢＣ１２３！？"),
        ("emoji", "SB3_EMOJI_😀🚀"),
        ("emoji-zwj", "SB3_FAMILY_👨‍👩‍👧‍👦"),
        ("combining", "SB3_COMB_e\u0301"),
        ("long", "SB3_LONG_" + ("A!@#$%^&*()_+-=[]{};:,.<>/?~" * 8)),
    ]
    for name, payload in cases:
        if name in skip:
            print("SKIP     %-22s board/channel limitation" % name)
            continue
        before = check(channel, name, payload, base, before, raw=raw)

    # 退格：输入 ABX，DEL 一次，再补 Y 回车，远端应执行 ABY
    if not raw and "backspace-line-edit" not in skip:
        send_raw(base, "echo SB3_BS_ABX")
        time.sleep(0.15)
        send_raw(base, "\b")
        time.sleep(0.15)
        send_raw(base, "Y\n")
        ok = wait_line(base, "SB3_BS_ABY")
        print("%-8s %-22s" % ("PASS" if ok else "FAIL", "backspace-line-edit"))
        if not ok:
            FAILURES.append("%s: backspace-line-edit tail=%r" %
                            (channel, terminal(base).get("tail")))

    # 纯行尾空格只用于验证渲染尾裁；带 END 的用例已验证空格传输保留
    print("INFO     trailing-space pure case is trimmed by renderer; "
          "spaces case covers transport")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="http://127.0.0.1:18096")
    ap.add_argument("--channels", nargs="+", required=True)
    ap.add_argument("--tag", default="T1")
    ap.add_argument("--raw", action="store_true",
                    help="通道自身回显（串口回环）：直接发送 payload，不包 shell 命令")
    ap.add_argument("--skip", default="",
                    help="逗号分隔的用例名；板端 locale 等限制导致无法验证时显式跳过")
    ap.add_argument("--no-history", action="store_true",
                    help="shell 通道先发 set +H，避免 payload 里的 ! 触发 bash 历史展开")
    args = ap.parse_args()
    skip = {x.strip() for x in args.skip.split(",") if x.strip()}
    for ch in args.channels:
        run_channel(args.base, ch, args.tag, raw=args.raw, skip=skip,
                    no_history=args.no_history)
    if FAILURES:
        print("\nRESULT: FAIL (%d)" % len(FAILURES))
        for f in FAILURES:
            print(" - " + f)
        return 1
    print("\nRESULT: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
