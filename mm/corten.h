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
 */
int corten_ptdesc_install(struct mm_struct *mm, struct page *pte_page);
void corten_ptdesc_uninstall(struct page *pte_page);

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

#endif /* _MM_CORTEN_H */
