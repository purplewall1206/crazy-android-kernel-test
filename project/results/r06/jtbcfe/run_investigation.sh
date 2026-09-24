#!/bin/bash
# jtbcfe investigation driver - INSIDE guest, root, from the 9p share.
# Usage: /mnt/jtbcfe/run_investigation.sh [quick|full]
set -u
HERE=/mnt/share/jtbcfe
OUT=$HERE/out
DBG=/sys/kernel/debug/corten
mkdir -p $OUT
log() { echo "[jtbcfe] $*" | tee -a $OUT/driver.log; }
MODE=${1:-quick}

snap_stats() { cat $DBG/arena_stats > $OUT/arena_stats.$1 2>/dev/null || echo none > $OUT/arena_stats.$1; }

cd $HERE
cp -f $HERE/JThreadBench.class . 2>/dev/null
javac HelloFmt.java 2>>$OUT/driver.log || log "javac HelloFmt FAILED"
gcc -shared -fPIC -O2 -o jtbcfe_dump.so dump_maps.c 2>$OUT/dumpbuild.err || log "dump build FAILED"
gcc -O2 -Wall -o watch_maps watch_maps.c 2>>$OUT/driver.log || log "watcher build FAILED"
HOOK=$(find /mnt -maxdepth 3 -name corten_mode_hook.so 2>/dev/null | head -1)
[ -n "$HOOK" ] || { log "FATAL no hook"; exit 1; }
log "hook=$HOOK java=$(which java) jdk=$(java -version 2>&1 | head -1)"

snap_stats before

runleg() {	# runleg <name> <preload> <logname> <args...>
	local name=$1 preload=$2 logn=$3; shift 3
	log "== $name"
	if [ -n "$preload" ]; then
		LD_PRELOAD="$preload" "$@" > $OUT/$logn 2>&1 &
	else
		"$@" > $OUT/$logn 2>&1 &
	fi
	local jp=$!
	if [ -x $HERE/watch_maps ]; then
		$HERE/watch_maps $jp $OUT/watch.$logn.txt 2>/dev/null &
		local wp=$!
	fi
	wait $jp; local rc=$?
	echo "rc=$rc" >> $OUT/$logn
	[ -n "${wp:-}" ] && kill $wp 2>/dev/null
	log "$name rc=$rc CFE=$(grep -c ClassFormatError $OUT/$logn) fmtline=$(grep 'FormatData_en ' $OUT/$logn | tail -1 | cut -c1-120)"
	unset wp
	return $rc
}

# A: base sanity + class source
runleg A-base "" A.base.log java -Xlog:class+load=info HelloFmt
# B: MODE HelloFmt (no threads)
runleg B-mode "$HOOK:$HERE/jtbcfe_dump.so" B.mode.log java -Xlog:class+load=info HelloFmt
# C: MODE JThreadBench 10 1
runleg C-mode "$HOOK:$HERE/jtbcfe_dump.so" C.mode.log java -Xlog:class+load=info -Xmx512m JThreadBench 10 1
# D: share variants
runleg D-off "$HOOK:$HERE/jtbcfe_dump.so" D.share-off.log java -Xshare:off -Xmx512m JThreadBench 10 1
runleg D-on  "$HOOK:$HERE/jtbcfe_dump.so" D.share-on.log  java -Xshare:on  -Xmx512m JThreadBench 10 1
# E: base control for JThreadBench
runleg E-base "" E.base.log java -Xmx512m JThreadBench 10 1

if [ "$MODE" = full ]; then
	# F: registered shape 2000x3 with dumper
	runleg F-2000 "$HOOK:$HERE/jtbcfe_dump.so" F.mode2000.log java -Xlog:class+load=info -Xmx512m JThreadBench 2000 3
	# G: strace the failure (10 threads, whole life)
	command -v strace >/dev/null && runleg G-strace "$HOOK" G.strace.log \
		strace -f -tt -e trace=openat,pread64,read,mmap,munmap,mprotect,futex \
		java -Xmx512m JThreadBench 10 1 || log "G skipped (no strace)"
fi

snap_stats after
log "== arena_stats before/after =="
diff $OUT/arena_stats.before $OUT/arena_stats.after > $OUT/stats.diff.txt
cat $OUT/stats.diff.txt
log "dumps: $(ls /tmp/jtbcfe_dump_*.txt 2>/dev/null | wc -l)"
cp /tmp/jtbcfe_dump_*.txt $OUT/ 2>/dev/null
log done
