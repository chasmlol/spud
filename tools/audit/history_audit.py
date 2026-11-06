#!/usr/bin/env python3
"""spud image-clarity audit: proves no frame depends on an earlier one (docs/RENDERING.md).

Usage (normally via scripts\\audit.cmd):
    history_audit.py [--build DIR] [--cache DIR] [--static-only]

1. Static checks (no GPU): GPU memory is only allocated through render::Device (so every resource
   declares its cross-frame lifetime), and the renderer and shaders contain no whole-image temporal
   techniques (TAA and its relatives). Effect-local temporal state is allowed; see docs/RENDERING.md.
2. History check (headless GPU): each pose is rendered twice. The first time the camera gets there by
   flying through other places; the second time a fresh process starts at the pose. Both images must
   match, and the first must also match itself 30 frames later. Up to MAX_DIFF_SHARE of the pixels may
   differ: the run prints them and still passes.

Exit codes: 0 pass, 1 audit failure, 2 harness/engine error.
"""

import argparse
import datetime
import os
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Words that name whole-image temporal techniques: TAA, TAA-based upscalers, frame generation and the
# camera jitter that feeds them. Effect-local temporal state (water sims, volumetric accumulation) is
# allowed and goes through the lifetime allowlist instead. A match in renderer code or shaders fails the
# audit; if it's a false positive, rename the identifier rather than weakening this list.
# Underscores count as separators so names inside snake_case identifiers are caught (my_taa_pass).
_START, _END = r"(?<![A-Za-z0-9])", r"(?![A-Za-z0-9])"
BANNED = re.compile(
    r"(?i)" + _START + r"((taa|dlss|xess|tsr)" + _END + r"|temporal_?(aa|anti_?alias|upscal|resolve)|fsr_?[234]"
    r"|frame_?(gen|interpolat)|optical_?flow|(camera|projection|subpixel|view)_?jitter"
    r"|prev(ious)?_?(final|scene)_?(colou?r|image|frame)|checkerboard)")
ALLOCATORS = re.compile(r"\b(vmaCreateImage|vmaCreateBuffer|vkCreateImage|vkCreateBuffer)\s*\(")

# A history in the image changes much of it; stray pixels on thin edges are only timing, so they don't fail a run. The limit
# is the user's call (2026-11-05).
MAX_DIFF_SHARE = 0.01

RES = (1280, 720)
# The history check itself: few enough frames that a full-screen history can't have converged, with the two effects
# allowed to ease (eye adaptation, the focus weight) held at fixed values.
HISTORY_FRAMES = 5
# A second pass with them live, compared once they have settled: the focus weight eases over 1.5 s at 1/60 s a frame,
# the adaptation closes in sooner. It only answers for those two effects.
SETTLED_FRAMES = 150


def cell(x, y):
    return x * 4096 + 2048, y * 4096 + 2048


# name: (cell, z above terrain in units, yaw, pitch, approach cell 2-3 cells away)
POSES = {
    "riverwood": ((4, -12), 250, 45, 5, (2, -14)),
    "whiterun-plains": ((5, -5), 400, 0, 5, (2, -7)),
    "falkreath-forest": ((-6, -21), 300, 75, 0, (-9, -22)),
    "throat-vista": ((13, -13), 2000, 270, 15, (16, -11)),
}


def static_checks():
    failures = []
    for path in sorted((ROOT / "src").rglob("*.[ch]pp")):
        rel = path.relative_to(ROOT).as_posix()
        if rel == "src/render/device.cpp":
            continue
        for n, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if ALLOCATORS.search(line):
                failures.append(f"{rel}:{n}: allocates GPU memory outside render::Device (no declared lifetime)")
    sources = list((ROOT / "src" / "render").rglob("*.[ch]pp")) + list((ROOT / "shaders").rglob("*.*"))
    for path in sorted(sources):
        rel = path.relative_to(ROOT).as_posix()
        for n, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            m = BANNED.search(line)
            if m:
                failures.append(f"{rel}:{n}: temporal technique keyword '{m.group(0)}'")
    return failures


def png_pixels(path):
    """Minimal PNG decoder for 8-bit RGB/RGBA, non-interlaced (what the engine writes)."""
    data = path.read_bytes()
    pos, idat, width, height, channels = 8, b"", 0, 0, 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            channels = {2: 3, 6: 4}[color]
            assert depth == 8
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = row[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1:
                row[i] = (row[i] + a) & 0xFF
            elif f == 2:
                row[i] = (row[i] + b) & 0xFF
            elif f == 3:
                row[i] = (row[i] + (a + b) // 2) & 0xFF
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                row[i] = (row[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        out[y * stride:(y + 1) * stride] = row
        prev = row
    return width, height, channels, out


def write_diff_png(path, width, height, mask):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    rows = b"".join(b"\x00" + bytes(mask[y * width:(y + 1) * width]) for y in range(height))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def compare(a, b, diff_path):
    """None if identical, else (over the limit?, a description), with a diff mask image."""
    if a.read_bytes() == b.read_bytes():
        return None
    wa, ha, ca, pa = png_pixels(a)
    wb, hb, cb, pb = png_pixels(b)
    if (wa, ha, ca) != (wb, hb, cb):
        return True, f"different sizes {wa}x{ha} vs {wb}x{hb}"
    mask = bytearray(wa * ha)
    count, worst = 0, 0
    for i in range(wa * ha):
        d = max(abs(pa[i * ca + k] - pb[i * ca + k]) for k in range(3))
        if d:
            count += 1
            worst = max(worst, d)
            mask[i] = 255
    if count == 0:
        return None  # same pixels, different encoding
    write_diff_png(diff_path, wa, ha, mask)
    share = count / (wa * ha)
    return share > MAX_DIFF_SHARE, f"{count} pixels differ ({100.0 * share:.3f}%), max channel difference {worst}; mask: {diff_path}"


def pose_line(cmd, name):
    (cx, cy), z, yaw, pitch, _ = POSES[name]
    x, y = cell(cx, cy)
    return f"{cmd} {x} {y} +{z} {yaw} {pitch}"


def run(exe, script_text, script_path, cache):
    script_path.write_text(script_text)
    # One weather throughout: which one the regions on the way pick is game state, like the hour, not history in the image.
    cmd = [str(exe), "--headless", "--weather", "0000081A", "--run", str(script_path)]
    if cache:
        cmd += ["--cache", str(cache)]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout[-3000:] + proc.stderr[-3000:])
        raise RuntimeError(f"engine failed on {script_path.name} (exit {proc.returncode})")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=str(ROOT / "build" / "release"))
    parser.add_argument("--cache")
    parser.add_argument("--static-only", action="store_true")
    args = parser.parse_args()

    failures = static_checks()
    print(f"static checks: {'PASS' if not failures else 'FAIL'}")
    for f in failures:
        print(f"    - {f}")
    if args.static_only:
        return 1 if failures else 0

    exe = Path(args.build) / "bin" / "spud.exe"
    if not exe.exists():
        print(f"error: {exe} not found; build first", file=sys.stderr)
        return 2
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = Path(os.environ.get("LOCALAPPDATA", ROOT)) / "spud" / "audit" / stamp
    out.mkdir(parents=True, exist_ok=True)
    # Actors stand still: what they're doing is game state, like the weather and the hour, not history in the image.
    head = f"world tamriel\nresolution {RES[0]} {RES[1]}\ndraw_distance 12\nset hold_actors 1\n"

    passes = (("history", "set hold_effect_state 1\n", HISTORY_FRAMES), ("settled", "", SETTLED_FRAMES))
    try:
        for tag, setup, frames in passes:
            # One process visits every pose, arriving by flight from a few cells away, so each frame it
            # captures has a long history behind it.
            warm = head + setup
            first = True
            for name, (_, z, yaw, pitch, approach) in POSES.items():
                ax, ay = cell(*approach)
                warm += f"{'start' if first else 'teleport'} {ax} {ay} +{z} {yaw} {pitch}\nwait_loaded\n"
                first = False
                warm += pose_line("fly", name) + f" 60\nwait_loaded\nwait_frames {frames}\n"
                warm += f"screenshot {out / (name + f'.{tag}.warm.png')}\nwait_frames 30\n"
                warm += f"screenshot {out / (name + f'.{tag}.warm-later.png')}\n"
            run(exe, warm, out / f"{tag}.warm.osc", args.cache)
            for name in POSES:
                cold = head + setup + pose_line("start", name) + f"\nwait_frames {frames}\n"
                cold += f"screenshot {out / (name + f'.{tag}.cold.png')}\n"
                run(exe, cold, out / f"{name}.{tag}.cold.osc", args.cache)
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    for tag, _, frames in passes:
        print(f"{tag} checks ({RES[0]}x{RES[1]}, {frames} frames, results: {out}):")
        for name in POSES:
            warm_png = out / f"{name}.{tag}.warm.png"
            stable = compare(warm_png, out / f"{name}.{tag}.warm-later.png", out / f"{name}.{tag}.stable-diff.png")
            history = compare(warm_png, out / f"{name}.{tag}.cold.png", out / f"{name}.{tag}.history-diff.png")
            checks = (("not stable at rest", stable), ("depends on how the camera got there", history))
            ok = not any(result and result[0] for _, result in checks)
            print(f"  {name:<18} {'PASS' if ok else 'FAIL'}")
            for what, result in checks:
                if result:
                    over, text = result
                    print(f"    - {what}: {text}{'' if over else ' (within the limit)'}")
                    if over:
                        failures.append(f"{name}: {what} ({tag}): {text}")

    print("\nRESULT:", "FAIL" if failures else "PASS")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
