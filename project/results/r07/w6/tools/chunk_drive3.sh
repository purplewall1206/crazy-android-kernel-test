#!/bin/bash
P=$1
SSHK="ssh -i /home/ppw/vm/trixie.id_rsa -p $P -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@127.0.0.1"
$SSHK "pkill -f '^/tmp/chunk/chunk_shape' 2>/dev/null; mkdir -p /tmp/chunk; dmesg -C" 2>/dev/null
timeout 20 scp -q -i /home/ppw/vm/trixie.id_rsa -P $P -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null /home/ppw/linux-6.18/project/results/r07/w6/tools/chunk_shape root@127.0.0.1:/tmp/chunk/ 2>/dev/null
$SSHK 'chmod +x /tmp/chunk/chunk_shape; setsid /tmp/chunk/chunk_shape /tmp/chunk > /tmp/chunk/out.log 2>&1 < /dev/null & sleep 0.6; pgrep -x chunk_shape | head -1' 2>/dev/null
CP=$($SSHK 'pgrep -x chunk_shape | head -1' 2>/dev/null)
echo "pid=$CP"
$SSHK "echo next > /tmp/chunk/state" 2>/dev/null; sleep 0.7
$SSHK "echo '== B-dropped: maps window rows:'; grep '^1000' /proc/$CP/maps; echo '== B-dropped: arenas rows of this mm:'; grep -v shadow /sys/kernel/debug/corten/arenas | grep -v '^ *mm ' | grep '100000000000\|1000008\|anon.*0b' | head -8; echo '== raw arenas:'; cat /sys/kernel/debug/corten/arenas" 2>/dev/null
$SSHK "echo next > /tmp/chunk/state" 2>/dev/null; sleep 0.7
$SSHK "echo '== C-remat: alive?'; kill -0 $CP && echo yes || echo NO; echo '== C maps window rows:'; grep '^1000' /proc/$CP/maps 2>/dev/null; echo '== out.log:'; cat /tmp/chunk/out.log; echo '== dmesg tail:'; dmesg | tail -4" 2>/dev/null
$SSHK "pkill -f '^/tmp/chunk/chunk_shape'" 2>/dev/null
