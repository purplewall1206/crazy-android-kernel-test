// SPDX-License-Identifier: GPL-2.0
/*
 * CortenMM arena layer tests (M3B_DESIGN.md sec 7.4): DECLARE validation
 * chain, per-mm xarray indexing, QUERY, RELEASE drain, shadow-VMA
 * conversion and process-exit cleanup.
 *
 * Like mm/corten_test.c this suite drives the gate-free internal entry
 * points (corten_arena_declare/release/query, corten_arena_lookup), so it
 * runs on a kernel booted with corten=off.  A real mm_struct is built with
 * mm_alloc() and populated with plain private-anonymous VMAs inserted
 * directly into its maple tree -- no userspace address space is needed.
 */
#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/corten_arena.h>
#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/mm_inline.h>
#include <linux/pgtable.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <uapi/linux/prctl.h>

#include "internal.h"
#include "vma.h"

/* Test layout: arena windows in the low user range, PMD-aligned. */
#define CORTEN_ARENA_TEST_BASE		(4UL * PMD_SIZE)
#define CORTEN_ARENA_TEST_LEN		(4UL * PMD_SIZE)
#define CORTEN_ARENA_TEST_LEN2		(2UL * PMD_SIZE)
#define CORTEN_ARENA_TEST_START2	(CORTEN_ARENA_TEST_BASE + 8UL * PMD_SIZE)
#define CORTEN_ARENA_TEST_NOWHERE	(CORTEN_ARENA_TEST_BASE + 64UL * PMD_SIZE)

/* A fresh MAP_NORESERVE-shaped private anonymous VMA (the DECLARE
 * contract's exact precondition).
 */
#define CORTEN_ARENA_TEST_FLAGS_OK	(VM_READ | VM_WRITE | \
					 VM_MAYREAD | VM_MAYWRITE | \
					 VM_NORESERVE)

struct corten_arena_test_mm {
	struct kunit *test;
	struct mm_struct *mm;
};

/* Insert one synthetic VMA into @mm's maple tree; @mm is exclusively
 * owned by the test, but the write lock is taken anyway because the maple
 * tree uses it as its external lock (vma_link() needs it for the
 * preallocation and the per-VMA write mark).
 */
static struct vm_area_struct *
corten_arena_test_mkvm(struct mm_struct *mm, unsigned long start,
		       unsigned long end, vm_flags_t flags)
{
	struct vm_area_struct *vma;
	int ret;

	vma = vm_area_alloc(mm);
	if (!vma)
		return NULL;

	vma_set_range(vma, start, end, 0);
	/* The vma cache is TYPESAFE_BY_RCU: fresh VMAs carry dummy vm_ops
	 * until marked anonymous, exactly like the mmap() path does.
	 */
	vma_set_anonymous(vma);
	vm_flags_init(vma, flags);

	mmap_write_lock(mm);
	ret = vma_link(mm, vma);
	mmap_write_unlock(mm);
	if (ret) {
		vm_area_free(vma);
		return NULL;
	}

	return vma;
}

static void corten_arena_test_unmap(struct mm_struct *mm, unsigned long start,
				    unsigned long len)
{
	mmap_write_lock(mm);
	do_munmap(mm, start, len, NULL);
	mmap_write_unlock(mm);
}

/* Tree-level VMA removal for synthetic bad-flag VMAs: their flag
 * combinations cannot exist in reality (fake VM_HUGETLB/VM_PKEY...), so
 * they must not go through the full munmap semantics; plain bookkeeping
 * keeps exit_mmap()'s map_count check happy.  Nothing can race the
 * exclusively-owned test mm.
 */
static void corten_arena_test_drop_vma(struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;

	mmap_write_lock(mm);
	{
		VMA_ITERATOR(vmi, mm, vma->vm_start);

		vma_iter_config(&vmi, vma->vm_start, vma->vm_end);
		if (!vma_iter_prealloc(&vmi, NULL)) {
			vma_iter_clear(&vmi);
			mm->map_count--;
		}
	}
	mmap_write_unlock(mm);
	vma_mark_detached(vma);
	vm_area_free(vma);
}

static void corten_arena_test_mm_destroy(void *ctx)
{
	struct corten_arena_test_mm *t = ctx;

	if (t->mm)
		mmput(t->mm);
}

/* One private mm with one plain VMA spanning the primary test range. */
static struct corten_arena_test_mm *
corten_arena_test_mm_setup(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct vm_area_struct *vma;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);
	kunit_add_action(test, corten_arena_test_mm_destroy, t);

	t->test = test;
	t->mm = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->mm);

	vma = corten_arena_test_mkvm(t->mm, CORTEN_ARENA_TEST_BASE,
				     CORTEN_ARENA_TEST_BASE +
				     CORTEN_ARENA_TEST_LEN,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);

	return t;
}

/*
 * The KUnit case runs in its own kthread (kunit_try_catch) whose mm is
 * NULL, while the cleanup actions run in the parent thread -- so the case
 * thread must not attach itself to @mm with kthread_use_mm() (it could
 * never detach safely).  RELEASE's legacy munmap completion and the test's
 * own do_munmap() teardown, however, read current->mm.  Anything that
 * munmaps therefore runs on the dedicated, fully self-managed worker
 * below; the case thread joins it and asserts on the recorded result.
 */
struct corten_arena_test_op {
	struct mm_struct *mm;
	void (*fn)(struct corten_arena_test_op *o);
	int ret;
	unsigned long addr;
	unsigned long len;
	struct completion done;
};

static void corten_arena_test_op_release(struct corten_arena_test_op *o)
{
	o->ret = corten_arena_release(o->mm, o->addr, o->len);
}

static int corten_arena_test_op_thread(void *data)
{
	struct corten_arena_test_op *o = data;

	kthread_use_mm(o->mm);
	o->fn(o);
	kthread_unuse_mm(o->mm);
	complete(&o->done);

	return 0;
}

static int corten_arena_test_run_op(struct kunit *test, struct mm_struct *mm,
				    void (*fn)(struct corten_arena_test_op *),
				    unsigned long addr, unsigned long len)
{
	struct corten_arena_test_op *o;
	struct task_struct *tsk;

	o = kunit_kzalloc(test, sizeof(*o), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, o);

	o->mm = mm;
	o->fn = fn;
	o->addr = addr;
	o->len = len;
	init_completion(&o->done);

	tsk = kthread_run(corten_arena_test_op_thread, o, "corten_arena_op");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tsk);
	wait_for_completion(&o->done);

	return o->ret;
}

/* ------------------------------------------------------------------ *
 * DECLARE validation chain (sec 2.1 contract, one case per line)
 * ------------------------------------------------------------------
 */

static void corten_arena_test_declare_reject(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;

	/* QUERY on an mm that never declared: -ENOENT. */
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_BASE),
			-ENOENT);

	/* Misaligned address / bad length. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE +
						  PAGE_SIZE,
					     CORTEN_ARENA_TEST_LEN),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE, 0),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN +
					     PAGE_SIZE),
			-EINVAL);

	/* No VMA at the address at all. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_NOWHERE,
					     CORTEN_ARENA_TEST_LEN2),
			-EINVAL);

	/* Partial range: the DECLARE must equal one VMA exactly. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN2),
			-EINVAL);

	/* The registry is created at the first DECLARE attempt (its ctl_lock
	 * lives in it, so it must exist before validation serializes) and
	 * is empty after a rejected DECLARE.  That is invisible to the
	 * contract: QUERY answers 0 ("not inside an arena"), not -ENOENT
	 * (which is reserved for mms that never attempted a DECLARE), and
	 * the lookup short-circuits on the zero arena count.
	 */
	KUNIT_EXPECT_NOT_NULL(test, READ_ONCE(mm->corten_state));
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_BASE),
			0);
}

static void corten_arena_test_declare_reject_flags(struct kunit *test)
{
	static const vm_flags_t bad_flag_table[] = {
		VM_ACCOUNT,		/* missing MAP_NORESERVE */
		VM_SHARED,		/* not private */
		VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP, /* special */
		VM_HUGETLB,
		VM_SEQ_READ,
		VM_RAND_READ,
		VM_GROWSDOWN,
		VM_LOCKED,
		VM_LOCKONFAULT,
		VM_MERGEABLE,
#ifdef CONFIG_64BIT
		VM_SEALED,
#endif
#ifdef CONFIG_ARCH_HAS_PKEYS
		VM_PKEY_BIT0,
#endif
#if defined(CONFIG_X86_USER_SHADOW_STACK) || defined(CONFIG_ARM64_GCS)
		VM_SHADOW_STACK,
#endif
#ifdef CONFIG_USERFAULTFD
		VM_UFFD_MISSING,
		VM_UFFD_WP,
#endif
	};
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	size_t i;

	BUILD_BUG_ON(ARRAY_SIZE(bad_flag_table) == 0);

	for (i = 0; i < ARRAY_SIZE(bad_flag_table); i++) {
		struct vm_area_struct *vma;

		vma = corten_arena_test_mkvm(t->mm, CORTEN_ARENA_TEST_NOWHERE,
					     CORTEN_ARENA_TEST_NOWHERE +
					     CORTEN_ARENA_TEST_LEN2,
					     CORTEN_ARENA_TEST_FLAGS_OK |
					     bad_flag_table[i]);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);

		KUNIT_EXPECT_EQ_MSG(test,
				    corten_arena_declare(t->mm,
							 CORTEN_ARENA_TEST_NOWHERE,
							 CORTEN_ARENA_TEST_LEN2),
				    -EINVAL,
				    "flag combination %#lx must be rejected",
				    (unsigned long)bad_flag_table[i]);

		corten_arena_test_drop_vma(vma);
	}

#ifdef CONFIG_USERFAULTFD
	/* An attached (fake) userfaultfd context is rejected even though it
	 * need not come with VM_UFFD_* flags.
	 */
	{
		struct vm_area_struct *vma =
			corten_arena_test_mkvm(t->mm,
					       CORTEN_ARENA_TEST_NOWHERE,
					       CORTEN_ARENA_TEST_NOWHERE +
					       CORTEN_ARENA_TEST_LEN2,
					       CORTEN_ARENA_TEST_FLAGS_OK);

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
		vma->vm_userfaultfd_ctx.ctx = (struct userfaultfd_ctx *)vma;
		KUNIT_EXPECT_EQ(test,
				corten_arena_declare(t->mm,
						     CORTEN_ARENA_TEST_NOWHERE,
						     CORTEN_ARENA_TEST_LEN2),
				-EINVAL);
		corten_arena_test_drop_vma(vma);
	}
#endif

	/* The one tolerated extra bit: transient soft-dirty tracking
	 * (vma_merge excludes it from flag comparisons for the same
	 * reason), so DECLARE must still succeed with it set.
	 */
	{
		struct vm_area_struct *vma =
			corten_arena_test_mkvm(t->mm,
					       CORTEN_ARENA_TEST_NOWHERE,
					       CORTEN_ARENA_TEST_NOWHERE +
					       CORTEN_ARENA_TEST_LEN2,
					       CORTEN_ARENA_TEST_FLAGS_OK |
					       VM_SOFTDIRTY);

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
		KUNIT_EXPECT_EQ(test,
				corten_arena_declare(t->mm,
						     CORTEN_ARENA_TEST_NOWHERE,
						     CORTEN_ARENA_TEST_LEN2),
				0);
		KUNIT_EXPECT_EQ(test,
				corten_arena_test_run_op(test, t->mm,
							 corten_arena_test_op_release,
							 CORTEN_ARENA_TEST_NOWHERE,
							 CORTEN_ARENA_TEST_LEN2),
				0);
	}
}

/* ------------------------------------------------------------------ *
 * Happy path: DECLARE, the 2M-frame xarray index, QUERY and lookup
 * ------------------------------------------------------------------
 */

static void corten_arena_test_declare_query(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct vm_area_struct *vma2;
	struct corten_arena *ar1, *ar2;

	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_NOT_NULL(test, READ_ONCE(mm->corten_state));

	/* QUERY: 1 at both ends and in every frame of the arena, 0 around
	 * it.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_BASE),
			1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_query(mm,
					   CORTEN_ARENA_TEST_BASE +
					   2 * PMD_SIZE + PAGE_SIZE),
			1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_query(mm,
					   CORTEN_ARENA_TEST_BASE +
					   CORTEN_ARENA_TEST_LEN - 1),
			1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_query(mm,
					   CORTEN_ARENA_TEST_BASE - PMD_SIZE),
			0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_query(mm,
					   CORTEN_ARENA_TEST_BASE +
					   CORTEN_ARENA_TEST_LEN),
			0);

	/* Lookup contents: RCU-protected read of the descriptor. */
	rcu_read_lock();
	ar1 = corten_arena_lookup(mm, CORTEN_ARENA_TEST_BASE + 3 * PMD_SIZE);
	KUNIT_EXPECT_NOT_NULL(test, ar1);
	if (ar1) {
		KUNIT_EXPECT_EQ(test, ar1->start, CORTEN_ARENA_TEST_BASE);
		KUNIT_EXPECT_EQ(test, ar1->end,
				CORTEN_ARENA_TEST_BASE +
				CORTEN_ARENA_TEST_LEN);
		KUNIT_EXPECT_EQ(test, ar1->prot,
				CORTEN_PERM_USER | CORTEN_PERM_READ |
				CORTEN_PERM_WRITE);
		KUNIT_EXPECT_PTR_EQ(test, ar1->mm, mm);
	}
	KUNIT_EXPECT_NULL(test,
			  corten_arena_lookup(mm,
					      CORTEN_ARENA_TEST_BASE +
					      CORTEN_ARENA_TEST_LEN +
					      PMD_SIZE));
	rcu_read_unlock();

	/* A second, disjoint arena: independent descriptor, independent
	 * frames.
	 */
	vma2 = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_START2,
				      CORTEN_ARENA_TEST_START2 +
				      CORTEN_ARENA_TEST_LEN2,
				      CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma2);
	KUNIT_EXPECT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_START2,
					     CORTEN_ARENA_TEST_LEN2),
			0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_query(mm,
					   CORTEN_ARENA_TEST_START2 +
					   PAGE_SIZE),
			1);

	rcu_read_lock();
	ar2 = corten_arena_lookup(mm, CORTEN_ARENA_TEST_START2);
	KUNIT_EXPECT_NOT_NULL(test, ar2);
	KUNIT_EXPECT_FALSE(test, ar1 && ar2 && ar1 == ar2);
	rcu_read_unlock();

	/* Overlap rejection takes precedence over the VMA validation (the
	 * frame check runs under ctl_lock before the VMA is even looked
	 * at), so sub-range and cross-border candidates fail with -EEXIST
	 * exactly like the exact re-declare.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			-EEXIST);
	KUNIT_EXPECT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE +
						  2 * PMD_SIZE,
					     CORTEN_ARENA_TEST_LEN2),
			-EEXIST);
	KUNIT_EXPECT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE -
						  PMD_SIZE,
					     2 * PMD_SIZE),
			-EEXIST);
}

/* ------------------------------------------------------------------ *
 * RELEASE: exact-match contract, drain and teardown
 * ------------------------------------------------------------------
 */

static void corten_arena_test_release(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;

	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);

	/* RELEASE must hit the declared range exactly (by start). */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_release,
						 CORTEN_ARENA_TEST_BASE + PMD_SIZE,
						 CORTEN_ARENA_TEST_LEN - PMD_SIZE),
			-ENOENT);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_release,
						 CORTEN_ARENA_TEST_NOWHERE,
						 CORTEN_ARENA_TEST_LEN2),
			-ENOENT);

	/* The real thing: drain + teardown. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_release,
						 CORTEN_ARENA_TEST_BASE,
						 CORTEN_ARENA_TEST_LEN),
			0);

	/* Arena gone; the shadow-VMA went with the legacy munmap; the
	 * registry survives (empty) for future DECLAREs.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_BASE),
			0);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_BASE));
	KUNIT_EXPECT_NOT_NULL(test, READ_ONCE(mm->corten_state));

	/* Double release. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_release,
						 CORTEN_ARENA_TEST_BASE,
						 CORTEN_ARENA_TEST_LEN),
			-ENOENT);

	/* DECLARE/RELEASE round trip again on a fresh VMA. */
	{
		struct vm_area_struct *vma;

		vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_BASE +
					     CORTEN_ARENA_TEST_LEN,
					     CORTEN_ARENA_TEST_FLAGS_OK);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	}
	KUNIT_EXPECT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_release,
						 CORTEN_ARENA_TEST_BASE,
						 CORTEN_ARENA_TEST_LEN),
			0);
}

/* ------------------------------------------------------------------ *
 * shadow-VMA conversion (sec 3)
 * ------------------------------------------------------------------
 */

static void corten_arena_test_shadow_vma(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct vm_area_struct *vma;

	vma = vma_lookup(t->mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_EXPECT_FALSE(test, vma->vm_flags & VM_CORTEN);
	KUNIT_EXPECT_FALSE(test, vma->vm_flags & VM_NOHUGEPAGE);

	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(t->mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);

	vma = vma_lookup(t->mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);

	/* Converted in place: still one VMA, still a plain private
	 * anonymous mapping, plus the shadow bits.
	 */
	KUNIT_EXPECT_EQ(test, vma->vm_start, CORTEN_ARENA_TEST_BASE);
	KUNIT_EXPECT_EQ(test, vma->vm_end,
			CORTEN_ARENA_TEST_BASE + CORTEN_ARENA_TEST_LEN);
	KUNIT_EXPECT_TRUE(test, vma->vm_flags & VM_CORTEN);
	KUNIT_EXPECT_TRUE(test, vma->vm_flags & VM_NOHUGEPAGE);
	KUNIT_EXPECT_TRUE(test, vma_is_anonymous(vma));
	KUNIT_EXPECT_NULL(test, vma->vm_file);
	/* rmap must be ready before the S4 fault path attaches folios. */
	KUNIT_EXPECT_NOT_NULL(test, vma->anon_vma);
#ifdef CONFIG_ANON_VMA_NAME
	KUNIT_ASSERT_NOT_NULL(test, vma->anon_name);
	KUNIT_EXPECT_STREQ(test, vma->anon_name->name, "corten_arena");
#endif

	/* And RELEASE removes the whole thing. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, t->mm,
						 corten_arena_test_op_release,
						 CORTEN_ARENA_TEST_BASE,
						 CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_EXPECT_NULL(test, vma_lookup(t->mm, CORTEN_ARENA_TEST_BASE));
}

/* ------------------------------------------------------------------ *
 * process exit (sec 5.2): explicit hook and the real exit_mmap path
 * ------------------------------------------------------------------
 */

static void corten_arena_test_exit(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;

	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);

	/* Direct hook: registry drained and unpublished, VMA untouched
	 * (the legacy unmap that normally follows does not run here).
	 */
	corten_arena_mm_exit(mm);
	KUNIT_EXPECT_NULL(test, READ_ONCE(mm->corten_state));
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_BASE),
			-ENOENT);

	/* The second mm goes through the real exit path: mmput() runs
	 * exit_mmap(), which drains the arena and unmaps the shadow-VMA
	 * through the standard funnels.  Success criterion: no warning,
	 * no leak.  DECLARE alone never munmaps, so the case thread can
	 * drive it directly and hand the mm back synchronously.
	 */
	{
		struct corten_arena_test_mm *t2 =
			corten_arena_test_mm_setup(test);

		KUNIT_ASSERT_EQ(test,
				corten_arena_declare(t2->mm,
						     CORTEN_ARENA_TEST_BASE,
						     CORTEN_ARENA_TEST_LEN),
				0);
		kunit_remove_action(test, corten_arena_test_mm_destroy, t2);
		mmput(t2->mm);
	}

	/* The first mm is untouched by the second one's teardown. */
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_BASE),
			-ENOENT);
}

/* ------------------------------------------------------------------ *
 * prctl dispatcher gates
 * ------------------------------------------------------------------
 */

static void corten_arena_test_prctl(struct kunit *test)
{
	unsigned long addr = CORTEN_ARENA_TEST_BASE;
	int ret;

	/* arg5 must be 0 (checked before any gate). */
	KUNIT_EXPECT_EQ(test, corten_prctl_arena(CORTEN_ARENA_QUERY, addr, 0,
						 1),
			-EINVAL);

	/* With the gates applied, the suite's un-privileged, corten=off
	 * context must be refused with a negative errno (-EPERM or
	 * -EOPNOTSUPP depending on the credentials of the KUnit runner).
	 */
	ret = corten_prctl_arena(CORTEN_ARENA_QUERY, addr, 0, 0);
	KUNIT_EXPECT_LT(test, ret, 0);
}

/* ------------------------------------------------------------------ *
 * DECLARE/RELEASE under concurrency (sec 6.3): one kthread churns
 * DECLARE/RELEASE while a second hammers QUERY and the S4-style
 * lookup+tryget_live access pattern; the drain in RELEASE must wait for
 * the reader's outstanding percpu_ref.
 * ------------------------------------------------------------------
 */

struct corten_arena_test_conc {
	struct mm_struct *mm;
	unsigned long start;
	unsigned long len;
	int iters;
	atomic_t errors;
	atomic_t gets;
	atomic_t query_bad;
	struct completion done[2];
};

static void corten_arena_test_conc_destroy(void *ctx)
{
	struct corten_arena_test_conc *c = ctx;

	if (c->mm)
		mmput(c->mm);
}

static void corten_arena_test_conc_reader(struct corten_arena_test_conc *c)
{
	while (!kthread_should_stop()) {
		struct corten_arena *ar;
		int q;

		q = corten_arena_query(c->mm, c->start + PMD_SIZE);
		if (q != 0 && q != 1 && q != -ENOENT)
			atomic_inc(&c->query_bad);

		rcu_read_lock();
		ar = corten_arena_lookup(c->mm, c->start + PMD_SIZE);
		if (ar && percpu_ref_tryget_live(&ar->active)) {
			rcu_read_unlock();
			atomic_inc(&c->gets);
			/* Mimic a short transaction, then drop the pin:
			 * a concurrent RELEASE's drain waits for it.
			 */
			cond_resched();
			percpu_ref_put(&ar->active);
		} else {
			rcu_read_unlock();
		}
		cond_resched();
	}
}

static int corten_arena_test_conc_worker(void *data)
{
	struct corten_arena_test_conc *c = data;
	int i;

	/* RELEASE's legacy munmap completion reads current->mm; attach so
	 * the worker behaves like a userspace thread of @c->mm.
	 */
	kthread_use_mm(c->mm);

	for (i = 0; i < c->iters && !kthread_should_stop(); i++) {
		struct vm_area_struct *vma;

		vma = corten_arena_test_mkvm(c->mm, c->start,
					     c->start + c->len,
					     CORTEN_ARENA_TEST_FLAGS_OK);
		if (!vma) {
			atomic_inc(&c->errors);
			break;
		}

		if (corten_arena_declare(c->mm, c->start, c->len)) {
			atomic_inc(&c->errors);
			corten_arena_test_unmap(c->mm, c->start, c->len);
			break;
		}

		if (corten_arena_release(c->mm, c->start, c->len)) {
			atomic_inc(&c->errors);
			corten_arena_test_unmap(c->mm, c->start, c->len);
		}
	}

	kthread_unuse_mm(c->mm);

	complete(&c->done[0]);
	while (!kthread_should_stop())
		schedule_timeout_idle(1);

	return 0;
}

static int corten_arena_test_conc_reader_worker(void *data)
{
	struct corten_arena_test_conc *c = data;

	corten_arena_test_conc_reader(c);

	complete(&c->done[1]);
	while (!kthread_should_stop())
		schedule_timeout_idle(1);

	return 0;
}

static void corten_arena_test_concurrent(struct kunit *test)
{
	struct corten_arena_test_conc *c;
	struct task_struct *tsk[2] = { NULL, NULL };

	c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, c);

	c->mm = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, c->mm);
	kunit_add_action(test, corten_arena_test_conc_destroy, c);

	c->start = CORTEN_ARENA_TEST_BASE;
	c->len = 2 * PMD_SIZE;
	c->iters = 20;
	atomic_set(&c->errors, 0);
	atomic_set(&c->gets, 0);
	atomic_set(&c->query_bad, 0);
	init_completion(&c->done[0]);
	init_completion(&c->done[1]);

	tsk[0] = kthread_run(corten_arena_test_conc_worker, c,
			     "corten_arena_w0");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tsk[0]);
	tsk[1] = kthread_run(corten_arena_test_conc_reader_worker, c,
			     "corten_arena_w1");
	if (IS_ERR(tsk[1])) {
		kthread_stop(tsk[0]);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tsk[1]);
	}

	/* Park the pair on separate CPUs when possible so the drain really
	 * races the reader.
	 */
	if (num_online_cpus() >= 2) {
		set_cpus_allowed_ptr(tsk[0], cpumask_of(0));
		set_cpus_allowed_ptr(tsk[1], cpumask_of(1 %
							num_online_cpus()));
	}

	wait_for_completion(&c->done[0]);
	kthread_stop(tsk[1]);
	wait_for_completion(&c->done[1]);
	kthread_stop(tsk[0]);

	KUNIT_EXPECT_EQ(test, atomic_read(&c->errors), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&c->query_bad), 0);
	KUNIT_EXPECT_GT(test, atomic_read(&c->gets), 0);

	/* Final state: everything released, registry empty. */
	KUNIT_EXPECT_EQ(test, corten_arena_query(c->mm, c->start), 0);
}

static struct kunit_case corten_arena_test_cases[] = {
	KUNIT_CASE(corten_arena_test_declare_reject),
	KUNIT_CASE(corten_arena_test_declare_reject_flags),
	KUNIT_CASE(corten_arena_test_declare_query),
	KUNIT_CASE(corten_arena_test_release),
	KUNIT_CASE(corten_arena_test_shadow_vma),
	KUNIT_CASE(corten_arena_test_exit),
	KUNIT_CASE(corten_arena_test_prctl),
	KUNIT_CASE(corten_arena_test_concurrent),
	{}
};

static struct kunit_suite corten_arena_test_suite = {
	.name = "corten_arena",
	.test_cases = corten_arena_test_cases,
};

kunit_test_suites(&corten_arena_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for the CortenMM arena registration layer");
