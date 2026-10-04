#!/bin/bash
# chunk_drive.sh — 在给定 VM 上复现 CHUNK 三态并落 maps/arenas/rowwalk 证据。
# 用法: chunk_drive.sh <ssh-port> <outprefix>
P=$1; PRE=$2
SSHK="ssh -i /home/ppw/vm/trixie.id_rsa -p $P -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@127.0.0.1"
$SSHK 'pkill -f '^^/tmp/chunk/chunk_shape' 2>/dev/null; mkdir -p /tmp/chunk' 2>/dev/null
timeout 20 scp -q -i /home/ppw/vm/trixie.id_rsa -P $P -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null /home/ppw/linux-6.18/project/results/r07/w6/tools/chunk_shape root@127.0.0.1:/tmp/chunk/ 2>/dev/null
$SSHK 'chmod +x /tmp/chunk/chunk_shape; setsid /tmp/chunk/chunk_shape /tmp/chunk > /tmp/chunk/out.log 2>&1 < /dev/null & sleep 0.6; pgrep -x chunk_shape | head -1' 2>/dev/null
CP=$($SSHK 'pgrep -x chunk_shape | head -1' 2>/dev/null)
echo "probe_pid=$CP"
for ST in A-full B-dropped C-remat; do
  $SSHK "echo next > /tmp/chunk/state" 2>/dev/null
  sleep 0.7
  $SSHK "cat /proc/$CP/maps > /tmp/chunk/maps-$ST 2>/dev/null; echo $ST done" 2>/dev/null
done
$SSHK "grep -c corten_arena /proc/$CP/maps; echo '== state A (full):'; cat /tmp/chunk/maps-A-full; echo '== state B (dropped):'; cat /tmp/chunk/maps-B-dropped; echo '== state C (remat):'; cat /tmp/chunk/maps-C-remat; echo '== arenas:'; cat /sys/kernel/debug/corten/arenas; kill $CP" 2>/dev/null
$SSHK "for s in A-full B-dropped C-remat; do echo ==\$s==; grep -E '1000000|heap' /tmp/chunk/maps-\$s; done" 2>/dev/null
