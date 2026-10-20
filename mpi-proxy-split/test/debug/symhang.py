#!/usr/bin/env python3
"""Symbolizes hangtrace output: '+0xOFF in /path' -> function (file offset -> vaddr via PT_LOAD)."""
import re, subprocess, sys
from collections import defaultdict
LOC = re.compile(r"= \+0x([0-9a-f]+) in (/\S+)")
loads = {}
def vaddr(path, off):
    if path not in loads:
        segs = []
        try:
            out = subprocess.run(["readelf", "-lW", path], capture_output=True, text=True).stdout
            for l in out.splitlines():
                f = l.split()
                if f and f[0] == "LOAD":
                    segs.append((int(f[1], 16), int(f[2], 16), int(f[4], 16)))
        except OSError:
            pass
        loads[path] = segs
    for o, v, sz in loads[path]:
        if o <= off < o + sz:
            return off - o + v
    return None
lines = open(sys.argv[1]).read().splitlines()
want = defaultdict(set)
for l in lines:
    m = LOC.search(l)
    if m:
        p, o = m.group(2), int(m.group(1), 16)
        v = vaddr(p, o)
        if v is not None:
            want[p].add(v)
name = {}
for p, vs in want.items():
    vs = sorted(vs)
    out = subprocess.run(["addr2line", "-f", "-C", "-i", "-e", p] + [hex(v) for v in vs],
                         capture_output=True, text=True).stdout.splitlines()
    # -i can add inline frames; without it the output is 2 lines per address
    out = subprocess.run(["addr2line", "-f", "-C", "-e", p] + [hex(v) for v in vs],
                         capture_output=True, text=True).stdout.splitlines()
    for i, v in enumerate(vs):
        fn, src = out[2 * i], out[2 * i + 1]
        name[(p, v)] = "%s %s" % (fn, src.split("/")[-1])
for l in lines:
    m = LOC.search(l)
    if m:
        p, o = m.group(2), int(m.group(1), 16)
        v = vaddr(p, o)
        n = name.get((p, v), "?")
        l = "%s  [%s %s]" % (l, p.split("/")[-1], n)
    print(l)
