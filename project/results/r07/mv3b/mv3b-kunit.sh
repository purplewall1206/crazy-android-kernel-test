#!/bin/bash
# mv3b-kunit.sh -- MV3.b KUnit runner (无盘直启, pidfile 纪律).
# 用法: bash mv3b-kunit.sh on1|on2|off
#   on1/on2: corten=on kunit.filter_glob=corten* (三套件执行)
#   off:     同 filter 无 corten=on (套件 skip 对账)
# 判读: 末尾 kunit 总结行 + notok=0。
set -u
RES=/home/ppw/linux-6.18/project/results/r07/mv3b
KERN=$RES/bzImage-mv3b-y
PIDFILE=/home/ppw/vm/qemu-mv3bkunit.pid
MODE=${1:?on1|on2|off}

APPEND="console=ttyS0 kunit.filter_glob=corten* panic=-1"
[ "$MODE" = off ] || APPEND="console=ttyS0 corten=on $APPEND"

qemu-system-x86_64 -enable-kvm -cpu host -m 2048 -smp 8 \
	-kernel $KERN -append "$APPEND" \
	-display none -serial stdio -no-reboot \
	-pidfile $PIDFILE > "$RES/kunit-$MODE.log" 2>&1 &
QP=$!
echo $QP > $PIDFILE
# 三套件 boot ~25s; 上限 150s 兜底。
for i in $(seq 150); do
	kill -0 $QP 2>/dev/null || break
	sleep 1
done
if kill -0 $QP 2>/dev/null; then
	kill "$QP" 2>/dev/null
	sleep 2
	echo "[kunit-$MODE] TIMEOUT (150s) — log 留档"
fi
rm -f $PIDFILE
echo "=== kunit-$MODE 总结 ==="
grep -E "kunit.*:.*passed|not ok|ok [0-9]+ -|BUG|WARNING" "$RES/kunit-$MODE.log" | tail -8
grep -E "^# (corten|corten_arena|corten_fault):" "$RES/kunit-$MODE.log" || grep -E "pass:|fail:" "$RES/kunit-$MODE.log" | tail -6
grep -E "remote_access_window|madvise_route" "$RES/kunit-$MODE.log" | tail -4
