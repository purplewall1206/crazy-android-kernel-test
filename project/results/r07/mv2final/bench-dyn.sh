#!/bin/bash
# bench-dyn.sh -- MV2-final round D35-refreshed dyn bench driver (mv2final ns).
#   bash bench-dyn.sh boot <kernel>   # =on default-entry boot (port 10041)
#   bash bench-dyn.sh run <tag>       # run the cell set, fetch JSONs
#   bash bench-dyn.sh kill
# Protocol = mv3cfeat D35 shape: mmbench_dyn, seed = 20260913 + b*1009 +
# c*97 + t*7 + k, min_seconds=2, runs k1..k3, =on default-entry world.
set -u
RES=/home/ppw/cortenmm/project/results/r07/mv2final
SHARE=/home/ppw/bench/share
IMG=/home/ppw/vm/trixie-w6v2.img
PORT=10041
SSHKEY=/home/ppw/vm/trixie.id_rsa
PIDFILE=/home/ppw/vm/qemu-mv2final.pid
SESSION=mv2final-vm
BIN=/mnt/cortenmm/bench/mmbench/mmbench_dyn

say() { echo "[mv2final] $(date '+%F %T') $*"; }

gssh() {
	timeout 60 ssh -i $SSHKEY -p $PORT -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 \
		root@127.0.0.1 "$@" 2>/dev/null
}

vm_kill() {
	[ -f $PIDFILE ] && { kill "$(cat $PIDFILE)" 2>/dev/null || true; rm -f $PIDFILE; }
	tmux kill-session -t $SESSION 2>/dev/null || true
	sleep 2
}

vm_boot() {
	local KERN=$1
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
		-display none -serial file:$RES/console-$2.log \
		-monitor unix:/home/ppw/vm/monitor-mv2final.sock,server,nowait \
		-pidfile $PIDFILE"
	say "booting kernel=$(sha256sum $KERN | cut -c1-16)"
	local i
	for i in $(seq 1 60); do
		gssh 'echo up' >/dev/null 2>&1 && { say "guest ssh up (try $i)"; return 0; }
		sleep 5
	done
	say "FAIL: ssh never up"; return 1
}

vm_run() {
	local TAG=$1
	local OUT=$RES/bench/$TAG
	mkdir -p $OUT/raw
	gssh 'mkdir -p /tmp/mb && mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null; true' || { say "FAIL mount"; return 1; }
	# Cells (bench, cont, threads): the knife-relevant D35 set.
	local b c t k seed fn
	for b in mmap-pf pf unmap unmap-virt; do
		for c in low; do
			for t in 1 4 8; do
				for k in 1 2 3; do
					case $b in
						mmap) bi=0 ;; mmap-pf) bi=1 ;; pf) bi=2 ;;
						unmap-virt) bi=3 ;; unmap) bi=4 ;;
					esac
					case $c in low) ci=0 ;; high) ci=1 ;; esac
					seed=$((20260913 + bi * 1009 + ci * 97 + t * 7 + k))
					fn="${b}_${c}_t${t}_run${k}.json"
					gssh "timeout 60 $BIN $b $c $t 2 $seed > /tmp/mb/$fn" \
						|| { say "FAIL $fn"; continue; }
					gssh "cat /tmp/mb/$fn" > $OUT/raw/$fn 2>/dev/null
					say "$fn $(head -c 120 $OUT/raw/$fn)"
				done
			done
		done
	done
	say "cells done tag=$TAG"
	gssh 'uname -r; grep -c corten /proc/cmdline' >> $OUT/host.txt 2>&1
}

case "${1:-}" in
boot) vm_boot "${2:?kernel}" "${3:?console}" ;;
run)  vm_run "${2:?tag}" ;;
kill) vm_kill; say killed ;;
*) echo "usage: $0 {boot kernel console|run tag|kill}"; exit 2 ;;
esac
