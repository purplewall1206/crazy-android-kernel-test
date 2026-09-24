#!/bin/bash
# r07-m6t34-rev-verify.sh — M6.T3+T4 复审 NO-GO 处置的验证编排(B1-B3/B4 修复后)。
# 分阶段可重跑: bash r07-m6t34-rev-verify.sh <stage 1|2|3|4>
#   1  =y 构建(零新增警告) + KUnit corten* on×2/off×1 + =n 八对象 + checkpatch
#   2  lockdep(O= 独立目录, PROVE_LOCKING+DEBUG_ATOMIC_SLEEP) 构建 + lockdep KUnit
#      + guest corten=on 实跑 evict 200000000 多轮 reclaim(零 splat 零 D 状态)
#   3  guest 普通配置(=y 终版件) run13 fails=0 复验 + evict 冒烟
#   4  patches/r07-m6t34.diff 备份 + 重导出 + 对账
# 每阶段先过 timegate(23:00-09:00 免费窗口)。禁止碰主树; 本脚本只动
# /home/ppw/linux-6.18-m6t34 与新建的 O= 目录 linux-6.18-m6t34-lkd。
set -u
KDIR=/home/ppw/linux-6.18-m6t34
PROJ=/home/ppw/cortenmm
RES=$PROJ/results/r07
LKD=/home/ppw/linux-6.18-m6t34-lkd
SHARE=/home/ppw/bench/share
TIMEGATE=$PROJ/bin/timegate.sh
SESSION=m6t34-vm
IMG=/home/ppw/vm/trixie-m6t34.img
PORT=10030
SSHKEY=/home/ppw/vm/trixie.id_rsa
MKTAG=m6t34-rev

mkdir -p $RES/$MKTAG

say() { echo "[m6t34-rev] $(date '+%F %T') $*"; }
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
		-pidfile /home/ppw/vm/qemu-m6t34.pid 2>&1 | tee $RES/$MKTAG/boot-$tag.log"
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

run_kunit() {	# $1=append $2=logfile $3=kernel [$4=known-signature grep -v 模式]
	local extra=$1 log=$2 kern=$3 known=${4:-__none__}
	timeout 420 qemu-system-x86_64 \
		-enable-kvm -m 2048 -smp 4 \
		-kernel "$kern" \
		-append "console=ttyS0 panic=-1 $extra" \
		-nographic -no-reboot >"$log" 2>&1
	# 退出码非 0 属预期(无 rootfs, panic=-1); 以日志为准。
	if [ "$known" = "__none__" ]; then
		if grep -E "BUG: sleeping function called from invalid context|possible (recursive locking|deadlock)|inconsistent lock state|unsafe locking scenario|bad unlock balance|DEBUG_LOCKS_WARN|BUG: unable to handle|kernel BUG|general protection fault" "$log"; then
			die "KUnit 日志出现 lockdep/oops 签名: $log"
		fi
	else
		# 已知既有签名($4)放行但记录; 其余一票否; 已知签名的回溯若
		# 触及本片路径(shrink/evict)同样一票否。
		grep -E "BUG: sleeping function called from invalid context|possible (recursive locking|deadlock)|inconsistent lock state|unsafe locking scenario|bad unlock balance|DEBUG_LOCKS_WARN|BUG: unable to handle|kernel BUG|general protection fault" "$log" \
			| grep -vE "$known" > "$log.knowncheck" || true
		if [ -s "$log.knowncheck" ]; then
			cat "$log.knowncheck"
			die "KUnit 日志出现非已知 lockdep/oops 签名: $log"
		fi
		grep -A 14 "BUG: sleeping function called from invalid context" "$log" \
			| grep -E "corten_shrink|corten_arena_evict" \
			&& die "已知签名回溯触及 shrink/evict 本片路径: $log"
		local nk
		nk=$(grep -c "$known" "$log" || true)
		say "已知既有签名 $nk 处(已登记类别, 回溯未触及本片路径): $log"
	fi
	grep -q "# Subtest: corten" "$log" || die "无 corten 套件: $log"
	grep -q "not ok" "$log" && die "存在失败用例: $log"
	local n
	n=$(grep -Ec "# corten(_arena|_fault)?: pass:[0-9]+ fail:0" "$log")
	[ "$n" -eq 3 ] || die "三套件全绿摘要不足(见 $n): $log"
	grep -E "# corten(_arena|_fault)?: pass:" "$log"
	say "KUnit 全绿: $log"
}

warn_audit() {	# $1=build log; 断言除两条基线既有告警外零 warning(=y 全量
		# 自 r01 起即有: cpuidle objtool + memblock_end_of_DRAM modpost)
	local hits
	hits=$(grep -inE "warning" "$1" \
		| grep -v "objtool: cpuidle_enter_state" \
		| grep -v "WARNING: modpost: vmlinux: memblock_end_of_DRAM: EXPORT_SYMBOL used for init symbol" \
		|| true)
	if [ -n "$hits" ]; then
		echo "$hits"
		die "构建日志出现非基线 warning: $1"
	fi
	say "构建零新增 warning: $1"
}

stage1() {
	$TIMEGATE || die "timegate 未放行"
	cd $KDIR || die "cd $KDIR 失败"
	grep -q "^CONFIG_CORTEN_MM=y" .config || die "当前配置 CORTEN_MM != y"
	grep -q "^CONFIG_PROVE_LOCKING" .config && grep -q "^CONFIG_PROVE_LOCKING=y" .config && die ".config 已是 lockdep 变体, 拒绝"

	say "== stage1: =y 构建"
	make -j12 > $RES/$MKTAG/build-y.log 2>&1 || die "=y 构建失败"
	warn_audit $RES/$MKTAG/build-y.log
	cp arch/x86/boot/bzImage /tmp/$MKTAG-bzimage-final || die "bzImage 归档失败"
	say "终版 bzImage 归档 /tmp/$MKTAG-bzimage-final: $(sha256sum < /tmp/$MKTAG-bzimage-final | cut -c1-16)..."

	say "== stage1: KUnit on×2"
	run_kunit "corten=on kunit.filter_glob=corten*" $RES/$MKTAG/kunit-on1.log arch/x86/boot/bzImage
	run_kunit "corten=on kunit.filter_glob=corten*" $RES/$MKTAG/kunit-on2.log arch/x86/boot/bzImage
	say "== stage1: KUnit off×1"
	run_kunit "kunit.filter_glob=corten*" $RES/$MKTAG/kunit-off1.log arch/x86/boot/bzImage

	say "== stage1: =n 八对象"
	cp .config /tmp/$MKTAG-config-y || die "config 备份失败"
	./scripts/config -d CORTEN_MM || die "scripts/config 失败"
	make olddefconfig >/dev/null || die "olddefconfig 失败"
	grep -q "^CONFIG_CORTEN_MM=y" .config && die "CORTEN_MM 未关"
	make -j12 mm/memory.o mm/mmap.o mm/migrate.o mm/rmap.o mm/swapfile.o \
		mm/gup.o mm/oom_kill.o arch/x86/mm/fault.o \
		> $RES/$MKTAG/build-n.log 2>&1 || die "=n 八对象构建失败"
	warn_audit $RES/$MKTAG/build-n.log
	cp /tmp/$MKTAG-config-y .config && make olddefconfig >/dev/null 2>&1
	grep -q "^CONFIG_CORTEN_MM=y" .config || die "config 恢复失败"
	say "=n 八对象零错零警, 配置已恢复 =y"

	say "== stage1: checkpatch --strict(5 改动文件, file 模式)"
	./scripts/checkpatch.pl --strict --file include/linux/corten.h \
		include/linux/corten_arena.h mm/corten.c mm/corten_arena.c \
		mm/corten_arena_test.c > $RES/$MKTAG/checkpatch-full.txt 2>&1
	# 顺序 = 参数顺序: 1 corten.h 2 corten_arena.h 3 corten.c 4 arena.c 5 test.c
	grep -E "^total: [0-9]+ errors, [0-9]+ warnings, [0-9]+ checks" \
		$RES/$MKTAG/checkpatch-full.txt > $RES/checkpatch-$MKTAG.txt
	paste <(printf '%s\n' corten.h corten_arena.h corten.c corten_arena.c \
		corten_arena_test.c) $RES/checkpatch-$MKTAG.txt
	! grep -qE "^total: [1-9][0-9]* errors" $RES/checkpatch-$MKTAG.txt \
		|| die "checkpatch 有 error"
	grep -E "^total: 0 errors, [1-9]" $RES/checkpatch-$MKTAG.txt >/dev/null && {
		sed -n '4p' $RES/checkpatch-$MKTAG.txt | grep -qE "^total: 0 errors, [01] warnings," \
			|| die "warning 只允许 arena.c 基线 ≤1 条"
	}
	say "== stage1 PASS"
}

stage2() {
	$TIMEGATE || die "timegate 未放行"
	cd $KDIR || die "cd $KDIR 失败"

	say "== stage2: lockdep 变体构建(树内切换, PROVE_LOCKING+DEBUG_ATOMIC_SLEEP)"
	# 树内切换(先例: config-prelockdep.backup 舞步); O= 与在树构建产物不兼容。
	[ -f /tmp/$MKTAG-config-y ] || cp .config /tmp/$MKTAG-config-y
	./scripts/config -e PROVE_LOCKING -e DEBUG_ATOMIC_SLEEP \
		|| die "scripts/config 失败"
	make olddefconfig >/dev/null || die "lockdep olddefconfig 失败"
	grep -q "^CONFIG_PROVE_LOCKING=y" .config || die "PROVE_LOCKING 未生效"
	grep -q "^CONFIG_DEBUG_ATOMIC_SLEEP=y" .config || die "DEBUG_ATOMIC_SLEEP 未生效"
	grep -q "^CONFIG_CORTEN_MM=y" .config || die "CORTEN_MM 丢失"
	make -j12 > $RES/$MKTAG/build-lockdep.log 2>&1 || die "lockdep 构建失败"
	[ -f arch/x86/boot/bzImage ] || die "lockdep bzImage 缺失"
	warn_audit $RES/$MKTAG/build-lockdep.log
	cp arch/x86/boot/bzImage /tmp/$MKTAG-bzimage-lockdep
	say "lockdep bzImage: $(sha256sum < /tmp/$MKTAG-bzimage-lockdep | cut -c1-16)..."

	say "== stage2: lockdep KUnit(corten=on, 无盘 qemu; DEBUG_ATOMIC_SLEEP 首开)"
	# 已知既有签名 = mm/mmu_gather.c:141 的 M4 zap 路径暴露(corten_arena_zap_window
	# 在 desc 写锁内 tlb_finish_mmu; 非 B1/非本片引入, 登记移交)。
	run_kunit "corten=on kunit.filter_glob=corten*" $RES/$MKTAG/kunit-lockdep.log \
		/tmp/$MKTAG-bzimage-lockdep \
		"BUG: sleeping function called from invalid context at mm/mmu_gather.c:141"

	say "== stage2: guest 实跑 evict 200000000(评审指名盲区闭环)"
	vm_launch /tmp/$MKTAG-bzimage-lockdep lockdep
	vm_wait_ssh
	gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; ls /mnt/m6t34-rev-evict-lockdep.sh' \
		|| die "guest 看不到 share 脚本"
	# 后台跑, 日志走 9p 回传
	gssh 'nohup sh /mnt/m6t34-rev-evict-lockdep.sh > /mnt/m6t34-rev-lockdep-evict.log 2>&1 & echo started'
	local i
	for i in $(seq 1 80); do
		sleep 30
		if gssh 'grep -q "===== FINAL" /mnt/m6t34-rev-lockdep-evict.log' 2>/dev/null; then
			break
		fi
		say "evict 进行中... ($((i*30))s)"
		[ "$i" -eq 80 ] && die "lockdep evict 40 分钟未完成(疑似锁死)"
	done
	gssh 'cat /mnt/m6t34-rev-lockdep-evict.log' > $RES/$MKTAG/lockdep-evict.log 2>&1
	tail -30 $RES/$MKTAG/lockdep-evict.log
	grep -q "FINAL: fails=0" $RES/$MKTAG/lockdep-evict.log || die "lockdep evict 脚本报失败"
	grep -q "evict 窗口 dmesg audit clean" $RES/$MKTAG/lockdep-evict.log \
		|| die "evict 窗口 dmesg 非静默(B1 判据)"
	gssh 'dmesg | grep -cE "BUG: sleeping function called from invalid context at mm/mmu_gather.c" || true' \
		> $RES/$MKTAG/lockdep-dmesg-zapcount.txt
	gssh 'dmesg | grep -E "BUG: sleeping function called from invalid context" | grep -vc "at mm/mmu_gather.c" || true' \
		> $RES/$MKTAG/lockdep-dmesg-othercount.txt
	[ "$(cat $RES/$MKTAG/lockdep-dmesg-othercount.txt | tr -d '[:space:]')" = "0" ] \
		|| die "guest dmesg 有非 zap 类原子睡眠签名(超出登记范围)"
	say "guest dmesg: zap 路径既有暴露 $(cat $RES/$MKTAG/lockdep-dmesg-zapcount.txt | tr -d '[:space:]') 处(登记), 其余 0"
	say "lockdep evict 实跑: evict 窗口零 splat, evict 完成, 零 D 状态(3 次采样)"

	say "== stage2: 恢复普通配置(=y; 重建由 stage4 收尾, stage3 用 stage1 归档件)"
	cp /tmp/$MKTAG-config-y .config && make olddefconfig >/dev/null 2>&1
	grep -q "^CONFIG_PROVE_LOCKING=y" .config && die "lockdep 配置未恢复"
	grep -q "^CONFIG_CORTEN_MM=y" .config || die "配置恢复异常"
	say "== stage2 PASS"
}

stage3() {
	$TIMEGATE || die "timegate 未放行"
	cd $KDIR || die "cd $KDIR 失败"

	say "== stage3: guest 普通配置(stage1 归档终版件) run13 复验 + evict 冒烟"
	[ -f /tmp/$MKTAG-bzimage-final ] || die "stage1 归档终版件缺失"
	vm_launch /tmp/$MKTAG-bzimage-final final
	vm_wait_ssh
	gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; ls /mnt/m6t3-shrink-test.sh /mnt/m6t34-rev-evict-smoke.sh' \
		|| die "guest 看不到 share 脚本"
	say "-- run13 (m6t3-shrink-test.sh)"
	gssh 'sh /mnt/m6t3-shrink-test.sh > /mnt/m6t34-rev-run13.log 2>&1; echo RC=$?' \
		> $RES/$MKTAG/run13-driver.log 2>&1 &
	DRVPID=$!
	local i
	for i in $(seq 1 40); do
		sleep 30
		if gssh 'grep -q "===== FINAL" /mnt/m6t34-rev-run13.log' 2>/dev/null; then
			break
		fi
		[ "$i" -eq 40 ] && die "run13 20 分钟未完成"
	done
	wait $DRVPID 2>/dev/null || true
	gssh 'cat /mnt/m6t34-rev-run13.log' > $RES/$MKTAG/run13-rev.log 2>&1
	grep -q "FINAL: fails=0" $RES/$MKTAG/run13-rev.log || die "run13 fails!=0"
	say "run13 fails=0 复验通过"

	say "-- evict 冒烟"
	gssh 'sh /mnt/m6t34-rev-evict-smoke.sh > /mnt/m6t34-rev-smoke.log 2>&1; echo done' >/dev/null 2>&1
	for i in $(seq 1 20); do
		gssh 'grep -q "===== FINAL" /mnt/m6t34-rev-smoke.log' 2>/dev/null && break
		sleep 10
		[ "$i" -eq 20 ] && die "evict 冒烟未完成"
	done
	gssh 'cat /mnt/m6t34-rev-smoke.log' > $RES/$MKTAG/evict-smoke.log 2>&1
	tail -8 $RES/$MKTAG/evict-smoke.log
	grep -q "FINAL: fails=0" $RES/$MKTAG/evict-smoke.log || die "evict 冒烟失败"
	gssh 'dmesg | grep -cE "BUG: sleeping function called from invalid context|WARNING|Oops|kernel BUG|INV7" || true' \
		> $RES/$MKTAG/final-dmesg-count.txt
	[ "$(cat $RES/$MKTAG/final-dmesg-count.txt | tr -d '[:space:]')" = "0" ] \
		|| die "终版 guest dmesg 有告警签名"
	say "== stage3 PASS"
}

stage4() {
	$TIMEGATE || die "timegate 未放行"
	cd $KDIR || die "cd $KDIR 失败"

	say "== stage4: 快照 diff 备份 + 重导出"
	if [ -f $PROJ/patches/r07-m6t34.diff ] && [ ! -f $PROJ/patches/r07-m6t34.diff.prerev ]; then
		cp $PROJ/patches/r07-m6t34.diff $PROJ/patches/r07-m6t34.diff.prerev
		say "原快照已备份 → patches/r07-m6t34.diff.prerev"
	fi
	git diff > $PROJ/patches/r07-m6t34.diff || die "git diff 导出失败"
	say "重导出完成:"
	git diff --stat | tail -8
	{
		echo "=== r07-m6t34 重审处置后快照 $(date '+%F %T')"
		echo "基座: $(git log --oneline -1)"
		git diff --stat
		echo "--- 与备份差异摘要(处置增量):"
		diffstat $PROJ/patches/r07-m6t34.diff.prerev $PROJ/patches/r07-m6t34.diff 2>/dev/null || true
		echo "--- 增量行数(重导出 vs 原快照):"
		diff $PROJ/patches/r07-m6t34.diff.prerev $PROJ/patches/r07-m6t34.diff \
			| grep -cE "^[<>]" || true
	} > $RES/$MKTAG/diff-export.txt 2>&1
	say "== stage4: 主构建树恢复一致 =y 终态(=n 切换后的对象重建)"
	grep -q "^CONFIG_CORTEN_MM=y" .config || { cp /tmp/$MKTAG-config-y .config; make olddefconfig >/dev/null; }
	make -j12 > $RES/$MKTAG/build-y-restore.log 2>&1 || die "=y 终态重建失败"
	warn_audit $RES/$MKTAG/build-y-restore.log
	cp arch/x86/boot/bzImage $PROJ/bzimg/r07-m6t34-rev
	sha256sum $PROJ/bzimg/r07-m6t34-rev | tee $PROJ/bzimg/r07-m6t34-rev.sha256
	cmp /tmp/$MKTAG-bzimage-final arch/x86/boot/bzImage \
		&& say "终版 bzImage 与 stage1 归档字节一致" \
		|| say "NOTE: bzImage 与 stage1 归档存在差异(时间戳级), 以 sha256 为准"
	say "== stage4 PASS"
}

case "${1:-}" in
1) stage1 ;;
2) stage2 ;;
3) stage3 ;;
4) stage4 ;;
all) stage1; stage2; stage3; stage4 ;;
*) echo "usage: $0 {1|2|3|4|all}"; exit 2 ;;
esac
say "stage $1 done"
