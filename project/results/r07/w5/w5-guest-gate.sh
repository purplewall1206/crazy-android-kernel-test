#!/bin/bash
# w5-guest-gate.sh — MV2 W-5 (植入消灭) guest 门驱动。
# 用法: bash w5-guest-gate.sh boot   # 先启动 VM (pidfile 纪律)
#       bash w5-guest-gate.sh gate   # 跑全电池
#       bash w5-guest-gate.sh kill   # 只用 pidfile 杀 qemu
# 门清单: smoke v2 26/26 双形态 / metis_eq ×2 checksum / sweep-live /
# mva1_probe / S-3 电池 / pgtables_bytes 残值==0 / registry 计数恒零 +
# j2_stale 归零 / dmesg 静默。
set -u
RES=/home/ppw/linux-6.18/project/results/r07/w5
SHARE=/home/ppw/bench/share
KERN=/home/ppw/linux-6.18-mva/arch/x86/boot/bzImage
IMG=/home/ppw/vm/trixie.img
PORT=10022
SSHKEY=/home/ppw/vm/trixie.id_rsa
PIDFILE=/home/ppw/vm/qemu.pid
SESSION=w5-vm

say() { echo "[w5gate] $(date '+%F %T') $*"; }
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
		-monitor unix:/home/ppw/vm/monitor-w5.sock,server,nowait \
		-pidfile $PIDFILE"
	say "VM launching (kernel=$KERN, tmux=$SESSION, port=$PORT)"
	local i
	for i in $(seq 1 60); do
		if gssh 'echo up' >/dev/null 2>&1; then
			say "guest ssh up (after $i tries)"
			gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; ls -d /mnt/t0dod/mode-smoke /mnt/r6dg /mnt/t1c /mnt/w45 /mnt/mve-battery' \
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
		echo "=== W-5 guest gate $(date '+%F %T') kernel=$(gssh 'uname -r') ==="
		# 基线计数 (registry 家族恒零判据的 before 值)
		gssh 'grep -E "mmap_punches|mmap_punch_rejects|placement_idle_ejects|placement_backstop|p4_ejects|implant_drops|j2_violations|j2_stale|mmap_region_routes|mmap_region_refuses" /sys/kernel/debug/corten/arena_stats'
		echo "=== 1. smoke v2 (双形态 26) ==="
		gssh 'mkdir -p /tmp/w5 && cd /tmp/w5 && cp -f /mnt/t0dod/mode-smoke/corten_mode_smoke /mnt/t0dod/mode-smoke/run_mode_smoke.sh . && chmod +x corten_mode_smoke run_mode_smoke.sh && sha256sum corten_mode_smoke | cut -c1-8 && bash ./run_mode_smoke.sh 2>&1 | tail -8; echo SMOKE_RC=$?'
		echo "=== 2. metis_eq x2 checksum ==="
		gssh 'cd /tmp/w5 && [ -s corpus.txt ] || /mnt/r6dg/gen_text 8 corpus.txt >/dev/null 2>&1 || head -c 8388608 /dev/zero | tr "\0" a > corpus.txt; for i in 1 2; do LD_PRELOAD=/mnt/r6dg/corten_mode_hook.so CORTEN_MODE_HOOK_STRICT=1 timeout 300 /mnt/r6dg/metis_eq 2 corpus.txt > metis$i.out 2> metis$i.err; echo metis$i RC=$?; grep -o "\"distinct_words\":[0-9]*,\"checksum\":\"[a-f0-9]*\"" metis$i.out; done'
		echo "=== 3. sweep-live ==="
		gssh 'cd /tmp/w5 && cp -f /mnt/w45/sweep-live . && chmod +x sweep-live && LD_PRELOAD=/mnt/r6dg/corten_mode_hook.so CORTEN_MODE_HOOK_STRICT=1 timeout 300 ./sweep-live 2>&1 | tail -6; echo SWEEP_RC=$?'
		echo "=== 4. mva1_probe ==="
		gssh 'cd /tmp/w5 && cp -f /mnt/t1c/mva1_probe . && chmod +x mva1_probe && LD_PRELOAD=/mnt/r6dg/corten_mode_hook.so CORTEN_MODE_HOOK_STRICT=1 timeout 300 ./mva1_probe > probe.out 2>&1; echo PROBE_RC=$?; grep -cE "^\[ok\]" probe.out; grep -E "probe done|S-1|S-4|FRESH|CHUNK|MemFree" probe.out | tail -8'
		echo "=== 5. S-3 battery ==="
		gssh 'sh /mnt/mve-battery/mve_s3_swapoff.sh 2>&1 | tail -6; echo S3_RC=$?'
		echo "=== 6. registry 家族终读数 + W-5 正证据 ==="
		gssh 'grep -E "mmap_punches|mmap_punch_rejects|placement_idle_ejects|placement_backstop|p4_ejects|implant_drops|j2_violations|j2_stale|mmap_region_routes|mmap_region_refuses" /sys/kernel/debug/corten/arena_stats'
		echo "=== 7. pgtables_bytes 残值 (判据==0) ==="
		gssh 'dmesg | grep -c "non-zero pgtables_bytes" || true'
		echo "=== 8. dmesg 静默审计 ==="
		gssh 'dmesg | grep -icE "corten.*(warn|bug|timed out)" || true; dmesg | grep -iE "corten|WARNING|BUG" | tail -12 || true'
	} > "$out" 2>&1
	say "gate 完成, 日志 $out"
	grep -E "SMOKE_RC|SWEEP_RC|PROBE_RC|S3_RC|=== " "$out"
}

case "${1:-}" in
boot) vm_boot ;;
gate) gate ;;
kill) vm_kill; say killed ;;
*) echo "usage: $0 {boot|gate|kill}"; exit 2 ;;
esac
