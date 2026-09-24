#!/bin/bash
# run_apps.sh — runner for the CortenMM paper §6.4 "real application"
# equivalent workloads (metis_eq / dedup_eq / psearchy_eq), STATE.md D7.
#
# usage: run_apps.sh <outdir> <text1.6G> <text368M>
#   env: SKIP_TIMEGATE=1   bypass the token-window gate (host smoke tests ONLY)
#        THREADS=N         thread count for the map/index/churn apps (default 8)
#        DEDUP_SECONDS=S   dedup_eq nominal duration (default 20)
#
# Schedule (paper §6.4: metis on a 1.6GB text, psearchy on a ~368MB corpus,
# dedup with ptmalloc vs tcmalloc):
#   metis_eq    8 threads x 3 runs   -> raw/metis_eq_run{k}.json
#   psearchy_eq 8 threads x 3 runs   -> raw/psearchy_eq_run{k}.json
#   dedup_eq    8 threads 20s x 3    -> raw/dedup_eq_run{k}.json
#   dedup_eq + tcmalloc x 3 (only if a libtcmalloc is found)
#                                     -> raw/dedup_eq_tcmalloc_run{k}.json
# Summary (median run + all raw runs) -> <outdir>/apps_summary.json
#
# Wall-clock budget: ~2-4 min on a fast 16-core host; budget ~10-20 min on
# the 8 vCPU guest.
set -u
cd "$(dirname "$0")" || exit 1
APP_DIR="$(pwd)"

if [ "${SKIP_TIMEGATE:-0}" != "1" ]; then
	bash /home/ppw/cortenmm/bin/timegate.sh || exit 1
else
	echo "[run_apps] SKIP_TIMEGATE=1: timegate bypassed (smoke test)" >&2
fi

OUT=${1:?usage: run_apps.sh <outdir> <text1.6G> <text368M>}
T1600=${2:?missing 1.6GB text}
T368=${3:?missing 368MB text}
THREADS=${THREADS:-8}
DSECS=${DEDUP_SECONDS:-20}
SEED=42
REPS=3

for f in "$T1600" "$T368"; do
	[ -r "$f" ] || { echo "[run_apps] cannot read $f" >&2; exit 2; }
done
mkdir -p "$OUT/raw"

# ---- build if needed (guest has gcc14; host gcc11 works too) -----------
build() {
	local b=$1 src=$2
	if [ ! -x "$b" ]; then
		echo "[run_apps] building $b" >&2
		gcc -O2 -Wall -Wextra -o "$b" "$src" -lpthread || exit 3
	fi
}
build metis_eq metis_eq.c
build dedup_eq dedup_eq.c
build psearchy_eq psearchy_eq.c

run_json() {	# run_json <binary> <args...> <outfile>
	local out=${@: -1}
	local -a a=("${@:1:$#-1}")
	if "${a[@]}" >"$out" 2>"$out.err"; then
		echo "[run_apps] $(basename "${a[0]}") done -> $out"
	else
		echo "[run_apps] FAILED: ${a[*]} (see $out.err)" >&2
		return 1
	fi
}

# ---- metis_eq (paper: map-reduce on a 1.6 GB text file, 8 workers) -----
for k in $(seq 1 "$REPS"); do
	run_json ./metis_eq "$THREADS" "$T1600" "$OUT/raw/metis_eq_run$k.json" || exit 1
done

# ---- psearchy_eq (paper: inverted index over a ~368MB corpus) ----------
for k in $(seq 1 "$REPS"); do
	run_json ./psearchy_eq "$THREADS" "$T368" "$OUT/raw/psearchy_eq_run$k.json" || exit 1
done

# ---- dedup_eq (paper: PARSEC dedup, ptmalloc vs tcmalloc) --------------
for k in $(seq 1 "$REPS"); do
	run_json ./dedup_eq "$THREADS" "$DSECS" "$SEED" "$OUT/raw/dedup_eq_run$k.json" || exit 1
done

# tcmalloc variant (optional; the paper evaluates dedup with tcmalloc too)
TCMALLOC=""
for cand in tcmalloc_minimal tcmalloc; do
	p=$(ldconfig -p 2>/dev/null | grep -m1 "lib$cand\.so" | awk '{print $NF}')
	if [ -n "$p" ] && [ -e "$p" ]; then TCMALLOC=$p; break; fi
done
if [ -n "$TCMALLOC" ]; then
	echo "[run_apps] tcmalloc found: $TCMALLOC"
	for k in $(seq 1 "$REPS"); do
		(
			export LD_PRELOAD=$TCMALLOC
			export DEDUP_EQ_ALLOCATOR=tcmalloc
			run_json ./dedup_eq "$THREADS" "$DSECS" "$SEED" \
				"$OUT/raw/dedup_eq_tcmalloc_run$k.json"
		) || exit 1
	done
else
	echo "[run_apps] tcmalloc not found (apt: libtcmalloc-minimal4) — skipping tcmalloc runs" >&2
fi

# ---- summary ------------------------------------------------------------
python3 - "$OUT" "$TCMALLOC" <<'PYEOF'
import glob, json, os, socket, subprocess, sys, datetime

out = sys.argv[1]
tcmalloc = sys.argv[2]
spec = [
    ("metis_eq",    "seconds",      True),   # lower is better
    ("psearchy_eq", "seconds",      True),
    ("dedup_eq",    "blocks_per_s", False),  # higher is better
]
if tcmalloc:
    spec.append(("dedup_eq_tcmalloc", "blocks_per_s", False))

def med_key(run, field):
    return float(run[field])

apps = {}
for name, field, lower in spec:
    runs = []
    for p in sorted(glob.glob(os.path.join(out, "raw", name + "_run*.json"))):
        with open(p) as f:
            runs.append(json.load(f))
    if not runs:
        continue
    vals = [med_key(r, field) for r in runs]
    svals = sorted(vals)
    med = svals[len(svals) // 2]
    median_run = runs[vals.index(med)]
    apps[name] = {
        "median_field": field,
        "lower_is_better": lower,
        "median": median_run,
        "runs": runs,
    }

def sh(cmd):
    try:
        return subprocess.check_output(cmd, shell=True, text=True).strip()
    except Exception:
        return "unknown"

summary = {
    "bench_set": "apps-equivalent (paper section 6.4)",
    "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
    "host": {
        "hostname": socket.gethostname(),
        "kernel": sh("uname -r"),
        "ncpu": os.cpu_count(),
    },
    "apps": apps,
}
with open(os.path.join(out, "apps_summary.json"), "w") as f:
    json.dump(summary, f, indent=1)
    f.write("\n")
print("[run_apps] summary -> " + os.path.join(out, "apps_summary.json"))
PYEOF
