#!/bin/bash
# corten-verify.sh — CortenMM KUnit 验证脚本 (r02 起的正本; bench/corten-lockdep-build.sh
# 是其前身, 由本脚本取代)。
#
# 无盘 qemu 直启 bzImage 跑 corten KUnit 套件, 三个子命令:
#
#   lockdep  PROVE_LOCKING 变体: 切配置(-e PROVE_LOCKING -e CORTEN_MM
#            -e CORTEN_MM_KUNIT_TEST) + olddefconfig + make -j12 + 无盘 qemu
#            跑 corten KUnit 全套; 断言全绿且 dmesg 无 lockdep 报警;
#            无论成败最后恢复原 .config (存 /tmp/config.corten-y)。
#   on       corten=on 变体: 当前配置下构建后用 corten=on 启动无盘 qemu 跑
#            KUnit (回归 fix1 的 static-key 延迟翻转, 顺带覆盖互锁/
#            ensure-alloc/bh-uninstall 在 gate 开启时的行为)。
#   plain    普通配置变体: 当前配置构建, 无盘 qemu 跑 KUnit N 次 (默认 3),
#            用于 flake 检查。
#
# 用法:
#   bash corten-verify.sh lockdep [logdir]
#   bash corten-verify.sh on      [logdir]
#   bash corten-verify.sh plain   [logdir] [runs]
# logdir 默认 /home/ppw/cortenmm/results/r02
#
# 注意: 必须在实验窗口 (23:00-09:00 CST) 运行:
#   bash /home/ppw/cortenmm/bin/timegate.sh && bash corten-verify.sh <mode>
set -u

TREE=/home/ppw/linux-6.18
LOGDIR=${2:-/home/ppw/cortenmm/results/r02}
SAVED=/tmp/config.corten-y
QEMU_TIMEOUT=420

mkdir -p "$LOGDIR"

die() { echo "[corten-verify] FAIL: $*"; exit 1; }

run_kunit() {                       # $1=append-extra-args $2=logfile
	local extra="$1" log="$2"
	timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
		-enable-kvm -m 2048 -smp 4 \
		-kernel "$TREE/arch/x86/boot/bzImage" \
		-append "console=ttyS0 panic=-1 $extra" \
		-nographic -no-reboot >"$log" 2>&1
	# panic=-1 + -no-reboot: 内核跑完 KUnit 后因无 rootfs panic 退出, qemu 返回非 0 属预期,
	# 以日志内容为准。
	# 注意: corten_test_txn_path_overflow 会按设计触发 WARN_ON_ONCE
	# (先 descent level 守卫后 PATH_MAX 守卫, 用例本身断言 -EPROTO),
	# 所以这里只匹配 lockdep/oops 的精确签名, 不做泛化 WARNING 匹配。
	grep -E "possible (recursive locking|deadlock)|inconsistent lock state|unsafe locking scenario|bad unlock balance|DEBUG_LOCKS_WARN|BUG: unable to handle|kernel BUG|general protection fault" "$log" &&
		die "内核日志出现 lockdep/oops 签名, 见 $log"
	grep -q "# Subtest: corten" "$log" || die "日志中无 corten KUnit 套件, 见 $log"
	if grep -q "not ok" "$log"; then
		die "KUnit 存在失败用例, 见 $log"
	fi
	# 行首有 dmesg 时间戳前缀, 不能锚定 "^#"。
	grep -E "# corten: pass:[0-9]+ fail:0" "$log" ||
		die "未见 corten 套件全绿摘要, 见 $log"
	echo "[corten-verify] KUnit corten 套件全绿 ($log)"
	grep -E "# corten: pass:" "$log"
	return 0
}

cmd_lockdep() {
	cd "$TREE" || die "无法进入 $TREE"
	cp .config "$SAVED" || die "无法备份 .config"
	restore() {
		cp "$SAVED" .config
		make olddefconfig >/dev/null 2>&1
		echo "[corten-verify] 已恢复原 .config"
	}
	trap restore EXIT

	./scripts/config -e KUNIT -e PROVE_LOCKING -e CORTEN_MM \
		-e CORTEN_MM_KUNIT_TEST || die "scripts/config 失败"
	make olddefconfig || die "olddefconfig 失败"
	grep -q "^CONFIG_PROVE_LOCKING=y" .config || die "PROVE_LOCKING 未生效"
	grep -q "^CONFIG_CORTEN_MM=y" .config || die "CORTEN_MM 未生效"
	grep -q "^CONFIG_CORTEN_MM_KUNIT_TEST=y" .config || die "KUNIT_TEST 未生效"

	echo "[corten-verify] PROVE_LOCKING 变体构建中 (日志: $LOGDIR/lockdep-build.log)"
	make -j12 >"$LOGDIR/lockdep-build.log" 2>&1 ||
		die "lockdep 构建失败, 见 $LOGDIR/lockdep-build.log"
	[ -f arch/x86/boot/bzImage ] || die "bzImage 缺失"

	echo "[corten-verify] 无盘 qemu 跑 corten KUnit (lockdep 变体)"
	run_kunit "kunit.filter_glob=corten" "$LOGDIR/lockdep-kunit.log"
	echo "[corten-verify] lockdep 变体: PASS (构建零错误 + KUnit 全绿 + 无 lockdep 报警)"
}

cmd_on() {
	cd "$TREE" || die "无法进入 $TREE"
	echo "[corten-verify] corten=on 变体: 确认配置与构建"
	grep -q "^CONFIG_CORTEN_MM=y" .config || die "当前配置 CORTEN_MM != y"
	make -j12 >"$LOGDIR/on-build.log" 2>&1 ||
		die "构建失败, 见 $LOGDIR/on-build.log"
	echo "[corten-verify] 无盘 qemu corten=on 跑 KUnit (回归 fix1 + 3a)"
	run_kunit "corten=on kunit.filter_glob=corten" "$LOGDIR/on-kunit.log"
	grep -q "corten: page descriptors enabled" "$LOGDIR/on-kunit.log" ||
		die "未见 fix1 的 initcall 阶段翻转日志, 见 $LOGDIR/on-kunit.log"
	echo "[corten-verify] corten=on 变体: PASS"
}

cmd_plain() {
	local runs=${3:-3} i
	cd "$TREE" || die "无法进入 $TREE"
	echo "[corten-verify] 普通配置变体: 构建后跑 $runs 次 KUnit"
	grep -q "^CONFIG_CORTEN_MM=y" .config || die "当前配置 CORTEN_MM != y"
	make -j12 >"$LOGDIR/plain-build.log" 2>&1 ||
		die "构建失败, 见 $LOGDIR/plain-build.log"
	for i in $(seq 1 "$runs"); do
		echo "[corten-verify] plain run $i/$runs"
		run_kunit "kunit.filter_glob=corten" \
			"$LOGDIR/plain-kunit-run$i.log"
	done
	echo "[corten-verify] plain 变体: PASS ($runs 次全绿, 无 flake)"
}

case "${1:-}" in
lockdep) cmd_lockdep ;;
on)      cmd_on ;;
plain)   cmd_plain ;;
*)
	echo "用法: $0 {lockdep|on|plain} [logdir] [runs(仅 plain)]" >&2
	exit 2
	;;
esac
