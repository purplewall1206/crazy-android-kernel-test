#!/bin/bash
# w6-guest-gate.sh — MV2 W-6 (终判据电池) main-VM guest 门驱动。
# 用法: bash w6-guest-gate.sh boot   # 启动主 VM (pidfile 纪律, 端口 10026)
#       bash w6-guest-gate.sh gate   # 跑全电池
#       bash w6-guest-gate.sh kill   # 只用 pidfile 杀 qemu
# 门清单: 基线计数 / smoke v2 26 双形态 / JTB 2000x3 x3 / metis_eq x2 /
# sweep-live / mva1_probe / S-3 双分支 / J3 oracle (注册驱动 in-boot 断言
# + 快照; 手动第二跑做 live wl/tree audit) / 终读数。
# 隔离: PORT=10026, trixie-w6v2.img, qemu-w6v2.pid, monitor-w6v2.sock,
# tmux w6v2-vm (A.1 基线 VM 走 w6-a1-vm.sh, 全量隔离)。
# rc 纪律: 每个工作负载 rc 落 guest 侧文件再回读 (不吃管道尾 rc)。
set -u
RES=/home/ppw/linux-6.18/project/results/r07/w6
SHARE=/home/ppw/bench/share
KERN=/home/ppw/linux-6.18-mva/arch/x86/boot/bzImage
IMG=/home/ppw/vm/trixie-w6v2.img
PORT=10026
SSHKEY=/home/ppw/vm/trixie.id_rsa
PIDFILE=/home/ppw/vm/qemu-w6v2.pid
SESSION=w6v2-vm
DBG=/sys/kernel/debug/corten

say() { echo "[w6gate] $(date '+%F %T') $*"; }
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
		-monitor unix:/home/ppw/vm/monitor-w6v2.sock,server,nowait \
		-pidfile $PIDFILE"
	say "VM launching (kernel=$KERN, tmux=$SESSION, port=$PORT)"
	local i
	for i in $(seq 1 60); do
		if gssh 'echo up' >/dev/null 2>&1; then
			say "guest ssh up (after $i tries)"
			gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; mount -t debugfs none /sys/kernel/debug 2>/dev/null || true; ls -d /mnt/t0dod/mode-smoke /mnt/r6dg /mnt/t1c /mnt/w45 /mnt/mve-battery /mnt/mvc-oracle' \
				|| die "share 挂载/目录不可见"
			return 0
		fi
		sleep 5
	done
	die "guest ssh never came up"
}

# wl/tree live audit: audit_gate 全文前后快照夹一次 echo pid > whitelist。
audit_pid() {  # $1=pid  $2=tag
	{
		echo "--- audit $2 pid=$1 pre"
		gssh "cat $DBG/audit_gate"
		echo "--- wl-write rc:"
		gssh "echo $1 > $DBG/whitelist; echo wl_write_rc=\$?"
		echo "--- audit $2 pid=$1 post"
		gssh "cat $DBG/audit_gate"
	}
}

gate() {
	local out=$RES/guest-gate.log
	{
		echo "=== W-6 guest gate $(date '+%F %T') ==="
		gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; mount -t debugfs none /sys/kernel/debug 2>/dev/null || true; mkdir -p /tmp/w6; uname -r'
		echo "=== 0. 基线计数 ==="
		gssh "cat $DBG/audit_gate"
		echo "--- arena_stats 关键族"
		gssh "grep -E 'mmap_punches|mmap_punch_rejects|placement|implant_drops|j2_|mmap_region|sweep_|maps_window_rows|gup_probes|arenas ' $DBG/arena_stats"
		echo "--- arenas 行数 (boot 基线)"
		gssh "wc -l < $DBG/arenas"
		echo "=== 1. smoke v2 (双形态 26) ==="
		gssh 'cd /tmp/w6 && cp -f /mnt/t0dod/mode-smoke/corten_mode_smoke /mnt/t0dod/mode-smoke/run_mode_smoke.sh . && chmod +x corten_mode_smoke run_mode_smoke.sh && sha256sum corten_mode_smoke | cut -c1-8 && bash ./run_mode_smoke.sh > smoke.full 2>&1; echo SMOKE_RC=$?; tail -8 smoke.full; grep -c "^PASS" smoke.full; grep -c "^FAIL" smoke.full'
		echo "=== 2. JTB (2000x3 x3) ==="
		gssh 'cd /tmp/w6 && mkdir -p jt && cp -f /mnt/JThreadBench.class jt/ && ok=0; for i in 1 2 3; do (cd jt && timeout 300 java -Xmx512m JThreadBench 2000 3 > ../jt$i.out 2> ../jt$i.err); rc=$?; [ $rc = 0 ] && ok=$((ok+1)) || echo "JTB run$i rc=$rc"; done; echo JTB_OK=$ok'
		echo "=== 3. metis_eq x2 checksum ==="
		gssh 'cd /tmp/w6 && [ -s corpus.txt ] || /mnt/r6dg/gen_text 8 corpus.txt >/dev/null 2>&1 || head -c 8388608 /dev/zero | tr "\0" a > corpus.txt; for i in 1 2; do LD_PRELOAD=/mnt/r6dg/corten_mode_hook.so CORTEN_MODE_HOOK_STRICT=1 timeout 300 /mnt/r6dg/metis_eq 2 corpus.txt > metis$i.out 2> metis$i.err; echo metis$i RC=$?; grep -o "\"distinct_words\":[0-9]*,\"checksum\":\"[a-f0-9]*\"" metis$i.out; done'
		echo "=== 4. sweep-live ==="
		gssh 'cd /tmp/w6 && cp -f /mnt/w45/sweep-live . && chmod +x sweep-live && LD_PRELOAD=/mnt/r6dg/corten_mode_hook.so CORTEN_MODE_HOOK_STRICT=1 timeout 300 ./sweep-live > sweep.full 2>&1; echo SWEEP_RC=$?; tail -6 sweep.full'
		echo "=== 5. mva1_probe ==="
		gssh 'cd /tmp/w6 && cp -f /mnt/t1c/mva1_probe . && chmod +x mva1_probe && LD_PRELOAD=/mnt/r6dg/corten_mode_hook.so CORTEN_MODE_HOOK_STRICT=1 timeout 300 ./mva1_probe > probe.out 2>&1; echo PROBE_RC=$?; grep -cE "^\[ok\]" probe.out; grep -E "probe done|S-1|S-4|FRESH|CHUNK|MemFree" probe.out | tail -8'
		echo "=== 6. S-3 电池 (双分支) ==="
		gssh 'cd /tmp/w6 && sh /mnt/mve-battery/mve_s3_swapoff.sh > s3.full 2>&1; echo S3_RC=$?; tail -12 s3.full'
		echo "=== 7. J3 oracle: 注册驱动 (in-boot 断言 + 快照; guest 本地盘副本保 dev:ino 稳定) ==="
		gssh 'mkdir -p /root/j3d && cp -f /mnt/mvc-oracle/run_mvc_oracle.sh /mnt/mvc-oracle/mvc_j3_workload /root/j3d/ && chmod +x /root/j3d/* && stat -c "j3w dev=%F maj:min=%t:%T inode=%i bytes=%s" /root/j3d/mvc_j3_workload && bash /root/j3d/run_mvc_oracle.sh /tmp/j3-snap > /tmp/w6/j3drv.full 2>&1; echo J3_DRIVER_RC=$?; cat /tmp/w6/j3drv.full'
		echo "=== 8. J3 oracle: 手动第二跑 (live wl/tree audit @ready; w6b: guest 侧驱动, setsid-over-ssh 挂连规避 + pgrep -x 自匹配规避) ==="
gssh 'echo IyEvYmluL2Jhc2gKY2QgL3RtcC93NgpzZXRzaWQgc2V0YXJjaCAtUiAvcm9vdC9qM2QvbXZjX2ozX3dvcmtsb2FkIC90bXAvajMtc25hcDMgPiBqM2xpdmUub3V0IDI+JjEgPCAvZGV2L251bGwgJgpmb3IgaSBpbiAkKHNlcSAxMDApOyBkbyBncmVwIC1xIHJlYWR5IGozbGl2ZS5vdXQgMj4vZGV2L251bGwgJiYgYnJlYWs7IHNsZWVwIDAuMTsgZG9uZQpncmVwIHJlYWR5IGozbGl2ZS5vdXQKcGdyZXAgLXggbXZjX2ozX3dvcmtsb2FkIHwgaGVhZCAtMSA+IC90bXAvdzYvajNwaWQK | base64 -d > /tmp/w6/j3live.sh && chmod +x /tmp/w6/j3live.sh && /tmp/w6/j3live.sh && cat /tmp/w6/j3pid'
		JPID=$(gssh 'cat /tmp/w6/j3pid')
		audit_pid "$JPID" j3-live
		gssh "pkill -TERM -x mvc_j3_workload; sleep 1; pgrep -x mvc_j3_workload >/dev/null && echo 'j3 STILL ALIVE' || echo 'j3 exited'; echo '--- arenas 行数 (j3 退出后)'; wc -l < $DBG/arenas"
		echo "=== 9. 终读数 ==="
		gssh "cat $DBG/audit_gate"
		echo "--- arena_stats 全量"
		gssh "cat $DBG/arena_stats"
		echo "--- arenas 全表"
		gssh "cat $DBG/arenas"
		echo "=== 10. pgtables 残值 + j2_stale + dmesg 审计 ==="
		gssh 'dmesg | grep "non-zero pgtables_bytes" | tail -5; echo PGTABLES_COUNT=$(dmesg | grep -c "non-zero pgtables_bytes"); dmesg | grep -icE "corten.*(warn|bug|timed out)" || true; dmesg | grep -iE "corten|WARNING|BUG" | tail -12 || true'
	} > "$out" 2>&1
	say "gate 完成, 日志 $out"
	grep -E "_RC=|JTB_OK|PGTABLES_COUNT|^=== " "$out" | head -40
	scp -i $SSHKEY -P $PORT -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -r \
		root@127.0.0.1:/tmp/j3-snap "$RES/j3-vc-snap" \
		&& say "J3 快照已取回 $RES/j3-vc-snap" || say "WARN: scp j3-snap 失败"
	scp -i $SSHKEY -P $PORT -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -r \
		root@127.0.0.1:/tmp/j3-snap2 "$RES/j3-vc-snap2" \
		&& say "live 跑快照已取回 $RES/j3-vc-snap2" || say "WARN: scp j3-snap2 失败"
}

case "${1:-}" in
boot) vm_boot ;;
gate) gate ;;
kill) vm_kill; say killed ;;
*) echo "usage: $0 {boot|gate|kill}"; exit 2 ;;
esac
