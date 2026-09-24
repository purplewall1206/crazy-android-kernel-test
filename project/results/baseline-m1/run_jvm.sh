#!/bin/bash
# run_jvm.sh — CortenMM paper §6.4 "JVM thread creation" runner.
#
# usage: run_jvm.sh <outdir>
#   env: SKIP_TIMEGATE=1  bypass token-window gate (host smoke ONLY)
#        JT_THREADS=N     threads per rep (default 2000, paper mimics
#                         the Android app-startup case)
#        JT_REPS=R        reps inside one JVM (default 3)
#
# Compiles JThreadBench.java, then runs it in 3 INDEPENDENT JVM processes
# (java -Xmx512m), each printing one JSON line; summary jvm_summary.json
# carries the three raw results plus their median median_ms.
#
# Wall-clock budget: ~1-3 min (JVM startup + 3 x 3 reps x 2000 threads).
set -u
cd "$(dirname "$0")" || exit 1

if [ "${SKIP_TIMEGATE:-0}" != "1" ]; then
	bash /home/ppw/cortenmm/bin/timegate.sh || exit 1
else
	echo "[run_jvm] SKIP_TIMEGATE=1: timegate bypassed (smoke test)" >&2
fi

OUT=${1:?usage: run_jvm.sh <outdir>}
T=${JT_THREADS:-2000}
R=${JT_REPS:-3}
mkdir -p "$OUT/raw"

command -v javac >/dev/null || { echo "[run_jvm] javac not found (apt: openjdk-21-jdk-headless on guest)" >&2; exit 3; }
command -v java >/dev/null || { echo "[run_jvm] java not found" >&2; exit 3; }

echo "[run_jvm] compiling JThreadBench.java ($(java -version 2>&1 | head -1))" >&2
javac -O JThreadBench.java || exit 3

for k in 1 2 3; do
	if java -Xmx512m JThreadBench "$T" "$R" >"$OUT/raw/jvm_run$k.json" 2>"$OUT/raw/jvm_run$k.err"; then
		echo "[run_jvm] JVM process $k done -> raw/jvm_run$k.json"
	else
		echo "[run_jvm] JVM process $k FAILED (see raw/jvm_run$k.err)" >&2
		exit 1
	fi
done

python3 - "$OUT" "$T" "$R" <<'PYEOF'
import glob, json, os, socket, subprocess, sys, datetime

out, t, r = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
runs = []
for p in sorted(glob.glob(os.path.join(out, "raw", "jvm_run*.json"))):
    with open(p) as f:
        runs.append(json.load(f))
meds = sorted(x["median_ms"] for x in runs)
summary = {
    "bench_set": "jvm-thread-create (paper section 6.4)",
    "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
    "host": {"hostname": socket.gethostname(),
             "kernel": subprocess.run(["uname", "-r"], capture_output=True,
                                      text=True).stdout.strip(),
             "ncpu": os.cpu_count()},
    "threads": t, "reps_per_jvm": r,
    "median_ms": meds[len(meds) // 2] if meds else None,
    "runs": runs,
}
with open(os.path.join(out, "jvm_summary.json"), "w") as f:
    json.dump(summary, f, indent=1)
    f.write("\n")
print("[run_jvm] summary -> " + os.path.join(out, "jvm_summary.json"))
PYEOF
