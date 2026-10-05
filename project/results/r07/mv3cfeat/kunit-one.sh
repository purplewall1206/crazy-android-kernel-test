#!/bin/bash
# one-glob kunit probe: $1 = glob, $2 = log suffix
set -u
RES=/home/ppw/linux-6.18/project/results/r07/mv3cfeat
KERN=$RES/bzImage-mv3cf-a4
PIDFILE=/home/ppw/vm/qemu-mv3cfkunit.pid
APPEND="console=ttyS0 corten=on kunit.filter_glob=$1 panic=-1"
qemu-system-x86_64 -enable-kvm -cpu host -m 2048 -smp 8 \
	-kernel $KERN -append "$APPEND" \
	-display none -serial stdio -no-reboot \
	-pidfile $PIDFILE > "$RES/kunit-one-$2.log" 2>&1 &
QP=$!
for i in $(seq 120); do kill -0 $QP 2>/dev/null || break; sleep 1; done
kill -0 $QP 2>/dev/null && kill "$QP"
echo "leaks=$(grep -c 'non-zero pgtables_bytes' $RES/kunit-one-$2.log) fails=$(grep -c 'not ok' $RES/kunit-one-$2.log)"
