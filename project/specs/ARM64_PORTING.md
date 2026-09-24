# CortenMM → ARM64: Porting Design Notes (M9)

Revisions: r1 (2026-09-14, initial design). **r2 (2026-09-16, 实证回填)** —
2026-09-15 aarch64 cross-compile results folded in: §8 P1 status note, §10
OQ4/OQ5 conclusions, new §11 (Round A/B evidence; logs under
`/home/ppw/cortenmm/results/r02/`). **r3 (2026-09-21, M9-P2 钩子落地)** —
lifecycle hooks landed and verified on arm64: §6 r3 status block, §10 OQ4
final resolution, §11.4 item 3, new §11.5 (M9-P2 evidence; logs under
`/home/ppw/cortenmm/publish/results/r07/`).

Status: design document (M9 deliverable). At r1 no code had been
cross-compiled; r2 supersedes that: the arm64 defconfig baseline `Image` and
`mm/corten.o` (Kconfig `depends` released in a throwaway worktree) now build
green on aarch64 (§11). The remaining second half of M9 — bootlin aarch64
toolchain cross-check + `qemu-system-aarch64` smoke — is still open. All line
references are against this tree (`/home/ppw/linux-6.18`, android17-6.18 @
68974e235117 + CortenMM M2a/M2b/M2c-fix1: b8386e4e2467, 2fd4070e745c,
1284a235f751) and against the paper text (`/home/ppw/paper/corte/paper.txt`,
CortenMM SOSP'25). The empirical evidence in §11 comes from the m9 worktree
`/home/ppw/linux-6.18-m9` (branch `m9-arm64`, HEAD `e911b31adb9c` = the three
M2 commits + M3a.F1, BH-symmetric locking); the Round B trial ran in a
since-removed throwaway worktree pinned at the same commit.

## 中文摘要

论文 §4.4 的四条 PTE 位语义假设（有效性 / leaf 判定 / 权限 / accessed-dirty）在
ARM64 上**全部成立且有直接对应位**（§2 对照表逐项给行号）；协议层
（covering-PT-page 锁 + 事务 API + xarray PFN 索引 + pin/stale 生命周期）**可原样移植**，
与体系结构无关。工程量集中在四处：① arm64 contpte 层——fold/unfold 会重写整块
CONT_PTES（16/128/32 个）邻居 PTE 并发 TLBI（`contpte.c:contpte_convert`），
其自身串行化假设是 PTL 而非我们的 descriptor 写锁，是**唯一需要改协议假设的点**
（§3.3，本文最重要的发现）；② BBM/TLB 语义：arm64 "map" 不是单条原子写，
需 break-then-make 且必须用 cmpxchg 感知原语与硬件 AF/DBM 并发写共存（§5）；
③ 几何参数化：CORTEN_PTES_PER_PT_PAGE=512 字面量与 5 级几何在 16K/64K 页基下
编译期即崩（PTRS_PER_PTE=2048/8192，64K 页 3 级无 PUD/P4D 宏），且 arm64 的
4/5 级是**运行时**折叠（pgtable_l4/l5_enabled），§4/§7 给出三页基全表；
④ 钩子落点：arm64 的释放漏斗是 `__pte_free_tlb`（asm/tlb.h:75）+ 已钩好的
asm-generic `pte_free`（117），双漏斗结构与 x86 同构，约 20-40 行（§6）。
估算移植层 450-1000 行（对照论文 Table 5 RISC-V=252 行：我们没有 contpte/BBM/
shadow-VMA 互操作这些 RISC-V 侧不存在的因素），详见 §8。风险表见 §9，
OPEN QUESTION 见 §10。

---

## 1. Executive summary

**Conclusion: the protocol layer ports as-is; the engineering hook layer and
two arm64-specific semantics (contpte, BBM/TLB) are the bulk of the work.**
The paper's own position (paper.txt:748-753) is that ARM "follows CortenMM's
assumptions on the page table format", that the contiguous bit and
break-before-make "do not require changing our design or our proof (§5), but
rather more engineering efforts". Reading the 6.18 arm64 code confirms this —
and additionally shows that the engineering effort is dominated by one thing
the paper never had to face: **on arm64, "write one PTE" is not a
single-PTE operation** once the contpte layer is in play (§3.3).

Quantified against the paper's §6.7 portability data (paper.txt:1373-1377,
Table 5: x86→RISC-V port = 252 LoC for CortenMM vs 699 for Linux; Intel MPK
82 vs 273; Intel TDX 368 vs 471; paper.txt:494-499 claims "< 200 LoC" for
RISC-V + MPK) and against our own x86 implementation:

| Item | Paper (RISC-V port) | Our x86 tree (actual) | ARM64 estimate |
|---|---|---|---|
| Protocol core (lock walk + txn ops) | unchanged | `mm/corten.c` ~700 LoC | **0 LoC** (arch-independent) |
| Data structures + pin/stale | unchanged | `include/linux/corten.h` 494 + `mm/corten.h` 228 | **0 LoC** |
| KUnit (synthetic trees) | n/a | `mm/corten_test.c` 2047 | **0 LoC** (reused) |
| Geometry / covering-level helpers | part of 252 | `corten.c:449-550` ~100 | 80-150 (fold-aware rewrite, §7) |
| Real page-table view (`root` op) | part of 252 | `corten.c:878-967` ~90 | 100-150 (leaf/PMD, runtime folding) |
| Lifecycle hooks | part of 252 | ~35 (`arch/x86/mm/pgtable.c:17-48`, `asm-generic/pgalloc.h:117-128`) | 20-40 (§6) |
| contpte / BBM semantic adaptation | **absent in paper's RISC-V** | absent (x86 has no equivalent) | 200-400 (§3.3, §5) |
| Kconfig/build gating | part of 252 | `mm/Kconfig:1429-1455` | 30-60 |
| arm64 KUnit + boot validation | n/a | n/a | 100-300 |

Total port-layer estimate: **450-1000 LoC** (median ~600), on top of an
M3-integrated x86 codebase whose current size is 4,152 LoC (1,383 + 228 +
494 + 2,047 incl. tests). The paper's 252-LoC RISC-V figure is *not* directly
comparable: it ports a codebase with no VMA layer to keep alive, on an ISA
with no contiguous bit, no 5-level tables, and 4K pages only (paper.txt:669
"currently supports page sizes of 4KB, 2MB, and 1GB"). The honest comparison is:
paper says porting is "straightforward" because the *format assumptions* hold
(§2 confirms, per item, with line numbers); our estimate says the same about
the protocol, and puts the arm64-specific semantic delta at roughly half the
total effort.

## 2. PTE bit semantics: paper assumptions vs arm64

Paper §4.4 (paper.txt:631): the implementation assumes software-controlled
PTE bits can 1) identify entry validity, 2) tell if the entry is a leaf,
3) enforce access permissions, 4) provide access and dirty information; "All
the ISAs CortenMM targets, namely ARM, x86, and RISC-V, meet the above
assumptions" (paper.txt:641-652). Item-by-item against
`arch/arm64/include/asm/pgtable-hwdef.h` (hwdef), `pgtable-prot.h` (prot) and
`pgtable.h` (pgt):

| # | Paper assumption | x86-64 | arm64 | Evidence / notes |
|---|---|---|---|---|
| 1a | "points to something" / present | `PRESENT` bit | `PTE_VALID` bit 0 | hwdef:164; `pte_valid()` pgt:208 |
| 1b | non-present states (swap/virt-alloc) encoded in PTE | swap: !PRESENT layout | swap: `!pte_valid && pte_val != 0`; `pte_none = !pte_val` (pgt:177); type bits `__SWP_TYPE_SHIFT 6 / 5 bits`, offset `<<12` (pgt:1548-1557) | ⚠ different from x86 (x86 reserves PRESENT=0 field space differently); CortenMM does not read swap PTEs for state (metadata array is authoritative, `corten_query` mm/corten.c:1067-1084) so this is informational, but any future "read state from PTE" fast path must not copy x86 encodings |
| 1c | present-but-invalid (PROT_NONE / NUMA) | `!PRESENT` + PROTNONE marker | `PTE_PRESENT_INVALID` **reuses bit 11 (PTE_NG)** when `!PTE_VALID`; `pte_present() = pte_valid \|\| pte_present_invalid` | prot:26, pgt:183, pgt:209-210; `pte_protnone()` pgt:597-608. ⚠ software must not interpret bit 11 as NG while !VALID |
| 2a | leaf at PTE level | always leaf | level-3 descriptor: `PTE_TYPE_PAGE` bits[1:0]=0b11 | hwdef:166 |
| 2b | leaf at PMD/PUD (huge) | `HUGE` bit (paper Fig. 9 L12 ORs it into is_present) | block descriptor: `PMD_TYPE_SECT` vs `PMD_TYPE_TABLE`; `pmd_leaf = pmd_present && !pmd_table` | pgt:817-821, pgt:913-915; `PMD_TYPE_SECT` hwdef:136-138; used exactly like x86 `pmd_leaf()` in our real view (`corten.c:904,910` returns `-EOPNOTSUPP`) → arm64 counterpart: `pmd_leaf()/pud_leaf()` |
| 3a | read/write enforceable | `RW` bit | `PTE_RDONLY` AP[2] bit 7 + `PTE_WRITE` **aliased to PTE_DBM bit 51** | prot:16 (`#define PTE_WRITE (PTE_DBM)`), hwdef:168,173; `pte_write()` pgt:186; `pte_wrprotect()` migrates hw-dirty to sw bit first (pgt:317-329) |
| 3b | user/kernel separation | `U/S` bit | `PTE_USER` AP[1] bit 6; ⚠ **execute-only user mappings do NOT set PTE_USER** | hwdef:167; pgt:188, pgt:212-214 comment; permission check `pte_access_permitted` pgt:251-258 uses PTE_USER+PTE_UXN |
| 4a | accessed | A bit in PTE | `PTE_AF` bit 10 | hwdef:170; `pte_young()` pgt:184; HW can set AF concurrently (FEAT_HAFDBS) → gather via cmpxchg loops (pgt:1329-1340) |
| 4b | dirty | D bit in PTE | hybrid: sw `PTE_DIRTY` bit 55 **or** hw-dirty = `pte_write && !pte_rdonly` (DBM) | prot:18; `pte_hw_dirty` pgt:204; `pte_dirty` pgt:206; encoding table pgt:428-440 |
| — | (not assumed but relevant) special | `SPECIAL` bit | **arm64 HAS `PTE_SPECIAL`, bit 56** | prot:19; `pte_special()` pgt:185; the anticipated "arm64 lacks SPECIAL → procfs special-casing" gap does **not** exist in 6.18. (What arm64 *does* do with it: contpte never folds special mappings, pgt:1716-1717 + contpte.c:226-229) |
| — | uffd-wp | sw bit | `PTE_UFFD_WP` bit 58 / swp bit 3 | prot:30-36 |

Summary: **every paper assumption has a 1:1 arm64 counterpart**; the caveats
are (a) `!PTE_VALID` ≠ x86 `!PRESENT` (no single present bit; swap and
present-invalid share the invalid encoding space with different bit layouts),
(b) dirty is a *derived* predicate (`PTE_DIRTY || (PTE_WRITE && !PTE_RDONLY)`,
pgt:439) because hardware DBM mutates the PTE behind the kernel's back, and
(c) exec-only pages break the naive "user bit" read. None of these affect the
protocol, whose state source is the metadata array, not the PTE — they affect
the M3 `corten_map()` PTE-programming path (§5).

## 3. Covering-page protocol on the arm64 hierarchy

### 3.1 Hierarchy / geometry table

All numbers derived from: `PTDESC_ORDER 3` (hwdef:10) → `PTDESC_TABLE_SHIFT =
PAGE_SHIFT - 3` (hwdef:13) → `ARM64_HW_PGTABLE_LEVEL_SHIFT(n) =
PTDESC_TABLE_SHIFT*(4-n) + 3` (hwdef:28) → shifts at hwdef:49-85; page size
choice `ARM64_{4K,16K,64K}_PAGES` (arm64/Kconfig:1428-1452);
`CONFIG_PAGE_SHIFT` 12/14/16 → `PAGE_SHIFT` (include/vdso/page.h:13);
`CONFIG_PGTABLE_LEVELS` defaults arm64/Kconfig:395-405; VA-bits choice with
default ARM64_VA_BITS_52 (arm64/Kconfig:1459+).

| | 4K pages | 16K pages | 64K pages |
|---|---|---|---|
| `PAGE_SHIFT` | 12 | 14 | 16 |
| `PTDESC_TABLE_SHIFT` (hwdef:13) | 9 | 11 | 13 |
| `PTRS_PER_PTE` (hwdef:49) | 512 | **2048** | **8192** |
| entries×8B = PT-page size | 4K | 16K | 64K |
| `PMD_SHIFT` / window (hwdef:56) | 21 / 2MB | 25 / 32MB | 29 / 512MB |
| `PTRS_PER_PMD` (hwdef:58) | 512 | 2048 | 8192 |
| `PUD_SHIFT` (hwdef:65) | 30 / 1GB | 36 / 64GB | folded (levels=3) |
| `PGTABLE_LEVELS` (Kconfig:395-405) | 3 (39-bit), 4 (48), 5 (52) | 3 (47-bit), 4 (48/52) | 2 (42-bit), 3 (48/52) |
| `PGDIR_SHIFT`, 48-bit VA | 39, `PTRS_PER_PGD`=512 (hwdef:81-85) | **47, PTRS_PER_PGD=2** | 42, PTRS_PER_PGD=64 |
| `PGDIR_SHIFT`, 52-bit VA ("pgdir shifted") | 48 (5 levels), PTRS_PER_PGD=16 | 47 (4 levels), PTRS_PER_PGD=32 | 42 (3 levels), PTRS_PER_PGD=1024 |
| `CONT_PTES` (Kconfig:317-321 → hwdef:91) | 16 | **128** | 32 |
| `CONT_PTE_SIZE` (hwdef:92) | 64KB | 2MB | 2MB |
| `CONT_PMDS` (Kconfig:323-327 → hwdef:96) | 16 | 32 | 32 |
| `CONT_PMD_SIZE` (hwdef:97) | 32MB | 1GB | 16GB |
| PTE-level covering window (= `PMD_SIZE`) | 2MB (same as x86) | **32MB** | **512MB** |

x86-64 comparison: 512 entries at every level, 2MB PMD window, PGD covers
512GB (4-level) — i.e. **only the 4K-page arm64 config is geometry-identical
to our current x86 assumptions**. `CORTEN_TXN_PATH_MAX 5`
(include/linux/corten.h:184) is sufficient for every config (max 5 levels);
the 2-level config (16K+36-bit) degenerates to "PTE pages hang directly off
the root", which the protocol handles (descent stops earlier) but which the
`enum corten_pt_level` window-mask table (`corten.c:449-464`) expresses only
if folded levels are remapped — see §7 and OQ5.

### 3.2 Leaf-at-PMD / leaf-at-PUD (block descriptors)

x86-64 core code refuses to build 1G leaves in most paths and THP is PMD-only;
our real view already treats both as unsupported (`corten_real_root()`
returns `-EOPNOTSUPP` on `pud_leaf`/`pmd_leaf`, mm/corten.c:904,910). arm64
differs in two ways:

1. **Leaf-at-PMD is the normal THP shape too** (`pmd_leaf()` pgt:821, THP via
   `PMD_TYPE_SECT`), so the existing check carries over verbatim — replace
   x86 `pmd_leaf()` with arm64 `pmd_leaf()`; no protocol change.
2. **`pmd_leaf_size()` can be `CONT_PMD_SIZE`** (pgt:824): a contiguous block
   of 16/32 PMD descriptors behaves as one giant leaf. For 2b (PTE-level
   tracking only) this is invisible — `pmd_leaf()` is still true and we bail
   — but an M4+ PMD-descriptor design must treat a CONT-PMD block as one
   protocol object (one lock for 16-32 descriptors), or refuse `pmd_cont()`
   in arena ranges. Recorded as risk R6, not designed here.

Deployment note: THP under the shadow-VMA interop (D1 opt-in arena) means the
arena *will* see `pmd_leaf()` on its paths; the protocol must keep returning
`-EOPNOTSUPP` and falling back, exactly as 2b does today, until M4+ decides
otherwise.

### 3.3 contpte: the deep-dive (most important finding)

The 6.18 contpte layer (`arch/arm64/mm/contpte.c`, wired via wrappers in
`arch/arm64/include/asm/pgtable.h:1679-1930`) transparently sets/clears
`PTE_CONT` (bit 52, hwdef:174) over blocks of `CONT_PTES` PTEs. Three facts
drive the whole arm64 port analysis:

**Fact 1 — fold/unfold rewrites the whole block.**
`contpte_convert()` (contpte.c:49-211), which serves *both* `__contpte_try_fold`
(contpte.c:213-275) and `__contpte_try_unfold` (contpte.c:277-290):

- aggregates AF/dirty by `__ptep_get_and_clear()`-ing **all `CONT_PTES`
  entries** of the block (contpte.c:61-69), leaving them transiently zero;
- flushes TLB for the block unless BBML2-noabort is present
  (`system_supports_bbml2_noabort()`, contpte.c:207-208);
- repaints all entries with `__set_ptes(..., CONT_PTES)` (contpte.c:210).

So a fold/unfold triggered by a *single-PTE* operation rewrites 16 (4K) / 128
(16K) / 32 (64K) neighbour PTEs of the same PT page. The neighbours are in the
same PT page (`CONT_PTE_SIZE < PMD_SIZE` in all configs, §3.1), so the *lock
granularity* of our protocol is fine — the covering write lock already spans
the whole block. The problem is different:

**Fact 2 — the contpte layer's own serialization assumption is the PTL, not
our descriptor lock.** The code says so twice: contpte.c:296-299 ("We are
guaranteed to be holding the PTL, so any contiguous range cannot be unfolded
or otherwise modified under our feet") and pgtable.h:1747-1753 ("All of these
APIs except for ptep_get_lockless() are expected to be called with the PTL
held", :1752). CortenMM's design *replaces* the PTL with the covering write lock
(paper Fig. 5; our `corten_txn_begin`, mm/corten.c:720-839). If an arena
transaction writes PTE k under the descriptor write lock only, while a
core-MM thread (reclaim rmap walk, GUP COW on the shadow-VMA side, THP split)
folds/unfolds the block holding the PTL, the two writers race on the same
PTEs *under different locks* — torn multi-PTE sequences and lost AF/dirty
aggregation. This is a genuine protocol-assumption conflict, not an
implementation detail.

**Fact 3 — fold/unfold triggers from the generic API our transaction would
naturally call.** `set_ptes()` with nr==1 → unfold-old, store,
`contpte_try_fold` (pgtable.h:1799-1814); `set_pte_at() =
set_ptes(..., 1)` (include/linux/pgtable.h:303). Fold fires whenever the
written PTE is the last of an aligned block, the pfn is block-aligned, the
entry is valid/noncont/nonspecial (pgtable.h:1705-1726), and the whole block
is covered by one folio with contiguous pfns and uniform prots
(contpte.c:216-273) — i.e. **any arena fault path populating the tail of a
large folio can trigger a 16-PTE rewrite + block TLBI inside what the paper
models as an atomic single-PTE map()**. Conversely `clear_full_ptes()` /
`wrprotect_ptes()` / `ptep_set_access_flags()` unfold on partial-block
operations (contpte.c:474-553, 672-675).

**Consequences and options** (decision needed, see OQ1):

- **Option A (recommended): arena transactions take the PTL in addition to
  the descriptor write lock whenever they touch hardware PTEs.** Lock order
  desc-write → PTL is acyclic: the uninstall interlock takes desc-write
  *without* the PTL (the free funnels run with PTLs already dropped — x86
  evidence mm/corten.c:396-401; arm64's `__pte_free_tlb` is reached from the
  same core funnels), and nothing takes PTL → desc-write. Cost: one
  spinlock acquire per fault; the contpte layer's assumptions then hold
  unchanged. This also future-proofs every other core-MM PTE primitive
  (`ptep_get` gathers AF/dirty across the block under PTL, contpte.c:292-338).
- Option B: forbid folding inside arena ranges (never map large folios into
  the arena, or mark arena PTEs so fold preconditions fail). Fragile:
  fold is triggered by core-MM paths we do not own, and "no large folios in
  the arena" gives up real performance.
- Option C: accept the race. **Rejected**: Fact 1 makes it a multi-PTE torn
  write, which our Atomic-Tree reasoning (paper §5) cannot absorb.

Metadata-side corollary: with FEAT_HAFDBS, AF/dirty of a folded block live on
*any* sub-PTE; a `corten_query()` that wants accessed/dirty ground truth must
use the gathered view (`ptep_get()` → `contpte_ptep_get`, pgt:1763-1771), not
a single-PTE read. Since our metadata array is authoritative (x86 design),
the arm64 issue is confined to the M3 sync-point where map/mark reconcile
metadata with PTE bits: use the gathered getters there.

## 4. Metadata array sizing

Design today: one 8-byte `struct corten_pte_meta` per virtual page
(include/linux/corten.h:156-165), `CORTEN_PTES_PER_PT_PAGE = 512` literal
(:138), `BUILD_BUG_ON(CORTEN_PTES_PER_PT_PAGE != PTRS_PER_PTE)` plus
`sizeof != 8` (mm/corten.c:314-315), array allocated on demand
(`corten_meta_ensure*`, GFP_NOWAIT|__GFP_NOWARN, mm/corten.h:52-61).

Per-PT-page array = `PTRS_PER_PTE × 8B`:

| Page size | Entries/PT page | Array bytes | Allocator | Full-population memory bound |
|---|---|---|---|---|
| 4K | 512 (hwdef:49) | 4KB (as x86 today) | order-0 / kmalloc-4k | 2× page-table memory (paper Fig. 22 ~2% of total) |
| 16K | 2048 (hwdef:13,49) | 16KB | kmalloc-16k, 1 allocation | same 2× ratio (8B per 8B entry) |
| 64K | 8192 | 64KB | kmalloc-64k, 1 allocation | same 2× ratio |

The per-entry 8B constant and the 1:1 array:PT-page ratio are invariant
across granules (both scale with `PAGE_SHIFT-3`), so the paper's ≤2%
full-population bound (paper.txt:1387-1389, Figure 22) carries over unchanged.

Required changes (all compile-time visible, none conceptual):

1. Replace the `512` literal with `PTRS_PER_PTE` directly or
   `(PAGE_SIZE / 8)` — the BUILD_BUG_ON (corten.c:314) then becomes the
   compile-time guard that catches every other 512-ism (full-array KUnit
   writeback test in corten_test.c iterates `CORTEN_PTES_PER_PT_PAGE`, so it
   self-adapts).
2. Allocation policy: `kmalloc(GFP_NOWAIT)` of 16KB/64KB on 16K/64K kernels
   still hits slab caches (KMALLOC_MAX on 64K-page systems is large); no
   order bump needed because the arrays are one `PAGE_SIZE` in size —
   but the alloc-failure path (`corten_alloc_should_fail` injection,
   `-ENOMEM` tolerated at mm/corten.c:317-326) must stay tolerant: bigger
   allocations fail more often under pressure. Keep GFP_NOWAIT; revisit if
   telemetry shows high deferral rates (debugfs `corten/stats`).

## 5. TLB shootdown: BBM vs x86 atomic PTE store

x86 model (current code): programming a mapping is a single atomic store
(`set_pte_at` on x86); stale-translation cleanup is INVLPG-style, orderable
after the store. The paper's protocol says map() atomically under the
covering write lock (paper Fig. 8 L22).

arm64 model, from the code:

- **Clearing is atomic**: `__ptep_get_and_clear()` = `xchg_relaxed(pte, 0)`
  (pgt:1378-1400); `__pte_clear()` = `__set_pte(0)` (pgt:1323-1326). The
  "break" half of break-before-make is a relaxed atomic exchange.
- **Permission tightening is cmpxchg-with-HW**: `___ptep_set_wrprotect()`
  loops on `cmpxchg_relaxed` to preserve concurrent hardware DBM updates
  (pgt:1449-1460); `__ptep_set_access_flags()` cmpxchgs only
  `PTE_RDONLY|PTE_AF|PTE_WRITE|PTE_DIRTY` and then `flush_tlb_page()` if
  dirty (arch/arm64/mm/fault.c:212-251); young-clearing is a cmpxchg loop
  (pgt:1329-1340). The hardware is a *concurrent PTE writer* (AF/DBM);
  blind stores would lose its updates.
- **Valid→valid rewrites must respect break-before-make**: unsafe attribute
  transitions are diagnosed under DEBUG_VM via `__check_safe_pte_update()` /
  `pgattr_change_is_safe()` (pgt:442-470); core-MM rewrites go through
  get_and_clear → flush → set, i.e. explicit BBM. `__set_pte()` completes
  with `queue_pte_barriers()` for kernel-valid entries (pgt:401-417).
- **Flush machinery**: `__flush_tlb_range_nosync()` = `dsb(ishst)` → per-page
  or range TLBI (`vale1is`/`vae1is`, user+kernel) → `mmu_notifier`
  (tlbflush.h:496-520); `__flush_tlb_range()` adds the completing
  `dsb(ish)` (tlbflush.h:522-530). `TLBI RANGE` instructions are used when
  `system_supports_tlb_range()`, with scale 3→0 decomposition and
  `MAX_TLBI_RANGE_PAGES = 2M pages` (tlbflush.h:437-476, 191-193);
  below-range HW falls back to per-stride TLBI; range-exceeding flushes
  collapse to `flush_tlb_mm()` (tlbflush.h:480-494; `MAX_DVM_OPS =
  PTRS_PER_PTE`, tlbflush.h:402). Batched shootdown exists:
  `arch_tlbbatch_add_pending()` issues TLBI per page, `arch_tlbbatch_flush()`
  only does the final DSB (tlbflush.h:380-397) — exactly the "issue early,
  acknowledge once" shape the paper borrows for its shootdown optimization
  (paper §4.5 "TLB shootdown"), so the M4 shootdown design ports without
  inventing anything.

Impact on the transaction model:

1. `corten_map()` on arm64 is **break-then-make**, not one store: if the
   target may hold a stale/valid entry, `__ptep_get_and_clear()` (atomic,
   safe), then the new `__set_ptes()`, with the TLBI deferred to the usual
   mmu_gather/batch machinery. The covering write lock makes the
   break→make window race-free *vs other transactions*; hardware AF/DBM
   updates in that window are handled by the same cmpxchg-aware primitives
   core-MM uses. The protocol's "map = single write" invariant survives as
   "map = single *atomic sequence* under the covering lock"; the paper's
   Atomic Spec is unaffected (it reasons about locks, not store counts).
2. Ordering inside a transaction must keep the arm64 rule that the flush
   (TLBI+DSB) for a cleared PTE precedes reuse of the VA — our unmap path
   (M3/M4) should push cleared pages into `mmu_gather` rather than flush
   inline, which also matches the existing uninstall ordering (staleness
   published under the desc write lock *before* `tlb_remove_ptdesc()`
   drops the page; arm64 hook point §6).
3. `pte_accessible(mm, pte)` exists in the arm64 flavor we must copy for the
   "may a stale TLB entry exist" test: pgt:229-230.

## 6. Lifecycle hook landing points (x86 double funnel → arm64)

x86 today (M2a, commit b8386e4e2467):

| Funnel | x86 location | Hook |
|---|---|---|
| alloc (user PTE page) | `arch/x86/mm/pgtable.c:17-31` (`pte_alloc_one`) | `corten_on_pte_alloc()` :28 |
| free, TLB-batched | `arch/x86/mm/pgtable.c:33-48` (`___pte_free_tlb`, reached via `__pte_free_tlb` wrapper `arch/x86/include/asm/pgalloc.h:56-59`) | `corten_on_pte_free()` :45 (before `tlb_remove_ptdesc`) |
| free, synchronous | `include/asm-generic/pgalloc.h:117-128` (`pte_free`) | `corten_on_pte_free()` :127 |
| (x86-only today) PT_RECLAIM reclaim funnel | `mm/Kconfig:1415-1421` depends `ARCH_SUPPORTS_PT_RECLAIM`, selected only by `arch/x86/Kconfig:331` | flows through `pte_free_tlb` → funnel 2 |

arm64 counterparts (verified in this tree; **r3 (2026-09-21): all landed, see
the r3 status block at the end of this section**):

1. **Alloc**: arm64 does *not* override `pte_alloc_one`; it inherits
   asm-generic (`arch/arm64/include/asm/pgalloc.h:18` includes
   asm-generic/pgalloc.h; `__pte_alloc_one_noprof` at pgalloc.h:75).
   Two placements: (a) add the `corten_on_pte_alloc()` call inside
   asm-generic `__pte_alloc_one_noprof()` (single place for every arch that
   inherits it; already gated by the static branch so cost is a nop), or
   (b) add an arm64 `pte_alloc_one()` override mirroring x86. Decision at
   implementation time (OQ4); either is ~10 LoC. Kernel PT pages
   (`pte_alloc_one_kernel()`) stay untracked, same as x86.
2. **Free, TLB-batched**: arm64's funnel is `__pte_free_tlb()` directly
   (`arch/arm64/include/asm/tlb.h:75-81`, calls `tlb_remove_ptdesc()` at
   :80) — there is no `___pte_free_tlb` indirection. Hook = one
   `corten_on_pte_free(pte)` line before `tlb_remove_ptdesc`, preserving the
   x86 ordering (staleness published under the descriptor write lock before
   the batched free completes; mm/corten.c:384-401). Why the batched funnel
   needs its own call: `tlb_remove_ptdesc()` → `tlb_remove_table()` →
   generic `__tlb_remove_table()` → `pagetable_dtor_free()`
   (asm-generic/tlb.h:217-222) — the batched path **never passes through**
   the hooked `pte_free()`, exactly like x86.
3. **Free, synchronous**: `pte_free()` in `include/asm-generic/pgalloc.h`
   **already contains the hook** (added in M2a) and arm64 inherits it —
   zero arm64-specific work. (`pte_free` users on arm64: fault-error,
   THP collapse/split, khugepaged deferred free — same call sites as x86.)
4. **Upper levels**: `__pmd_free_tlb`/`__pud_free_tlb` at asm/tlb.h:84-102
   are the M4+ landing points when PMD/PUD pages start carrying descriptors
   (`pud_free` additionally gated on `pgtable_l4_enabled()`,
   `arch/arm64/include/asm/pgalloc.h:59-64`). Not needed for the arm64 port
   of the current 2b feature set.
5. **PGD**: arm64 `pgd_alloc()` may come from a dedicated `pgd_cache`
   kmem_cache when `PGD_SIZE != PAGE_SIZE` (`arch/arm64/mm/pgd.c:27-40`) —
   a non-`struct page` allocation. Irrelevant while only PTE-level pages are
   tracked; a hard blocker only if PGD-level descriptors are ever proposed
   (risk R7).

Net: the arm64 hook delta is the `__pte_free_tlb` one-liner plus the alloc
decision — structurally identical double-funnel coverage to x86; a PT page
cannot die untracked.

**r3 status (2026-09-21, M9-P2 landed — evidence in §11.5)**: OQ4 was
resolved as option (a), with one non-obvious consequence found and handled.
The `corten_on_pte_alloc()` call now lives inside asm-generic
`__pte_alloc_one_noprof()` (pgalloc.h:94, comment :87-92) — one landing
spot for x86 (whose `pte_alloc_one()` wraps `__pte_alloc_one()`) and arm64
(inherits wholesale). Because `corten_ptdesc_install()` is *not*
re-entrant-safe on the same pfn — a second install replaces the descriptor
and fires `WARN_ON_ONCE` (mm/corten.c:356) — leaving x86's own funnel call
in place would have double-installed on every user PTE allocation, so the
M9-P2 patch deletes the x86-side call (`arch/x86/mm/pgtable.c:17-27`) as
the paired dedup. The arm64 side adds exactly the predicted one-liner:
`corten_on_pte_free(pte)` before `tlb_remove_ptdesc()` in
`arch/arm64/include/asm/tlb.h` (:93, plus the `linux/corten.h` include at
:12), with the free-hook ordering identical to x86's `___pte_free_tlb()`.
The synchronous `pte_free()` hook (:136) needed no change. Coverage after
M9-P2: every arm64 PT page birth/death funnels through a hook — alloc via
`pte_alloc_one*` → `__pte_alloc_one_noprof` (:94); free via either
`pte_free()` (:136, synchronous paths) or `__pte_free_tlb` (asm/tlb.h:93,
TLB-batched paths: `free_pgtables()`, zap).

## 7. 16K/64K base-page specifics

Aggregating §3.1/§4; what actually changes:

1. **Metadata array** (`CORTEN_PTES_PER_PT_PAGE`): 2048 (16K) / 8192 (64K)
   entries, 16KB / 64KB arrays (§4). Literal → `PTRS_PER_PTE`.
2. **Covering-window geometry**: the 2b scope check "range fits one PMD
   window" (`corten_covering_level() != CORTEN_LEVEL_PTE → -EOPNOTSUPP`,
   mm/corten.c:993-994) becomes 32MB / 512MB windows. The pure helpers
   (`corten_child_covers_range`, `corten_covering_level`,
   `corten_covering_va_base`, `corten_slot_index`, mm/corten.c:449-550) are
   written against x86 macros and need re-derivation:
   - `P4D_MASK`/`PUD_SHIFT`/`PTRS_PER_P4D` etc. **do not exist** on
     64K kernels (`#if CONFIG_PGTABLE_LEVELS > 3 / > 4` guards, hwdef:64-76)
     → the switch statements must be `#if`-guarded or table-driven from
     `CONFIG_PGTABLE_LEVELS` (2-level 16K+36 and 64K+42 configs degenerate
     further — OQ5);
   - arm64 folds levels at **runtime** (`pgtable_l4_enabled()`,
     pgt:980-991; `pgtable_l5_enabled()`; populate helpers check it,
     pgalloc.h:47-48,76-78), unlike x86's compile-time folding: a 4K+52-bit
     kernel has 5 configured levels where the hardware-at-boot may run 4.
     The geometry helpers must take the runtime fold state into account
     (the real view's `root` op already walks generically via
     `p4d_offset/pud_offset/pmd_offset`, so this is confined to the pure
     helpers and any window-shape debugging output).
   - the CORTEN_TXN_PATH_MAX=5 bound (corten.h:184) stays valid; the
     WARN_ON_ONCE structural check (mm/corten.c:755) likewise.
3. **Alignment constraints**: transaction ranges stay page-aligned
   (`corten_lock_range` checks, mm/corten.c:979-982). No arm64-specific
   extra alignment is needed for the *protocol*; CONT_PTE block alignment
   (§3.3) matters only to the PTE-programming path, and TLBI RANGE needs
   64KB-aligned starts only under LPA2 (handled inside
   `__flush_tlb_range_op`, tlbflush.h:448-450).
4. **contpte block size**: 128 sub-PTEs per block on 16K (contpte.c folds
   across `CONT_PTES`), so a fold rewrites 128 neighbours — Option A of
   §3.3 is *more* necessary on 16K kernels.
5. **MAX_DVM_OPS = PTRS_PER_PTE** (tlbflush.h:402) silently becomes 2048/8192
   on 16K/64K kernels, loosening the flush_tlb_mm collapse threshold —
   kernel-internal, noted only because it touches per-unmap flush cost
   estimates in M4.

## 8. Work breakdown and milestones

| Phase | Scope | Est. LoC | Risk | Exit criterion |
|---|---|---|---|---|
| P1 compile-level **(r2: core proven green, scope shrunk — status note below)** | Relax `CORTEN_MM` `depends on MMU && X86_64` → `(X86_64 \|\| ARM64)` (mm/Kconfig:1429-1432); kill the `512` literal (corten.h:138, corten.c:314); `#if`/table-drive the geometry helpers (corten.c:449-550) for folded levels; fix 64K-page breakage | 60-150 | low — compiler finds everything | `make ARCH=arm64 LLVM=1 defconfig+corten` builds; KUnit passes under qemu -M virt |
| P2 hook landing **(r3: landed 2026-09-21, see §6 r3 status + §11.5)** | asm-generic `pte_alloc_one` placement decision (OQ4) + `__pte_free_tlb` one-liner (asm/tlb.h:75-81); debugfs counters sanity on arm64 | 20-50 | low | boot with `corten=on`, ptdesc counter tracks PT pages across exec/exit stress |
| P3 semantic adaptation | Real view on arm64: `pud_leaf/pmd_leaf` (incl. `pmd_cont`) → `-EOPNOTSUPP`; runtime l4/l5 folding in geometry; **contpte interop decision (OQ1, Option A: PTL nesting)**; map/mark sync-point uses gathered `ptep_get` + BBM-safe sequence (§5) | 250-500 | **high** — correctness core | KUnit + arena smoke: fault-populate, mprotect, munmap, fork-COW on arm64; lockdep clean incl. desc→PTL order |
| P4 validation | arm64 KUnit suite run (synthetic-tree tests reused unchanged), bootlin aarch64 cross-build of android17 config + corten, qemu-system-aarch64 boot matrix (4K defconfig mandatory; 16K config as capability), document results | 100-300 (mostly test glue) | medium | M9 DoD: defconfig cross-compile green + boot smoke report |

**P1 r2 status (2026-09-16, evidence in §11)** — *empirically de-risked, scope
shrunk, not yet landed*:

- **Done / proven** (Round B, throwaway worktree, sed-released depends):
  `mm/corten.o` compiles on arm64 with **zero errors** at
  `CONFIG_PAGE_SHIFT=12` (115,304-byte object) — the compile-level workload
  the 60-150 LoC band predicted for the core **measured zero**; the only real
  failures are 2 x86 bit macros in the KUnit file (§11.2). The `depends`
  relaxation itself is *validated but not committed*: the tree still says
  `depends on MMU && X86_64` (mm/Kconfig:1432, both trees).
- **Remaining P1**: ① neutralize `mm/corten_test.c:1910/:1916`
  (`_PAGE_PSE|_PAGE_PRESENT`, test-only, ~5-15 LoC); ② land the Kconfig
  `depends` change per OQ5's r2 recommendation (~2-5 LoC). The `512`-literal
  replacement and folded-level `#if` guards are **not needed on 4K**
  (BUILD_BUG_ON passes at PTRS_PER_PTE=512); they are 16K/64K predictions and
  move with §7/R5 to a future 16K/64K compile pass — out of the 4K critical
  path.
- Net: P1 compile-level delta on the defconfig slice is **~10-25 LoC**, not
  60-150; the exit criterion "`defconfig+corten` builds" is half-met at
  object level, with the full `CONFIG_CORTEN_MM=y` Image + qemu KUnit run
  belonging to P4.

Comparison to paper §6.7 for the report table: our "RISC-V-equivalent" number
(P1+P2, the pure port) is ~100-200 LoC — consistent with the paper's 252
(paper.txt:1375, Table 5 row "RISC-V 252 vs Linux 699") once their port also
included wiring into their OS. P3 is the arm64-specific delta the paper
hand-waves as "more engineering efforts" (paper.txt:755); P4 has no
paper counterpart. The Linux-side comparison row (699 LoC for Linux→RISC-V)
should be quoted as the cost of porting Linux's *existing* two-abstraction MM,
not as our baseline.

## 9. Risk table

| # | Risk | Evidence | Severity | Mitigation |
|---|---|---|---|---|
| R1 | contpte fold/unfold rewrites whole block + block TLBI, serialized by PTL — conflicts with desc-write-lock-only PTE writes | contpte.c:49-211 (convert), :296-299 (PTL assumption), pgt:1747-1753, :1705-1726 (fold trigger), :1799-1814 (set_ptes) | **high** | §3.3 Option A (nest PTL under desc write lock; order is acyclic — uninstall holds desc lock w/o PTL, mm/corten.c:396-401); OQ1 |
| R2 | Hardware AF/DBM writes PTEs concurrently; dirty is derived (`PTE_DIRTY ‖ (WRITE ∧ !RDONLY)`); folded-block AF/dirty live on any sub-PTE | pgt:204,206,428-440; contpte.c:292-338 (gather) | medium | map/mark sync-point uses cmpxchg-aware primitives + gathered getters (§5) |
| R3 | `PTE_PRESENT_INVALID` reuses `PTE_NG` bit (bit 11) when invalid; metadata/PTE reconciliation must not interpret bit 11 | prot:26; pgt:209-210 | low | document bit-11 duality in the M3 sync-point code; never read NG from invalid PTEs |
| R4 | 5-level / 52-bit VA: PGD shrinks to 16 entries; **runtime** l4/l5 folding (arm64) vs x86 compile-time | hwdef:81-85; pgt:980-991; pgalloc.h:47-48,76-78; Kconfig:405 | medium | P3 geometry rework covers it; real view walks via generic p4d/pud offsets; add 52-bit config to P4 matrix if HW/qemu supports (OQ2) |
| R5 | 64K pages: PTRS_PER_PTE=8192 (64KB metadata arrays); PUD/P4D macros absent (3 levels); 2-level 42-bit config | hwdef:13,49,64-76; Kconfig:395-405 | medium (compile-visible) | §4 sizing + `#if`-guarded geometry; treat 2-level as out-of-scope unless build breaks say otherwise (OQ5) |
| R6 | CONT-PMD blocks (leaf-at-PMD with contiguous bit) = one logical leaf spanning 16-32 PMD entries | pgt:821-824 | low for 2b (we bail on `pmd_leaf`), high only for M4+ PMD descriptors | keep `-EOPNOTSUPP`; defer block-granularity protocol objects to M4+ design |
| R7 | arm64 PGD sometimes from kmem_cache (not a page) → PGD-level descriptors impossible on those configs | arch/arm64/mm/pgd.c:27-40 | low (out of scope) | never track PGD; note in M4+ design |
| R8 | ASID/NG semantics: arena shootdowns must be ASID-scoped (`vale1is`+ASID, user variant adds `USER_ASID_FLAG`); context switch gives free full-ASID flush | tlbflush.h:44-47, :50, :352-362 | low | use existing `__flush_tlb_range` helpers; never hand-roll TLBI |
| R9 | ARM relaxed memory model: protocol correctness currently argued with x86/TSO intuitions in comments; rwlock ACQUIRE/RELEASE suffices for locked paths, but lockless metadata reads (debugfs, future `_adv` RCU protocol) need explicit READ_ONCE/smp-load-acquire reasoning (paper itself defers to VRM-style verification) | paper.txt:1483-1487 | medium for M4+ `_adv`, low for `_rw` | keep all unlocked reads READ_ONCE + documented; KCSAN runs in M7 |
| R10 | No PT_RECLAIM on arm64 in 6.18 → one fewer free funnel than x86 | mm/Kconfig:1415-1421; arch/x86/Kconfig:331 | low (positive) | if arm64 gains PT_RECLAIM it funnels through `pte_free_tlb` → hook already in place |

## 10. Open questions

- **OQ1 (blocking P3) — 已决议（2026-09-22 STATE D22, 调研班 next/m9p3-decision-draft.md）**:
  contpte interop — take the PTL inside arena
  transactions (Option A) vs forbid folding in arena ranges (Option B).
  Needs a small benchmark (fault latency with/without PTL nesting) plus an
  upstream-reviewability judgment; leaning Option A (§3.3).
  **D22 终态 = ③′（Option B 的显式化）叠加 Option A 的验证性收尾**: ① OQ1 r1 描述的
  "desc 写锁 vs PTL 双锁写同一批 PTE"竞态在当前树已不以其原始形态存在——arena 全部
  12 个 PTE 触达点的 PTL 已嵌套在 desc 写锁内层（mm/corten_arena.c:4728/:3373 注释），
  全树无 PTL→desc 取序, 剩余工作 = lockdep 验证 + 文档钉死（Option A 验证尾）; ② fold
  永不进 arena 由巧合合力（folio 单覆盖前置条件 contpte.c:248-256 + arena 全 order-0 +
  VM_NOHUGEPAGE/空窗门）升格为显式契约: `__contpte_try_fold` 顶部 `mm->corten_mode`
  一行 mm 粒度拒绝 + `pte_valid_cont` 检测器×2-3 + debugfs 计数, ~130-250 行; ③ 方案②
  （CONFIG 禁用 contpte）否决——用全内核 TLB reach 换已被结构性消掉的竞态, 代价收益
  不成比例; ④ 顺带裁定: BBM `!vma` 臂裸 `set_pte_at`（corten_arena.c:651）= 独立切片
  S5 承载。实施排在 M-V 关键路径之后（避开 mm/corten_arena.c 与 A.2 增量的写冲突）。
- **OQ2**: BBML2-noabort availability in the test environment decides whether
  the fold path flushes (contpte.c:207-208). qemu 6.2 (installed) predates
  BBML2 emulation; expect every fold to flush in M9 tests — perf numbers for
  fold-heavy workloads will be pessimistic vs real HW.
- **OQ3**: does `corten_query()` expose gathered AF/dirty (block-granular on
  folded blocks) or keep metadata purely authoritative with the sync-point
  absorbing the difference? Touches M6 reclaim integration semantics.
- **OQ4**: alloc-hook placement — asm-generic `__pte_alloc_one_noprof()`
  (one hook for all inheriting arches) vs per-arch override; the former is
  cleaner for multi-arch but changes a shared header for an X86+ARM64-only
  feature.
  **r2 conclusion (§11.1)**: the asm-generic placement is compile-proven
  harmless on arm64 — `include/asm-generic/pgalloc.h` unconditionally
  includes `<linux/corten.h>` (:5) and already carries the free-side hook in
  `pte_free()` (:127); arm64 inherits that header unconditionally via
  `arch/arm64/include/asm/pgalloc.h:17` (zero arm64-side corten references),
  and the full Round A defconfig build came out 0E/0W through exactly this
  include chain. **Recommendation: take option (a)** — put
  `corten_on_pte_alloc()` inside asm-generic `__pte_alloc_one_noprof()`
  (pgalloc.h:75), symmetric with the shipped `pte_free` hook. The only
  residual concern is upstream reviewability of touching a shared header,
  not mechanics; on the compile-safety axis the question is closed.
  **r3 final (2026-09-21): RESOLVED, option (a) implemented** — hook landed
  at pgalloc.h:94; the placement forced one paired change the r2 text did
  not anticipate: x86's own `pte_alloc_one()` wraps `__pte_alloc_one()`, so
  its M2a-era funnel call had to be removed or every x86 user-PTE alloc
  would double-install (and trip the replace-descriptor `WARN_ON_ONCE`,
  mm/corten.c:356). Net x86-side diff is a pure deletion; see §6 r3 status
  and §11.5.
- **OQ5**: folded-level handling in `enum corten_pt_level` for 2/3-level
  configs (16K+36, 64K+42/48/52, 4K+39): compile-time `#if` remap vs runtime
  table; decide in P1 with the actual defconfig matrix (android17 arm64
  defaults to 4K pages, 48-bit VA? confirm CONFIG_ARM64_VA_BITS for the GKI
  config we cross-compile in M9).
  **r2 answer (§11)**: the crossed config is arm64 defconfig = 4K pages +
  **52-bit VA / 5 levels** (`CONFIG_ARM64_VA_BITS=52`, `CONFIG_PGTABLE_LEVELS=5`,
  `CONFIG_PAGE_SHIFT=12`; m9-baseline-config.log) — not the 48-bit guess
  above. The `depends on X86_64` gate (mm/Kconfig:1432) still blocks arm64 at
  config level: Round A shows `CONFIG_CORTEN_MM= <absent>`, and Round B could
  only proceed via a temporary sed. But Round B found **zero
  hierarchy-assumption errors** on this config, so the folded-level remap is
  **no longer urgent**: decouple it from P1 and revisit only when a 16K/64K
  compile matrix is actually run (§7, R5 — those granules remain
  compile-time-unverified predictions). Interim honest gating suggestion:
  `depends on MMU && (X86_64 || (ARM64 && ARM64_4K_PAGES))` — scopes the
  arm64 enablement to exactly what Round B proved, keeps the level-remap
  question out of the critical path.
- **OQ6**: qemu -M virt highmem/VA-bits settings for the 5-level smoke test
  (4K+52-bit requires HW LVA support in the model; optional for M9).

---

## 11. 实证结果（2026-09-15 交叉编译，r2 回填）

证据树: `/home/ppw/linux-6.18-m9`（分支 `m9-arm64`，HEAD `e911b31adb9c` =
M2a `b8386e4e2467` + M2b `2fd4070e745c` + M2c-fix1 `1284a235f751` 三提交 +
M3a.F1）。原始材料: `results/r02/m9-baseline-config.log`、
`m9-baseline-build.log`、`m9-trial-Kconfig.bak`、`m9-trial-config.log`、
`m9-trial-build.log`；汇总 `results/r02/infra-report.md`。工具链
`/home/ppw/tools/aarch64-toolchain/bin/aarch64-linux-`，`make -j6`。
两轮口径: **Round A = 基线（CORTEN_MM 被 depends 裁掉）全量 defconfig 构建；
Round B = throwaway worktree 内放开 depends 后只编 corten 两个对象**。

### 11.1 Round A — arm64 defconfig 基线: PASS

- **判定 PASS**: 2026-09-15 00:24:45 CST 产出 `arch/arm64/boot/Image` —
  42,031,616 B（≈40.1 MiB），sha256
  `24f8f716726556d9dfda886f2636cb3fd0b73a075fc4c7c8ac26dc361d8f6204`（r2 时在
  m9 worktree 复核一致），make 退出码 0，全日志 **0 错误 / 0 警告**
  （14,921 行，`results/r02/m9-baseline-build.log`）。构建跨中断续传完成
  （make 幂等零重编），累计 ≈**6h50m** wall。
- **配置口径**: arm64 defconfig + MGLRU/zram(lz4)。关键符号
  （m9-baseline-config.log 末段）: `CONFIG_ARM64_4K_PAGES=y` /
  `CONFIG_ARM64_VA_BITS=52` / `CONFIG_PGTABLE_LEVELS=5` /
  **`CONFIG_PAGE_SHIFT=12`** — 即 §3.1 表中**与 x86 几何完全一致的 4K 基**
  （`PTRS_PER_PTE=512`、4KB 元数据数组），但落在 5 级（52-bit VA）配置上；
  这同时回答了 OQ5 里 "confirm CONFIG_ARM64_VA_BITS" 的子问题（52，非 48）。
- **CORTEN_MM 被 depends 裁掉的取证**: 同 config 日志末行
  `CONFIG_CORTEN_MM= <absent>`；原因即 `mm/Kconfig:1432`
  `depends on MMU && X86_64`（两棵树该行号一致）在 arm64 上不满足，于是
  `mm/Makefile:156-157` 的 `obj-$(CONFIG_CORTEN_MM)` 两行不展开，
  corten.o / corten_test.o 根本不进构建 → Round A 同时证明: **基线构建的
  0E/0W 与 corten 代码无关**，corten 的 arm64 编译证据只能来自 Round B。
- **对 OQ4 的旁证（见 §10）**: 这是一次穿过 `arch/arm64/include/asm/pgalloc.h:17`
  → `include/asm-generic/pgalloc.h`（:5 无条件 `#include <linux/corten.h>`、
  :127 `pte_free()` 内已有 `corten_on_pte_free()` 钩子；arm64 侧 asm 头对
  corten 零引用）的全量构建，0E/0W ⇒ **corten.h + 共享头钩子模式在 arm64
  编译无害，实证成立**。

### 11.2 Round B — 放开 depends 后: 核心零错误，阻塞仅 test 文件 2 处

执行方式（如实记录，`results/r02/infra-report.md` §2）: 因 Round A 的 make
正在 m9 树上运行，就地改 Kconfig 会触发 syncconfig 污染，故用一次性
throwaway worktree `/home/ppw/linux-6.18-m9-trial`（分支
`m9-arm64-corten-trial` @ e911b31，零提交，事后已 `worktree remove --force`
+ 删分支，m9 树零扰动）。步骤: sed `mm/Kconfig:1432` `depends on MMU && X86_64`
→ `depends on MMU`（备份 `results/r02/m9-trial-Kconfig.bak`）；克隆 m9
.config 并追加 `CONFIG_CORTEN_MM=y` 后 `olddefconfig`；
`make -k ARCH=arm64 CROSS_COMPILE=… mm/corten.o mm/corten_test.o`。

**核心结论: `mm/corten.o` 在 arm64 零错误编译通过**（115,304 B 对象）——
协议核心（covering-PT-page 锁、事务 API、xarray PFN 索引、钩子调用点）在
`CONFIG_PAGE_SHIFT=12` 下**原样可编译**。§1 表中 "Protocol core 0 LoC" 与
"Data structures 0 LoC" 两行由设计论证升级为**编译实证**；§3.1 所述 "只有 4K
配置与 x86 几何同一" 的可编译面也一并验证。

失败归类表（两轮证据 `results/r02/m9-trial-build.log`，以
`=== ROUND2 08:12:39 KUNIT=y rerun ===` 分隔；第一轮 7 错 → 第二轮 2 错）:

| 类别 | 现象 | 位置 | 判定 |
|---|---|---|---|
| x86 头依赖（页表位宏） | `_PAGE_PSE` / `_PAGE_PRESENT` undeclared（第二轮仅存的 2 个错误） | `mm/corten_test.c:1910` `set_pmd(pmdp, __pmd(_PAGE_PSE \| _PAGE_PRESENT))`；同函数 `:1916` 同款 `set_pud` 用法（编译器每函数每标识符只报一次） | **真实阻塞，共 2 处，全部 test-only**: x86 `pgtable_types.h` 位宏，arm64 头文件无此符号（已 grep `arch/arm64/include/asm/pgtable*.h` 确认）。修复方向: 大页 leaf 构造改架构中立 helper（走 `pmd_mkinvalid`/prot 类 API）或该测试段按 `CONFIG_X86_64` 门控；约 5-15 LoC |
| 层级假设 | — | — | **0 个**。核心事务/锁/几何/视图代码在 4K+5 级配置上无任何 arch 层级假设泄漏 |
| 门控伪影（方法论记录，非移植失败） | 第一轮 5 个 implicit-declaration: `corten_test_inject_alloc_fail` :1154，`corten_test_render_dbg` :2024，`CORTEN_DBG_STATS/TXN/DUMP` :2024/:2034/:2043 | 这些声明在 `mm/corten.h:211-227` 的 `#ifdef CONFIG_CORTEN_MM_KUNIT_TEST` 内；该 Kconfig 符号 `depends on CORTEN_MM && KUNIT`、`default KUNIT_ALL_TESTS`，defconfig 无 KUNIT → 符号被裁；单目标 `make mm/corten_test.o` 绕过 `obj-$(…)` 门控强编译才触发 | **非真实错误**: 第二轮补 `CONFIG_KUNIT=y` + `CONFIG_CORTEN_MM_KUNIT_TEST=y` 后全部消失。**勿计入移植工作量**（P1 估算修正见 §11.3） |

### 11.3 对 §8 P1 工作量估算的修正

原文 P1 估 60-150 LoC（Kconfig 放开 + 512 字面量 + 几何 `#if` 化 + 64K 修复），
风险栏写 "compiler finds everything"。实证: **编译器几乎没找到东西**——
4K/52-bit defconfig 切片上核心 0 错误，全部真实阻塞集中在 test 文件的
2 个位宏。据此修正:

1. **协议核心跨架构可编译直接成立**。原 P1 的核心假设（"纯编译级工作"）
   被实证反转为好消息: 核心不需要任何编译级改动。§1 总估算 450-1000 LoC 中
   属于 P1 的 60-150 行在 4K 主线上收敛为 **~10-25 LoC**
   （test 位宏 5-15 + Kconfig depends 2-5）。
2. **P1 剩余项收窄**（与 §8 状态段一致）: ① `corten_test.c:1910/:1916`
   位宏中立化（test-only）；② `depends` 放开落地（语义按 OQ5 r2 建议，
   先 `ARM64 && ARM64_4K_PAGES` 试点或直接 `|| ARM64`）。512 字面量与折叠
   层级改写在 4K 配置下**无需**（`BUILD_BUG_ON` 在 `PTRS_PER_PTE=512` 下自然
   通过），随 §7/R5 移交 16K/64K 编译矩阵，不阻塞 4K 主线。
3. **风险表影响**: R5（64K/16K 几何）与 R4（5 级/52-bit）维持原判——Round B
   只覆盖了 5 级配置的**编译**面，运行时折叠（`pgtable_l4/l5_enabled`）与
   16K/64K 粒度仍是未验证预测；P3 的 contpte/BBM 语义工作（R1/R2）完全未被
   本轮触及，仍是 450-1000 估算的主体与最大风险。

### 11.4 M9 剩余工作排序（r2 视角）

1. **P1 收尾（最小改动量大）**: test 位宏中立化 + Kconfig depends 落地
   （含 OQ4 的 asm-generic alloc 钩子落点——其编译风险已被 Round A 实证排除）→
   在 m9 树重跑 `mm/corten.o`/`mm/corten_test.o`（KUNIT=y）双绿即闭环。
2. **P4 前半（M9 gate 主线）**: `CONFIG_CORTEN_MM=y` 全量 arm64 Image +
   `qemu-system-aarch64 -M virt` 启动 smoke（Round A 已给基线 Image 与
   构建管线，增量成本≈一次增量构建）。
3. **P2 钩子**: `__pte_free_tlb` 一行 + alloc 落点（OQ4 已定推荐项），
   debugfs 计数器 sanity。**r3 (2026-09-21): 已落地并验证** —— alloc 钩子按
   OQ4 定案放进 asm-generic `__pte_alloc_one_noprof()`（连带删除 x86 侧重复
   调用，见 §6 r3 状态段），arm64 `__pte_free_tlb` 加 `corton_on_pte_free`
   一行；arm64 双对象 0E/0W + CORTEN_MM=y Image + qemu KUnit 验证见 §11.5。
   （遗留: debugfs 计数器 sanity 需 corten=on 用户态冒烟，归 P4；
   arm64 TCG 下 interlock 测试的时钟脆弱性记录见 §11.5。）
4. **P3 语义适配（最大风险主体，未被本轮降低）**: contpte PTL 嵌套（OQ1）、
   BBM 映射序（§5）、gathered `ptep_get` 同步点；本轮唯一相关输入是
   "0 个层级假设错误" 免除了几何重写的编译层恐慌。
5. **16K/64K 编译矩阵**: §7/R5/OQ5 的遗留验证面，按需排期。

### 11.5 M9-P2 钩子落地实证（2026-09-21, r3）

树: `/home/ppw/linux-6.18-m9`（`m9-arm64`，rebase 至 `2639d3294b9d` =
M3b 全量 + M4.T0 + M5 + M6 + M9-P1 `025756094542`；本班为该 M6 基座上
arm64 首次构建/运行验证）。补丁: `patches/r07-m9p2.diff`
（3 文件 +29/-13, checkpatch --strict 0E/0W/0C）: asm-generic
`__pte_alloc_one_noprof` alloc 钩子（OQ4-(a)）+ x86 `pte_alloc_one`
重复调用删除（配对改动，§6 r3）+ arm64 `__pte_free_tlb` 一行钩子 +
`linux/corten.h` include。日志: `results/r07/m9p2-*.log`。

| Gate | 结果 |
|---|---|
| arm64 olddefconfig（CORTEN_MM=y 门控入树） | PASS（4K/52-bit/5 级，ARENA 正确缺席） |
| `mm/corten.o` + `mm/corten_test.o` | 0E/0W，RC=0 |
| `Image`（CORTEN_MM=y，增量） | PASS 0E/0W; 42,232,320 B，sha256 `1aa6b167…946562`（较 Round A 基线 +200,704 B，与 M9-P1 Image 同字节数） |
| qemu `-M virt -cpu max -smp 4` KUnit corten* | 7 boot: **4× 25/0/0 全绿** + 3× 24/1（interlock TCG 时钟伪影，见下） |
| x86 回归（defconfig+MEMCG，KVM, -smp 4） | **corten 25/0/0 全绿**（含 interlock ok 19）；构建 0E，仅 2 条登记在案的基线警告 |

**interlock 的 TCG 时钟伪影（诚实记录）**: `corten_test_txn_uninstall_interlock`
在 arm64 TCG 下 7 boot 命中 3 次 24/1，两种签名均为 wall-clock 脚手架被
guest 时钟异常打破而非协议破坏: (a) runtime 20.25s=worker A 的 20s deadline
先到期正常放锁（a_err=1, a_locked=0），主线程 200ms 检查窗被拉长后读到
`b_done==1`——uninstall 是在 A 合法放锁**之后**完成的; (b) guest ktime 跳变
使 A 的 deadline 起跑即过期、毫秒级穿场（`phase=5 begin_ret=0`，锁真实
拿到并走完全生命周期）。归属: 卸装/持锁路径与该测试在
e911b31..2639d3294b9d 间字节不变（git diff 核实）; 本班钩子 static-branch
关闭且不在测试路径; x86 KVM 同基座绿; **且该 flake 是 x86 侧 M7 既有登记项**
（run/inflight.txt: "宿主过载 interlock flake 2 例=M7 登记"、"interlock
3/6 vs 基线 1/4"——本班 arm64 3/7 与登记率同量级，属同一已知项跨架构再现）。
→ 判定为 interlock 测试对宿主负载/时钟的固有脆弱性（M7 登记延续）;
遗留加固建议（go-信号替代 deadline 轮询，或 TCG 多数决+KVM 复核判定口径）
移交测试 owner; M9-P1 单次 25/0/0 未采样到此敏感性。

**基座附带发现（非本补丁范围）**: `x86_64 defconfig`（MEMCG/SHRINKER_DEBUG=n）
下 `mm/corten_arena.c` 两处编译失败（:3323 `shrinker->id`、:8530
`mem_cgroup_is_descendant` 隐式声明）——M6.T3/T4 的 memcg-shrinker 代码
缺 Kconfig 依赖守护，历史 gki 系配置（MEMCG=y）从未暴露; 留 maintainer
评估补 `depends on`/`#ifdef`。

**P2 判定: PASS**（debugfs corten=on 计数器 sanity 一项按计划移交 P4 用户态
冒烟）。P3（contpte/BBM 语义适配）未被本班触及，仍是最大风险主体。

---

### Verification appendix: every constant re-derived (self-check)

- `PTRS_PER_PTE = 2^(PAGE_SHIFT-3)`: 4K→512, 16K→2048, 64K→8192
  (hwdef:10,13,49). The task prompt's guessed "512/256/32" is wrong — the PT
  page is always exactly one base page of 8-byte entries.
- `PMD window`: `2*TTS+3` = 21/25/29 → 2MB/32MB/512MB (hwdef:28,56).
- `PUD window`: `3*TTS+3` = 30/36 → 1GB/64GB; absent on 64K (Kconfig:395-405
  levels=3; hwdef guard :64).
- `PGDIR`: `shift(4-LEVELS)`; entries `1<<(VA_BITS-PGDIR_SHIFT)` (hwdef:81-85):
  4K/48→512, 4K/52→16@48, 16K/48→2@47, 16K/52→32@47, 64K/48→64@42, 64K/52→1024@42.
- `CONT_PTES = 2^CONFIG_ARM64_CONT_PTE_SHIFT` (Kconfig:317-321 defaults
  4/7/5; hwdef:90-93): 16/128/32; `CONT_PTE_SIZE` 64KB/2MB/2MB; all `< PMD_SIZE`.
- `CONT_PMDS = 2^CONFIG_ARM64_CONT_PMD_SHIFT` (Kconfig:323-327 defaults
  4/5/5; hwdef:95-98): 16/32/32; `CONT_PMD_SIZE` 32MB/1GB/16GB.
- Metadata array per PT page = `PTRS_PER_PTE × 8B` = 4KB/16KB/64KB — equal to
  the PT page size in every granule (ratio invariant).
- x86 numbers quoted for comparison: hooks at
  `arch/x86/mm/pgtable.c:17-48`, `include/asm-generic/pgalloc.h:117-128`,
  `mm/Kconfig:1429-1455`; geometry/views at `mm/corten.c:449-550,878-967`;
  protocol core `mm/corten.c:552-865`; current LoC 1,383+228+494+2,047.
- Paper: assumptions paper.txt:641-652; ARM paragraph :748-753; Table 5
  :1373-1377 (RISC-V 252 / Linux 699; MPK 82/273; TDX 368/471); implementation
  size :665-667; <200 LoC claim :494-499; relaxed-memory note :1483-1487.
