#!/bin/bash
# perf2-kunit.sh - KUnit corten* boot runner (diskless boot, serial capture).
# usage: perf2-kunit.sh <on|off> <tag>
#   on  = append "corten=on";  off = plain (corten default off).
#   Output: /home/ppw/cortenmm/results/r07/perf2/kunit-<tag>.log
#   Greps the per-suite "ok/not ok" lines and Totals; exit 1 on any not-ok.
set -u
KERNEL=/home/ppw/linux-6.18-perf2/arch/x86/boot/bzImage
MODE="${1:-on}"
TAG="${2:-tmp}"
R=/home/ppw/cortenmm/results/r07/perf2
LOG="$R/kunit-$TAG.log"
mkdir -p "$R"

APPEND="console=ttyS0 panic=-1 kunit.filter_glob=corten*"
[ "$MODE" = on ] && APPEND="$APPEND corten=on"

tmux kill-session -t kunit-perf2 2>/dev/null
qemu-system-x86_64 -enable-kvm -cpu host -m 2048 -smp 4 -kernel "$KERNEL" \
	-append "$APPEND" -display none -serial stdio -no-reboot \
	>"$LOG" 2>&1 &
QPID=$!
# The diskless boot runs KUnit at init then panics (panic=-1 reboots forever);
# 90s wall is plenty for the corten* suites (previous rounds: <10s of kernel
# time).  Poll the log for the last Totals line then wait 5s.
for i in $(seq 1 90); do
	grep -q "Totals:" "$LOG" && break
	sleep 1
done
sleep 8
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "== $TAG ($MODE) =="
grep -E "^(\[ *[0-9]+\.[0-9]+\] )?(ok|not ok|    # Totals|# Totals|    Totals)" "$LOG" |
	sed 's/^\[[ 0-9]*\.[0-9]*\] //' |
	grep -vE "^$" | tail -80
N=$(grep -c "not ok" "$LOG")
T=$(grep -c "Totals:" "$LOG")
echo "SUMMARY $TAG: not_ok_lines=$N totals_lines=$T"
[ "$N" = 0 ] && [ "$T" -ge 1 ]
