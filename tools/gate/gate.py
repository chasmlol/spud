#!/usr/bin/env python3
"""spud gate: one queue for the machine's heavy work, so perf runs never overlap anything else.

Every CPU- or GPU-heavy job runs under a pass handed out by a small broker process:

    shared     builds, imports, headless checks, the image audit, game and engine windows.
               Any number at once.
    exclusive  perf suites, bench runs, timing-sensitive tests. One at a time, on a quiet machine.

The user's own launchers (--who "you (...)") go ahead of waiting exclusive jobs: only a running one holds them up.
Other requests are served in arrival order. Once an exclusive request is waiting, no new shared work starts:
running shared jobs finish, the exclusive job runs alone, then the queue carries on. One exception keeps a
checkout from blocking itself: if it already held a shared pass before the exclusive request arrived
its further shared work still starts, because the exclusive job is waiting
for that checkout anyway. Work it starts after its older passes end queues normally. A pass belongs to
the process holding it and is released when that process exits, however it exits.

Usage (normally through the scripts in scripts\\, which call this for you):
    gate.py run shared|exclusive [--who NAME] [--what TEXT] [--retries N] [--quiet-timeout S] -- COMMAND...
    gate.py status
    gate.py serve        the broker; started automatically on first use

Before an exclusive job starts, the gate also waits for heavy work that bypassed it (compilers, spud,
skytool, Skyrim, a busy GPU). While the job runs it keeps watching. If such work appears, the run is
contaminated: the file named by SPUD_GATE_FLAG is written so the job can refuse to report or record
baselines, and the job is re-queued up to --retries times. Exit code 75 means it never ran clean.
"""

import argparse
import ctypes
import json
import multiprocessing
import os
import shutil
import subprocess
import sys
import threading
import time
from ctypes import wintypes
from multiprocessing.connection import Client, Listener
from pathlib import Path

# SPUD_GATE_PIPE / SPUD_GATE_STATE / SPUD_GATE_OWNER let the gate's tests run a private broker.
PIPE = os.environ.get("SPUD_GATE_PIPE") or r"\\.\pipe\spud-gate"
AUTHKEY = b"spud-gate-1"
PROTOCOL = 3
USER_WHO = "you ("  # the --who of the launchers the user starts by hand
RECONNECT_GRACE_S = 5
ROOT = Path(__file__).resolve().parents[2]
# Which checkout (session) a request comes from: its passes count as one owner (see the module doc).
OWNER = os.environ.get("SPUD_GATE_OWNER") or str(ROOT).lower()
STATE_DIR = (Path(os.environ["SPUD_GATE_STATE"]) if os.environ.get("SPUD_GATE_STATE") else
             Path(os.environ.get("LOCALAPPDATA", str(ROOT))) / "spud" / "gate")
CONTAMINATED_EXIT = 75
BROKER_IDLE_EXIT_S = 1800

# Heavy work that must never overlap an exclusive job, whether or not it went through the gate.
# SPUD_GATE_WATCH (comma-separated names) and SPUD_GATE_NO_GPU_CHECK=1 exist for the gate's own tests.
WATCHED = ({n.strip().lower() for n in os.environ["SPUD_GATE_WATCH"].split(",")}
           if os.environ.get("SPUD_GATE_WATCH") else
           {"cl.exe", "link.exe", "ninja.exe", "glslc.exe", "spud.exe", "skytool.exe", "skyrimse.exe",
            "skse64_loader.exe"})
GPU_CHECK = not os.environ.get("SPUD_GATE_NO_GPU_CHECK")
GPU_QUIET_PERCENT = 10

# A nearly full drive slows the whole machine down (the dev machine's QLC SSD stalls on writes when full) and then
# fails builds and imports. Below DISK_LOW_GB the gate removes finished work's leftovers (tools/clean/clean.py)
# before heavy work; below DISK_CRITICAL_GB it refuses new heavy work.
DISK_LOW_GB = 60
DISK_CRITICAL_GB = 20


class GateError(Exception):
    pass


def note(text):
    print(f"[gate] {text}", file=sys.stderr, flush=True)


# --- processes and GPU -------------------------------------------------------------------------------

class _ProcessEntry(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD),
                ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
_k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
_k32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ProcessEntry)]
_k32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ProcessEntry)]
_k32.CloseHandle.argtypes = [wintypes.HANDLE]
_INVALID_HANDLE = wintypes.HANDLE(-1).value


def processes():
    """(pid, parent pid, lowercase exe name) for every process on the machine."""
    snap = _k32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if snap in (None, _INVALID_HANDLE):
        return []
    out = []
    try:
        entry = _ProcessEntry()
        entry.dwSize = ctypes.sizeof(_ProcessEntry)
        ok = _k32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            out.append((entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile.lower()))
            ok = _k32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        _k32.CloseHandle(snap)
    return out


def outside_heavy(job_pid=None):
    """Watched processes that are not part of the job rooted at job_pid: [(name, pid)]."""
    procs = processes()
    mine = set()
    if job_pid:
        children = {}
        for pid, ppid, _ in procs:
            children.setdefault(ppid, []).append(pid)
        stack = [job_pid]
        while stack:
            pid = stack.pop()
            if pid not in mine:
                mine.add(pid)
                stack.extend(children.get(pid, []))
    return sorted({(name, pid) for pid, _, name in procs if name in WATCHED and pid not in mine})


def gpu_busy_percent():
    try:
        out = subprocess.run(["nvidia-smi", "--query-gpu=utilization.gpu", "--format=csv,noheader,nounits"],
                             capture_output=True, text=True, timeout=10).stdout
        values = [int(v) for v in out.split() if v.strip().isdigit()]
        return max(values) if values else None
    except (OSError, subprocess.SubprocessError):
        return None


def wait_quiet(timeout_s):
    """Before an exclusive job: wait for heavy work outside the gate to finish and the GPU to go idle."""
    start = time.monotonic()
    last_note = 0.0
    quiet = 0
    while True:
        heavy = outside_heavy()
        gpu = gpu_busy_percent() if GPU_CHECK else None
        if not heavy and (gpu is None or gpu <= GPU_QUIET_PERCENT):
            quiet += 1
            if quiet >= 3:
                return
        else:
            quiet = 0
            busy = ", ".join(f"{name} (pid {pid})" for name, pid in heavy[:4]) or f"GPU at {gpu}%"
            if time.monotonic() - last_note > 15:
                note(f"waiting for work outside the gate to finish: {busy}")
                last_note = time.monotonic()
            if time.monotonic() - start > timeout_s:
                raise GateError(f"the machine was still busy after {timeout_s} s ({busy}); exclusive job not started")
        time.sleep(1)


# --- broker ------------------------------------------------------------------------------------------

def log(text):
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    with open(STATE_DIR / "broker.log", "a", encoding="utf-8") as f:
        f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {text}\n")


def is_user(req):
    return req["who"].startswith(USER_WHO)


class Broker:
    def __init__(self):
        self.cond = threading.Condition()
        self.queue = []    # waiting requests, in arrival order
        self.holders = {}  # id -> granted request
        self.next_id = 1
        self.clients = 0
        self.last_activity = time.monotonic()
        self.started = time.time()
        self.draining = False  # exit as soon as nothing is held or queued, so newer code can take over

    def grantable(self, req):
        if req["mode"] == "exclusive":
            # A broker that has just started hasn't heard from the clients reconnecting to it yet.
            settled = time.time() - self.started > RECONNECT_GRACE_S
            return self.queue[0] is req and not self.holders and settled and not any(map(is_user, self.queue))
        if any(h["mode"] == "exclusive" for h in self.holders.values()):
            return False
        # The user's own launchers only ever wait for an exclusive job that is really running.
        if is_user(req):
            return True
        # An exclusive request waiting ahead of this one goes first...
        position = next(i for i, q in enumerate(self.queue) if q is req)
        first_exclusive = next((q for q in self.queue[:position] if q["mode"] == "exclusive"), None)
        if first_exclusive is None:
            return True
        # ...unless this checkout already held the machine before it asked: the exclusive job is waiting
        # for that checkout regardless, and making its follow-up work wait too would deadlock it on itself.
        return req["owner"] is not None and any(
            h["owner"] == req["owner"] and h["granted"] <= first_exclusive["arrived"]
            for h in self.holders.values())

    def status(self):
        now = time.time()

        def row(r):
            return {"mode": r["mode"], "who": r["who"], "what": r["what"], "for_s": round(now - r["since"])}

        with self.cond:
            return {"holders": [row(r) for r in self.holders.values()], "queue": [row(r) for r in self.queue],
                    "broker_pid": os.getpid(), "up_s": round(now - self.started), "protocol": PROTOCOL,
                    "draining": self.draining}

    def serve_client(self, conn):
        req = None
        with self.cond:
            self.clients += 1
        try:
            msg = conn.recv()
            if msg.get("op") == "status":
                conn.send(self.status())
                return
            if msg.get("op") == "drain":
                with self.cond:
                    self.draining = True
                log("draining: will exit once nothing is held or queued")
                conn.send({"state": "draining"})
                return
            if msg.get("op") != "acquire" or msg.get("mode") not in ("shared", "exclusive"):
                conn.send({"error": "bad request"})
                return
            with self.cond:
                now = time.time()
                req = {"id": self.next_id, "mode": msg["mode"], "who": msg.get("who", "?"),
                       "what": msg.get("what", ""), "owner": msg.get("owner"), "since": now, "arrived": now,
                       "granted": None}
                self.next_id += 1
                self.queue.append(req)
            last_update = 0.0
            while True:
                with self.cond:
                    if self.grantable(req):
                        self.queue.remove(req)
                        req["since"] = req["granted"] = time.time()
                        self.holders[req["id"]] = req
                        break
                    holders = [{"mode": h["mode"], "who": h["who"], "what": h["what"],
                                "for_s": round(time.time() - h["since"])} for h in self.holders.values()]
                    ahead = next(i for i, q in enumerate(self.queue) if q is req)
                    self.cond.wait(1.0)
                if conn.poll():
                    conn.recv()  # raises EOFError when the waiting client has gone away
                if time.monotonic() - last_update > 5:
                    conn.send({"state": "waiting", "holders": holders, "ahead": ahead})
                    last_update = time.monotonic()
            log(f"grant {req['mode']} #{req['id']} {req['who']}: {req['what']}")
            conn.send({"state": "granted"})
            while conn.recv().get("op") != "release":  # EOFError when the holder exits
                pass
        except (EOFError, OSError):
            pass
        finally:
            with self.cond:
                if req is not None:
                    if any(q is req for q in self.queue):
                        self.queue.remove(req)
                    if self.holders.pop(req["id"], None):
                        log(f"release #{req['id']} {req['who']}")
                self.clients -= 1
                self.last_activity = time.monotonic()
                self.cond.notify_all()
            conn.close()


def serve():
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    try:
        listener = Listener(PIPE, family="AF_PIPE", authkey=AUTHKEY)
    except OSError:
        return 0  # another broker already owns the pipe
    broker = Broker()
    log(f"broker started (pid {os.getpid()})")

    def watchdog():
        while True:
            time.sleep(1)
            with broker.cond:
                empty = not broker.holders and not broker.queue
                idle = empty and broker.clients == 0 and time.monotonic() - broker.last_activity > BROKER_IDLE_EXIT_S
                drained = empty and broker.draining
            if idle or drained:
                log("drained, exiting" if drained else "idle, exiting")
                os._exit(0)

    threading.Thread(target=watchdog, daemon=True).start()
    while True:
        try:
            conn = listener.accept()
        except (OSError, EOFError, multiprocessing.AuthenticationError):
            continue
        threading.Thread(target=broker.serve_client, args=(conn,), daemon=True).start()


# --- client ------------------------------------------------------------------------------------------

def start_broker():
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, "-I", str(Path(__file__).resolve()), "serve"]
    detached = 0x00000008 | 0x00000200  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
    for breakaway in (0x01000000, 0):    # CREATE_BREAKAWAY_FROM_JOB where allowed: outlive the caller
        try:
            subprocess.Popen(cmd, creationflags=detached | breakaway, cwd=str(STATE_DIR), close_fds=True,
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return
        except OSError:
            continue


def connect(start=True):
    try:
        return Client(PIPE, family="AF_PIPE", authkey=AUTHKEY)
    except OSError:
        if not start:
            raise
    start_broker()
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        time.sleep(0.2)
        try:
            return Client(PIPE, family="AF_PIPE", authkey=AUTHKEY)
        except OSError:
            continue
    raise GateError(f"could not reach the gate broker (log: {STATE_DIR / 'broker.log'})")


def describe(holders):
    return "; ".join(f"{h['mode']} {h['who']}: {h['what']} ({h['for_s']} s)" for h in holders)


class Pass:
    """Holds a shared or exclusive pass for the duration of a with-block."""

    def __init__(self, mode, who, what):
        self.mode, self.who, self.what = mode, who, what
        self.conn = None

    def __enter__(self):
        while True:
            try:
                self.conn = connect()
                self.conn.send({"op": "acquire", "v": PROTOCOL, "mode": self.mode, "who": self.who,
                                "what": self.what, "owner": OWNER})
                waited = False
                while True:
                    msg = self.conn.recv()
                    if msg.get("state") == "granted":
                        if waited:
                            note("go")
                        return self
                    if msg.get("state") == "waiting":
                        waited = True
                        busy = describe(msg["holders"]) or "work queued ahead"
                        note(f"waiting for the machine: {busy} ({msg['ahead']} ahead in the queue)")
                    elif msg.get("error"):
                        raise GateError(msg["error"])
            except (EOFError, OSError):
                note("lost the gate broker while waiting; reconnecting")
                time.sleep(1)

    def __exit__(self, *exc):
        try:
            self.conn.send({"op": "release"})
            self.conn.close()
        except (OSError, AttributeError):
            pass
        return False


def short_what(command):
    """A readable label: drop 'cmd /c', keep script and program names without their directories."""
    parts = list(command)
    if len(parts) >= 2 and Path(parts[0]).name.lower() in ("cmd", "cmd.exe") and parts[1].lower() == "/c":
        parts = parts[2:]
    return " ".join(Path(p).name if p.lower().endswith((".py", ".cmd", ".bat", ".exe")) else p for p in parts)


def run_job(command, mode, limit=None):
    """Runs the command, ending its whole process tree after `limit` seconds if one is given; for exclusive jobs,
    watches for heavy work outside it. Returns (code, intruders)."""
    env = dict(os.environ, SPUD_GATE=mode)
    flag = None
    if mode == "exclusive":
        flag = STATE_DIR / f"contaminated-{os.getpid()}.json"
        flag.unlink(missing_ok=True)
        env["SPUD_GATE_FLAG"] = str(flag)
    proc = subprocess.Popen(command, env=env)
    intruders = {}
    deadline = time.monotonic() + limit if limit else None
    try:
        while True:
            try:
                proc.wait(timeout=0.5 if flag or deadline else None)
                break
            except subprocess.TimeoutExpired:
                if deadline and time.monotonic() > deadline:
                    note(f"over its {limit:.0f} s limit; ending it")
                    subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True)
                    proc.wait()
                    break
                for name, pid in outside_heavy(proc.pid) if flag else ():
                    if pid not in intruders:
                        intruders[pid] = name
                        note(f"CONTAMINATED: {name} (pid {pid}) started outside the gate during this run")
                        flag.write_text(json.dumps({"process": name, "pid": pid, "time": time.time()}))
    except KeyboardInterrupt:
        proc.wait()
    if flag:
        flag.unlink(missing_ok=True)
    return proc.returncode, intruders


def free_gb():
    return shutil.disk_usage(ROOT).free / 2**30


def check_disk():
    if free_gb() >= DISK_LOW_GB:
        return
    cleaner = ROOT / "tools" / "clean" / "clean.py"
    lock = STATE_DIR / "clean.lock"
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    try:
        if lock.exists() and time.time() - lock.stat().st_mtime > 3600:
            lock.unlink(missing_ok=True)  # a cleaner that died
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        fd = None  # another client is cleaning right now
    if fd is not None and cleaner.exists():
        try:
            note(f"only {free_gb():.0f} GB free: removing finished work's leftovers (scripts\\clean.cmd --apply)")
            with open(STATE_DIR / "clean.log", "a", encoding="utf-8") as log:
                log.write(f"--- {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
                log.flush()
                subprocess.call([sys.executable, "-I", str(cleaner), "--apply"], stdout=log, stderr=log)
        finally:
            os.close(fd)
            lock.unlink(missing_ok=True)
    if free_gb() < DISK_CRITICAL_GB:
        raise GateError(f"only {free_gb():.0f} GB free on {ROOT.drive}; free some space before more heavy work "
                        f"(scripts\\clean.cmd lists what can go)")


def cmd_run(args):
    command = args.command
    if not command:
        raise GateError("nothing to run")
    if os.environ.get("SPUD_GATE"):
        # Already inside a pass (a gated script calling another): nesting would deadlock, so just run.
        return subprocess.call(command)
    check_disk()
    who = args.who or os.environ.get("SPUD_GATE_WHO") or ROOT.name
    what = args.what or short_what(command)
    attempt = 0
    while True:
        with Pass(args.mode, who, what):
            if args.mode == "exclusive":
                wait_quiet(args.quiet_timeout)
            code, intruders = run_job(command, args.mode, args.limit)
        if not intruders:
            return code
        attempt += 1
        if attempt > args.retries:
            note(f"still contaminated after {args.retries} re-runs; results must not be used")
            return CONTAMINATED_EXIT
        note(f"re-queuing (re-run {attempt} of {args.retries})")


def cmd_drain(_args):
    """Ask the running broker to exit once idle; the next request starts this checkout's gate.py."""
    try:
        conn = connect(start=False)
    except OSError:
        print("gate: no broker running; the next request starts the current code")
        return 0
    conn.send({"op": "drain", "v": PROTOCOL})
    try:
        reply = conn.recv()
    except EOFError:
        reply = {}
    conn.close()
    if reply.get("state") == "draining":
        print("gate: broker will exit as soon as nothing is held or queued")
        return 0
    print("gate: the running broker predates 'drain'; it is replaced when it next goes idle for 30 minutes")
    return 1


def cmd_status(_args):
    try:
        conn = connect(start=False)
    except OSError:
        print("gate: idle (no broker running)")
    else:
        conn.send({"op": "status", "v": PROTOCOL})
        s = conn.recv()
        conn.close()
        extra = " (draining: restarts with newer code once idle)" if s.get("draining") else ""
        print(f"gate broker pid {s['broker_pid']}, protocol {s.get('protocol', 1)}, up {s['up_s']} s{extra}")
        print("holding:" if s["holders"] else "holding: nothing")
        for h in s["holders"]:
            print(f"  {h['mode']:<9} {h['who']:<28} {h['what']}  ({h['for_s']} s)")
        print("queued:" if s["queue"] else "queued: nothing")
        for q in s["queue"]:
            print(f"  {q['mode']:<9} {q['who']:<28} {q['what']}  (waiting {q['for_s']} s)")
    outside = outside_heavy()
    if outside:
        print("heavy processes running (gated or not): " + ", ".join(f"{n} ({p})" for n, p in outside))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="op", required=True)
    run = sub.add_parser("run", help="run a command under a pass")
    run.add_argument("mode", choices=["shared", "exclusive"])
    run.add_argument("--who", help="who is asking (default: this checkout's folder name)")
    run.add_argument("--what", help="label shown in status (default: the command)")
    run.add_argument("--retries", type=int, default=0, help="exclusive: re-runs after contamination")
    run.add_argument("--limit", type=float, help="seconds before the job is ended, with its whole process tree")
    run.add_argument("--quiet-timeout", type=int, default=900,
                     help="exclusive: seconds to wait for outside work to finish before giving up")
    sub.add_parser("status", help="show who holds the machine and what is queued")
    sub.add_parser("drain", help="restart the broker with the current code once it is idle")
    sub.add_parser("serve", help="run the broker (started automatically)")
    # Everything after "--" is the command, passed through untouched (argparse would eat its options).
    argv = sys.argv[1:]
    command = []
    if "--" in argv:
        split = argv.index("--")
        argv, command = argv[:split], argv[split + 1:]
    args = parser.parse_args(argv)
    args.command = command
    try:
        if args.op == "serve":
            return serve()
        if args.op == "status":
            return cmd_status(args)
        if args.op == "drain":
            return cmd_drain(args)
        return cmd_run(args)
    except GateError as e:
        note(f"error: {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
