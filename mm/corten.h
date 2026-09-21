/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CortenMM internal interfaces, shared between mm/corten.c and
 * mm/corten_test.c.  Not exported to the rest of the kernel; the public
 * surface lives in include/linux/corten.h.
 */
#ifndef _MM_CORTEN_H
#define _MM_CORTEN_H

#include <linux/corten.h>
#include <linux/types.h>

struct page;

/*
 * Descriptor install/uninstall, called by the PT-page lifecycle hooks
 * (corten_on_pte_alloc/free) and directly by the KUnit tests.  They skip the
 * corten_enabled_static() gate so tests can exercise the bookkeeping on a
 * kernel booted with corten=off.
 *
 * corten_ptdesc_uninstall() is exclusive against descriptor-lock holders:
 * it publishes staleness under the descriptor write lock (M3a interlock),
 * so a transaction holding that write lock always outlives -- never gets
 * surprised by -- the PT-page teardown.  Every desc->lock taker is
 * BH-symmetric (_bh rwlock variants on both sides), which is what makes
 * the uninstaller's waits bounded even when it runs in softirq context
 * (khugepaged pte_free_defer() -> RCU_SOFTIRQ -> pte_free_now ->
 * pte_free); see corten_ptdesc_uninstall() in mm/corten.c.  It must
 * therefore only be called from contexts that do not already hold any
 * descriptor lock, and only from process or softirq context (the free
 * funnels qualify: they run with the PTE locks dropped; hardirq is not a
 * free-funnel context and the _bh unlock asserts !in_hardirq()).
 */
int corten_ptdesc_install(struct mm_struct *mm, struct page *pte_page);
void corten_ptdesc_uninstall(struct page *pte_page);

/*
 * Idempotent tracking re-arm (r03 defect C root fix): if @pte_page has no
 * descriptor (its M2a install at pte_alloc time failed, leaving the window
 * untracked so transactions fall back to the legacy body), attach one now.
 * A page that is already tracked is left untouched -- the fresh-descriptor
 * WARN_ON(old) contract of corten_ptdesc_install() must never fire here.
 * The arena's fill_upper() calls this on every fill, so a window whose
 * install once failed becomes transactional again at the next touch.
 * Return: true when the page is tracked (pre-existing or re-armed).
 */
bool corten_ptdesc_rearm(struct mm_struct *mm, struct page *pte_page);

/*
 * Drift observability (debugfs stats, next to free_untracked): a nonzero
 * count means legacy-written PTEs were found and cleaned on an untracked
 * window (r03 defect C).  reinstalled -- the successful re-arm counter --
 * is maintained inside corten_ptdesc_rearm().
 */
void corten_legacy_drift_inc(void);

/* KUnit-visible snapshots of the drift counters. */
long corten_ptdesc_reinstalled_count(void);
long corten_legacy_drift_count(void);

/*
 * Set a descriptor's PT level and, with it, its per-level lock class
 * (the level hierarchy is the protocol's lock order: a transaction holds
 * PGD..PTE locks strictly top-down, so same-level descriptors are never
 * nested).  Call before the descriptor's lock is first acquired, instead
 * of writing desc->level directly.
 */
void corten_ptdesc_set_level(struct corten_ptdesc *desc,
			     enum corten_pt_level lvl);

/*
 * PFN-keyed lookup.  corten_ptdesc_get() is safe against concurrent
 * uninstall: the load+pin runs under rcu_read_lock() and the descriptor is
 * freed via kfree_rcu() only after the xarray entry is erased and the last
 * reference dropped.  corten_ptdesc_put() must be called for every
 * successful get().
 */
struct corten_ptdesc *corten_ptdesc_get(unsigned long pfn);
void corten_ptdesc_put(struct corten_ptdesc *desc);

/*
 * Per-PTE metadata array, allocated on demand with GFP_NOWAIT|__GFP_NOWARN
 * (reclaim-recursion red line): callers must tolerate -ENOMEM and retry at a
 * point where a failure is recoverable.
 *
 * corten_meta_ensure() takes desc->lock itself; corten_meta_ensure_locked()
 * is for contexts that already hold desc->lock write side -- the covering
 * page inside a transaction.
 */
int corten_meta_ensure(struct corten_ptdesc *desc);
int corten_meta_ensure_locked(struct corten_ptdesc *desc);

/*
 * Pure covering-page selection helpers (paper Figure 5 L3: the geometric
 * child_pt_page_should_cover() test, assuming 4K base pages -- hugepage
 * leaves are a slice-2b concern and rejected by the glue).
 *
 * The child window of a PT page at level @lvl is the VA window covered by
 * one PT page one level below (@lvl == PGD: the top-level child page, ...
 * @lvl == PMD: a PTE-level page, window PMD_SIZE).
 */

/**
 * corten_child_covers_range - would a single child PT page cover [start,end)?
 * @start: first VA of the range
 * @end: first VA past the range (must be > @start)
 * @lvl: level of the PT page whose children are examined
 *
 * Paper analogue: child_pt_page_should_cover(cur_pt_page, range).  True iff
 * the range fits into one child slot of a PT page at @lvl, i.e. the locking
 * protocol would descend to the child instead of writing-locking the page
 * at @lvl.
 */
bool corten_child_covers_range(unsigned long start, unsigned long end,
			       enum corten_pt_level lvl);

/**
 * corten_covering_level - lowest PT level whose page covers [start,end)
 * @start: first VA of the range
 * @end: first VA past the range (must be > @start)
 *
 * Returns the level of the covering PT page: the deepest PT page that spans
 * the whole range (PTE-level page if the range fits in one PMD window, its
 * parent PMD page if it crosses a 2M boundary but fits in one 1G window,
 * and so on up to the root page itself).
 */
enum corten_pt_level corten_covering_level(unsigned long start,
					   unsigned long end);

/**
 * corten_covering_va_base - base VA of the window covered by the PT page at
 *                           @lvl that contains @addr
 * @addr: virtual address inside the window
 * @lvl: level of the PT page (PTE-level page: PMD_SIZE-aligned base, ...)
 */
unsigned long corten_covering_va_base(unsigned long addr,
				      enum corten_pt_level lvl);

/**
 * corten_slot_index - index of @addr's slot within a PT page at @lvl
 * @addr: virtual address
 * @lvl: level of the PT page (PTE-level: pte_offset index, ...)
 */
unsigned int corten_slot_index(unsigned long addr, enum corten_pt_level lvl);

/*
 * The locking protocol core, generic over the shape of the PT-page tree.
 *
 * The protocol (corten_txn_begin below) needs exactly two operations on the
 * tree: find the highest tracked PT page on the path to an address, and
 * descend to the child of a given PT page.  Two views exist:
 *
 *   - the real x86 page table (mm/corten.c): only PTE-level pages carry
 *     descriptors (the M2a hooks), so root() navigates the untracked
 *     PGD/P4D/PUD/PMD levels itself and returns the PTE-page descriptor;
 *
 *   - synthetic descriptor trees (KUnit): full multi-level chains of
 *     descriptors, exercising the multi-level descent the real tree cannot
 *     express yet (intermediate-level tracking is an M3+ extension).
 *
 * Both store their PT pages as struct corten_ptdesc in the PFN xarray and
 * both hand the core PINNED descriptors (corten_ptdesc_get/put contract,
 * see include/linux/corten.h pin discipline).
 */
struct corten_tree_ops {
	/**
	 * @root: pinned descriptor of the highest tracked PT page on the
	 * path from the tree root to @addr, or:
	 *   NULL              -- no tracked PT page on that path (hole;
	 *                        the protocol reports -ENOENT),
	 *   ERR_PTR(-EOPNOTSUPP) -- the PT page exists but is untracked
	 *                        (allocated before corten=on, or install
	 *                        failure): transactions must fall back,
	 *   ERR_PTR(-EAGAIN)  -- raced with PT-page removal, retry
	 *                        (paper Figure 7).
	 */
	struct corten_ptdesc *(*root)(void *ctx, unsigned long addr);
	/**
	 * @child: pinned descriptor of the child PT page of @parent that
	 * contains @addr, or NULL if that child page does not exist (a
	 * hole below a tracked page), or ERR_PTR() as above.  @parent is
	 * read-locked; a child can therefore not appear or vanish during
	 * the call in a well-formed tree.
	 */
	struct corten_ptdesc *(*child)(void *ctx, struct corten_ptdesc *parent,
				       unsigned long addr);
	/**
	 * @alloc: ensure-alloc (paper Figure 5 L5', reference
	 * "alloc_if_none"): make the child PT page of @parent that covers
	 * @addr exist, returning its PINNED descriptor.  Optional (NULL
	 * views keep the pure no-allocation protocol: holes report
	 * -ENOENT).
	 *
	 * Call contract (corten_txn_begin):
	 *   - called with @parent write-locked, so the call is exclusive
	 *     against competing transactions AND against uninstall (the
	 *     M3 interlock makes uninstall wait for write-lock holders);
	 *   - must not sleep and must not acquire any descriptor lock it
	 *     does not already hold: it runs inside the parent's spinlock;
	 *     memory used to instantiate the child must be obtained with
	 *     GFP_NOWAIT semantics or pre-allocated;
	 *   - the core re-checked ops->child() under the write lock before
	 *     calling, so a non-ERR_PTR return installs a child that was
	 *     absent at upgrade time (the upgrade-window double-install
	 *     race is the core's responsibility, not the view's);
	 *   - the view publishes the child in its own tree and owns the
	 *     bookkeeping (va_base, level, back-links);
	 *   - failures are ERR_PTR(): -ENOMEM (out of memory), or
	 *     -EOPNOTSUPP (this view never allocates outside the fault
	 *     path -- the real x86 view until 3b).
	 */
	struct corten_ptdesc *(*alloc)(void *ctx, struct corten_ptdesc *parent,
				       unsigned long addr);
};

/*
 * Core locking protocol (paper Figure 5), shared by all tree views.  On
 * success @txn->covering is write-locked and pinned; @txn->path is empty
 * (the walk's path read locks are dropped once the covering write lock is
 * held -- Linux rwlock readers cannot be held across a sleeping
 * transaction).  On error nothing is locked or pinned.
 */
int corten_txn_begin(struct mm_struct *mm, unsigned long start,
		     unsigned long end, const struct corten_tree_ops *ops,
		     void *ctx, struct corten_txn *txn);
/* Release a transaction begun by corten_txn_begin(). */
void corten_txn_finish(struct corten_txn *txn);

/*
 * [perf2a] Resolve @addr to its metadata slot pointer inside a running
 * transaction (no payload copy): NULL = no array (nothing recorded),
 * ERR_PTR = range/alignment violation.  The full-reset zap uses it to
 * bound the per-slot reset to the recorded minority of its range.
 * Caller contract: the covering write lock (see mm/corten.c).
 */
struct corten_pte_meta *corten_txn_slot(struct corten_txn *txn,
					unsigned long addr);

/*
 * Allocation-failure injection for the KUnit tests (review gap: the
 * GFP_NOWAIT failure paths were untested).  corten_alloc_should_fail() is
 * consulted by the descriptor and metadata allocators; outside
 * CONFIG_CORTEN_MM_KUNIT_TEST builds it folds to false.
 */
#ifdef CONFIG_CORTEN_MM_KUNIT_TEST
void corten_test_inject_alloc_fail(int nr);
bool corten_alloc_should_fail(void);
#else
static inline bool corten_alloc_should_fail(void)
{
	return false;
}
#endif

/*
 * The in-memory debugfs render harness (S8 assertions): shared test
 * infrastructure, not skeleton-test property -- every KUnit object that
 * must read what the debugfs files would show drives the very same
 * seq_show functions through it (M6.T1: the fault-path suite reads the
 * reclaim-guard counters this way).
 */
#if IS_ENABLED(CONFIG_CORTEN_MM_KUNIT_TEST) ||			\
	IS_ENABLED(CONFIG_CORTEN_MM_ARENA_KUNIT_TEST) ||	\
	IS_ENABLED(CONFIG_CORTEN_MM_ARENA_FAULT_KUNIT_TEST)
enum corten_dbg_file {
	CORTEN_DBG_STATS,
	CORTEN_DBG_DUMP,
	CORTEN_DBG_TXN,
	CORTEN_DBG_ARENAS,
	CORTEN_DBG_ARENA_STATS,
};
/*
 * Render one of the debugfs seq_show outputs into a NUL-terminated,
 * caller-kfree() buffer, or ERR_PTR().  Test-only: drives the very same
 * functions the debugfs files use, without requiring debugfs to be
 * mounted (builtin KUnit runs happen before any userspace could mount
 * it).
 */
char *corten_test_render_dbg(enum corten_dbg_file which);
#endif

#endif /* _MM_CORTEN_H */
