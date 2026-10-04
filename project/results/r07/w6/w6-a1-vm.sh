#!/bin/bash
# w6-a1-vm.sh — MV2 W-6 J3 跨内核对拍的 A.1 基线 VM 驱动。
# 用法: bash w6-a1-vm.sh boot    # bzImage-mva1 + trixie-a1.img, 端口 10025
#       bash w6-a1-vm.sh snap    # smoke v2 快验 + J3 oracle 快照 (guest 本地盘副本)
#       bash w6-a1-vm.sh kill
# 隔离: PORT=10025, trixie-a1.img (trixie.img 副本), qemu-a1.pid,
# monitor-a1.sock, tmux a1-vm — 与主 VM (10026/trixie-w6v2) 全量隔离。
# 坑清单口径: A.1 快照跑现代 workload 前先跑 smoke v2 快验并如实记录
# (D32 拒绝臂/W-4 sweep 在 A.1 不存在, form-B 预期差异 → 对拍范围按
# 任务书收缩到 maps 行集并在报告披露)。
set -u
RES=/home/ppw/linux-6.18/project/results/r07/w6
SHARE=/home/ppw/bench/share
KERN=/home/ppw/cortenmm/bzimg/r07-mva1/bzImage-mva1
IMG=/home/ppw/vm/trixie-a1.img
PORT=10025
SSHKEY=/home/ppw/vm/trixie.id_rsa
PIDFILE=/home/ppw/vm/qemu-a1.pid
SESSION=a1-vm
DBG=/sys/kernel/debug/corten

say() { echo "[a1vm] $(date '+%F %T') $*"; }
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
	vm_kill
	tmux new-session -d -s $SESSION \
		"qemu-system-x86_64 -enable-kvm -cpu host -m 4096 -smp 8 \
		-kernel $KERN \
		-append 'console=ttyS0 root=/dev/vda rw net.ifnames=0 systemd.mask=sys-kernel-config.mount corten=on mitigations=off kunit.enable=0 log_buf_len=16M' \
		-drive file=$IMG,if=virtio,format=raw \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
		-device virtio-net-pci,netdev=net0 \
		-fsdev local,id=fs0,path=$SHARE,security_model=none \
		-device virtio-9p-pci,fsdev=fs0,mount_tag=hostshare \
		-display none -serial stdio \
		-monitor unix:/home/ppw/vm/monitor-a1.sock,server,nowait \
		-pidfile $PIDFILE"
	say "A.1 VM launching (kernel=$KERN, tmux=$SESSION, port=$PORT)"
	local i
	for i in $(seq 1 60); do
		if gssh 'echo up' >/dev/null 2>&1; then
			say "guest ssh up (after $i tries)"
			gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; ls -d /mnt/mvc-oracle /mnt/t0dod/mode-smoke' \
				|| die "share 挂载/目录不可见"
			return 0
		fi
		sleep 5
	done
	die "guest ssh never came up"
}

snap() {
	local out=$RES/a1-snap.log
	{
		echo "=== A.1 baseline $(date '+%F %T') uname=$(gssh 'uname -r') ==="
		gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; mount -t debugfs none /sys/kernel/debug 2>/dev/null || true; mkdir -p /root/j3d /tmp/w6; cp -f /mnt/mvc-oracle/run_mvc_oracle.sh /mnt/mvc-oracle/mvc_j3_workload /root/j3d/ && chmod +x /root/j3d/* && stat -c "j3w dev=%F maj:min=%t:%T inode=%i bytes=%s" /root/j3d/mvc_j3_workload'
		echo "=== 1. smoke v2 快验 (预期 form-B 差异, 如实记录) ==="
		gssh 'cd /tmp/w6 && cp -f /mnt/t0dod/mode-smoke/corten_mode_smoke /mnt/t0dod/mode-smoke/run_mode_smoke.sh . && chmod +x corten_mode_smoke run_mode_smoke.sh && sha256sum corten_mode_smoke | cut -c1-8 && bash ./run_mode_smoke.sh > smoke.full 2>&1; echo SMOKE_RC=$?; tail -10 smoke.full; grep -c "^PASS" smoke.full; grep -c "^FAIL" smoke.full; grep "^FAIL" smoke.full'
		echo "=== 2. J3 oracle 快照 (A.1-compat 变体: 注册件依赖 V-C 的 process_vm_readv 路由, A.1 上 rc=3 不落快照 — 已实证; 变体只跳该腿, maps 面不变, 同名同路径) ==="
		scp -i $SSHKEY -P $PORT -o StrictHostKeyChecking=no \
			-o UserKnownHostsFile=/dev/null \
			"$RES/tools/mvc_j3_workload_a1" \
			root@127.0.0.1:/root/j3d/mvc_j3_workload \
			|| die "变体上传失败"
		gssh 'stat -c "j3w(a1-variant) inode=%i bytes=%s" /root/j3d/mvc_j3_workload; J3_SKIP_VMREADV=1 setsid setarch -R /root/j3d/mvc_j3_workload /tmp/j3-snap > /tmp/w6/j3live.out 2>&1 < /dev/null & for i in $(seq 100); do grep -q ready /tmp/w6/j3live.out 2>/dev/null && break; sleep 0.1; done; grep ready /tmp/w6/j3live.out; pgrep -f mvc_j3_workload | head -1'
		gssh 'pkill -TERM -f mvc_j3_workload; sleep 1; ls -la /tmp/j3-snap/; echo J3_A1_SNAP_DONE'
		echo "=== 3. A.1 侧 audit_gate (存在与否如实记录) ==="
		gssh "cat $DBG/audit_gate 2>/dev/null || echo 'audit_gate 不可读'"
		echo "=== 4. dmesg corten 安静 ==="
		gssh 'dmesg | grep -icE "corten.*(warn|bug|timed out)" || true; dmesg | grep -iE "corten|WARNING|BUG" | tail -8 || true'
	} > "$out" 2>&1
	say "a1 snap 完成, 日志 $out"
	grep -E "_RC=|^=== " "$out"
	scp -i $SSHKEY -P $PORT -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -r \
		root@127.0.0.1:/tmp/j3-snap "$RES/j3-a1-snap" \
		&& say "快照已取回 $RES/j3-a1-snap" || die "scp 快照失败"
}

case "${1:-}" in
boot) vm_boot ;;
snap) snap ;;
kill) vm_kill; say killed ;;
*) echo "usage: $0 {boot|snap|kill}"; exit 2 ;;
esac
