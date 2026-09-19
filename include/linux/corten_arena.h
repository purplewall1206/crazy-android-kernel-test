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

#include <linux/completion.h>
#include <linux/corten.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/percpu-refcount.h>
#include <linux/refcount.h>
#include <linux/seq_file.h>
#include <linux/types.h>
#include <linux/xarray.h>

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
	CORTEN_ARENA_NR_STATS,
};

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
 *            CORTEN_ARENA_POOL_MAX (overflow releases the LRU victim,
 *            counted).
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
	unsigned long __percpu	*stats;
};

/* T1c resident-pool capacity.  The guest benchmark shapes (8 vCPU: the
 * t8 mmbench legs, tcmalloc's 8 per-thread caches) churn at most a couple
 * of distinct PMD-rounded sizes per process, so 16 slots cover the whole
 * working set with headroom; the cost of a parked arena is its ~150-byte
 * descriptor plus one 4K PT page per 2M window (~70KB worst case).  A
 * >MAX-size round-robin simply degrades to the pre-pool behaviour (the
 * overflowing park releases the LRU victim, counted as pool_over).
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
long corten_arena_test_pool_parks(void);
long corten_arena_test_pool_hits(void);
long corten_arena_test_pool_misses(void);
long corten_arena_test_pool_over(void);
long corten_arena_test_pool_ejects(void);
long corten_arena_test_pool_nr(struct mm_struct *mm);
bool corten_arena_test_pool_idle(struct mm_struct *mm, unsigned long addr);
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

static inline enum corten_fault_action
corten_arena_user_fault(struct mm_struct *mm, unsigned long address,
			unsigned long error_code, struct pt_regs *regs,
			unsigned int *flags)
{
	return CORTEN_FAULT_FALLBACK;
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

#endif /* CONFIG_CORTEN_MM_ARENA */

#endif /* _LINUX_CORTEN_ARENA_H */
