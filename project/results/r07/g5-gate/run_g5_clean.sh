#!/bin/bash
# run_g5_clean.sh — G5 gate clean re-run (run2) after contamination diagnosis.
#
# run1 findings (archived as run1-*): the hook's atexit fork-probe is inherited
# by every fork child (atexit list copied by fork), so each measured lat_proc
# child performed an extra fork+waitpid+fprintf at exit; stderr also went to a
# 9p file (synchronous writes).  Both contaminate latency; fixed here by:
#   1. NOPROBE hook variant (probe stays covered by T0-DoD runs, not latency)
#   2. symmetric stdout+stderr redirection to guest-local /tmp (both arms)
# The ENTER/GET STRICT assertion is unchanged (marker still asserted per run).
#
# usage: run_g5_clean.sh <outroot>   (writes <outroot>/run2/)
set -u
ROOT=${1:?usage: run_g5_clean.sh <outroot>}
OUT=$ROOT/run2
REPS=3
LP=/usr/lib/lmbench/bin/x86_64-linux-gnu/lat_proc
HOOK=/mnt/hostshare/g5gate/corten_mode_hook_noprobe.so
ARENA=/sys/kernel/debug/corten/arena_stats
WORK=/tmp/g5run2
mkdir -p "$OUT/raw" "$WORK"

[ -x "$LP" ] || { echo "FATAL: lat_proc missing"; exit 1; }
[ -f "$HOOK" ] || { echo "FATAL: noprobe hook missing"; exit 1; }
HELLO=/var/tmp/lmbench/hello
[ -x "$HELLO" ] || { mkdir -p /var/tmp/lmbench; printf 'int main(void){return 0;}\n' >/var/tmp/lmbench/hello.c; gcc -O2 -o "$HELLO" /var/tmp/lmbench/hello.c; }

parse_us() { grep -h "microseconds" "$1" | sed -n 's/^[^:]*: \([0-9.]*\) microseconds$/\1/p' | tail -1; }

cp "$ARENA" "$OUT/arena_stats.before" 2>/dev/null || true

for rep in $(seq 1 "$REPS"); do
	for op in fork exec shell; do
		if env -u LD_PRELOAD "$LP" "$op" >"$WORK/base_${op}_${rep}.out" 2>&1; then
			us=$(parse_us "$WORK/base_${op}_${rep}.out")
			echo "[g5c] base $op rep${rep}: ${us:-PARSE_FAIL} us"
			cp "$WORK/base_${op}_${rep}.out" "$OUT/raw/"
		else
			echo "[g5c] FATAL base $op rep${rep}"; exit 1
		fi
		if LD_PRELOAD="$HOOK" CORTEN_MODE_HOOK_STRICT=1 "$LP" "$op" \
			>"$WORK/mode_${op}_${rep}.out" 2>"$WORK/mode_${op}_${rep}.err"; then
			grep -q "corten_mode_hook: MODE on" "$WORK/mode_${op}_${rep}.err" || { echo "[g5c] FATAL: marker missing $op rep${rep}"; exit 1; }
			us=$(parse_us "$WORK/mode_${op}_${rep}.err")
			echo "[g5c] mode $op rep${rep}: ${us:-PARSE_FAIL} us"
			cp "$WORK/mode_${op}_${rep}.err" "$OUT/raw/"
		else
			rc=$?; echo "[g5c] FATAL mode $op rep${rep} rc=$rc"; head -5 "$WORK/mode_${op}_${rep}.err"; exit 1
		fi
	done
done

cp "$ARENA" "$OUT/arena_stats.after" 2>/dev/null || true
uname -r > "$OUT/uname.txt"

python3 - "$OUT" <<'PYEOF'
import glob, json, os, re, sys
out = sys.argv[1]
res = {}
for arm in ("base", "mode"):
    for op in ("fork", "exec", "shell"):
        vals = []
        for p in sorted(glob.glob(os.path.join(out, "raw", f"{arm}_{op}_rep?*.out")) + glob.glob(os.path.join(out, "raw", f"{arm}_{op}_rep?.err"))):
            m = re.findall(r":\s*([0-9.]+)\s*microseconds", open(p).read())
            if m: vals.append(float(m[-1]))
        med = sorted(vals)[len(vals)//2] if vals else None
        res[f"{arm}.{op}"] = {"runs_us": vals, "median_us": med}
print("\n== G5 run2 (clean) summary ==")
for op in ("fork","exec","shell"):
    b = res[f"base.{op}"]["median_us"]; m = res[f"mode.{op}"]["median_us"]
    if b and m:
        print(f"{op:6s} base={b:9.2f} mode={m:9.2f} delta={(m-b)/b*100.0:+7.2f}%  base={res[f'base.{op}']['runs_us']} mode={res[f'mode.{op}']['runs_us']}")
json.dump({"g5_gate_run2_clean":"lmbench lat_proc BASE vs MODE (noprobe hook, STRICT, /tmp stderr)",
           "ops":res}, open(os.path.join(out,"g5_summary.json"),"w"), indent=1)
PYEOF
echo "[g5c] done"
