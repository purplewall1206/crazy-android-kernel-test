// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the CortenMM M2a data-structure skeleton.
 *
 * These tests exercise the internal bookkeeping directly (mm/corten.h) and
 * do not depend on a real user address space: PT pages come from the page
 * allocator and the owning mm is only stored as a back-link, so any mm
 * pointer (init_mm here) works.  The corten_enabled_static() gate is
 * deliberately not touched: tests run fine on kernels booted with
 * corten=off because they call corten_ptdesc_install()/uninstall(), the
 * functions below the gate.
 *
 * Slice 2b owns the protocol-level tests (lock_range/query/map/mark/unmap
 * atomicity); this file only covers descriptor indexing, refcounting,
 * metadata arrays and the pure covering-page helpers.
 */
#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/pgtable.h>

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

static struct kunit_case corten_test_cases[] = {
	KUNIT_CASE(corten_test_layout),
	KUNIT_CASE(corten_test_ptdesc_get_put),
	KUNIT_CASE(corten_test_meta_roundtrip),
	KUNIT_CASE(corten_test_meta_reinit_cleared),
	KUNIT_CASE(corten_test_covering_level),
	KUNIT_CASE(corten_test_covering_base_index),
	{}
};

static struct kunit_suite corten_test_suite = {
	.name = "corten",
	.test_cases = corten_test_cases,
};

kunit_test_suite(corten_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit test for the CortenMM data structure skeleton");
