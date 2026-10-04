#!/bin/bash
# mv3c-guest-gate.sh -- MV3.c guest gate driver (mv3c namespace, pidfile discipline).
# Usage: bash mv3c-guest-gate.sh journal   # =on full boot, unmasked journald
#        bash mv3c-guest-gate.sh kill      # kill qemu via pidfile only
# Criteria: full systemd boot (tmpfiles-setup-dev-early completes), dmesg
#   zero Bad page / zero WARNING / zero Oops (pgtables residue line = known
#   ancient stale-counter shape, tracked separately, see report 3.2).
set -u
RES=/home/ppw/linux-6.18/project/results/r07/mv3c
SHARE=/home/ppw/bench/share
KERN=$RES/bzImage-mv3c-y
IMG=/home/ppw/vm/trixie-w6v2.img
PORT=10030
SSHKEY=/home/ppw/vm/trixie.id_rsa
PIDFILE=/home/ppw/vm/qemu-mv3c.pid
SESSION=mv3c-vm

say() { echo "[mv3c-gate] $(date '+%F %T') $*"; }
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
		-append 'console=ttyS0 root=/dev/vda rw net.ifnames=0 systemd.mask=sys-kernel-config.mount corten=on corten_mode_default=on mitigations=off kunit.enable=0 log_buf_len=16M' \
		-drive file=$IMG,if=virtio,format=raw \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
		-device virtio-net-pci,netdev=net0 \
		-fsdev local,id=fs0,path=$SHARE,security_model=none \
		-device virtio-9p-pci,fsdev=fs0,mount_tag=hostshare \
		-display none -serial file:$RES/console-mv3c-journal.log \
		-monitor unix:/home/ppw/vm/monitor-mv3c.sock,server,nowait \
		-pidfile $PIDFILE"
	say "VM launching (kernel=$KERN, tmux=$SESSION, port=$PORT)"
	local i
	for i in $(seq 1 60); do
		if gssh 'echo up' >/dev/null 2>&1; then
			say "guest ssh up (after $i tries)"
			gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; mount -t debugfs none /sys/kernel/debug 2>/dev/null || true; mkdir -p /tmp/mv3c' \
				|| die "share/debugfs mount failed"
			return 0
		fi
		sleep 5
	done
	die "guest ssh never came up"
}

case "${1:-journal}" in
	journal) vm_boot ;;
	kill) vm_kill; say "killed" ;;
	*) die "bad mode $1" ;;
esac
