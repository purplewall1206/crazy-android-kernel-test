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

# long-haul arm for the full battery (LTP build alone runs tens of minutes);
# the 30s probe arm killed the previous run's battery mid-SEC4.
gssh_long() {
	timeout 7200 ssh -i $SSHKEY -p $PORT -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 \
		-o ServerAliveInterval=30 \
		root@127.0.0.1 "$@" 2>/dev/null
}

# Ledger #3 (w3fix4): the artifact fetch's SSH resilience.  The original
# single `timeout 600 scp` died whole on one transient TLS/SSH reset (the
# r07/mv3d first battery lost its P2 artifact fetch exactly that way -- the
# "timeout: failed to run command 'g'" 66-byte files).  Now: direct scp with
# 3 attempts, and on persistent failure the segmented arm -- the guest splits
# the file into 16M parts (each part a fresh short scp, 3 attempts apiece),
# the host reassembles and verifies the guest's md5.  $1 = guest path,
# $2 = local destination file.
gfetch() {
	local GPATH="$1" LPATH="$2" try rc=1 sum
	for try in 1 2 3; do
		timeout 600 scp -i $SSHKEY -P $PORT \
			-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
			"root@127.0.0.1:$GPATH" "$LPATH" 2>/dev/null && { rc=0; break; }
		say "gfetch direct try $try failed ($(basename $GPATH))"
		sleep 3
	done
	[ $rc -eq 0 ] && return 0
	say "gfetch falling back to 16M segmented fetch ($(basename $GPATH))"
	sum=$(gssh "split -b 16M '$GPATH' '$GPATH.part-' && md5sum '$GPATH' | cut -d' ' -f1 && ls '$GPATH'.part-* | wc -l" 2>/dev/null | tail -2)
	local want_sum want_n
	want_sum=$(echo "$sum" | head -1)
	want_n=$(echo "$sum" | tail -1)
	case "$want_sum" in
	''|*[!0-9a-f]*) say "gfetch: guest split failed"; return 1 ;;
	esac
	: > "$LPATH"
	local part
	for part in $(gssh "ls -1 '$GPATH'.part-* 2>/dev/null" 2>/dev/null); do
		local base
		base=$(basename "$part")
		rc=1
		for try in 1 2 3; do
			timeout 300 scp -i $SSHKEY -P $PORT \
				-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
				"root@127.0.0.1:$part" "$RES/.gfetch-part" 2>/dev/null && { rc=0; break; }
			say "gfetch part $base try $try failed"
			sleep 3
		done
		[ $rc -ne 0 ] && { say "gfetch: part $base lost"; return 1; }
		cat "$RES/.gfetch-part" >> "$LPATH" && rm -f "$RES/.gfetch-part"
	done
	local got_sum
	got_sum=$(md5sum "$LPATH" | cut -d' ' -f1)
	if [ "$got_sum" = "$want_sum" ]; then
		gssh "rm -f '$GPATH'.part-*" 2>/dev/null
		say "gfetch segmented OK ($(basename $GPATH), $want_n parts, md5 ${want_sum:0:12})"
		return 0
	fi
	say "gfetch: md5 mismatch got=$got_sum want=$want_sum"
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
	say "battery start world=$W -> $(basename $OUT)"
	gssh_long "bash /mnt/mv3d/guest-battery.sh $W" > $OUT 2>&1
	local rc=$?
	# artifact tarball: per-test ltp logs + smoke/metis/sweep outputs
	# (ledger #3: resilient fetch -- 3 direct attempts, then 16M parts)
	gssh "cd /tmp && tar czf mv3d-art-$W.tar.gz mv3d 2>/dev/null" || true
	rm -f $RES/mv3d-art-$W.tar.gz
	if gfetch "/tmp/mv3d-art-$W.tar.gz" "$RES/mv3d-art-$W.tar.gz"; then
		tar xzf $RES/mv3d-art-$W.tar.gz -C $RES/ 2>/dev/null
	else
		say "WARN: artifact tarball for world=$W unfetchable"
	fi
	say "battery done world=$W rc=$rc"
	echo "BATTERY_RC=$rc" >> $STATE
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
