#!/bin/bash
# mv3d-gate.sh -- MV3.d full-system MODE battery host driver.
#   bash mv3d-gate.sh boot on|off   # boot VM in that world (console to results)
#   bash mv3d-gate.sh run on|off    # run guest battery, capture logs + artifacts
#   bash mv3d-gate.sh full          # off battery -> on battery, one shot
#   bash mv3d-gate.sh kill          # pidfile-only kill (never TaskStop)
# Isolation: PORT=10031, trixie-mv3d.img (private copy), qemu-mv3d.pid,
#   monitor-mv3d.sock, tmux mv3d-vm -- no intersection with mv3a/mv3b/w7v2 VMs.
set -u
WT=/home/ppw/linux-6.18-mva
RES=$WT/project/results/r07/mv3d
KERN=$WT/arch/x86/boot/bzImage
IMG=/home/ppw/vm/trixie-mv3d.img
SHARE=/home/ppw/bench/share
PORT=10031
SSHKEY=/home/ppw/vm/trixie.id_rsa
PIDFILE=/home/ppw/vm/qemu-mv3d.pid
SESSION=mv3d-vm
STATE=$RES/gate-state.txt
mkdir -p $RES

say() { echo "[mv3d] $(date '+%F %T') $*" | tee -a $STATE; }

gssh() {
	timeout 30 ssh -i $SSHKEY -p $PORT -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 \
		root@127.0.0.1 "$@" 2>/dev/null
}

# Ledger #3 (r07 mv3d): the battery leg lost its SSH session mid-run once
# (7216s in-guest run), which truncated battery-on.log and left the
# artifact tarball an empty shell (p2-audit-gate.txt &co).  Every remote
# step from here on is either (a) short and retried, or (b) detached in
# the guest and polled through short-lived sessions -- a dropped session
# can no longer kill or truncate a run.

# Short-command retry: up to $1 attempts, 5s backoff.  $2.. the command.
gssh_retry() {
	local n=${1:?attempts}; shift
	local i
	for i in $(seq 1 $n); do
		if gssh "$@"; then
			return 0
		fi
		sleep 5
	done
	return 1
}

# Long-haul arm for interactive phases (LTP build alone runs tens of
# minutes); the battery leg itself no longer rides on it (vm_run runs
# the battery detached and polls).  The 30s probe arm killed the
# previous run's battery mid-SEC4.
gssh_long() {
	timeout 7200 ssh -i $SSHKEY -p $PORT -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 \
		-o ServerAliveInterval=30 \
		root@127.0.0.1 "$@" 2>/dev/null
}

# Pull one guest file with integrity verification: up to $4 attempts,
# each fetch compared against the guest-side sha256.  Returns 0 only
# when the local copy is byte-exact.
scp_verified() { # $1 guest path  $2 local path  $3 label  $4 attempts
	local g=$1 l=$2 label=$3 n=${4:-5} i gh lh
	for i in $(seq 1 $n); do
		gh=$(gssh "sha256sum $g 2>/dev/null | cut -d' ' -f1") || { sleep 5; continue; }
		[ -n "$gh" ] || { sleep 5; continue; }
		timeout 600 scp -i $SSHKEY -P $PORT -o StrictHostKeyChecking=no \
			-o UserKnownHostsFile=/dev/null "root@127.0.0.1:$g" "$l" 2>/dev/null || { sleep 5; continue; }
		lh=$(sha256sum "$l" 2>/dev/null | cut -d' ' -f1)
		if [ "$gh" = "$lh" ]; then
			say "fetch ok ($label, try $i)"
			return 0
		fi
		say "fetch mismatch ($label, try $i): guest=$gh local=$lh"
		sleep 5
	done
	say "FAIL: fetch $label after $n attempts"
	return 1
}

vm_kill() {
	if [ -f $PIDFILE ]; then
		kill "$(cat $PIDFILE)" 2>/dev/null || true
		rm -f $PIDFILE
	fi
	tmux kill-session -t $SESSION 2>/dev/null || true
	sleep 2
}

vm_boot() { # $1 = on|off
	local W=$1 EXTRA="" CON
	if [ "$W" = on ]; then
		EXTRA="corten=on corten_mode_default=on"
		CON=console-mv3d-on.log
	else
		EXTRA="corten=on"
		CON=console-mv3d-off.log
	fi
	vm_kill
	tmux new-session -d -s $SESSION \
		"qemu-system-x86_64 -enable-kvm -cpu host -m 4096 -smp 8 \
		-kernel $KERN \
		-append 'console=ttyS0 root=/dev/vda rw net.ifnames=0 systemd.mask=sys-kernel-config.mount $EXTRA mitigations=off kunit.enable=0 log_buf_len=16M' \
		-drive file=$IMG,if=virtio,format=raw \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
		-device virtio-net-pci,netdev=net0 \
		-fsdev local,id=fs0,path=$SHARE,security_model=none \
		-device virtio-9p-pci,fsdev=fs0,mount_tag=hostshare \
		-display none -serial file:$RES/$CON \
		-monitor unix:/home/ppw/vm/monitor-mv3d.sock,server,nowait \
		-pidfile $PIDFILE"
	say "VM booting world=$W kernel=$(sha256sum $KERN | cut -c1-16) console=$CON"
	local i
	for i in $(seq 1 60); do
		if gssh 'echo up' >/dev/null 2>&1; then
			say "guest ssh up (try $i)"
			gssh 'mkdir -p /tmp/mv3d; mount -t 9p -o trans=virtio hostshare /mnt 2>/dev/null; mount -t debugfs none /sys/kernel/debug 2>/dev/null; true' \
				|| { say "FAIL: share/debugfs mount"; return 1; }
			return 0
		fi
		sleep 5
	done
	say "FAIL: guest ssh never came up"
	return 1
}

vm_run() { # $1 = on|off
	local W=$1
	local OUT=$RES/battery-$W.log
	local i n RLOG RPOLL
	RLOG=/tmp/mv3d/battery-$W.log
	say "battery start world=$W (detached) -> $(basename $OUT)"

	# Detached start: the battery lives in the guest, not in the SSH
	# session.  A drop between start and the last poll costs nothing.
	# The wrapper records the battery's exit status for the poll.  The
	# start is idempotent (a retried attempt after a lost connection
	# must not spawn a second battery over the same log).
	gssh_retry 5 "if [ -f $RLOG.pid ] && kill -0 \$(cat $RLOG.pid 2>/dev/null) 2>/dev/null; then echo ALREADY-RUNNING; else rm -f $RLOG $RLOG.rc $RLOG.pid; cd /tmp && nohup bash -c 'bash /mnt/mv3d/guest-battery.sh $W; echo \$? > $RLOG.rc' > $RLOG 2>&1 < /dev/null & echo \$! > $RLOG.pid; echo STARTED; fi" \
		|| { say "FAIL: battery start"; echo "BATTERY_RC=start-fail" >> $STATE; return 1; }

	# Poll the completion marker with short, retried sessions.
	n=""
	for i in $(seq 1 1440); do		# 1440 * 10s = 4h ceiling
		RPOLL=$(gssh_retry 3 "if [ -f $RLOG.rc ]; then echo DONE; cat $RLOG.rc; else if kill -0 \$(cat $RLOG.pid 2>/dev/null) 2>/dev/null; then echo RUNNING; else echo DONE; echo 1; fi; fi")
		case "$RPOLL" in
		*DONE*)
			n=$(echo "$RPOLL" | grep -oE '^[0-9]+$' | head -1)
			[ -n "$n" ] || n=0
			break
			;;
		esac
		sleep 10
	done
	if [ -z "$n" ]; then
		n=1
		say "WARN: battery poll ceiling hit or poll failed (last=$RPOLL)"
	fi

	# The battery log itself, byte-exact or loud.
	scp_verified "$RLOG" "$OUT" "battery-$W.log" 5 \
		|| say "WARN: battery log fetch failed; console remains the record"

	# Artifact tarball, segmented: per-chunk fetches are individually
	# retryable and hash-verified, then reassembled and checked against
	# the guest-side whole-file sha256.
	gssh_retry 3 "cd /tmp && tar czf mv3d-art-$W.tar.gz mv3d 2>/dev/null; split -b 16m mv3d-art-$W.tar.gz /tmp/mv3d-art-$W.part-; sha256sum mv3d-art-$W.tar.gz > /tmp/mv3d-art-$W.sha" || true
	local parts ok=1
	parts=$(gssh_retry 3 "ls /tmp/mv3d-art-$W.part-* 2>/dev/null | wc -l")
	case "$parts" in ''|*[!0-9]*) parts=0 ;; esac
	if [ "$parts" -gt 0 ]; then
		rm -f $RES/mv3d-art-$W.tar.gz.assembled
		for i in $(seq 0 $((parts - 1))); do
			local P Part
			Part=$(printf "%02d" "$i")
			P=$RES/mv3d-art-$W.part-$Part
			scp_verified "/tmp/mv3d-art-$W.part-$Part" "$P" \
				"art-$W part $Part" 5 || { ok=0; break; }
			cat $P >> $RES/mv3d-art-$W.tar.gz.assembled
		done
		if [ $ok = 1 ]; then
			local gh lh
			gh=$(gssh_retry 3 "cut -d' ' -f1 /tmp/mv3d-art-$W.sha")
			lh=$(sha256sum $RES/mv3d-art-$W.tar.gz.assembled 2>/dev/null | cut -d' ' -f1)
			if [ -n "$gh" ] && [ "$gh" = "$lh" ]; then
				mv $RES/mv3d-art-$W.tar.gz.assembled $RES/mv3d-art-$W.tar.gz
				tar xzf $RES/mv3d-art-$W.tar.gz -C $RES/ 2>/dev/null \
					&& say "artifacts verified world=$W"
			else
				say "FAIL: artifact sha mismatch world=$W (guest=$gh local=$lh)"
			fi
		fi
		gssh_retry 3 "rm -f /tmp/mv3d-art-$W.part-*" || true
	else
		# Segmentation refused (tiny/no artifacts): the direct fetch.
		if scp_verified "/tmp/mv3d-art-$W.tar.gz" "$RES/mv3d-art-$W.tar.gz" "art-$W" 5; then
			tar xzf $RES/mv3d-art-$W.tar.gz -C $RES/ 2>/dev/null
		fi
	fi
	say "battery done world=$W rc=$n"
	echo "BATTERY_RC=$n" >> $STATE
	grep -E "^===SEC|_RC=|LTP_SUMMAR|MVA1 |S3_RC|corr_signatures|pgtables_residue" $OUT | head -40
}

case "${1:-}" in
boot) vm_boot "${2:?on|off}" ;;
run)  vm_run "${2:?on|off}" ;;
kill) vm_kill; say killed ;;
full)
	: > $STATE
	echo "=== MV3.d full battery $(date '+%F %T') kernel=$(sha256sum $KERN | cut -c1-16)" >> $STATE
	say "stage=off-boot"
	vm_boot off   || { say "ABORT off boot"; exit 1; }
	say "stage=off-battery"
	vm_run off
	say "stage=off-done, stage=on-boot"
	vm_boot on    || { say "ABORT on boot"; exit 1; }
	say "stage=on-battery"
	vm_run on
	say "stage=on-done"
	vm_kill
	say "FULL DONE"
	;;
killq) vm_kill; say killed ;;
*) echo "usage: $0 {boot on|off|run on|off|full|kill}"; exit 2 ;;
esac
