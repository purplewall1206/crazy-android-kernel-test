#!/usr/bin/env python3
# analyze_g1final.py - M4.T5 five-round G1 consolidation analysis (M8 gate).
# Mirrors analyze_t5.py semantics: median per arm, delta normalized so >0
# always means "T0 better", CV = pstdev/mean (EVAL sec 2.3).
import json, glob, os, statistics, re

RAW = os.path.join(os.path.dirname(os.path.abspath(__file__)), "raw")

def load(pat):
    out = []
    for f in sorted(glob.glob(os.path.join(RAW, pat))):
        with open(f) as fh:
            out.append(json.load(fh))
    return out

def med(xs):
    return statistics.median(float(x) for x in xs)

def cv(xs):
    xs = [float(x) for x in xs]
    return statistics.pstdev(xs) / statistics.mean(xs) * 100.0 if len(xs) > 1 and statistics.mean(xs) else 0.0

# ---------------- mmbench cells: 5 benches x low x {t4,t8} x 5 rounds
cells = {}
for f in sorted(glob.glob(os.path.join(RAW, "mmbench_*_run*.json"))):
    r = json.load(open(f))
    m = re.match(r"mmbench_(.+)_run(\d+)\.(base|t0)\.json$", os.path.basename(f))
    if not m:
        continue
    key = (m.group(1),)  # e.g. unmap-virt_low_t4
    cells.setdefault(key, {"base": {}, "t0": {}})
    cells[key][m.group(3)][int(m.group(2))] = r

print("== mmbench cells (ops_per_us, higher better; delta>0 = T0 better) ==")
rows = []
for key in sorted(cells):
    b, t0 = cells[key]["base"], cells[key]["t0"]
    name = key[0]
    bs = [b[k]["ops_per_us"] for k in sorted(b)]
    ts = [t0[k]["ops_per_us"] for k in sorted(t0)]
    paired = [(t0[k]["ops_per_us"] - b[k]["ops_per_us"]) / b[k]["ops_per_us"] * 100.0
              for k in sorted(b) if k in t0]
    bm, tm = med(bs), med(ts)
    delta = (tm - bm) / bm * 100.0
    rows.append((name, bs, ts, bm, tm, delta, cv(bs), cv(ts), paired))
    pos = sum(1 for p in paired if p > 0)
    print(f"{name:22s} base5={['%.6g' % x for x in bs]}")
    print(f"{'':22s} t0_5  ={['%.6g' % x for x in ts]}")
    print(f"{'':22s} med base={bm:.6g} (CV {cv(bs):.1f}%)  t0={tm:.6g} (CV {cv(ts):.1f}%)  "
          f"delta={delta:+.2f}%  paired={[f'{p:+.2f}' for p in paired]} (pos {pos}/5)")

print()
print("== G1 gate (M8/EVAL sec 6): item qualifies when BOTH low t4 & t8 delta >= +10% ==")
g1 = {}
dmap = {(r[0].split("_")[0], r[0].rsplit("_", 1)[1]): r[5] for r in rows}
g1rows = []
for bench in ("mmap-pf", "pf", "unmap", "unmap-virt"):
    d4 = dmap.get((bench, "t4"))
    d8 = dmap.get((bench, "t8"))
    q = (d4 is not None and d8 is not None and min(d4, d8) >= 10.0)
    g1rows.append((bench, d4, d8, q))
    fmt = lambda v: f"{v:+.2f}%" if v is not None else "n/a"
    print(f"{bench:10s} t4={fmt(d4)}  t8={fmt(d8)}  qualifies={q}")
nq = sum(1 for r in g1rows if r[3])
print(f"G1: {nq}/4 qualifying -> {'MET' if nq >= 2 else 'NOT MET'}")
mm = next((r for r in rows if r[0] == "mmap_low_t4"), None)
mm8 = next((r for r in rows if r[0] == "mmap_low_t8"), None)
if mm and mm8:
    print(f"(informational) mmap low t4={mm[5]:+.2f}%  t8={mm8[5]:+.2f}%")

# ---------------- apps
print()
print("== apps (G3 recheck, x1 per arm; delta>0 = T0 better) ==")
def app(pat, field, higher, label):
    bs, ts = load(pat + ".base.json"), load(pat + ".t0.json")
    bv = [x[field] for x in bs]
    tv = [x[field] for x in ts]
    b, t = med(bv), med(tv)
    delta = (t - b) / b * 100.0 if higher else (b - t) / b * 100.0
    print(f"{label:34s} base={b:.4g} t0={t:.4g} delta={delta:+.2f}% "
          f"(base={[f'{v:.4g}' for v in bv]} t0={[f'{v:.4g}' for v in tv]}) rc="
          f"{[x['rc'] for x in bs + ts]}")
    return delta

app("psearchy_eq_run1", "seconds", False, "psearchy_eq t8 (s, lower better)")
app("metis_eq_run1", "seconds", False, "metis_eq t8 (s, lower better)")
app("dedup_eq_run1", "blocks_per_s", True, "dedup_eq t8 glibc (blk/s)")
app("dedup_eq_tcmalloc_run1", "blocks_per_s", True, "dedup_eq t8 tcmalloc (blk/s)")

print()
print("== JThreadBench (CFE regression watch) ==")
for arm in ("base", "t0"):
    for f in sorted(glob.glob(os.path.join(RAW, f"jtb_run1.{arm}.json"))):
        r = json.load(open(f))
        keys = {k: r[k] for k in r if k not in ("arm", "utc", "rc", "argv")}
        print(f"{arm}: rc={r['rc']} {json.dumps(keys)[:260]}")
    err = os.path.join(RAW, f"jtb_run1.{arm}.err")
    if os.path.exists(err):
        txt = open(err, errors="replace").read()
        cfe = txt.count("ClassFormatError") + txt.count("ClassFormat")
        print(f"{arm}: stderr ClassFormat hits={cfe} stderr_bytes={len(txt)}")

# ---------------- debugfs counters
print()
print("== arena_stats deltas ==")
def counters(path):
    d = {}
    for line in open(path):
        parts = line.split()
        if len(parts) == 2:
            try:
                d[parts[0]] = int(parts[1])
            except ValueError:
                pass
    return d
before = counters(os.path.join(os.path.dirname(RAW), "arena_stats.before"))
after = counters(os.path.join(os.path.dirname(RAW), "arena_stats.after"))
mid = counters(os.path.join(os.path.dirname(RAW), "arena_stats.midpoint"))
watch = ("auto_mmaps", "munmap_releases", "pool_parks", "pool_hits", "pool_misses",
         "pool_over", "pool_ejects", "mprotect_routes", "fork_faithful", "drain_timeout",
         "va_recycles", "seg_claims", "mag_skips", "shrink_scans", "shrink_swapped",
         "ejects", "auto_attach_fail", "auto_fallbacks", "meta_arrays", "ptdescs")
print(f"{'counter':20s} {'before':>12s} {'mid(unmap-virt+unmap)':>22s} {'after':>12s}")
for k in watch:
    if k in after or k in before:
        print(f"{k:20s} {before.get(k,-1):12d} {mid.get(k,-1):22d} {after.get(k,-1):12d}")

# ---------------- paired JSON seeds sanity (same seed both arms = paired units)
print()
print("== seed pairing audit (base vs t0 same round) ==")
bad = 0
for key in cells:
    b, t0 = cells[key]["base"], cells[key]["t0"]
    for k in b:
        if k in t0 and b[k]["seed"] != t0[k]["seed"]:
            print("SEED MISMATCH", key, k)
            bad += 1
print(f"seed mismatches: {bad} (pairs checked: {sum(len(v['base']) for v in cells.values())})")
