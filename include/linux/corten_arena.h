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
 */
struct corten_arena {
	unsigned long		start;
	unsigned long		end;
	u8			prot;
	struct mm_struct	*mm;
	struct percpu_ref	active;
	struct completion	drained;
	bool			frozen;
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
 * mmap_base / brk / vdso areas.  The per-mm cursor hands out PMD-aligned
 * ranges from it; T0 never recycles them (window exhaustion degrades to
 * the legacy mmap path, counted).
 */
#define CORTEN_MODE_WINDOW_START	0x100000000000UL	/* 16T */
#define CORTEN_MODE_WINDOW_END		0x400000000000UL	/* 64T */

/**
 * struct corten_mm_state - per-mm arena registry, lazily allocated.
 * @arenas: 2M frame index (addr >> PMD_SHIFT) -> struct corten_arena *.
 *          Stores happen only under @ctl_lock; loads are lockless RCU.
 * @nr: number of active arenas; the lookup fast path short-circuits on
 *      zero without touching the xarray.
 * @ctl_lock: serializes DECLARE/RELEASE (including the drain wait) so
 *            that arena registration is atomic w.r.t. itself.  The fault
 *            path never takes it.
 * @next_va: the MODE-process auto-arena allocation cursor
 *           (M4T0_SPEC.md sec 1.3): the next candidate base address in
 *           the [CORTEN_MODE_WINDOW_START, CORTEN_MODE_WINDOW_END)
 *           window.  Written only under this mm's mmap_lock for writing
 *           (do_mmap auto-attach route / ENTER); read with the same lock
 *           held.
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
	unsigned long __percpu	*stats;
};

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
