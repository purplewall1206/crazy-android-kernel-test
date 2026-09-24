#!/usr/bin/env bash
# run_abab.sh - M4.T1/T2 3-cell ABAB verification (guest side).
# Waits for the battery to finish, then runs the four G1-relevant cells
# with the DYNAMIC mmbench (both arms same binary, paired seeds).
# Results land on the 9p share (host-visible) + /root/m4t12.
set -u
OUT=/root/m4t12
MMB=$OUT/bin/mmbench_dyn
HOOK=$OUT/corten_mode_hook.so
RSLT=/mnt/hostshare/m4t12-results
LOG=$OUT/abab.log
mkdir -p "$RSLT/raw"
: >"$LOG"
log() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG"; }

# stage gate: battery must be done
for i in $(seq 1 240); do
	grep -q "BATTERY DONE" "$OUT/battery.log" 2>/dev/null && break
	sleep 30
done
log "battery gate cleared (or timed out): $(tail -1 $OUT/battery.log 2>/dev/null)"

# re-arm: if JThreadBench never ran (battery stuck), run it here once
if [ ! -s "$OUT/jt.out.done" ] && ! grep -q "JThreadBench MODE" "$OUT/battery.log" 2>/dev/null; then
	log "running JThreadBench (MODE) directly"
	env LD_PRELOAD=$HOOK CORTEN_MODE_HOOK_STRICT=1 \
		java -Xmx512m -cp $OUT JThreadBench 2000 3 \
		>"$OUT/jt.out" 2>"$OUT/jt.err"
	log "JThreadBench MODE rc=$?"
	touch "$OUT/jt.out.done"
	cp "$OUT/jt.out" "$RSLT/jt_mode.json" 2>/dev/null
	env -u LD_PRELOAD java -Xmx512m -cp $OUT JThreadBench 2000 3 \
		>"$OUT/jtb.out" 2>"$OUT/jtb.err"
	log "JThreadBench base rc=$?"
	cp "$OUT/jtb.out" "$RSLT/jt_base.json" 2>/dev/null
fi

# smoke re-run capture (the driver ran inside battery; keep its transcript)
cp "$OUT/battery.log" "$RSLT/battery.log" 2>/dev/null

cell() {
	local b=$1 c=$2 t=$3 bidx cidx k seed base t0
	case "$b" in
		mmap) bidx=0 ;; mmap-pf) bidx=1 ;; pf) bidx=2 ;; unmap-virt) bidx=3 ;; unmap) bidx=4 ;;
	esac
	case "$c" in low) cidx=0 ;; high) cidx=1 ;; esac
	log "=== cell $b/$c/t$t ==="
	for k in 1 2 3; do
		seed=$((20260913 + bidx * 1009 + cidx * 97 + t * 7 + k))
		env -u LD_PRELOAD -u CORTEN_MODE_HOOK_STRICT \
			timeout -k 10 300 "$MMB" "$b" "$c" "$t" 3 "$seed" \
			>"$OUT/raw/${b}_${c}_t${t}_r${k}.base.json" 2>"/dev/null"
		log "base r$k rc=$? $(cat $OUT/raw/${b}_${c}_t${t}_r${k}.base.json 2>/dev/null | grep -o '\"ops_per_us\":[0-9.]*')"
		env LD_PRELOAD=$HOOK CORTEN_MODE_HOOK_STRICT=1 \
			timeout -k 10 300 "$MMB" "$b" "$c" "$t" 3 "$seed" \
			>"$OUT/raw/${b}_${c}_t${t}_r${k}.t0.json" 2>"/dev/null"
		log "t0   r$k rc=$? $(cat $OUT/raw/${b}_${c}_t${t}_r${k}.t0.json 2>/dev/null | grep -o '\"ops_per_us\":[0-9.]*')"
	done
}

cell unmap-virt low 4
cell unmap-virt low 8
cell unmap low 8
cell mmap-pf low 8

dmesg | grep -icE "corten.*(warn|bug|oops|timed out)" > "$RSLT/dmesg_warn.txt"
grep -E "auto_mmaps|munmap_releases|mprotect_routes|seg_claims|mag_skips|va_recycles" \
	/sys/kernel/debug/corten/arena_stats > "$RSLT/arena_stats.after" 2>/dev/null
log "ABAB DONE"
