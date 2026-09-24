#!/bin/bash
# run_g5.sh — G5 gate: lmbench lat_proc fork / fork+exec / shell, BASE vs MODE
# (M5.T4, on r07-t5final HEAD 2639d3294b9d, faithful-fork semantics).
#
# Method mirrors the M1 baseline runner (results/r01/baseline/run_latproc.sh):
# lat_proc is a pure latency benchmark; each op run once per rep, 3 reps,
# median parsed from the "N microseconds" line.  Arms are interleaved
# rep-by-rep to cancel drift.  MODE arm = LD_PRELOAD constructor hook with
# CORTEN_MODE_HOOK_STRICT=1; a missing "MODE on" marker aborts the arm.
#
# usage: run_g5.sh <outdir>
set -u
OUT=${1:?usage: run_g5.sh <outdir>}
REPS=3
LP=/usr/lib/lmbench/bin/x86_64-linux-gnu/lat_proc
HOOK=/mnt/hostshare/g5gate/corten_mode_hook.so
ARENA=/sys/kernel/debug/corten/arena_stats
mkdir -p "$OUT/raw"

[ -x "$LP" ] || { echo "FATAL: lat_proc missing at $LP"; exit 1; }
[ -f "$HOOK" ] || { echo "FATAL: hook missing at $HOOK"; exit 1; }
HELLO=/var/tmp/lmbench/hello
if [ ! -x "$HELLO" ]; then
	mkdir -p /var/tmp/lmbench
	printf 'int main(void){return 0;}\n' > /var/tmp/lmbench/hello.c
	gcc -O2 -o "$HELLO" /var/tmp/lmbench/hello.c
fi
[ -x "$HELLO" ] || cp /bin/true "$HELLO"

parse_us() { sed -n 's/^[^:]*: \([0-9.]*\) microseconds$/\1/p' | tail -1; }

cp "$ARENA" "$OUT/arena_stats.before" 2>/dev/null || true

for rep in $(seq 1 "$REPS"); do
	for op in fork exec shell; do
		# BASE arm
		if "$LP" "$op" >"$OUT/raw/base_${op}_rep${rep}.txt" 2>&1; then
			us=$(parse_us <"$OUT/raw/base_${op}_rep${rep}.txt")
			echo "[g5] base  $op rep${rep}: ${us:-PARSE_FAIL} us"
		else
			echo "[g5] base  $op rep${rep}: RUN FAILED"; cp "$OUT/raw/base_${op}_rep${rep}.txt" /dev/null
		fi
		# MODE arm (STRICT: aborted process => non-zero rc / no marker)
		if LD_PRELOAD="$HOOK" CORTEN_MODE_HOOK_STRICT=1 "$LP" "$op" \
			>"$OUT/raw/mode_${op}_rep${rep}.txt" 2>"$OUT/raw/mode_${op}_rep${rep}.err"; then
			if ! grep -q "corten_mode_hook: MODE on" "$OUT/raw/mode_${op}_rep${rep}.err"; then
				echo "[g5] FATAL: MODE marker missing for $op rep${rep} (silently no-op preload)"; exit 1
			fi
			if grep -q "fork-probe FAILED" "$OUT/raw/mode_${op}_rep${rep}.err"; then
				echo "[g5] FATAL: hook fork-probe FAILED for $op rep${rep}"; exit 1
			fi
			us=$(parse_us <"$OUT/raw/mode_${op}_rep${rep}.txt")
			echo "[g5] mode  $op rep${rep}: ${us:-PARSE_FAIL} us"
		else
			rc=$?
			echo "[g5] FATAL: mode $op rep${rep} rc=$rc (STRICT ENTER failed?)"
			cat "$OUT/raw/mode_${op}_rep${rep}.err" 2>/dev/null | head -5
			exit 1
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
        for p in sorted(glob.glob(os.path.join(out, "raw", f"{arm}_{op}_rep*.txt"))):
            m = re.findall(r":\s*([0-9.]+)\s*microseconds", open(p).read())
            if m:
                vals.append(float(m[-1]))
        med = sorted(vals)[len(vals) // 2] if vals else None
        res[f"{arm}.{op}"] = {"runs_us": vals, "median_us": med}

summary = {"g5_gate": "lmbench lat_proc fork/fork+exec/shell, BASE vs MODE (LD_PRELOAD hook, STRICT)",
           "ops": res}
print("\n== G5 summary (median us) ==")
for op in ("fork", "exec", "shell"):
    b = res[f"base.{op}"]["median_us"]; m = res[f"mode.{op}"]["median_us"]
    if b and m:
        delta = (m - b) / b * 100.0
        print(f"{op:6s} base={b:9.2f} mode={m:9.2f} delta={delta:+7.2f}%  (base runs={res[f'base.{op}']['runs_us']}, mode runs={res[f'mode.{op}']['runs_us']})")
json.dump(summary, open(os.path.join(out, "g5_summary.json"), "w"), indent=1)
print("summary ->", os.path.join(out, "g5_summary.json"))
PYEOF
echo "[g5] done"
