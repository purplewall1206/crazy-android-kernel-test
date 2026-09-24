#!/bin/bash
# perf2-launch.sh - boot the perf2 worktree kernel in its own VM
# (port 10031, image trixie-perf2.img, tmux session vm-perf2).
# usage: perf2-launch.sh [append-extra]
set -u
KERNEL=/home/ppw/linux-6.18-perf2/arch/x86/boot/bzImage
IMG=/home/ppw/vm/trixie-perf2.img
EXTRA="${1:-corten=on mitigations=off}"

tmux kill-session -t vm-perf2 2>/dev/null || true
sleep 1
rm -f /home/ppw/vm/qemu-perf2.pid
tmux new-session -d -s vm-perf2 "qemu-system-x86_64 \
  -enable-kvm -cpu host \
  -m 4096 -smp 8 \
  -kernel $KERNEL \
  -append 'console=ttyS0 root=/dev/vda rw net.ifnames=0 $EXTRA' \
  -drive file=$IMG,if=virtio,format=raw \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:10031-:22 \
  -device virtio-net-pci,netdev=net0 \
  -fsdev local,id=fs0,path=/home/ppw/bench/share,security_model=none \
  -device virtio-9p-pci,fsdev=fs0,mount_tag=hostshare \
  -display none -serial stdio \
  -pidfile /home/ppw/vm/qemu-perf2.pid"
echo "vm-perf2 launching (port 10031)"
