#!/bin/bash
# run_all.sh — CortenMM Table 3 microbenchmark matrix runner (run inside the guest).
#
# Default matrix: 5 benches x {low,high} x threads {1,2,4,8} x 3 reps x 5s.
# Per (bench, contention, threads) config the reps' ops_per_sec are aggregated
# to median/min/max into summary.csv; every raw JSON line from microbench is
# appended to raw.jsonl (one JSON object per line).
#
# Usage:
#   OUT=/root/bench/results bash run_all.sh [--smoke]
#     --smoke : 1s per config, 1 rep, threads "1,4" (fast self-check)
#   Optional overrides: BIN=/root/bench/microbench DUR=5 REPS=3 THREADS="1,2,4,8"
set -u -o pipefail

BIN="${BIN:-/root/bench/microbench}"
OUT="${OUT:-/root/bench/results}"
SMOKE=0
for arg in "$@"; do
    case "$arg" in
        --smoke) SMOKE=1 ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done
if [ "$SMOKE" = 1 ]; then
    DUR="${DUR:-1}"; REPS="${REPS:-1}"; THREADS="${THREADS:-1,4}"
else
    DUR="${DUR:-5}"; REPS="${REPS:-3}"; THREADS="${THREADS:-1,2,4,8}"
fi

[ -x "$BIN" ] || { echo "microbench not found/executable at $BIN" >&2 \
    && echo "build first: gcc -O2 -Wall -Wextra -pthread -o $BIN microbench.c" >&2; exit 1; }

mkdir -p "$OUT"
RAW="$OUT/raw.jsonl"
SUMMARY="$OUT/summary.csv"
: > "$RAW"
echo "bench,contention,threads,median_ops_per_sec,min_ops_per_sec,max_ops_per_sec,reps" > "$SUMMARY"

BENCHES="mmap mmap-PF unmap-virt unmap PF"
IFS=',' read -r -a TLIST <<< "$THREADS"

median3() {  # median of a whitespace-separated numeric list (stdin)
    printf '%s\n' "$@" | sort -g | awk '{v[NR]=$1}
        END { if (NR==0) print "NA";
              else if (NR%2==1) print v[(NR+1)/2];
              else printf "%.1f\n", (v[NR/2]+v[NR/2+1])/2 }'
}

echo "matrix: benches=5 contention=low,high threads=[$THREADS] reps=$REPS duration=${DUR}s -> $OUT" >&2
for b in $BENCHES; do
    for c in low high; do
        for t in "${TLIST[@]}"; do
            vals=""
            for r in $(seq 1 "$REPS"); do
                echo "run: bench=$b cont=$c threads=$t rep=$r/${REPS} dur=${DUR}s" >&2
                # microbench prints exactly one JSON line per config on stdout
                line=$("$BIN" --bench "$b" --contention "$c" --threads "$t" --duration "$DUR") || {
                    echo "microbench failed (bench=$b cont=$c threads=$t), aborting" >&2; exit 1; }
                printf '%s\n' "$line" >> "$RAW"
                p=$(printf '%s' "$line" | sed -n 's/.*"ops_per_sec":\([0-9.]*\).*/\1/p')
                [ -n "$p" ] || { echo "cannot parse ops_per_sec from: $line" >&2; exit 1; }
                vals="$vals $p"
            done
            # shellcheck disable=SC2086
            med=$(median3 $vals)
            # shellcheck disable=SC2086
            mn=$(printf '%s\n' $vals | sort -g | head -1)
            # shellcheck disable=SC2086
            mx=$(printf '%s\n' $vals | sort -g | tail -1)
            n=$(printf '%s\n' $vals | grep -c .)
            echo "$b,$c,$t,$med,$mn,$mx,$n" >> "$SUMMARY"
        done
    done
done
echo "done: $(grep -c . "$RAW") raw lines -> $RAW" >&2
echo "summary -> $SUMMARY" >&2
