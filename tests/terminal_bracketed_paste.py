#!/usr/bin/env python3
"""bracketed paste 真实 SSH 验证：粘贴内容进入输入缓冲，按 Enter 前不执行。

用法：
  python tests/terminal_bracketed_paste.py --base http://127.0.0.1:18096 \
      --channel ssh-demo1 --tag BP1
"""

import argparse
import sys
import time

from terminal_key_matrix import Client

A = "printf 'BP_A '"
B = "printf 'BP_B\\n'"
MARK = "BP_A BP_B"


def has_exact_line(client, text):
    return any(line.strip() == text for line in client.tail().splitlines())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="http://127.0.0.1:18096")
    ap.add_argument("--token", default="")
    ap.add_argument("--channel", default="ssh-demo1")
    ap.add_argument("--tag", default="BP1")
    args = ap.parse_args()

    c = Client(args.base, args.token)
    c.select(args.channel)
    for _ in range(50):
        if any(x in c.tail() for x in ("$", "#")):
            break
        time.sleep(0.2)
    else:
        print("FAIL BRACKET-prompt : no prompt")
        return 1

    seq = "\x1b[200~" + A + "\n" + B + "\x1b[201~"
    c.send(seq)
    time.sleep(1.2)
    early = has_exact_line(c, MARK)
    if early:
        print("FAIL BRACKET-hold : executed before Enter")
        return 1
    print("PASS BRACKET-hold : pasted text not executed before Enter")

    c.send("\r")
    for _ in range(10):
        time.sleep(0.3)
        if has_exact_line(c, MARK):
            print("PASS BRACKET-submit : both commands ran after Enter")
            print("RESULT: ALL PASS")
            return 0
    print("FAIL BRACKET-submit : outputs missing")
    return 1


if __name__ == "__main__":
    sys.exit(main())
