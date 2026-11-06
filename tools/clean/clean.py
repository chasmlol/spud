"""spud disk cleaner: removes regenerable leftovers of finished work, never sources or branches.

    clean.py              list what would be removed (nothing is deleted)
    clean.py --apply      remove it, then print the free space before and after

What counts as a leftover (each rule also needs the thing to have sat unused for a while, so running work is safe):
  - build folders of worktrees whose branch is already in main and whose build hasn't run for BUILD_IDLE_H hours
    (worktrees and branches themselves are kept; a build folder is recreated by the next build);
  - private import caches, unused for CACHE_IDLE_D days: those under %LOCALAPPDATA%\\spud\\cache other than the
    shared `skyrim` one, the ones a workstream keeps in %LOCALAPPDATA%\\spud\\<name>\\cache, and every cache copy in
    Claude's private AppData store (see docs/AUTOMATION.md);
  - old run folders: perf and audit results, beyond the newest few and older than
    RUN_IDLE_D days;
  - huge background-task logs left in this project's Claude temp folders.

The gate (tools/gate/gate.py) runs this with --apply by itself when the drive gets low.
"""
import argparse
import os
import shutil
import stat
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOCAL = Path(os.environ.get("LOCALAPPDATA", str(Path.home() / "AppData" / "Local")))
SPUD = LOCAL / "spud"
SHARED_CACHE = "skyrim"

BUILD_IDLE_H = 6
CACHE_IDLE_D = 2
RUN_IDLE_D = 7
RUN_KEEP = {"perf": 10, "audit": 5}
TASK_LOG_MIN_BYTES = 200 * 1024 * 1024
TASK_LOG_IDLE_D = 1


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True)


def newest_mtime(path):
    """The newest mtime among a folder and its direct entries: cheap, and enough to tell recent use."""
    times = [path.stat().st_mtime]
    try:
        for entry in os.scandir(path):
            try:
                times.append(entry.stat(follow_symlinks=False).st_mtime)
            except OSError:
                pass
    except OSError:
        pass
    return max(times)


def idle_hours(path):
    return (time.time() - newest_mtime(path)) / 3600


def finished_builds():
    out, worktree = [], None
    for line in git("worktree", "list", "--porcelain").stdout.splitlines():
        if line.startswith("worktree "):
            worktree = Path(line[len("worktree "):])
        elif line.startswith("branch ") and worktree is not None and worktree != ROOT:
            branch = line[len("branch "):]
            build = worktree / "build"
            if not build.is_dir():
                continue
            # The newest of the build folder and its trees (build/release, build/<tree>) shows the last build.
            last = max([newest_mtime(build)] + [newest_mtime(p) for p in build.iterdir() if p.is_dir()])
            idle = (time.time() - last) / 3600
            if idle >= BUILD_IDLE_H and git("merge-base", "--is-ancestor", branch, "main").returncode == 0:
                out.append((build, f"build of a worktree already in main, unused {idle:.0f} h"))
    return out


def stale_caches():
    out = []
    roots = [SPUD] + list((LOCAL / "Packages").glob("Claude_*/LocalCache/Local/spud"))
    for root in roots:
        private_store = root != SPUD
        for cache in list((root / "cache").glob("*")) + list(root.glob("*/cache")):
            if not cache.is_dir() or cache == SPUD / "cache" / SHARED_CACHE:
                continue
            idle_d = idle_hours(cache) / 24
            if idle_d >= CACHE_IDLE_D:
                where = "Claude's private AppData copy" if private_store else "private cache"
                out.append((cache, f"{where}, unused {idle_d:.1f} days"))
    return out


def old_runs():
    out = []
    for rel, keep in RUN_KEEP.items():
        folder = SPUD / rel
        if not folder.is_dir():
            continue
        runs = sorted((p for p in folder.iterdir() if p.is_dir()), key=newest_mtime, reverse=True)
        for run in runs[keep:]:
            idle_d = idle_hours(run) / 24
            if idle_d >= RUN_IDLE_D:
                out.append((run, f"{rel} run beyond the newest {keep}, {idle_d:.0f} days old"))
    return out


def huge_task_logs():
    out = []
    temp = Path(os.environ.get("TEMP", str(LOCAL / "Temp"))) / "claude"
    for log in temp.glob("C--spud*/*/tasks/*.output"):  # this project's sessions only
        try:
            st = log.stat()
        except OSError:
            continue
        if st.st_size >= TASK_LOG_MIN_BYTES and (time.time() - st.st_mtime) / 86400 >= TASK_LOG_IDLE_D:
            out.append((log, f"background-task log of {st.st_size / 2**30:.1f} GB"))
    return out


def remove(path):
    def clear_readonly(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    if path.is_dir():
        shutil.rmtree(path, onexc=clear_readonly)
    else:
        path.unlink(missing_ok=True)


def free_gb():
    return shutil.disk_usage(ROOT).free / 2**30


def clean(apply):
    items = finished_builds() + stale_caches() + old_runs() + huge_task_logs()
    before = free_gb()
    for path, why in items:
        print(f"{'removing' if apply else 'would remove'}: {path}  ({why})", flush=True)
        if apply:
            try:
                remove(path)
            except OSError as e:
                print(f"  could not remove it: {e}", flush=True)
    if not items:
        print("nothing to clean")
    if apply:
        print(f"free space: {before:.0f} GB -> {free_gb():.0f} GB")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true", help="remove the leftovers (default: only list them)")
    return clean(ap.parse_args().apply)


if __name__ == "__main__":
    sys.exit(main())
