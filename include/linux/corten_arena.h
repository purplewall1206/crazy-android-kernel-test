/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CortenMM arena layer - opt-in per-process address-range registration.
 *
 * An "arena" (paper Sec. 3.4, M3B_DESIGN.md sec 2) is a range of the
 * address space that a process declares with prctl(PR_CORTEN_ARENA) so
 * that page faults inside it can be served by CortenMM transactions
 * (mm/corten.h) instead of the VMA machinery.  Declaring requires an
 * existing MAP_NORESERVE private anonymous VMA that exactly matches the
 * declared range; the VMA is converted in place into a *shadow-VMA*
 * (VM_CORTEN|VM_NOHUGEPAGE, sec 3) so that every other kernel subsystem
 * keeps working on it unchanged, while the arena layer resolves the range
 * through a per-mm xarray keyed by 2M frame index without ever touching
 * the VMA tree.
 *
 * Lock order (DESIGN.md sec 7 INV2 as amended by DEV-13, outermost
 * first):
 *
 *	mmap_lock (W or R) -> ctl_lock (per-mm state mutex) -> vma write
 *	marks -> percpu_ref(active) [drain barrier] -> fill_lock ->
 *	desc->lock (write, BH-symmetric) -> PTE lock.
 *
 * DECLARE/RELEASE/fork-demote acquire mmap_lock(W) first and nest
 * ctl_lock inside it; the do_mmap auto-attach runs with the write lock
 * already held and takes only ctl_lock.  Waiting for a drain under both
 * locks stays deadlock-free: a transaction (fault path, arch/x86/mm/
 * fault.c:1344 fast hook, mm/memory.c:6558 slow gate) pins the arena
 * with percpu_ref tryget/put and never takes mmap_lock for writing nor
 * ctl_lock, and the percpu_ref release callback runs from RCU softirq
 * context and only does complete().  The only waiters a drain can block
 * behind its held mmap_write are legacy space operations, for the
 * remaining lifetime of the in-flight transactions (microseconds).
 * corten_arena_mm_exit() drains with mm_users already 0 and takes only
 * the lower ctl_lock, which cannot form a cycle by itself.
 */
#ifndef _LINUX_CORTEN_ARENA_H
#define _LINUX_CORTEN_ARENA_H

#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/corten.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/percpu-refcount.h>
#include <linux/refcount.h>
#include <linux/seq_file.h>
#include <linux/types.h>
#include <linux/xarray.h>

struct file;
struct mm_struct;
struct pt_regs;
struct seq_file;
struct vm_area_struct;

/*
 * prctl interface (include/uapi/linux/prctl.h):
 *
 *   prctl(PR_CORTEN_ARENA, op, addr, len, 0)
 *
 *     op = CORTEN_ARENA_DECLARE: [addr, addr+len) must exactly equal one
 *          existing MAP_NORESERVE private anonymous VMA and be 2M aligned.
 *     op = CORTEN_ARENA_RELEASE: [addr, addr+len) must exactly equal a
 *          previously declared arena; the range is drained and unmapped.
 *     op = CORTEN_ARENA_QUERY:   returns 1 if @addr is inside a declared
 *          arena, 0 if not, -ENOENT if this mm has no arenas at all.
 *     arg5 must be 0; the whole operation needs CAP_SYS_ADMIN and corten=on
 *     (otherwise -EOPNOTSUPP, i.e. the behaviour of a kernel built without
 *     CONFIG_CORTEN_MM).
 */

/* Per-mm statistics (relaxed counters, debugfs reads them later on). */
enum corten_arena_stat {
	CORTEN_ARENA_STAT_DECLARES = 0,
	CORTEN_ARENA_STAT_RELEASES,
	/* S4/S5 fault-path counters (M3B_DESIGN.md sec 7.4). */
	CORTEN_ARENA_STAT_FAULTS,	/* faults that entered an arena txn */
	CORTEN_ARENA_STAT_MAPPED,	/* anon pages mapped by arena faults */
	CORTEN_ARENA_STAT_ZERO_PAGES,	/* shared-zero-page read installs */
	CORTEN_ARENA_STAT_RESTORES,	/* CORTEN_MAPPED PTE rebuilds */
	CORTEN_ARENA_STAT_ACCERR,	/* permission SIGSEGVs */
	CORTEN_ARENA_STAT_MAPERR,	/* undeclared-address SIGSEGVs */
	CORTEN_ARENA_STAT_FILLS,	/* fill_upper() that allocated */
	CORTEN_ARENA_STAT_FALLBACKS,	/* fallbacks to the legacy path */
	/* S6 space-operation counters. */
	CORTEN_ARENA_STAT_UNMAP_PAGES,	/* pages zapped by arena munmap */
	CORTEN_ARENA_STAT_MUNMAP_TXNS,
	CORTEN_ARENA_STAT_MMAP_MARK_TXNS,
	/* Lifecycle health: a non-zero value means an active percpu_ref
	 * leaked (pairing bug) -- the descriptor was deliberately not
	 * freed to keep the process/exit path moving (r03 DoD failure B).
	 */
	CORTEN_ARENA_STAT_DRAIN_TIMEOUTS,
	/* M5 COW write-fault branches (M5_FORK_SPEC.md sec 3.2): the
	 * map_count==1 reuse (SHARED cleared in place) and the private
	 * copy.  The T2 对拍表 reads both; reuse > 0 is what proves the
	 * no-copy branch is reachable after a fork+exit.
	 */
	CORTEN_ARENA_STAT_COW_REUSE,
	CORTEN_ARENA_STAT_COW_COPY,
	/* M5.T3 removed the slow-gate forced-write shapes (M5.T2'
	 * CORTEN_ARENA_STAT_FORCE_WRITES): their read-only survivor was
	 * only re-followable by FOLL_FORCE callers and livelocked plain
	 * GUP write pins (results/r07/m5t3-verify.md).
	 */
	CORTEN_ARENA_NR_STATS,
};

/*
 * Region record classes (MV_VMA_FREE_SPEC.md sec 2.1): the mmap-unit
 * semantic carrier the VMA used to provide.  A region is embedded 1:1 in
 * its arena descriptor (region bounds == arena bounds, DEV-12's per-mmap
 * auto-arena shape); the page-level state stays in the per-PTE metadata
 * (the sole source of truth), the region record only carries the
 * mapping-level semantics that do not fit in the metadata.
 *
 *	CORTEN_REGION_ANON	MODE auto-mmap private anonymous (V-A
 *				coverage; the only producer today);
 *	CORTEN_REGION_FILE	private file mapping of a MODE process
 *				(V-B): enum and fields in place, no
 *				producer yet;
 *	CORTEN_REGION_RESERVED	special region -- the park-pool reservation
 *				window (content zapped, no live mapping
 *				semantics) and the conceptual home of the
 *				magazine reserve sentinels.  Delegated-domain
 *				exec/stack/vdso VMAs are NOT regions.
 */
enum corten_region_class {
	CORTEN_REGION_ANON = 0,
	CORTEN_REGION_FILE,
	CORTEN_REGION_RESERVED,
};

/*
 * CORTEN_RF_*: recorded leftover VMA-flag semantics (MV_VMA_FREE_SPEC.md
 * sec 2.6, the INV-MV3 closed encoding table).  The T0/M4T0 flag whitelist
 * rejects all of these on the mapping paths today except VM_SOFTDIRTY
 * (transient dirty-tracking bookkeeping, tolerated); the bits exist so
 * that consumers can ask "what did the mapping flags say" once the VMA is
 * gone, and so that a future whitelist widening only grows producers, not
 * the encoding.  A new VM_* bit must be given a row in that table
 * (RF/PERM/REJECT/DELEG) -- review checklist item.
 */
#define CORTEN_RF_SOFTDIRTY	_BITUL(0)	/* VM_SOFTDIRTY was set */
#define CORTEN_RF_DONTCOPY	_BITUL(1)	/* VM_DONTCOPY (fork reservation) */
#define CORTEN_RF_WIPEONFORK	_BITUL(2)	/* VM_WIPEONFORK (fork reservation) */
#define CORTEN_RF_SEQ_READ	_BITUL(3)	/* VM_SEQ_READ access hint */
#define CORTEN_RF_RAND_READ	_BITUL(4)	/* VM_RAND_READ access hint */
#define CORTEN_RF_ALL							\
	(CORTEN_RF_SOFTDIRTY | CORTEN_RF_DONTCOPY | CORTEN_RF_WIPEONFORK | \
	 CORTEN_RF_SEQ_READ | CORTEN_RF_RAND_READ)

/*
 * pagemap entry bits and the PSS fixed-point shift, shared between the
 * window fill (mm/corten_arena.c) and the /proc reader
 * (fs/proc/task_mmu.c).  Numerically identical to that file's
 * PM_xxx and PSS_SHIFT defines (stable pagemap ABI bits); task_mmu.c
 * compile-time-pairs them so the two spellings cannot drift.
 */
#define CORTEN_PM_PFRAME_BITS		55
#define CORTEN_PM_PFRAME_MASK		GENMASK_ULL(CORTEN_PM_PFRAME_BITS - 1, 0)
#define CORTEN_PM_MMAP_EXCLUSIVE	BIT_ULL(56)
#define CORTEN_PM_FILE			BIT_ULL(61)
#define CORTEN_PM_SWAP			BIT_ULL(62)
#define CORTEN_PM_PRESENT		BIT_ULL(63)
#define CORTEN_PSS_SHIFT		12

/*
 * The label ANON window rows render under -- the exact byte string the
 * shadow-VMA era printed through its anon_vma_name ("[anon:%s]" of
 * mm/corten_arena.c's CORTEN_ARENA_VMA_NAME), kept verbatim so the J3
 * byte-diff oracle cannot see the source switch.
 */
#define CORTEN_REGION_ROW_LABEL		"[anon:corten_arena]"

/**
 * struct corten_arena - descriptor of one declared arena.
 * @start: first VA of the arena (PMD_SIZE aligned).
 * @end: first VA past the arena (PMD_SIZE aligned).
 * @prot: CORTEN_PERM_* upper bound recorded from the VMA at DECLARE time.
 * @mm: owning address space (diagnostics back-link; never taken by
 *      reference -- the arena cannot outlive its mm, see
 *      corten_arena_mm_exit()).
 * @active: transaction liveness counter.  The fault path takes it with
 *          percpu_ref_tryget_live() for the duration of one transaction;
 *          RELEASE/exit wait for it to reach zero before touching the
 *          shadow-VMA or the page tables (M3B_DESIGN.md sec 6.3).  Its
 *          release callback completes @drained (6.x has no
 *          percpu_ref_wait_for_zero).
 * @drained: signalled by the @active release callback at zero count.
 * @fill_lock: serializes upper-page-table ensure-alloc for this arena
 *             (used by the S5 fault path; declared here because it must
 *             not nest inside any descriptor lock).
 * @vma: the arena's shadow-VMA, cached at DECLARE under mmap_write_lock
 *       (right after the in-place conversion) and cleared again under
 *       mmap_write_lock in RELEASE after the drain.  The fault path reads
 *       it without touching the VMA tree: while any transaction holds an
 *       @active reference, RELEASE is parked in its drain (sec 6.3) and
 *       cannot reach the mmap_write_lock that removes the VMA, so the
 *       pointer is stable for as long as the reference is held.  This is
 *       what keeps the hot path at zero maple-tree walks (M3 DoD).
 * @rcu: kfree_rcu() deferral so that an RCU-protected lookup can still
 *       read @start/@end while a concurrent RELEASE unregisters.
 * @frozen: fork freeze window (M5_FORK_SPEC.md sec 1.3, DEV-15).  Set
 *          under the owner mm's mmap_write + the registry ctl_lock by
 *          corten_arena_fork_begin() together with the transaction-drain
 *          kill; cleared by the single unfreeze closure before that lock
 *          pair is released.  Read locklessly by the fault-path lookup:
 *          a frozen arena refuses new transactions (lookup_get returns
 *          NULL), so the dup_mmap() write lock -- which every legacy
 *          fallback needs -- is what makes the window a static snapshot.
 * @idle: T1c resident-pool state (M4T12_T1C: park instead of RELEASE).
 *          Set under the owner mm's mmap_write + ctl_lock when a
 *          full-coverage munmap of a MODE-process arena parks the arena
 *          in the per-mm pool instead of tearing it down: the content is
 *          zapped and the metadata reset (a pristine, empty window), the
 *          shadow-VMA loses VM_CORTEN and R/W/X (a reserved PROT_NONE
 *          anonymous mapping), but the descriptor, its live percpu_ref
 *          and the warm xarray slots stay.  Read locklessly by
 *          corten_arena_lookup(): a parked arena is lookup-invisible, so
 *          the munmapped range keeps legacy semantics (no arena, access
 *          faults).  Cleared by the reactivation that hands the window
 *          to the next mmap.
 * @pool: the per-mm pool's LRU node (valid only while @idle is set;
 *          INIT_LIST_HEAD()d at DECLARE so the membership test is
 *          exactly the @idle flag).
 * @rclass: region record class (MV_VMA_FREE_SPEC.md sec 2, V-A.0):
 *          %CORTEN_REGION_ANON while live, %CORTEN_REGION_RESERVED while
 *          parked (@idle set).  %CORTEN_REGION_FILE is the V-B.1
 *          producer's class (private file mappings of a MODE process).
 *          Written under the owner mm's mmap_lock for writing (the same
 *          writers that flip @idle/@prot); read locklessly.
 * @may_prot: CORTEN_PERM_* upper bound of the region's mprotect upgrade
 *          space (MAY semantics: always a superset of @prot).  Closes the
 *          M4T0 gap where the mprotect route guessed the bound from
 *          @prot; no reader consumes it yet (V-A.3 routes it into
 *          protect_range).
 * @rflags: CORTEN_RF_* recording of the mapping's surviving VMA-flag
 *          semantics at attach time (sec 2.6 encoding table).
 * @rfile: FILE-class backing file, held by reference (get_file, taken
 *          by corten_region_register_file()); NULL for every other
 *          class.  Dropped by corten_region_file_teardown().
 * @rpoff: FILE-class mapping start page offset (0 for other classes);
 *          page pgoff = rpoff + (addr - start) / PAGE_SIZE.
 * @npieces: punch-shape marker: <= 1 single-piece (the region itself),
 *          > 1 punched multi-piece.  The pieces table has no producer in
 *          V-A.0 (the punch route still owns its VMA surgery).
 * @rpieces: {start,end} piece list; empty while @npieces <= 1 (which it
 *          always is in V-A.0).  Re-initialised at every region
 *          registration.
 * @carrier: detached, not-in-tree VMA that will host rmap/PTE-API
 *          semantics for the region (sec 2.3).  Field only in V-A.0 --
 *          no producer, stays NULL; the semantics land in V-A.2b.
 */
struct corten_arena {
	unsigned long		start;
	unsigned long		end;
	u8			prot;
	struct mm_struct	*mm;
	struct percpu_ref	active;
	struct completion	drained;
	bool			frozen;
	bool			idle;
	struct list_head	pool;
	/* Upper-page-table ensure-alloc serialization (never nests inside
	 * a descriptor lock).
	 */
	struct mutex		fill_lock;
	/* The arena's shadow-VMA, cached at DECLARE under mmap_write_lock
	 * and cleared under it in RELEASE after the drain.  The fault path
	 * reads it without touching the VMA tree: while any transaction
	 * holds an @active reference, RELEASE is parked in its drain
	 * (sec 6.3), so the pointer is stable for as long as the reference
	 * is held -- zero maple-tree walks on the hot path (M3 DoD).
	 */
	struct vm_area_struct	*vma;
	/* S8 observability ledger node (debugfs arenas).  Linked at the
	 * end of a successful DECLARE and unlinked at deregistration,
	 * both under the global corten_arena_list_lock; readers walk it
	 * under RCU (the descriptor itself is freed by kfree_rcu()).
	 * Purely observational -- the per-mm xarray remains the only
	 * lookup structure and the fault path never touches this list.
	 */
	struct list_head	obs;

	/* Region record (MV_VMA_FREE_SPEC.md sec 2.2, V-A.0): the
	 * mmap-unit semantics the VMA used to carry, embedded 1:1.  All
	 * writers run under the owner mm's mmap_lock for writing next to
	 * the existing @prot/@idle write points (sec 2.7: no new lock --
	 * mmap_lock is the region record's lock).
	 */
	enum corten_region_class rclass;  /* ANON / FILE / RESERVED */
	u8			may_prot; /* CORTEN_PERM_* MAY bound */
	u32			rflags;	  /* CORTEN_RF_* (sec 2.6) */
	struct file		*rfile;	  /* FILE class: refcounted */
	loff_t			rpoff;	  /* FILE class: start page offset */
	unsigned int		npieces;  /* >1 = punched multi-piece */
	struct list_head	rpieces;  /* piece list; empty if <=1 */
	struct vm_area_struct	*carrier; /* sec 2.3 detached VMA (V-A.2b) */

	struct rcu_head		rcu;
};

/*
 * MODE-process auto-arena window (DESIGN.md sec 2, M4T0_SPEC.md sec 1.3):
 * a fixed span inside x86_64 TASK_SIZE (128T), disjoint from the legacy
 * mmap_base / brk / vdso areas.  T1 (M4.T1) replaces the T0 single cursor
 * with a per-cpu 2M-frame magazine: each CPU claims an exclusive segment
 * of the window once (obstacle-scanned at claim time, marked with
 * CORTEN_FRAME_RESERVE sentinels) and then serves its allocations by a
 * private bump pointer -- no per-mmap VMA-tree walk and no cross-cpu
 * cursor sharing (PS-E1, DESIGN.md sec 1 row E1).  Window exhaustion
 * degrades to the legacy mmap path, counted.
 */
#define CORTEN_MODE_WINDOW_START	0x100000000000UL	/* 16T */
#define CORTEN_MODE_WINDOW_END		0x400000000000UL	/* 64T */

/* Magazine geometry: one 1GiB segment per cpu, handed out frame-granular.
 * 48T of window / 1GiB caps the segment count at 48k; the claimed-segment
 * list is walked on RELEASE only (to restore reserve markers), so its
 * length is cold-path.
 */
#define CORTEN_VA_SEG_FRAMES		512UL
#define CORTEN_VA_SEG_SIZE		(CORTEN_VA_SEG_FRAMES * PMD_SIZE)

/* Reserve sentinel stored in state->arenas for claimed-but-unallocated
 * magazine frames.  It keeps every other xarray walker honest the same
 * way a live arena does (targeted DECLAREs overlap-reject it, punches
 * erase it) while corten_arena_lookup() reports "no arena" for it.  A
 * real (never published) object rather than an IS_ERR encoding, so the
 * xarray stores an ordinary pointer.
 */
struct corten_arena;
extern struct corten_arena corten_va_reserve_sentinel;

/* One magazine block on the per-mm recycle list (M4.T1): frames of a
 * released auto-arena whose markers were restored.  The next magazine
 * allocation carves from here before claiming fresh window (real VA
 * recycling, the T0-R2 follow-up).  Writers are serialized by the owner
 * mm's mmap_lock for writing (both the RELEASE side and the alloc side
 * run under it); the list is torn down in corten_arena_state_free().
 */
struct corten_va_freeblk {
	struct list_head	list;
	unsigned long		base;	/* first frame VA (PMD-aligned) */
	unsigned long		frames;	/* frame count */
};

/* One cpu's private VA segment (bump allocator state). */
struct corten_va_seg {
	unsigned long		base;	/* first VA of the segment */
	unsigned long		end;	/* first VA past the segment */
	unsigned long		next;	/* next unallocated VA (bump) */
};

/*
 * V-A.3a implant registry record (MV_VMA_FREE_SPEC.md sec 2.4 "登记植入",
 * D24): one window-domain VA range the legacy funnel legally owns -- the
 * VMAs a punch route (file MAP_FIXED, sec 5.6 D-G'') or a P1b idle-eject
 * placed over former arena frames.  Stored page-granular and disjoint,
 * sorted by @start, in a growable array (see @implants below).
 */
struct corten_implant_range {
	unsigned long		start;
	unsigned long		end;
};

/**
 * struct corten_mm_state - per-mm arena registry, lazily allocated.
 * @arenas: 2M frame index (addr >> PMD_SHIFT) -> struct corten_arena *.
 *          Stores happen only under @ctl_lock; loads are lockless RCU.
 * @nr: number of active arenas; the lookup fast path short-circuits on
 *      zero without touching the xarray.
 * @ctl_lock: serializes DECLARE/RELEASE (including the drain wait) so
 *            that arena registration is atomic w.r.t. itself.  The fault
 *            path never takes it.
 * @next_va: the MODE-process global window cursor (M4T0_SPEC.md sec 1.3,
 *           T1 repurposed): the next candidate base for SEGMENT CLAIMS
 *           and for allocations too large for one magazine segment.
 *           Written only under this mm's mmap_lock for writing (do_mmap
 *           auto-attach route / ENTER); read with the same lock held.
 * @va_segs: per-cpu magazine segments (M4.T1, PS-E1): one
 *           CORTEN_VA_SEG_SIZE-bounded, PMD-aligned span per cpu, handed
 *           out frame-granular by a private bump pointer.  All writers
 *           (segment claim, bump advance, recycle-list push/pop) run
 *           under this mm's mmap_lock for writing, so the percpu layout
 *           only provides address locality and xarray-subtree disjointness,
 *           not concurrency.
 * @va_free: recycled frame blocks of released auto-arenas (M4.T1
 *           real recycle): LIFO, carved before fresh window is claimed.
 * @va_nrfree: frames currently on @va_free (bounded; overflow degrades
 *             to plain erase, counted).
 * @seg_list: claimed segments (base/end), for marker restoration on
 *            RELEASE and for the debugfs report.
 * @arena_pool: T1c resident arena pool (M4T12_T1C): parked (idle)
 *            arenas of this MODE mm, LRU order (tail = most recently
 *            parked).  Writers run under this mm's mmap_lock for writing
 *            and the registry ctl_lock (park, reactivation, ejection,
 *            pool flush); the only reader fast path (corten_arena_pool_
 *            pick) holds the same locks.
 * @nr_pool: arenas currently parked in @arena_pool; bounded by
 *            CORTEN_ARENA_POOL_MAX (overflow degrades to the pre-pool
 *            behaviour, counted).
 * @implants: V-A.3a implant registry (D24): sorted, disjoint window-domain
 *            ranges the legacy funnel legally owns (punch implants, P1b
 *            idle-eject placements).  Written under mmap_write + @ctl_lock,
 *            read under mmap_read or better, freed with the state.
 * @nr_implants: entries in use in @implants.
 * @nr_implants_alloc: allocation size of @implants.
 * @owner_mm: back-link to the owning address space (M6.T3 shrinker
 *            registry walk).  Never taken by reference: the state is
 *            created and destroyed by the mm's own lifecycle paths
 *            (state_create here, corten_arena_mm_exit()) and the
 *            shrinker pins the mm with mmget_not_zero() before any use
 *            past the RCU section, so the pointer is stable for as long
 *            as the registry membership survives.
 * @shrink_reg: M6.T3 global shrinker-registry node (corten_mm_registry).
 *            Linked (list_add_tail_rcu) when the registry is published
 *            and unlinked (list_del_rcu) at exit; the state memory is
 *            freed after a grace period -- synchronously for
 *            arena-bearing states, deferred via @rcu for arena-less
 *            ones (A5) -- so RCU readers (the shrinker) never touch a
 *            freed state.
 * @rcu: deferred-free handle of the A5 arena-less exit fast path
 *            (corten_arena_mm_exit() hands the teardown to call_rcu()
 *            instead of parking the dying process in
 *            synchronize_rcu()).
 * @shrink_lock: serializes shrinker/evict victim selection per mm (one
 *            picker at a time; two pickers could isolate the same folio
 *            onto two lists).  trylock-only from reclaim context: a
 *            busy mm is skipped, never waited on.
 * @shrink_cursor: per-mm victim rotation cursor over the frame-index
 *            keyspace of @arenas (written only under @shrink_lock).
 *            Gives the shrinker and the evict driver window-level
 *            round-robin instead of T2's from-frame-0 rescan.
 * @shrink_aged: M6.T3 two-pass aging bookkeeping: frame-index ->
 *            xa_mk_value(1) marks windows whose young bits pass 1
 *            already cleared (they are pass-2 evaluation candidates).
 *            Written only under @shrink_lock; GFP_NOWAIT stores.
 * @stats: percpu counters, indexed by enum corten_arena_stat.  Relaxed;
 *         the debugfs readers land with the observability slice (S8,
 *         M3B_DESIGN.md sec 7.4).
 *
 * Published once via mm->corten_state with smp_store_release() and never
 * removed while the mm lives (exit_mmap() takes it down when no fault can
 * be in flight); readers pair it with smp_load_acquire().
 */
struct corten_mm_state {
	struct xarray		arenas;
	refcount_t		nr;
	/* DECLARE/RELEASE (incl. the drain wait) serialization; never
	 * taken by the fault path.
	 */
	struct mutex		ctl_lock;
	unsigned long		next_va;
	struct corten_va_seg __percpu *va_segs;
	struct list_head	va_free;
	unsigned long		va_nrfree;
	struct list_head	seg_list;
	struct list_head	arena_pool;
	unsigned long		nr_pool;
	/* V-A.3a implant registry (D24): the sorted, disjoint
	 * corten_implant_range array -- window-domain ranges the legacy
	 * funnel legally owns.  Writers (corten_implant_mark: the punch
	 * route's success arms and the P1b idle-eject) run under this mm's
	 * mmap_write for writing + ctl_lock; readers
	 * (corten_implant_covers) run under mmap_read or better.  Grown by
	 * krealloc under those locks; freed with the state.  The array is
	 * the V-A.3a budget shape -- TODO(V-A.3c): replace with a per-mm
	 * interval tree when the INV-MV2 walker needs augmented queries.
	 */
	struct corten_implant_range *implants;
	unsigned int		nr_implants;
	unsigned int		nr_implants_alloc;
	struct mm_struct	*owner_mm;
	struct list_head	shrink_reg;
	spinlock_t		shrink_lock;
	unsigned long		shrink_cursor;
	struct xarray		shrink_aged;
	unsigned long __percpu	*stats;
	struct rcu_head		rcu;
};

/* T1c resident-pool capacity.  The guest benchmark shapes (8 vCPU: the
 * t8 mmbench legs, tcmalloc's 8 per-thread caches) churn at most a couple
 * of distinct PMD-rounded sizes per process, so 16 slots cover the whole
 * working set with headroom; the cost of a parked arena is its ~200-byte
 * descriptor (V-A.0 added the embedded region record, sec 2.2) plus one
 * 4K PT page per 2M window (~70KB worst case).  A >MAX-size round-robin
 * simply degrades to the pre-pool behaviour (the overflowing park
 * releases the LRU victim, counted as pool_over).
 */
#define CORTEN_ARENA_POOL_MAX	16

/*
 * S4 hot-path fault hook result (M3B_DESIGN.md sec 4.1).  The arch hook
 * (arch/x86/mm/fault.c) translates these into the x86 delivery exits.
 */
enum corten_fault_action {
	CORTEN_FAULT_FALLBACK = 0,	/* not ours / legacy must run */
	CORTEN_FAULT_HANDLED,		/* transaction completed the fault */
	CORTEN_FAULT_ACCERR,		/* SEGV_ACCERR delivery */
	CORTEN_FAULT_MAPERR,		/* SEGV_MAPERR delivery */
	CORTEN_FAULT_OOM,		/* pagefault_out_of_memory() */
	CORTEN_FAULT_BUS,		/* V-B.3: SIGBUS BUS_ADRERR delivery
					 * (FILE region, beyond EOF -- the
					 * filemap_fault() verdict)
					 */
};

/**
 * struct corten_region_iter - enumeration cursor of the region registry.
 * @frame: next 2M frame index to examine.
 * @last: arena pointer produced by the previous corten_region_next()
 *        call (the pointer-dedup key: one region spans several frame
 *        slots and must be produced exactly once).
 *
 * Stack-allocate, zero with corten_region_iter_init().
 *
 * V-C: the pure-data cursors below (corten_region_iter,
 * corten_region_row, corten_row_iter, corten_smap_stats) live outside
 * the CONFIG_CORTEN_MM_ARENA ifdef because fs/proc/internal.h embeds
 * them by value in struct proc_maps_private; with the arena layer
 * disabled the =n stubs below keep every consumer a no-op.
 */
struct corten_region_iter {
	unsigned long frame;
	struct corten_arena *last;
};

static inline void corten_region_iter_init(struct corten_region_iter *it)
{
	it->frame = 0;
	it->last = NULL;
}

/** One renderable window-domain row (a region or a punched piece). */
struct corten_region_row {
	/** @ar: the owning region record. */
	struct corten_arena	*ar;
	/** @start: first VA of the row (page aligned). */
	unsigned long		start;
	/** @end: first VA past the row. */
	unsigned long		end;
};

/** Row-stream cursor: the region iterator plus the open region state. */
struct corten_row_iter {
	/** @rit: underlying region-registry cursor. */
	struct corten_region_iter rit;
	/** @ar: region whose rows are being emitted (NULL between). */
	struct corten_arena	*ar;
	/** @next: next candidate row start inside @ar. */
	unsigned long		next;
};

/** smaps aggregation of one window row (simplified mem_size_stats). */
struct corten_smap_stats {
	/** @resident: present non-special PTE bytes (Rss). */
	unsigned long	resident;
	/** @anon: present anonymous bytes (Anonymous). */
	unsigned long	anon;
	/** @swapped: swap-entry PTE bytes (Swap). */
	unsigned long	swapped;
	/** @pss: proportional bytes, PSS_SHIFT fixed point (Pss). */
	u64		pss;
};

#ifdef CONFIG_CORTEN_MM_ARENA

/*
 * Hot path: called from do_user_addr_fault() for user-mode faults only,
 * before any VMA/mmap_lock action.  Gate contract: corten=off or an mm
 * without arenas returns CORTEN_FAULT_FALLBACK at O(1).
 */
enum corten_fault_action corten_arena_user_fault(struct mm_struct *mm,
						 unsigned long address,
						 unsigned long error_code,
						 struct pt_regs *regs,
						 unsigned int *flags);

/*
 * V-A.3b audit #1 (j2-audit J1 hygiene): the fast hook's window arm.
 * True for a MODE mm's window-domain address that just got the FALLBACK
 * verdict -- the caller skips lock_vma_under_rcu()'s mas_walk (a
 * guaranteed miss post-A.1, S-1) and takes the slow path directly,
 * where the mmap_read keeps the DECLARE/park race serialization.  The
 * helper itself never terminates a fault (no lockless MAPERR); it
 * bumps the fault_fallback_window observation counter.  Declared here
 * because the caller is arch code (arch/x86/mm/fault.c).
 */
bool corten_fault_window_fallback(struct mm_struct *mm, unsigned long addr);

/*
 * Gate-free internal entry points.  They apply no capability check and no
 * corten=on gate so that KUnit can drive them on an un-enabled kernel
 * (same convention as corten_ptdesc_install()); the only syscall-context
 * caller must be corten_prctl_arena().
 */
int corten_arena_declare(struct mm_struct *mm, unsigned long addr,
			 unsigned long len);
int corten_arena_release(struct mm_struct *mm, unsigned long addr,
			 unsigned long len);
int corten_arena_query(struct mm_struct *mm, unsigned long addr);
void corten_arena_mm_exit(struct mm_struct *mm);

/**
 * corten_arena_lookup - resolve the arena covering @addr, if any.
 * @mm: address space to look in.
 * @addr: address to resolve (any alignment; granularity is PMD_SIZE).
 *
 * O(1), zero VMA-tree/mmap_lock involvement: one load of mm->corten_state,
 * one load of the arena count and one RCU-protected xa_load.  This is the
 * fast path the S4 fault hook is built around.
 *
 * The caller must hold rcu_read_lock() and treat the result as
 * RCU-protected: to use the arena past the read-side critical section it
 * must pin it with percpu_ref_tryget_live() (a failed tryget means the
 * arena is being RELEASEd and the caller must fall back to the legacy
 * path).  The arena is freed by kfree_rcu().
 *
 * Return: the arena containing @addr or NULL.
 */
struct corten_arena *corten_arena_lookup(struct mm_struct *mm,
					 unsigned long addr);

/*
 * ------------------------------------------------------------------ *
 * Region registry (MV_VMA_FREE_SPEC.md sec 2.4, V-A.0): the per-mm 2M
 * frame xarray doubles as the region registry -- a region is embedded
 * 1:1 in its arena, so no second structure exists.  All consumers go
 * through the two functions below (never xa_load/xa_for_each directly)
 * so the internal implementation can change (e.g. a page-granular
 * augmented rbtree, OQ-MV-13) without touching them.
 * ------------------------------------------------------------------
 */

/**
 * corten_region_lookup - resolve the region covering @addr, if any.
 * @mm: address space to look in.
 * @addr: address to resolve (any alignment; granularity is PMD_SIZE).
 *
 * The registry's point-query form: identical semantics to
 * corten_arena_lookup() (reserve markers and parked/RESERVED regions are
 * not covering regions -- a parked window was munmapped and keeps legacy
 * semantics).  Same RCU/pinning contract: caller holds rcu_read_lock(),
 * pin with percpu_ref_tryget_live() to use past the critical section.
 *
 * Return: the arena/region containing @addr or NULL.
 */
struct corten_arena *corten_region_lookup(struct mm_struct *mm,
					  unsigned long addr);

/**
 * corten_region_next - enumerate the region registry in address order.
 * @mm: address space whose regions to walk.
 * @it: the caller's cursor (advanced by the call).
 *
 * Produces each region exactly once (frame slots of one region are
 * pointer-deduplicated), in ascending [start) order, skipping the
 * magazine reserve markers (they are not regions).  Unlike the point
 * lookup this is the full-registry view: parked (RESERVED) regions are
 * produced too -- rendering/visibility decisions belong to the consumer.
 *
 * Locking contract (sec 2.4): caller holds mmap_lock for read, or holds
 * rcu_read_lock() and pins what it wants to keep (kfree_rcu() release).
 * Writers hold mmap_lock for writing (sec 2.7).
 *
 * Return: the next region or NULL when the walk is exhausted.
 */
struct corten_arena *corten_region_next(struct mm_struct *mm,
					struct corten_region_iter *it);

/**
 * corten_region_register - stamp @ar's region record (the registry's
 *                          write side).
 * @ar: the arena whose embedded region to (re)initialise.
 * @rclass: the class to record.
 * @may_prot: CORTEN_PERM_* MAY bound to record; @ar->prot is ORed in so
 *            that the may_prot >= prot invariant holds by construction.
 * @rflags: CORTEN_RF_* to record.
 *
 * Callers are the arena lifecycle write points (DECLARE/auto-attach,
 * fork child registration, park, reactivation), all under the owner mm's
 * mmap_lock for writing.  A non-FILE stamp requires the FILE payload to
 * be gone already (corten_region_file_teardown() dropped it) -- clearing
 * a live reference here would leak it silently, so the stale payload
 * trips the WARN instead.
 */
void corten_region_register(struct corten_arena *ar,
			    enum corten_region_class rclass, u8 may_prot,
			    u32 rflags);

/**
 * corten_region_register_file - stamp @ar's region record in the FILE
 *                               class (V-B.1, the registry's FILE write
 *                               side).
 * @ar: the arena whose embedded region to initialise.
 * @may_prot: CORTEN_PERM_* MAY bound (corten_file_may()'s answer); the
 *            recorded prot is ORed in, same superset rule as register().
 * @rflags: CORTEN_RF_* to record.
 * @file: the backing file; the region takes its own reference
 *        (get_file) -- the carrier's vm_file borrows this same
 *        reference (same source, same drop:
 *        corten_region_file_teardown()).
 * @pgoff: mapping start page offset; a page's file index derives as
 *         @pgoff + (addr - ar->start) / PAGE_SIZE (INV-MV3(d): the
 *         carrier's vm_pgoff must equal this).
 *
 * The caller holds the owner mm's mmap_lock for writing and publishes
 * the carrier before the stamp (register()'s INV-MV3 tripwire reads the
 * carrier pairing).  i_mmap membership is NOT taken here; it is the
 * attach/fork publication's last step.
 */
void corten_region_register_file(struct corten_arena *ar, u8 may_prot,
				 u32 rflags, struct file *file,
				 unsigned long pgoff);

/**
 * corten_region_invariants_ok - INV-MV3 registry walk (sec 2.6, V-A.1).
 * @mm: address space whose region records to check.
 *
 * Asserts the record pairings the VMA-free park surgery exercises --
 * may_prot >= prot, and idle <=> CORTEN_REGION_RESERVED -- on every
 * arena of the registry.  The caller holds mmap_lock for read.
 */
bool corten_region_invariants_ok(struct mm_struct *mm);

/*
 * ------------------------------------------------------------------ *
 * V-C dual-source /proc rendering (MV_VMA_FREE_SPEC.md sec 3.3.1):
 * the window-domain row stream.  A "row" is one renderable piece of
 * one region -- the whole region while it is unpunched, each surviving
 * sub-span when a MAP_FIXED punch carved legacy implants out of it
 * (the shadow era rendered the same split through its tree VMA
 * pieces).  Rows are produced in ascending address order, skipping
 * parked regions (S-4: a parked window was munmapped, rendering it
 * would lie) and tree-anchored targeted-DECLARE arenas (their
 * shadow-VMA is the tree stream's own row).
 *
 * Locking: the caller holds mmap_lock for read or better (the row
 * walk reads frame slots, implant ranges and carrier pointers, all of
 * whose writers hold mmap_lock for writing).
 * ------------------------------------------------------------------
 */

/**
 * corten_row_iter_init - initialise a row-stream cursor.
 * @it: the cursor to zero.
 */
void corten_row_iter_init(struct corten_row_iter *it);

/**
 * corten_row_next - produce the next window row in address order.
 * @mm: address space whose regions to walk.
 * @it: the caller's cursor (advanced by the call).
 * @row: receives the produced row.
 *
 * Return: true and fills @row, or false when the stream is exhausted.
 */
bool corten_row_next(struct mm_struct *mm, struct corten_row_iter *it,
		     struct corten_region_row *row);

/**
 * corten_row_query - first window row with row->end > @addr (the
 * covering-or-next shape find_vma() gives the tree stream).
 * @mm: address space to query.
 * @addr: the probe address.
 * @row: receives the answer.
 *
 * Return: true and fills @row, or false (no window row at/after @addr).
 */
bool corten_row_query(struct mm_struct *mm, unsigned long addr,
		      struct corten_region_row *row);

/**
 * corten_maps_dual_source - should a /proc walk of @mm merge the
 * window row stream?  True for a MODE mm (the only producer of
 * carrier regions).  Such walks take the mmap_read locking arm: the
 * window stream is only stable under it (carriers are created and
 * freed under mmap_lock for writing), and it keeps the per-VMA-lock
 * RCU walk -- whose lock_next_vma() would be a guaranteed window
 * miss -- out of the J1 ledger.
 */
bool corten_maps_dual_source(struct mm_struct *mm);

/**
 * corten_region_smap_stats - PT aggregation of one window row.
 * @mm: address space (the row's owner).
 * @row: the row to aggregate (from corten_row_next/query()).
 * @out: receives the counters (zeroed by the call).
 *
 * Walks the row's PTEs under the PTE lock (INV6 discipline, the
 * mincore-fill skeleton): present pages count Rss/Anonymous and the
 * mapcount-proportional Pss, swap entries count Swap.  The
 * debugfs-same-source aggregation basis of MV_VMA_FREE_SPEC.md sec 3.3.1
 * -- the smaps buckets beyond Rss/Pss/Anonymous/Swap stay zero on
 * window rows (registered disclosure, not parity claims).
 */
void corten_region_smap_stats(struct mm_struct *mm,
			      const struct corten_region_row *row,
			      struct corten_smap_stats *out);

/**
 * corten_pagemap_fill - pagemap truth of a window-domain span.
 * @mm: address space.
 * @addr: span start (page aligned).
 * @end: span end (page aligned, > @addr).
 * @show_pfn: CAP_SYS_ADMIN pfn disclosure (the pagemap reader's).
 * @emit: per-page entry callback (add_to_pagemap shape); a non-zero
 *        return aborts the walk and is passed through.
 * @ctx: @emit's opaque context.
 *
 * Fills the span [addr, end) with pagemap entries derived from the
 * real PTEs (present/swap bits, pfn under @show_pfn, the
 * PM_FILE/PM_MMAP_EXCLUSIVE classification): the frame-table truth a
 * MODE mm's window pages carry, where the generic walk would see only
 * a VMA hole.  All PTE reads take the PTE lock.
 *
 * Return: 0, or the non-zero @emit return verbatim.
 */
int corten_pagemap_fill(struct mm_struct *mm, unsigned long addr,
			unsigned long end, bool show_pfn,
			int (*emit)(void *ctx, u64 pme), void *ctx);

/**
 * corten_prctl_arena - prctl(PR_CORTEN_ARENA) dispatcher.
 * @op: CORTEN_ARENA_DECLARE/_RELEASE/_QUERY.
 * @addr: arg3, the range start (QUERY: the probed address).
 * @len: arg4, the range length (QUERY: ignored; the uapi contract passes 0).
 * @arg5: raw prctl arg5, must be 0.
 *
 * Applies the CAP_SYS_ADMIN and corten=on gates; with CONFIG_CORTEN_MM
 * disabled the stub below keeps the case label compiled in but reduces it
 * to a single -EOPNOTSUPP return, identical to an unknown prctl.
 *
 * Return: 0/1 on success (QUERY), negative errno otherwise.
 */
int corten_prctl_arena(unsigned int op, unsigned long addr, unsigned long len,
		       unsigned long arg5);

/**
 * corten_prctl_mode - prctl(PR_CORTEN_MODE) dispatcher.
 * @op: CORTEN_MODE_ENTER/_EXIT/_GET.
 * @arg3: raw prctl arg3, must be 0.
 * @arg4: raw prctl arg4, must be 0.
 * @arg5: raw prctl arg5, must be 0.
 *
 * ENTER needs CAP_SYS_ADMIN and corten=on (same posture as
 * PR_CORTEN_ARENA); EXIT/GET need corten=on.  With CONFIG_CORTEN_MM
 * disabled the stub below keeps the case label compiled in but reduces
 * it to a single -EOPNOTSUPP return, identical to an unknown prctl.
 *
 * EXIT tears every arena of the current mm down (RELEASE semantics,
 * one by one) and clears the mode -- M4T0_SPEC.md sec 1.1; the older
 * "-EBUSY while arenas alive" sketch in the spec was superseded when
 * fork adopted the same teardown-on-transition policy (DEV-11).
 *
 * Return: 0/1 on success (GET), negative errno otherwise.
 */
int corten_prctl_mode(unsigned int op, unsigned long arg3,
		      unsigned long arg4, unsigned long arg5);

/*
 * S8 observability renderers, called by the debugfs files in mm/corten.c
 * (mm/corten_arena.c owns the arena data, corten.c owns the directory).
 */
void corten_arena_arenas_report(struct seq_file *m);
void corten_arena_stats_report(struct seq_file *m);
/* V-A.3c: the A-series exit gate one-stop read (J1 pair + J2 walker
 * ledger + verdict, kselftest output style).
 */
void corten_arena_audit_gate_report(struct seq_file *m);

#ifdef CONFIG_CORTEN_MM_ARENA_KUNIT_TEST
/* Test hooks for the drain-timeout aggregate wiring: force one timeout
 * record (per-mm half skipped by passing no state) and read the global
 * mirror back.
 */
void corten_arena_test_inject_drain_timeout(void);
long corten_arena_test_drain_timeouts(void);

/* Test hooks for the M5 fork unwinds (R-A): arm a forced fork_commit
 * failure at @stage (1 = commit entry, 2 = after the first arena was
 * mirrored; 0 disarms) and read back an arena's frozen bit.
 */
void corten_arena_test_fork_fail_arm(int stage);
bool corten_arena_test_arena_frozen(struct mm_struct *mm, unsigned long addr);
long corten_arena_test_fork_faithful_count(void);
long corten_arena_test_fork_skips(void);

/* M4.T1 magazine hooks (mm/corten_arena_test.c): allocate @len
 * (PMD-rounded) from @cpu's segment of @mm's magazine (caller holds the
 * mm's mmap_lock for writing), probe a frame's reserve marker, and read
 * the named counters.
 */
int corten_arena_test_mag_alloc_cpu(struct mm_struct *mm, int cpu,
				    unsigned long len, unsigned long *addr);
bool corten_arena_test_frame_reserved(struct mm_struct *mm,
				      unsigned long addr);
long corten_arena_test_mag_skips(void);
long corten_arena_test_seg_claims(void);
long corten_arena_test_va_recycles(void);

/* T1c pool hooks: the named counters, the pool occupancy of @mm's
 * registry, and the parked-state probe for one frame.
 */
struct vm_area_struct *corten_arena_test_carrier_of(struct mm_struct *mm,
						    unsigned long addr);
long corten_arena_test_carriers(void);
long corten_arena_test_auto_vgate(void);
long corten_arena_test_j1_probes(void);
long corten_arena_test_j1_hits(void);

long corten_arena_test_pool_parks(void);
long corten_arena_test_pool_hits(void);
long corten_arena_test_pool_misses(void);
long corten_arena_test_pool_over(void);
long corten_arena_test_pool_ejects(void);
long corten_arena_test_park_unmap_fails(void);
long corten_arena_test_pool_nr(struct mm_struct *mm);
bool corten_arena_test_pool_idle(struct mm_struct *mm, unsigned long addr);
bool corten_arena_test_pt_present(struct mm_struct *mm, unsigned long addr);
bool corten_arena_test_perm_pgprot_pure_eq(u8 perm);

/* V-A.3a placement-surface hooks: the "normally zero" disclosure
 * counters (placement backstop firings, P4 defensive ejects, P1b
 * idle-eject admissions) and the implant registry's occupancy.
 */
long corten_arena_test_placement_backstop(void);
long corten_arena_test_p4_ejects(void);
long corten_arena_test_placement_idle_ejects(void);
long corten_arena_test_implant_nr(struct mm_struct *mm);

/* V-A.3c INV-MV2 walker ledger (C-group anchors). */
long corten_arena_test_j2_walks(void);
long corten_arena_test_j2_violations(void);
long corten_arena_test_j2_stale(void);
long corten_arena_test_j2_first_violation(void);

/* V-A.3b J1-hygiene funnels (audit #1/#2/#29): the fault arm-pair
 * counter and the uffd entry rejects.
 */
long corten_arena_test_fault_fallback_window(void);
long corten_arena_test_uffd_rejects(void);

/* V-C observation counters: GUP-slow window probes (carrier answers
 * and loud window rejects) and the dual-source window rows produced
 * for maps/smaps/PROCMAP_QUERY readers.
 */
long corten_arena_test_gup_probes(void);
long corten_arena_test_gup_probe_rejects(void);
long corten_arena_test_maps_window_rows(void);

/* V-B.2 (H7) hooks: the file-event route counter (truncate/invalidation
 * events handed to the chunk-zap transaction) and the zap backstop
 * (must stay 0 -- every hit is a routing hole).
 */
long corten_arena_test_truncate_routes(void);
long corten_arena_test_zap_single_refuses(void);

/* M6.T3 shrinker hooks: drive the count/scan bodies directly (the
 * shrinker is only registered on a corten=on boot; the bodies are the
 * same functions the shrinker calls, with a synthetic shrink_control),
 * the registry occupancy, and the T4 observability counters.
 */
unsigned long corten_arena_test_shrink_count(void);
unsigned long corten_arena_test_shrink_scan(int nr);
long corten_arena_test_registry_nr(void);
long corten_arena_test_shrink_scans(void);
long corten_arena_test_aging_passes(void);
long corten_arena_test_shrink_swapped(void);
long corten_arena_test_shrink_skipped(void);
/* M6.T3 two-pass aging probe: is @addr's window flagged pass-1-done? */
bool corten_arena_test_window_aged(struct mm_struct *mm, unsigned long addr);

/* M6.T4: live per-desc resident/swapped totals over the registry (the
 * same walker the shrinker count uses).
 */
long corten_arena_test_resident_pages(void);
long corten_arena_test_swapped_pages(void);
#endif

#else /* !CONFIG_CORTEN_MM_ARENA */

/*
 * With the arena layer disabled, exit_mmap() still calls
 * corten_arena_mm_exit() (a no-op: there is nothing to drain) and the
 * prctl dispatcher collapses to -EOPNOTSUPP; declare/release/query have
 * no reachable caller but are stubbed to keep the surface total.
 */
static inline void corten_arena_mm_exit(struct mm_struct *mm)
{
}

static inline int corten_arena_declare(struct mm_struct *mm,
				       unsigned long addr, unsigned long len)
{
	return -EOPNOTSUPP;
}

static inline int corten_arena_release(struct mm_struct *mm,
				       unsigned long addr, unsigned long len)
{
	return -EOPNOTSUPP;
}

static inline int corten_arena_query(struct mm_struct *mm, unsigned long addr)
{
	return -ENOENT;
}

static inline struct corten_arena *corten_arena_lookup(struct mm_struct *mm,
						       unsigned long addr)
{
	return NULL;
}

static inline struct corten_arena *corten_region_lookup(struct mm_struct *mm,
							unsigned long addr)
{
	return NULL;
}

struct corten_region_iter;
struct corten_region_row;
struct corten_row_iter;

static inline struct corten_arena *
corten_region_next(struct mm_struct *mm, struct corten_region_iter *it)
{
	return NULL;
}

static inline void corten_region_register(struct corten_arena *ar,
					  enum corten_region_class rclass,
					  u8 may_prot, u32 rflags)
{
}

static inline void
corten_region_register_file(struct corten_arena *ar, u8 may_prot, u32 rflags,
			    struct file *file, unsigned long pgoff)
{
}

static inline bool corten_region_invariants_ok(struct mm_struct *mm)
{
	return true;
}

static inline void corten_row_iter_init(struct corten_row_iter *it)
{
}

static inline bool corten_row_next(struct mm_struct *mm,
				   struct corten_row_iter *it,
				   struct corten_region_row *row)
{
	return false;
}

static inline bool corten_row_query(struct mm_struct *mm,
				    unsigned long addr,
				    struct corten_region_row *row)
{
	return false;
}

static inline bool corten_maps_dual_source(struct mm_struct *mm)
{
	return false;
}

static inline void corten_region_smap_stats(struct mm_struct *mm,
					    const struct corten_region_row *row,
					    struct corten_smap_stats *out)
{
}

static inline int corten_pagemap_fill(struct mm_struct *mm,
				      unsigned long addr, unsigned long end,
				      bool show_pfn,
				      int (*emit)(void *ctx, u64 pme),
				      void *ctx)
{
	return 0;
}

static inline enum corten_fault_action
corten_arena_user_fault(struct mm_struct *mm, unsigned long address,
			unsigned long error_code, struct pt_regs *regs,
			unsigned int *flags)
{
	return CORTEN_FAULT_FALLBACK;
}

static inline bool
corten_fault_window_fallback(struct mm_struct *mm, unsigned long addr)
{
	return false;
}

static inline int corten_prctl_arena(unsigned int op, unsigned long addr,
				     unsigned long len, unsigned long arg5)
{
	return -EOPNOTSUPP;
}

static inline int corten_prctl_mode(unsigned int op, unsigned long arg3,
				    unsigned long arg4, unsigned long arg5)
{
	return -EOPNOTSUPP;
}

static inline void corten_arena_arenas_report(struct seq_file *m)
{
	seq_puts(m, "arena layer disabled (CONFIG_CORTEN_MM_ARENA=n)\n");
}

static inline void corten_arena_stats_report(struct seq_file *m)
{
}

static inline void corten_arena_audit_gate_report(struct seq_file *m)
{
}

#endif /* CONFIG_CORTEN_MM_ARENA */

#endif /* _LINUX_CORTEN_ARENA_H */
