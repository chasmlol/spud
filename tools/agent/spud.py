#!/usr/bin/env python3
"""Small client for an spud control session (docs/AUTOMATION.md).

Starts a headless session if none is listening on the port (or --start is given),
sends each command line and prints one JSON response per line. Exits non-zero if a
command fails.

  python tools/agent/spud.py "cell 5 -2 +3000" "wait_loaded" "state -"
  python tools/agent/spud.py --cache D:/cache --width 1920 --height 1080 "screenshot C:/tmp/a.png"
  python tools/agent/spud.py --quit            # stop the session
  echo "state -" | python tools/agent/spud.py -    # lines from stdin
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import time

DEFAULT_PORT = 47110


def default_exe():
    # The most recently built spud.exe under build/*/bin.
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    build = os.path.join(root, "build")
    found = []
    if os.path.isdir(build):
        for d in os.listdir(build):
            exe = os.path.join(build, d, "bin", "spud.exe")
            if os.path.exists(exe):
                found.append((os.path.getmtime(exe), exe))
    return max(found)[1] if found else "spud.exe"


def connect(port, timeout):
    deadline = time.time() + timeout
    while True:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=2)
        except OSError:
            if time.time() > deadline:
                return None
            time.sleep(0.2)


class Session:
    def __init__(self, sock):
        self.sock = sock
        self.sock.settimeout(None)
        self.buf = b""
        self.next_id = 1

    def send(self, line):
        rid = self.next_id
        self.next_id += 1
        self.sock.sendall((json.dumps({"id": rid, "line": line}) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("session closed")
            self.buf += chunk
        raw, self.buf = self.buf.split(b"\n", 1)
        return json.loads(raw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lines", nargs="*", help="command lines ('-' reads lines from stdin)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--exe", default=default_exe())
    ap.add_argument("--start", action="store_true", help="always start a new session")
    ap.add_argument("--no-start", action="store_true", help="only attach to a running session")
    ap.add_argument("--windowed", action="store_true", help="start the session with a window")
    ap.add_argument("--cache", help="import cache for a started session")
    ap.add_argument("--world", help="worldspace for a started session")
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--log", help="write the started session's log here")
    ap.add_argument("--quit", action="store_true", help="send 'quit' at the end")
    args = ap.parse_args()

    lines = []
    for item in args.lines:
        if item == "-":
            lines += [l.rstrip("\n") for l in sys.stdin if l.strip()]
        else:
            lines.append(item)
    if args.quit:
        lines.append("quit")

    sock = None if args.start else connect(args.port, 0.5)
    if sock is None:
        if args.no_start:
            print(f"no session on 127.0.0.1:{args.port}", file=sys.stderr)
            return 2
        cmd = [args.exe, "--control", str(args.port)]
        if not args.windowed:
            cmd.append("--headless")
        for opt in ("cache", "world", "width", "height"):
            value = getattr(args, opt)
            if value is not None:
                cmd += [f"--{opt}", str(value)]
        log = open(args.log, "w") if args.log else subprocess.DEVNULL
        flags = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        subprocess.Popen(cmd, stdout=log, stderr=log, stdin=subprocess.DEVNULL, creationflags=flags)
        sock = connect(args.port, 120)
        if sock is None:
            print(f"session did not start on port {args.port}: {' '.join(cmd)}", file=sys.stderr)
            return 2

    session = Session(sock)
    status = 0
    for line in lines:
        try:
            reply = session.send(line)
        except ConnectionError:
            if line.strip() == "quit":
                break
            raise
        print(json.dumps(reply))
        sys.stdout.flush()
        if not reply.get("ok"):
            status = 1
    sock.close()
    return status


if __name__ == "__main__":
    sys.exit(main())
