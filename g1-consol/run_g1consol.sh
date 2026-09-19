#!/usr/bin/env bash
# run_g1consol.sh - G1 consolidation re-test (M8: 5-round ABAB on the four
# noise-flagged cells of t5-run2).  Guest side, root, corten=on r06-rogue.
#
# Underlying primitives of bench/t5/run_t5_compare.sh (params pinned):
#   mmbench   : mmbench <bench> <cont> <t> 1 <seed>,
#               seed = 20260913 + bidx*1009 + cidx*97 + t*7 + k
#               (BENCHES order mmap mmap-pf pf unmap-virt unmap; CONTS low high;
#               k = round 1..5; identical seed across arms = paired work units)
#   arms      : BASE = env -u LD_PRELOAD (never ENTERs MODE)
#               T0   = LD_PRELOAD=<hook> CORTEN_MODE_HOOK_STRICT=1
#               (hook built from /mnt/cortenmm/bench/mode-hook/corten_mode_hook.c)
#   order     : ABAB per cell (base,t0) x 5 rounds, same as the T5 driver
#   psearchy  : psearchy_eq 8 /root/data/text368.txt (M1: THREADS=8, T368;
#               metric 'seconds', lower=better), one EXCLUDED warmup leg first
#               (t5-run2 registered rep1 warmup contamination on BOTH arms).
#
# D6 same-binary: mmbench/psearchy_eq are copied from the 9p tree to
# /root/g1consol/bin and their sha256 must equal the t5-run2 env.txt values.
# All leg output lands guest-local under /root/g1consol (9p cache independence);
# progress is mirrored to /mnt/share/g1consol-progress.log for host polling.
set -u

OUT=/root/g1consol
MIRR=/mnt/share/g1consol-progress.log
BENCH9P=/mnt/cortenmm/bench
DBG=/sys/kernel/debug/corten
SEED_BASE=20260913
MIN_SECONDS=1
LEG_TIMEOUT=120
START=$(date +%s)

mkdir -p "$OUT/raw" "$OUT/bin"
LOG="$OUT/progress.log"
: >"$LOG"
log() {
	echo "[g1 $(printf '%4ds' $(( $(date +%s) - START )))] $*" >>"$LOG"
	echo "[g1 $(printf '%4ds' $(( $(date +%s) - START )))] $*" >>"$MIRR"
}

# ---------------------------------------------------------------- preflight
[ "$(id -u)" = 0 ] || { log "FATAL: not root"; exit 1; }
[ "$(awk '/^enabled /{print $2}' "$DBG/arena_stats" 2>/dev/null)" = "1" ] ||
	{ log "FATAL: corten not enabled"; exit 1; }
pgrep -a mmbench >/dev/null && { log "FATAL: mmbench already running"; exit 1; }
pgrep -a psearchy >/dev/null && { log "FATAL: psearchy already running"; exit 1; }

# same-binary copies (sha256 audited against t5-run2 meta/env.txt)
cp -f "$BENCH9P/mmbench/mmbench" "$OUT/bin/mmbench"
cp -f "$BENCH9P/apps/psearchy_eq" "$OUT/bin/psearchy_eq"
MMB="$OUT/bin/mmbench"
PSQ="$OUT/bin/psearchy_eq"
log "mmbench sha256 $(sha256sum "$MMB" | awk '{print $1}') (run2: 2a9c066c...)"
log "psearchy sha256 $(sha256sum "$PSQ" | awk '{print $1}') (run2: edeb2424...)"

HOOK="$OUT/corten_mode_hook.so"
gcc -shared -fPIC -O2 -Wall -Wextra -o "$HOOK" \
	"$BENCH9P/mode-hook/corten_mode_hook.c" 2>"$OUT/hook-build.err" ||
	{ log "FATAL: hook build failed"; cat "$OUT/hook-build.err" >>"$LOG"; exit 1; }
log "hook built sha256 $(sha256sum "$HOOK" | awk '{print $1}') (run2: 4846da71...)"

{
	echo "utc: $(date -u +%FT%TZ)"
	echo "kernel: $(uname -r)"
	echo "cmdline: $(cat /proc/cmdline)"
	echo "hostname: $(hostname)"
	echo "nproc: $(nproc)"
	echo "loadavg_start: $(cat /proc/loadavg)"
	echo "mmbench_sha256: $(sha256sum "$MMB" | awk '{print $1}')"
	echo "psearchy_eq_sha256: $(sha256sum "$PSQ" | awk '{print $1}')"
	echo "hook_sha256: $(sha256sum "$HOOK" | awk '{print $1}')"
	echo "hook_src: $BENCH9P/mode-hook/corten_mode_hook.c"
	echo "text368: $(stat -c %s /root/data/text368.txt) /root/data/text368.txt"
	echo "seed_formula: 20260913 + bidx*1009 + cidx*97 + t*7 + k (k=1..5)"
} >"$OUT/meta-env.txt"
cp "$DBG/arena_stats" "$OUT/arena_stats.before" 2>/dev/null || true

# ---------------------------------------------------------------- leg runner
# leg <arm> <outbase> <cmd...>: native single-line JSON -> <outbase>.json
# (wrapped with arm/utc/rc/argv), stderr -> <outbase>.err, rc -> <outbase>.rc
leg() {
	local arm=$1 base=$2 rc; shift 2
	if [ "$arm" = t0 ]; then
		env LD_PRELOAD="$HOOK" CORTEN_MODE_HOOK_STRICT=1 \
			timeout -k 10 "$LEG_TIMEOUT" "$@" >"$base.native" 2>"$base.err"
	else
		env -u LD_PRELOAD -u CORTEN_MODE_HOOK_STRICT \
			timeout -k 10 "$LEG_TIMEOUT" "$@" >"$base.native" 2>"$base.err"
	fi
	rc=$?
	echo "$rc" >>"$base.rc"
	python3 - "$base" "$arm" "$rc" "$*" <<'PYEOF'
import datetime, json, os, sys
base, arm, rc, argv = sys.argv[1:5]
rec = {"arm": arm, "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
       "rc": int(rc), "argv": argv}
try:
    with open(base + ".native") as f:
        rec.update(json.load(f))
except Exception as e:
    rec["parse_error"] = str(e)
with open(base + ".json", "w") as f:
    f.write(json.dumps(rec, separators=(",", ":")) + "\n")
os.remove(base + ".native")
PYEOF
	if [ "$rc" -ne 0 ]; then
		log "FAIL: $(basename "$base") rc=$rc"
	else
		log "ok: $(basename "$base")"
	fi
}

# cell_mmbench <bench> <cont> <t>: 5 ABAB rounds
cell_mmbench() {
	local b=$1 c=$2 t=$3 bidx cidx k seed
	case "$b" in
		mmap) bidx=0 ;; mmap-pf) bidx=1 ;; pf) bidx=2 ;; unmap-virt) bidx=3 ;; unmap) bidx=4 ;;
	esac
	case "$c" in low) cidx=0 ;; high) cidx=1 ;; esac
	log "=== cell mmbench $b/$c/t$t (bidx=$bidx cidx=$cidx) ==="
	for k in 1 2 3 4 5; do
		seed=$((SEED_BASE + bidx * 1009 + cidx * 97 + t * 7 + k))
		leg base "$OUT/raw/mmbench_${b}_${c}_t${t}_run${k}.base" \
			"$MMB" "$b" "$c" "$t" "$MIN_SECONDS" "$seed"
		leg t0 "$OUT/raw/mmbench_${b}_${c}_t${t}_run${k}.t0" \
			"$MMB" "$b" "$c" "$t" "$MIN_SECONDS" "$seed"
	done
}

# ---------------------------------------------------------------- cells
# order: G1-critical first, psearchy adjudication in the middle
cell_mmbench unmap-virt low 4
cell_mmbench unmap-virt low 8

log "=== psearchy_eq warmup leg (EXCLUDED from stats) ==="
env -u LD_PRELOAD -u CORTEN_MODE_HOOK_STRICT \
	timeout -k 10 "$LEG_TIMEOUT" "$PSQ" 8 /root/data/text368.txt \
	>"$OUT/psearchy_warmup.out" 2>"$OUT/psearchy_warmup.err"
log "warmup rc=$?"
for k in 1 2 3 4 5; do
	leg base "$OUT/raw/psearchy_eq_run${k}.base" "$PSQ" 8 /root/data/text368.txt
	leg t0 "$OUT/raw/psearchy_eq_run${k}.t0" "$PSQ" 8 /root/data/text368.txt
done

cell_mmbench mmap-pf low 4
cell_mmbench mmap-pf low 8
cell_mmbench unmap high 4

# ---------------------------------------------------------------- closeout
cp "$DBG/arena_stats" "$OUT/arena_stats.after" 2>/dev/null || true
dmesg 2>/dev/null | grep -icE "corten.*(warn|bug|oops)" >"$OUT/corten_dmesg_count.txt"
echo "loadavg_end: $(cat /proc/loadavg)" >>"$OUT/meta-env.txt"
log "ALL DONE"
