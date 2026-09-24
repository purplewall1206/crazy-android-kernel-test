#!/usr/bin/env bash
# diag_profile.sh - M4.T1/T2 diagnosis (m4t12): perf profile the MODE ceremony
# vs BASE on the three G1-relevant mmbench cells.  Guest side, root, corten=on.
# Cells: mmap low t8 / unmap-virt low t4 / unmap low t8, MIN_SECONDS=15 per leg.
set -u
OUT=/root/diag
MMB=$OUT/bin/mmbench
HOOK=$OUT/corten_mode_hook.so
mkdir -p "$OUT/prof"
CELLS="mmap:low:8 unmap-virt:low:4 unmap:low:8"

for cell in $CELLS; do
	b=${cell%%:*}; rest=${cell#*:}; c=${rest%%:*}; t=${rest##*:}
	for arm in base t0; do
		f="$OUT/prof/${b}_${c}_t${t}.${arm}"
		if [ "$arm" = t0 ]; then
			ENV=(env LD_PRELOAD="$HOOK" CORTEN_MODE_HOOK_STRICT=1)
		else
			ENV=(env -u LD_PRELOAD -u CORTEN_MODE_HOOK_STRICT)
		fi
		echo "=== $b/$c/t$t [$arm] perf start $(date +%T) ==="
		perf record -F 999 -g -o "$f.data" -- "${ENV[@]}" "$MMB" "$b" "$c" "$t" 15 1 \
			>"$f.native" 2>"$f.err"
		echo "rc=$? ops=$(grep -o '"ops_per_us":[0-9.]*' "$f.native" | head -1)"
	done
done
echo ALLDONE
