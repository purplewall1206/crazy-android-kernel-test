#!/bin/bash
# mv3cf-kunit.sh -- MV3.c-feat KUnit runner (diskless direct boot, pidfile discipline).
# usage: bash mv3cf-kunit.sh on1|on2|off
set -u
RES=/home/ppw/linux-6.18/project/results/r07/mv3cfeat
KERN=$RES/bzImage-mv3cf-a5
PIDFILE=/home/ppw/vm/qemu-mv3cfkunit.pid
MODE=${1:?on1|on2|off}

APPEND="console=ttyS0 kunit.filter_glob=corten* panic=-1"
[ "$MODE" = off ] || APPEND="console=ttyS0 corten=on $APPEND"

qemu-system-x86_64 -enable-kvm -cpu host -m 2048 -smp 8 \
	-kernel $KERN -append "$APPEND" \
	-display none -serial stdio -no-reboot \
	-pidfile $PIDFILE > "$RES/kunit-$MODE.log" 2>&1 &
QP=$!
echo $QP > $PIDFILE
for i in $(seq 150); do
	kill -0 $QP 2>/dev/null || break
	sleep 1
done
if kill -0 $QP 2>/dev/null; then
	kill "$QP" 2>/dev/null
	sleep 2
	echo "[kunit-$MODE] TIMEOUT (150s) -- log kept"
else
	echo "[kunit-$MODE] done"
fi
grep -E "^# (Totals|not ok)" "$RES/kunit-$MODE.log" | head -2
