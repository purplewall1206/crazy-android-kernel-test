#!/bin/bash
# mv2final-guest-gate.sh -- MV2-final round guest double-boot battery.
#   bash mv2final-guest-gate.sh boot [journal|off] [console-tag]
#   bash mv2final-guest-gate.sh gate <tag>     # full battery legs
#   bash mv2final-guest-gate.sh light <tag>    # second-boot light battery
#   bash mv2final-guest-gate.sh kill
# 判据: brk 路由接管 (brk_region/brk_grow 不再 2:2882 失衡), arena_stats 首读
# 不挂, smoke 26/26, metis x2 checksum 同基准 2d383eeed4ceb73b, dmesg 静默.
set -u
RES=/home/ppw/cortenmm/project/results/r07/mv2final
SHARE=/home/ppw/bench/share
IMG=/home/ppw/vm/trixie-w6v2.img
PORT=10041
SSHKEY=/home/ppw/vm/trixie.id_rsa
PIDFILE=/home/ppw/vm/qemu-mv2final.pid
SESSION=mv2final-vm
DBG=/sys/kernel/debug/corten
BASELINE=2d383eeed4ceb73b

say() { echo "[mv2final-gate] $(date '+%F %T') $*"; }

gssh() {
	timeout 90 ssh -i $SSHKEY -p $PORT -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 \
		root@127.0.0.1 "$@" 2>/dev/null
}

vm_kill() {
	[ -f $PIDFILE ] && { kill "$(cat $PIDFILE)" 2>/dev/null || true; rm -f $PIDFILE; }
	tmux kill-session -t $SESSION 2>/dev/null || true
	sleep 2
}

vm_boot() { # $1 world(journal|off) $2 console tag
	local MODE=${1:-journal} EXTRA="" CON
	[ "$MODE" = journal ] && EXTRA="corten_mode_default=on"
	local CONSOLE=$2
	vm_kill
	tmux new-session -d -s $SESSION \
		"qemu-system-x86_64 -enable-kvm -cpu host -m 4096 -smp 8 \
		-kernel ${KERN:?kernel env} \
		-append 'console=ttyS0 root=/dev/vda rw net.ifnames=0 systemd.mask=sys-kernel-config.mount corten=on $EXTRA mitigations=off kunit.enable=0 log_buf_len=16M' \
		-drive file=$IMG,if=virtio,format=raw \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
		-device virtio-net-pci,netdev=net0 \
		-fsdev local,id=fs0,path=$SHARE,security_model=none \
		-device virtio-9p-pci,fsdev=fs0,mount_tag=hostshare \
		-display none -serial file:$RES/$CONSOLE \
		-monitor unix:/home/ppw/vm/monitor-mv2final.sock,server,nowait \
		-pidfile $PIDFILE"
	say "booting world=$MODE console=$CONSOLE"
	local i
	for i in $(seq 1 60); do
		gssh 'echo up' >/dev/null 2>&1 && { say "guest ssh up (try $i)"; return 0; }
		sleep 5
	done
	say "FAIL: ssh never up"; return 1
}

gate() { # $1 tag  $2 light(1)|full(0)
	local TAG=$1 LIGHT=${2:-0}
	gssh 'mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null; mount -t debugfs none /sys/kernel/debug 2>/dev/null; mkdir -p /tmp/mv2f' || { say "FAIL mounts"; return 1; }
	{
	echo "=== mv2final gate tag=$TAG $(date '+%F %T') ==="
	gssh 'uname -r; systemctl is-system-running; echo IS_RUNNING_RC=$?; systemctl --failed --no-legend | head -5'
	echo "=== leg1: brk 路由接管 (journalctl + heap churn 后读计数) ==="
	gssh "journalctl -b --no-pager | tail -2 >/dev/null; echo JOURNALCTL_RC=\$?; \
		/tmp/mv2f/heap-churn 2>/dev/null || true; \
		grep -E 'brk_grow|brk_shrink|brk_legacy|brk_region|brk_noop|exec_default_enters' $DBG/arena_stats"
	echo "=== leg2: arena_stats 首读计时 (无 churn) ==="
	gssh "/usr/bin/time -f 'FIRST_READ_SECONDS=%e' timeout 120 cat $DBG/arena_stats > /tmp/mv2f/stats-$TAG.out 2> /tmp/mv2f/stats-$TAG.time; echo FIRST_READ_RC=\$?; cat /tmp/mv2f/stats-$TAG.time; wc -l /tmp/mv2f/stats-$TAG.out"
	echo "=== leg3: smoke 26/26 ==="
	gssh 'cd /tmp/mv2f && cp -f /mnt/t0dod/mode-smoke/corten_mode_smoke /mnt/t0dod/mode-smoke/run_mode_smoke.sh . && chmod +x corten_mode_smoke run_mode_smoke.sh && bash ./run_mode_smoke.sh > smoke-$USER.full 2>&1; echo SMOKE_RC=$?; echo PASS=$(grep -c "^PASS" smoke-root.full); echo FAIL=$(grep -c "^FAIL" smoke-root.full)'
	echo "=== leg4: metis x2 checksum 同基准 ($BASELINE) ==="
	gssh 'cd /tmp/mv2f && [ -s corpus.txt ] || /mnt/r6dg/gen_text 8 corpus.txt >/dev/null 2>&1; for i in 1 2; do timeout 300 /mnt/r6dg/metis_eq 2 corpus.txt > metis'$TAG'$i.out 2>/dev/null; echo metis'$TAG'$i RC=$?; grep -o "\"distinct_words\":[0-9]*,\"checksum\":\"[a-f0-9]*\"" metis'$TAG'$i.out; done'
	if [ "$LIGHT" = 0 ]; then
		echo "=== leg5: dmesg 静默 (WARN/BUG/oops 扫描) ==="
		gssh 'dmesg | grep -cE "WARNING|BUG:|Oops|general protection" ; dmesg | grep -E "WARNING|BUG:|Oops|general protection" | grep -vE "pgtables_bytes" | head -8'
	fi
	} | tee -a $RES/gate-$TAG.log
	say "gate done tag=$TAG"
}

prepare() {
	gssh 'mkdir -p /tmp/mv2f && cp -f /mnt/cortenmm/bench/heap-churn /tmp/mv2f/ 2>/dev/null; chmod +x /tmp/mv2f/heap-churn 2>/dev/null; true'
}

case "${1:-}" in
boot)  vm_boot "${2:-journal}" "${3:-console-mv2final.log}" ;;
gate)  prepare; gate "${2:?tag}" 0 ;;
light) prepare; gate "${2:?tag}" 1 ;;
kill)  vm_kill; say killed ;;
*) echo "usage: $0 {boot [journal|off] console|gate tag|light tag|kill}"; exit 2 ;;
esac
