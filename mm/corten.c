// SPDX-License-Identifier: GPL-2.0
/*
 * CortenMM - page-descriptor based transactional memory management.
 *
 * This file implements the M2a data-structure skeleton and the M2b
 * CortenMMrw locking protocol and transaction API:
 *
 *   - the PFN-keyed xarray of page descriptors (engineering adaptation of
 *     the paper's boot-time contiguous descriptor array, see
 *     include/linux/corten.h),
 *   - the PT-page lifecycle hooks wired into the x86 pte_alloc_one() and
 *     the two PTE-page free funnels,
 *   - the on-demand per-PTE metadata arrays,
 *   - the pure covering-page selection helpers,
 *   - the CortenMMrw locking protocol (paper Figure 5), written as a
 *     generic core over a "tree view" (corten_tree_ops) plus a real-x86
 *     glue view, so KUnit can drive synthetic multi-level descriptor
 *     trees without a real process,
 *   - the transaction operations (paper Figure 4: query/map/mark/unmap),
 *     metadata-only in this slice (the hardware PTE write is M3 scope),
 *   - the debugfs introspection under /sys/kernel/debug/corten/.
 *
 * All of it is inert unless the kernel is booted with corten=on; the hooks
 * cost one not-taken static branch in that state.
 */

#include <linux/atomic.h>
#include <linux/corten.h>
#include <linux/debugfs.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/pgtable.h>
#include <linux/rcupdate.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/static_key.h>
#include <linux/string.h>
#include <linux/xarray.h>

#include "corten.h"

/*
 * corten=on enables the hooks at boot (default off).  There is deliberately
 * no runtime off switch: descriptors are created and destroyed in strictly
 * paired hooks, and a mid-boot disable would leave pages allocated while
 * enabled with no free-side event to retire them.  Unknown values are
 * ignored (and consumed so they do not leak into init's environment).
 */
DEFINE_STATIC_KEY_FALSE(corten_enabled_key);

/*
 * The static key must NOT be flipped from the __setup() callback.  The
 * callback runs from obsolete_checksetup() (init/main.c) during
 * parse_args("Booting kernel") in start_kernel(), i.e. before mm_core_init()
 * and poking_init(); jump-label patching at that point goes through the
 * text-patching machinery before it is usable, which hung every corten=on
 * boot (results/r01/m2-on-panic.log).  The parameter is therefore only
 * recorded here and applied by corten_late_init() below: initcalls run from
 * do_initcalls() via do_basic_setup() (init/main.c), strictly after
 * jump_label_init() (start_kernel, init/main.c:1057) and long after the
 * allocator and text-patching are up.  By initcall time no user PT page can
 * have been allocated yet (userspace starts after every initcall), so
 * enabling here still covers the hooks completely.
 */
static bool corten_param_on;

static int __init corten_setup_param(char *s)
{
	if (!s)
		return 0;
	if (!strcmp(s, "on")) {
		corten_param_on = true;
		pr_info("corten: requested on, activating at initcall time\n");
	}
	return 1;
}
__setup("corten=", corten_setup_param);

static int __init corten_late_init(void)
{
	if (corten_param_on) {
		static_branch_enable(&corten_enabled_key);
		pr_info("corten: page descriptors enabled\n");
	}

	return 0;
}
early_initcall(corten_late_init);

/* PFN -> page descriptor.  The engineering substitute for the paper's
 * boot-time contiguous descriptor array indexed by PFN.
 */
static DEFINE_XARRAY(corten_ptdesc_xa);

/*
 * Statistics (debugfs).  These are the only relaxed cross-CPU counters in
 * the file; they are advisory and never used for control flow.
 */
static atomic_long_t corten_nr_ptdescs;
static atomic_long_t corten_nr_meta_arrays;
static atomic_long_t corten_nr_desc_alloc_fail;
static atomic_long_t corten_nr_meta_alloc_fail;
static atomic_long_t corten_nr_free_untracked;
/* Active transactions (entered/exited through the protocol below). */
static atomic_long_t corten_nr_txns = ATOMIC_LONG_INIT(0);
static atomic_long_t corten_nr_txns_max;

#ifdef CONFIG_CORTEN_MM_KUNIT_TEST
/*
 * Allocation-failure injection (review gap: the GFP_NOWAIT failure paths of
 * the allocators were untested).  The KUnit suite arms a countdown; the
 * next that many descriptor/metadata allocations fail as if memory was
 * exhausted.
 */
static atomic_t corten_alloc_fail_countdown = ATOMIC_INIT(0);

void corten_test_inject_alloc_fail(int nr)
{
	atomic_set(&corten_alloc_fail_countdown, nr);
}

bool corten_alloc_should_fail(void)
{
	int old = atomic_read(&corten_alloc_fail_countdown);

	do {
		if (old <= 0)
			return false;
	} while (atomic_cmpxchg(&corten_alloc_fail_countdown, old,
				old - 1) != old);

	return true;
}
#endif

/* ---- descriptor lifecycle ------------------------------------------ */

static void corten_meta_free(struct corten_ptdesc *desc)
{
	if (desc->meta) {
		atomic_long_dec(&corten_nr_meta_arrays);
		kfree(desc->meta);
		desc->meta = NULL;
	}
}

/*
 * Descriptor release, reached exactly when the last reference drops.  The
 * xarray entry has already been erased by the uninstaller (remove before
 * reclaim), so lockless lookup can no longer find us; kfree_rcu() only has
 * to cover lookups that were in flight between xa_load() and the pin.
 */
static void corten_ptdesc_release(struct corten_ptdesc *desc)
{
	corten_meta_free(desc);
	kfree_rcu(desc, rcu);
}

/**
 * corten_ptdesc_get - pin the page descriptor of PT page @pfn.
 * @pfn: PFN of a user PTE-level page.
 *
 * Safe against concurrent uninstall; see mm/corten.h.  Returns the pinned
 * descriptor or NULL if the PT page is not (or no longer) tracked.
 */
struct corten_ptdesc *corten_ptdesc_get(unsigned long pfn)
{
	struct corten_ptdesc *desc;

	rcu_read_lock();
	desc = xa_load(&corten_ptdesc_xa, pfn);
	/*
	 * refs >= 1 while the entry is indexed (the uninstaller drops the
	 * base reference only after xa_erase), so the inc_not_zero cannot
	 * fail for an entry seen in the xarray; the check keeps the pin
	 * protocol well-formed under any future indexing change.
	 */
	if (desc && !refcount_inc_not_zero(&desc->refs))
		desc = NULL;
	rcu_read_unlock();

	return desc;
}

/**
 * corten_ptdesc_put - drop a pin taken by corten_ptdesc_get().
 * @desc: descriptor returned by corten_ptdesc_get().
 */
void corten_ptdesc_put(struct corten_ptdesc *desc)
{
	if (refcount_dec_and_test(&desc->refs))
		corten_ptdesc_release(desc);
}

/**
 * corten_meta_ensure_locked - allocate the per-PTE metadata array if absent.
 * @desc: descriptor of the PT page whose array is needed.
 *
 * Caller must hold desc->lock write side (the covering page of a running
 * transaction) or otherwise own the descriptor exclusively (the installer
 * before publication).  Allocation uses GFP_NOWAIT|__GFP_NOWARN: PT-page
 * sized allocations happen deep inside the fault path where sleeping on
 * reclaim would risk the reclaim-recursion deadlock, so callers must
 * tolerate -ENOMEM and retry where a failure is recoverable.
 */
int corten_meta_ensure_locked(struct corten_ptdesc *desc)
{
	struct corten_pte_meta *meta;

	if (READ_ONCE(desc->meta))
		return 0;

	if (corten_alloc_should_fail()) {
		atomic_long_inc(&corten_nr_meta_alloc_fail);
		return -ENOMEM;
	}

	meta = kmalloc(CORTEN_META_ARRAY_BYTES,
		       GFP_NOWAIT | __GFP_NOWARN | __GFP_ZERO);
	if (!meta) {
		atomic_long_inc(&corten_nr_meta_alloc_fail);
		return -ENOMEM;
	}

	/* The lock (or exclusive installer context) makes the publish plain
	 * and keeps pinned readers on the same lock side seeing either NULL
	 * or a fully formed array.
	 */
	if (!desc->meta) {
		desc->meta = meta;
		atomic_long_inc(&corten_nr_meta_arrays);
	} else {
		kfree(meta);
	}

	return 0;
}

/**
 * corten_meta_ensure - unlocked wrapper around corten_meta_ensure_locked().
 * @desc: descriptor of the PT page, pinned by the caller.
 */
int corten_meta_ensure(struct corten_ptdesc *desc)
{
	int ret;

	if (READ_ONCE(desc->meta))
		return 0;

	write_lock(&desc->lock);
	ret = corten_meta_ensure_locked(desc);
	write_unlock(&desc->lock);

	return ret;
}

/**
 * corten_ptdesc_install - attach a page descriptor to a fresh PTE-level page.
 * @mm: owning address space (back-link; may be NULL for mock contexts).
 * @pte_page: the PT page being born.
 *
 * Called from the pte_alloc_one() funnel.  Failures are tolerated on
 * purpose: the PT page is already allocated and cannot be rolled back from
 * here, so an untracked PT page only means "CortenMM transactions on this
 * page must fall back" (the protocol reports -EOPNOTSUPP); it must never
 * break the caller.  The descriptor stays reachable until the paired
 * corten_ptdesc_uninstall().
 *
 * Returns 0 or a negative error (informational; callers ignore it).
 */
int corten_ptdesc_install(struct mm_struct *mm, struct page *pte_page)
{
	unsigned long pfn = page_to_pfn(pte_page);
	struct corten_ptdesc *desc;
	void *old;
	int ret;

	BUILD_BUG_ON(CORTEN_PTES_PER_PT_PAGE != PTRS_PER_PTE);
	BUILD_BUG_ON(sizeof(struct corten_pte_meta) != 8);

	if (corten_alloc_should_fail()) {
		atomic_long_inc(&corten_nr_desc_alloc_fail);
		return -ENOMEM;
	}

	desc = kmalloc(sizeof(*desc), GFP_NOWAIT | __GFP_NOWARN | __GFP_ZERO);
	if (!desc) {
		atomic_long_inc(&corten_nr_desc_alloc_fail);
		return -ENOMEM;
	}

	rwlock_init(&desc->lock);
	refcount_set(&desc->refs, 1);
	desc->mm = mm;
	desc->level = CORTEN_LEVEL_PTE;
	desc->magic = CORTEN_PTDESC_MAGIC;

	/*
	 * Insert before the page becomes reachable through the page table
	 * (pmd_install() happens after pte_alloc_one() returns), so no RCU
	 * period is needed on the insert side.
	 */
	old = xa_store(&corten_ptdesc_xa, pfn, desc, GFP_NOWAIT);
	if (xa_is_err(old)) {
		kfree(desc);
		atomic_long_inc(&corten_nr_desc_alloc_fail);
		return xa_err(old);
	}
	/*
	 * A PT page must be born and die exactly once between its paired
	 * hooks; seeing a stale entry here would mean the free funnel missed
	 * a page (reported loudly -- this is an invariant of the protocol).
	 * The stale descriptor loses its base reference; outstanding pins
	 * keep it alive until their put().
	 */
	if (WARN_ON_ONCE(old)) {
		/* The replaced descriptor was never retired by its free
		 * funnel; drop its base reference and its counter entry.
		 */
		atomic_long_dec(&corten_nr_ptdescs);
		corten_ptdesc_put(old);
	}

	atomic_long_inc(&corten_nr_ptdescs);

	/* Best effort: transactions retry at first use (corten_map()/mark()
	 * run corten_meta_ensure_locked() under the covering write lock).
	 */
	ret = corten_meta_ensure(desc);
	if (ret)
		pr_debug("corten: pfn %lu metadata deferred (%d)\n", pfn, ret);

	return 0;
}

/**
 * corten_ptdesc_uninstall - retire the descriptor of a dying PTE-level page.
 * @pte_page: the PT page being freed.
 *
 * Called from the two PTE-page free funnels (pte_free_tlb()/pte_free()).
 * Order follows the RCU remove-before-reclaim rule: the xarray entry is
 * erased first so no new lookup can find the descriptor, then the base
 * reference is dropped; the kfree_rcu() in corten_ptdesc_release() waits
 * out lookups that were in flight.  Staleness is published before the base
 * reference drops so pinned protocol readers re-check it under the
 * descriptor lock and fail with -EAGAIN (paper Figure 7).  M3 owes an
 * interlock making this function wait for in-flight write-lock holders
 * before the PT page memory is reused (see include/linux/corten.h).
 */
void corten_ptdesc_uninstall(struct page *pte_page)
{
	unsigned long pfn = page_to_pfn(pte_page);
	struct corten_ptdesc *desc;

	desc = xa_erase(&corten_ptdesc_xa, pfn);
	if (!desc) {
		/*
		 * Untracked page: only possible for pages allocated before
		 * corten=on took effect (or KUnit double-uninstalls).  Not
		 * an error, but worth counting.
		 */
		atomic_long_inc(&corten_nr_free_untracked);
		return;
	}

	WRITE_ONCE(desc->stale, 1);
	atomic_long_dec(&corten_nr_ptdescs);
	corten_ptdesc_put(desc);
}

/*
 * PT-page lifecycle hooks.  Called unconditionally from the alloc/free
 * funnels; the static branch keeps the disabled cost at one nop.
 */
void corten_on_pte_alloc(struct mm_struct *mm, struct page *pte_page)
{
	if (!corten_enabled_static())
		return;
	/* Informational only; see corten_ptdesc_install(). */
	corten_ptdesc_install(mm, pte_page);
}

void corten_on_pte_free(struct page *pte_page)
{
	if (!corten_enabled_static())
		return;
	corten_ptdesc_uninstall(pte_page);
}

/* ---- covering-page selection (pure arithmetic) ---------------------- */

static unsigned long corten_level_window_mask(enum corten_pt_level lvl)
{
	switch (lvl) {
	case CORTEN_LEVEL_PTE:
		return PMD_MASK;
	case CORTEN_LEVEL_PMD:
		return PUD_MASK;
	case CORTEN_LEVEL_PUD:
		return P4D_MASK;
	case CORTEN_LEVEL_P4D:
		return PGDIR_MASK;
	case CORTEN_LEVEL_PGD:
	default:
		return 0;	/* root page covers everything */
	}
}

/**
 * corten_child_covers_range - would one child PT page cover [start, end)?
 *
 * The child window of a page at @lvl is the slot size of its entries
 * (PMD-level page: one PTE child spans PMD_SIZE, etc.).
 */
bool corten_child_covers_range(unsigned long start, unsigned long end,
			       enum corten_pt_level lvl)
{
	unsigned long last = end - 1;
	unsigned long mask;

	switch (lvl) {
	case CORTEN_LEVEL_PGD:
		mask = PGDIR_MASK;
		break;
	case CORTEN_LEVEL_P4D:
		mask = P4D_MASK;
		break;
	case CORTEN_LEVEL_PUD:
		mask = PUD_MASK;
		break;
	case CORTEN_LEVEL_PMD:
		mask = PMD_MASK;
		break;
	case CORTEN_LEVEL_PTE:
	default:
		return false;	/* leaf pages have no children */
	}

	return (start & mask) == (last & mask);
}

/**
 * corten_covering_level - lowest PT level whose page spans [start, end).
 *
 * Mirrors the CortenMMrw descent: descend while a child covers, and the
 * level at which the descent stops is the covering page's level.
 */
enum corten_pt_level corten_covering_level(unsigned long start,
					   unsigned long end)
{
	unsigned long last = end - 1;

	if ((start & PMD_MASK) == (last & PMD_MASK))
		return CORTEN_LEVEL_PTE;
	if ((start & PUD_MASK) == (last & PUD_MASK))
		return CORTEN_LEVEL_PMD;
	if ((start & P4D_MASK) == (last & P4D_MASK))
		return CORTEN_LEVEL_PUD;
	if ((start & PGDIR_MASK) == (last & PGDIR_MASK))
		return CORTEN_LEVEL_P4D;

	return CORTEN_LEVEL_PGD;
}

/**
 * corten_covering_va_base - base VA of the window covered by the PT page at
 *                           @lvl containing @addr.
 */
unsigned long corten_covering_va_base(unsigned long addr,
				      enum corten_pt_level lvl)
{
	return addr & corten_level_window_mask(lvl);
}

/**
 * corten_slot_index - index of @addr's slot within a PT page at @lvl.
 */
unsigned int corten_slot_index(unsigned long addr, enum corten_pt_level lvl)
{
	switch (lvl) {
	case CORTEN_LEVEL_PGD:
		return pgd_index(addr);
	case CORTEN_LEVEL_P4D:
		return (addr >> P4D_SHIFT) & (PTRS_PER_P4D - 1);
	case CORTEN_LEVEL_PUD:
		return (addr >> PUD_SHIFT) & (PTRS_PER_PUD - 1);
	case CORTEN_LEVEL_PMD:
		return (addr >> PMD_SHIFT) & (PTRS_PER_PMD - 1);
	case CORTEN_LEVEL_PTE:
	default:
		return (addr >> PAGE_SHIFT) & (PTRS_PER_PTE - 1);
	}
}

/* ---- CortenMMrw locking protocol (paper Figure 5) -------------------- */

static void corten_txn_release_path(struct corten_txn *txn)
{
	while (txn->nr_path) {
		struct corten_ptdesc *desc = txn->path[--txn->nr_path];

		/* Reverse acquisition order (paper Figure 5 L13:
		 * release_locks_in_reverse), deepest first.
		 */
		read_unlock(&desc->lock);
		corten_ptdesc_put(desc);
	}
}

/**
 * corten_txn_begin - the generic CortenMMrw locking protocol core.
 *
 * Paper Figure 5, step by step:
 *
 *   L2   cur = ops->root(ctx, start)     -- descend from the (highest
 *                                           tracked) root page;
 *   L3   while child_covers(cur, range)  -- corten_child_covers_range();
 *   L4   read_lock(cur)                  -- taken on every page the walk
 *                                           stands on, keeping the whole
 *                                           root->covering path locked;
 *   L5/L6 cur = child_node_of(cur)       -- ops->child(), pinned;
 *   L5'  no child (hole): bail out with -ENOENT.  The paper instead
 *        write-locks the current page after dropping its read lock; the
 *        verified reference allocates the missing child there
 *        (entry.alloc_if_none).  2b is the pure protocol and allocates
 *        nothing; M3 replaces this branch with ensure-alloc under the
 *        write lock.
	 *   L7/L8 write_lock(covering)           -- Linux rwlock_t has no upgrade,
	 *                                           so the candidate's read lock is
	 *                                           dropped and re-taken write side.
	 *                                           Scope of the paper's "frozen
	 *                                           descent" argument (the
	 *                                           full-path read locks keep the
	 *                                           walk's decision valid because
	 *                                           no other thread can change the
	 *                                           child pointers of a read-locked
	 *                                           page): the real x86 view takes
	 *                                           at most one read lock (2b
	 *                                           tracks PTE-level pages only;
	 *                                           corten_real_child() is
	 *                                           unreachable), so its descent
	 *                                           decision cannot be invalidated
	 *                                           regardless of path locks --
	 *                                           what keeps the walked
	 *                                           hierarchy stable there is the
	 *                                           caller-level contract, i.e.
	 *                                           mmap_lock held for read (see
	 *                                           corten_lock_range()).  The
	 *                                           "path locks freeze structural
	 *                                           changes" reasoning holds only
	 *                                           for multi-level tree views
	 *                                           (the synthetic KUnit views),
	 *                                           and there it rests on the
	 *                                           view's own contract that
	 *                                           read-locked pages have
	 *                                           immutable child pointers.
	 *                                           Only staleness has to be
	 *                                           re-checked under the write
	 *                                           lock -- stale means raced with
	 *                                           PT-page removal, paper
	 *                                           Figure 7;
 *   L13  unlock                          -- the path read locks are
 *                                           released bottom-up as soon as
 *                                           the write lock is secured
 *                                           (deviation from the paper
 *                                           documented in
 *                                           include/linux/corten.h: Linux
 *                                           rwlock readers must not be
 *                                           held across a sleeping
 *                                           transaction); corten_unlock()
 *                                           then only drops the covering
 *                                           write lock.
 *
 * On success @txn->covering is write-locked and pinned; on error nothing
 * is locked or pinned.
 */
int corten_txn_begin(struct mm_struct *mm, unsigned long start,
		     unsigned long end, const struct corten_tree_ops *ops,
		     void *ctx, struct corten_txn *txn)
{
	struct corten_ptdesc *cur;
	long active, watermark;

	txn->mm = mm;
	txn->start = start;
	txn->end = end;
	txn->covering = NULL;
	txn->nr_path = 0;
	txn->level = 0;

	/* L2: root page of the walk (highest tracked PT page). */
	cur = ops->root(ctx, start);
	if (!cur)
		/* Hole: no tracked PT page covers @start at all (M3 adds
		 * ensure-alloc; the pure protocol does not allocate).
		 */
		return -ENOENT;
	if (IS_ERR(cur))
		return PTR_ERR(cur);

	for (;;) {
		struct corten_ptdesc *child;

		/* CORTEN_TXN_PATH_MAX bounds the walk structurally (the
		 * level strictly decreases each round); a violation means a
		 * corrupt tree view.
		 */
		if (WARN_ON_ONCE(txn->nr_path >= CORTEN_TXN_PATH_MAX)) {
			corten_ptdesc_put(cur);
			corten_txn_release_path(txn);
			return -EPROTO;
		}

		/* L4: read-lock the page the walk stands on. */
		read_lock(&cur->lock);
		if (unlikely(READ_ONCE(cur->stale))) {
			read_unlock(&cur->lock);
			corten_ptdesc_put(cur);
			corten_txn_release_path(txn);
			return -EAGAIN;
		}
		txn->path[txn->nr_path++] = cur;

		/* L3: does a single child of cur cover the whole range? */
		if (!corten_child_covers_range(start, end, cur->level))
			break;		/* cur is the covering candidate */

		/* L5/L6: descend to the child containing start. */
		child = ops->child(ctx, cur, start);
		if (!child) {
			/* Hole below a tracked page: 2b does not allocate
			 * (M3: ensure-alloc under the write lock, see
			 * above).  Nothing below the hole can hold state,
			 * so -ENOENT is exact here.
			 */
			corten_txn_release_path(txn);
			return -ENOENT;
		}
		if (IS_ERR(child)) {
			int err = PTR_ERR(child);

			corten_txn_release_path(txn);
			return err;
		}
		cur = child;
	}

	/* L7/L8: upgrade the candidate to the covering write lock. */
	cur = txn->path[--txn->nr_path];
	read_unlock(&cur->lock);
	write_lock(&cur->lock);
	if (unlikely(READ_ONCE(cur->stale))) {
		write_unlock(&cur->lock);
		corten_ptdesc_put(cur);
		corten_txn_release_path(txn);
		return -EAGAIN;
	}

	txn->covering = cur;
	txn->level = cur->level;
	/* First visit: record the covered window base (0 until now; the
	 * pte_alloc_one() hook does not know the VA).  Only one thread can
	 * be here per descriptor (the write lock).
	 */
	if (!READ_ONCE(cur->va_base))
		WRITE_ONCE(cur->va_base,
			   corten_covering_va_base(start, cur->level));

	/* The path read locks have done their job; release them so the
	 * transaction body may sleep/allocate (the covering write lock alone
	 * provides the mutual exclusion).  Correctness in the real view does
	 * not depend on these locks "freezing" the tree: its path is at most
	 * 1 long and the hierarchy is kept stable by the caller's mmap_lock
	 * (corten_lock_range() contract) -- see the L7/L8 note above for the
	 * scope of the freeze argument in multi-level tree views.
	 */
	corten_txn_release_path(txn);

	active = atomic_long_inc_return(&corten_nr_txns);
	watermark = atomic_long_read(&corten_nr_txns_max);
	while (active > watermark &&
	       !atomic_long_try_cmpxchg(&corten_nr_txns_max, &watermark,
					active))
		;

	return 0;
}

/**
 * corten_txn_finish - release a transaction (paper Figure 5 L13).
 *
 * The covering write lock goes first, then any leftover path read locks in
 * reverse acquisition order, then the pins.
 */
void corten_txn_finish(struct corten_txn *txn)
{
	struct corten_ptdesc *covering = txn->covering;

	if (!covering) {
		/* Unlocked (failed begin): release whatever is left over,
		 * which for a well-formed caller is nothing.
		 */
		WARN_ON_ONCE(txn->nr_path);
		corten_txn_release_path(txn);
		return;
	}

	txn->covering = NULL;
	write_unlock(&covering->lock);
	corten_ptdesc_put(covering);
	atomic_long_dec(&corten_nr_txns);
	corten_txn_release_path(txn);
}

/* ---- real x86 page-table view --------------------------------------- */

/*
 * Navigate the untracked upper levels (PGD/P4D/PUD/PMD) of the real page
 * table.  M2b tracks PTE-level pages only (the M2a hooks), so the highest
 * tracked page on the path to @addr is the PTE page of its 2M window.
 *
 * Safety contract (see corten_lock_range): the caller keeps the walked
 * hierarchy alive for the duration of the walk; the returned descriptor is
 * additionally protected by the pin protocol against PT-page teardown.
 */
static struct corten_ptdesc *corten_real_root(void *ctx, unsigned long addr)
{
	struct mm_struct *mm = ctx;
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	pgd_t pgd;
	p4d_t p4d;
	pud_t pud;
	pmd_t pmd;
	struct corten_ptdesc *desc;

	pgdp = pgd_offset(mm, addr);
	pgd = READ_ONCE(*pgdp);
	if (!pgd_present(pgd))
		return NULL;
	p4dp = p4d_offset(pgdp, addr);
	p4d = READ_ONCE(*p4dp);
	if (!p4d_present(p4d))
		return NULL;
	pudp = pud_offset(p4dp, addr);
	pud = READ_ONCE(*pudp);
	if (!pud_present(pud))
		return NULL;
	if (unlikely(pud_leaf(pud)))
		return ERR_PTR(-EOPNOTSUPP);	/* 1G leaf: M4+ */
	pmdp = pmd_offset(pudp, addr);
	pmd = READ_ONCE(*pmdp);
	if (!pmd_present(pmd))
		return NULL;
	if (unlikely(pmd_leaf(pmd)))
		return ERR_PTR(-EOPNOTSUPP);	/* THP leaf: M4+ */

	desc = corten_ptdesc_get(page_to_pfn(pmd_page(pmd)));
	if (!desc)
		/* PT page present but untracked (born before corten=on, or
		 * install failed): transactions must fall back.
		 */
		return ERR_PTR(-EOPNOTSUPP);
	if (unlikely(READ_ONCE(desc->stale) || desc->mm != mm)) {
		corten_ptdesc_put(desc);
		return ERR_PTR(-EAGAIN);
	}

	return desc;
}

/*
 * The real 2b walk never descends below the first (and only) tracked level,
 * so this is unreachable from the protocol; it exists to keep the tree view
 * total.  M3+ installs descriptors for intermediate PT pages (pmd/pud hooks)
 * and fleshes this out.
 */
static struct corten_ptdesc *corten_real_child(void *ctx,
					       struct corten_ptdesc *parent,
					       unsigned long addr)
{
	WARN_ON_ONCE(1);
	return ERR_PTR(-EOPNOTSUPP);
}

static const struct corten_tree_ops corten_real_ops = {
	.root		= corten_real_root,
	.child		= corten_real_child,
};

/**
 * corten_lock_range - see include/linux/corten.h.
 */
int corten_lock_range(struct mm_struct *mm, unsigned long start,
		      unsigned long len, struct corten_txn *txn)
{
	unsigned long end;

	if (unlikely(!mm || !txn))
		return -EINVAL;
	if (unlikely(!len || (len & ~PAGE_MASK)))
		return -EINVAL;
	if (unlikely(start & ~PAGE_MASK))
		return -EINVAL;
	if (unlikely(check_add_overflow(start, len, &end)))
		return -EOVERFLOW;

	/*
	 * 2b scope: only PTE-level PT pages carry descriptors, so the range
	 * must fit a single PMD (2M) window -- i.e. a single lowest-level
	 * covering page.  Wider ranges are rejected rather than
	 * under-locked; M4+ turns the cursor into an iterator issuing one
	 * transaction per covering PT page.
	 */
	if (unlikely(corten_covering_level(start, end) != CORTEN_LEVEL_PTE))
		return -EOPNOTSUPP;

	return corten_txn_begin(mm, start, end, &corten_real_ops, mm, txn);
}

/* ---- transaction operations (paper Figure 4) ------------------------- */

/*
 * Resolve @addr to its metadata entry within the covering PT page, under
 * the write lock held by the transaction.  Returns NULL when the page's
 * metadata array does not exist (nothing recorded yet: queries as
 * CORTEN_INVALID), the entry pointer, or an ERR_PTR.
 */
static struct corten_pte_meta *corten_txn_meta(struct corten_txn *txn,
					       unsigned long addr)
{
	unsigned int idx;

	if (unlikely(addr < txn->start || addr >= txn->end))
		return ERR_PTR(-ERANGE);
	if (unlikely(addr & ~PAGE_MASK))
		return ERR_PTR(-EINVAL);
	if (unlikely(!txn->covering)) {
		WARN_ON_ONCE(1);
		return ERR_PTR(-EINVAL);
	}

	idx = corten_slot_index(addr, txn->covering->level);
	if (unlikely(idx >= CORTEN_PTES_PER_PT_PAGE)) {
		WARN_ON_ONCE(1);
		return ERR_PTR(-EIO);
	}
	if (!txn->covering->meta)
		return NULL;

	return &txn->covering->meta[idx];
}

/* Page-aligned sub-range of the locked range ([start, start+len) -> *endp). */
static int corten_txn_subrange(struct corten_txn *txn, unsigned long start,
			       unsigned long len, unsigned long *endp)
{
	if (unlikely(!len || (len & ~PAGE_MASK)))
		return -EINVAL;
	if (unlikely(start & ~PAGE_MASK))
		return -EINVAL;
	if (unlikely(start < txn->start || len > txn->end - start))
		return -ERANGE;

	*endp = start + len;
	return 0;
}

/*
 * mark() state machine (paper Figure 8 L6/L12 + Sec. 4.3): a page keeps its
 * state across permission/COW updates (from == to), and virtual allocation
 * moves Invalid to one of the virtually-allocated states.  Everything else
 * needs unmap()/map() first.
 */
static bool corten_mark_legal(enum corten_page_state from,
			      enum corten_page_state to)
{
	if (from == to)
		return true;

	return from == CORTEN_INVALID &&
	       (to == CORTEN_PRIVATE_ANON || to == CORTEN_FILE_MAPPED ||
		to == CORTEN_SHARED_ANON);
}

/**
 * corten_query - see include/linux/corten.h.
 */
int corten_query(struct corten_txn *txn, unsigned long addr,
		 struct corten_pte_meta *out)
{
	struct corten_pte_meta *m = corten_txn_meta(txn, addr);

	if (IS_ERR(m))
		return PTR_ERR(m);

	if (m) {
		*out = *m;
	} else {
		out->state = CORTEN_INVALID;
		out->perm = 0;
		out->flags = 0;
		memset(out->__resv, 0, sizeof(out->__resv));
	}

	return 0;
}

/**
 * corten_map - see include/linux/corten.h.
 */
int corten_map(struct corten_txn *txn, unsigned long addr, struct page *page,
	       u8 perm, unsigned int flags)
{
	struct corten_pte_meta *m;
	int ret;

	if (unlikely(flags & ~CORTEN_MAP_ALL))
		return -EINVAL;
	if (unlikely(perm & ~CORTEN_PERM_ALL))
		return -EINVAL;
	if (unlikely(!page))
		return -EINVAL;
	if (unlikely(!txn->covering)) {
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	/* Validate the transition before allocating anything. */
	m = corten_txn_meta(txn, addr);
	if (IS_ERR(m))
		return PTR_ERR(m);
	if (m && m->state == CORTEN_MAPPED && !(flags & CORTEN_MAP_FORCE))
		return -EEXIST;

	ret = corten_meta_ensure_locked(txn->covering);
	if (unlikely(ret))
		return ret;

	m = corten_txn_meta(txn, addr);
	if (IS_ERR(m))
		return PTR_ERR(m);
	if (unlikely(!m))
		return -ENOMEM;

	/* 2b: metadata-layer state transition only.  M3 sync-writes the
	 * hardware PTE (set_pte_at + TLB bookkeeping) here, inside the same
	 * covering write lock, and records the page identity in __resv.
	 */
	m->state = CORTEN_MAPPED;
	m->perm = perm;
	m->flags = 0;

	return 0;
}

/**
 * corten_mark - see include/linux/corten.h.
 */
int corten_mark(struct corten_txn *txn, unsigned long start, unsigned long len,
		const struct corten_pte_meta *meta)
{
	unsigned long addr, end;
	int ret;

	if (unlikely(!meta))
		return -EINVAL;
	if (unlikely(meta->state <= CORTEN_INVALID ||
		     meta->state > CORTEN_SHARED_ANON))
		return -EINVAL;
	if (unlikely(meta->perm & ~CORTEN_PERM_ALL))
		return -EINVAL;
	if (unlikely(meta->flags & ~CORTEN_PF_ALL))
		return -EINVAL;
	/* Paper Sec. 4.3: the writable bit records the pre-fork writability.
	 * A logically writable shared page must carry it, otherwise a write
	 * fault would take the FOLL_FORCE COW-copy branch as if the page had
	 * been read-only before the fork, losing the recorded writability
	 * (see include/linux/corten.h).  The fault-side copy/re-enable logic
	 * itself is M5.
	 */
	if (unlikely((meta->perm & CORTEN_PERM_WRITE) &&
		     (meta->flags & CORTEN_PF_SHARED) &&
		     !(meta->flags & CORTEN_PF_WRITABLE)))
		return -EINVAL;

	ret = corten_txn_subrange(txn, start, len, &end);
	if (unlikely(ret))
		return ret;

	/* Validate the whole sub-range first: a transaction is all-or-nothing. */
	for (addr = start; addr < end; addr += PAGE_SIZE) {
		struct corten_pte_meta *m = corten_txn_meta(txn, addr);

		if (IS_ERR(m))
			return PTR_ERR(m);
		if (!corten_mark_legal(m ? m->state : CORTEN_INVALID,
				       meta->state))
			return -EINVAL;
	}

	ret = corten_meta_ensure_locked(txn->covering);
	if (unlikely(ret))
		return ret;

	for (addr = start; addr < end; addr += PAGE_SIZE) {
		struct corten_pte_meta *m = corten_txn_meta(txn, addr);

		if (IS_ERR(m))
			return PTR_ERR(m);
		if (unlikely(!m))
			return -ENOMEM;
		*m = *meta;
	}

	return 0;
}

/**
 * corten_unmap - see include/linux/corten.h.
 */
int corten_unmap(struct corten_txn *txn, unsigned long start,
		 unsigned long len)
{
	unsigned long addr, end;
	int ret;

	ret = corten_txn_subrange(txn, start, len, &end);
	if (unlikely(ret))
		return ret;

	/* Validate the whole sub-range first (paper: atomic transaction). */
	for (addr = start; addr < end; addr += PAGE_SIZE) {
		struct corten_pte_meta *m = corten_txn_meta(txn, addr);

		if (IS_ERR(m))
			return PTR_ERR(m);
		if (!m || m->state == CORTEN_INVALID)
			return -ENOENT;
	}

	for (addr = start; addr < end; addr += PAGE_SIZE) {
		struct corten_pte_meta *m = corten_txn_meta(txn, addr);

		if (IS_ERR(m))
			return PTR_ERR(m);
		if (unlikely(!m))
			return -ENOMEM;
		/* Dropping the physical page / swap-slot references
		 * recorded in the metadata payload is wired in by M3 (page
		 * refs) and M6 (swap).  2b flips the state back to Invalid
		 * and scrubs the payload bytes (__resv): the slot may be
		 * re-marked later and must not resurrect stale payload
		 * fields that the state machine no longer validates.
		 */
		m->state = CORTEN_INVALID;
		m->perm = 0;
		m->flags = 0;
		memset(m->__resv, 0, sizeof(m->__resv));
	}

	return 0;
}

/**
 * corten_unlock - see include/linux/corten.h.
 */
void corten_unlock(struct corten_txn *txn)
{
	corten_txn_finish(txn);
}

/* ---- debugfs introspection ------------------------------------------ */

static int corten_stats_show(struct seq_file *m, void *v)
{
	long arrays = atomic_long_read(&corten_nr_meta_arrays);

	seq_printf(m, "enabled             %d\n",
		   static_key_enabled(&corten_enabled_key));
	seq_printf(m, "ptdescs             %ld\n",
		   atomic_long_read(&corten_nr_ptdescs));
	seq_printf(m, "meta_arrays         %ld\n", arrays);
	seq_printf(m, "meta_bytes          %ld\n",
		   arrays * CORTEN_META_ARRAY_BYTES);
	seq_printf(m, "desc_alloc_fail     %ld\n",
		   atomic_long_read(&corten_nr_desc_alloc_fail));
	seq_printf(m, "meta_alloc_fail     %ld\n",
		   atomic_long_read(&corten_nr_meta_alloc_fail));
	seq_printf(m, "free_untracked      %ld\n",
		   atomic_long_read(&corten_nr_free_untracked));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(corten_stats);

/*
 * One line per live descriptor: pfn, level, covered window base, pins,
 * staleness, metadata-array presence and populated-children count.  Safe
 * against concurrent uninstall via RCU (kfree_rcu); entries erased during
 * the iteration simply do not show up.
 */
static int corten_dump_show(struct seq_file *m, void *v)
{
	unsigned long pfn = 0;
	struct corten_ptdesc *desc;

	seq_puts(m, "      pfn level             va_base refs stale meta children\n");

	rcu_read_lock();
	xa_for_each(&corten_ptdesc_xa, pfn, desc) {
		seq_printf(m, "%10lu %5u %016lx %4u %5u %4u %8u\n",
			   pfn, desc->level, READ_ONCE(desc->va_base),
			   refcount_read(&desc->refs), READ_ONCE(desc->stale),
			   READ_ONCE(desc->meta) ? 1 : 0,
			   READ_ONCE(desc->nr_children));
	}
	rcu_read_unlock();

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(corten_dump);

static int corten_txn_show(struct seq_file *m, void *v)
{
	seq_printf(m, "active             %ld\n",
		   atomic_long_read(&corten_nr_txns));
	seq_printf(m, "active_max         %ld\n",
		   atomic_long_read(&corten_nr_txns_max));

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(corten_txn);

static int __init corten_debugfs_init(void)
{
	struct dentry *dir;

	/* debugfs_init() is a core_initcall (fs/debugfs/inode.c), so by this
	 * late_initcall the debugfs core is registered; the debugfs helpers
	 * degrade to no-ops when CONFIG_DEBUG_FS is off.  Nothing here
	 * touches the corten static key, so there is no early-param-style
	 * ordering hazard.
	 */
	dir = debugfs_create_dir("corten", NULL);
	debugfs_create_file("stats", 0444, dir, NULL, &corten_stats_fops);
	debugfs_create_file("dump", 0444, dir, NULL, &corten_dump_fops);
	debugfs_create_file("txn", 0444, dir, NULL, &corten_txn_fops);

	return 0;
}
late_initcall(corten_debugfs_init);
