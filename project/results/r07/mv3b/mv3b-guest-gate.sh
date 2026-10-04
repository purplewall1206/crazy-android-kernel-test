#!/bin/bash
# mv3b-guest-gate.sh -- MV3.b (无损闭合清单) guest 门驱动。
# 用法: bash mv3b-guest-gate.sh head   # =on 带 journal 全启动 (MV3.d 前置硬门, 头项判据)
#       bash mv3b-guest-gate.sh boot   # 启动主 VM (masked journald 四件套, 与 mv3a 电池同形)
#       bash mv3b-guest-gate.sh gate   # 跑全电池 (含 arena_stats churn 计时)
#       bash mv3b-guest-gate.sh off    # =off 回归 boot+回归读数
#       bash mv3b-guest-gate.sh kill   # 只用 pidfile 杀 qemu
# 头项判据: journald 不 mask 的 =on boot -- systemd 全启动 (tmpfiles-setup-dev-early
#   完成), journalctl 有完整 journal, dmesg 无 mm.h:2648 WARN, journald 无 watchdog kill。
# 隔离: PORT=10029, trixie-w6v2.img, qemu-mv3b.pid, monitor-mv3b.sock, tmux mv3b-vm
#   (全命名空间独立, 不与 mv3a/w7v2/a1 VM 相交)。
# rc 纪律: 每个工作负载 rc 落 guest 侧文件再回读。
set -u
RES=/home/ppw/linux-6.18/project/results/r07/mv3b
SHARE=/home/ppw/bench/share
KERN=$RES/bzImage-mv3b-y
IMG=/home/ppw/vm/trixie-w6v2.img
PORT=10029
SSHKEY=/home/ppw/vm/trixie.id_rsa
PIDFILE=/home/ppw/vm/qemu-mv3b.pid
SESSION=mv3b-vm
DBG=/sys/kernel/debug/corten
BASELINE=2d383eeed4ceb73b
# mv3a 电池同形的 journald 四件套 mask (masked 形态; head 形态不 mask)。
JMASK="systemd.mask=systemd-journald.service systemd.mask=systemd-journald.socket systemd.mask=systemd-journald-dev-log.socket systemd.mask=systemd-journal-flush.service"

say() { echo "[mv3b-gate] $(date '+%F %T') $*"; }
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

vm_boot() {
	# $1: journal (带 journal, 头项判据) | masked (journald 四件套 mask) | off (=off 回归)
	local MODE=${1:-masked}
	local EXTRA=""
	local CONSOLE=console-mv3b-masked.log
	case "$MODE" in
		journal) EXTRA="corten_mode_default=on"; CONSOLE=console-mv3b-journal.log ;;
		masked)  EXTRA="corten_mode_default=on $JMASK" ;;
		off)     CONSOLE=console-mv3b-off.log ;;
		*) die "bad mode $MODE" ;;
	esac
	vm_kill
	tmux new-session -d -s $SESSION \
		"qemu-system-x86_64 -enable-kvm -cpu host -m 4096 -smp 8 \
		-kernel $KERN \
		-append 'console=ttyS0 root=/dev/vda rw net.ifnames=0 systemd.mask=sys-kernel-config.mount corten=on $EXTRA mitigations=off kunit.enable=0 log_buf_len=16M' \
		-drive file=$IMG,if=virtio,format=raw \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
		-device virtio-net-pci,netdev=net0 \
		-fsdev local,id=fs0,path=$SHARE,security_model=none \
		-device virtio-9p-pci,fsdev=fs0,mount_tag=hostshare \
		-display none -serial file:$RES/$CONSOLE \
		-monitor unix:/home/ppw/vm/monitor-mv3b.sock,server,nowait \
		-pidfile $PIDFILE"
	say "VM launching (mode=$MODE, kernel=$KERN, tmux=$SESSION, port=$PORT)"
	local i
	for i in $(seq 1 60); do
		if gssh 'echo up' >/dev/null 2>&1; then
			say "guest ssh up (after $i tries)"
			gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; mount -t debugfs none /sys/kernel/debug 2>/dev/null || true; mkdir -p /tmp/mv3b' \
				|| die "share/debugfs 挂载失败"
			return 0
		fi
		sleep 5
	done
	die "guest ssh never came up"
}

# arena_stats churn 计时: 先造 churn (metis 裸跑 + 循环 churn), 再计时读。
# $1 = 标签
churn_read() {
	local TAG=${1:-churn}
	gssh "cd /tmp/mv3b && [ -s corpus.txt ] || /mnt/r6dg/gen_text 8 corpus.txt >/dev/null 2>&1 || head -c 8388608 /dev/zero | tr '\0' a > corpus.txt; \
		(timeout 60 /mnt/r6dg/metis_eq 1 corpus.txt > churn-metis.out 2>&1 &) ; \
		(for i in \$(seq 1 20); do grep -c corten_arena /proc/self/maps >/dev/null; grep arenas $DBG/arenas >/dev/null; sleep 0.2; done &) ; \
		sleep 3; /usr/bin/time -f 'arena_stats read: %es' timeout 120 cat $DBG/arena_stats > stats-$TAG.out 2> stats-$TAG.time; echo CHURN_READ_RC=\$?; cat stats-$TAG.time; grep -E 'arenas |exec_default_enters|remote_win_short|gup_probes' stats-$TAG.out"
}

head_gate() {
	local out=$RES/head-journal.log
	{
		echo "=== MV3.b HEAD 门: =on 带 journal 全启动 $(date '+%F %T') ==="
		gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; mount -t debugfs none /sys/kernel/debug 2>/dev/null || true; uname -r; mkdir -p /tmp/mv3b'
		echo "=== H1. systemd 全启动判定 (is-system-running + failed 单元) ==="
		gssh 'systemctl is-system-running; echo RC=$?; systemctl --failed --no-legend | head; echo "--- tmpfiles-setup-dev-early:"; systemctl show -p ActiveState -p ExecMainStatus systemd-tmpfiles-setup-dev-early.service; journalctl -b -u systemd-tmpfiles-setup-dev-early.service --no-pager | tail -3'
		echo "=== H2. journald 健康 (active, 无 watchdog kill) ==="
		gssh 'systemctl is-active systemd-journald.service; journalctl -b --no-pager | wc -l; journalctl -b --no-pager | grep -ci "watchdog" || true'
		echo "=== H3. journal 完整性抽验 (启动期内核行 + 服务行在案) ==="
		gssh 'journalctl -b --no-pager | grep -m1 "Linux version"; journalctl -b --no-pager | grep -c "systemd\[1\]"'
		echo "=== H4. 裸 mode 探针 (journal 形态下) ==="
		gssh '/mnt/mv3a/mode_probe > /tmp/mv3b/probe.out 2>&1; echo PROBE_RC=$?; cat /tmp/mv3b/probe.out | head -20'
		echo "=== H5. cmdline 读数面正证 (procPs cmdline 非空 + remote_win_short 计数) ==="
		gssh 'for p in $(pgrep -x systemd | head -1) $(pgrep -x sshd | head -1); do echo "pid=$p cmdline=[$(cat /proc/$p/cmdline | tr "\0" " ")]"; done; grep -E "remote_win_short|remote_access" '$DBG'/arena_stats'
		echo "=== H6. arena_stats churn 计时 (journal 形态, 头项 churn 环境) ==="
		churn_read journal
		echo "=== H7. dmesg 静默审计 (无 2648 WARN / 无 watchdog kill / 无新 WARN) ==="
		gssh 'dmesg | grep -c "mm.h:2648" || true; dmesg | grep -iE "WARNING|BUG\]" | grep -v "non-zero pgtables" | tail -8 || true; dmesg | grep -icE "watchdog" || true'
	} > "$out" 2>&1
	say "HEAD 门完成, 日志 $out"
	grep -E "_RC=|^=== |is-system|ActiveState" "$out" | head -20
}

gate() {
	local out=$RES/guest-gate.log
	{
		echo "=== MV3.b guest gate (masked journald, corten_mode_default=on) $(date '+%F %T') ==="
		echo "=== 0. 参数正证据 ==="
		gssh 'dmesg | grep -i "corten:" | head -5'
		echo "=== 1. 裸 mode 探针 ==="
		gssh '/mnt/mv3a/mode_probe > /tmp/mv3b/probe.out 2>&1; echo PROBE_RC=$?; head -20 /tmp/mv3b/probe.out'
		echo "=== 2. 三进程族 mode 读数 ==="
		gssh 'for p in $(pgrep -x sshd | head -1) $(pgrep -x systemd | head -1) $(pgrep -x dbus-daemon | head -1); do echo "pid=$p comm=$(cat /proc/$p/comm) arenas=$(grep -c corten_arena /proc/$p/maps) cmdline_len=$(wc -c < /proc/$p/cmdline)"; done'
		echo "=== 3. smoke v2 裸形 (26) rc 落盘 ==="
		gssh 'cd /tmp/mv3b && cp -f /mnt/t0dod/mode-smoke/corten_mode_smoke /mnt/t0dod/mode-smoke/run_mode_smoke.sh . && chmod +x corten_mode_smoke run_mode_smoke.sh && bash ./run_mode_smoke.sh > smoke-bare.full 2>&1; echo SMOKE_BARE_RC=$?; grep -c "^PASS" smoke-bare.full; grep -c "^FAIL" smoke-bare.full; tail -3 smoke-bare.full'
		echo "=== 4. metis_eq 裸跑 x2 checksum 同基准 ($BASELINE) ==="
		gssh 'cd /tmp/mv3b && [ -s corpus.txt ] || /mnt/r6dg/gen_text 8 corpus.txt >/dev/null 2>&1 || head -c 8388608 /dev/zero | tr "\0" a > corpus.txt; for i in 1 2; do timeout 300 /mnt/r6dg/metis_eq 2 corpus.txt > metis$i.out 2> metis$i.err; echo metis$i RC=$?; grep -o "\"distinct_words\":[0-9]*,\"checksum\":\"[a-f0-9]*\"" metis$i.out; done'
		echo "=== 5. arena_stats churn 计时 (masked 形态) ==="
		churn_read masked
		echo "=== 6. counters (exec_default_enters 前进 + remote_win_short 语义) ==="
		gssh "grep -E 'exec_default_enters|arenas |remote_win_short|gup_probes|madvise_hints|madvise_parked' $DBG/arena_stats"
		echo "=== 7. dmesg 静默审计 ==="
		gssh 'dmesg | grep -c "mm.h:2648" || true; dmesg | grep -iE "WARNING|BUG\]" | grep -iv "non-zero pgtables" | tail -8 || true'
	} > "$out" 2>&1
	say "gate 完成, 日志 $out"
	grep -E "_RC=|^=== " "$out" | head -30
}

gate_off() {
	local out=$RES/regress-off.log
	{
		echo "=== MV3.b =off 回归 (corten=on 无 default 参数) $(date '+%F %T') ==="
		echo "=== 分离证明: 裸探针 mode=0 ==="
		gssh '/mnt/mv3a/mode_probe > /tmp/mv3b/probe-off.out 2>&1; echo PROBE_RC=$?; head -8 /tmp/mv3b/probe-off.out'
		echo "=== smoke 裸形 (=off 世界) ==="
		gssh 'cd /tmp/mv3b && bash ./run_mode_smoke.sh > smoke-off.full 2>&1; echo SMOKE_OFF_RC=$?; grep -c "^PASS" smoke-off.full; grep -c "^FAIL" smoke-off.full'
		echo "=== dmesg 静默 ==="
		gssh 'dmesg | grep -icE "corten.*(warn|bug|timed out)" || true; dmesg | grep -iE "WARNING|BUG\]" | tail -5 || true'
	} > "$out" 2>&1
	say "=off 回归完成, 日志 $out"
	grep -E "_RC=|^=== " "$out" | head -12
}

case "${1:-}" in
head)  vm_boot journal; head_gate ;;
boot)  vm_boot "${2:-masked}" ;;
bootgrep) vm_boot journal; sleep 45; grep -cE "Bad page|NULL pointer|try_grab_folio|still mapped|mm.h:2648" $RES/console-mv3b-journal.log || true; tail -4 $RES/console-mv3b-journal.log | sed 's/\x1b\[[0-9;]*m//g' | cut -c1-120; bash $0 kill; echo BOOTGREP_DONE ;;
gate)  gate ;;
gate)  gate ;;
off)   vm_boot off; gate_off ;;
kill)  vm_kill; say killed ;;
*) echo "usage: $0 {head|boot|gate|off|kill}"; exit 2 ;;
esac
