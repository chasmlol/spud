#!/usr/bin/env python3
"""spud perf harness: run bench scenarios, check budgets and baselines.

Usage (normally via scripts\\perf.cmd):
    perf.py [--build DIR] [--scenario NAME ...] [--full] [--runs N] [--windowed] [--update-baseline] [--cache DIR]

Default is the quick suite: one headless run per scenario, about a minute. --full runs each scenario
three times (median) for milestone sign-off and baselines; --windowed includes presentation and pacing.

Exit codes: 0 pass, 1 budget or regression failure, 2 harness/engine error.
"""

import argparse
import bisect
import datetime
import json
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PERF_DIR = ROOT / "tools" / "perf"
SCENARIO_DIR = PERF_DIR / "scenarios"
BASELINE_DIR = PERF_DIR / "baselines"

HITCH_WINDOW = 60          # frames of history for the rolling median
HITCH_RATIO = 2.0          # a hitch is > 2x the rolling median ...
HITCH_MIN_EXCESS_MS = 2.0  # ... and at least 2 ms over it
REGRESSION_RATIO = 1.10    # fail if mean/p99 worsen by more than 10% ...
REGRESSION_MIN_MS = 0.1    # ... and by more than 0.1 ms
QUICK_SUITE_TARGET_S = 60  # the quick suite's real-time length, set by the user; shorten scenarios, never this
TOUR_FILE = PERF_DIR / "tour" / "tour.bench"


def percentile(sorted_values, p):
    if not sorted_values:
        return 0.0
    k = (len(sorted_values) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(sorted_values) - 1)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (k - lo)


def count_hitches(times):
    """Frames more than HITCH_RATIO x the median of the previous HITCH_WINDOW frames."""
    hitches = []
    window = []  # sorted copy of the last HITCH_WINDOW values
    history = []
    for i, t in enumerate(times):
        if len(window) >= 8:
            median = window[len(window) // 2]
            if t > HITCH_RATIO * median and t - median >= HITCH_MIN_EXCESS_MS:
                hitches.append((i, t, median))
        history.append(t)
        bisect.insort(window, t)
        if len(history) > HITCH_WINDOW:
            window.pop(bisect.bisect_left(window, history[-HITCH_WINDOW - 1]))
    return hitches


def frame_stats(times):
    s = sorted(times)
    deltas = sorted(abs(b - a) for a, b in zip(times, times[1:]))
    median = percentile(s, 50)
    return {
        "frames": len(times),
        "mean_ms": statistics.fmean(times) if times else 0.0,
        "p50_ms": median,
        "p95_ms": percentile(s, 95),
        "p99_ms": percentile(s, 99),
        "p999_ms": percentile(s, 99.9),
        "max_ms": s[-1] if s else 0.0,
        "stdev_ms": statistics.pstdev(times) if len(times) > 1 else 0.0,
        "jitter_p99_ms": percentile(deltas, 99),
        "max_over_median": (s[-1] / median) if median > 0 else 0.0,
        "hitches": len(count_hitches(times)),
    }


def analyse(result):
    frames = result["frames"]
    cpu = [f["cpu_ms"] for f in frames]
    gpu = [f["gpu_ms"] for f in frames if f.get("gpu_ms") is not None]
    out = {"cpu": frame_stats(cpu), "gpu": frame_stats(gpu) if gpu else None, "segments": {}}
    names = result.get("segments", [])
    for index, name in enumerate(names):
        seg = [f["cpu_ms"] for f in frames if f.get("segment") == index]
        if seg:
            out["segments"][name] = frame_stats(seg)
    hitches = count_hitches(cpu)
    out["worst_hitches"] = [
        {"frame": i, "t": frames[i].get("t"), "ms": round(t, 2), "median_ms": round(m, 2),
         "segment": names[frames[i]["segment"]] if names and frames[i].get("segment") is not None else None}
        for i, t, m in sorted(hitches, key=lambda h: -h[1])[:10]
    ]
    out["streaming"] = result.get("streaming", {})
    out["memory"] = result.get("memory", {})
    out["settings"] = result.get("settings", {})
    out["startup_ms"] = result.get("startup_ms")
    return out


def median_of_runs(runs):
    """Per-metric median across runs, so one noisy run doesn't decide the result."""
    merged = {"cpu": {}, "gpu": None, "runs": runs}
    for key in runs[0]["cpu"]:
        merged["cpu"][key] = statistics.median(r["cpu"][key] for r in runs)
    if all(r["gpu"] for r in runs):
        merged["gpu"] = {k: statistics.median(r["gpu"][k] for r in runs) for k in runs[0]["gpu"]}
    return merged


def machine_id(result):
    m = result.get("machine", {})
    raw = f'{m.get("gpu", "unknown-gpu")}-{m.get("cpu", "unknown-cpu")}'
    return re.sub(r"[^a-z0-9]+", "-", raw.lower()).strip("-")[:80]


CONTAMINATED_EXIT = 75  # the gate re-queues the run on this code


def contamination():
    """What the gate saw start outside it during this run, or None (see tools/gate/gate.py)."""
    flag = os.environ.get("SPUD_GATE_FLAG")
    if flag and Path(flag).exists():
        try:
            return json.loads(Path(flag).read_text())
        except (OSError, json.JSONDecodeError):
            return {"process": "unknown"}
    return None


def run_engine(exe, scenario, out_json, cache, windowed, timeout=900):
    cmd = [str(exe), "--bench", str(scenario), "--bench-out", str(out_json)]
    if not windowed:
        cmd.append("--headless")
    if cache:
        cmd += ["--cache", str(cache)]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if proc.returncode != 0 or not out_json.exists():
        sys.stderr.write(proc.stdout[-4000:] + proc.stderr[-4000:])
        raise RuntimeError(f"engine failed on {scenario.name} (exit {proc.returncode})")
    return json.loads(out_json.read_text())


def budget_for(budgets, machine, scenario):
    """Absolute budgets for the machine class whose id prefixes `machine`, or None."""
    for prefix, b in budgets.get("machines", {}).items():
        if machine and machine.startswith(prefix):
            return {**b["default"], **b.get("scenarios", {}).get(scenario, {})}
    return None


def check(name, stats, budget, baseline):
    failures = []
    cpu = stats["cpu"]
    if budget:
        if cpu["hitches"] > budget["hitches"]:
            failures.append(f"{cpu['hitches']:.0f} hitches (budget {budget['hitches']})")
        if cpu["mean_ms"] > budget["mean_ms"]:
            failures.append(f"mean {cpu['mean_ms']:.2f} ms ({1000 / cpu['mean_ms']:.0f} fps) > {budget['mean_ms']} ms")
        if cpu["p99_ms"] > budget["p99_ms"]:
            failures.append(f"p99 {cpu['p99_ms']:.2f} ms > {budget['p99_ms']} ms")
        if cpu["max_ms"] > budget["max_ms"]:
            failures.append(f"max {cpu['max_ms']:.2f} ms > {budget['max_ms']} ms")
        if cpu["max_over_median"] > budget["max_over_median"]:
            failures.append(f"max is {cpu['max_over_median']:.2f}x median > {budget['max_over_median']}x")
        if cpu["jitter_p99_ms"] > budget["jitter_p99_ms"]:
            failures.append(f"jitter p99 {cpu['jitter_p99_ms']:.2f} ms > {budget['jitter_p99_ms']} ms")
    if baseline:
        for key in ("mean_ms", "p99_ms"):
            old, new = baseline["cpu"][key], cpu[key]
            if new > old * REGRESSION_RATIO and new - old > REGRESSION_MIN_MS:
                failures.append(f"{key} regressed {old:.2f} -> {new:.2f} ms (+{(new / old - 1) * 100:.0f}%)")
        if cpu["hitches"] > baseline["cpu"]["hitches"]:
            failures.append(f"hitches increased {baseline['cpu']['hitches']:.0f} -> {cpu['hitches']:.0f}")
    return failures


def tour_file(part, out_dir):
    """The tour, or its setup and one `# part` of it."""
    lines = TOUR_FILE.read_text().splitlines()
    record = lines.index("record")
    head, body = lines[:record + 1], lines[record + 1:]
    if part:
        starts = [i for i, line in enumerate(body) if line.startswith("# part ")]
        if not 1 <= part <= len(starts):
            raise SystemExit(f"error: the tour has parts 1 to {len(starts)}")
        end = starts[part] if part < len(starts) else len(body)
        body = body[starts[part - 1]:end]
    out = out_dir / "tour.bench"
    out.write_text("\n".join(head + body) + "\n")
    return out


def tour_stats(result):
    """Per section, and overall: frames behind a real loading screen don't count; everything else does."""
    frames = result["frames"]
    target = result["tour"]["target_ms"]
    names = result.get("segments", [])
    zones = result.get("cpu_zones_ms", {})
    passes = result.get("gpu_passes_ms", {})
    counted = list(range(len(frames)))  # the engine leaves real loading screens out

    def stats(indices):
        t = [frames[i]["cpu_ms"] for i in indices]
        g = [frames[i]["gpu_ms"] for i in indices if frames[i]["gpu_ms"] >= 0]
        s = sorted(t)
        worst = max(indices, key=lambda i: frames[i]["cpu_ms"])
        return {
            "frames": len(t),
            "fps": 1000 / statistics.fmean(t),
            "mean_ms": statistics.fmean(t),
            "p99_ms": percentile(s, 99),
            "max_ms": s[-1],
            "over": sum(1 for v in t if v > target),
            # Frames that stepped the world: the ones carrying the game's logic. The rest draw between ticks.
            "tick_mean_ms": statistics.fmean([frames[i]["cpu_ms"] for i in indices if not frames[i].get("still")] or [0.0]),
            "tick_p99_ms": percentile(sorted(frames[i]["cpu_ms"] for i in indices if not frames[i].get("still")), 99),
            # A tick that runs a whole tick after its time is a stutter, however good the frame times look.
            "late_ticks": sum(1 for i in indices if frames[i].get("late", 0.0) > 1000.0 / result["tour"]["fixed_hz"]),
            "worst_late_ms": max((frames[i].get("late", 0.0) for i in indices), default=0.0),
            "gpu_mean_ms": statistics.fmean(g) if g else 0.0,
            "gpu_p99_ms": percentile(sorted(g), 99) if g else 0.0,
            "zones_mean_ms": {z: statistics.fmean(v[i] for i in indices) for z, v in zones.items()},
            "passes_mean_ms": {p: statistics.fmean(max(v[i], 0.0) for i in indices) for p, v in passes.items()},
            "worst": {"t": frames[worst]["t"], "ms": frames[worst]["cpu_ms"], "gpu_ms": frames[worst]["gpu_ms"],
                      "zones": {z: v[worst] for z, v in zones.items()}},
        }

    sections = {}
    for index, name in enumerate(names):
        indices = [i for i in counted if frames[i]["segment"] == index]
        if indices:
            sections[name] = stats(indices)
    return {"target_ms": target, "all": stats(counted), "sections": sections, "digests": result.get("digests", []),
            "ticks": sum(1 for f in frames if not f.get("still")), "settings": result.get("settings", {}),
            "tour": result["tour"], "memory": result.get("memory", {})}


def print_tour(run, label):
    target = run["target_ms"]
    tour = run["tour"]
    print(f"\n{label}: target: no frame over {target} ms; took {tour['wall_s']:.0f} s, of which "
          f"{tour['loading_s']:.0f} s behind loading screens (not counted); {run['ticks']} world ticks"
          f"{'' if tour['finished'] else '; STOPPED AT THE TIME LIMIT'}")
    header = (f"{'section':<14} {'frames':>7} {'fps':>6} {'mean':>6} {'p99':>6} {'max':>8} {'over':>6} {'tick':>6} {'tick99':>6} "
              f"{'late':>5} {'latest':>7} {'gpu':>6} {'gpu99':>6}")
    print(header)
    print("-" * len(header))
    for name, s in list(run["sections"].items()) + [("WHOLE TOUR", run["all"])]:
        print(f"{name:<14} {s['frames']:7d} {s['fps']:6.0f} {s['mean_ms']:6.2f} {s['p99_ms']:6.2f} {s['max_ms']:8.2f} "
              f"{s['over']:6d} {s['tick_mean_ms']:6.2f} {s['tick_p99_ms']:6.2f} {s['late_ticks']:5d} {s['worst_late_ms']:7.1f} "
              f"{s['gpu_mean_ms']:6.2f} {s['gpu_p99_ms']:6.2f}")


def run_tour(args, exe):
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = Path(os.environ.get("LOCALAPPDATA", ROOT)) / "spud" / "perf" / f"tour-{stamp}"
    out_dir.mkdir(parents=True, exist_ok=True)
    bench = tour_file(args.part, out_dir)
    runs = []
    for i in range(args.runs or 1):
        result = run_engine(exe, bench, out_dir / f"tour.run{i}.json", args.cache, args.windowed, timeout=240)
        runs.append(tour_stats(result))
        (out_dir / f"tour.run{i}.json").unlink() if args.slim else None
        if bad := contamination():
            print(f"CONTAMINATED: {bad.get('process')} started outside the gate during the run; "
                  "results discarded, nothing recorded", file=sys.stderr)
            return CONTAMINATED_EXIT
        print_tour(runs[-1], f"run {i + 1}")
    (out_dir / "report.json").write_text(json.dumps(runs, indent=2))
    print(f"\nresults: {out_dir}")
    same = all(r["digests"] == runs[0]["digests"] for r in runs)
    if len(runs) > 1:
        print("replay:", "identical" if same else "DIFFERS")
        if not same:
            for i, name in enumerate(runs[0]["sections"]):
                row = [r["digests"][i] if i < len(r["digests"]) else "-" for r in runs]
                print(f"    {name:<14} {'same' if len(set(row)) == 1 else 'differs: ' + ' '.join(d[:8] for d in row)}")
    passed = all(r["all"]["over"] == 0 and r["all"]["late_ticks"] == 0 and r["tour"]["finished"] for r in runs)
    print("RESULT:", "PASS" if passed else "FAIL",
          f"(frames over {runs[0]['target_ms']} ms per run: {[r['all']['over'] for r in runs]}; "
          f"late ticks: {[r['all']['late_ticks'] for r in runs]})")
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=str(ROOT / "build" / "release"))
    parser.add_argument("--scenario", action="append", help="scenario name (default: all available)")
    parser.add_argument("--full", action="store_true",
                        help="3 runs per scenario (median); required for --update-baseline. Default: 1 quick run")
    parser.add_argument("--runs", type=int, help="override the number of runs per scenario")
    parser.add_argument("--update-baseline", action="store_true")
    parser.add_argument("--cache", help="import cache dir (default: the engine's default)")
    parser.add_argument("--windowed", action="store_true",
                        help="run through a real window and present (default: headless, nothing on screen)")
    parser.add_argument("--tour", action="store_true", help="the 300 fps tour instead of the scenarios")
    parser.add_argument("--part", type=int, help="tour: only this `# part` of it")
    parser.add_argument("--slim", action="store_true", help="tour: keep only the report, not the per-frame results")
    args = parser.parse_args()
    if args.tour:
        exe = Path(args.build) / "bin" / "spud.exe"
        if not exe.exists():
            print(f"error: {exe} not found; build first (scripts\\build.cmd release)", file=sys.stderr)
            return 2
        return run_tour(args, exe)
    mode = "windowed" if args.windowed else "headless"
    if args.runs is None:
        args.runs = 3 if args.full else 1
    if args.update_baseline and args.runs < 3:
        print("error: baselines must come from a --full run (3 runs per scenario)", file=sys.stderr)
        return 2
    if os.environ.get("SPUD_GATE") != "exclusive":
        print("warning: not running under an exclusive gate pass; other work may skew the numbers. "
              "Use scripts\\perf.cmd.", file=sys.stderr)
    started = time.monotonic()

    exe = Path(args.build) / "bin" / "spud.exe"
    if not exe.exists():
        print(f"error: {exe} not found; build first (scripts\\build.cmd release)", file=sys.stderr)
        return 2
    scenarios = sorted(SCENARIO_DIR.glob("*.bench"))
    if args.scenario:
        scenarios = [s for s in scenarios if s.stem in args.scenario]
    if not scenarios:
        print("error: no scenarios matched", file=sys.stderr)
        return 2

    budgets = json.loads((PERF_DIR / "budgets.json").read_text())
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = Path(os.environ.get("LOCALAPPDATA", ROOT)) / "spud" / "perf" / stamp
    out_dir.mkdir(parents=True, exist_ok=True)

    report = {"time": stamp, "scenarios": {}}
    machine = None
    failed = False
    for scenario in scenarios:
        runs = []
        try:
            for i in range(args.runs):
                result = run_engine(exe, scenario, out_dir / f"{scenario.stem}.run{i}.json", args.cache,
                                    args.windowed)
                machine = machine or machine_id(result)
                runs.append(analyse(result))
        except (RuntimeError, subprocess.TimeoutExpired, json.JSONDecodeError, KeyError) as e:
            print(f"error: {scenario.stem}: {e}", file=sys.stderr)
            return 2
        if bad := contamination():
            print(f"CONTAMINATED: {bad.get('process')} started outside the gate during the run; "
                  "results discarded, nothing recorded", file=sys.stderr)
            return CONTAMINATED_EXIT
        report["scenarios"][scenario.stem] = median_of_runs(runs)

    baseline_path = BASELINE_DIR / f"{machine}-{mode}.json"
    baseline = json.loads(baseline_path.read_text()) if baseline_path.exists() else None

    print(f"\nmachine {machine}, {mode}, {args.runs} runs per scenario, median shown (results: {out_dir})\n")
    if not budget_for(budgets, machine, ""):
        print("no absolute budgets for this machine class; checking regressions against its baseline only\n")
    header = (f"{'scenario':<20} {'frames':>7} {'fps':>6} {'mean':>6} {'p99':>6} {'p99.9':>6} {'max':>6} "
              f"{'jit99':>6} {'hitch':>5} {'gpu99':>6}  result")
    print(header)
    print("-" * len(header))
    for name, stats in report["scenarios"].items():
        budget = budget_for(budgets, machine, name)
        base = baseline["scenarios"].get(name) if baseline and not args.update_baseline else None
        failures = check(name, stats, budget, base)
        stats["failures"] = failures
        failed |= bool(failures)
        c, g = stats["cpu"], stats["gpu"]
        gpu99 = f"{g['p99_ms']:6.2f}" if g else "     -"
        fps = 1000 / c["mean_ms"] if c["mean_ms"] > 0 else 0
        verdict = "PASS" if not failures else "FAIL"
        print(f"{name:<20} {c['frames']:7.0f} {fps:6.0f} {c['mean_ms']:6.2f} {c['p99_ms']:6.2f} {c['p999_ms']:6.2f} "
              f"{c['max_ms']:6.2f} {c['jitter_p99_ms']:6.2f} {c['hitches']:5.0f} {gpu99}  {verdict}")
        for f in failures:
            print(f"    - {f}")
        worst = stats["runs"][0]["worst_hitches"]
        for h in worst[:3]:
            print(f"    hitch: frame {h['frame']} t={h['t']}s {h['ms']} ms (median {h['median_ms']}) {h['segment'] or ''}")

    if bad := contamination():
        print(f"CONTAMINATED: {bad.get('process')} started outside the gate during the run; "
              "results discarded, nothing recorded", file=sys.stderr)
        return CONTAMINATED_EXIT
    (out_dir / "report.json").write_text(json.dumps(report, indent=2))
    if args.update_baseline:
        BASELINE_DIR.mkdir(parents=True, exist_ok=True)
        slim = {"time": stamp, "scenarios": {n: {"cpu": s["cpu"], "gpu": s["gpu"]} for n, s in report["scenarios"].items()}}
        baseline_path.write_text(json.dumps(slim, indent=2))
        print(f"\nbaseline written: {baseline_path}")
    elif not baseline:
        print(f"\nno baseline for this machine yet; record one with --update-baseline")

    took = time.monotonic() - started
    print(f"\nsuite took {took:.0f} s")
    if args.runs == 1 and not args.scenario and took > QUICK_SUITE_TARGET_S * 1.05:
        print(f"WARNING: the quick suite must take {QUICK_SUITE_TARGET_S} s; it took {took:.0f} s. "
              "Shorten scenarios in tools/perf/gen_scenarios.py.")
    print("RESULT:", "FAIL" if failed else "PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
