# CortenMM → ARM64: Porting Design Notes (M9)

Status: design document (M9 deliverable). No code in this document has been
cross-compiled yet; that is the second half of M9 (bootlin aarch64 toolchain +
`qemu-system-aarch64` smoke, run separately). All line references are against
this tree (`/home/ppw/linux-6.18`, android17-6.18 @ 68974e235117 + CortenMM
M2a/M2b/M2c-fix1: b8386e4e2467, 2fd4070e745c, 1284a235f751) and against the
paper text (`/home/ppw/paper/corte/paper.txt`, CortenMM SOSP'25).

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

arm64 counterparts (verified in this tree):

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
   the batched free completes; mm/corten.c:384-401).
3. **Free, synchronous**: `pte_free()` in `include/asm-generic/pgalloc.h:117-128`
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
| P1 compile-level | Relax `CORTEN_MM` `depends on MMU && X86_64` → `(X86_64 \|\| ARM64)` (mm/Kconfig:1429-1432); kill the `512` literal (corten.h:138, corten.c:314); `#if`/table-drive the geometry helpers (corten.c:449-550) for folded levels; fix 64K-page breakage | 60-150 | low — compiler finds everything | `make ARCH=arm64 LLVM=1 defconfig+corten` builds; KUnit passes under qemu -M virt |
| P2 hook landing | asm-generic `pte_alloc_one` placement decision (OQ4) + `__pte_free_tlb` one-liner (asm/tlb.h:75-81); debugfs counters sanity on arm64 | 20-50 | low | boot with `corten=on`, ptdesc counter tracks PT pages across exec/exit stress |
| P3 semantic adaptation | Real view on arm64: `pud_leaf/pmd_leaf` (incl. `pmd_cont`) → `-EOPNOTSUPP`; runtime l4/l5 folding in geometry; **contpte interop decision (OQ1, Option A: PTL nesting)**; map/mark sync-point uses gathered `ptep_get` + BBM-safe sequence (§5) | 250-500 | **high** — correctness core | KUnit + arena smoke: fault-populate, mprotect, munmap, fork-COW on arm64; lockdep clean incl. desc→PTL order |
| P4 validation | arm64 KUnit suite run (synthetic-tree tests reused unchanged), bootlin aarch64 cross-build of android17 config + corten, qemu-system-aarch64 boot matrix (4K defconfig mandatory; 16K config as capability), document results | 100-300 (mostly test glue) | medium | M9 DoD: defconfig cross-compile green + boot smoke report |

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

- **OQ1 (blocking P3)**: contpte interop — take the PTL inside arena
  transactions (Option A) vs forbid folding in arena ranges (Option B).
  Needs a small benchmark (fault latency with/without PTL nesting) plus an
  upstream-reviewability judgment; leaning Option A (§3.3).
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
- **OQ5**: folded-level handling in `enum corten_pt_level` for 2/3-level
  configs (16K+36, 64K+42/48/52, 4K+39): compile-time `#if` remap vs runtime
  table; decide in P1 with the actual defconfig matrix (android17 arm64
  defaults to 4K pages, 48-bit VA? confirm CONFIG_ARM64_VA_BITS for the GKI
  config we cross-compile in M9).
- **OQ6**: qemu -M virt highmem/VA-bits settings for the 5-level smoke test
  (4K+52-bit requires HW LVA support in the model; optional for M9).

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
