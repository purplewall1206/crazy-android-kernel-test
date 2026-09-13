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
 * Lock order (M3B_DESIGN.md sec 6.1, outermost first):
 *
 *	ctl_lock (per-mm state mutex) -> mmap_lock (W or R) -> vma write
 *	marks -> percpu_ref(active) [drain barrier] -> fill_lock ->
 *	desc->lock (write, BH-symmetric) -> PTE lock.
 *
 * The fault path (S4) only takes percpu_ref tryget/put and never any of
 * the above locks, which is what makes waiting for a drain under
 * ctl_lock deadlock-free.
 */
#ifndef _LINUX_CORTEN_ARENA_H
#define _LINUX_CORTEN_ARENA_H

#include <linux/completion.h>
#include <linux/corten.h>
#include <linux/mutex.h>
#include <linux/percpu-refcount.h>
#include <linux/refcount.h>
#include <linux/types.h>
#include <linux/xarray.h>

struct mm_struct;

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
 * @rcu: kfree_rcu() deferral so that an RCU-protected lookup can still
 *       read @start/@end while a concurrent RELEASE unregisters.
 */
struct corten_arena {
	unsigned long		start;
	unsigned long		end;
	u8			prot;
	struct mm_struct	*mm;
	struct percpu_ref	active;
	struct completion	drained;
	/* Upper-page-table ensure-alloc serialization (never nests inside
	 * a descriptor lock).
	 */
	struct mutex		fill_lock;
	struct rcu_head		rcu;
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
	unsigned long __percpu	*stats;
};

#ifdef CONFIG_CORTEN_MM_ARENA

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

#else /* !CONFIG_CORTEN_MM_ARENA */

static inline struct corten_arena *corten_arena_lookup(struct mm_struct *mm,
						       unsigned long addr)
{
	return NULL;
}

static inline int corten_prctl_arena(unsigned int op, unsigned long addr,
				     unsigned long len, unsigned long arg5)
{
	return -EOPNOTSUPP;
}

/* exit_mmap() calls this unconditionally; with the arena layer disabled
 * there is nothing to drain.
 */
static inline void corten_arena_mm_exit(struct mm_struct *mm)
{
}

#endif /* CONFIG_CORTEN_MM_ARENA */

#endif /* _LINUX_CORTEN_ARENA_H */
