#!/bin/bash
P=$1
SSHK="ssh -i /home/ppw/vm/trixie.id_rsa -p $P -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@127.0.0.1"
$SSHK "pkill -f '^/tmp/chunk/chunk_shape' 2>/dev/null; mkdir -p /tmp/chunk; dmesg -C" 2>/dev/null
timeout 20 scp -q -i /home/ppw/vm/trixie.id_rsa -P $P -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null /home/ppw/linux-6.18/project/results/r07/w6/tools/chunk_shape root@127.0.0.1:/tmp/chunk/ 2>/dev/null
$SSHK 'chmod +x /tmp/chunk/chunk_shape; setsid /tmp/chunk/chunk_shape /tmp/chunk > /tmp/chunk/out.log 2>&1 < /dev/null & sleep 0.6; pgrep -x chunk_shape | head -1' 2>/dev/null
CP=$($SSHK 'pgrep -x chunk_shape | head -1' 2>/dev/null)
echo "pid=$CP"
for ST in A-full B-dropped C-remat; do
  $SSHK "echo next > /tmp/chunk/state" 2>/dev/null
  sleep 0.7
  $SSHK "cat /proc/$CP/maps > /tmp/chunk/maps-$ST 2>/dev/null; echo '-- $ST live=$(kill -0 $CP 2>/dev/null && echo yes || echo NO); arenas-window:'; grep -E '1000' /sys/kernel/debug/corten/arenas | grep $(grep $CP /proc/*/status 2>/dev/null | head -0); grep -c . /sys/kernel/debug/corten/arenas" 2>/dev/null
  $SSHK "grep -E '^1' /proc/$CP/maps 2>/dev/null" 2>/dev/null
done
$SSHK "kill -0 $CP 2>/dev/null && echo alive-at-end || echo DEAD-at-end; dmesg | tail -6" 2>/dev/null
