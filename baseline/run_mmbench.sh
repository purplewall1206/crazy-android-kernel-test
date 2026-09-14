#!/usr/bin/env bash
# run_mmbench.sh — machine-readable runner for the paper Table 3 microbenchmarks.
#
# Usage: run_mmbench.sh <outdir> [threads ...]
#   <outdir>         output directory (created if missing)
#   [threads ...]    thread counts; default: "1 2 4 8 16"
#
# For every bench (mmap mmap-pf pf unmap-virt unmap) x contention (low high) x
# threads it runs mmbench 3 times, keeps the per-run single-line JSON under
#   <outdir>/raw/<bench>_<cont>_t<threads>_run<k>.json
# and writes the aggregate
#   <outdir>/mmbench_summary.json
# as a JSON array of
#   {"bench","contention","threads","median_ops_per_us","run1","run2","run3"}
# (median over ops_per_us).
#
# Gate: exits unless `bash /home/ppw/cortenmm/bin/timegate.sh` passes
# (blocks until the free-token window when run outside it).
#
# Env overrides:
#   MMBENCH_MIN_SECONDS  per-run minimum seconds      (default 1)
#   MMBENCH_SEED_BASE    base RNG seed                (default 20260913)
#   MMBENCH_ARENA_MB     arena size override (passed through to mmbench)
#
# Exit code is non-zero if any configuration failed; failed runs keep their
# stderr in <outdir>/raw/<bench>_<cont>_t<threads>_run<k>.err.
# All runner diagnostics go to stderr; stdout stays machine-readable.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$SCRIPT_DIR/mmbench"
TIMEGATE="/home/ppw/cortenmm/bin/timegate.sh"
BENCHES="mmap mmap-pf pf unmap-virt unmap"
CONTS="low high"
RUNS=3
MIN_SECONDS="${MMBENCH_MIN_SECONDS:-1}"
SEED_BASE="${MMBENCH_SEED_BASE:-20260913}"

if [ $# -lt 1 ]; then
	echo "usage: $0 <outdir> [threads ...]" >&2
	echo "  default threads: 1 2 4 8 16" >&2
	exit 2
fi
OUTDIR="$1"
shift
THREADS="${*:-1 2 4 8 16}"

for t in $THREADS; do
	case "$t" in
	'' | *[!0-9]*)
		echo "run_mmbench: invalid thread count '$t'" >&2
		exit 2
		;;
	esac
	[ "$t" -ge 1 ] && [ "$t" -le 1024 ] || {
		echo "run_mmbench: thread count '$t' out of range 1..1024" >&2
		exit 2
	}
done

# ---- gate ------------------------------------------------------------------
if ! bash "$TIMEGATE" >&2; then
	echo "run_mmbench: timegate did not pass, aborting" >&2
	exit 1
fi

# ---- binary (build if missing) ---------------------------------------------
if [ ! -x "$BIN" ]; then
	echo "run_mmbench: $BIN missing, building..." >&2
	if ! gcc -O2 -Wall -Wextra -pthread -static -o "$BIN" "$SCRIPT_DIR/mmbench.c" -lm 2>"$SCRIPT_DIR/.build.err"; then
		echo "run_mmbench: static build failed, falling back to dynamic" >&2
		gcc -O2 -Wall -Wextra -pthread -o "$BIN" "$SCRIPT_DIR/mmbench.c" -lm 2>>"$SCRIPT_DIR/.build.err" || {
			cat "$SCRIPT_DIR/.build.err" >&2
			echo "run_mmbench: build failed" >&2
			exit 1
		}
	fi
fi

mkdir -p "$OUTDIR/raw"
SUMMARY="$OUTDIR/mmbench_summary.json"
LINES="$OUTDIR/.summary_lines.tmp"
: >"$LINES"

bidx=0
FAIL=0
for b in $BENCHES; do
	cidx=0
	for cont in $CONTS; do
		for t in $THREADS; do
			vals=()
			ok=1
			for k in $(seq 1 "$RUNS"); do
				raw="$OUTDIR/raw/${b}_${cont}_t${t}_run${k}.json"
				err="$raw.err"
				seed=$((SEED_BASE + bidx * 1009 + cidx * 97 + t * 7 + k))
				if ! "$BIN" "$b" "$cont" "$t" "$MIN_SECONDS" "$seed" \
					>"$raw" 2>"$err"; then
					echo "run_mmbench: FAIL ($b $cont t=$t run$k, exit=$?) — stderr kept in $err" >&2
					FAIL=1
					ok=0
					rm -f "$raw"
					break
				fi
				if ! python3 -m json.tool "$raw" >/dev/null 2>&1; then
					echo "run_mmbench: FAIL ($b $cont t=$t run$k, invalid JSON) — stderr kept in $err" >&2
					FAIL=1
					ok=0
					break
				fi
				v="$(python3 -c 'import json,sys; print("%.10g" % json.load(open(sys.argv[1]))["ops_per_us"])' "$raw")"
				vals+=("$v")
				rm -f "$err"
				echo "run_mmbench: $b $cont t=$t run$k ops_per_us=$v" >&2
			done
			if [ "$ok" -eq 1 ]; then
				med="$(printf '%s\n' "${vals[@]}" | sort -g | sed -n '2p')"
				printf '{"bench":"%s","contention":"%s","threads":%s,"median_ops_per_us":%s,"run1":%s,"run2":%s,"run3":%s}\n' \
					"$b" "$cont" "$t" "$med" "${vals[0]}" "${vals[1]}" "${vals[2]}" >>"$LINES"
			else
				FAIL=1
			fi
		done
		cidx=$((cidx + 1))
	done
	bidx=$((bidx + 1))
done

# ---- assemble summary array -------------------------------------------------
{
	echo "["
	awk 'NR > 1 { printf(",\n") } { printf("%s", $0) }' "$LINES"
	echo "]"
} >"$SUMMARY"
rm -f "$LINES"

if [ "$FAIL" -eq 0 ]; then
	echo "run_mmbench: all configurations OK, summary: $SUMMARY" >&2
else
	echo "run_mmbench: COMPLETED WITH FAILURES, summary: $SUMMARY" >&2
fi

exit "$FAIL"
