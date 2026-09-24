#!/bin/bash
# r07-m6t34-zapfix-verify.sh — zap_window tlb_finish_mmu 协议修正的验证编排。
# 分阶段可重跑: bash r07-m6t34-zapfix-verify.sh <stage 1|2|3|4>
#   1  lockdep 变体(=y+PROVE_LOCKING+DEBUG_ATOMIC_SLEEP) KUnit —— 判据: 原先
#      登记的 mmu_gather.c:141 睡眠签名必须绝迹(不再走"已知签名"放行), 全绿
#   2  DEBUG_ATOMIC_SLEEP guest A/B: 修复前基线件(登记的 268 处家族)三形态
#      unmap/churn/DONTNEED 各 15s 必现 splat; 修复后件同样负载 = 0 splat
#   3  guest 普通配置(=y 修复后件) run13 fails=0 + JThreadBench rc=0 + dmesg 静默
#   4  (由主流程处理: 恢复 =y、重导出 diff、verify.md §8 —— 脚本外)
# 禁止碰主树/其它 worktree; 只动 /home/ppw/linux-6.18-m6t34 与 results/r07。
set -u
KDIR=/home/ppw/linux-6.18-m6t34
PROJ=/home/ppw/cortenmm
RES=$PROJ/results/r07/m6t34-zapfix
SHARE=/home/ppw/bench/share
TIMEGATE=$PROJ/bin/timegate.sh
SESSION=m6t34-vm
IMG=/home/ppw/vm/trixie-m6t34.img
PORT=10030
SSHKEY=/home/ppw/vm/trixie.id_rsa
PREFIX_LOCKDEP=/tmp/m6t34-rev-bzimage-lockdep	# 修复前基线(登记 268 处)
FIXED_LOCKDEP=/tmp/m6t34-zapfix-bzimage-lockdep
FIXED_Y=/tmp/m6t34-zapfix-bzimage-final

mkdir -p $RES

say() { echo "[zapfix] $(date '+%F %T') $*"; }
die() { say "FAIL: $*"; exit 1; }

gssh() {
	ssh -i $SSHKEY -p $PORT -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 \
		root@127.0.0.1 "$@"
}

vm_kill() {
	tmux kill-session -t $SESSION 2>/dev/null || true
	if [ -f /home/ppw/vm/qemu-m6t34.pid ]; then
		kill "$(cat /home/ppw/vm/qemu-m6t34.pid)" 2>/dev/null || true
		rm -f /home/ppw/vm/qemu-m6t34.pid
	fi
	sleep 2
	return 0
}

vm_launch() {	# $1 = kernel path, $2 = boot log suffix
	local kern=$1 tag=$2
	vm_kill
	tmux new-session -d -s $SESSION \
		"qemu-system-x86_64 -enable-kvm -cpu host -m 4096 -smp 8 \
		-kernel $kern \
		-append 'console=ttyS0 root=/dev/vda rw net.ifnames=0 systemd.mask=sys-kernel-config.mount corten=on mitigations=off kunit.enable=0 log_buf_len=16M fsck.mode=force fsck.repair=yes' \
		-drive file=$IMG,if=virtio,format=raw \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
		-device virtio-net-pci,netdev=net0 \
		-fsdev local,id=fs0,path=$SHARE,security_model=none \
		-device virtio-9p-pci,fsdev=fs0,mount_tag=hostshare \
		-display none -serial stdio \
		-monitor unix:/home/ppw/vm/monitor-m6t34.sock,server,nowait \
		-pidfile /home/ppw/vm/qemu-m6t34.pid 2>&1 | tee $RES/boot-$tag.log"
	say "VM launching (kernel=$kern, tmux=$SESSION, port=$PORT)"
}

vm_wait_ssh() {
	local i
	for i in $(seq 1 60); do
		if gssh 'echo up' >/dev/null 2>&1; then
			say "guest ssh up (after ${i} tries)"
			return 0
		fi
		sleep 5
	done
	die "guest ssh never came up"
}

run_kunit_strict() {	# $1=append $2=logfile $3=kernel —— 零签名放行
	local extra=$1 log=$2 kern=$3
	timeout 420 qemu-system-x86_64 \
		-enable-kvm -m 2048 -smp 4 \
		-kernel "$kern" \
		-append "console=ttyS0 panic=-1 $extra" \
		-nographic -no-reboot >"$log" 2>&1
	if grep -E "BUG: sleeping function called from invalid context|possible (recursive locking|deadlock)|inconsistent lock state|unsafe locking scenario|bad unlock balance|DEBUG_LOCKS_WARN|BUG: unable to handle|kernel BUG|general protection fault" "$log"; then
		die "KUnit 日志出现 lockdep/oops 签名(修复后应为零): $log"
	fi
	grep -q "# Subtest: corten" "$log" || die "无 corten 套件: $log"
	grep -q "not ok" "$log" && die "存在失败用例: $log"
	local n
	n=$(grep -Ec "# corten(_arena|_fault)?: pass:[0-9]+ fail:0" "$log")
	[ "$n" -eq 3 ] || die "三套件全绿摘要不足(见 $n): $log"
	grep -E "# corten(_arena|_fault)?: pass:" "$log"
	say "KUnit 全绿且零 lockdep 签名: $log"
}

stage1() {
	$TIMEGATE || die "timegate 未放行"
	say "== stage1: lockdep 变体 KUnit(严格零签名: 修复目标即 mmu_gather.c:141 签名绝迹)"
	[ -f $FIXED_LOCKDEP ] || die "修复后 lockdep 件缺失: $FIXED_LOCKDEP"
	run_kunit_strict "corten=on kunit.filter_glob=corten*" \
		$RES/kunit-lockdep-fix.log $FIXED_LOCKDEP
	# 复跑一次(此前 on2/off1 有宿主过载 flake, 记录在案)
	run_kunit_strict "corten=on kunit.filter_glob=corten*" \
		$RES/kunit-lockdep-fix2.log $FIXED_LOCKDEP
	say "== stage1 PASS"
}

ds_run() {	# $1=tag $2=kernel —— 三形态 15s x3 + splat 审计
	local tag=$1 kern=$2
	vm_launch $kern $tag
	vm_wait_ssh
	gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; ls /mnt/m6t34-zapfix-ds.sh /mnt/ds_dontneed' \
		|| die "guest 看不到 share 脚本/工具"
	gssh 'sh /mnt/m6t34-zapfix-ds.sh' > $RES/ds-$tag.log 2>&1
	tail -6 $RES/ds-$tag.log
	grep -q "===== FINAL" $RES/ds-$tag.log || die "ds 驱动未完成: $tag"
}

stage2() {
	$TIMEGATE || die "timegate 未放行"
	say "== stage2: DEBUG_ATOMIC_SLEEP guest A/B (unmap/churn/DONTNEED 各 15s)"
	say "-- A 臂: 修复前基线件(登记 268 处家族), 判据 = sleeps 必现(>0)"
	[ -f $PREFIX_LOCKDEP ] || die "修复前基线件缺失: $PREFIX_LOCKDEP"
	ds_run prefix $PREFIX_LOCKDEP
	grep -q "FINAL: fails=1" $RES/ds-prefix.log \
		|| grep -q "FINAL: fails=2" $RES/ds-prefix.log \
		|| die "A 臂未复现 sleeping splat(基线判据 fails>0)"
	SP=$(grep -oE "sleeps=[0-9]+" $RES/ds-prefix.log | head -1 | cut -d= -f2)
	[ "$SP" -gt 0 ] || die "A 臂 sleeps=0, 无法构成前后对照"
	say "A 臂(修复前): $SP 处 sleeping splat(复现)"
	say "-- B 臂: 修复后件, 判据 = fails=0 (sleeps=0 且 warns=0)"
	ds_run fix $FIXED_LOCKDEP
	grep -q "FINAL: fails=0" $RES/ds-fix.log || die "B 臂仍有失败(见 ds-fix.log)"
	say "B 臂(修复后): 零 splat 零警告"
	say "== stage2 PASS"
}

stage3() {
	$TIMEGATE || die "timegate 未放行"
	say "== stage3: guest 普通配置(修复后 =y 件) run13 + JThreadBench + dmesg 静默"
	[ -f $FIXED_Y ] || die "修复后 =y 件缺失: $FIXED_Y"
	vm_launch $FIXED_Y final
	vm_wait_ssh
	gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; ls /mnt/m6t3-shrink-test.sh /mnt/JThreadBench.java' \
		|| die "guest 看不到 share 脚本"
	say "-- run13 (m6t3-shrink-test.sh)"
	gssh 'sh /mnt/m6t3-shrink-test.sh > /mnt/m6t34-zapfix-run13.log 2>&1; echo RC=$?' \
		> $RES/run13-driver.log 2>&1 &
	DRVPID=$!
	local i
	for i in $(seq 1 40); do
		sleep 30
		if gssh 'grep -q "===== FINAL" /mnt/m6t34-zapfix-run13.log' 2>/dev/null; then
			break
		fi
		say "run13 进行中... ($((i*30))s)"
		[ "$i" -eq 40 ] && die "run13 20 分钟未完成"
	done
	wait $DRVPID 2>/dev/null || true
	gssh 'cat /mnt/m6t34-zapfix-run13.log' > $RES/run13.log 2>&1
	grep -q "FINAL: fails=0" $RES/run13.log || die "run13 fails!=0"
	say "run13 fails=0 通过"

	say "-- JThreadBench (2000 线程 x3, 3 JVM)"
	gssh 'cd /mnt && javac -O JThreadBench.java 2>/dev/null || true'
	local k ok=0
	for k in 1 2 3; do
		if gssh "cd /mnt && timeout 300 java -Xmx512m JThreadBench 2000 3 > /tmp/jtb$k.out 2>/tmp/jtb$k.err"; then
			ok=$((ok+1))
			gssh "tail -1 /tmp/jtb$k.out" >> $RES/jtb.log
		else
			echo "jvm$k rc!=0" >> $RES/jtb.log
		fi
	done
	say "JThreadBench rc=0 JVM 数: $ok/3"
	[ "$ok" -eq 3 ] || die "JThreadBench 有 JVM 失败(见 jtb.log)"

	say "-- 终版 dmesg 审计"
	gssh 'dmesg | grep -cE "BUG: sleeping function called from invalid context|WARNING|Oops|kernel BUG|INV7" || true' \
		> $RES/final-dmesg-count.txt
	[ "$(cat $RES/final-dmesg-count.txt | tr -d '[:space:]')" = "0" ] \
		|| die "终版 guest dmesg 有告警签名"
	say "== stage3 PASS"
}

case "${1:-}" in
1) stage1 ;;
2) stage2 ;;
3) stage3 ;;
all) stage1; stage2; stage3 ;;
*) echo "usage: $0 {1|2|3|all}"; exit 2 ;;
esac
say "stage $1 done"
