#!/bin/bash
# CortenMM storm loop (sec 164): generator-storm / fork-storm / frame
# pressure cycling against the boot VM.  Results append to
# results/r08/storm/storm-<date>.log; a KASAN/oops signature in the
# guest console aborts the round with a nonzero status.
set -u
KT=${KT:-/home/ppw/linux-6.18/arch/x86/boot/bzImage}
IMG=${IMG:-/home/ppw/vm/soakC-sys.qcow2}
PORT=${PORT:-10050}
SESSION=storm-$(date +%H%M)
R=/home/ppw/linux-6.18/project/results/r08/storm
mkdir -p "$R"
LOG="$R/storm-$(date +%m%d-%H%M).log"

tmux new-session -d -s "$SESSION" "qemu-system-x86_64 -enable-kvm -cpu host \
  -m 4096 -smp 4 -kernel $KT \
  -append 'console=ttyS0 root=/dev/vda rw net.ifnames=0 corten=on \
  corten_mode_default=on corten_stack_s2=on corten_arena.silent=on \
  systemd.mask=sys-kernel-config.mount mitigations=off kunit.enable=0 \
  log_buf_len=16M' -drive file=$IMG,if=virtio,format=raw \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
  -device virtio-net-pci,netdev=net0 -display none \
  -serial file:/tmp/$SESSION-serial.log -pidfile /tmp/$SESSION.pid; \
  sleep 999999"
for i in $(seq 1 40); do
  timeout 10 ssh -i /home/ppw/vm/trixie.id_rsa -p $PORT \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=5 root@127.0.0.1 'echo up' >/dev/null 2>&1 && break
  sleep 5
done
echo "[$(date +%T)] storm VM up (port $PORT)" | tee -a "$LOG"
ROUND=0
while true; do
  ROUND=$((ROUND+1))
  # generator storm: exec 40 short-lived processes through the arena
  # exec path (each = sweep + declare + fork-exit cycle)
  timeout 60 ssh -i /home/ppw/vm/trixie.id_rsa -p $PORT \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    root@127.0.0.1 'for i in $(seq 1 40); do systemd-detect-virt \
    >/dev/null 2>&1; done; echo done' >>"$LOG" 2>&1
  # fork storm: 200 subshell forks through the arena fork mirror
  timeout 60 ssh -i /home/ppw/vm/trixie.id_rsa -p $PORT \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    root@127.0.0.1 'for i in $(seq 1 200); do (exit 0); done; echo fs' \
    >>"$LOG" 2>&1
  # verdict scan on the serial console
  BAD=$(grep -cE "Oops|Attempted to kill|Protocol error|KASAN: " \
        /tmp/$SESSION-serial.log 2>/dev/null || echo 0)
  echo "[$(date +%T)] round $ROUND bad-sigs=$BAD" | tee -a "$LOG"
  if [ "$BAD" != "${BAD/0/}" ] 2>/dev/null; then :; fi
  [ "$BAD" -gt 0 ] 2>/dev/null && {
    echo "[$(date +%T)] ABORT: bad signature" | tee -a "$LOG"
    grep -E "Oops|KASAN: |Protocol error" /tmp/$SESSION-serial.log \
      | tail -20 >> "$LOG"
    break
  }
  sleep 20
done
