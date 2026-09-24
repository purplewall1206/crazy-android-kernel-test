#!/bin/bash
# mva2-verify.sh - M-V A.2a/A.2b 夜间验证矩阵 (worktree 感知版 corten-verify.sh)
#
# 用法:
#   bash mva2-verify.sh kunit-y      # =y 构建 + 无盘 qemu KUnit corten* (corten=on)
#   bash mva2-verify.sh kunit-plain  # =y 配置原样 (corten=off 默认) 跑 KUnit
#   bash mva2-verify.sh kunit-das    # DEBUG_ATOMIC_SLEEP 变体
#   bash mva2-verify.sh kunit-lk     # PROVE_LOCKING 变体 (结束后恢复 .config)
#   bash mva2-verify.sh n-objects    # =n 折叠: 13 对象构建 + nm 零符号 (恢复 .config)
#   bash mva2-verify.sh bzimage      # =y 全量构建出 bzImage (guest 用)
set -u
TREE=/home/ppw/linux-6.18-mva
LOGDIR=${2:-/home/ppw/cortenmm/results/r07/mva2}
SAVED=/tmp/mva2build/config-y-backup
QEMU_TIMEOUT=480

mkdir -p "$LOGDIR"
die() { echo "[mva2-verify] FAIL: $*"; exit 1; }

run_kunit() {
	local extra="$1" log="$2"
	cd "$TREE" || die "no tree"
	timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
		-enable-kvm -m 2048 -smp 4 \
		-kernel "$TREE/arch/x86/boot/bzImage" \
		-append "console=ttyS0 panic=-1 $extra" \
		-nographic -no-reboot >"$log" 2>&1
	grep -E "possible (recursive locking|deadlock)|inconsistent lock state|unsafe locking scenario|bad unlock balance|DEBUG_LOCKS_WARN|BUG: unable to handle|kernel BUG|general protection fault" "$log" &&
		die "内核日志出现 lockdep/oops 签名, 见 $log"
	grep -q "# Subtest: corten" "$log" || die "日志中无 corten KUnit 套件, 见 $log"
	grep -q "not ok" "$log" && die "KUnit 存在失败用例, 见 $log"
	grep -E "# corten: pass:[0-9]+ fail:0" "$log" ||
		die "未见 corten 套件全绿摘要, 见 $log"
	echo "[mva2-verify] KUnit 全绿 ($log)"
	grep -E "# corten: pass:" "$log"
	return 0
}

case "${1:-}" in
kunit-y)
	cd "$TREE"
	grep -q "^CONFIG_CORTEN_MM=y" .config || die "CORTEN_MM != y"
	make -j12 >"$LOGDIR/build-y.log" 2>&1 || die "=y 构建失败"
	run_kunit "corten=on kunit.filter_glob=corten*" "$LOGDIR/kunit-on1.log"
	;;
kunit-on2)
	cd "$TREE"
	run_kunit "corten=on kunit.filter_glob=corten*" "$LOGDIR/kunit-on2.log"
	;;
kunit-plain)
	cd "$TREE"
	run_kunit "kunit.filter_glob=corten*" "$LOGDIR/kunit-off1.log"
	;;
kunit-das)
	cd "$TREE"
	cp .config "$SAVED" || die "无法备份 .config"
	restore_das() { cp "$SAVED" .config; make olddefconfig >/dev/null 2>&1; echo "[mva2-verify] .config 已恢复"; }
	trap restore_das EXIT
	./scripts/config -e DEBUG_ATOMIC_SLEEP && make olddefconfig || die "config 失败"
	make -j12 >"$LOGDIR/build-das.log" 2>&1 || die "DAS 构建失败"
	run_kunit "corten=on kunit.filter_glob=corten*" "$LOGDIR/kunit-das.log"
	;;
kunit-lk)
	cd "$TREE"
	cp .config "$SAVED" || die "无法备份 .config"
	restore_lk() { cp "$SAVED" .config; make olddefconfig >/dev/null 2>&1; echo "[mva2-verify] .config 已恢复"; }
	trap restore_lk EXIT
	./scripts/config -e PROVE_LOCKING && make olddefconfig || die "config 失败"
	make -j12 >"$LOGDIR/build-lk.log" 2>&1 || die "lockdep 构建失败"
	run_kunit "corten=on kunit.filter_glob=corten*" "$LOGDIR/kunit-lk.log"
	;;
n-objects)
	cd "$TREE"
	cp .config "$SAVED" || die "无法备份 .config"
	restore_n() { cp "$SAVED" .config; make olddefconfig >/dev/null 2>&1; echo "[mva2-verify] .config 已恢复(=y)"; }
	trap restore_n EXIT
	./scripts/config -d CORTEN_MM -d CORTEN_MM_ARENA \
		-d CORTEN_MM_KUNIT_TEST -d CORTEN_MM_ARENA_KUNIT_TEST \
		-d CORTEN_MM_ARENA_FAULT_KUNIT_TEST && make olddefconfig || die "config 失败"
	grep -q "^CONFIG_CORTEN_MM=y" .config && die "=n 未生效"
	OBJ="mm/mmap.o mm/mmap_lock.o mm/memory.o mm/mprotect.o mm/madvise.o mm/mremap.o mm/rmap.o mm/gup.o mm/oom_kill.o mm/swapfile.o mm/migrate.o mm/mempolicy.o arch/x86/mm/fault.o arch/x86/kernel/sys_x86_64.o"
	make $OBJ >"$LOGDIR/build-n.log" 2>&1 || die "=n 对象构建失败"
	for o in $OBJ; do
		nm "$o" | grep -qi corten && die "$o 含 corten 符号 (=n 未折叠)"
	done
	echo "[mva2-verify] =n 十四对象 RC=0 零警告 + nm 零 corten 符号"
	;;
bzimage)
	cd "$TREE"
	grep -q "^CONFIG_CORTEN_MM=y" .config || die "CORTEN_MM != y"
	make -j12 >"$LOGDIR/build-bz.log" 2>&1 || die "bzImage 构建失败"
	cp -f arch/x86/boot/bzImage "$LOGDIR/bzImage-mva2-y"
	sha256sum "$LOGDIR/bzImage-mva2-y"
	;;
*)
	die "用法: mva2-verify.sh {kunit-y|kunit-on2|kunit-plain|kunit-das|kunit-lk|n-objects|bzimage} [logdir]"
	;;
esac
echo "[mva2-verify] $1 PASS"
