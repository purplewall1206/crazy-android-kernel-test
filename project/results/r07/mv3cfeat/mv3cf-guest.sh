#!/bin/bash
# mv3cf-guest.sh -- MV3.c-feat round guest driver (mv3cf namespace).
# Usage: bash mv3cf-guest.sh <mode>
#   boot-on <console>   =on default-entry boot (corten=on corten_mode_default=on)
#   boot-off <console>  =off control boot (corten=on only, no default entry)
#   boot-n   <console>  corten absent entirely (both gates off)
#   kill                kill qemu via pidfile only
# Isolation: port 10032, pidfile qemu-mv3cf.pid, tmux mv3cf-vm, monitor-mv3cf.sock
# -- never touches perf2 (10031) or the mv3a/mv3b/mv3c namespaces.
set -u
RES=/home/ppw/linux-6.18/project/results/r07/mv3cfeat
SHARE=/home/ppw/bench/share
IMG=/home/ppw/vm/trixie-w6v2.img
PORT=10032
SSHKEY=/home/ppw/vm/trixie.id_rsa
PIDFILE=/home/ppw/vm/qemu-mv3cf.pid
SESSION=mv3cf-vm

say() { echo "[mv3cf] $(date '+%F %T') $*"; }
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

vm_boot() {	# $1 = kernel, $2 = append line, $3 = console name
	vm_kill
	tmux new-session -d -s $SESSION \
		"qemu-system-x86_64 -enable-kvm -cpu host -m 4096 -smp 8 \
		-kernel $1 \
		-append '$2' \
		-drive file=$IMG,if=virtio,format=raw \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
		-device virtio-net-pci,netdev=net0 \
		-fsdev local,id=fs0,path=$SHARE,security_model=none \
		-device virtio-9p-pci,fsdev=fs0,mount_tag=hostshare \
		-display none -serial file:$RES/$3 \
		-monitor unix:/home/ppw/vm/monitor-mv3cf.sock,server,nowait \
		-pidfile $PIDFILE"
	say "VM launching (kernel=$1, tmux=$SESSION, port=$PORT, console=$3)"
	local i
	for i in $(seq 1 60); do
		if gssh 'echo up' >/dev/null 2>&1; then
			say "guest ssh up (after $i tries)"
			gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null || true; mount -t debugfs none /sys/kernel/debug 2>/dev/null || true; mkdir -p /tmp/mv3cf' \
				|| die "share/debugfs mount failed"
			return 0
		fi
		sleep 5
	done
	die "guest ssh never came up ($3)"
}

KERN_BASE=$RES/bzImage-mv3cfeat-base
APPEND_COMMON="console=ttyS0 root=/dev/vda rw net.ifnames=0 systemd.mask=sys-kernel-config.mount mitigations=off kunit.enable=0 log_buf_len=16M"

case "${1:-}" in
	boot-on)  vm_boot "${2:-$KERN_BASE}" "$APPEND_COMMON corten=on corten_mode_default=on" "${3:?console name}" ;;
	boot-off) vm_boot "${2:-$KERN_BASE}" "$APPEND_COMMON corten=on" "${3:?console name}" ;;
	boot-n)   vm_boot "${2:-$KERN_BASE}" "$APPEND_COMMON" "${3:?console name}" ;;
	kill)     vm_kill; say "killed" ;;
	*) die "bad mode $1" ;;
esac
