#!/bin/bash
# w3fix7-vm.sh - takeover bench/gate VM driver (mv3d-battery vm_boot shape).
# usage: w3fix7-vm.sh boot|run|fetch|kill
set -u
KT=/home/ppw/linux-6.18-mva/arch/x86/boot/bzImage
IMG=/home/ppw/vm/w3fix7-t2.qcow2
KEY=/home/ppw/vm/trixie.id_rsa
PORT=10036
PIDFILE=/home/ppw/vm/qemu-w3fix7-t2.pid
SESSION=w3fix7-t2
R=/home/ppw/linux-6.18-mva/project/results/r07/w3fix7
COMMON="systemd.mask=sys-kernel-config.mount mitigations=off kunit.enable=0 log_buf_len=16M"

g() { timeout 120 ssh -i $KEY -p $PORT -o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 root@127.0.0.1 "$@"; }

case "${1:-}" in
kill)
	[ -f $PIDFILE ] && kill "$(cat $PIDFILE)" 2>/dev/null
	rm -f $PIDFILE
	tmux kill-session -t $SESSION 2>/dev/null
	sleep 2; echo killed ;;
boot)
	tmux new-session -d -s $SESSION \
	  "qemu-system-x86_64 -enable-kvm -cpu host -m 4096 -smp 8 \
	  -kernel $KT \
	  -append 'console=ttyS0 root=/dev/vda rw net.ifnames=0 $COMMON corten=on' \
	  -drive file=$IMG,if=virtio,format=qcow2 \
	  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
	  -device virtio-net-pci,netdev=net0 \
	  -fsdev local,id=fs0,path=/home/ppw/bench/share,security_model=none \
	  -device virtio-9p-pci,fsdev=fs0,mount_tag=hostshare \
	  -display none -serial file:$R/console-w3fix7-takeover.log \
	  -pidfile $PIDFILE"
	ok=0
	for i in $(seq 1 60); do
		g 'echo up' >/dev/null 2>&1 && { ok=1; break; }
		sleep 5
	done
	[ $ok = 1 ] || { echo BOOT_FAIL; exit 1; }
	sleep 10
	echo "BOOT_OK ssh_try=$i"
	g 'uname -r; cat /proc/cmdline' ;;
run)
	g 'bash /mnt/w3fix7-guest.sh' 2>&1 | tee $R/guest-run-takeover.log
	;;
fetch)
	mkdir -p $R/bench-after-takeover $R/gate-takeover
	g "cd /root/w3fix7 && tar czf - mmpf-*.json" | tar xzf - -C $R/bench-after-takeover
	g "cd /root/w3fix7 && tar czf - mode-smoke.log metis.1.out metis.1.err metis.2.out metis.2.err audit_gate.txt dmesg-corten-warns.txt" \
	  | tar xzf - -C $R/gate-takeover 2>/dev/null
	ls $R/bench-after-takeover | wc -l; ls $R/gate-takeover
	;;
*) echo "usage: $0 boot|run|fetch|kill" ;;
esac
