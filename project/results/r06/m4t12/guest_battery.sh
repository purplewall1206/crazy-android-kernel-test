#!/usr/bin/env bash
# m4t12 guest battery: probe sanity -> mode smoke -> JThreadBench -> ABAB cells
set -u
OUT=/root/m4t12
MMB=$OUT/bin/mmbench_dyn
HOOK=$OUT/corten_mode_hook.so
DBG=/sys/kernel/debug/corten
LOG=$OUT/battery.log
: >"$LOG"
log() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG"; }

log "kernel $(uname -r)"
log "dmesg corten warns before: $(dmesg | grep -icE 'corten.*(warn|bug|oops)')"

# ---- probe sanity (short: MODE vs BASE, all four shapes) ----
log "=== probe sanity ==="
gcc -O2 -o $OUT/m4t12_probe $OUT/m4t12_probe.c 2>>"$LOG" &&
for m in mpl uv u BASE BASE-u; do
	$OUT/m4t12_probe $m 8000 | tee -a "$LOG"
done

# ---- run_mode_smoke (26 internal cases) ----
log "=== run_mode_smoke ==="
cd $OUT
cp /mnt/hostshare/cortenmm/bench/mode-smoke/run_mode_smoke.sh $OUT/ 2>/dev/null
chmod +x run_mode_smoke.sh
./run_mode_smoke.sh 2>&1 | tee -a "$LOG"
log "smoke driver rc=$?"

# ---- JThreadBench (MODE via hook, 2000 threads x1 leg + HelloFmt x2) ----
log "=== JThreadBench ==="
env LD_PRELOAD=$HOOK CORTEN_MODE_HOOK_STRICT=1 \
	java -Xmx512m -cp $OUT JThreadBench 2000 3 >$OUT/jt.out 2>$OUT/jt.err
rc=$?
log "JThreadBench MODE rc=$rc ($(tail -c 120 $OUT/jt.out | tr '\n' ' '))"
grep -m1 ClassFormatError $OUT/jt.err >>"$LOG" 2>/dev/null
env -u LD_PRELOAD java -Xmx512m -cp $OUT JThreadBench 2000 3 >$OUT/jtb.out 2>$OUT/jtb.err
log "JThreadBench base rc=$?"

dmesg | grep -icE "corten.*(warn|bug|oops)" > $OUT/dmesg_warn.txt
log "dmesg corten warns after: $(cat $OUT/dmesg_warn.txt)"
log "BATTERY DONE"
