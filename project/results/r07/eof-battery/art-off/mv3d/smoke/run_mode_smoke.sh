#!/bin/bash
# T0a MODE-process smoke driver (guest side, run as root).
# Build on host: gcc -static -O2 -Wall -Wextra -o corten_mode_smoke
#   corten_mode_smoke.c   (binary reaches the guest via the 9p share or scp)
set -u
DBG=/sys/kernel/debug/corten
fails=0
log() { echo "[smoke] $*"; }

if [ ! -d "$DBG" ]; then
	echo "[smoke] FAIL: $DBG missing (debugfs mounted? corten=on?)"
	exit 1
fi

arenas_lines() { tail -n +2 "$DBG/arenas" | wc -l; }

log "debugfs arenas before: $(arenas_lines)"

# count window arenas before the run (should be stable baseline)
before=$(arenas_lines)

./corten_mode_smoke 2>&1
rc=$?
log "smoke binary rc=$rc"

# window arenas observed during the run are gone only if the app exits;
# the process-based check happens inside the binary.  Here we assert the
# ledger returned to its baseline after the smoke process exited.
after=$(arenas_lines)
log "debugfs arenas after: $after"
if [ "$before" != "$after" ]; then
	log "FAIL: arena ledger not back to baseline ($before -> $after)"
	fails=$((fails+1))
fi

# no corten warnings/oops in the ring
if dmesg | grep -qiE "corten.*(WARN|BUG|timed out)"; then
	log "FAIL: corten warnings in dmesg"
	dmesg | grep -iE "corten" | tail -20
	fails=$((fails+1))
fi

[ $rc -ne 0 ] && fails=$((fails+1))

if [ $fails -eq 0 ]; then
	log "SMOKE-DRIVER PASS"
	exit 0
fi
log "SMOKE-DRIVER FAIL ($fails)"
exit 1
