#!/bin/bash
# mv3a-guest-gate.sh -- MV3.a (execve 默认进场) main-VM guest 门驱动。
# 用法: bash mv3a-guest-gate.sh boot    # 启动主 VM (corten=on corten_mode_default=on)
#       bash mv3a-guest-gate.sh gate    # 跑全电池
#       bash mv3a-guest-gate.sh kill    # 只用 pidfile 杀 qemu
# 门清单: 参数正证据 (dmesg pr_info) / 裸 mode 探针 (无 hook LD_PRELOAD)
#   / 三进程族 mode 读数 (sshd 链/bash/systemd 子进程) + arenas 表 region
#   对拍 / smoke v2 26 双形态 (hook 形 + 裸形) / metis_eq 裸跑 checksum
#   同基准 (2d383eeed4ceb73b) / exec_default_enters 计数前进 / dmesg 静默。
# 隔离: PORT=10028, trixie-w6v2.img, qemu-mv3a.pid, monitor-mv3a.sock,
#   tmux mv3a-vm (全命名空间独立, 不与 w7v2/a1 VM 相交)。
# rc 纪律: 每个工作负载 rc 落 guest 侧文件再回读。
set -u
RES=/home/ppw/linux-6.18/project/results/r07/mv3a
SHARE=/home/ppw/bench/share
KERN=$RES/bzImage-mv3a-y
IMG=/home/ppw/vm/trixie-w6v2.img
PORT=10028
SSHKEY=/home/ppw/vm/trixie.id_rsa
PIDFILE=/home/ppw/vm/qemu-mv3a.pid
SESSION=mv3a-vm
DBG=/sys/kernel/debug/corten
BASELINE=2d383eeed4ceb73b

say() { echo "[mv3a-gate] $(date '+%F %T') $*"; }
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
	# $1 可选: off (corten=on 无 default 参数) 用于 =off 回归; 缺省 on。
	local MODE=${1:-on}
	local EXTRA=""
	[ "$MODE" = on ] && EXTRA="corten_mode_default=on"
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
		-display none -serial file:$RES/console-mv3a-$MODE.log \
		-monitor unix:/home/ppw/vm/monitor-mv3a.sock,server,nowait \
		-pidfile $PIDFILE"
	say "VM launching (mode=$MODE, kernel=$KERN, tmux=$SESSION, port=$PORT)"
	local i
	for i in $(seq 1 60); do
		if gssh 'echo up' >/dev/null 2>&1; then
			say "guest ssh up (after $i tries)"
			gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; mount -t debugfs none /sys/kernel/debug 2>/dev/null || true; ls -d /mnt/t0dod/mode-smoke /mnt/r6dg /mnt/mv3a' \
				|| die "share 挂载/目录不可见"
			return 0
		fi
		sleep 5
	done
	die "guest ssh never came up"
}

gate() {
	local out=$RES/guest-gate.log
	{
		echo "=== MV3.a guest gate (corten_mode_default=on) $(date '+%F %T') ==="
		gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; mount -t debugfs none /sys/kernel/debug 2>/dev/null || true; mkdir -p /tmp/mv3a; uname -r'
		echo "=== 0. 参数正证据 (dmesg 双 pr_info) ==="
		gssh 'dmesg | grep -i "corten:" | head -5'
		echo "=== 1. 裸 mode 探针 (无 hook LD_PRELOAD, sshd exec 链) rc 落盘 ==="
		gssh '/mnt/mv3a/mode_probe > /tmp/mv3a/probe.out 2>&1; echo PROBE_RC=$? | tee /tmp/mv3a/probe.rc; cat /tmp/mv3a/probe.out'
		echo "=== 2. 三进程族 mode 读数 (无 hook) ==="
		echo "--- 2a. bash 自身 maps arena 行 + bash -c 探针:"
		gssh 'bash -c "grep -c corten_arena /proc/self/maps; /mnt/mv3a/mode_probe"'
		echo "--- 2b. systemd 子进程族 (sshd/journald/dbus) maps arena 行:"
		gssh 'for p in $(pgrep -x sshd | head -1) $(pgrep -x systemd-journal | head -1) $(pgrep -x dbus-daemon | head -1); do echo "pid=$p comm=$(cat /proc/$p/comm) arenas=$(grep -c corten_arena /proc/$p/maps)"; done'
		echo "--- 2c. arenas 表对拍 (2b 第一行 pid 的首个 arena 地址):"
		gssh 'p=$(pgrep -x sshd | head -1); a=$(grep corten_arena /proc/$p/maps | head -1 | cut -d- -f1); echo "pid=$p addr=$a"; grep -i "$a" '$DBG'/arenas | head -2'
		echo "=== 3. smoke v2 hook 形 (26) rc 落盘 ==="
		gssh 'cd /tmp/mv3a && cp -f /mnt/t0dod/mode-smoke/corten_mode_smoke /mnt/t0dod/mode-smoke/run_mode_smoke.sh . && chmod +x corten_mode_smoke run_mode_smoke.sh && LD_PRELOAD=/mnt/r6dg/corten_mode_hook.so CORTEN_MODE_HOOK_STRICT=1 bash ./run_mode_smoke.sh > smoke-hook.full 2>&1; echo SMOKE_HOOK_RC=$?; grep -c "^PASS" smoke-hook.full; grep -c "^FAIL" smoke-hook.full; tail -3 smoke-hook.full'
		echo "=== 4. smoke v2 裸形 (无 LD_PRELOAD, 靠默认进场) rc 落盘 ==="
		gssh 'cd /tmp/mv3a && bash ./run_mode_smoke.sh > smoke-bare.full 2>&1; echo SMOKE_BARE_RC=$?; grep -c "^PASS" smoke-bare.full; grep -c "^FAIL" smoke-bare.full; tail -3 smoke-bare.full'
		echo "=== 5. metis_eq 裸跑 x2 (无 hook) checksum 同基准 ($BASELINE) ==="
		gssh 'cd /tmp/mv3a && [ -s corpus.txt ] || /mnt/r6dg/gen_text 8 corpus.txt >/dev/null 2>&1 || head -c 8388608 /dev/zero | tr "\0" a > corpus.txt; for i in 1 2; do timeout 300 /mnt/r6dg/metis_eq 2 corpus.txt > metis$i.out 2> metis$i.err; echo metis$i RC=$?; grep -o "\"distinct_words\":[0-9]*,\"checksum\":\"[a-f0-9]*\"" metis$i.out; done'
		echo "=== 6. exec_default_enters 计数 (boot 基线前进证据) ==="
		gssh "grep -E 'exec_default_enters|arenas ' $DBG/arena_stats"
		echo "=== 7. dmesg 静默审计 ==="
		gssh 'dmesg | grep -icE "corten.*(warn|bug|timed out)" || true; dmesg | grep -iE "WARNING|BUG\]" | grep -iv "non-zero pgtables" | tail -8 || true'
	} > "$out" 2>&1
	say "gate 完成, 日志 $out"
	grep -E "_RC=|^=== " "$out" | head -30
}

case "${1:-}" in
boot) vm_boot "${2:-on}" ;;
gate) gate ;;
kill) vm_kill; say killed ;;
*) echo "usage: $0 {boot|gate|kill} [on|off]"; exit 2 ;;
esac
