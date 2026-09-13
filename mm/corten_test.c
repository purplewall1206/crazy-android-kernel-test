// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the CortenMM M2a data-structure skeleton, the M2b
 * CortenMMrw locking protocol and transaction API, and the M3a protocol
 * hardening (uninstall/write-lock interlock and hole ensure-alloc).
 *
 * Two test worlds:
 *
 *  - synthetic descriptor trees (corten_test_tree_*): multi-level chains of
 *    real, xarray-indexed descriptors driven through the generic protocol
 *    core (corten_txn_begin), no real process or page table needed -- this
 *    is what exercises the multi-level descent, the hole/stale handling,
 *    the ensure-alloc upgrade window, the uninstall interlock and the
 *    mutual-exclusion properties (paper Figure 5 + Sec. 3.3);
 *
 *  - a real x86 page table under a private mm (mm_alloc), driven through
 *    the public corten_lock_range() glue that navigates the untracked
 *    PGD/P4D/PUD/PMD levels.
 *
 * The corten_enabled_static() gate is deliberately not touched: tests run
 * fine on kernels booted with corten=off because they call the functions
 * below the gate (corten_ptdesc_install()/uninstall(), the protocol core,
 * the transaction operations).
 */
#include <kunit/test.h>
#include <linux/pgalloc.h>
#include <linux/atomic.h>
#include <linux/bottom_half.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>

#include "corten.h"

/* Exit action: retire the descriptor (if any) and return the PT page. */
static void corten_test_uninstall_free(void *ctx)
{
	struct page *page = ctx;

	corten_ptdesc_uninstall(page);
	__free_page(page);
}

/*
 * Allocate a PT page and install a descriptor for it, registered for
 * cleanup so assertions cannot leak either side.
 */
static struct page *corten_test_track_ptpage(struct kunit *test,
					     struct mm_struct *mm)
{
	struct page *page;

	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	kunit_add_action(test, corten_test_uninstall_free, page);

	KUNIT_ASSERT_EQ(test, 0, corten_ptdesc_install(mm, page));

	return page;
}

/* Fill every metadata entry with a state-dependent pattern. */
static void corten_test_fill_meta(struct corten_ptdesc *desc)
{
	int i;

	for (i = 0; i < CORTEN_PTES_PER_PT_PAGE; i++) {
		struct corten_pte_meta *m = &desc->meta[i];

		m->state = (i % 2) ? CORTEN_PRIVATE_ANON : CORTEN_MAPPED;
		m->perm = CORTEN_PERM_READ | (i % 2 ? CORTEN_PERM_WRITE :
						      CORTEN_PERM_EXEC);
		m->flags = (i % 3) ? CORTEN_PF_SHARED : CORTEN_PF_WRITABLE;
	}
}

/* Verify every metadata entry is back to the freshly-zeroed state. */
static void corten_test_expect_meta_cleared(struct kunit *test,
					    struct corten_ptdesc *desc)
{
	int i;

	for (i = 0; i < CORTEN_PTES_PER_PT_PAGE; i++) {
		const struct corten_pte_meta *m = &desc->meta[i];

		KUNIT_EXPECT_EQ(test, m->state, CORTEN_INVALID);
		KUNIT_EXPECT_EQ(test, m->perm, 0);
		KUNIT_EXPECT_EQ(test, m->flags, 0);
	}
}

static void corten_test_layout(struct kunit *test)
{
	/* The metadata array is exactly one page: 512 entries of 8B. */
	KUNIT_EXPECT_EQ(test, sizeof(struct corten_pte_meta), 8);
	KUNIT_EXPECT_EQ(test, CORTEN_PTES_PER_PT_PAGE, PTRS_PER_PTE);
	KUNIT_EXPECT_EQ(test, CORTEN_META_ARRAY_BYTES, PAGE_SIZE);

	/* The gate is expected to be off (default); skip rather than fail
	 * when the kernel was booted with corten=on -- slice 2b protocol
	 * tests legitimately run with the gate enabled.
	 */
	if (corten_enabled_static())
		kunit_skip(test, "corten=on boot");
}

static void corten_test_ptdesc_get_put(struct kunit *test)
{
	struct page *page = corten_test_track_ptpage(test, &init_mm);
	unsigned long pfn = page_to_pfn(page);
	struct corten_ptdesc *desc;
	struct corten_ptdesc *pin1;
	struct corten_ptdesc *pin2;

	desc = corten_ptdesc_get(pfn);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	KUNIT_EXPECT_EQ(test, desc->magic, CORTEN_PTDESC_MAGIC);
	KUNIT_EXPECT_EQ(test, desc->level, CORTEN_LEVEL_PTE);
	KUNIT_EXPECT_PTR_EQ(test, desc->mm, &init_mm);
	/* 1 base reference (the xarray entry) + this lookup pin. */
	KUNIT_EXPECT_EQ(test, refcount_read(&desc->refs), 2);

	/* Overlapping pins stack. */
	pin1 = corten_ptdesc_get(pfn);
	KUNIT_ASSERT_NOT_NULL(test, pin1);
	KUNIT_EXPECT_PTR_EQ(test, pin1, desc);
	pin2 = corten_ptdesc_get(pfn);
	KUNIT_ASSERT_NOT_NULL(test, pin2);
	KUNIT_EXPECT_EQ(test, refcount_read(&desc->refs), 4);
	corten_ptdesc_put(pin1);
	corten_ptdesc_put(pin2);
	KUNIT_EXPECT_EQ(test, refcount_read(&desc->refs), 2);

	/* Unknown PFNs do not resolve. */
	KUNIT_EXPECT_NULL(test, corten_ptdesc_get(pfn + 1));

	/*
	 * Uninstall while pinned: the lookup index is gone immediately but
	 * the pin keeps the descriptor readable until our put().
	 */
	corten_ptdesc_uninstall(page);
	KUNIT_EXPECT_NULL(test, corten_ptdesc_get(pfn));
	KUNIT_EXPECT_EQ(test, READ_ONCE(desc->stale), 1);
	KUNIT_EXPECT_EQ(test, desc->magic, CORTEN_PTDESC_MAGIC);
	corten_ptdesc_put(desc);
}

static void corten_test_meta_roundtrip(struct kunit *test)
{
	struct page *page = corten_test_track_ptpage(test, &init_mm);
	unsigned long pfn = page_to_pfn(page);
	struct corten_ptdesc *desc;
	int i;

	desc = corten_ptdesc_get(pfn);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	/* install() allocates the array eagerly; ensure() must accept it. */
	KUNIT_ASSERT_NOT_NULL(test, desc->meta);
	KUNIT_ASSERT_EQ(test, corten_meta_ensure(desc), 0);

	corten_test_fill_meta(desc);
	for (i = 0; i < CORTEN_PTES_PER_PT_PAGE; i++) {
		const struct corten_pte_meta *m = &desc->meta[i];

		KUNIT_EXPECT_EQ(test, m->state,
				(i % 2) ? CORTEN_PRIVATE_ANON : CORTEN_MAPPED);
		KUNIT_EXPECT_EQ(test, m->perm,
				CORTEN_PERM_READ | (i % 2 ? CORTEN_PERM_WRITE :
							   CORTEN_PERM_EXEC));
		KUNIT_EXPECT_EQ(test, m->flags,
				(i % 3) ? CORTEN_PF_SHARED :
					  CORTEN_PF_WRITABLE);
	}

	corten_ptdesc_put(desc);
}

static void corten_test_meta_reinit_cleared(struct kunit *test)
{
	struct page *page = alloc_page(GFP_KERNEL);
	unsigned long pfn;
	struct corten_ptdesc *desc;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	kunit_add_action(test, corten_test_uninstall_free, page);
	pfn = page_to_pfn(page);

	/* First lifetime: dirty every entry, then retire the descriptor. */
	KUNIT_ASSERT_EQ(test, 0, corten_ptdesc_install(&init_mm, page));
	desc = corten_ptdesc_get(pfn);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	KUNIT_ASSERT_NOT_NULL(test, desc->meta);
	corten_test_fill_meta(desc);
	corten_ptdesc_put(desc);
	corten_ptdesc_uninstall(page);
	KUNIT_EXPECT_NULL(test, corten_ptdesc_get(pfn));

	/*
	 * Second lifetime on the same PT page: a fresh descriptor with a
	 * freshly allocated (zeroed) metadata array -- no state may leak
	 * across the free.
	 */
	KUNIT_ASSERT_EQ(test, 0, corten_ptdesc_install(&init_mm, page));
	desc = corten_ptdesc_get(pfn);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	KUNIT_EXPECT_EQ(test, desc->magic, CORTEN_PTDESC_MAGIC);
	KUNIT_EXPECT_EQ(test, READ_ONCE(desc->va_base), 0UL);
	KUNIT_EXPECT_EQ(test, desc->nr_children, 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(desc->stale), 0);
	KUNIT_ASSERT_NOT_NULL(test, desc->meta);
	corten_test_expect_meta_cleared(test, desc);
	corten_ptdesc_put(desc);
}

/*
 * Covering-page selection (paper Figure 5 L3-L8) on synthetic addresses.
 * All constants come from the kernel's own PMD/PUD/P4D/PGDIR macros, so the
 * expectations hold under both 4- and 5-level configurations.
 */
static void corten_test_covering_level(struct kunit *test)
{
	unsigned long a, b;

	/* Range inside one 4K page: a PTE-level page covers it. */
	a = PMD_SIZE + PAGE_SIZE;
	KUNIT_EXPECT_EQ(test, corten_covering_level(a, a + PAGE_SIZE),
			CORTEN_LEVEL_PTE);
	/* Two 4K pages, still inside one 2M window: PTE-level. */
	KUNIT_EXPECT_EQ(test,
			corten_covering_level(a, a + 2 * PAGE_SIZE),
			CORTEN_LEVEL_PTE);

	/* Range crossing a 2M boundary inside one 1G window: the covering
	 * page is the PMD-level page holding both PTE pages.
	 */
	a = PMD_SIZE - PAGE_SIZE;
	b = PMD_SIZE + PAGE_SIZE;
	KUNIT_EXPECT_EQ(test, corten_covering_level(a, b), CORTEN_LEVEL_PMD);

	/* Range crossing a 1G boundary inside one P4D window: PUD-level. */
	a = PUD_SIZE - PAGE_SIZE;
	b = PUD_SIZE + PAGE_SIZE;
	KUNIT_EXPECT_EQ(test, corten_covering_level(a, b), CORTEN_LEVEL_PUD);

	/* Range crossing a P4D entry boundary: with 5-level paging the range
	 * stays inside one root window and the covering page is the
	 * P4D-level page; with 4-level paging the P4D window equals the
	 * root window, so the descent stops at the root itself.
	 */
	a = P4D_SIZE - PAGE_SIZE;
	b = P4D_SIZE + PAGE_SIZE;
	if ((a & PGDIR_MASK) == (b & PGDIR_MASK))
		KUNIT_EXPECT_EQ(test, corten_covering_level(a, b),
				CORTEN_LEVEL_P4D);
	else
		KUNIT_EXPECT_EQ(test, corten_covering_level(a, b),
				CORTEN_LEVEL_PGD);

	/* child_covers_range agrees with the descent at each level. */
	KUNIT_EXPECT_TRUE(test, corten_child_covers_range(a, a + PAGE_SIZE,
							  CORTEN_LEVEL_PMD));
	KUNIT_EXPECT_FALSE(test, corten_child_covers_range(PMD_SIZE - PAGE_SIZE,
							   PMD_SIZE + PAGE_SIZE,
							   CORTEN_LEVEL_PMD));
	KUNIT_EXPECT_TRUE(test, corten_child_covers_range(PMD_SIZE - PAGE_SIZE,
							  PMD_SIZE + PAGE_SIZE,
							  CORTEN_LEVEL_PUD));
	KUNIT_EXPECT_FALSE(test, corten_child_covers_range(a, a + PAGE_SIZE,
							   CORTEN_LEVEL_PTE));
}

static void corten_test_covering_base_index(struct kunit *test)
{
	unsigned long addr = PMD_SIZE + 5 * PAGE_SIZE + 123;

	/* A PTE-level page covers a PMD_SIZE-aligned window. */
	KUNIT_EXPECT_EQ(test, corten_covering_va_base(addr, CORTEN_LEVEL_PTE),
			addr & PMD_MASK);
	KUNIT_EXPECT_EQ(test, corten_covering_va_base(addr, CORTEN_LEVEL_PTE),
			PMD_SIZE);
	KUNIT_EXPECT_EQ(test, corten_covering_va_base(addr, CORTEN_LEVEL_PMD),
			addr & PUD_MASK);

	/* Slot indices match the hardware page-table offsets. */
	KUNIT_EXPECT_EQ(test, corten_slot_index(addr, CORTEN_LEVEL_PTE),
			(addr >> PAGE_SHIFT) & (PTRS_PER_PTE - 1));
	KUNIT_EXPECT_EQ(test, corten_slot_index(addr, CORTEN_LEVEL_PMD),
			(addr >> PMD_SHIFT) & (PTRS_PER_PMD - 1));
	KUNIT_EXPECT_EQ(test, corten_slot_index(addr, CORTEN_LEVEL_PUD),
			(addr >> PUD_SHIFT) & (PTRS_PER_PUD - 1));
	KUNIT_EXPECT_EQ(test, corten_slot_index(addr, CORTEN_LEVEL_PGD),
			pgd_index(addr));

	/* A PT page's base is the start of its own window. */
	KUNIT_EXPECT_EQ(test,
			corten_covering_va_base(
				corten_covering_va_base(addr, CORTEN_LEVEL_PTE),
				CORTEN_LEVEL_PTE),
			corten_covering_va_base(addr, CORTEN_LEVEL_PTE));
}

/* ---- synthetic descriptor trees -------------------------------------- */

#define CORTEN_TEST_TREE_MAX	8

struct corten_test_tree {
	struct page		*page[CORTEN_TEST_TREE_MAX];
	struct corten_ptdesc	*desc[CORTEN_TEST_TREE_MAX];
	unsigned long		va_base[CORTEN_TEST_TREE_MAX];
	enum corten_pt_level	level[CORTEN_TEST_TREE_MAX];
	int			parent[CORTEN_TEST_TREE_MAX];
	int			nr;
};

/* Destroy action: retire every descriptor and return every PT page. */
static void corten_test_tree_destroy(void *ctx)
{
	struct corten_test_tree *t = ctx;
	int i;

	for (i = 0; i < t->nr; i++) {
		if (t->desc[i]) {
			corten_ptdesc_uninstall(t->page[i]);
			corten_ptdesc_put(t->desc[i]);
			t->desc[i] = NULL;
		}
		if (t->page[i]) {
			__free_page(t->page[i]);
			t->page[i] = NULL;
		}
	}
	t->nr = 0;
}

/*
 * Add one synthetic PT page covering the window starting at @va_base, child
 * of node @parent (-1: root).  Must be called in root-first order so that
 * parents exist when children are linked.
 */
static void corten_test_tree_add(struct kunit *test, struct corten_test_tree *t,
				 unsigned long va_base,
				 enum corten_pt_level level, int parent)
{
	struct page *page;
	struct corten_ptdesc *desc;
	int idx = t->nr;

	KUNIT_ASSERT_LT(test, idx, CORTEN_TEST_TREE_MAX);
	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	KUNIT_ASSERT_EQ(test, 0, corten_ptdesc_install(&init_mm, page));
	desc = corten_ptdesc_get(page_to_pfn(page));
	KUNIT_ASSERT_NOT_NULL(test, desc);

	desc->va_base = va_base;
	corten_ptdesc_set_level(desc, level);
	t->page[idx] = page;
	t->desc[idx] = desc;
	t->va_base[idx] = va_base;
	t->level[idx] = level;
	t->parent[idx] = parent;
	if (parent >= 0)
		t->desc[parent]->nr_children++;
	t->nr++;
}

/* Does node @i's window contain @addr?  (Base bookkeeping mirrors the
 * descriptor so the ops below could be driven either way.)
 */
static bool corten_test_tree_covers(struct corten_test_tree *t, int i,
				    unsigned long addr)
{
	return corten_covering_va_base(addr, t->level[i]) == t->va_base[i];
}

/*
 * Tree view for the protocol core: root() finds the deepest unparented node
 * covering @addr (synthetic forests are single-rooted per VA region),
 * child() finds the child of @parent covering @addr.  Both return pinned
 * descriptors per the corten_tree_ops contract.
 */
static struct corten_ptdesc *corten_test_tree_root(void *ctx,
						   unsigned long addr)
{
	struct corten_test_tree *t = ctx;
	int best = -1;
	int i;

	for (i = 0; i < t->nr; i++) {
		if (t->parent[i] >= 0)
			continue;
		if (!corten_test_tree_covers(t, i, addr))
			continue;
		if (best < 0 || t->level[i] > t->level[best])
			best = i;
	}
	if (best < 0)
		return NULL;

	return corten_ptdesc_get(page_to_pfn(t->page[best]));
}

static struct corten_ptdesc *corten_test_tree_child(void *ctx,
						    struct corten_ptdesc *parent,
						    unsigned long addr)
{
	struct corten_test_tree *t = ctx;
	int pn = -1;
	int i;

	for (i = 0; i < t->nr; i++) {
		if (t->desc[i] == parent) {
			pn = i;
			break;
		}
	}
	if (pn < 0)
		return ERR_PTR(-EIO);

	for (i = 0; i < t->nr; i++) {
		if (t->parent[i] != pn)
			continue;
		if (t->level[i] != parent->level + 1)
			continue;
		if (!corten_test_tree_covers(t, i, addr))
			continue;
		return corten_ptdesc_get(page_to_pfn(t->page[i]));
	}

	/* No child page exists below @parent for @addr: a hole. */
	return NULL;
}

static const struct corten_tree_ops corten_test_tree_ops = {
	.root	= corten_test_tree_root,
	.child	= corten_test_tree_child,
};

/*
 * Standard 3-level test tree:
 *
 *   [0] PUD  window [0, P4D_SIZE)
 *     [1] PMD  window [0, PUD_SIZE)
 *       [3] PTE  window [0, PMD_SIZE)
 *       [4] PTE  window [PMD_SIZE, 2*PMD_SIZE)
 *     [2] PMD  window [PUD_SIZE, 2*PUD_SIZE)
 *       [5] PTE  window [PUD_SIZE, PUD_SIZE + PMD_SIZE)
 *
 * Node [2] deliberately has only one PTE child, leaving a hole below it.
 *
 * The tree is test-managed heap memory: KUnit runs cleanup actions outside
 * the test function's stack frame, so action contexts must never point at
 * the caller's locals.  Cleanup is LIFO, so registering the destroy action
 * before the tree is populated runs it before the kfree of the tree.
 */
static struct corten_test_tree *corten_test_tree_std(struct kunit *test)
{
	struct corten_test_tree *t;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);
	kunit_add_action(test, corten_test_tree_destroy, t);

	corten_test_tree_add(test, t, 0, CORTEN_LEVEL_PUD, -1);
	corten_test_tree_add(test, t, 0, CORTEN_LEVEL_PMD, 0);
	corten_test_tree_add(test, t, PUD_SIZE, CORTEN_LEVEL_PMD, 0);
	corten_test_tree_add(test, t, 0, CORTEN_LEVEL_PTE, 1);
	corten_test_tree_add(test, t, PMD_SIZE, CORTEN_LEVEL_PTE, 1);
	corten_test_tree_add(test, t, PUD_SIZE, CORTEN_LEVEL_PTE, 2);

	return t;
}

/*
 * Begin a transaction over [start, start+len) and assert the protocol
 * selected @want as the covering page at level @want_lvl.
 */
static void corten_test_expect_covering(struct kunit *test,
					struct corten_test_tree *t,
					unsigned long start, unsigned long len,
					int want, enum corten_pt_level want_lvl)
{
	struct corten_txn txn;
	int ret;

	ret = corten_txn_begin(&init_mm, start, start + len,
			       &corten_test_tree_ops, t, &txn);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, txn.covering, t->desc[want]);
	KUNIT_EXPECT_EQ(test, txn.level, want_lvl);
	/* The walk's path read locks were released; only the covering pin
	 * remains on top of the base + tree pins.
	 */
	KUNIT_EXPECT_EQ(test, txn.nr_path, 0);
	KUNIT_EXPECT_EQ(test, refcount_read(&txn.covering->refs), 3);
	corten_txn_finish(&txn);
	KUNIT_EXPECT_EQ(test, refcount_read(&t->desc[want]->refs), 2);
}

/* Covering selection on the synthetic tree: the walk must stop at the
 * lowest page that spans the whole range (paper Figure 5 L3/L7).
 */
static void corten_test_txn_covering(struct kunit *test)
{
	struct corten_test_tree *t;

	t = corten_test_tree_std(test);

	/* Range inside one 2M window: PTE-level page [3]. */
	corten_test_expect_covering(test, t, 8 * PAGE_SIZE, PAGE_SIZE,
				    3, CORTEN_LEVEL_PTE);
	/*
	 * Range crossing a 2M boundary within 1G: the PMD page [1] whose
	 * children are the two 2M windows involved.
	 */
	corten_test_expect_covering(test, t, PMD_SIZE - PAGE_SIZE,
				    2 * PAGE_SIZE, 1, CORTEN_LEVEL_PMD);
	/*
	 * Range crossing a 1G boundary within the PUD window: PUD page [0].
	 */
	corten_test_expect_covering(test, t, PUD_SIZE - PAGE_SIZE,
				    2 * PAGE_SIZE, 0, CORTEN_LEVEL_PUD);
	/* Same as the first case, under the second PMD page. */
	corten_test_expect_covering(test, t, PUD_SIZE + 8 * PAGE_SIZE,
				    PAGE_SIZE, 5, CORTEN_LEVEL_PTE);
}

/* A hole (missing intermediate PT page) reports -ENOENT and locks nothing;
 * M3 replaces this with ensure-alloc under the covering write lock.
 */
static void corten_test_txn_hole(struct kunit *test)
{
	struct corten_test_tree *t;
	struct corten_txn txn;
	int ret;

	t = corten_test_tree_std(test);

	/* Below PMD node [2], only the window at PUD_SIZE has a PTE page;
	 * a range deeper in its window hits the hole.
	 */
	ret = corten_txn_begin(&init_mm, PUD_SIZE + 4 * PMD_SIZE,
			       PUD_SIZE + 4 * PMD_SIZE + PAGE_SIZE,
			       &corten_test_tree_ops, t, &txn);
	KUNIT_EXPECT_EQ(test, ret, -ENOENT);
	KUNIT_EXPECT_NULL(test, txn.covering);
	KUNIT_EXPECT_EQ(test, txn.nr_path, 0);
	/* Nothing was left pinned by the failed walk. */
	KUNIT_EXPECT_EQ(test, refcount_read(&t->desc[2]->refs), 2);
	KUNIT_EXPECT_EQ(test, refcount_read(&t->desc[0]->refs), 2);
}

/* Stale descriptors (paper Figure 7: raced with PT-page removal) fail the
 * transaction entry with -EAGAIN so the caller can retry.
 */
static void corten_test_txn_stale_eagain(struct kunit *test)
{
	struct corten_test_tree *t;
	struct corten_txn txn;
	int ret;

	t = corten_test_tree_std(test);

	/* Stale at the covering level (root here is still the PUD). */
	WRITE_ONCE(t->desc[3]->stale, 1);
	ret = corten_txn_begin(&init_mm, 8 * PAGE_SIZE, 12 * PAGE_SIZE,
			       &corten_test_tree_ops, t, &txn);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_NULL(test, txn.covering);
	KUNIT_EXPECT_EQ(test, refcount_read(&t->desc[3]->refs), 2);
	WRITE_ONCE(t->desc[3]->stale, 0);

	/* Stale one level above the leaf: the walk pins the child, sees the
	 * flag under the read lock and unwinds.
	 */
	WRITE_ONCE(t->desc[1]->stale, 1);
	ret = corten_txn_begin(&init_mm, 8 * PAGE_SIZE, 12 * PAGE_SIZE,
			       &corten_test_tree_ops, t, &txn);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, refcount_read(&t->desc[1]->refs), 2);
	KUNIT_EXPECT_EQ(test, refcount_read(&t->desc[3]->refs), 2);
	WRITE_ONCE(t->desc[1]->stale, 0);

	/* After the removal completes (flag cleared), the retry succeeds. */
	ret = corten_txn_begin(&init_mm, 8 * PAGE_SIZE, 12 * PAGE_SIZE,
			       &corten_test_tree_ops, t, &txn);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, txn.covering, t->desc[3]);
	corten_txn_finish(&txn);
}

/* Range validation of the public entry point (no page table needed). */
static void corten_test_lock_range_args(struct kunit *test)
{
	struct corten_txn txn;

	KUNIT_EXPECT_EQ(test, -EINVAL,
			corten_lock_range(NULL, PAGE_SIZE, PAGE_SIZE, &txn));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			corten_lock_range(&init_mm, PAGE_SIZE, 0, &txn));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			corten_lock_range(&init_mm, PAGE_SIZE, PAGE_SIZE / 2,
					  &txn));
	KUNIT_EXPECT_EQ(test, -EINVAL,
			corten_lock_range(&init_mm, PAGE_SIZE + 1, PAGE_SIZE,
					  &txn));
	/* Crosses a 2M boundary: with only PTE-level descriptors tracked,
	 * there is no PMD-level covering page to lock -- reject rather than
	 * under-lock (M4+ turns the cursor into a per-page iterator).
	 */
	KUNIT_EXPECT_EQ(test, -EOPNOTSUPP,
			corten_lock_range(&init_mm, PMD_SIZE - PAGE_SIZE,
					  2 * PAGE_SIZE, &txn));
	/* Same for a 1G-crossing range. */
	KUNIT_EXPECT_EQ(test, -EOPNOTSUPP,
			corten_lock_range(&init_mm, PUD_SIZE - PAGE_SIZE,
					  2 * PAGE_SIZE, &txn));
	/* End address overflow. */
	KUNIT_EXPECT_EQ(test, -EOVERFLOW,
			corten_lock_range(&init_mm, ~0UL & PAGE_MASK,
					  2 * PAGE_SIZE, &txn));
}

/* Transaction state machine over the metadata array (paper Figure 4/8). */
static void corten_test_txn_state_machine(struct kunit *test)
{
	struct page *page;
	struct corten_test_tree *t;
	struct corten_txn txn;
	struct corten_pte_meta m, q;

	t = corten_test_tree_std(test);
	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	kunit_add_action(test, corten_test_uninstall_free, page);

	KUNIT_ASSERT_EQ(test, 0,
			corten_txn_begin(&init_mm, 8 * PAGE_SIZE,
					 12 * PAGE_SIZE,
					 &corten_test_tree_ops, t, &txn));

	/* mmap(): Invalid -> PrivateAnon (paper Figure 8 L6). */
	memset(&m, 0, sizeof(m));
	m.state = CORTEN_PRIVATE_ANON;
	m.perm = CORTEN_PERM_READ | CORTEN_PERM_WRITE | CORTEN_PERM_USER;
	KUNIT_EXPECT_EQ(test, 0, corten_mark(&txn, 8 * PAGE_SIZE, PAGE_SIZE,
					     &m));
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, 8 * PAGE_SIZE, &q));
	KUNIT_EXPECT_EQ(test, q.state, CORTEN_PRIVATE_ANON);
	KUNIT_EXPECT_EQ(test, q.perm, m.perm);
	KUNIT_EXPECT_EQ(test, q.flags, 0);

	/* Same-state mark: permission update (mprotect). */
	m.perm = CORTEN_PERM_READ;
	KUNIT_EXPECT_EQ(test, 0, corten_mark(&txn, 8 * PAGE_SIZE, PAGE_SIZE,
					     &m));
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, 8 * PAGE_SIZE, &q));
	KUNIT_EXPECT_EQ(test, q.perm, CORTEN_PERM_READ);

	/* page fault: PrivateAnon -> Mapped (paper Figure 8 L22). */
	KUNIT_EXPECT_EQ(test, 0, corten_map(&txn, 8 * PAGE_SIZE, page,
					    CORTEN_PERM_READ |
					    CORTEN_PERM_WRITE |
					    CORTEN_PERM_USER, 0));
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, 8 * PAGE_SIZE, &q));
	KUNIT_EXPECT_EQ(test, q.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, q.flags, 0);

	/* Mapped -> Mapped without force: -EEXIST; with force: ok. */
	KUNIT_EXPECT_EQ(test, -EEXIST, corten_map(&txn, 8 * PAGE_SIZE, page,
						  CORTEN_PERM_READ, 0));
	KUNIT_EXPECT_EQ(test, 0, corten_map(&txn, 8 * PAGE_SIZE, page,
					    CORTEN_PERM_READ,
					    CORTEN_MAP_FORCE));

	/* munmap(): Mapped -> Invalid; Invalid -> unmap: -ENOENT. */
	KUNIT_EXPECT_EQ(test, 0, corten_unmap(&txn, 8 * PAGE_SIZE, PAGE_SIZE));
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, 8 * PAGE_SIZE, &q));
	KUNIT_EXPECT_EQ(test, q.state, CORTEN_INVALID);
	KUNIT_EXPECT_EQ(test, q.perm, 0);
	KUNIT_EXPECT_EQ(test, q.flags, 0);
	KUNIT_EXPECT_EQ(test, -ENOENT,
			corten_unmap(&txn, 8 * PAGE_SIZE, PAGE_SIZE));

	/* Illegal state transitions (each rejected, nothing written). */
	m.state = CORTEN_INVALID;
	KUNIT_EXPECT_EQ(test, -EINVAL, corten_mark(&txn, 8 * PAGE_SIZE,
						   PAGE_SIZE, &m));
	m.state = CORTEN_MAPPED;	/* Invalid -> Mapped: use map() */
	KUNIT_EXPECT_EQ(test, -EINVAL, corten_mark(&txn, 8 * PAGE_SIZE,
						   PAGE_SIZE, &m));
	m.state = CORTEN_SWAPPED;	/* Invalid -> Swapped: not a mark */
	KUNIT_EXPECT_EQ(test, -EINVAL, corten_mark(&txn, 8 * PAGE_SIZE,
						   PAGE_SIZE, &m));
	m.state = CORTEN_FILE_MAPPED;
	KUNIT_EXPECT_EQ(test, 0, corten_mark(&txn, 8 * PAGE_SIZE, PAGE_SIZE,
					     &m));
	m.state = CORTEN_PRIVATE_ANON;	/* resident identity change */
	KUNIT_EXPECT_EQ(test, -EINVAL, corten_mark(&txn, 8 * PAGE_SIZE,
						   PAGE_SIZE, &m));

	/* COW bits (paper Sec. 4.3): a logically writable shared page must
	 * remember it was writable before the fork read-only protection.
	 */
	m.state = CORTEN_FILE_MAPPED;
	m.perm = CORTEN_PERM_READ;
	m.flags = CORTEN_PF_SHARED;
	KUNIT_EXPECT_EQ(test, 0, corten_mark(&txn, 8 * PAGE_SIZE, PAGE_SIZE,
					     &m));
	m.perm = CORTEN_PERM_READ | CORTEN_PERM_WRITE;
	m.flags = CORTEN_PF_SHARED | CORTEN_PF_WRITABLE;
	KUNIT_EXPECT_EQ(test, 0, corten_mark(&txn, 8 * PAGE_SIZE, PAGE_SIZE,
					     &m));
	m.flags = CORTEN_PF_SHARED;	/* writable but "was read-only"? */
	KUNIT_EXPECT_EQ(test, -EINVAL, corten_mark(&txn, 8 * PAGE_SIZE,
						   PAGE_SIZE, &m));
	m.flags = CORTEN_PF_ALL + 1;	/* unknown flag bit */
	KUNIT_EXPECT_EQ(test, -EINVAL, corten_mark(&txn, 8 * PAGE_SIZE,
						   PAGE_SIZE, &m));
	m.flags = 0;
	m.perm = CORTEN_PERM_ALL + 1;	/* unknown perm bit */
	KUNIT_EXPECT_EQ(test, -EINVAL, corten_mark(&txn, 8 * PAGE_SIZE,
						   PAGE_SIZE, &m));

	/* Range checks: outside the locked range, misaligned, overshooting. */
	m.state = CORTEN_FILE_MAPPED;
	m.perm = CORTEN_PERM_READ;
	KUNIT_EXPECT_EQ(test, -ERANGE, corten_query(&txn, 0, &q));
	KUNIT_EXPECT_EQ(test, -ERANGE, corten_query(&txn, 12 * PAGE_SIZE, &q));
	KUNIT_EXPECT_EQ(test, -EINVAL, corten_query(&txn, 8 * PAGE_SIZE + 1,
						    &q));
	KUNIT_EXPECT_EQ(test, -ERANGE, corten_mark(&txn, 8 * PAGE_SIZE,
						   5 * PAGE_SIZE, &m));
	KUNIT_EXPECT_EQ(test, -ERANGE, corten_unmap(&txn, 4 * PAGE_SIZE,
						    PAGE_SIZE));

	corten_unlock(&txn);
	KUNIT_EXPECT_NULL(test, txn.covering);
}

/* Atomicity of the validate-then-apply loops: one bad page in a sub-range
 * leaves every page untouched.
 */
static void corten_test_txn_atomic_validate(struct kunit *test)
{
	struct corten_test_tree *t;
	struct corten_txn txn;
	struct corten_pte_meta m, q;

	t = corten_test_tree_std(test);

	KUNIT_ASSERT_EQ(test, 0,
			corten_txn_begin(&init_mm, 8 * PAGE_SIZE,
					 16 * PAGE_SIZE,
					 &corten_test_tree_ops, t, &txn));

	memset(&m, 0, sizeof(m));
	m.state = CORTEN_PRIVATE_ANON;
	m.perm = CORTEN_PERM_READ;
	/* Both pages valid: unmap succeeds and clears both. */
	KUNIT_EXPECT_EQ(test, 0, corten_mark(&txn, 8 * PAGE_SIZE,
					     2 * PAGE_SIZE, &m));
	KUNIT_EXPECT_EQ(test, 0, corten_unmap(&txn, 8 * PAGE_SIZE,
					      2 * PAGE_SIZE));
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, 12 * PAGE_SIZE, &q));
	KUNIT_EXPECT_EQ(test, q.state, CORTEN_INVALID);

	/* Re-mark only the first page, then unmap both: the second page is
	 * Invalid, so the whole unmap fails and the first page must still
	 * hold its state.
	 */
	KUNIT_EXPECT_EQ(test, 0, corten_mark(&txn, 8 * PAGE_SIZE, PAGE_SIZE,
					     &m));
	KUNIT_EXPECT_EQ(test, -ENOENT, corten_unmap(&txn, 8 * PAGE_SIZE,
						    2 * PAGE_SIZE));
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, 8 * PAGE_SIZE, &q));
	KUNIT_EXPECT_EQ(test, q.state, CORTEN_PRIVATE_ANON);
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, 12 * PAGE_SIZE, &q));
	KUNIT_EXPECT_EQ(test, q.state, CORTEN_INVALID);

	corten_unlock(&txn);
}

/* Real x86 page table under a private mm, driven through the public
 * corten_lock_range() glue: covering selection, hole, untracked fallback,
 * stale -> -EAGAIN and the metadata roundtrip.
 */
struct corten_test_real_ctx {
	struct mm_struct *mm;
	pud_t *pud;
	pmd_t *pmd;
	struct page *pte_page;		/* tracked PT page */
	struct page *pte_page2;		/* untracked PT page */
};

static void corten_test_real_destroy(void *ctx)
{
	struct corten_test_real_ctx *r = ctx;

	if (r->pte_page) {
		corten_ptdesc_uninstall(r->pte_page);
		__free_page(r->pte_page);
	}
	if (r->pte_page2)
		__free_page(r->pte_page2);
	/*
	 * Balance the pgtables_bytes accounting that __pud_alloc()/
	 * __pmd_alloc() did: the raw pud_free()/pmd_free() here bypass the
	 * TLB-funnel paths where mm_dec_nr_puds()/mm_dec_nr_pmds() normally
	 * run.  The manually populated PTE pages were never accounted, so
	 * they need no decrement.
	 */
	if (r->pmd) {
		mm_dec_nr_pmds(r->mm);
		pmd_free(r->mm, r->pmd);
	}
	if (r->pud) {
		mm_dec_nr_puds(r->mm);
		pud_free(r->mm, r->pud);
	}
	if (r->mm)
		mmput(r->mm);
}

static void corten_test_txn_real_glue(struct kunit *test)
{
	/*
	 * The context is test-managed heap memory: KUnit runs cleanup
	 * actions outside the test function's stack frame.
	 *
	 * 4G-aligned base: its pud/pmd slot indices are 0, so the pointers
	 * returned by pud_alloc()/pmd_alloc() are the page bases the
	 * page-aligned BUG_ONs in pud_free()/pmd_free() demand.
	 */
	struct corten_test_real_ctx *r;
	unsigned long base = 0;
	unsigned long addr = base + PAGE_SIZE;
	unsigned long addr_hole = base + 4 * PMD_SIZE;
	unsigned long addr_untracked = base + 6 * PMD_SIZE;
	struct corten_txn txn;
	struct corten_pte_meta m, q;
	struct corten_ptdesc *desc;
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	int ret;

	r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r);
	kunit_add_action(test, corten_test_real_destroy, r);

	r->mm = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r->mm);

	pgdp = pgd_offset(r->mm, addr);
	p4dp = p4d_alloc(r->mm, pgdp, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, p4dp);
	pudp = pud_alloc(r->mm, p4dp, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pudp);
	r->pud = pudp;
	pmdp = pmd_alloc(r->mm, pudp, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pmdp);
	r->pmd = pmdp;

	/*
	 * Install the PT page for @addr.  On a corten=on kernel the
	 * pte_alloc_one() hook would have tracked it; the tests run under
	 * the gate too, so install explicitly (same function the hook
	 * calls).  The neighboring windows are populated without a
	 * descriptor (untracked) or not at all (hole).
	 */
	r->pte_page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r->pte_page);
	pmd_populate(r->mm, pmdp, r->pte_page);
	KUNIT_ASSERT_EQ(test, 0, corten_ptdesc_install(r->mm, r->pte_page));

	r->pte_page2 = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r->pte_page2);
	pmd_populate(r->mm, pmd_offset(pudp, addr_untracked), r->pte_page2);

	/* Happy path: lock, mark (mmap), map (fault), query, unmap. */
	ret = corten_lock_range(r->mm, addr, PAGE_SIZE, &txn);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, txn.level, CORTEN_LEVEL_PTE);
	KUNIT_EXPECT_EQ(test, txn.covering->va_base, addr & PMD_MASK);
	KUNIT_EXPECT_EQ(test, refcount_read(&txn.covering->refs), 2);

	memset(&m, 0, sizeof(m));
	m.state = CORTEN_PRIVATE_ANON;
	m.perm = CORTEN_PERM_READ | CORTEN_PERM_WRITE | CORTEN_PERM_USER;
	KUNIT_EXPECT_EQ(test, 0, corten_mark(&txn, addr, PAGE_SIZE, &m));
	KUNIT_EXPECT_EQ(test, 0, corten_map(&txn, addr, r->pte_page, m.perm,
					    0));
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, addr, &q));
	KUNIT_EXPECT_EQ(test, q.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, 0, corten_unmap(&txn, addr, PAGE_SIZE));
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, addr, &q));
	KUNIT_EXPECT_EQ(test, q.state, CORTEN_INVALID);
	corten_unlock(&txn);

	/* Hole: the pud exists but this pmd slot was never populated. */
	ret = corten_lock_range(r->mm, addr_hole, PAGE_SIZE, &txn);
	KUNIT_EXPECT_EQ(test, ret, -ENOENT);

	/* Untracked PT page: present in the hardware tree but without a
	 * descriptor -- transactions must fall back.
	 */
	ret = corten_lock_range(r->mm, addr_untracked, PAGE_SIZE, &txn);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);

	/* Stale -> -EAGAIN (paper Figure 7), then a successful retry. */
	desc = corten_ptdesc_get(page_to_pfn(r->pte_page));
	KUNIT_ASSERT_NOT_NULL(test, desc);
	WRITE_ONCE(desc->stale, 1);
	ret = corten_lock_range(r->mm, addr, PAGE_SIZE, &txn);
	KUNIT_EXPECT_EQ(test, ret, -EAGAIN);
	WRITE_ONCE(desc->stale, 0);
	corten_ptdesc_put(desc);
	ret = corten_lock_range(r->mm, addr, PAGE_SIZE, &txn);
	KUNIT_ASSERT_EQ(test, ret, 0);
	corten_unlock(&txn);
}

/*
 * Mutual exclusion on synthetic trees (paper Sec. 3.3): two kthreads run
 * transactions; overlapping ranges serialize on the covering descriptor
 * (the in-critical-section counter must never exceed 1), disjoint ranges
 * run in parallel (the counter must reach 2).
 */
struct corten_test_mutex_ctx {
	struct corten_test_tree *tree;
	atomic_t in_crit;
	atomic_t max_crit;
	atomic_t violations;
	atomic_t op_errors;
	atomic_t lock_errors;
	int expect_exclusive;	/* overlap test: in_crit must stay at 1 */
	int iters;
	struct completion done[2];
};

struct corten_test_worker {
	struct corten_test_mutex_ctx *c;
	int id;
	unsigned long start;
	unsigned long len;
};

/* Park inside the critical section long enough that a truly parallel
 * disjoint transaction must be observed.
 */
static void corten_test_hold_room(void)
{
	u64 until = ktime_get_mono_fast_ns() + 100 * NSEC_PER_USEC;

	while (ktime_get_mono_fast_ns() < until)
		cpu_relax();
}

static int corten_test_mutex_worker(void *data)
{
	struct corten_test_worker *w = data;
	struct corten_test_mutex_ctx *c = w->c;
	int i;

	for (i = 0; i < c->iters; i++) {
		struct corten_txn txn;
		struct corten_pte_meta m = { };
		int now, seen, ret;

		ret = corten_txn_begin(&init_mm, w->start, w->start + w->len,
				       &corten_test_tree_ops, c->tree, &txn);
		if (ret) {
			atomic_inc(&c->lock_errors);
			continue;
		}

		/* Critical section: only reachable while holding the
		 * covering write lock.
		 */
		now = atomic_inc_return(&c->in_crit);
		seen = atomic_read(&c->max_crit);
		while (now > seen &&
		       !atomic_try_cmpxchg(&c->max_crit, &seen, now))
			;
		if (now > 1 && c->expect_exclusive)
			atomic_inc(&c->violations);
		corten_test_hold_room();

		/* Real transaction work under the write lock: mmap-mark,
		 * fault-map, munmap (all legal transitions).  The struct
		 * page argument is only validated non-NULL in 2b; reuse a
		 * PT page of the tree.
		 */
		m.state = CORTEN_PRIVATE_ANON;
		m.perm = CORTEN_PERM_READ | CORTEN_PERM_WRITE |
			 CORTEN_PERM_USER;
		if (corten_mark(&txn, w->start, w->len, &m))
			atomic_inc(&c->op_errors);
		if (corten_map(&txn, w->start, c->tree->page[0], m.perm, 0))
			atomic_inc(&c->op_errors);
		if (corten_unmap(&txn, w->start, w->len))
			atomic_inc(&c->op_errors);

		/* Leave the instrumentation window only after all the
		 * transaction operations have run, so they are covered by
		 * the exclusivity check and not just the bare lock hold.
		 * The decrement must happen while the covering write lock
		 * is still held: once corten_unlock() releases it the
		 * successor's begin can complete and increment in_crit
		 * before this thread's decrement lands, which would count
		 * as a bogus overlap.
		 */
		atomic_dec(&c->in_crit);

		corten_unlock(&txn);
	}

	complete(&c->done[w->id]);
	while (!kthread_should_stop())
		schedule_timeout_idle(1);

	return 0;
}

/*
 * Runs the two-worker mutual-exclusion scenario.  Returns true when the
 * environment cannot honour the concurrency placement (fewer than 3 online
 * CPUs, or the affinity request is refused); the caller kunit_skips in that
 * case instead of silently measuring a serialized run that would pass
 * vacuously.
 */
static bool corten_test_mutex_run(struct kunit *test,
				  struct corten_test_mutex_ctx *c,
				  unsigned long start0, unsigned long start1)
{
	struct corten_test_worker workers[2];
	struct task_struct *tsk[2] = { NULL, NULL };
	int i;

	if (num_online_cpus() < 3)
		return true;
	c->iters = 200;

	/* The workers live on this stack; corten_test_mutex_run() does not
	 * return until both threads have parked in kthread_should_stop().
	 */
	for (i = 0; i < 2; i++) {
		workers[i].c = c;
		workers[i].id = i;
		workers[i].len = 4 * PAGE_SIZE;
		init_completion(&c->done[i]);
	}
	workers[0].start = start0;
	workers[1].start = start1;

	tsk[0] = kthread_run(corten_test_mutex_worker, &workers[0],
			     "corten_kunit_0");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tsk[0]);
	tsk[1] = kthread_run(corten_test_mutex_worker, &workers[1],
			     "corten_kunit_1");
	if (IS_ERR(tsk[1])) {
		kthread_stop(tsk[0]);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tsk[1]);
	}

	/* Spread the workers over two CPUs (1 and 2) so disjoint
	 * transactions can genuinely overlap; refuse to report a result
	 * when the placement cannot be established.
	 */
	for (i = 0; i < 2; i++) {
		if (set_cpus_allowed_ptr(tsk[i], cpumask_of(i + 1))) {
			kthread_stop(tsk[0]);
			kthread_stop(tsk[1]);
			return true;
		}
	}

	for (i = 0; i < 2; i++)
		wait_for_completion(&c->done[i]);
	for (i = 0; i < 2; i++)
		kthread_stop(tsk[i]);

	KUNIT_EXPECT_EQ(test, atomic_read(&c->lock_errors), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&c->op_errors), 0);

	return false;
}

static void corten_test_txn_mutex_overlap(struct kunit *test)
{
	struct corten_test_mutex_ctx *c;

	c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, c);
	c->tree = corten_test_tree_std(test);

	/* The workers' ranges ([8P,12P) and [10P,14P)) overlap and share the
	 * same covering descriptor -> strictly serialized: the critical
	 * section must never hold both threads.
	 */
	c->expect_exclusive = 1;
	if (corten_test_mutex_run(test, c, 8 * PAGE_SIZE, 10 * PAGE_SIZE))
		kunit_skip(test, "need 3 online CPUs for worker placement");

	KUNIT_EXPECT_EQ(test, atomic_read(&c->violations), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&c->max_crit), 1);
}

static void corten_test_txn_mutex_disjoint(struct kunit *test)
{
	struct corten_test_mutex_ctx *c;

	c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, c);
	c->tree = corten_test_tree_std(test);

	/* Disjoint 2M windows -> different covering descriptors -> the
	 * transactions do not contend (the paper's core selling point).
	 */
	if (corten_test_mutex_run(test, c, 8 * PAGE_SIZE,
				  PMD_SIZE + 8 * PAGE_SIZE))
		kunit_skip(test, "need 3 online CPUs for worker placement");

	KUNIT_EXPECT_EQ(test, atomic_read(&c->violations), 0);
	KUNIT_EXPECT_GE(test, atomic_read(&c->max_crit), 2);
}

/* Allocation-failure injection (review gap: the GFP_NOWAIT failure paths
 * of the descriptor and metadata allocators were untested).
 */
static void corten_test_fail_alloc(struct kunit *test)
{
	struct page *page;
	struct page *mappage;
	struct corten_test_tree *t;
	struct corten_txn txn;
	struct corten_pte_meta *old_meta, q;
	int ret;

	/* Descriptor allocation failure: nothing is published for the PFN,
	 * and a disarmed retry succeeds.
	 */
	corten_test_inject_alloc_fail(1);
	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	kunit_add_action(test, corten_test_uninstall_free, page);
	KUNIT_EXPECT_EQ(test, -ENOMEM, corten_ptdesc_install(&init_mm, page));
	KUNIT_EXPECT_NULL(test, corten_ptdesc_get(page_to_pfn(page)));
	corten_test_inject_alloc_fail(0);
	KUNIT_ASSERT_EQ(test, 0, corten_ptdesc_install(&init_mm, page));
	corten_ptdesc_uninstall(page);

	/* Metadata allocation failure inside a transaction: corten_map()
	 * reports -ENOMEM, the transaction stays locked and usable, and a
	 * disarmed retry allocates and applies.
	 */
	t = corten_test_tree_std(test);
	mappage = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mappage);

	old_meta = t->desc[3]->meta;
	KUNIT_ASSERT_NOT_NULL(test, old_meta);
	t->desc[3]->meta = NULL;
	kfree(old_meta);

	KUNIT_ASSERT_EQ(test, 0,
			corten_txn_begin(&init_mm, 8 * PAGE_SIZE,
					 12 * PAGE_SIZE,
					 &corten_test_tree_ops, t, &txn));
	corten_test_inject_alloc_fail(1);
	ret = corten_map(&txn, 8 * PAGE_SIZE, mappage, CORTEN_PERM_READ, 0);
	KUNIT_EXPECT_EQ(test, ret, -ENOMEM);
	corten_test_inject_alloc_fail(0);
	ret = corten_map(&txn, 8 * PAGE_SIZE, mappage, CORTEN_PERM_READ, 0);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, 8 * PAGE_SIZE, &q));
	KUNIT_EXPECT_EQ(test, q.state, CORTEN_MAPPED);
	corten_unlock(&txn);
}

/* ---- M3a: ensure-alloc (paper Figure 5 L5') -------------------------- */

/*
 * Synthetic hole context: a parent descriptor with a missing child plus a
 * pool of PRE-ALLOCATED PT pages that the @alloc callback installs.  All
 * pool state below is accessed under @parent's descriptor lock (the
 * read/walk side through corten_test_hole_child(), the write/alloc side
 * through corten_test_hole_alloc()), which is what makes this safe for the
 * concurrent test; pre-allocating keeps the callback inside its
 * no-sleeping contract.
 */
#define CORTEN_TEST_HOLE_POOL	2

struct corten_test_hole_ctx {
	struct corten_test_tree *t;
	struct corten_ptdesc	*parent;	/* the node with the hole */
	struct page		*pool[CORTEN_TEST_HOLE_POOL];
	struct corten_ptdesc	*desc[CORTEN_TEST_HOLE_POOL];
	int			pool_nr;
	int			next;		/* handed out so far */
	int			installed;	/* children actually created */
};

/* Cleanup action: retire the pool descriptors and return the PT pages. */
static void corten_test_hole_destroy(void *ctx)
{
	struct corten_test_hole_ctx *h = ctx;
	int i;

	for (i = 0; i < CORTEN_TEST_HOLE_POOL; i++) {
		if (h->desc[i]) {
			corten_ptdesc_uninstall(h->pool[i]);
			corten_ptdesc_put(h->desc[i]);
			h->desc[i] = NULL;
		}
		if (h->pool[i]) {
			__free_page(h->pool[i]);
			h->pool[i] = NULL;
		}
	}
}

/*
 * Wire a hole context for the child slot of tree node @parent below
 * @pool_nr pre-created PT pages (0 is valid: every allocation fails).
 * Heap-allocated: kunit cleanup actions outlive this stack frame.
 */
static struct corten_test_hole_ctx *
corten_test_hole_init(struct kunit *test, struct corten_test_tree *t,
		      int parent, int pool_nr)
{
	struct corten_test_hole_ctx *h;
	int i;

	h = kunit_kzalloc(test, sizeof(*h), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, h);
	kunit_add_action(test, corten_test_hole_destroy, h);

	h->t = t;
	h->parent = t->desc[parent];
	h->pool_nr = pool_nr;
	KUNIT_ASSERT_LE(test, pool_nr, CORTEN_TEST_HOLE_POOL);

	for (i = 0; i < pool_nr; i++) {
		struct page *page = alloc_page(GFP_KERNEL);

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
		h->pool[i] = page;
		KUNIT_ASSERT_EQ(test, 0, corten_ptdesc_install(&init_mm, page));
		h->desc[i] = corten_ptdesc_get(page_to_pfn(page));
		KUNIT_ASSERT_NOT_NULL(test, h->desc[i]);
	}

	return h;
}

static struct corten_ptdesc *corten_test_hole_root(void *ctx,
						   unsigned long addr)
{
	struct corten_test_hole_ctx *h = ctx;

	return corten_test_tree_root(h->t, addr);
}

static struct corten_ptdesc *corten_test_hole_child(void *ctx,
						    struct corten_ptdesc *parent,
						    unsigned long addr)
{
	struct corten_test_hole_ctx *h = ctx;
	int i, n;

	/* Children installed by ensure-alloc live in the hole pool; both
	 * this read and the allocator's write are serialized by @parent's
	 * descriptor lock.
	 */
	if (parent == h->parent) {
		n = READ_ONCE(h->next);
		for (i = 0; i < n; i++) {
			if (corten_covering_va_base(addr,
						   h->desc[i]->level) ==
			    h->desc[i]->va_base)
				return corten_ptdesc_get(
						page_to_pfn(h->pool[i]));
		}
		return NULL;
	}

	return corten_test_tree_child(h->t, parent, addr);
}

static struct corten_ptdesc *corten_test_hole_alloc(void *ctx,
						    struct corten_ptdesc *parent,
						    unsigned long addr)
{
	struct corten_test_hole_ctx *h = ctx;
	struct corten_ptdesc *desc;

	/* Called with @parent write-locked (the ops contract): the pool
	 * hand-out and bookkeeping below are exclusive.
	 */
	if (h->next >= h->pool_nr)
		return ERR_PTR(-ENOMEM);

	desc = h->desc[h->next];
	desc->va_base = corten_covering_va_base(addr, CORTEN_LEVEL_PTE);
	corten_ptdesc_set_level(desc, CORTEN_LEVEL_PTE);
	h->next++;
	h->installed++;
	parent->nr_children++;

	/* Handed out pinned and unlocked: the walk read-locks it (and
	 * stale-checks it) like any other child.
	 */
	return corten_ptdesc_get(page_to_pfn(h->pool[h->next - 1]));
}

static const struct corten_tree_ops corten_test_hole_ops = {
	.root	= corten_test_hole_root,
	.child	= corten_test_hole_child,
	.alloc	= corten_test_hole_alloc,
};

/* A view that declines to allocate: the real x86 view's 3a stance (its
 * ensure-alloc must be wired from inside the fault path, slice 3b).
 */
static struct corten_ptdesc *corten_test_alloc_decline(void *ctx,
						       struct corten_ptdesc *parent,
						       unsigned long addr)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static const struct corten_tree_ops corten_test_noalloc_ops = {
	.root	= corten_test_tree_root,
	.child	= corten_test_tree_child,
	.alloc	= corten_test_alloc_decline,
};

/* Ensure-alloc success path: the covering page is the freshly created
 * child, it is fully usable inside the same transaction, the parent's
 * child accounting moves, and a second descent does not allocate again.
 */
static void corten_test_txn_hole_fill(struct kunit *test)
{
	struct corten_test_hole_ctx *h, *h_empty;
	struct corten_test_tree *t;
	struct corten_txn txn;
	struct corten_pte_meta m, q;
	unsigned long start = PUD_SIZE + 4 * PMD_SIZE;
	int children_before;
	int ret;

	t = corten_test_tree_std(test);
	h = corten_test_hole_init(test, t, 2, CORTEN_TEST_HOLE_POOL);
	/* PMD[2] already has one static PTE child; the fill adds one. */
	children_before = h->parent->nr_children;

	/* Fill: begin succeeds and the new PTE page is the covering. */
	KUNIT_ASSERT_EQ(test, 0,
			corten_txn_begin(&init_mm, start, start + PAGE_SIZE,
					 &corten_test_hole_ops, h, &txn));
	KUNIT_EXPECT_PTR_EQ(test, txn.covering, h->desc[0]);
	KUNIT_EXPECT_EQ(test, txn.level, CORTEN_LEVEL_PTE);
	KUNIT_EXPECT_EQ(test, txn.covering->va_base, start);
	KUNIT_EXPECT_EQ(test, h->installed, 1);
	KUNIT_EXPECT_EQ(test, h->parent->nr_children, children_before + 1);

	/* The freshly allocated child is a fully usable covering page. */
	memset(&m, 0, sizeof(m));
	m.state = CORTEN_PRIVATE_ANON;
	m.perm = CORTEN_PERM_READ | CORTEN_PERM_WRITE | CORTEN_PERM_USER;
	KUNIT_EXPECT_EQ(test, 0, corten_mark(&txn, start, PAGE_SIZE, &m));
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, start, &q));
	KUNIT_EXPECT_EQ(test, q.state, CORTEN_PRIVATE_ANON);
	KUNIT_EXPECT_EQ(test, 0, corten_unmap(&txn, start, PAGE_SIZE));
	corten_unlock(&txn);

	/* Second descent over the (now filled) hole must not allocate. */
	KUNIT_ASSERT_EQ(test, 0,
			corten_txn_begin(&init_mm, start, start + PAGE_SIZE,
					 &corten_test_hole_ops, h, &txn));
	KUNIT_EXPECT_PTR_EQ(test, txn.covering, h->desc[0]);
	KUNIT_EXPECT_EQ(test, h->installed, 1);
	corten_unlock(&txn);

	/* Allocating view declines (-EOPNOTSUPP) over a different hole. */
	ret = corten_txn_begin(&init_mm, 4 * PMD_SIZE,
			       4 * PMD_SIZE + PAGE_SIZE,
			       &corten_test_noalloc_ops, t, &txn);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	KUNIT_EXPECT_NULL(test, txn.covering);

	/* Allocation failure (-ENOMEM): pool of zero pages. */
	h_empty = corten_test_hole_init(test, t, 1, 0);
	ret = corten_txn_begin(&init_mm, 4 * PMD_SIZE,
			       4 * PMD_SIZE + PAGE_SIZE,
			       &corten_test_hole_ops, h_empty, &txn);
	KUNIT_EXPECT_EQ(test, ret, -ENOMEM);
	KUNIT_EXPECT_NULL(test, txn.covering);

	/* Pure protocol (no @alloc callback): unchanged -ENOENT, and a
	 * failed walk leaves nothing pinned or locked behind.
	 */
	ret = corten_txn_begin(&init_mm, 4 * PMD_SIZE,
			       4 * PMD_SIZE + PAGE_SIZE,
			       &corten_test_tree_ops, t, &txn);
	KUNIT_EXPECT_EQ(test, ret, -ENOENT);
	KUNIT_EXPECT_EQ(test, txn.nr_path, 0);
	KUNIT_EXPECT_EQ(test, refcount_read(&t->desc[0]->refs), 2);
	KUNIT_EXPECT_EQ(test, refcount_read(&t->desc[1]->refs), 2);
}

/*
 * Upgrade-window concurrency (review gap: the read_unlock()/write_lock()
 * gap in the ensure-alloc upgrade must not double-install).  Two kthreads
 * hammer the same hole; each successful transaction marks and unmaps its
 * range, so the metadata must also stay consistent.  The decisive
 * assertion: exactly ONE child is ever installed.
 */
struct corten_test_race_ctx {
	struct corten_test_hole_ctx *h;
	unsigned long		start;
	int			iters;
	atomic_t		begins_ok;
	atomic_t		begins_err;
	atomic_t		op_errors;
	struct completion	done[2];
};

struct corten_test_race_worker {
	struct corten_test_race_ctx *c;
	int			id;
};

static int corten_test_race_worker(void *data)
{
	struct corten_test_race_worker *w = data;
	struct corten_test_race_ctx *c = w->c;
	int i;

	for (i = 0; i < c->iters; i++) {
		struct corten_txn txn;
		struct corten_pte_meta m = { };
		int ret;

		ret = corten_txn_begin(&init_mm, c->start,
				       c->start + 4 * PAGE_SIZE,
				       &corten_test_hole_ops, c->h, &txn);
		if (ret) {
			atomic_inc(&c->begins_err);
			continue;
		}
		atomic_inc(&c->begins_ok);

		m.state = CORTEN_PRIVATE_ANON;
		m.perm = CORTEN_PERM_READ | CORTEN_PERM_WRITE |
			 CORTEN_PERM_USER;
		if (corten_mark(&txn, c->start, 4 * PAGE_SIZE, &m))
			atomic_inc(&c->op_errors);
		if (corten_unmap(&txn, c->start, 4 * PAGE_SIZE))
			atomic_inc(&c->op_errors);

		corten_unlock(&txn);
	}

	complete(&c->done[w->id]);
	while (!kthread_should_stop())
		schedule_timeout_idle(1);

	return 0;
}

static void corten_test_txn_hole_race(struct kunit *test)
{
	struct corten_test_race_ctx *c;
	struct corten_test_hole_ctx *h;
	struct corten_test_race_worker w[2];
	struct corten_test_tree *t;
	struct task_struct *tsk[2] = { NULL, NULL };
	unsigned long start = PUD_SIZE + 4 * PMD_SIZE;
	int children_before;
	int i;

	if (num_online_cpus() < 3)
		kunit_skip(test, "need 3 online CPUs for worker placement");

	t = corten_test_tree_std(test);
	h = corten_test_hole_init(test, t, 2, CORTEN_TEST_HOLE_POOL);
	children_before = h->parent->nr_children;

	c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, c);
	c->h = h;
	c->start = start;
	c->iters = 200;
	for (i = 0; i < 2; i++) {
		w[i].c = c;
		w[i].id = i;
		init_completion(&c->done[i]);
	}

	for (i = 0; i < 2; i++) {
		tsk[i] = kthread_run(corten_test_race_worker, &w[i],
				     "corten_race_%d", i);
		if (IS_ERR(tsk[i])) {
			if (i == 1)
				kthread_stop(tsk[0]);
			KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tsk[i]);
		}
	}
	for (i = 0; i < 2; i++) {
		if (set_cpus_allowed_ptr(tsk[i], cpumask_of(i + 1))) {
			kthread_stop(tsk[0]);
			kthread_stop(tsk[1]);
			kunit_skip(test, "cannot place workers on CPUs 1,2");
		}
	}

	for (i = 0; i < 2; i++) {
		if (!wait_for_completion_timeout(&c->done[i],
						 msecs_to_jiffies(60000)))
			KUNIT_FAIL(test, "race worker %d timed out\n", i);
	}
	for (i = 0; i < 2; i++)
		kthread_stop(tsk[i]);

	KUNIT_EXPECT_EQ(test, atomic_read(&c->begins_err), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&c->op_errors), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&c->begins_ok), 2 * c->iters);
	/* Every begin either found the child or filled the hole exactly
	 * once: the write-lock re-check collapses the upgrade window.
	 */
	KUNIT_EXPECT_EQ(test, h->installed, 1);
	KUNIT_EXPECT_EQ(test, h->parent->nr_children, children_before + 1);
}

/* ---- M3a: uninstall <-> write-lock interlock ------------------------- */

/*
 * Regression test for the 2b review gap "a transaction holding the
 * covering write lock could outlive the PT-page reclaim".  Thread A holds
 * the covering write lock; thread B runs corten_ptdesc_uninstall() on
 * that very PT page.  The interlock makes uninstall wait: B must not
 * complete until A releases the lock, and A's metadata operations must
 * work for the whole hold.
 */
struct corten_test_interlock_ctx {
	struct corten_test_tree *t;
	unsigned long		pfn;
	/* Diagnostics for harness-stall forensics (see the timeout paths). */
	atomic_t		a_phase;	/* IL_* phase of worker A */
	atomic_t		a_begin_ret;	/* A's begin() result */
	atomic_t		a_locked;	/* A holds the covering write */
	atomic_t		go;		/* release signal for A */
	atomic_t		b_started;
	atomic_t		b_done;		/* uninstall returned */
	atomic_t		violations;	/* uninstall completed too early */
	atomic_t		a_err;
	struct completion	a_exited;
	struct completion	b_exited;
};

/* corten_test_interlock_ctx::a_phase values. */
#define CORTEN_TEST_IL_ENTERED		1
#define CORTEN_TEST_IL_BEGIN_DONE	2
#define CORTEN_TEST_IL_LOCKED		3
#define CORTEN_TEST_IL_RELEASED		4
#define CORTEN_TEST_IL_EXITED		5

/* Bounded spin (no schedule: we are under a spinlock). */
static int corten_test_interlock_a(void *data)
{
	struct corten_test_interlock_ctx *c = data;
	u64 deadline = ktime_get_mono_fast_ns() + 20 * NSEC_PER_SEC;
	struct corten_txn txn;
	struct corten_pte_meta m = { };
	int ret;

	atomic_set(&c->a_phase, CORTEN_TEST_IL_ENTERED);
	ret = corten_txn_begin(&init_mm, 8 * PAGE_SIZE, 12 * PAGE_SIZE,
			       &corten_test_tree_ops, c->t, &txn);
	atomic_set(&c->a_begin_ret, ret);
	if (ret) {
		atomic_inc(&c->a_err);
		goto out;
	}
	atomic_set(&c->a_phase, CORTEN_TEST_IL_BEGIN_DONE);

	atomic_set(&c->a_locked, 1);
	atomic_set(&c->a_phase, CORTEN_TEST_IL_LOCKED);
	while (!atomic_read(&c->go)) {
		if (ktime_get_mono_fast_ns() > deadline) {
			/* Not a protocol violation, but report that the
			 * hold could not be released (harness stall).
			 */
			atomic_inc(&c->a_err);
			break;
		}
		cpu_relax();
	}

	/* The PT page stayed exclusively ours: metadata still works. */
	m.state = CORTEN_PRIVATE_ANON;
	m.perm = CORTEN_PERM_READ;
	if (corten_mark(&txn, 8 * PAGE_SIZE, PAGE_SIZE, &m) ||
	    corten_unmap(&txn, 8 * PAGE_SIZE, PAGE_SIZE))
		atomic_inc(&c->a_err);

	atomic_set(&c->a_locked, 0);
	atomic_set(&c->a_phase, CORTEN_TEST_IL_RELEASED);
	corten_unlock(&txn);
out:
	complete(&c->a_exited);
	atomic_set(&c->a_phase, CORTEN_TEST_IL_EXITED);
	while (!kthread_should_stop())
		schedule_timeout_idle(1);

	return 0;
}

static int corten_test_interlock_b(void *data)
{
	struct corten_test_interlock_ctx *c = data;

	atomic_set(&c->b_started, 1);
	corten_ptdesc_uninstall(pfn_to_page(c->pfn));
	atomic_set(&c->b_done, 1);
	complete(&c->b_exited);
	while (!kthread_should_stop())
		schedule_timeout_idle(1);

	return 0;
}

/* Bounded poll of a flag from the (lock-free) main thread. */
static bool corten_test_wait_flag(atomic_t *flag, unsigned int ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(ms);

	while (!atomic_read(flag)) {
		if (time_after(jiffies, deadline))
			return false;
		schedule_timeout_uninterruptible(msecs_to_jiffies(10));
	}

	return true;
}

static void corten_test_txn_uninstall_interlock(struct kunit *test)
{
	struct corten_test_interlock_ctx *c;
	struct corten_test_tree *t;
	struct task_struct *tsk_a = NULL, *tsk_b = NULL;
	unsigned long pfn;

	if (num_online_cpus() < 3)
		kunit_skip(test, "need 3 online CPUs for worker placement");

	t = corten_test_tree_std(test);
	c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, c);
	c->t = t;
	pfn = page_to_pfn(t->page[3]);
	c->pfn = pfn;
	init_completion(&c->a_exited);
	init_completion(&c->b_exited);

	/* A first: it must HOLD the covering write lock before B starts
	 * uninstalling, otherwise B's uninstall would legitimately
	 * complete in the unlocked window and the test would measure
	 * nothing.
	 */
	tsk_a = kthread_run(corten_test_interlock_a, c, "corten_il_a");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tsk_a);
	if (set_cpus_allowed_ptr(tsk_a, cpumask_of(1)))
		goto place_fail_a;
	if (!corten_test_wait_flag(&c->a_locked, 10000)) {
		atomic_set(&c->go, 1);
		kthread_stop(tsk_a);
		KUNIT_FAIL(test,
			   "worker A never acquired the lock (phase=%d begin_ret=%d)\n",
			   atomic_read(&c->a_phase),
			   atomic_read(&c->a_begin_ret));
		return;
	}

	tsk_b = kthread_run(corten_test_interlock_b, c, "corten_il_b");
	if (IS_ERR(tsk_b)) {
		atomic_set(&c->go, 1);
		kthread_stop(tsk_a);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tsk_b);
	}
	if (set_cpus_allowed_ptr(tsk_b, cpumask_of(2)))
		goto place_fail_b;

	/* Give B ample time to finish if the interlock were broken. */
	if (!corten_test_wait_flag(&c->b_started, 5000)) {
		atomic_set(&c->go, 1);
		kthread_stop(tsk_a);
		kthread_stop(tsk_b);
		KUNIT_FAIL(test, "worker B never started\n");
		return;
	}
	schedule_timeout_uninterruptible(msecs_to_jiffies(200));
	if (atomic_read(&c->b_done))
		atomic_inc(&c->violations);
	KUNIT_EXPECT_EQ(test, atomic_read(&c->a_locked), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&c->violations), 0);

	/* Release A: uninstall must now complete. */
	atomic_set(&c->go, 1);
	KUNIT_EXPECT_TRUE(test, corten_test_wait_flag(&c->b_done, 5000));
	wait_for_completion_timeout(&c->a_exited, msecs_to_jiffies(5000));
	kthread_stop(tsk_a);
	kthread_stop(tsk_b);

	KUNIT_EXPECT_EQ(test, atomic_read(&c->a_err), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&c->violations), 0);
	/* The descriptor was retired -- but only after A's unlock. */
	KUNIT_EXPECT_NULL(test, corten_ptdesc_get(pfn));
	KUNIT_EXPECT_EQ(test, READ_ONCE(t->desc[3]->stale), 1);
	return;

place_fail_b:
	atomic_set(&c->go, 1);
	kthread_stop(tsk_a);
	kthread_stop(tsk_b);
	kunit_skip(test, "cannot place workers on CPUs 1,2");
place_fail_a:
	kthread_stop(tsk_a);
	kunit_skip(test, "cannot place workers on CPUs 1,2");
}

/*
 * The deferred free funnel reaches corten_ptdesc_uninstall() from softirq
 * context (khugepaged: pte_free_defer() -> call_rcu(pte_free_now) ->
 * pte_free -> hook), so the M3a hardening made every desc->lock taker
 * BH-symmetric.  Drive the uninstall from an explicitly BH-disabled
 * context to cover exactly that lock-usage class: under PROVE_LOCKING a
 * non-BH-symmetric scheme (an irqsave write here vs plain task-side read
 * holders, or the reverse mix) shows up as an inconsistent lock-usage
 * report; after the hardening this must stay silent and succeed.
 * (local_bh_disable() reproduces the softirq handler's lockdep context
 * class; a real tasklet would only add scheduling realism.)
 */
static void corten_test_uninstall_bh_ctx(struct kunit *test)
{
	struct page *page = corten_test_track_ptpage(test, &init_mm);
	unsigned long pfn = page_to_pfn(page);
	struct corten_ptdesc *desc;

	desc = corten_ptdesc_get(pfn);
	KUNIT_ASSERT_NOT_NULL(test, desc);

	local_bh_disable();
	corten_ptdesc_uninstall(page);
	local_bh_enable();

	/* The retirement is complete and observable: the lookup is gone,
	 * staleness was published under the lock, and our pin keeps the
	 * descriptor readable until the put().
	 */
	KUNIT_EXPECT_NULL(test, corten_ptdesc_get(pfn));
	KUNIT_EXPECT_EQ(test, READ_ONCE(desc->stale), 1);
	KUNIT_EXPECT_EQ(test, desc->magic, CORTEN_PTDESC_MAGIC);
	corten_ptdesc_put(desc);
}

/* ---- M3a: structural guards ------------------------------------------ */

/*
 * Corrupt-view guard: a view whose descent never terminates (every node
 * labeled PUD, chained children) must hit the CORTEN_TXN_PATH_MAX bound
 * and fail with -EPROTO, releasing every pin it took.  Note: exercising
 * the guards fires the descent level guard's WARN_ON_ONCE first and the
 * path bound's WARN_ON_ONCE second -- expected log noise.
 */
static struct corten_ptdesc *corten_test_chain_child(void *ctx,
						     struct corten_ptdesc *parent,
						     unsigned long addr)
{
	struct corten_test_tree *t = ctx;
	int pn = -1;
	int i;

	for (i = 0; i < t->nr; i++) {
		if (t->desc[i] == parent) {
			pn = i;
			break;
		}
	}
	if (pn < 0)
		return ERR_PTR(-EIO);
	for (i = 0; i < t->nr; i++) {
		if (t->parent[i] == pn)
			return corten_ptdesc_get(page_to_pfn(t->page[i]));
	}

	return NULL;
}

static const struct corten_tree_ops corten_test_chain_ops = {
	.root	= corten_test_tree_root,
	.child	= corten_test_chain_child,
};

static void corten_test_txn_path_overflow(struct kunit *test)
{
	struct corten_test_tree *t;
	struct corten_txn txn;
	int i, ret;

	/* A 6-node chain: one deeper than any legal walk.  The chain's
	 * levels repeat (all nodes labeled PUD), which intentionally
	 * violates two protocol invariants: the descent's "level strictly
	 * moves toward the leaf" guard (WARN_ON_ONCE in corten_txn_begin())
	 * fires on the first same-level hop, and the CORTEN_TXN_PATH_MAX
	 * bound then stops the walk -- both warnings are expected log
	 * noise, and the -EPROTO comes from the path bound.
	 *
	 * No lockdep_off() bracket: the nested acquisitions are rwlock READ
	 * sides of one lock class, and recursive read-read nesting is legal
	 * for lockdep, so the validator is welcome to watch this walk --
	 * silencing it would only hide regressions.
	 */
	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);
	kunit_add_action(test, corten_test_tree_destroy, t);
	for (i = 0; i < 6; i++)
		corten_test_tree_add(test, t, 0, CORTEN_LEVEL_PUD,
				     i - 1);

	ret = corten_txn_begin(&init_mm, 0, PAGE_SIZE,
			       &corten_test_chain_ops, t, &txn);
	KUNIT_EXPECT_EQ(test, ret, -EPROTO);
	KUNIT_EXPECT_NULL(test, txn.covering);
	KUNIT_EXPECT_EQ(test, txn.nr_path, 0);
	for (i = 0; i < t->nr; i++)
		KUNIT_EXPECT_EQ(test, refcount_read(&t->desc[i]->refs), 2);
}

/* ops->child() reporting an error (ERR_PTR) must fail the begin cleanly. */
static struct corten_ptdesc *corten_test_child_err(void *ctx,
						   struct corten_ptdesc *parent,
						   unsigned long addr)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static const struct corten_tree_ops corten_test_child_err_ops = {
	.root	= corten_test_tree_root,
	.child	= corten_test_child_err,
};

static void corten_test_txn_child_err(struct kunit *test)
{
	struct corten_test_tree *t;
	struct corten_txn txn;
	int ret;

	t = corten_test_tree_std(test);

	ret = corten_txn_begin(&init_mm, PAGE_SIZE, 2 * PAGE_SIZE,
			       &corten_test_child_err_ops, t, &txn);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	KUNIT_EXPECT_NULL(test, txn.covering);
	KUNIT_EXPECT_EQ(test, txn.nr_path, 0);
	/* Nothing pinned or locked behind. */
	KUNIT_EXPECT_EQ(test, refcount_read(&t->desc[0]->refs), 2);
}

/* Real-view huge leaves (pmd_leaf/pud_leaf) must decline with
 * -EOPNOTSUPP (the covering PT-page protocol is 4K-page shaped; hugepage
 * coverage is M4+ scope).
 */
static void corten_test_real_huge_leaf(struct kunit *test)
{
	unsigned long addr = PAGE_SIZE;
	struct mm_struct *mm;
	struct corten_txn txn;
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	int ret;

	mm = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mm);

	pgdp = pgd_offset(mm, addr);
	p4dp = p4d_alloc(mm, pgdp, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, p4dp);
	pudp = pud_alloc(mm, p4dp, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pudp);
	pmdp = pmd_alloc(mm, pudp, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pmdp);

	/* PMD-level leaf (THP-shaped): present, but not a PT page. */
	set_pmd(pmdp, __pmd(_PAGE_PSE | _PAGE_PRESENT));
	ret = corten_lock_range(mm, addr, PAGE_SIZE, &txn);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	pmd_clear(pmdp);

	/* PUD-level leaf (1G page). */
	set_pud(pudp, __pud(_PAGE_PSE | _PAGE_PRESENT));
	ret = corten_lock_range(mm, addr, PAGE_SIZE, &txn);
	KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	pud_clear(pudp);

	/* Balance the pgtables_bytes accounting that pmd_alloc()/pud_alloc()
	 * did (same raw teardown as corten_test_real_destroy()).
	 */
	mm_dec_nr_pmds(mm);
	pmd_free(mm, pmdp);
	mm_dec_nr_puds(mm);
	pud_free(mm, pudp);
	mmput(mm);
}

/* ---- M3a: full-window atomicity + debugfs content --------------------- */

/* A 512-entry (full-window) transaction stays all-or-nothing at the
 * window's edges: one invalid or conflicting slot anywhere must leave
 * every one of the 512 entries untouched.
 */
static void corten_test_full_window_atomic(struct kunit *test)
{
	unsigned long start = 0;
	unsigned long len = CORTEN_PTES_PER_PT_PAGE * PAGE_SIZE;
	struct corten_test_tree *t;
	struct corten_txn txn;
	struct corten_pte_meta m, q;

	t = corten_test_tree_std(test);

	KUNIT_ASSERT_EQ(test, 0,
			corten_txn_begin(&init_mm, start, start + len,
					 &corten_test_tree_ops, t, &txn));
	KUNIT_EXPECT_PTR_EQ(test, txn.covering, t->desc[3]);

	/* Mark the whole 512-page window in one transaction. */
	memset(&m, 0, sizeof(m));
	m.state = CORTEN_PRIVATE_ANON;
	m.perm = CORTEN_PERM_READ;
	KUNIT_EXPECT_EQ(test, 0, corten_mark(&txn, start, len, &m));

	/* One Invalid slot: the full-window unmap fails atomically. */
	KUNIT_EXPECT_EQ(test, 0,
			corten_unmap(&txn, 256 * PAGE_SIZE, PAGE_SIZE));
	KUNIT_EXPECT_EQ(test, -ENOENT, corten_unmap(&txn, start, len));
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, 0, &q));
	KUNIT_EXPECT_EQ(test, q.state, CORTEN_PRIVATE_ANON);
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, 511 * PAGE_SIZE, &q));
	KUNIT_EXPECT_EQ(test, q.state, CORTEN_PRIVATE_ANON);

	/* One conflicting slot (Mapped refuses PrivateAnon): the
	 * full-window mark fails atomically.
	 */
	t->desc[3]->meta[511].state = CORTEN_MAPPED;
	KUNIT_EXPECT_EQ(test, -EINVAL, corten_mark(&txn, start, len, &m));
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, 0, &q));
	KUNIT_EXPECT_EQ(test, q.state, CORTEN_PRIVATE_ANON);
	t->desc[3]->meta[511].state = CORTEN_PRIVATE_ANON;

	/* Repair the gap, then retire the whole window in one go. */
	KUNIT_EXPECT_EQ(test, 0,
			corten_mark(&txn, 256 * PAGE_SIZE, PAGE_SIZE, &m));
	KUNIT_EXPECT_EQ(test, 0, corten_unmap(&txn, start, len));
	KUNIT_EXPECT_EQ(test, 0, corten_query(&txn, 511 * PAGE_SIZE, &q));
	KUNIT_EXPECT_EQ(test, q.state, CORTEN_INVALID);

	corten_unlock(&txn);
}

/* Parse the decimal that follows @key in a rendered debugfs section. */
static bool corten_test_dbg_value(const char *s, const char *key, long *out)
{
	const char *p = strstr(s, key);
	long v = 0;
	bool neg = false;

	if (!p)
		return false;
	p += strlen(key);
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '-') {
		neg = true;
		p++;
	}
	if (*p < '0' || *p > '9')
		return false;
	while (*p >= '0' && *p <= '9')
		v = v * 10 + (*p++ - '0');
	*out = neg ? -v : v;

	return true;
}

/* Content assertions for the three debugfs files.  The builtin KUnit run
 * happens before userspace could mount debugfs, so the assertions drive
 * the very renderers the files use (corten_test_render_dbg()).
 */
static void corten_test_debugfs_content(struct kunit *test)
{
	struct page *page = corten_test_track_ptpage(test, &init_mm);
	unsigned long pfn = page_to_pfn(page);
	char needle[24];
	char *s;
	long v;

	/* stats: gate line, live descriptors, array accounting. */
	s = corten_test_render_dbg(CORTEN_DBG_STATS);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, s);
	KUNIT_EXPECT_NOT_NULL(test, strstr(s, "enabled"));
	KUNIT_EXPECT_TRUE(test, corten_test_dbg_value(s, "ptdescs", &v));
	KUNIT_EXPECT_GE(test, v, 1);
	KUNIT_EXPECT_TRUE(test, corten_test_dbg_value(s, "meta_bytes", &v));
	KUNIT_EXPECT_GE(test, v, CORTEN_META_ARRAY_BYTES);
	kfree(s);

	/* txn: nothing left running, watermark recorded. */
	s = corten_test_render_dbg(CORTEN_DBG_TXN);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, s);
	KUNIT_EXPECT_TRUE(test, corten_test_dbg_value(s, "active", &v));
	KUNIT_EXPECT_EQ(test, v, 0);
	KUNIT_EXPECT_TRUE(test, corten_test_dbg_value(s, "active_max", &v));
	KUNIT_EXPECT_GE(test, v, 1);
	kfree(s);

	/* dump: header plus one line per live descriptor. */
	s = corten_test_render_dbg(CORTEN_DBG_DUMP);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, s);
	KUNIT_EXPECT_NOT_NULL(test, strstr(s, "pfn"));
	snprintf(needle, sizeof(needle), "%lu ", pfn);
	KUNIT_EXPECT_NOT_NULL(test, strstr(s, needle));
	kfree(s);
}

static struct kunit_case corten_test_cases[] = {
	KUNIT_CASE(corten_test_layout),
	KUNIT_CASE(corten_test_ptdesc_get_put),
	KUNIT_CASE(corten_test_meta_roundtrip),
	KUNIT_CASE(corten_test_meta_reinit_cleared),
	KUNIT_CASE(corten_test_covering_level),
	KUNIT_CASE(corten_test_covering_base_index),
	KUNIT_CASE(corten_test_txn_covering),
	KUNIT_CASE(corten_test_txn_hole),
	KUNIT_CASE(corten_test_txn_stale_eagain),
	KUNIT_CASE(corten_test_lock_range_args),
	KUNIT_CASE(corten_test_txn_state_machine),
	KUNIT_CASE(corten_test_txn_atomic_validate),
	KUNIT_CASE(corten_test_txn_real_glue),
	KUNIT_CASE(corten_test_txn_mutex_overlap),
	KUNIT_CASE(corten_test_txn_mutex_disjoint),
	KUNIT_CASE(corten_test_fail_alloc),
	KUNIT_CASE(corten_test_txn_hole_fill),
	KUNIT_CASE(corten_test_txn_hole_race),
	KUNIT_CASE(corten_test_txn_uninstall_interlock),
	KUNIT_CASE(corten_test_uninstall_bh_ctx),
	KUNIT_CASE(corten_test_txn_path_overflow),
	KUNIT_CASE(corten_test_txn_child_err),
	KUNIT_CASE(corten_test_real_huge_leaf),
	KUNIT_CASE(corten_test_full_window_atomic),
	KUNIT_CASE(corten_test_debugfs_content),
	{}
};

static struct kunit_suite corten_test_suite = {
	.name = "corten",
	.test_cases = corten_test_cases,
};

kunit_test_suite(corten_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit test for CortenMM protocol and data structures");
