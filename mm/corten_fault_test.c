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
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/mm_inline.h>
#include <linux/pgtable.h>
#include <linux/sched.h>
#include <linux/slab.h>

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
		/* CORTEN_INVALID: never marked -> undeclared access. */
		{ "invalid-read", { CORTEN_INVALID, CORTEN_PERM_ALL, 0, { 0 } },
		  false, false, CORTEN_DISP_MAPERR },
		{ "invalid-write", { CORTEN_INVALID, CORTEN_PERM_ALL, 0, { 0 } },
		  true, false, CORTEN_DISP_MAPERR },
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
	unsigned long unmarked = FT_BASE + 2 * PAGE_SIZE;
	unsigned long ro_addr = FT_BASE + 3 * PAGE_SIZE;
	int ret;

	/* Never marked: SEGV_MAPERR semantics. */
	KUNIT_EXPECT_EQ(test, ft_write_fault(t, unmarked), VM_FAULT_SIGSEGV);
	KUNIT_EXPECT_EQ(test, corten_arena_handle_mm_fault(t->vma, unmarked,
							   0, NULL),
			VM_FAULT_SIGSEGV);

	/* Read-only chunk: write is SEGV_ACCERR (same vm_fault_t), read
	 * goes to the zero page.
	 */
	ret = corten_lock_range(t->mm, ro_addr, PAGE_SIZE, &txn);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = corten_mark(&txn, ro_addr, PAGE_SIZE, &ro);
	corten_unlock(&txn);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_EQ(test, ft_write_fault(t, ro_addr), VM_FAULT_SIGSEGV);
	KUNIT_EXPECT_EQ(test, corten_arena_handle_mm_fault(t->vma, ro_addr, 0,
							   NULL), 0);
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

static struct kunit_case corten_fault_test_cases[] = {
	KUNIT_CASE(corten_fault_test_dispatch),
	KUNIT_CASE(corten_fault_test_unmap_classify),
	KUNIT_CASE(corten_fault_test_mmap_classify),
	KUNIT_CASE(corten_fault_test_map_anon),
	KUNIT_CASE(corten_fault_test_zero_page),
	KUNIT_CASE(corten_fault_test_sigsegv),
	KUNIT_CASE(corten_fault_test_restore),
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
