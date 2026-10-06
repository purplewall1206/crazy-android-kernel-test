#!/bin/bash
# dpa-boot.sh -- w3fix4 ledger #1 DPA retest driver.
# World = the mv3b DPA oops boot: CONFIG_DEBUG_PAGEALLOC=y(ENABLE_DEFAULT),
# page_owner=on, corten=on corten_mode_default=on (default-enter world,
# organic journalctl/tmpfiles brk churn) + explicit full-speed brk_churn.
# Isolation: port 10033, overlay disk, tmux w3fix4-dpa -- the live P2 VM
# (10031) is untouched.
set -u
WT=/home/ppw/linux-6.18-mva
RES=$WT/project/results/r07/w3fix4
KERN=$WT/arch/x86/boot/bzImage
IMG=/home/ppw/vm/overlay-w3fix4-dpa2.qcow2
SHARE=/home/ppw/bench/share
PORT=10033
SSHKEY=/home/ppw/vm/trixie.id_rsa
PIDFILE=/home/ppw/vm/qemu-w3fix4-dpa.pid
SESSION=w3fix4-dpa
SSH="ssh -i $SSHKEY -p $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 root@127.0.0.1"

tmux kill-session -t $SESSION 2>/dev/null
[ -f $PIDFILE ] && kill "$(cat $PIDFILE)" 2>/dev/null; rm -f $PIDFILE
sleep 2
tmux new-session -d -s $SESSION \
	"qemu-system-x86_64 -enable-kvm -cpu host -m 4096 -smp 8 \
	-kernel $KERN \
	-append 'console=ttyS0 root=/dev/vda rw net.ifnames=0 corten=on corten_mode_default=on mitigations=off kunit.enable=0 log_buf_len=16M systemd.mask=sys-kernel-config.mount page_owner=on' \
	-drive file=$IMG,if=virtio,format=qcow2 \
	-netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
	-device virtio-net-pci,netdev=net0 \
	-fsdev local,id=fs0,path=$SHARE,security_model=none \
	-device virtio-9p-pci,fsdev=fs0,mount_tag=hostshare \
	-display none -serial file:$RES/console-dpa.log \
	-monitor unix:/home/ppw/vm/monitor-w3fix4-dpa.sock,server,nowait \
	-pidfile $PIDFILE"
echo "[dpa] booting kernel=$(sha256sum $KERN | cut -c1-16) console=$RES/console-dpa.log"
for i in $(seq 1 90); do
	$SSH 'echo up' >/dev/null 2>&1 && { echo "[dpa] guest ssh up (try $i)"; exit 0; }
	sleep 5
done
echo "[dpa] FAIL: ssh never came up"
exit 1
