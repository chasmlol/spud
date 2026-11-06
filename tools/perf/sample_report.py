#!/usr/bin/env python3
"""Where the main thread's time went, from a run with SPUD_SAMPLE=<file> (src/core/sampler.hpp).

    sample_report.py <file> [--exe build/release/bin/spud.exe] [--tag N] [--top 40] [--under NAME]

Prints functions by inclusive and by self share of samples. --tag keeps one section of a tour; --under keeps samples
with that function (substring) on their stack, so a stall can be followed down.
"""

import argparse
import collections
import ctypes
import struct
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class Symbol(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.ULONG), ("TypeIndex", wintypes.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wintypes.ULONG), ("Size", wintypes.ULONG), ("ModBase", ctypes.c_uint64),
                ("Flags", wintypes.ULONG), ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64),
                ("Register", wintypes.ULONG), ("Scope", wintypes.ULONG), ("Tag", wintypes.ULONG),
                ("NameLen", wintypes.ULONG), ("MaxNameLen", wintypes.ULONG), ("Name", ctypes.c_char * 512)]


def symboliser(exe, base):
    dbghelp = ctypes.WinDLL("dbghelp")
    process = wintypes.HANDLE(0x1234)
    dbghelp.SymSetOptions(0x2 | 0x10)  # undecorated names, line info off is fine
    dbghelp.SymInitialize(process, str(exe.parent).encode(), False)
    dbghelp.SymLoadModuleEx.restype = ctypes.c_uint64
    dbghelp.SymLoadModuleEx.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.c_char_p, ctypes.c_char_p,
                                        ctypes.c_uint64, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
    dbghelp.SymLoadModuleEx(process, None, str(exe).encode(), None, base, 0, None, 0)
    dbghelp.SymFromAddr.argtypes = [wintypes.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                                    ctypes.POINTER(Symbol)]
    cache = {}

    def name(address):
        if address not in cache:
            symbol = Symbol()
            symbol.SizeOfStruct = 88
            symbol.MaxNameLen = 500
            offset = ctypes.c_uint64()
            ok = dbghelp.SymFromAddr(process, address, ctypes.byref(offset), ctypes.byref(symbol))
            cache[address] = symbol.Name.decode(errors="replace") if ok else f"{address:#x}"
        return cache[address]

    return name


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("file")
    parser.add_argument("--exe", default=str(ROOT / "build" / "release" / "bin" / "spud.exe"))
    parser.add_argument("--tag", type=int)
    parser.add_argument("--top", type=int, default=40)
    parser.add_argument("--under")
    args = parser.parse_args()
    data = Path(args.file).read_bytes()
    words = struct.unpack(f"<{len(data) // 8}Q", data)
    name = symboliser(Path(args.exe).resolve(), words[0])
    inclusive, own, total, i, tags = collections.Counter(), collections.Counter(), 0, 1, collections.Counter()
    while i < len(words):
        depth, tag = words[i] & 0xFFFFFFFF, words[i] >> 32
        stack = [name(a) for a in words[i + 1:i + 1 + depth]]
        i += 1 + depth
        tags[tag] += 1
        if args.tag is not None and tag != args.tag:
            continue
        if args.under:
            hits = [k for k, s in enumerate(stack) if args.under in s]
            if not hits:
                continue
            stack = stack[:hits[-1] + 1]
        total += 1
        own[stack[0] if stack else "?"] += 1
        for s in set(stack):
            inclusive[s] += 1
    print(f"{total} samples (about {total / 1000:.1f} s); by tag: {dict(sorted(tags.items()))}")
    for title, table in (("inclusive", inclusive), ("self", own)):
        print(f"\n{title}:")
        for s, n in table.most_common(args.top):
            print(f"{100 * n / max(total, 1):6.1f}%  {n:7d}  {s[:110]}")


if __name__ == "__main__":
    main()
