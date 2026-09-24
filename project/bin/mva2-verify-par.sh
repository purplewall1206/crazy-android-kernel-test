#!/bin/bash
# mva2-verify-par.sh — 32-core parallel verification matrix.
# Legs run concurrently in separate worktrees/dirs; results land per-leg.
# Usage: mva2-verify-par.sh <tree-with-increment> <logdir> [diffname]
set -u
TREE=${1:?tree}; LOGDIR=${2:?logdir}; NAME=${3:-par}
mkdir -p "$LOGDIR"
PIDS=(); LEGS=()
fail=0
run_kunit_leg() { # legname cfg_extra outfile
	local leg=$1 extra=$2 out=$3
	(
	cd "$TREE"
	timeout 600 qemu-system-x86_64 -enable-kvm -m 2048 -smp 6 \
		-kernel "$TREE/arch/x86/boot/bzImage" \
		-append "console=ttyS0 panic=-1 corten=on $extra" \
		-nographic -no-reboot >"$out" 2>&1
	) &
	PIDS+=($!); LEGS+=("$leg")
}
echo "par-matrix start $(date '+%F %T') tree=$TREE" > "$LOGDIR/state.txt"
cd "$TREE"
step() { echo "[$(date '+%T')] == $1 ==" >> "$LOGDIR/state.txt"; }

# Build once (shared by all legs)
step "build"
make -j24 > "$LOGDIR/build.log" 2>&1 || { echo "FAIL build" >> "$LOGDIR/state.txt"; exit 1; }
echo "BUILD OK" >> "$LOGDIR/state.txt"

# KUnit legs in parallel: on / off / DAS / lockdep already built into image? No --
# each variant needs its own kernel. Build variants SEQUENTIALLY but fast (-j24),
# then run each variant's KUnit concurrently against its own bzImage.
for V in "" DEBUG_ATOMIC_SLEEP PROVE_LOCKING; do
	step "build-variant-${V:-base}"
	./scripts/config ${V:+-e $V} && make olddefconfig >/dev/null 2>&1
	make -j24 > "$LOGDIR/build-${V:-base}.log" 2>&1 || { echo "FAIL build ${V:-base}" >> "$LOGDIR/state.txt"; exit 1; }
	cp arch/x86/boot/bzImage "$LOGDIR/bzImage-${V:-base}"
	case "$V" in
	"") run_kunit_leg on "kunit.filter_glob=corten*" "$LOGDIR/kunit-on.log" ;;
	DEBUG_ATOMIC_SLEEP) run_kunit_leg das "kunit.filter_glob=corten*" "$LOGDIR/kunit-das.log" ;;
	PROVE_LOCKING) run_kunit_leg lk "kunit.filter_glob=corten*" "$LOGDIR/kunit-lk.log" ;;
	esac
done
# plain = base image, corten=off
run_kunit_leg off "kunit.filter_glob=corten*" "$LOGDIR/kunit-off.log"
wait
for f in "$LOGDIR"/kunit-{on,off,das,lk}.log; do
	grep -q "not ok" "$f" && { echo "FAIL $(basename $f) has not-ok" >> "$LOGDIR/state.txt"; fail=1; }
	grep -E "# (corten|corten_arena|corten_fault): pass:" "$f" | tail -3 >> "$LOGDIR/state.txt"
done
[ $fail = 1 ] && exit 1
echo "KUNIT PAR-4 GREEN" >> "$LOGDIR/state.txt"

# =n objects (sequential, fast)
step "n-objects"
./scripts/config -d CORTEN_MM -d CORTEN_MM_ARENA -d CORTEN_MM_KUNIT_TEST \
	-d CORTEN_MM_ARENA_KUNIT_TEST -d CORTEN_MM_ARENA_FAULT_KUNIT_TEST && make olddefconfig >/dev/null 2>&1
OBJ="mm/mmap.o mm/mmap_lock.o mm/memory.o mm/mprotect.o mm/madvise.o mm/mremap.o mm/rmap.o mm/gup.o mm/oom_kill.o mm/swapfile.o mm/migrate.o mm/mempolicy.o mm/vma.o mm/mincore.o mm/msync.o arch/x86/mm/fault.o arch/x86/kernel/sys_x86_64.o"
make $OBJ > "$LOGDIR/build-n.log" 2>&1 || { echo "FAIL =n" >> "$LOGDIR/state.txt"; exit 1; }
nm $(find . -name built-in.a -path "*mm*" | head -1) 2>/dev/null | grep -c corten | grep -q "^0$" || { echo "FAIL =n symbols" >> "$LOGDIR/state.txt"; exit 1; }
echo "=n 16 objects zero symbols" >> "$LOGDIR/state.txt"
./scripts/config -e CORTEN_MM -e CORTEN_MM_ARENA -e CORTEN_MM_KUNIT_TEST \
	-e CORTEN_MM_ARENA_KUNIT_TEST -e CORTEN_MM_ARENA_FAULT_KUNIT_TEST && make olddefconfig >/dev/null 2>&1 && make -j24 > "$LOGDIR/build-restore.log" 2>&1
echo "PAR MATRIX DONE $(date '+%F %T')" >> "$LOGDIR/state.txt"
