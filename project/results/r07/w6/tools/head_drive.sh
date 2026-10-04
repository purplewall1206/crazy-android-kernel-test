#!/bin/bash
P=$1
SSHK="ssh -i /home/ppw/vm/trixie.id_rsa -p $P -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@127.0.0.1"
$SSHK "pkill -f '^/tmp/chunk/chunk_shape_head' 2>/dev/null; mkdir -p /tmp/chunk; dmesg -C" 2>/dev/null
timeout 20 scp -q -i /home/ppw/vm/trixie.id_rsa -P $P -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null /home/ppw/linux-6.18/project/results/r07/w6/tools/chunk_shape_head root@127.0.0.1:/tmp/chunk/ 2>/dev/null
$SSHK 'chmod +x /tmp/chunk/chunk_shape_head; setsid /tmp/chunk/chunk_shape_head /tmp/chunk > /tmp/chunk/outh.log 2>&1 < /dev/null & sleep 0.6; pgrep -x chunk_shape_hea | head -1' 2>/dev/null
CP=$($SSHK 'pgrep -x chunk_shape_hea | head -1' 2>/dev/null)
echo "pid=$CP"
$SSHK "echo next > /tmp/chunk/state" 2>/dev/null; sleep 0.7
$SSHK "echo '== head-dropped B: window rows:'; grep '^1000' /proc/$CP/maps 2>/dev/null; echo '== arenas:'; grep '100000000000' /sys/kernel/debug/corten/arenas" 2>/dev/null
$SSHK "pkill -f '^/tmp/chunk/chunk_shape_head'" 2>/dev/null
