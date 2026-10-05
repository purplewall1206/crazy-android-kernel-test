#!/bin/bash
# mv3b-x5-gate.sh -- corruption-signature ×5 gate: five sequential =on+journal
# boots on the fixed kernel; per-boot corruption-signature audit + pgtables
# residue ledger. One VM at a time (image contention discipline).
set -u
RES=/home/ppw/linux-6.18/project/results/r07/mv3b
IMG=/home/ppw/vm/trixie-w6v2.img
KERN=$RES/bzImage-mv3b-y
PORT=10029
SSHKEY=/home/ppw/vm/trixie.id_rsa
PIDFILE=/home/ppw/vm/qemu-mv3b.pid
SESSION=mv3b-vm
LEDGER=$RES/x5-ledger.txt
: > $LEDGER
echo "=== ×5 corruption gate $(date '+%F %T') kernel=$(sha256sum $KERN | cut -c1-16)" >> $LEDGER

gssh() { timeout 25 ssh -i $SSHKEY -p $PORT -o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 root@127.0.0.1 "$@" 2>/dev/null; }

for N in 1 2 3 4 5; do
	CON=$RES/console-mv3b-x5-$N.log
	tmux kill-session -t $SESSION 2>/dev/null || true
	[ -f $PIDFILE ] && kill "$(cat $PIDFILE)" 2>/dev/null
	rm -f $PIDFILE; sleep 2
	tmux new-session -d -s $SESSION "qemu-system-x86_64 -enable-kvm -cpu host -m 4096 -smp 8 \
	  -kernel $KERN \
	  -append 'console=ttyS0 root=/dev/vda rw net.ifnames=0 systemd.mask=sys-kernel-config.mount corten=on corten_mode_default=on mitigations=off kunit.enable=0 log_buf_len=16M' \
	  -drive file=$IMG,if=virtio,format=raw \
	  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
	  -device virtio-net-pci,netdev=net0 \
	  -fsdev local,id=fs0,path=/home/ppw/bench/share,security_model=none \
	  -device virtio-9p-pci,fsdev=fs0,mount_tag=hostshare \
	  -display none -serial file:$CON \
	  -monitor unix:/home/ppw/vm/monitor-mv3b.sock,server,nowait -pidfile $PIDFILE"
	echo "[x5-$N] $(date '+%T') booting" >> $LEDGER
	UP=0
	for i in $(seq 1 24); do
		gssh 'echo up' >/dev/null && { UP=1; break; }
		sleep 5
	done
	sleep 8   # let late-boot settle (journald flush etc.)
	if [ $UP = 1 ]; then
		AUD=$(gssh 'dmesg | grep -cE "Bad page|WARNING: CPU|BUG: kernel|BUG: unable|list_del|segfault|mm.h:2648"; dmesg | grep -c "non-zero pgtables_bytes"; systemctl is-active systemd-journald; grep -c "" /dev/null; journalctl -b --no-pager 2>/dev/null | wc -l; test -f /var/log/journal -o -d /var/log/journal && echo PERSIST || echo RUNTIME' 2>/dev/null)
		echo "[x5-$N] ssh=UP audit=<corr=$(echo "$AUD" | sed -n 1p) pgtbl=$(echo "$AUD" | sed -n 2p) journald=$(echo "$AUD" | sed -n 3p) jlines=$(echo "$AUD" | sed -n 5p) $(echo "$AUD" | sed -n 6p)>" >> $LEDGER
	else
		echo "[x5-$N] ssh=DOWN (console verdict follows)" >> $LEDGER
	fi
	# console-based audit regardless of ssh
	CORR=$(sed 's/\x1b\[[0-9;]*m//g' $CON | grep -cE "Bad page|WARNING: CPU|BUG: kernel|BUG: unable|list_del corruption|segfault|mm.h:2648")
	PGT=$(sed 's/\x1b\[[0-9;]*m//g' $CON | grep -c "non-zero pgtables_bytes")
	FIN=$(sed 's/\x1b\[[0-9;]*m//g' $CON | grep -cE "Finished systemd-tmpfiles-setup-dev-early|Reached target (multi-user|graphical).target")
	echo "[x5-$N] console: corr_signatures=$CORR pgtables_residue=$PGT boot_progress_markers=$FIN" >> $LEDGER
	if [ $CORR != 0 ]; then
		sed 's/\x1b\[[0-9;]*m//g' $CON | grep -aE "Bad page|WARNING: CPU|BUG: kernel|BUG: unable|list_del|segfault|mm.h:2648" | head -5 >> $LEDGER
	fi
	kill "$(cat $PIDFILE)" 2>/dev/null
	tmux kill-session -t $SESSION 2>/dev/null || true
	rm -f $PIDFILE; sleep 2
done
echo "=== ×5 gate done $(date '+%F %T')" >> $LEDGER
grep -E "^\[x5|===" $LEDGER
