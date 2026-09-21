#!/bin/bash
# mva0 KUnit corten* runner (diskless boot, three-suite filter).
# usage: kunit-run.sh <on|off> <tag>
set -u
KERNEL=/home/ppw/linux-6.18-mva/arch/x86/boot/bzImage
MODE="${1:-on}"
TAG="${2:-tmp}"
R=/home/ppw/cortenmm/results/r07/mva0
LOG="$R/kunit-$TAG.log"

APPEND="console=ttyS0 panic=-1 kunit.filter_glob=corten* log_buf_len=16M"
[ "$MODE" = on ] && APPEND="$APPEND corten=on"

qemu-system-x86_64 -enable-kvm -cpu host -m 2048 -smp 4 -kernel "$KERNEL" \
	-append "$APPEND" -display none -serial stdio -no-reboot \
	>"$LOG" 2>&1 &
QPID=$!
for i in $(seq 1 240); do
	grep -c "Totals:" "$LOG" | grep -q 3 && break
	kill -0 "$QPID" 2>/dev/null || break
	sleep 1
done
sleep 15
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "== $TAG ($MODE) =="
grep -E "Subtest:|ok |not ok|# Totals" "$LOG" | tail -90
N=$(grep -c "^ *not ok" "$LOG")
T=$(grep -c "Totals:" "$LOG")
echo "SUMMARY $TAG: not_ok_lines=$N totals_lines=$T"
[ "$N" = 0 ] && [ "$T" -ge 1 ]
