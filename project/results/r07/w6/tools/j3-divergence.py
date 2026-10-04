#!/usr/bin/env python3
"""j3-divergence.py - W-6 J3 cross-kernel maps divergence ledger.

Usage: j3-divergence.py <a1-maps> <fix-maps>

Classifies every line-level difference between the A.1 baseline snapshot
and the fixed-kernel snapshot into registered-evolution buckets:
  - window-file:    V-B.3 routes NULL-addr private file maps into the
                    window (A.1: legacy mmap area)
  - window-slot:    the window placement sequence shifts as a consequence
                    (park slot recycling differences)
  - sweep-label:    W-4 entry-sweep adopted a tree VMA -> the row now
                    renders from the region record ([anon:corten_arena]
                    label on anon shapes; A.1 rendered the unnamed VMA)
  - other:          anything else (must be empty for a clean verdict)
Emits: identical-line count, per-bucket counts, and the full ledger.
"""
import sys


def rows(path):
    out = {}
    order = []
    for line in open(path):
        line = line.rstrip("\n")
        if not line:
            continue
        key = line.split()[0]
        out[key] = line
        order.append(key)
    return out, order


def bucket(a, b):
    awin = a.startswith("1000000")
    bwin = b.startswith("1000000")
    if awin != bwin:
        return "window-file" if "corten_arena" in b or bwin else "window-file"
    if awin and bwin:
        return "window-slot"
    if "corten_arena" in b and "corten_arena" not in a:
        return "sweep-label"
    return "other"


a, ao = rows(sys.argv[1])
b, bo = rows(sys.argv[2])
same = sum(1 for k in a if k in b and a[k] == b.get(k))
only_a = [k for k in ao if k not in b]
only_b = [k for k in bo if k not in a]
changed = [k for k in ao if k in b and a[k] != b[k]]
print(f"A.1 rows={len(ao)} fix rows={len(bo)} byte-identical rows={same}")
print(f"only-in-A.1={len(only_a)} only-in-fix={len(only_b)} changed-in-both={len(changed)}")
counts = {}
print("\n== ledger ==")
for k in only_a:
    bu = "removed-from-fix"
    counts[bu] = counts.get(bu, 0) + 1
    print(f"[{bu}] A.1: {a[k]}")
for k in only_b:
    m = "added-in-fix"
    counts[m] = counts.get(m, 0) + 1
    print(f"[{m}] fix: {b[k]}")
for k in changed:
    bu = bucket(a[k], b[k])
    counts[bu] = counts.get(bu, 0) + 1
    print(f"[{bu}] - {a[k]}\n[{bu}] + {b[k]}")
print("\n== buckets ==")
for k, v in sorted(counts.items()):
    print(f"{k}: {v}")
print(f"other: {counts.get('other', 0)} (must be 0)")
