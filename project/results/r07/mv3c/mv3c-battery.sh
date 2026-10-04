#!/bin/bash
# mv3c-battery.sh -- MV3.c-debug 轮 followup 全电池 (82a35da 树)。
# 用法: bash mv3c-battery.sh
#   5x =on boot (unmasked journal 形) 每轮 corruption 签名审计
#   + PGTABLES/j2_stale 常规读数; 第 3 轮跑 smoke 双形态 + metis x2。
# 判据: corruption 签名 = Bad page / WARNING / Oops / list_del /
#   mm.h:2648 / segfault (corruption-signature gate, mv3b 口径);
#   non-zero pgtables = 老 stale 计数 (house 排除, 逐 boot 记账)。
set -u
RES=/home/ppw/linux-6.18/project/results/r07/mv3c
SHARE=/home/ppw/bench/share
KERN=$RES/bzImage-mv3c-y
IMG=/home/ppw/vm/trixie-w6v2.img
PORT=10030
SSHKEY=/home/ppw/vm/trixie.id_rsa
PIDFILE=/home/ppw/vm/qemu-mv3c.pid
SESSION=mv3c-vm
DBG=/sys/kernel/debug/corten
BASELINE=2d383eeed4ceb73b
SIG='BUG: Bad page|WARNING|Oops|list_del corruption|mm.h:2648|segfault'

say() { echo "[mv3c-battery] $(date '+%F %T') $*"; }
die() { say "FAIL: $*"; exit 1; }

gssh() {
	ssh -i $SSHKEY -p $PORT -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 \
		root@127.0.0.1 "$@"
}

vm_kill() {
	if [ -f $PIDFILE ]; then
		kill "$(cat $PIDFILE)" 2>/dev/null || true
		rm -f $PIDFILE
	fi
	tmux kill-session -t $SESSION 2>/dev/null || true
	sleep 2
}

vm_boot() {	# $1 = console log name
	vm_kill
	tmux new-session -d -s $SESSION \
		"qemu-system-x86_64 -enable-kvm -cpu host -m 4096 -smp 8 \
		-kernel $KERN \
		-append 'console=ttyS0 root=/dev/vda rw net.ifnames=0 systemd.mask=sys-kernel-config.mount corten=on corten_mode_default=on mitigations=off kunit.enable=0 log_buf_len=16M' \
		-drive file=$IMG,if=virtio,format=raw \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
		-device virtio-net-pci,netdev=net0 \
		-fsdev local,id=fs0,path=$SHARE,security_model=none \
		-device virtio-9p-pci,fsdev=fs0,mount_tag=hostshare \
		-display none -serial file:$RES/$1 \
		-monitor unix:/home/ppw/vm/monitor-mv3c.sock,server,nowait \
		-pidfile $PIDFILE"
	local i
	for i in $(seq 1 60); do
		if gssh 'echo up' >/dev/null 2>&1; then
			gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; mount -t debugfs none /sys/kernel/debug 2>/dev/null || true; mkdir -p /tmp/mv3c' \
				|| die "share/debugfs mount failed"
			return 0
		fi
		sleep 5
	done
	die "guest ssh never came up ($1)"
}

audit() {	# $1 = tag; prints signature counts + stats readouts
	local log=$RES/$1
	echo "--- [$1] corruption-signature audit:"
	echo "badpage=$(grep -c 'BUG: Bad page' $log) warn=$(grep -c 'WARNING' $log) oops=$(grep -cE 'Oops|list_del corruption' $log) mmh2648=$(grep -c 'mm.h:2648' $log) segv=$(grep -ci 'segfault' $log)"
	echo "pgres(stale-counter, house-excluded)=$(grep -c 'non-zero pgtables' $log)"
	echo "--- [$1] PGTABLES/j2 readouts:"
	echo "PGTABLES_COUNT(dmesg)=$(grep -c 'non-zero pgtables_bytes' $log)"
	timeout 25 gssh "timeout 10 grep -E 'j2_stale|j2_walks|j2_violations' $DBG/j2 2>/dev/null | head -3; echo J2_RC=\$?" || echo "J2_READ_TIMEOUT(host-side)"
}

workloads() {	# smoke dual-form + metis x2, rc 落盘再回读
	local out=$RES/battery-workloads.log
	{
		echo "=== smoke v2 hook form (26) ==="
		gssh 'cd /tmp/mv3c && cp -f /mnt/t0dod/mode-smoke/corten_mode_smoke /mnt/t0dod/mode-smoke/run_mode_smoke.sh . && chmod +x corten_mode_smoke run_mode_smoke.sh && LD_PRELOAD=/mnt/r6dg/corten_mode_hook.so CORTEN_MODE_HOOK_STRICT=1 bash ./run_mode_smoke.sh > smoke-hook.full 2>&1; echo SMOKE_HOOK_RC=$?; grep -c "^PASS" smoke-hook.full; grep -c "^FAIL" smoke-hook.full; tail -3 smoke-hook.full'
		echo "=== smoke v2 bare form (default-entry) ==="
		gssh 'cd /tmp/mv3c && bash ./run_mode_smoke.sh > smoke-bare.full 2>&1; echo SMOKE_BARE_RC=$?; grep -c "^PASS" smoke-bare.full; grep -c "^FAIL" smoke-bare.full; tail -3 smoke-bare.full'
		echo "=== metis_eq x2 (baseline $BASELINE) ==="
		gssh 'cd /tmp/mv3c && [ -s corpus.txt ] || /mnt/r6dg/gen_text 8 corpus.txt >/dev/null 2>&1 || head -c 8388608 /dev/zero | tr "\0" a > corpus.txt; for i in 1 2; do timeout 300 /mnt/r6dg/metis_eq 2 corpus.txt > metis$i.out 2> metis$i.err; echo metis$i RC=$?; grep -o "\"distinct_words\":[0-9]*,\"checksum\":\"[a-f0-9]*\"" metis$i.out; done'
		echo "=== exec_default_enters / dmesg audit ==="
		timeout 25 gssh "grep -E 'exec_default_enters' $DBG/arena_stats" || echo "STATS_READ_TIMEOUT(host-side; arena_stats renderer CPU-bound, see report 3.2/8)"
		gssh 'dmesg | grep -iE "WARNING|BUG\]" | grep -iv "non-zero pgtables" | tail -5 || true; echo DMESG_AUDIT_RC=$?'
	} > "$out" 2>&1
	say "workloads done, log $out"
	grep -E "_RC=|^=== |^PASS" "$out" | head -20
}

say "battery start, kernel=$KERN"
for i in 1 2 3 4 5; do
	console=console-mv3c-battery-$i.log
	say "=== boot $i/5"
	if ! vm_boot $console; then die "boot $i failed"; fi
	sleep 10	# settle: graphical/login 完成
	audit $console
	if [ "$i" = 3 ]; then say "=== workloads on boot 3"; workloads; audit $console; fi
done
vm_kill
say "battery complete"
