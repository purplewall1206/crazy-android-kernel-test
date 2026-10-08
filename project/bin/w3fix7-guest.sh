#!/bin/bash
# w3fix7-guest.sh - runs INSIDE the guest (root, corten=on kernel).
# Bench-after leg (12 cells) + smoke/metis/audit gates.
# Binaries ride in from the 9p share once (mv3d lesson: battery must not
# depend on 9p reads), everything else local /root.
set -u
OUT=/root/w3fix7
mkdir -p $OUT
log() { echo "[w3f7 $(date -u +%H:%M:%S)] $*"; }

[ -d /sys/kernel/debug/corten ] || mount -t debugfs none /sys/kernel/debug
DBG=/sys/kernel/debug/corten
[ -d "$DBG" ] || { log "FATAL: no debugfs corten"; exit 2; }
grep -q '^enabled  *1$' "$DBG/arena_stats" || { log "FATAL: corten not enabled"; exit 2; }

log "kernel: $(uname -r) cmdline: $(cat /proc/cmdline)"
log "host load: $(cat /proc/loadavg)"

# ---- pull kit from 9p ------------------------------------------------------
[ -d /mnt ] || mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt
MB=/mnt/cortenmm/bench/mmbench/mmbench_dyn
HK=/mnt/r6dg/corten_mode_hook.so
[ -x "$MB" ] || { log "FATAL: $MB missing"; exit 2; }
log "mmbench_dyn sha256: $(sha256sum "$MB" | cut -d' ' -f1)"
cp "$MB" /root/mmbench_dyn; cp "$HK" /root/corten_mode_hook.so
chmod +x /root/mmbench_dyn

# ---- bench-after: 12 cells (mmpf stock/mode x t4/t8 x k1-3) ----------------
# seeds: t4 k1-3 = 20260951-53; t8 k1-3 = 20260979-81 (base-paired)
stat_line() { awk -v K="$1" '$1==K{print $2}' "$DBG/arena_stats" 2>/dev/null; }
for arm in stock mode; do
  for t in 4 8; do
    for k in 1 2 3; do
      if [ $t = 4 ]; then seed=$((20260950 + k)); else seed=$((20260978 + k)); fi
      p0=$(stat_line pool_parks)
      if [ $arm = mode ]; then
	env LD_PRELOAD=/root/corten_mode_hook.so CORTEN_MODE_HOOK_STRICT=1 \
	  /root/mmbench_dyn mmap-pf low $t 2 $seed \
	  >$OUT/mmpf-mode-t$t-k$k.json 2>$OUT/mmpf-mode-t$t-k$k.err
      else
	/root/mmbench_dyn mmap-pf low $t 2 $seed \
	  >$OUT/mmpf-stock-t$t-k$k.json 2>$OUT/mmpf-stock-t$t-k$k.err
      fi
      rc=$?
      p1=$(stat_line pool_parks)
      ok=$(python3 -m json.tool $OUT/mmpf-$arm-t$t-k$k.json >/dev/null 2>&1 && echo valid || echo INVALID)
      log "$arm t$t k$k seed=$seed rc=$rc json=$ok pool_parks $p0 -> $p1 ops=$(grep -o '"ops": [0-9]*' $OUT/mmpf-$arm-t$t-k$k.json | grep -o '[0-9]*$')"
    done
  done
done

# ---- gates -----------------------------------------------------------------
# smoke: 26/26 + SMOKE-DRIVER PASS (driver + binary from share copy)
cp /mnt/run_mode_smoke.sh /mnt/corten_mode_smoke /root/ 2>/dev/null
chmod +x /root/run_mode_smoke.sh /root/corten_mode_smoke 2>/dev/null
(cd /root && bash ./run_mode_smoke.sh) >$OUT/mode-smoke.log 2>&1
log "smoke rc=$? verdict: $(grep -aoE 'SMOKE-DRIVER (PASS|FAIL)' $OUT/mode-smoke.log | tail -1) $(grep -acE '^PASS' $OUT/mode-smoke.log 2>/dev/null | sed 's/^/PASS_lines=/')"

# metis_eq: corpus -> exact checksum gate (2d383eeed4ceb73b), two runs
cp /mnt/r6dg/metis_eq /mnt/r6dg/gen_text /root/
chmod +x /root/metis_eq /root/gen_text
[ -s /root/corpus.txt ] || /root/gen_text 8 /root/corpus.txt >/dev/null 2>&1
log "corpus bytes: $(stat -c%s /root/corpus.txt)"
for i in 1 2; do
  env LD_PRELOAD=/root/corten_mode_hook.so CORTEN_MODE_HOOK_STRICT=1 \
    /root/metis_eq 2 /root/corpus.txt >$OUT/metis.$i.out 2>$OUT/metis.$i.err
  log "metis run$i rc=$? $(grep -aoE '"distinct_words": [0-9]+,"checksum": "[0-9a-f]+"' $OUT/metis.$i.out | tail -1) marker=$(grep -ac 'MODE on' $OUT/metis.$i.err)"
done

# audit gate
cat $DBG/audit_gate > $OUT/audit_gate.txt 2>&1
log "audit_gate: $(grep -E 'gate_pass|j1_hits|j2_violations' $OUT/audit_gate.txt | tr '\n' ' ')"

# dmesg quiet check
dmesg | grep -icE "corten.*(warn|bug|oops)" >$OUT/dmesg-corten-warns.txt
log "dmesg corten warn/bug/oops lines: $(cat $OUT/dmesg-corten-warns.txt)"
log "=== DONE ==="
