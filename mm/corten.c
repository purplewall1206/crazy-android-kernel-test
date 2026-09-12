// SPDX-License-Identifier: GPL-2.0
/*
 * CortenMM - page-descriptor based transactional memory management.
 *
 * This file implements the M2a data-structure skeleton:
 *
 *   - the PFN-keyed xarray of page descriptors (engineering adaptation of
 *     the paper's boot-time contiguous descriptor array, see
 *     include/linux/corten.h),
 *   - the PT-page lifecycle hooks wired into the x86 pte_alloc_one() and
 *     the two PTE-page free funnels,
 *   - the on-demand per-PTE metadata arrays,
 *   - the pure covering-page selection helpers used by the locking
 *     protocols (slice 2b) and by the KUnit tests,
 *   - the debugfs statistics under /sys/kernel/debug/corten/.
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

static int __init corten_setup_param(char *s)
{
	if (!s)
		return 0;
	if (!strcmp(s, "on")) {
		static_branch_enable(&corten_enabled_key);
		pr_info("corten: page descriptors enabled\n");
	}
	return 1;
}
__setup("corten=", corten_setup_param);

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
 * corten_meta_ensure - allocate the per-PTE metadata array on demand.
 * @desc: descriptor of the PT page, pinned by the caller.
 *
 * The array is 512 x 8B = 4K per PT page and is freed together with the
 * descriptor.  Allocation uses GFP_NOWAIT|__GFP_NOWARN: PT-page allocation
 * runs deep inside the fault path where sleeping on reclaim would risk the
 * reclaim-recursion deadlock, so callers must tolerate -ENOMEM and retry
 * where a failure is recoverable.
 */
int corten_meta_ensure(struct corten_ptdesc *desc)
{
	struct corten_pte_meta *meta;

	if (READ_ONCE(desc->meta))
		return 0;

	meta = kmalloc(CORTEN_META_ARRAY_BYTES,
		       GFP_NOWAIT | __GFP_NOWARN | __GFP_ZERO);
	if (!meta) {
		atomic_long_inc(&corten_nr_meta_alloc_fail);
		return -ENOMEM;
	}

	/* Publish under the descriptor lock so pinned readers see either
	 * NULL or a fully formed array.
	 */
	write_lock(&desc->lock);
	if (!desc->meta) {
		desc->meta = meta;
		meta = NULL;
		atomic_long_inc(&corten_nr_meta_arrays);
	}
	write_unlock(&desc->lock);

	/* The loser of the publish race drops its spare array (kfree(NULL)
	 * on the winner is a no-op).
	 */
	kfree(meta);

	return 0;
}

/**
 * corten_ptdesc_install - attach a page descriptor to a fresh PTE-level page.
 * @mm: owning address space (back-link; may be NULL for mock contexts).
 * @pte_page: the PT page being born.
 *
 * Called from the pte_alloc_one() funnel.  Failures are tolerated on
 * purpose: the PT page is already allocated and cannot be rolled back from
 * here, so an untracked PT page only means "CortenMM transactions on this
 * page must fall back" (slice 2b); it must never break the caller.  The
 * descriptor stays reachable until the paired corten_ptdesc_uninstall().
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
	 * a page (reported loudly -- this is an invariant of slice 2b).  The
	 * stale descriptor loses its base reference; outstanding pins keep
	 * it alive until their put().
	 */
	if (WARN_ON_ONCE(old)) {
		/* The replaced descriptor was never retired by its free
		 * funnel; drop its base reference and its counter entry.
		 */
		atomic_long_dec(&corten_nr_ptdescs);
		corten_ptdesc_put(old);
	}

	atomic_long_inc(&corten_nr_ptdescs);

	/* Best effort: slice 2b retries at first transaction use. */
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
 * out lookups that were in flight.
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

/*
 * Covering-page selection (paper Figure 5 L3-L8).  Pure arithmetic on the
 * x86-64 window masks, valid for both 4- and 5-level configurations: the
 * P4D window equals the PGD window when 5-level paging is inactive
 * (p4d folded), and the cascade then naturally stops at CORTEN_LEVEL_PGD.
 */

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

/* ---- debugfs statistics -------------------------------------------- */

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

static int __init corten_debugfs_init(void)
{
	struct dentry *dir;

	/* debugfs core registers before late_initcall, and the debugfs
	 * helpers degrade to no-ops when CONFIG_DEBUG_FS is off.
	 */
	dir = debugfs_create_dir("corten", NULL);
	debugfs_create_file("stats", 0444, dir, NULL, &corten_stats_fops);

	return 0;
}
late_initcall(corten_debugfs_init);
