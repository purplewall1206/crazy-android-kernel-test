#!/bin/bash
P=$1; OUT=$2
SSHK="ssh -i /home/ppw/vm/trixie.id_rsa -p $P -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@127.0.0.1"
$SSHK "rm -f /tmp/samples/*; mkdir -p /tmp/samples" 2>/dev/null
$SSHK 'cd /tmp/p && setsid env LD_PRELOAD=/mnt/r6dg/corten_mode_hook.so CORTEN_MODE_HOOK_STRICT=1 ./mva1_probe > probe-s.out 2>&1 < /dev/null & for i in $(seq 4000); do n=$(ls /tmp/samples 2>/dev/null | wc -l); [ $n -ge 400 ] && break; sleep 0.02; done' 2>/dev/null &
BPID=$!
sleep 0.5
$SSHK 'PP=$(pgrep -x mva1_probe | head -1); echo probe_pid=$PP > /tmp/samples/pid; for i in $(seq 400); do cp /proc/$PP/maps /tmp/samples/m$i 2>/dev/null; sleep 0.03; done' 2>/dev/null
wait $BPID 2>/dev/null
$SSHK 'cat /tmp/samples/pid; grep -E "CHUNK|S-4|passes" /tmp/p/probe-s.out' 2>/dev/null
timeout 60 scp -q -i /home/ppw/vm/trixie.id_rsa -P $P -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -r root@127.0.0.1:/tmp/samples "$OUT" 2>/dev/null && echo "samples-> $OUT"
