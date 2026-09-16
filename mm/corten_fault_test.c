// SPDX-License-Identifier: GPL-2.0
/*
 * CortenMM arena fault-path and space-operation-routing tests
 * (M3B_DESIGN.md sec 4/5, slices S4-S7).
 *
 * Test contract (S4 degraded scope, per design sec 9 slice table): the
 * dispatch state machine and the S6 routing classifiers run as pure,
 * table-driven tests; the real fault chain is additionally exercised on a
 * real mm_alloc()'d address space (fill_upper double-install race, map
 * race with the pte_none re-check, zero-page read + write upgrade, MAPPED
 * restore, chunk unmap) -- the guest smoke test of the syscall paths
 * (bench/arena-stress/) completes the coverage.
 *
 * Like mm/corten_test.c this suite drives gate-free internal entry points
 * (no corten_enabled_static() on any of them), so it runs on a kernel
 * booted with corten=off.
 */
#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/corten.h>
#include <linux/corten_arena.h>
#include <linux/cpumask.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/mm_inline.h>
#include <linux/pgtable.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "corten_arena.h"
#include "corten.h"		/* corten_ptdesc_install() (gate-free) */
#include "internal.h"
#include "vma.h"

/* Test layout: one 2M-frame arena in the low user range, PMD-aligned. */
#define FT_BASE			(4UL * PMD_SIZE)
#define FT_ARENA_LEN		(2UL * PMD_SIZE)
#define FT_NPAGES		8

/* A fresh MAP_NORESERVE private-anonymous VMA (DECLARE precondition). */
#define FT_FLAGS_OK		(VM_READ | VM_WRITE | \
				 VM_MAYREAD | VM_MAYWRITE | VM_NORESERVE)

struct ft_mm {
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned long ar_start;
	unsigned long ar_end;
};

static void ft_mm_destroy(void *ctx)
{
	struct ft_mm *t = ctx;

	if (t->mm)
		mmput(t->mm);
}

static struct vm_area_struct *ft_mkvm(struct mm_struct *mm, unsigned long start,
				      unsigned long end, vm_flags_t flags)
{
	struct vm_area_struct *vma;
	int ret;

	vma = vm_area_alloc(mm);
	if (!vma)
		return NULL;

	vma_set_range(vma, start, end, 0);
	/* The vma cache is SLAB_TYPESAFE_BY_RCU: a fresh VMA is not
	 * anonymous until told so (same as the real mmap path).
	 */
	vma_set_anonymous(vma);
	vm_flags_init(vma, flags);
	/* mmap_region() derives this from the flags; the arena fault
	 * paths build PTEs from vm_page_prot (do_anonymous_page style),
	 * so a hand-made VMA must set it or every install lands as a
	 * non-present PTE (the "pte_present false" failures).
	 */
	vma->vm_page_prot = vm_get_page_prot(flags);

	mmap_write_lock(mm);
	ret = vma_link(mm, vma);
	mmap_write_unlock(mm);
	if (ret) {
		vm_area_free(vma);
		return NULL;
	}

	return vma;
}

/* One mm with a declared (shadowized) 2M arena at FT_BASE. */
static struct ft_mm *ft_setup(struct kunit *test)
{
	struct ft_mm *t;
	int ret;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);
	kunit_add_action(test, ft_mm_destroy, t);

	t->mm = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->mm);

	t->vma = ft_mkvm(t->mm, FT_BASE, FT_BASE + FT_ARENA_LEN, FT_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->vma);

	ret = corten_arena_declare(t->mm, FT_BASE, FT_ARENA_LEN);
	KUNIT_ASSERT_EQ(test, ret, 0);

	t->ar_start = FT_BASE;
	t->ar_end = FT_BASE + FT_ARENA_LEN;

	/*
	 * Make window 0 transaction-ready: fill_upper() creates the upper
	 * tables and the tracked PTE-level page (on a corten=on boot the
	 * M2a hook installs its descriptor inside pte_alloc_one()), so
	 * corten_mark() can record the virtual allocation before any
	 * fault.  Mirrors the real mmap route (sec 5.6 fill + mark).
	 */
	{
		struct corten_arena *ar = corten_arena_lookup_get(t->mm,
								  FT_BASE);

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ar);
		KUNIT_ASSERT_EQ(test, corten_arena_fill_upper(ar, FT_BASE), 0);
		percpu_ref_put(&ar->active);
	}

	return t;
}

/* Metadata of @addr through the public transaction API. */
static int ft_meta(struct ft_mm *t, unsigned long addr,
		   struct corten_pte_meta *out)
{
	struct corten_txn txn;
	int ret;

	ret = corten_lock_range(t->mm, addr, PAGE_SIZE, &txn);
	if (ret)
		return ret;
	ret = corten_query(&txn, addr, out);
	corten_unlock(&txn);

	return ret;
}

/* Record the virtual allocation (the mmap() of the arena contract)
 * for [addr, addr+len): metadata PRIVATE_ANON with @perm.  Faults on
 * unmarked pages are SEGV_MAPERR by design (sec 4.3 dispatch).
 */
static int ft_mark(struct ft_mm *t, unsigned long addr, unsigned long len,
		   u8 perm)
{
	struct corten_txn txn;
	struct corten_pte_meta m = { };
	int ret;

	ret = corten_lock_range(t->mm, addr, len, &txn);
	if (ret)
		return ret;
	m.state = CORTEN_PRIVATE_ANON;
	m.perm = perm;
	ret = corten_mark(&txn, addr, len, &m);
	corten_unlock(&txn);

	return ret;
}

#define FT_PERM_RW	(CORTEN_PERM_READ | CORTEN_PERM_WRITE | \
			 CORTEN_PERM_USER)

static pte_t *ft_pte(struct ft_mm *t, unsigned long addr)
{
	pgd_t *pgdp = pgd_offset(t->mm, addr);
	p4d_t *p4dp = p4d_offset(pgdp, addr);
	pud_t *pudp = pud_offset(p4dp, addr);
	pmd_t *pmdp = pmd_offset(pudp, addr);

	if (!pmd_present(*pmdp))
		return NULL;

	return pte_offset_map(pmdp, addr);
}

/* ------------------------------------------------------------------ *
 * S4: dispatch state machine (pure)
 * ------------------------------------------------------------------
 */

static void corten_fault_test_dispatch(struct kunit *test)
{
	static const struct {
		const char *name;
		struct corten_pte_meta m;
		bool write, instr;
		enum corten_disp expect;
	} cases[] = {
		/* CORTEN_INVALID: an unmarked page inside a declared arena
		 * is the fresh-allocation fault (Fig.8 L26-38), not a
		 * mapping error -- faults outside any arena never reach
		 * this classifier.  The permission gate runs at the
		 * caller against the arena prot (the zeroed metadata
		 * carries none); this was r03 DoD failure mode A.
		 */
		{ "invalid-read", { CORTEN_INVALID, CORTEN_PERM_ALL, 0, { 0 } },
		  false, false, CORTEN_DISP_FRESH },
		{ "invalid-write", { CORTEN_INVALID, CORTEN_PERM_ALL, 0, { 0 } },
		  true, false, CORTEN_DISP_FRESH },
		/* PRIVATE_ANON: the virtual allocation from the mmap mark. */
		{ "anon-read", { CORTEN_PRIVATE_ANON, CORTEN_PERM_READ |
				 CORTEN_PERM_WRITE | CORTEN_PERM_USER, 0, { 0 } },
		  false, false, CORTEN_DISP_ZERO_PAGE },
		{ "anon-write", { CORTEN_PRIVATE_ANON, CORTEN_PERM_READ |
				  CORTEN_PERM_WRITE | CORTEN_PERM_USER, 0, { 0 } },
		  true, false, CORTEN_DISP_MAP_ANON },
		/* Permission mismatches -> SEGV_ACCERR. */
		{ "ro-write", { CORTEN_PRIVATE_ANON, CORTEN_PERM_READ |
				CORTEN_PERM_USER, 0, { 0 } },
		  true, false, CORTEN_DISP_ACCERR },
		{ "rw-exec", { CORTEN_PRIVATE_ANON, CORTEN_PERM_READ |
			       CORTEN_PERM_WRITE | CORTEN_PERM_USER, 0, { 0 } },
		  false, true, CORTEN_DISP_ACCERR },
		{ "none-any", { CORTEN_PRIVATE_ANON, 0, 0, { 0 } },
		  false, false, CORTEN_DISP_ACCERR },
		{ "none-write", { CORTEN_PRIVATE_ANON, 0, 0, { 0 } },
		  true, false, CORTEN_DISP_ACCERR },
		/* MAPPED with sufficient permission: rebuild from metadata. */
		{ "mapped-reread", { CORTEN_MAPPED, CORTEN_PERM_READ |
				     CORTEN_PERM_WRITE | CORTEN_PERM_USER, 0, { 0 } },
		  false, false, CORTEN_DISP_RESTORE },
		{ "mapped-write", { CORTEN_MAPPED, CORTEN_PERM_READ |
				    CORTEN_PERM_WRITE | CORTEN_PERM_USER, 0, { 0 } },
		  true, false, CORTEN_DISP_RESTORE },
		/* MAPPED, permission lost -> ACCERR; shared -> M5 stub. */
		{ "mapped-ro-write", { CORTEN_MAPPED, CORTEN_PERM_READ |
				       CORTEN_PERM_USER, 0, { 0 } },
		  true, false, CORTEN_DISP_ACCERR },
		{ "mapped-shared", { CORTEN_MAPPED, CORTEN_PERM_READ |
				     CORTEN_PERM_USER, CORTEN_PF_SHARED, { 0 } },
		  true, false, CORTEN_DISP_STUB },
		{ "mapped-shared-writable-flag-ok", { CORTEN_MAPPED,
		  CORTEN_PERM_READ | CORTEN_PERM_WRITE | CORTEN_PERM_USER,
		  CORTEN_PF_SHARED | CORTEN_PF_WRITABLE, { 0 } },
		  true, false, CORTEN_DISP_RESTORE },
		/* Producer states M3 never creates: refuse loudly. */
		{ "swapped", { CORTEN_SWAPPED, CORTEN_PERM_ALL, 0, { 0 } },
		  false, false, CORTEN_DISP_STUB },
		{ "file", { CORTEN_FILE_MAPPED, CORTEN_PERM_ALL, 0, { 0 } },
		  false, false, CORTEN_DISP_STUB },
		{ "shared-anon", { CORTEN_SHARED_ANON, CORTEN_PERM_ALL, 0, { 0 } },
		  false, false, CORTEN_DISP_STUB },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++)
		KUNIT_EXPECT_EQ_MSG(test, corten_arena_dispatch(&cases[i].m,
							       cases[i].write,
							       cases[i].instr),
				    cases[i].expect, "%s", cases[i].name);
}

/* ------------------------------------------------------------------ *
 * S6: routing classifiers (pure)
 * ------------------------------------------------------------------
 */

static void corten_fault_test_unmap_classify(struct kunit *test)
{
	unsigned long ar_start = FT_BASE, ar_end = FT_BASE + FT_ARENA_LEN;

	/* Outside (before, after, gap-free adjacency). */
	KUNIT_EXPECT_EQ(test,
			corten_arena_unmap_classify(FT_BASE - PAGE_SIZE,
						    FT_BASE, ar_start, ar_end),
			CORTEN_UNMAP_OUTSIDE);
	KUNIT_EXPECT_EQ(test,
			corten_arena_unmap_classify(ar_end, ar_end + PAGE_SIZE,
						    ar_start, ar_end),
			CORTEN_UNMAP_OUTSIDE);
	/* Strictly inside. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_unmap_classify(FT_BASE + PAGE_SIZE,
						    FT_BASE + 4 * PAGE_SIZE,
						    ar_start, ar_end),
			CORTEN_UNMAP_CHUNK);
	/* Exactly the arena: RELEASE territory for munmap. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_unmap_classify(ar_start, ar_end,
						    ar_start, ar_end),
			CORTEN_UNMAP_EXACT);
	/* Crossings. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_unmap_classify(FT_BASE - PAGE_SIZE,
						    FT_BASE + PAGE_SIZE,
						    ar_start, ar_end),
			CORTEN_UNMAP_PARTIAL);
	KUNIT_EXPECT_EQ(test,
			corten_arena_unmap_classify(FT_BASE,
						    ar_end + PAGE_SIZE,
						    ar_start, ar_end),
			CORTEN_UNMAP_PARTIAL);
	KUNIT_EXPECT_EQ(test,
			corten_arena_unmap_classify(FT_BASE + PAGE_SIZE,
						    ar_end + PAGE_SIZE,
						    ar_start, ar_end),
			CORTEN_UNMAP_PARTIAL);
}

static void corten_fault_test_mmap_classify(struct kunit *test)
{
	unsigned long fixed = MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS;

	/* The [P1-4] positive gate. */
	KUNIT_EXPECT_EQ(test, corten_arena_mmap_classify(fixed, false),
			CORTEN_MMAP_MARK);
	/* Every exclusion of the routing gate. */
	KUNIT_EXPECT_EQ(test, corten_arena_mmap_classify(MAP_PRIVATE |
							 MAP_ANONYMOUS, false),
			CORTEN_MMAP_LEGACY);			/* no FIXED */
	KUNIT_EXPECT_EQ(test, corten_arena_mmap_classify(fixed |
							 MAP_FIXED_NOREPLACE,
							 false),
			CORTEN_MMAP_LEGACY);			/* NOREPLACE */
	KUNIT_EXPECT_EQ(test, corten_arena_mmap_classify(fixed, true),
			CORTEN_MMAP_LEGACY);			/* file */
	KUNIT_EXPECT_EQ(test, corten_arena_mmap_classify(MAP_FIXED |
							 MAP_ANONYMOUS |
							 MAP_SHARED, false),
			CORTEN_MMAP_LEGACY);			/* shared */
	KUNIT_EXPECT_EQ(test, corten_arena_mmap_classify(fixed |
							 MAP_HUGETLB, false),
			CORTEN_MMAP_LEGACY);
	KUNIT_EXPECT_EQ(test, corten_arena_mmap_classify(fixed |
							 MAP_GROWSDOWN, false),
			CORTEN_MMAP_LEGACY);
	KUNIT_EXPECT_EQ(test, corten_arena_mmap_classify(fixed |
							 MAP_POPULATE, false),
			CORTEN_MMAP_LEGACY);
	KUNIT_EXPECT_EQ(test, corten_arena_mmap_classify(fixed |
							 MAP_LOCKED, false),
			CORTEN_MMAP_LEGACY);
}

/* ------------------------------------------------------------------ *
 * S4/S5: real fault chain on a synthetic address space
 * ------------------------------------------------------------------
 */

/* One write fault through the slow-path entry point. */
static int ft_write_fault(struct ft_mm *t, unsigned long addr)
{
	return corten_arena_handle_mm_fault(t->vma, addr, FAULT_FLAG_WRITE,
					    NULL);
}

static void corten_fault_test_map_anon(struct kunit *test)
{
	/*
	 * The real fault chain needs the M2a descriptor-install hook,
	 * which is gated on corten_enabled_static(): on a corten=off boot
	 * every arena fault correctly falls back to the legacy path and
	 * there is nothing for these cases to observe.  They run in the
	 * corten=on verification boot; the pure dispatch/classifier cases
	 * above cover the corten=off contract (S4 degraded scope).
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "real fault chain requires corten=on");
	struct ft_mm *t = ft_setup(test);
	struct corten_pte_meta m;
	struct folio *folio;
	pte_t *ptep, pte;
	unsigned long addr = FT_BASE + 3 * PAGE_SIZE;

	KUNIT_ASSERT_EQ_MSG(test, ft_mark(t, addr, PAGE_SIZE, FT_PERM_RW),
			    0, "ft_mark failed for map_anon");
	KUNIT_EXPECT_EQ(test, ft_write_fault(t, addr), 0);

	/* Metadata transitioned to MAPPED with the DECLARE permissions. */
	KUNIT_EXPECT_EQ(test, ft_meta(t, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, m.perm,
			CORTEN_PERM_READ | CORTEN_PERM_WRITE |
			CORTEN_PERM_USER);

	/* PTE present, writable, pointing at a sane exclusive folio. */
	ptep = ft_pte(t, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));
	KUNIT_EXPECT_TRUE(test, pte_write(pte));
	KUNIT_EXPECT_FALSE(test, pte_special(pte));

	folio = page_folio(pte_page(pte));
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 1);	/* PTE ref */
	KUNIT_EXPECT_EQ(test, folio_mapped(folio), 1);
	KUNIT_EXPECT_TRUE(test, folio_test_anon(folio));
	/* The page was zeroed (P1-2) and is NOT on the LRU (sec 4.6). */
	KUNIT_EXPECT_TRUE(test, folio_test_uptodate(folio));
	KUNIT_EXPECT_FALSE(test, folio_test_lru(folio));

	/* Accounting: exactly one anonymous page. */
	KUNIT_EXPECT_EQ(test, get_mm_counter_sum(t->mm, MM_ANONPAGES), 1);
}

static void corten_fault_test_zero_page(struct kunit *test)
{
	/*
	 * The real fault chain needs the M2a descriptor-install hook,
	 * which is gated on corten_enabled_static(): on a corten=off boot
	 * every arena fault correctly falls back to the legacy path and
	 * there is nothing for these cases to observe.  They run in the
	 * corten=on verification boot; the pure dispatch/classifier cases
	 * above cover the corten=off contract (S4 degraded scope).
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "real fault chain requires corten=on");
	struct ft_mm *t = ft_setup(test);
	struct corten_pte_meta m;
	pte_t *ptep, pte;
	unsigned long addr = FT_BASE + PAGE_SIZE;

	/* Read fault: shared zero page, no anon accounting, metadata
	 * stays PRIVATE_ANON (sec 4.6 zero-page rule).
	 */
	KUNIT_EXPECT_EQ(test, ft_mark(t, addr, PAGE_SIZE, FT_PERM_RW), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_handle_mm_fault(t->vma, addr, 0,
							   NULL), 0);

	ptep = ft_pte(t, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));
	KUNIT_EXPECT_TRUE(test, pte_special(pte));
	KUNIT_EXPECT_EQ(test, pte_pfn(pte), my_zero_pfn(addr));
	KUNIT_EXPECT_EQ(test, get_mm_counter_sum(t->mm, MM_ANONPAGES), 0);

	KUNIT_EXPECT_EQ(test, ft_meta(t, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_PRIVATE_ANON);

	/* The write upgrade: real page replaces the zero page.  This is
	 * the branch whose PTE is NOT pte_none any more, i.e. the
	 * "replace a present zero-page PTE" path of map_anon.
	 */
	KUNIT_EXPECT_EQ(test, ft_write_fault(t, addr), 0);

	ptep = ft_pte(t, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));
	KUNIT_EXPECT_TRUE(test, pte_write(pte));
	KUNIT_EXPECT_FALSE(test, pte_special(pte));
	KUNIT_EXPECT_NE(test, pte_pfn(pte), my_zero_pfn(addr));
	KUNIT_EXPECT_EQ(test, get_mm_counter_sum(t->mm, MM_ANONPAGES), 1);

	KUNIT_EXPECT_EQ(test, ft_meta(t, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
}

static void corten_fault_test_sigsegv(struct kunit *test)
{
	/*
	 * The real fault chain needs the M2a descriptor-install hook,
	 * which is gated on corten_enabled_static(): on a corten=off boot
	 * every arena fault correctly falls back to the legacy path and
	 * there is nothing for these cases to observe.  They run in the
	 * corten=on verification boot; the pure dispatch/classifier cases
	 * above cover the corten=off contract (S4 degraded scope).
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "real fault chain requires corten=on");
	struct ft_mm *t = ft_setup(test);
	struct corten_txn txn;
	struct corten_pte_meta ro = {
		.state = CORTEN_PRIVATE_ANON,
		.perm = CORTEN_PERM_READ | CORTEN_PERM_USER,
		.flags = 0,
	};
	unsigned long ro_addr = FT_BASE + 3 * PAGE_SIZE;
	int ret;

	/* Read-only chunk: write is SEGV_ACCERR (same vm_fault_t), read
	 * goes to the zero page.  An instruction fault on an execute-
	 * never arena is likewise ACCERR, including on a never-seeded
	 * page (the fresh-allocation gate runs against the arena prot).
	 */
	ret = corten_lock_range(t->mm, ro_addr, PAGE_SIZE, &txn);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = corten_mark(&txn, ro_addr, PAGE_SIZE, &ro);
	corten_unlock(&txn);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_EQ(test, ft_write_fault(t, ro_addr), VM_FAULT_SIGSEGV);
	KUNIT_EXPECT_EQ(test, corten_arena_handle_mm_fault(t->vma, ro_addr, 0,
							   NULL), 0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_handle_mm_fault(t->vma,
					FT_BASE + 2 * PAGE_SIZE,
					FAULT_FLAG_INSTRUCTION, NULL),
			VM_FAULT_SIGSEGV);
}

/*
 * The first touch of a page the suite never seeded metadata for is the
 * paper's Fig.8 L26-38 fresh-allocation fault -- the synthesizing body
 * of the arena fault cycle.  The original suite missed it entirely: every
 * real-chain case pre-marked its pages, and both review and KUnit sailed
 * through while the guest deterministicly died with SEGV_MAPERR on the
 * first touch (r03 DoD failure mode A).  Lesson recorded here: every
 * real-chain case must cover at least one unseeded page.
 */
static void corten_fault_test_fresh_fault(struct kunit *test)
{
	/*
	 * The real fault chain needs the M2a descriptor-install hook,
	 * which is gated on corten_enabled_static(): on a corten=off boot
	 * every arena fault correctly falls back to the legacy path and
	 * there is nothing for these cases to observe.  They run in the
	 * corten=on verification boot; the pure dispatch/classifier cases
	 * above cover the corten=off contract (S4 degraded scope).
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "real fault chain requires corten=on");
	struct ft_mm *t = ft_setup(test);
	struct corten_pte_meta m;
	struct folio *folio;
	pte_t *ptep, pte;
	void *kvaddr;
	unsigned long waddr = FT_BASE + 6 * PAGE_SIZE;
	unsigned long raddr = FT_BASE + 7 * PAGE_SIZE;

	/* Write fault: synthesize the PrivateAnon allocation and install
	 * a real zeroed page; the metadata lands on MAPPED with the
	 * arena's permissions (not the zeroed junk the query saw).
	 */
	KUNIT_EXPECT_EQ(test, ft_write_fault(t, waddr), 0);

	KUNIT_EXPECT_EQ(test, ft_meta(t, waddr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, m.perm,
			CORTEN_PERM_READ | CORTEN_PERM_WRITE |
			CORTEN_PERM_USER);

	ptep = ft_pte(t, waddr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));
	KUNIT_EXPECT_TRUE(test, pte_write(pte));
	KUNIT_EXPECT_FALSE(test, pte_special(pte));

	folio = page_folio(pte_page(pte));
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 1);	/* PTE ref */
	KUNIT_EXPECT_EQ(test, folio_mapped(folio), 1);
	KUNIT_EXPECT_TRUE(test, folio_test_anon(folio));
	KUNIT_EXPECT_FALSE(test, folio_test_lru(folio));

	/* The synthesized page is usable memory, zeroed at allocation
	 * (P1-2): write a pattern from the kernel side (the KUnit thread
	 * cannot fault its own user memory) and read it back.
	 */
	kvaddr = kmap_local_page(folio_page(folio, 0));
	KUNIT_EXPECT_EQ(test, ((u32 *)kvaddr)[0], 0);
	memset(kvaddr, 0x5a, sizeof(u32));
	KUNIT_EXPECT_EQ(test, ((u32 *)kvaddr)[0], 0x5a5a5a5a);
	kunmap_local(kvaddr);

	KUNIT_EXPECT_EQ(test, get_mm_counter_sum(t->mm, MM_ANONPAGES), 1);

	/* Read fault on another never-seeded page: shared zero page, no
	 * anon accounting, metadata stays PRIVATE_ANON with the arena
	 * permissions recorded.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_handle_mm_fault(t->vma, raddr, 0,
							   NULL), 0);

	ptep = ft_pte(t, raddr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));
	KUNIT_EXPECT_TRUE(test, pte_special(pte));
	KUNIT_EXPECT_EQ(test, pte_pfn(pte), my_zero_pfn(raddr));
	KUNIT_EXPECT_EQ(test, get_mm_counter_sum(t->mm, MM_ANONPAGES), 1);

	KUNIT_EXPECT_EQ(test, ft_meta(t, raddr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_PRIVATE_ANON);
	KUNIT_EXPECT_EQ(test, m.perm,
			CORTEN_PERM_READ | CORTEN_PERM_WRITE |
			CORTEN_PERM_USER);

	/* Its write upgrade re-dispatches into the regular MAPPED
	 * machinery through the fresh path's second look.
	 */
	KUNIT_EXPECT_EQ(test, ft_write_fault(t, raddr), 0);
	KUNIT_EXPECT_EQ(test, get_mm_counter_sum(t->mm, MM_ANONPAGES), 2);
	KUNIT_EXPECT_EQ(test, ft_meta(t, raddr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
}

static void corten_fault_test_restore(struct kunit *test)
{
	/*
	 * The real fault chain needs the M2a descriptor-install hook,
	 * which is gated on corten_enabled_static(): on a corten=off boot
	 * every arena fault correctly falls back to the legacy path and
	 * there is nothing for these cases to observe.  They run in the
	 * corten=on verification boot; the pure dispatch/classifier cases
	 * above cover the corten=off contract (S4 degraded scope).
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "real fault chain requires corten=on");
	struct ft_mm *t = ft_setup(test);
	pte_t *ptep, pte;
	spinlock_t *ptl;
	pmd_t *pmdp;
	unsigned long addr = FT_BASE + 4 * PAGE_SIZE;

	KUNIT_EXPECT_EQ(test, ft_mark(t, addr, PAGE_SIZE, FT_PERM_RW), 0);
	KUNIT_EXPECT_EQ(test, ft_write_fault(t, addr), 0);

	/* Simulate a stale permission pair (what NUMA balancing's
	 * change_prot_numa() or an unhooked protnone writer leaves
	 * behind): drop the write bit under the PTE lock, metadata stays
	 * CORTEN_MAPPED.  The next write fault must self-heal through
	 * CORTEN_DISP_RESTORE.
	 */
	pmdp = pmd_offset(pud_offset(p4d_offset(pgd_offset(t->mm, addr),
						addr), addr), addr);
	ptep = pte_offset_map_lock(t->mm, pmdp, addr, &ptl);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	set_ptes(t->mm, addr, ptep, pte_wrprotect(pte), 1);
	pte_unmap_unlock(ptep, ptl);

	KUNIT_EXPECT_EQ(test, ft_write_fault(t, addr), 0);

	ptep = ft_pte(t, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));
	KUNIT_EXPECT_TRUE(test, pte_write(pte));
	KUNIT_EXPECT_EQ(test, get_mm_counter_sum(t->mm, MM_ANONPAGES), 1);
}

/* ------------------------------------------------------------------ *
 * Races: fill_upper double install, map retry (sec 4.4 / Fig.7)
 * ------------------------------------------------------------------
 */

struct ft_race {
	struct ft_mm *t;
	struct corten_arena *ar;	/* pinned for the workers */
	atomic_t ret_bad;
	struct completion done;		/* completed once per worker */
};

static int corten_fault_test_fill_worker(void *data)
{
	struct ft_race *r = data;
	int i;

	for (i = 0; i < 200 && !kthread_should_stop(); i++)
		if (corten_arena_fill_upper(r->ar, FT_BASE))
			atomic_inc(&r->ret_bad);
	cond_resched();

	complete(&r->done);
	while (!kthread_should_stop())
		schedule_timeout_idle(1);

	return 0;
}

static void corten_fault_test_fill_upper_race(struct kunit *test)
{
	/*
	 * The real fault chain needs the M2a descriptor-install hook,
	 * which is gated on corten_enabled_static(): on a corten=off boot
	 * every arena fault correctly falls back to the legacy path and
	 * there is nothing for these cases to observe.  They run in the
	 * corten=on verification boot; the pure dispatch/classifier cases
	 * above cover the corten=off contract (S4 degraded scope).
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "real fault chain requires corten=on");
	struct ft_mm *t = ft_setup(test);
	struct ft_race *r;
	struct task_struct *w0, *w1;
	struct corten_txn txn;
	unsigned long i;
	int ret;

	r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r);
	r->t = t;
	r->ar = corten_arena_lookup_get(t->mm, FT_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r->ar);
	atomic_set(&r->ret_bad, 0);
	init_completion(&r->done);

	w0 = kthread_run(corten_fault_test_fill_worker, r, "corten_ft_f0");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, w0);
	w1 = kthread_run(corten_fault_test_fill_worker, r, "corten_ft_f1");
	if (IS_ERR(w1)) {
		kthread_stop(w0);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, w1);
	}
	if (num_online_cpus() >= 2) {
		set_cpus_allowed_ptr(w0, cpumask_of(0));
		set_cpus_allowed_ptr(w1, cpumask_of(1 % num_online_cpus()));
	}

	/* One wait per worker: both must finish their loops. */
	wait_for_completion(&r->done);
	wait_for_completion(&r->done);
	kthread_stop(w1);
	kthread_stop(w0);
	percpu_ref_put(&r->ar->active);

	KUNIT_EXPECT_EQ(test, atomic_read(&r->ret_bad), 0);

	/* The window is exactly once tracked: the transaction locks it and
	 * the metadata array exists (512 slots).
	 */
	for (i = 0; i < 4; i++) {
		ret = corten_lock_range(t->mm, FT_BASE + i * PAGE_SIZE,
					PAGE_SIZE, &txn);
		KUNIT_EXPECT_EQ(test, ret, 0);
		corten_unlock(&txn);
	}

	/* And the fault chain works on top of the filled tables. */
	KUNIT_EXPECT_EQ(test, ft_mark(t, FT_BASE + PAGE_SIZE, PAGE_SIZE,
				      FT_PERM_RW), 0);
	KUNIT_EXPECT_EQ(test, ft_write_fault(t, FT_BASE + PAGE_SIZE), 0);
}

/*
 * Churn-content repro (r03 defect C): random 16K..2M chunks at 4K-grid
 * offsets inside the arena, driven through the exact gate-free funnel the
 * arena_stress syscalls take -- corten_arena_mmap_route() (the MAP_FIXED
 * mark), real write faults, in-place content readback, then
 * corten_arena_munmap_route() (the transactional chunk zap) -- followed by
 * the post-unmap zerocheck: a fresh remap must present no PTE at all
 * until a fault installs one, so a read can never see the previous
 * cycle's content.
 */
static void corten_fault_test_churn_repro(struct kunit *test)
{
	/*
	 * The real fault chain needs the M2a descriptor-install hook,
	 * which is gated on corten_enabled_static(): on a corten=off boot
	 * every arena fault correctly falls back to the legacy path and
	 * there is nothing for these cases to observe.  They run in the
	 * corten=on verification boot; the pure dispatch/classifier cases
	 * above cover the corten=off contract (S4 degraded scope).
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "real fault chain requires corten=on");
	struct ft_mm *t = ft_setup(test);
	unsigned long seed = 12345;
	unsigned int inplace_fails = 0, stale_fails = 0;
	int c;

	for (c = 0; c < 150; c++) {
		size_t len = 16384UL << ((seed >> 33) & 7);
		unsigned long off, base, n, i;

		seed = seed * 6364136223846793005ULL +
		       1442695040888963407ULL;
		off = ((seed >> 20) %
		       ((FT_ARENA_LEN - len) / 16384UL + 1)) * 16384UL;
		base = FT_BASE + off;
		n = len / PAGE_SIZE;

		/* chunk_map: the mark transaction of mmap(MAP_FIXED). */
		KUNIT_ASSERT_EQ(test,
				corten_arena_mmap_route(t->mm, base, len,
					PROT_READ | PROT_WRITE,
					MAP_FIXED | MAP_PRIVATE |
					MAP_ANONYMOUS | MAP_NORESERVE,
					false),
				1);

		/* touch + in-place readback: the write fault installs a
		 * private zeroed page (the kernel-side kmap store stands
		 * in for the user store -- the KUnit thread cannot fault
		 * its own user memory); the readback must return exactly
		 * what was stored, on the same translation.
		 */
		for (i = 0; i < n; i++) {
			unsigned long addr = base + i * PAGE_SIZE;
			pte_t *ptep;
			pte_t pte;
			uint64_t exp = 0x5a5a000000000000ULL ^
				       ((uint64_t)(c + 1) << 32) ^ i;
			uint64_t got = 0;
			void *kv;

			KUNIT_ASSERT_EQ(test, ft_write_fault(t, addr), 0);
			ptep = ft_pte(t, addr);
			KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
			pte = ptep_get(ptep);
			pte_unmap(ptep);
			KUNIT_ASSERT_TRUE(test, pte_present(pte));
			KUNIT_ASSERT_FALSE(test, pte_special(pte));
			kv = kmap_local_page(pte_page(pte));
			*(uint64_t *)kv = exp;
			got = *(uint64_t *)kv;
			kunmap_local(kv);
			if (got != exp)
				inplace_fails++;
		}

		/* munmap: the chunk-zap funnel of munmap(2). */
		KUNIT_ASSERT_EQ(test,
				corten_arena_munmap_route(t->mm, base, len),
				1);

		/* zerocheck: remap, then scan BEFORE any fault -- a
		 * present PTE here is the previous cycle's content
		 * surviving the munmap (the guest ZFAIL).
		 */
		KUNIT_ASSERT_EQ(test,
				corten_arena_mmap_route(t->mm, base, len,
					PROT_READ | PROT_WRITE,
					MAP_FIXED | MAP_PRIVATE |
					MAP_ANONYMOUS | MAP_NORESERVE,
					false),
				1);
		for (i = 0; i < n; i++) {
			pte_t *ptep = ft_pte(t, base + i * PAGE_SIZE);

			if (ptep && !pte_none(ptep_get(ptep)))
				stale_fails++;
			if (ptep)
				pte_unmap(ptep);
		}
		KUNIT_ASSERT_EQ(test,
				corten_arena_munmap_route(t->mm, base, len),
				1);
	}

	KUNIT_EXPECT_EQ(test, inplace_fails, 0);
	KUNIT_EXPECT_EQ(test, stale_fails, 0);
	if (inplace_fails || stale_fails)
		kunit_info(test, "churn repro: inplace=%u stale=%u\n",
			   inplace_fails, stale_fails);
}

static void corten_fault_test_free_page(void *ctx)
{
	put_page(ctx);
}

/*
 * Untracked-window behavior under a descriptor-install failure (r03
 * defect C, review follow-up): arm the allocator injection so the M2a
 * install inside pte_alloc_one() fails for both the fresh install and
 * the re-arm within the same fill_upper() -- the PT page comes to exist
 * but the window stays untracked.  Assert the whole contract: the
 * transaction layer reports -ENOENT; a legacy-written plain PTE (a
 * manually installed private page stands in for what the legacy fault
 * body writes) is still dropped by the munmap funnel -- content must
 * not survive an untracked munmap; the next fill_upper() re-arms the
 * descriptor (reinstalled counter) and the window becomes transactional
 * again; legacy_drift moves when drifted content is cleaned up.
 */
#ifdef CONFIG_CORTEN_MM_KUNIT_TEST
static void corten_fault_test_untracked_drift(struct kunit *test)
{
	/*
	 * The real fault chain needs the M2a descriptor-install hook,
	 * which is gated on corten_enabled_static(): on a corten=off boot
	 * every arena fault correctly falls back to the legacy path and
	 * there is nothing for these cases to observe.  They run in the
	 * corten=on verification boot; the pure dispatch/classifier cases
	 * above cover the corten=off contract (S4 degraded scope).
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "real fault chain requires corten=on");
	struct ft_mm *t = ft_setup(test);
	unsigned long win = FT_BASE + PMD_SIZE;
	unsigned long baseline_anon;
	long rein0, drift0;
	struct vm_area_struct *vma = t->vma;
	struct page *page;
	struct corten_txn txn;
	struct folio *folio;
	pmd_t *pmdp;
	pte_t *ptep;
	pte_t entry;
	spinlock_t *ptl;

	rein0 = corten_ptdesc_reinstalled_count();
	drift0 = corten_legacy_drift_count();
	baseline_anon = get_mm_counter_sum(t->mm, MM_ANONPAGES);

	/* ① fill with both installs failing: PT page exists, window
	 * untracked (two failures: the fresh install inside pte_alloc
	 * and the re-arm at the end of the same fill_upper()).
	 */
	corten_test_inject_alloc_fail(2);
	{
		struct corten_arena *ar = corten_arena_lookup_get(t->mm, win);

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ar);
		KUNIT_ASSERT_EQ(test, corten_arena_fill_upper(ar, win), 0);
		percpu_ref_put(&ar->active);
	}
	corten_test_inject_alloc_fail(0);

	/* (a) the transaction layer reports the window untracked: the PT
	 * page exists but has no descriptor, which lock_range reports as
	 * -EOPNOTSUPP (a plain hole is -ENOENT).
	 */
	KUNIT_EXPECT_EQ(test, corten_lock_range(t->mm, win, PAGE_SIZE, &txn),
			-EOPNOTSUPP);

	/* (b) simulate the legacy fallback's PTE: a plain private page
	 * with rmap and accounting, no metadata behind it.
	 */
	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	kunit_add_action(test, corten_fault_test_free_page, page);
	folio = page_folio(page);
	add_mm_counter(t->mm, MM_ANONPAGES, 1);
	folio_add_new_anon_rmap(folio, vma, win, RMAP_EXCLUSIVE);
	entry = mk_pte(page, vma->vm_page_prot);
	entry = pte_sw_mkyoung(entry);
	entry = pte_mkwrite(pte_mkdirty(entry), vma);

	pmdp = pmd_offset(pud_offset(p4d_offset(pgd_offset(t->mm, win),
						win), win), win);
	ptep = pte_offset_map_lock(t->mm, pmdp, win, &ptl);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	set_ptes(t->mm, win, ptep, entry, 1);
	pte_unmap_unlock(ptep, ptl);

	/* munmap on the untracked window: the chunk funnel takes the
	 * -ENOENT branch and must zap by content -- the page is released
	 * and the drift counter moves.
	 */
	KUNIT_ASSERT_EQ(test, corten_arena_munmap_route(t->mm, win,
							PAGE_SIZE), 1);
	kunit_release_action(test, corten_fault_test_free_page, page);

	ptep = ft_pte(t, win);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	KUNIT_EXPECT_TRUE(test, pte_none(ptep_get(ptep)));
	pte_unmap(ptep);
	KUNIT_EXPECT_EQ(test, get_mm_counter_sum(t->mm, MM_ANONPAGES),
			baseline_anon);
	KUNIT_EXPECT_GT(test, corten_legacy_drift_count(), drift0);

	/* (c) heal: the next fill re-arms the descriptor (reinstalled
	 * counter) and the window becomes transactional again.
	 */
	{
		struct corten_arena *ar = corten_arena_lookup_get(t->mm, win);

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ar);
		KUNIT_ASSERT_EQ(test, corten_arena_fill_upper(ar, win), 0);
		percpu_ref_put(&ar->active);
	}
	KUNIT_EXPECT_GT(test, corten_ptdesc_reinstalled_count(), rein0);

	KUNIT_ASSERT_EQ(test, ft_mark(t, win, PAGE_SIZE, FT_PERM_RW), 0);
	KUNIT_EXPECT_EQ(test, ft_write_fault(t, win), 0);
}
#endif /* CONFIG_CORTEN_MM_KUNIT_TEST */

static int corten_fault_test_map_worker(void *data)
{
	struct ft_race *r = data;
	int i;

	for (i = 0; i < FT_NPAGES && !kthread_should_stop(); i++)
		if (ft_write_fault(r->t, FT_BASE + i * PAGE_SIZE))
			atomic_inc(&r->ret_bad);
	cond_resched();

	complete(&r->done);
	while (!kthread_should_stop())
		schedule_timeout_idle(1);

	return 0;
}

/*
 * Two threads write-fault the same pages concurrently: both must succeed
 * (the loser of the pte_none() re-check retries and re-dispatches), and
 * each page ends mapped exactly once.
 */
static void corten_fault_test_map_race(struct kunit *test)
{
	/*
	 * The real fault chain needs the M2a descriptor-install hook,
	 * which is gated on corten_enabled_static(): on a corten=off boot
	 * every arena fault correctly falls back to the legacy path and
	 * there is nothing for these cases to observe.  They run in the
	 * corten=on verification boot; the pure dispatch/classifier cases
	 * above cover the corten=off contract (S4 degraded scope).
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "real fault chain requires corten=on");
	struct ft_mm *t = ft_setup(test);
	struct ft_race *r;
	struct task_struct *w0, *w1;
	struct corten_pte_meta m;
	unsigned long addr;
	int i;

	r = kunit_kzalloc(test, sizeof(*r), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r);
	r->t = t;
	r->ar = corten_arena_lookup_get(t->mm, FT_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, r->ar);
	atomic_set(&r->ret_bad, 0);
	init_completion(&r->done);

	/* The mmap leg: all pages virtually allocated before the race. */
	KUNIT_ASSERT_EQ(test, ft_mark(t, FT_BASE, FT_NPAGES * PAGE_SIZE,
				      FT_PERM_RW), 0);

	w0 = kthread_run(corten_fault_test_map_worker, r, "corten_ft_m0");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, w0);
	w1 = kthread_run(corten_fault_test_map_worker, r, "corten_ft_m1");
	if (IS_ERR(w1)) {
		kthread_stop(w0);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, w1);
	}
	if (num_online_cpus() >= 2) {
		set_cpus_allowed_ptr(w0, cpumask_of(0));
		set_cpus_allowed_ptr(w1, cpumask_of(1 % num_online_cpus()));
	}

	wait_for_completion(&r->done);
	wait_for_completion(&r->done);
	kthread_stop(w1);
	kthread_stop(w0);
	percpu_ref_put(&r->ar->active);

	KUNIT_EXPECT_EQ(test, atomic_read(&r->ret_bad), 0);
	KUNIT_EXPECT_EQ(test, get_mm_counter_sum(t->mm, MM_ANONPAGES), FT_NPAGES);

	for (i = 0; i < FT_NPAGES; i++) {
		addr = FT_BASE + i * PAGE_SIZE;

		KUNIT_EXPECT_EQ(test, ft_meta(t, addr, &m), 0);
		KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	}
}

/* ------------------------------------------------------------------ *
 * S6: transactional chunk unmap (content drop, VA kept)
 * ------------------------------------------------------------------
 */

static void corten_fault_test_chunk_unmap(struct kunit *test)
{
	/*
	 * The real fault chain needs the M2a descriptor-install hook,
	 * which is gated on corten_enabled_static(): on a corten=off boot
	 * every arena fault correctly falls back to the legacy path and
	 * there is nothing for these cases to observe.  They run in the
	 * corten=on verification boot; the pure dispatch/classifier cases
	 * above cover the corten=off contract (S4 degraded scope).
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "real fault chain requires corten=on");
	struct ft_mm *t = ft_setup(test);
	struct corten_arena *ar;
	struct corten_pte_meta m;
	pte_t *ptep, pte;
	unsigned long addr = FT_BASE + 5 * PAGE_SIZE;

	KUNIT_EXPECT_EQ(test, ft_mark(t, addr, 2 * PAGE_SIZE, FT_PERM_RW), 0);
	KUNIT_EXPECT_EQ(test, ft_write_fault(t, addr), 0);
	ptep = ft_pte(t, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));

	ar = corten_arena_lookup_get(t->mm, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ar);

	/* Drop a two-page range covering the mapped page plus one
	 * never-touched page (the INVALID slot must be tolerated).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_unmap_chunk(t->mm, ar, addr,
						       2 * PAGE_SIZE), 0);
	percpu_ref_put(&ar->active);

	/* PTE gone, metadata back to INVALID, accounting closed. */
	ptep = ft_pte(t, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_none(pte));

	KUNIT_EXPECT_EQ(test, ft_meta(t, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);

	KUNIT_EXPECT_EQ(test, get_mm_counter_sum(t->mm, MM_ANONPAGES), 0);
}

/*
 * T0b (M4T0_SPEC.md sec 3.3): routed mprotect on a live arena page.
 * The recorded perm moves in the same transaction and the already
 * installed PTE is rewritten to the matching encoding (TLB flushed) --
 * a downgrade must deny the next write with SEGV_ACCERR from the
 * metadata gate, an upgrade must let it through again.
 */
static void corten_fault_test_mprotect_pte(struct kunit *test)
{
	/*
	 * The route is gated on corten_enabled_static() and the desc
	 * locks only exist on a corten=on boot (same scope note as the
	 * other real-chain cases above).
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "real fault chain requires corten=on");
	struct ft_mm *t = ft_setup(test);
	unsigned long addr = FT_BASE + 5 * PAGE_SIZE;
	struct corten_pte_meta m;
	pte_t *ptep, pte;

	/* The route serves MODE-process takeover; mode_enter is the
	 * gate-free internal step (the registry exists after DECLARE).
	 */
	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(t->mm), 0);

	KUNIT_ASSERT_EQ(test, ft_mark(t, addr, PAGE_SIZE, FT_PERM_RW), 0);
	KUNIT_ASSERT_EQ(test, ft_write_fault(t, addr), 0);

	/* Downgrade to read-only: metadata and live PTE move together. */
	KUNIT_ASSERT_EQ(test,
			corten_arena_mprotect_route(t->mm, addr, PAGE_SIZE,
						    PROT_READ, -1), 1);
	KUNIT_EXPECT_EQ(test, ft_meta(t, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_PERM_READ | CORTEN_PERM_USER);

	ptep = ft_pte(t, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));
	KUNIT_EXPECT_FALSE(test, pte_write(pte));

	/* The next write is denied by the metadata gate (ACCERR). */
	KUNIT_EXPECT_NE(test, ft_write_fault(t, addr), 0);

	/* Upgrade back: the fault path rebuilds the writable PTE. */
	KUNIT_ASSERT_EQ(test,
			corten_arena_mprotect_route(t->mm, addr, PAGE_SIZE,
						    PROT_READ | PROT_WRITE,
						    -1), 1);
	KUNIT_EXPECT_EQ(test, ft_write_fault(t, addr), 0);
}

/* The JVM reserve+commit shape (D-G regression anchor): a PROT_NONE
 * reservation declares the arena -- the FRESH gate's upper bound
 * (ar->prot) carries no R/W and no window has a tracked PT page yet --
 * and then mprotect() commits a prefix.  The commit must be visible to
 * the first fault inside the prefix (pending perm), while the rest of
 * the reservation keeps the inaccessible contract (first write ACCERRs
 * instead of installing a page behind the stale bound).
 */
static void corten_fault_test_mprotect_fresh(struct kunit *test)
{
	struct ft_mm *t;
	unsigned long addr;
	struct corten_pte_meta m;
	pte_t *ptep, pte;

	if (!corten_enabled_static())
		kunit_skip(test, "real fault chain requires corten=on");

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);
	kunit_add_action(test, ft_mm_destroy, t);

	t->mm = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->mm);

	/* The reservation: PROT_NONE (no VM_READ/VM_WRITE), so DECLARE
	 * records an arena upper bound without R/W.
	 */
	t->vma = ft_mkvm(t->mm, FT_BASE, FT_BASE + FT_ARENA_LEN,
			 FT_FLAGS_OK & ~(VM_READ | VM_WRITE));
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->vma);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(t->mm, FT_BASE, FT_ARENA_LEN), 0);
	t->ar_start = FT_BASE;
	t->ar_end = FT_BASE + FT_ARENA_LEN;

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(t->mm), 0);

	/* Commit a 33-page prefix through the route: no window of the
	 * arena has a tracked PT page, so this exercises the fill +
	 * pending-perm path, not the live-PTE rewrite.
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_mprotect_route(t->mm, FT_BASE,
						    33 * PAGE_SIZE,
						    PROT_READ | PROT_WRITE,
						    -1), 1);

	/* First touch in the committed prefix: gated by the pending perm
	 * (not the stale bound), and the install is writable.
	 */
	addr = FT_BASE + PAGE_SIZE;
	KUNIT_ASSERT_EQ(test, ft_write_fault(t, addr), 0);
	KUNIT_EXPECT_EQ(test, ft_meta(t, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, m.perm, FT_PERM_RW);
	ptep = ft_pte(t, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte) && pte_write(pte));

	/* Outside the prefix the reservation is still PROT_NONE: the
	 * first write must be denied, not installed behind the commit.
	 */
	KUNIT_EXPECT_NE(test,
			ft_write_fault(t, FT_BASE + 96 * PAGE_SIZE), 0);
}

/* The "pmd present, descriptor missing" shape (D-G' regression anchor):
 * the window's PT page exists but its descriptor install failed
 * (GFP_NOWAIT) or the descriptor was dropped, so the window is
 * untracked even though the upper tables exist.  The mprotect route and
 * the fault path must re-arm the descriptor and recover the transaction
 * instead of silently degrading to the legacy body.
 */
static void corten_fault_test_untracked_rearm(struct kunit *test)
{
	struct ft_mm *t = ft_setup(test);
	unsigned long win = FT_BASE + PMD_SIZE;
	unsigned long addr = win + PAGE_SIZE;
	long rein0;
	struct corten_pte_meta m;
	struct corten_txn txn_;
	pte_t *ptep, pte;

	if (!corten_enabled_static())
		kunit_skip(test, "real fault chain requires corten=on");

	/* Window 1 is untouched by ft_setup (only window 0 is filled):
	 * fill it with both installs failing (the fresh install inside
	 * pte_alloc_one and the re-arm at the end of fill_upper) -- the
	 * pmd stays populated but the window is untracked, and
	 * lock_range() reports -EOPNOTSUPP for it (a plain hole is
	 * -ENOENT).  The failed begin holds no locks, so the probe leaks
	 * nothing on the expected-failure path.
	 */
	rein0 = corten_ptdesc_reinstalled_count();

	corten_test_inject_alloc_fail(2);
	{
		struct corten_arena *ar = corten_arena_lookup_get(t->mm, win);

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ar);
		KUNIT_ASSERT_EQ(test, corten_arena_fill_upper(ar, win), 0);
		percpu_ref_put(&ar->active);
	}
	corten_test_inject_alloc_fail(0);
	KUNIT_EXPECT_EQ(test, corten_lock_range(t->mm, win, PAGE_SIZE, &txn_),
			-EOPNOTSUPP);

	/* The mprotect route recovers the window: its -EOPNOTSUPP arm
	 * re-arms the descriptor (install succeeds with injection off),
	 * locks, and records the pending perm like a tracked window.
	 */
	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(t->mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_mprotect_route(t->mm, win, 33 * PAGE_SIZE,
						    PROT_READ | PROT_WRITE,
						    -1), 1);
	KUNIT_EXPECT_GT(test, corten_ptdesc_reinstalled_count(), rein0);
	KUNIT_EXPECT_EQ(test, ft_meta(t, win, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_PRIVATE_ANON);
	KUNIT_EXPECT_EQ(test, m.perm, FT_PERM_RW);

	/* The recovered window is transactional: the first fault installs
	 * a writable page instead of falling back to the legacy body.
	 */
	KUNIT_ASSERT_EQ(test, ft_write_fault(t, addr), 0);
	KUNIT_EXPECT_EQ(test, ft_meta(t, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	ptep = ft_pte(t, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte) && pte_write(pte));
}

static struct kunit_case corten_fault_test_cases[] = {
	KUNIT_CASE(corten_fault_test_dispatch),
	KUNIT_CASE(corten_fault_test_unmap_classify),
	KUNIT_CASE(corten_fault_test_mmap_classify),
	KUNIT_CASE(corten_fault_test_map_anon),
	KUNIT_CASE(corten_fault_test_zero_page),
	KUNIT_CASE(corten_fault_test_sigsegv),
	KUNIT_CASE(corten_fault_test_fresh_fault),
	KUNIT_CASE(corten_fault_test_churn_repro),
#ifdef CONFIG_CORTEN_MM_KUNIT_TEST
	KUNIT_CASE(corten_fault_test_untracked_drift),
#endif
	KUNIT_CASE(corten_fault_test_restore),
	KUNIT_CASE(corten_fault_test_mprotect_pte),
	KUNIT_CASE(corten_fault_test_mprotect_fresh),
	KUNIT_CASE(corten_fault_test_untracked_rearm),
	KUNIT_CASE(corten_fault_test_fill_upper_race),
	KUNIT_CASE(corten_fault_test_map_race),
	KUNIT_CASE(corten_fault_test_chunk_unmap),
	{}
};

static struct kunit_suite corten_fault_test_suite = {
	.name = "corten_fault",
	.test_cases = corten_fault_test_cases,
};

kunit_test_suite(corten_fault_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for the CortenMM arena fault path");
