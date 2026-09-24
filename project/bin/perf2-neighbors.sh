#!/bin/bash
# perf2-neighbors.sh - SIGSTOP/SIGCONT all other qemu-system VMs during
# perf2 measurement windows (perf1/t5run4 practice).  My own VM is
# identified by pidfile $PERF2_PIDFILE.  usage: perf2-neighbors.sh stop|cont
set -u
PIDFILE=/home/ppw/vm/qemu-perf2.pid
ME=0
[ -f "$PIDFILE" ] && ME=$(cat "$PIDFILE")
case "${1:-}" in
stop)
	for pid in $(pgrep -x qemu-system-x86); do
		[ "$pid" = "$ME" ] && continue
		kill -STOP "$pid" 2>/dev/null && echo "STOP $pid"
	done
	;;
cont)
	for pid in $(pgrep -x qemu-system-x86); do
		[ "$pid" = "$ME" ] && continue
		kill -CONT "$pid" 2>/dev/null && echo "CONT $pid"
	done
	;;
status)
	for pid in $(pgrep -x qemu-system-x86); do
		[ "$pid" = "$ME" ] && continue
		s=$(awk '{print $3}' /proc/$pid/stat 2>/dev/null)
		echo "$pid state=$s"
	done
	;;
*)
	echo "usage: $0 stop|cont|status"; exit 2
	;;
esac
