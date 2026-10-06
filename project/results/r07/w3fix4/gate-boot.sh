#!/bin/bash
# gate-boot.sh -- w3fix4 final-verification guest dual boot (kernel #393).
#   bash gate-boot.sh on    # =on default-enter world: mode probe, churn adoption, stats read
#   bash gate-boot.sh off   # =off world (corten=on, default off): regression + smoke
# Uses the preserved bzImage-w3fix4-final-y; port 10033, overlay disk.
set -u
WT=/home/ppw/linux-6.18-mva
RES=$WT/project/results/r07/w3fix4
KERN=$RES/bzImage-w3fix4-final-y
IMG=/home/ppw/vm/overlay-w3fix4-dpa.qcow2
SHARE=/home/ppw/bench/share
PORT=10033
SSHKEY=/home/ppw/vm/trixie.id_rsa
PIDFILE=/home/ppw/vm/qemu-w3fix4-dpa.pid
SESSION=w3fix4-dpa
SSH="ssh -i $SSHKEY -p $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 root@127.0.0.1"

vm_kill() {
	tmux kill-session -t $SESSION 2>/dev/null
	[ -f $PIDFILE ] && kill "$(cat $PIDFILE)" 2>/dev/null; rm -f $PIDFILE
	sleep 2
}

vm_boot() { # $1 = on|off
	local W=$1 EXTRA CON
	if [ "$W" = on ]; then
		EXTRA="corten=on corten_mode_default=on"
		CON=console-gate-on.log
	else
		EXTRA="corten=on"
		CON=console-gate-off.log
	fi
	vm_kill
	tmux new-session -d -s $SESSION \
		"qemu-system-x86_64 -enable-kvm -cpu host -m 4096 -smp 8 \
		-kernel $KERN \
		-append 'console=ttyS0 root=/dev/vda rw net.ifnames=0 $EXTRA systemd.mask=sys-kernel-config.mount mitigations=off kunit.enable=0 log_buf_len=16M' \
		-drive file=$IMG,if=virtio,format=qcow2 \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
		-device virtio-net-pci,netdev=net0 \
		-fsdev local,id=fs0,path=$SHARE,security_model=none \
		-device virtio-9p-pci,fsdev=fs0,mount_tag=hostshare \
		-display none -serial file:$RES/$CON \
		-monitor unix:/home/ppw/vm/monitor-w3fix4-dpa.sock,server,nowait \
		-pidfile $PIDFILE"
	echo "[gate] booting world=$W kernel=$(sha256sum $KERN | cut -c1-16)"
	local i
	for i in $(seq 1 60); do
		$SSH 'echo up' >/dev/null 2>&1 && { echo "[gate] ssh up (try $i)"; return 0; }
		sleep 5
	done
	echo "[gate] FAIL: ssh never came up"; return 1
}

case "${1:-}" in
on)
	vm_boot on || exit 1
	$SSH 'mkdir -p /root/kit; mount -t debugfs none /sys/kernel/debug 2>/dev/null;
		gcc -O2 -o /root/bc /root/brk_churn.c && /root/bc 100 f && echo CHURN_RC=0
		timeout 20 cat /sys/kernel/debug/corten/arena_stats > /root/kit/gate-on-stats.txt; echo STATS_RC=$?
		grep -E "arenas |brk_region_grows|brk_region_shrinks|brk_legacy|exec_default_enters|stats_walk_truncs|probe_stale_skips" /root/kit/gate-on-stats.txt
		echo "oopses=$(dmesg | grep -cE "Oops|BUG:|general protection")"
		' 2>/dev/null
	;;
off)
	vm_boot off || exit 1
	$SSH 'mkdir -p /root/kit; mount -t debugfs none /sys/kernel/debug 2>/dev/null;
		gcc -O2 -o /root/bc /root/brk_churn.c && /root/bc 50 f && echo CHURN_RC=0
		timeout 20 cat /sys/kernel/debug/corten/arena_stats > /root/kit/gate-off-stats.txt; echo STATS_RC=$?
		grep -E "arenas |brk_legacy|auto_mmaps" /root/kit/gate-off-stats.txt
		echo "oopses=$(dmesg | grep -cE "Oops|BUG:|WARNING:|general protection")"
		echo "corten_dmesg_lines=$(dmesg | grep -ci corten)"
		' 2>/dev/null
	;;
*) echo "usage: $0 on|off"; exit 2 ;;
esac
