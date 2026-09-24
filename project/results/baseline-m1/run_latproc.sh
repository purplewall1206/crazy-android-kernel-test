#!/bin/bash
# run_latproc.sh — lmbench lat_proc fork / fork+exec / shell runner
# (paper §6.4/§6.2 context: process-creation latencies, Figure 20).
#
# usage: run_latproc.sh <outdir>
#   env: SKIP_TIMEGATE=1  bypass token-window gate (host smoke ONLY)
#        LAT_PROC=/path/to/lat_proc   override binary detection
#
# Binary detection order (lmbench installs into an arch-specific dir):
#   1. $LAT_PROC
#   2. `which lat_proc`
#   3. ls /usr/lib/lmbench/bin/*/lat_proc   (Debian layout)
#
# lat_proc is a pure latency benchmark and needs no memory-size
# calibration; each op (fork / exec / shell) is run 3 times and the
# median "N microseconds" line is parsed.  If the binary is missing the
# summary records skipped=true and the script exits 0 (so the night loop
# can proceed); per-run raw stdout is kept under raw/.
set -u
cd "$(dirname "$0")" || exit 1

if [ "${SKIP_TIMEGATE:-0}" != "1" ]; then
	bash /home/ppw/cortenmm/bin/timegate.sh || exit 1
else
	echo "[run_latproc] SKIP_TIMEGATE=1: timegate bypassed (smoke test)" >&2
fi

OUT=${1:?usage: run_latproc.sh <outdir>}
REPS=3
mkdir -p "$OUT/raw"

LP=""
if [ -n "${LAT_PROC:-}" ] && [ -x "$LAT_PROC" ]; then
	LP=$LAT_PROC
else
	LP=$(command -v lat_proc || true)
	if [ -z "$LP" ]; then
		for c in /usr/lib/lmbench/bin/*/lat_proc; do
			[ -x "$c" ] && LP=$c && break
		done
	fi
fi

if [ -z "$LP" ]; then
	echo "[run_latproc] lat_proc not found (apt: lmbench; or set LAT_PROC=...)" >&2
	printf '{"bench_set":"lat_proc","skipped":true,"reason":"lat_proc binary not found"}\n' \
		>"$OUT/latproc_summary.json"
	exit 0
fi
echo "[run_latproc] using lat_proc: $LP" >&2

# lmbench's "shell" op execs a path compiled into the binary
# (Debian: /var/tmp/lmbench/hello).  The package ships nothing there, so
# build a tiny no-op hello first, otherwise 'shell' would measure /bin/sh
# printing "hello: not found" instead of the intended fork+exec+sh -c.
HELLO=/var/tmp/lmbench/hello
if [ ! -x "$HELLO" ]; then
	mkdir -p /var/tmp/lmbench 2>/dev/null || true
	if command -v gcc >/dev/null; then
		printf 'int main(void){return 0;}\n' >/var/tmp/lmbench/hello.c
		gcc -O2 -o "$HELLO" /var/tmp/lmbench/hello.c 2>/dev/null || true
	fi
	[ -x "$HELLO" ] || cp /bin/true "$HELLO" 2>/dev/null || true
	if [ -x "$HELLO" ]; then
		echo "[run_latproc] built $HELLO for the shell op" >&2
	else
		echo "[run_latproc] WARNING: cannot create $HELLO; shell op will include sh error noise" >&2
	fi
fi

# usage sanity check first (also documents which ops this build supports)
"$LP" 2>&1 | head -5 >&2 || true

parse_us() {	# extract the "N.NN microseconds" latency from lat_proc output
	sed -n 's/^[^:]*: \([0-9.]*\) microseconds$/\1/p' | tail -1
}

for op in fork exec shell; do
	for k in $(seq 1 "$REPS"); do
		if "$LP" "$op" >"$OUT/raw/lat_proc_${op}_run$k.txt" 2>&1; then
			us=$(parse_us <"$OUT/raw/lat_proc_${op}_run$k.txt")
			echo "[run_latproc] $op run$k: ${us:-PARSE_FAIL} us"
			if [ -z "$us" ]; then
				echo "[run_latproc] WARNING: could not parse latency for $op run$k" >&2
			fi
		else
			echo "[run_latproc] $op run$k FAILED" >&2
			echo "FAILED" >"$OUT/raw/lat_proc_${op}_run$k.txt"
		fi
	done
done

python3 - "$OUT" "$LP" <<'PYEOF'
import glob, json, os, re, socket, subprocess, sys, datetime

out, lp = sys.argv[1], sys.argv[2]
ops = {}
for op in ("fork", "exec", "shell"):
    vals = []
    for p in sorted(glob.glob(os.path.join(out, "raw", f"lat_proc_{op}_run*.txt"))):
        txt = open(p).read()
        m = re.findall(r":\s*([0-9.]+)\s*microseconds", txt)
        if m:
            vals.append(float(m[-1]))
    ops[op] = {
        "runs_us": vals,
        "median_us": sorted(vals)[len(vals) // 2] if vals else None,
    }

summary = {
    "bench_set": "lmbench lat_proc (fork / fork+exec / shell)",
    "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
    "host": {"hostname": socket.gethostname(),
             "kernel": subprocess.run(["uname", "-r"], capture_output=True,
                                      text=True).stdout.strip(),
             "ncpu": os.cpu_count()},
    "lat_proc_path": lp,
    "ops": ops,
}
with open(os.path.join(out, "latproc_summary.json"), "w") as f:
    json.dump(summary, f, indent=1)
    f.write("\n")
print("[run_latproc] summary -> " + os.path.join(out, "latproc_summary.json"))
PYEOF
