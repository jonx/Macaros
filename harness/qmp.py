#!/usr/bin/env python3
"""Minimal QMP client for the AROS AArch64 bring-up harness.

QMP is how the agent drives QEMU programmatically. The headline use is
'screendump' (the framebuffer "way of seeing" for M9+), but any QMP command
works via the 'cmd' subcommand (e.g. sendkey, query-status, system_reset).

  qmp.py --sock ./run/qmp.sock screendump ./run/screen.png
  qmp.py --sock ./run/qmp.sock cmd '{"execute":"query-status"}'
"""
import socket, json, sys, argparse, time, os


def connect(path, retries=50):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    for _ in range(retries):
        try:
            s.connect(path)
            break
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.1)
    else:
        sys.exit(f"qmp: could not connect to {path}")
    f = s.makefile("rw")
    f.readline()                      # consume QMP greeting
    _send(f, {"execute": "qmp_capabilities"})   # negotiate out of capabilities mode
    return f


def _send(f, obj):
    f.write(json.dumps(obj) + "\n")
    f.flush()
    while True:                       # skip async events, return the command reply
        line = f.readline()
        if not line:
            return None
        msg = json.loads(line)
        if "event" in msg:
            continue
        return msg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sock", default="./run/qmp.sock")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sd = sub.add_parser("screendump")
    sd.add_argument("out")
    sd.add_argument("--format", default="png")
    raw = sub.add_parser("cmd")
    raw.add_argument("json")
    args = ap.parse_args()

    f = connect(args.sock)

    if args.cmd == "screendump":
        out = os.path.abspath(args.out)
        # 'format' requires QEMU >= 7.1; on older builds we retry and get PPM.
        r = _send(f, {"execute": "screendump",
                      "arguments": {"filename": out, "format": args.format}})
        if r and "error" in r:
            r = _send(f, {"execute": "screendump", "arguments": {"filename": out}})
        print(json.dumps(r))
    elif args.cmd == "cmd":
        print(json.dumps(_send(f, json.loads(args.json))))


if __name__ == "__main__":
    main()
