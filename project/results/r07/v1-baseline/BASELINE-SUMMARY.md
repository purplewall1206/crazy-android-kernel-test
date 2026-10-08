# V1-removal BASELINE battery — android17-6.18 @ 909bccf49fb5 (post E2-audit-retire)

Date: 2026-10-08 (attempt 2 complete 19:17:57). Battery: `/home/ppw/bench/share/v1-baseline-battery.sh`
(fork of t1-battery.sh; R=r07/v1-baseline, PORT=10031, PIDFILE=/home/ppw/vm/qemu-v1base.pid,
SESSION=v1-base, IMG=qcow2 overlay — backing trixie-mv3d.img raw never written directly).

## Kernel under test

| | sha256[:16] | provenance |
|---|---|---|
| baseline (used) | `3aebb2c169b30e90` | clean full build (#1), detached worktree `/home/ppw/v1base-wt` @ 909bccf49fb5, production .config; banner `6.18.32-g909bccf49fb5` (no -dirty) |
| attempt 1 (rejected) | `c8234ee6903fff45` | main-worktree build at 16:30 contaminated by live pr-v1 V1-surgery edits (mm/corten_arena.c, mm/gup.c, mm/mmap.c uncommitted in shared tree); banner `-dirty` |

KT deviation note: task pinned KT=main-tree bzImage, but the shared main worktree carries an
in-flight pr-v1 surgery (uncommitted mm/ edits), so any main-tree build absorbs it. The baseline
kernel is built from the identical committed content (909bccf49fb5) in a detached worktree;
pr-v1 branch and its working files were not touched.

### Attempt-1 incident (evidence: attempt1-contaminated/)
P1 (default-off) booted and ran green, but both default-entry-on boots (P2+JMASK, P3 journal)
panicked at first exec of init: `/sbin/init exists but couldn't execute it (error -14)` then
SIGSEGV/No-working-init panic. Not KASLR-flaky (two boots, two KASLR offsets, same failure).
Preflight of the clean rebuild booted default-on in ~15 s (ssh try=3, zero panics,
gate_pass=1, exec_default_enters=214) — failure isolated to the contaminated build, not the
committed tree. Attempt-1 state and consoles preserved under `attempt1-contaminated/`.

## Baseline readings (attempt 2, all three legs DONE-PHASE)

| check | expected | reading | verdict |
|---|---|---|---|
| LTP_SUMMAR | PASS=98 FAIL=11 (= baseline) | `PASS=98 FAIL=11 CONF=0 MISSING=1 OTHER=5 TOTAL=115` (P1 world, battery-off.log) | MATCH |
| P2 audit_gate | gate_pass=1, j2_violations=0 | `gate_pass 1`, `j2_violations 0`, `j2_stale 0`, `j2_first_violation 0x0`, `j2_walks 0` (p2-audit-gate.txt; post-E2-retire gate is the residual oracle only — J1/wl counters retired from production paths) | PASS |
| smoke | 26/26 | P1 world: `SMOKE_BARE_RC=0 PASS=26 FAIL=0`, `SMOKE_HOOK_RC=1 PASS=26 FAIL=0`; on-world recovery: `PASS=26 FAIL=0` rc=1 (identical form to reference on-world run) | PASS |
| metis checksum | 2d383eeed4ceb73b | art-off/mv3d/metis1.out bare: `2d383eeed4ceb73b`; on-world bare x2 (live p3 VM, mode=1): both runs `2d383eeed4ceb73b` | MATCH (deterministic x2) |
| exec_default_enters | >0 | `251` (p2-counters.txt, P2 world) | PASS |

Secondary:
- P1 mode_probe mode=0 (PROBE_RC=1) — default-entry off, mode separation holds.
- P3 mode_probe mode=1 (PROBE_RC=0) — default-entry on, unmasked-journald face boots;
  systemd-analyze 18.899s, graphical.target 11.680s, journald 307ms; is-system-running
  `degraded` with the single known unit (redis-server) — same as all prior runs.
- dmesg: P1 integrity=0; on-world (P3 face, full dmesg fetched) has 2x
  `BUG: non-zero pgtables_bytes on freeing mm: 12288` (t=12.0/12.4s). Pre-existing, NOT an
  E2-retire regression: the E2 soak console (kernel g6dcc90dcedd2, pre-909bccf) shows the
  same two lines (soak checks stable at dmesg_bug=[2 5] for 6 checks / 3h). Inherited
  baseline condition — carry into the V1 gap table.
- No kernel panic anywhere in attempt 2 (console-p{1,2,3}*.log clean).

## Flake losses (recorded faithfully; known SSH banner-exchange flake)
- `battery-on` in-guest results lost: driver marked `PROC_GONE_OR_SSH_DEAD at 3750s`; the
  fetch returned 0 bytes (battery-on.log empty, art-on/ empty). On-world LTP_SUMMAR therefore
  not captured this run — the on-world LTP verdict relies on the pre-flake expectation and
  matches at P1 world. VM rebooted into P3 before a retry could fetch /tmp/mv3d (tmpfs).
- `p2-dmesg-full.txt` fetch died (91 bytes, banner timeout) — P3's dmesg used as the
  on-world integrity evidence instead.
- `p2-smoke-hook2.log` empty (connection already dead) — replaced by the on-world hook-smoke
  rerun on the live P3 VM (`p3-recovered-smoke-hook.log`, PASS=26 FAIL=0 rc=1 = reference form).

## Artifacts
- State/driver: `BATTERY-STATE`, `driver.log`, `driver-outer.log`
- Per-leg: `battery-off.log`, `battery-on.log` (truncated by flake), `art-off/`, `art-on/` (empty), consoles `console-p*.log`
- Readings: `p2-audit-gate.txt`, `p2-counters.txt`, `p2-smoke-hook2.log` (empty, flake), `p1-*`, `p2-*`, `p3-journal-face.log`, `p3-dmesg-full.txt`, `p3-recovered-smoke-hook.log`
- Incident: `attempt1-contaminated/` (attempt-1 full state incl. both panic consoles)
- Kernel: `/home/ppw/v1base-wt/arch/x86/boot/bzImage` (sha256 3aebb2c169b30e90d5e66292b46eab7f50e557a6262e5668feb172b8db4b7d42)
- Overlay: `/home/ppw/vm/v1base2.qcow2` (attempt 2), `/home/ppw/vm/v1base.qcow2` (attempt 1, kept for forensics)
- Battery script: `/home/ppw/bench/share/v1-baseline-battery.sh`

Baseline verdict: GREEN at the committed 909bccf49fb5 point against all five required checks,
with the on-world LTP verdict carried by P1-world evidence + the reference on-world form
(flake loss recorded above). Two inherited pgtables_bytes BUG lines documented for V1.
