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
#include <linux/mman.h>
#include <linux/mmap_lock.h>
#include <linux/mm_inline.h>
#include <linux/pgtable.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <uapi/linux/mman.h>
#include <uapi/linux/prctl.h>

#include "corten.h"		/* corten_test_render_dbg() (S8 assertions) */
#include "corten_arena.h"	/* corten_arena_range_overlaps() (gate-free) */
#include "internal.h"
#include "vma.h"

/* Test layout: arena windows in the low user range, PMD-aligned. */
#define CORTEN_ARENA_TEST_BASE		(4UL * PMD_SIZE)
#define CORTEN_ARENA_TEST_LEN		(4UL * PMD_SIZE)
#define CORTEN_ARENA_TEST_LEN2		(2UL * PMD_SIZE)
#define CORTEN_ARENA_TEST_START2	(CORTEN_ARENA_TEST_BASE + 8UL * PMD_SIZE)
#define CORTEN_ARENA_TEST_NOWHERE	(CORTEN_ARENA_TEST_BASE + 64UL * PMD_SIZE)

/* The T0a auto-arena window (M4T0_SPEC.md sec 1.3): one PMD slot at the
 * window base drives the attach/release/cursor cases.
 */
#define CORTEN_ARENA_TEST_WIN		(CORTEN_MODE_WINDOW_START)
#define CORTEN_ARENA_TEST_WIN_LEN	(2UL * PMD_SIZE)

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
	/* T0b mremap-route arguments and results. */
	unsigned long len2;
	unsigned long flags;
	unsigned long new_addr;
	long retl;
	char *buf;		/* pattern in/out for copy_to_user checks */
	struct completion done;
};

static void corten_arena_test_op_release(struct corten_arena_test_op *o)
{
	o->ret = corten_arena_release(o->mm, o->addr, o->len);
}

/* MODE EXIT tears the declared arenas down through RELEASE's legacy
 * munmap leg, which reads current->mm (vms_complete_munmap_vmas()) --
 * so, like RELEASE itself, the exit runs on the attached op worker.
 */
static void corten_arena_test_op_mode_exit(struct corten_arena_test_op *o)
{
	o->ret = corten_arena_mode_exit(o->mm);
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

/* corten_arena_auto_mmap_route() is the do_mmap() front half: its
 * contract is this mm's mmap_write held by the caller (the obstacle
 * walk asserts it).  The direct calls below take the lock exactly like
 * do_mmap() does.
 */
static int corten_arena_test_auto_route_locked(struct mm_struct *mm,
					       unsigned long len,
					       unsigned long prot,
					       unsigned long *addr,
					       unsigned long *lenp,
					       unsigned long *flagsp)
{
	int ret;

	mmap_write_lock(mm);
	ret = corten_arena_auto_mmap_route(mm, len, prot, addr, lenp,
					   flagsp);
	mmap_write_unlock(mm);

	return ret;
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
 * Overlap decision: the entry-reject anchor for the space-operation
 * hooks and the move_pages()/migrate_pages() audit (M4T0_SPEC.md #9)
 * ------------------------------------------------------------------
 */

static void corten_arena_test_range_overlaps(struct kunit *test)
{
	/*
	 * The decision gates on corten_enabled_static() (one static-branch
	 * read before the xarray walk), so on a corten=off boot every
	 * answer would be a constant false and there is nothing to
	 * observe (same degraded contract as the real-chain cases in
	 * mm/corten_fault_test.c).
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "overlap gate requires corten=on");
	{
		struct corten_arena_test_mm *t =
			corten_arena_test_mm_setup(test);
		struct mm_struct *mm = t->mm;

		/* No arena yet: even the whole-mm sweep is false. */
		KUNIT_EXPECT_FALSE(test,
				   corten_arena_range_overlaps(mm, 0,
							       TASK_SIZE));

		KUNIT_ASSERT_EQ(test,
				corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
						     CORTEN_ARENA_TEST_LEN),
				0);

		/* Inside: first page, last page, a straddling chunk and
		 * the whole-mm sweep the migrate entry hooks use.
		 */
		KUNIT_EXPECT_TRUE(test,
				  corten_arena_range_overlaps(mm,
						CORTEN_ARENA_TEST_BASE,
						PAGE_SIZE));
		KUNIT_EXPECT_TRUE(test,
				  corten_arena_range_overlaps(mm,
						CORTEN_ARENA_TEST_BASE +
						CORTEN_ARENA_TEST_LEN -
						PAGE_SIZE, PAGE_SIZE));
		KUNIT_EXPECT_TRUE(test,
				  corten_arena_range_overlaps(mm,
						CORTEN_ARENA_TEST_BASE +
						3 * PMD_SIZE, 2 * PAGE_SIZE));
		KUNIT_EXPECT_TRUE(test,
				  corten_arena_range_overlaps(mm, 0,
							      TASK_SIZE));

		/* Outside: the page before, the page after, a disjoint
		 * range, and the zero-length query.
		 */
		KUNIT_EXPECT_FALSE(test,
				   corten_arena_range_overlaps(mm,
					CORTEN_ARENA_TEST_BASE - PAGE_SIZE,
					PAGE_SIZE));
		KUNIT_EXPECT_FALSE(test,
				   corten_arena_range_overlaps(mm,
					CORTEN_ARENA_TEST_BASE +
					CORTEN_ARENA_TEST_LEN, PAGE_SIZE));
		KUNIT_EXPECT_FALSE(test,
				   corten_arena_range_overlaps(mm,
					CORTEN_ARENA_TEST_NOWHERE,
					PMD_SIZE));
		KUNIT_EXPECT_FALSE(test,
				   corten_arena_range_overlaps(mm,
					CORTEN_ARENA_TEST_BASE, 0));
	}
}

/* ------------------------------------------------------------------ *
 * RELEASE: exact-match contract, drain and teardown
 * ------------------------------------------------------------------
 */

/*
 * D15 (r06-t5 overhead diagnosis): arena refs must be BORN atomic
 * (PERCPU_REF_INIT_ATOMIC).  A percpu-born ref pushes every
 * percpu_ref_kill_and_confirm() through the percpu->atomic switch's
 * call_rcu() grace period; RELEASE drains under mmap_write, so each
 * arena munmap carried a measured 4.9-20ms lower bound and the dedup_eq
 * tcmalloc arm collapsed to 12x its base wall time (25,600 serialized
 * releases).  Anchor: immediately after a live DECLARE -- with no kill
 * and no switch in flight -- the active ref is already in atomic mode.
 */
static void corten_arena_test_ref_born_atomic(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	unsigned long __percpu *percpu_count;
	struct corten_arena *ar;

	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(t->mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);

	rcu_read_lock();
	ar = corten_arena_lookup(t->mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_NULL(test, ar);
	if (percpu_ref_tryget_live(&ar->active)) {
		rcu_read_unlock();
		/* The drain contract (D15): born atomic, so a healthy
		 * kill confirms synchronously -- no grace period sits
		 * between percpu_ref_kill_and_confirm() and completion.
		 */
		KUNIT_EXPECT_FALSE(test,
				   __ref_is_percpu(&ar->active,
						   &percpu_count));
		percpu_ref_put(&ar->active);
	} else {
		rcu_read_unlock();
	}

	/* Unchanged RELEASE semantics on top. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, t->mm,
						 corten_arena_test_op_release,
						 CORTEN_ARENA_TEST_BASE,
						 CORTEN_ARENA_TEST_LEN),
			0);
}

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
	/* T0a: churn the same slot in the auto-arena window through
	 * corten_arena_auto_attach()/release instead of the low base
	 * range through declare()/release() -- the DEV-13 lock-order
	 * regression must cover the do_mmap-shaped path too.
	 */
	bool use_window;
	atomic_t errors;
	atomic_t gets;
	atomic_t query_bad;
	/* D15: born-atomic refs made the RELEASE drain grace-free, so the
	 * churn window is microseconds wide.  The reader cannot win it by
	 * free-running any more; the worker pauses at the first arena
	 * until the reader reports a pinned ref (completion below), which
	 * is the race the drain contract is about.
	 */
	struct completion got;
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
			/* D15: tell the worker a live arena is pinned; its
			 * next RELEASE's drain waits for the put below
			 * (bounded by this short hold, not by a grace
			 * period any more).
			 */
			if (atomic_read(&c->gets) == 1)
				complete(&c->got);
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

	if (c->use_window && corten_arena_mode_enter(c->mm))
		atomic_inc(&c->errors);

	for (i = 0; i < c->iters && !kthread_should_stop(); i++) {
		struct vm_area_struct *vma;
		int r;

		vma = corten_arena_test_mkvm(c->mm, c->start,
					     c->start + c->len,
					     CORTEN_ARENA_TEST_FLAGS_OK);
		if (!vma) {
			atomic_inc(&c->errors);
			break;
		}

		/* The window shape runs the attach under the write lock,
		 * exactly as the do_mmap tail hook does (DEV-13 outermost
		 * acquisition); the declare shape takes the lock itself.
		 */
		if (c->use_window) {
			mmap_write_lock(c->mm);
			r = corten_arena_auto_attach(c->mm, c->start, c->len);
			mmap_write_unlock(c->mm);
		} else {
			r = corten_arena_declare(c->mm, c->start, c->len);
		}
		if (r) {
			atomic_inc(&c->errors);
			corten_arena_test_unmap(c->mm, c->start, c->len);
			break;
		}

		/* D15: give the reader a fair shot at the first arena --
		 * with a grace-free drain the churn window is microseconds,
		 * and a free-running reader loses it every time.
		 */
		if (i == 0)
			wait_for_completion_timeout(&c->got, 10 * HZ);

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

/* Shared body of the DECLARE/RELEASE-vs-reader race (sec 6.3 + DEV-13):
 * @use_window switches the churned range between the low base range
 * (prctl DECLARE shape) and the auto-arena window slot (do_mmap
 * auto-attach shape).
 */
static void corten_arena_test_concurrent_run(struct kunit *test,
					     bool use_window)
{
	struct corten_arena_test_conc *c;
	struct task_struct *tsk[2] = { NULL, NULL };

	c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, c);

	c->mm = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, c->mm);
	kunit_add_action(test, corten_arena_test_conc_destroy, c);

	c->start = use_window ? CORTEN_ARENA_TEST_WIN : CORTEN_ARENA_TEST_BASE;
	c->len = use_window ? CORTEN_ARENA_TEST_WIN_LEN : 2 * PMD_SIZE;
	c->iters = 20;
	c->use_window = use_window;
	atomic_set(&c->errors, 0);
	atomic_set(&c->gets, 0);
	atomic_set(&c->query_bad, 0);
	init_completion(&c->got);
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

static void corten_arena_test_concurrent(struct kunit *test)
{
	corten_arena_test_concurrent_run(test, false);
}

/* The DEV-13 lock-order regression for the T0a window path: the same
 * race as above, driven through corten_arena_auto_attach() (the body
 * do_mmap runs under its write lock) with the fault-style reader
 * hammering lookup+tryget on the window slot.  A deadlock or a
 * lock-order violation in the inverted nesting shows up here.
 */
static void corten_arena_test_concurrent_window(struct kunit *test)
{
	corten_arena_test_concurrent_run(test, true);
}

/* ------------------------------------------------------------------ *
 * S8 observability (debugfs arenas ledger + drain-timeout aggregate)
 * ------------------------------------------------------------------
 */

/* DECLARE must make the arena visible to the debugfs renderer with its
 * range and liveness status, RELEASE must make it disappear again: the
 * global ledger tracks the per-mm xarray faithfully (M3B_DESIGN.md
 * sec 7.4).
 */
static void corten_arena_test_obs_ledger(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	char expect[64];
	char *dbg;

	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);

	dbg = corten_test_render_dbg(CORTEN_DBG_ARENAS);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dbg);
	snprintf(expect, sizeof(expect), "[%lx,%lx)",
		 CORTEN_ARENA_TEST_BASE,
		 CORTEN_ARENA_TEST_BASE + CORTEN_ARENA_TEST_LEN);
	KUNIT_EXPECT_NOT_NULL(test, strstr(dbg, expect));
	KUNIT_EXPECT_NOT_NULL(test, strstr(dbg, "active"));
	kfree(dbg);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_release,
						 CORTEN_ARENA_TEST_BASE,
						 CORTEN_ARENA_TEST_LEN),
			0);

	dbg = corten_test_render_dbg(CORTEN_DBG_ARENAS);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dbg);
	KUNIT_EXPECT_NULL(test, strstr(dbg, expect));
	kfree(dbg);
}

/* The drain-timeout aggregate wiring (r03 final-smoke legacy item 1):
 * one recorded timeout moves the global mirror by exactly one and the
 * arena_stats renderer surfaces the new value.  The record is injected
 * directly (a real timeout needs a leaked transaction reference, i.e. a
 * kernel bug), so this asserts the debugfs aggregation path, not the
 * drain itself.
 */
static void corten_arena_test_drain_timeout_stat(struct kunit *test)
{
	long before = corten_arena_test_drain_timeouts();
	char expect[64];
	char *dbg;

	corten_arena_test_inject_drain_timeout();
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(),
			before + 1);

	dbg = corten_test_render_dbg(CORTEN_DBG_ARENA_STATS);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dbg);
	snprintf(expect, sizeof(expect), "drain_timeout       %ld",
		 before + 1);
	KUNIT_EXPECT_NOT_NULL(test, strstr(dbg, expect));
	kfree(dbg);
}

/* ------------------------------------------------------------------ *
 * T0a: release-on-full-coverage classification (M4T0_SPEC.md sec 3.2,
 * pure) -- the glibc free() shape: user munmaps the page-rounded request
 * length, kernel arena is 2M-rounded, tail < 2M must RELEASE.
 * ------------------------------------------------------------------
 */

static void corten_arena_test_release_classify(struct kunit *test)
{
	const unsigned long a = 0x1000UL * PAGE_SIZE;	/* an arena base */
	const unsigned long e = a + 4 * PMD_SIZE;	/* its end */

	/* Exact stays exact (tail 0). */
	KUNIT_EXPECT_EQ(test,
			corten_arena_release_classify(CORTEN_UNMAP_EXACT, a,
						      e, a, e),
			CORTEN_UNMAP_EXACT);

	/* The rule: CHUNK from the arena base with a tail < 2M -> EXACT. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_release_classify(CORTEN_UNMAP_CHUNK, a,
						      e - PAGE_SIZE, a, e),
			CORTEN_UNMAP_EXACT);
	/* The glibc free() shape's worst tail: 2M-4K (the page-rounded
	 * request of a full 2M chunk) is still < 2M -> EXACT.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_release_classify(CORTEN_UNMAP_CHUNK, a,
						      e - (PMD_SIZE - PAGE_SIZE),
						      a, e),
			CORTEN_UNMAP_EXACT);
	/* Boundary: a full 2M tail is real chunk territory. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_release_classify(CORTEN_UNMAP_CHUNK, a,
						      e - 2 * PMD_SIZE, a, e),
			CORTEN_UNMAP_CHUNK);

	/* Not from the arena base: allocator mid-block free, stays CHUNK. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_release_classify(CORTEN_UNMAP_CHUNK,
						      a + PMD_SIZE,
						      a + 2 * PMD_SIZE, a, e),
			CORTEN_UNMAP_CHUNK);

	/* Crossing and outside classes pass through untouched. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_release_classify(CORTEN_UNMAP_PARTIAL,
						      a + PMD_SIZE,
						      e + PMD_SIZE, a, e),
			CORTEN_UNMAP_PARTIAL);
	KUNIT_EXPECT_EQ(test,
			corten_arena_release_classify(CORTEN_UNMAP_OUTSIDE,
						      a - PMD_SIZE, a, a, e),
			CORTEN_UNMAP_OUTSIDE);
}

/* ------------------------------------------------------------------ *
 * T0a: auto-mmap whitelist classification (sec 3.1, pure)
 * ------------------------------------------------------------------
 */

static void corten_arena_test_auto_classify(struct kunit *test)
{
	static const unsigned long bad_bits[] = {
		MAP_FIXED, MAP_FIXED_NOREPLACE, MAP_HUGETLB, MAP_GROWSDOWN,
		MAP_POPULATE, MAP_LOCKED, MAP_SYNC, MAP_STACK,
		MAP_UNINITIALIZED, MAP_DENYWRITE, MAP_EXECUTABLE,
		MAP_NONBLOCK, MAP_DROPPABLE, MAP_32BIT,
	};
	unsigned long good = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	size_t i;

	/* The clean hits. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_classify(MAP_PRIVATE |
							MAP_ANONYMOUS, false),
			CORTEN_MMAP_AUTO);
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_classify(good, false),
			CORTEN_MMAP_AUTO);

	/* A file backing is never auto-arena material. */
	KUNIT_EXPECT_EQ(test, corten_arena_auto_mmap_classify(good, true),
			CORTEN_MMAP_LEGACY);

	/* Shared types (and the MAP_DROPPABLE alias inside the type
	 * nibble) stay legacy.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_classify(MAP_SHARED |
							MAP_ANONYMOUS, false),
			CORTEN_MMAP_LEGACY);
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_classify(MAP_SHARED_VALIDATE |
							MAP_ANONYMOUS, false),
			CORTEN_MMAP_LEGACY);
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_classify(MAP_PRIVATE |
							MAP_ANONYMOUS |
							MAP_DROPPABLE, false),
			CORTEN_MMAP_LEGACY);

	/* Missing MAP_ANONYMOUS / bare type bits. */
	KUNIT_EXPECT_EQ(test, corten_arena_auto_mmap_classify(MAP_PRIVATE,
							      false),
			CORTEN_MMAP_LEGACY);

	for (i = 0; i < ARRAY_SIZE(bad_bits); i++) {
		KUNIT_EXPECT_EQ_MSG(test,
				    corten_arena_auto_mmap_classify(good |
								    bad_bits[i],
								    false),
				    CORTEN_MMAP_LEGACY,
				    "flag bit %#lx must stay legacy",
				    bad_bits[i]);
	}
}

/* ------------------------------------------------------------------ *
 * T0a: window cursor arithmetic (sec 3.1, pure)
 * ------------------------------------------------------------------
 */

static void corten_arena_test_auto_place(struct kunit *test)
{
	unsigned long addr;

	/* Fresh cursor: the first arena lands exactly at the window base. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_place(CORTEN_MODE_WINDOW_START,
						2 * PMD_SIZE, &addr), 0);
	KUNIT_EXPECT_EQ(test, addr, CORTEN_MODE_WINDOW_START);

	/* The cursor is PMD-aligned; a page-stepped cursor rounds up. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_place(CORTEN_MODE_WINDOW_START +
						PAGE_SIZE, PMD_SIZE, &addr),
			0);
	KUNIT_EXPECT_EQ(test, addr,
			CORTEN_MODE_WINDOW_START + PMD_SIZE);

	/* The last fitting arena. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_place(CORTEN_MODE_WINDOW_END -
						PMD_SIZE, PMD_SIZE, &addr), 0);
	KUNIT_EXPECT_EQ(test, addr, CORTEN_MODE_WINDOW_END - PMD_SIZE);

	/* Exhaustion: one page past the last fitting placement. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_place(CORTEN_MODE_WINDOW_END -
						PMD_SIZE + PAGE_SIZE,
						PMD_SIZE, &addr), -ENOSPC);
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_place(CORTEN_MODE_WINDOW_END,
						PMD_SIZE, &addr), -ENOSPC);

	/* Never-fitting and degenerate lengths. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_place(CORTEN_MODE_WINDOW_START,
						CORTEN_MODE_WINDOW_END -
						CORTEN_MODE_WINDOW_START +
						PMD_SIZE, &addr), -ENOSPC);
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_place(CORTEN_MODE_WINDOW_START, 0,
						&addr), -ENOSPC);
}

/* ------------------------------------------------------------------ *
 * T0a: prctl gates + gate-free MODE enter/exit/get
 * ------------------------------------------------------------------
 */

static void corten_arena_test_mode(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	int ret;

	/* arg3/arg4/arg5 must be 0 (checked before any gate). */
	KUNIT_EXPECT_EQ(test,
			corten_prctl_mode(CORTEN_MODE_GET, 1, 0, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test,
			corten_prctl_mode(CORTEN_MODE_GET, 0, 1, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test,
			corten_prctl_mode(CORTEN_MODE_GET, 0, 0, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test,
			corten_prctl_mode(42, 0, 0, 0), -EINVAL);

	/* With corten=off the whole prctl is -EOPNOTSUPP (feature-off
	 * contract); the gate-free internals below still run so the
	 * semantics are verified on every boot.
	 */
	ret = corten_prctl_mode(CORTEN_MODE_GET, 0, 0, 0);
	if (!corten_enabled_static()) {
		KUNIT_EXPECT_EQ(test, ret, -EOPNOTSUPP);
	} else {
		/* This thread has no mm of its own (kunit_try_catch):
		 * the dispatcher answers -EINVAL; on a thread with an mm
		 * it would answer 0/1 -- the semantics live in the
		 * gate-free checks below.
		 */
		KUNIT_EXPECT_TRUE(test, ret == -EINVAL || ret == 0 ||
				  ret == 1);
	}

	/* Gate-free enter/exit/get on a synthetic mm. */
	KUNIT_EXPECT_EQ(test, corten_arena_mode_get(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_mode_get(mm), 1);
	KUNIT_EXPECT_NOT_NULL(test, READ_ONCE(mm->corten_state));
	/* The registry was created eagerly with the window cursor. */
	KUNIT_EXPECT_EQ(test,
			READ_ONCE(mm->corten_state)->next_va,
			CORTEN_MODE_WINDOW_START);
	/* Re-enter is idempotent. */
	KUNIT_EXPECT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_mode_get(mm), 1);

	/* Exit with no arenas: mode cleared, registry stays. */
	KUNIT_EXPECT_EQ(test, corten_arena_mode_exit(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_mode_get(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_mode_exit(mm), 0);

	/* Exit tears declared arenas down (sec 1.1, RELEASE one by one).
	 * The RELEASE legs read current->mm, so -- like every other
	 * munmap in this file -- the exit runs on the op worker, not on
	 * this mm-less case thread.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_EXPECT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
	KUNIT_EXPECT_EQ(test, corten_arena_mode_get(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_BASE),
			0);
}

/* ------------------------------------------------------------------ *
 * T0a: the do_mmap route decision (sec 3.1) -- needs the real chain,
 * so it only produces observations on a corten=on boot.
 * ------------------------------------------------------------------
 */

/* Sum a per-mm relaxed counter across cpus (the increments happened on
 * whichever cpu the calling thread ran on).
 */
static long corten_arena_test_stat_sum(struct corten_mm_state *state,
				       enum corten_arena_stat which)
{
	int cpu;
	long sum = 0;

	if (!state)
		return 0;
	for_each_online_cpu(cpu)
		sum += per_cpu_ptr(state->stats, cpu)[which];

	return sum;
}

static void corten_arena_test_auto_route(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct corten_mm_state *state;
	unsigned long addr = 0, len = 2 * PAGE_SIZE, flags;
	unsigned long fallbacks;

	/*
	 * The route gates on corten_enabled_static(); on a corten=off
	 * boot the answer is a constant 0 and there is nothing to
	 * observe.
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "auto-mmap route requires corten=on");
	if (sysctl_overcommit_memory == OVERCOMMIT_NEVER)
		kunit_skip(test, "auto takeover degraded (OVERCOMMIT_NEVER)");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	state = READ_ONCE(mm->corten_state);
	KUNIT_ASSERT_NOT_NULL(test, state);

	/* Non-whitelisted flags: legacy, cursor untouched. */
	flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
	addr = 0;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_auto_route_locked(mm, len, PROT_READ |
						     PROT_WRITE, &addr, &len,
						     &flags), 0);
	KUNIT_EXPECT_EQ(test, addr, 0);
	KUNIT_EXPECT_EQ(test, state->next_va, CORTEN_MODE_WINDOW_START);

	/* Plain whitelist hit: rewritten onto the window base, 2M-rounded,
	 * MAP_FIXED|MAP_NORESERVE forced.
	 */
	addr = 0;
	len = 2 * PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_auto_route_locked(mm, len, PROT_READ |
						     PROT_WRITE, &addr, &len,
						     &flags), 1);
	KUNIT_EXPECT_EQ(test, addr, CORTEN_MODE_WINDOW_START);
	/* 8K of request, one 2M-rounded arena slot. */
	KUNIT_EXPECT_EQ(test, len, PMD_SIZE);
	KUNIT_EXPECT_EQ(test, flags, MAP_PRIVATE | MAP_ANONYMOUS |
				     MAP_FIXED | MAP_NORESERVE);
	KUNIT_EXPECT_EQ(test, state->next_va,
			CORTEN_MODE_WINDOW_START + PMD_SIZE);

	/* Second hit: the cursor advanced. */
	addr = 0;
	len = PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_auto_route_locked(mm, len,
							    PROT_NONE, &addr,
							    &len, &flags),
			1);
	KUNIT_EXPECT_EQ(test, addr,
			CORTEN_MODE_WINDOW_START + PMD_SIZE);
	KUNIT_EXPECT_EQ(test, len, PMD_SIZE);

	/* Arena obstacle (a MODE-targeted DECLARE inside the window): the
	 * route skips past it instead of MAP_FIXED-destroying it.  The
	 * cursor is rewound first so the candidate really collides with
	 * the arena.
	 */
	{
		struct vm_area_struct *vma;

		vma = corten_arena_test_mkvm(mm, CORTEN_MODE_WINDOW_START,
					     CORTEN_MODE_WINDOW_START +
					     CORTEN_ARENA_TEST_WIN_LEN,
					     CORTEN_ARENA_TEST_FLAGS_OK);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
		KUNIT_ASSERT_EQ(test,
				corten_arena_declare(mm,
						     CORTEN_MODE_WINDOW_START,
						     CORTEN_ARENA_TEST_WIN_LEN),
				0);
	}

	WRITE_ONCE(state->next_va, CORTEN_MODE_WINDOW_START);
	addr = 0;
	len = PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_auto_route_locked(mm, len,
							    PROT_READ, &addr,
							    &len, &flags),
			1);
	KUNIT_EXPECT_EQ(test, addr,
			CORTEN_MODE_WINDOW_START + 2 * PMD_SIZE);
	KUNIT_EXPECT_EQ(test, state->next_va,
			CORTEN_MODE_WINDOW_START + 3 * PMD_SIZE);

	/* Plain legacy VMA obstacle: also skipped, never overwritten. */
	{
		struct vm_area_struct *vma;

		vma = corten_arena_test_mkvm(mm,
					     CORTEN_MODE_WINDOW_START +
					     4 * PMD_SIZE,
					     CORTEN_MODE_WINDOW_START +
					     5 * PMD_SIZE,
					     CORTEN_ARENA_TEST_FLAGS_OK);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	}
	WRITE_ONCE(state->next_va,
		   CORTEN_MODE_WINDOW_START + 4 * PMD_SIZE - PAGE_SIZE);
	addr = 0;
	len = PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_auto_route_locked(mm, len,
							    PROT_READ, &addr,
							    &len, &flags),
			1);
	KUNIT_EXPECT_EQ(test, addr,
			CORTEN_MODE_WINDOW_START + 5 * PMD_SIZE);

	/* Window exhaustion: graceful legacy degradation, counted. */
	fallbacks = corten_arena_test_stat_sum(state,
					       CORTEN_ARENA_STAT_FALLBACKS);
	WRITE_ONCE(state->next_va, CORTEN_MODE_WINDOW_END);
	addr = 0xdead0000UL;
	len = PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_auto_route_locked(mm, len,
							    PROT_READ, &addr,
							    &len, &flags),
			0);
	KUNIT_EXPECT_EQ(test, addr, 0xdead0000UL);	/* untouched */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_stat_sum(state,
						   CORTEN_ARENA_STAT_FALLBACKS),
			fallbacks + 1);

	/* The declared arena is still alive: the exit's RELEASE legs read
	 * current->mm, so the teardown runs on the op worker.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
}

/* ------------------------------------------------------------------ *
 * T0a: attach + the release-on-full-coverage rule end to end (sec 3.2)
 * ------------------------------------------------------------------
 */

static void corten_arena_test_op_munmap_route(struct corten_arena_test_op *o)
{
	o->ret = corten_arena_munmap_route(o->mm, o->addr, o->len);
}

static void corten_arena_test_auto_attach_release(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct vm_area_struct *vma;
	char expect[64];
	char *dbg;
	int ret;

	if (!corten_enabled_static())
		kunit_skip(test, "munmap routing requires corten=on");

	/* The route creates the registry before the real do_mmap attaches;
	 * mode_enter is the same registry-establishing step (gate-free).
	 */
	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* A glibc-shaped mmap: a plain 2M NORESERVE anonymous VMA at the
	 * window base (as mmap_region would create after a takeover
	 * rewrite), attached by hand -- the do_mmap tail hook's body.
	 * The arena must be exactly one 2M chunk here: the release rule
	 * only applies while the leftover tail stays under 2M.
	 */
	vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_WIN,
				     CORTEN_ARENA_TEST_WIN + PMD_SIZE,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	/* auto_attach() is the do_mmap() tail hook: DECLARE's locked body
	 * runs under the caller's mmap_write (mmap_region()'s contract).
	 */
	mmap_write_lock(mm);
	ret = corten_arena_auto_attach(mm, CORTEN_ARENA_TEST_WIN, PMD_SIZE);
	mmap_write_unlock(mm);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_query(mm, CORTEN_ARENA_TEST_WIN), 1);

	/* Visible in the observability ledger. */
	snprintf(expect, sizeof(expect), "[%lx,%lx)",
		 CORTEN_ARENA_TEST_WIN, CORTEN_ARENA_TEST_WIN + PMD_SIZE);
	dbg = corten_test_render_dbg(CORTEN_DBG_ARENAS);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dbg);
	KUNIT_EXPECT_NOT_NULL(test, strstr(dbg, expect));
	kfree(dbg);

	/* free() shape: one page of the base -- CHUNK by span, RELEASE by
	 * the full-coverage rule.  The whole arena (VMA included) must be
	 * gone.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE),
			1);
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_WIN),
			0);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));

	/* A genuine mid-arena chunk keeps the VA: chunk zap, not release. */
	vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_WIN,
				     CORTEN_ARENA_TEST_WIN +
				     CORTEN_ARENA_TEST_WIN_LEN,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	/* auto_attach() is the do_mmap() tail hook: DECLARE's locked body
	 * runs under the caller's mmap_write (mmap_region()'s contract).
	 */
	mmap_write_lock(mm);
	ret = corten_arena_auto_attach(mm, CORTEN_ARENA_TEST_WIN,
				       CORTEN_ARENA_TEST_WIN_LEN);
	mmap_write_unlock(mm);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN +
						 PMD_SIZE,
						 PMD_SIZE),
			1);
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_WIN),
			1);
	KUNIT_EXPECT_NOT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));
}

/* ------------------------------------------------------------------ *
 * T0a: fork transition (sec 5, DEV-11) -- full teardown on the parent,
 * MODE-bit inheritance on both sides, clean metadata after the scrub.
 * ------------------------------------------------------------------
 */

static void corten_arena_test_fork_demote(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct mm_struct *child;
	struct vm_area_struct *vma;

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	/* Not registered with kunit: mmput() it explicitly at the end so
	 * the exit_mmap() assertion window stays deterministic.
	 */

	/* dup_mmap() holds oldmm's write lock at the demote point. */
	mmap_write_lock(mm);
	KUNIT_EXPECT_EQ(test, corten_arena_fork_demote(child, mm), 0);
	mmap_write_unlock(mm);

	/* MODE bit inherited on the child, kept on the parent. */
	KUNIT_EXPECT_TRUE(test, READ_ONCE(child->corten_mode));
	KUNIT_EXPECT_TRUE(test, READ_ONCE(mm->corten_mode));

	/* Child: no registry, no arenas (mm_init guarantees NULL). */
	KUNIT_EXPECT_NULL(test, READ_ONCE(child->corten_state));

	/* Parent: arenas gone, shadow-VMA restored to a plain anonymous
	 * VMA with identical contents and bounds.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_BASE),
			0);
	vma = vma_lookup(mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_EXPECT_FALSE(test, vma->vm_flags & VM_CORTEN);
	KUNIT_EXPECT_FALSE(test, vma->vm_flags & VM_NOHUGEPAGE);
	KUNIT_EXPECT_TRUE(test, vma_is_anonymous(vma));

	/* The scrub guarantees a clean re-DECLARE (a stale CORTEN_MAPPED
	 * behind the fresh arena would resurrect old translations --
	 * T0-R5).  No PTE was ever written in this test, so emptiness
	 * holds; the metadata side is what the re-DECLARE exercises.
	 */
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

	/* No-arena MODE fork: pure bit copy, zero teardown work. */
	{
		struct mm_struct *child2 = mm_alloc();
		struct mm_struct *t2mm;

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child2);
		KUNIT_EXPECT_EQ(test, corten_arena_fork_demote(child2, mm),
				0);
		KUNIT_EXPECT_TRUE(test, READ_ONCE(child2->corten_mode));

		/* And a fork of a MODE process without any registry. */
		t2mm = mm_alloc();
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t2mm);
		KUNIT_EXPECT_EQ(test, corten_arena_mode_enter(t2mm), 0);
		KUNIT_EXPECT_EQ(test, corten_arena_fork_demote(child2, t2mm),
				0);
		KUNIT_EXPECT_TRUE(test, READ_ONCE(child2->corten_mode));
		KUNIT_EXPECT_TRUE(test, READ_ONCE(t2mm->corten_mode));
		mmput(child2);
		mmput(t2mm);
	}

	KUNIT_EXPECT_EQ(test, corten_arena_mode_exit(mm), 0);
	mmput(child);
}

/* ------------------------------------------------------------------ *
 * DEV-11 completion (r06 "rogue" family anchor): the fork demotion must
 * carry the routed mprotect() commits into the plain-VMA layer.  A
 * PROT_NONE reservation whose chunk was committed through the route
 * demotes into a split VMA: the committed range keeps R/W, the rest of
 * the reservation stays inaccessible.  Pre-fix the whole range came out
 * with the DECLARE flags and a surviving parent (glibc heap cleanup
 * after a fork probe) wrote straight into SEGV_ACCERR.
 * ------------------------------------------------------------------
 */

static void corten_arena_test_fork_demote_perm(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm, *child;
	struct vm_area_struct *vma;
	unsigned long commit;
	int ret;

	/* The commit under test goes through the mprotect route, whose
	 * decision gates on corten_enabled_static(); on a corten=off boot
	 * the route is the legacy funnel and there is nothing to observe.
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "mprotect route requires corten=on");

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);
	kunit_add_action(test, corten_arena_test_mm_destroy, t);

	t->test = test;
	t->mm = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->mm);
	mm = t->mm;

	/* The reservation: PROT_NONE-shaped (no R/W), the JVM/glibc heap
	 * reserve shape.
	 */
	vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_BASE,
				     CORTEN_ARENA_TEST_BASE +
				     CORTEN_ARENA_TEST_LEN,
				     CORTEN_ARENA_TEST_FLAGS_OK &
				     ~(VM_READ | VM_WRITE));
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);

	/* Commit pages 1..2 (inclusive) of the arena through the route. */
	commit = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	KUNIT_ASSERT_EQ(test,
			corten_arena_mprotect_route(mm, commit,
						    2 * PAGE_SIZE,
						    PROT_READ | PROT_WRITE,
						    -1),
			1);

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);

	/* dup_mmap() holds oldmm's write lock at the demote point. */
	mmap_write_lock(mm);
	ret = corten_arena_fork_demote(child, mm);
	mmap_write_unlock(mm);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* The committed chunk: a plain anonymous piece carrying R/W. */
	vma = vma_lookup(mm, commit);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_EXPECT_TRUE(test, vma_is_anonymous(vma));
	KUNIT_EXPECT_FALSE(test, vma->vm_flags & VM_CORTEN);
	KUNIT_EXPECT_TRUE(test, vma->vm_flags & VM_READ);
	KUNIT_EXPECT_TRUE(test, vma->vm_flags & VM_WRITE);
	KUNIT_EXPECT_EQ(test, vma->vm_start, commit);
	KUNIT_EXPECT_EQ(test, vma->vm_end, commit + 2 * PAGE_SIZE);

	/* Outside the commit the reservation stays inaccessible. */
	vma = vma_lookup(mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_EXPECT_FALSE(test, vma->vm_flags & VM_READ);
	KUNIT_EXPECT_FALSE(test, vma->vm_flags & VM_WRITE);
	KUNIT_EXPECT_EQ(test, vma->vm_start, CORTEN_ARENA_TEST_BASE);
	KUNIT_EXPECT_EQ(test, vma->vm_end, commit);

	vma = vma_lookup(mm, CORTEN_ARENA_TEST_BASE + PMD_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_EXPECT_FALSE(test, vma->vm_flags & VM_READ);
	KUNIT_EXPECT_FALSE(test, vma->vm_flags & VM_WRITE);

	corten_arena_mode_exit(mm);
	mmput(child);
}

/* ------------------------------------------------------------------ *
 * T0b: mprotect / madvise / mremap routing (M4T0_SPEC.md sec 3.3/3.4,
 * STATE D12).  Decision tables on a real declared arena, the MODE
 * full-on state machine, and the named arena_stats counters.
 * ------------------------------------------------------------------
 */

#define CORTEN_ARENA_TEST_PERM_RW	(CORTEN_PERM_READ | CORTEN_PERM_WRITE | \
					 CORTEN_PERM_USER)

/* Metadata of @addr through the transaction API (the arena-side twin
 * of the fault test's ft_meta()).
 */
static int corten_arena_test_meta(struct mm_struct *mm, unsigned long addr,
				  struct corten_pte_meta *out)
{
	struct corten_txn txn;
	int ret;

	ret = corten_lock_range(mm, addr, PAGE_SIZE, &txn);
	if (ret)
		return ret;
	ret = corten_query(&txn, addr, out);
	corten_unlock(&txn);

	return ret;
}

/* Record a PrivateAnon virtual allocation with @perm -- fill_upper()
 * first, exactly like the mark route (corten_arena_mmap_route) does.
 */
static int corten_arena_test_mark(struct mm_struct *mm, unsigned long addr,
				  u8 perm)
{
	struct corten_arena *ar;
	struct corten_txn txn;
	struct corten_pte_meta m = { };
	int ret;

	ar = corten_arena_lookup_get(mm, addr);
	if (!ar)
		return -ENOENT;
	ret = corten_arena_fill_upper(ar, addr);
	if (ret)
		goto out;
	ret = corten_lock_range(mm, addr, PAGE_SIZE, &txn);
	if (ret)
		goto out;
	m.state = CORTEN_PRIVATE_ANON;
	m.perm = perm;
	ret = corten_mark(&txn, addr, PAGE_SIZE, &m);
	corten_unlock(&txn);
out:
	percpu_ref_put(&ar->active);

	return ret;
}

/* Read one named counter out of the arena_stats debugfs render (the
 * very lines the DoD evidence greps).
 */
static long corten_arena_test_named_counter(struct kunit *test,
					    const char *name)
{
	char *dbg = corten_test_render_dbg(CORTEN_DBG_ARENA_STATS);
	char *val, *end;
	long n = -1;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dbg);
	val = strstr(dbg, name);
	if (val) {
		val += strlen(name);
		while (*val == ' ' || *val == '\t')
			val++;
		/* kstrtol() wants the whole string: end the line before
		 * parsing (the render continues past this counter).
		 */
		end = strchr(val, '\n');
		if (end)
			*end = '\0';
		if (kstrtol(val, 10, &n))
			n = -1;
	}
	kfree(dbg);

	return n;
}

static void corten_arena_test_mprotect_route(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct corten_pte_meta m;
	struct corten_arena *ar;
	struct vm_area_struct *vma;
	u8 prot;
	long r0;

	if (!corten_enabled_static())
		kunit_skip(test, "mprotect route requires corten=on");

	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_mark(mm, CORTEN_ARENA_TEST_BASE,
					       CORTEN_ARENA_TEST_PERM_RW), 0);

	/* MODE matrix: a MODE-targeted arena (mode=0) keeps the S6
	 * verdict -- permission changes are not routed without MODE.
	 */
	mmap_write_lock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mprotect_route(mm, CORTEN_ARENA_TEST_BASE,
						    2 * PMD_SIZE,
						    PROT_READ | PROT_WRITE,
						    -1), -EOPNOTSUPP);
	mmap_write_unlock(mm);

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* Out-of-scope combinations: pkey and non-RWX prot bits. */
	mmap_write_lock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mprotect_route(mm, CORTEN_ARENA_TEST_BASE,
						    PMD_SIZE, PROT_READ, 0),
			-EOPNOTSUPP);
	mmap_write_unlock(mm);
	mmap_write_lock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mprotect_route(mm, CORTEN_ARENA_TEST_BASE,
						    PMD_SIZE,
						    PROT_READ | PROT_GROWSDOWN,
						    -1), -EOPNOTSUPP);
	mmap_write_unlock(mm);

	/* PARTIAL: the range reaches past the arena boundary. */
	mmap_write_lock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mprotect_route(mm,
						    CORTEN_ARENA_TEST_BASE +
						    3 * PMD_SIZE,
						    2 * PMD_SIZE, PROT_READ,
						    -1), -EOPNOTSUPP);
	mmap_write_unlock(mm);

	/* In-arena CHUNK: routed, recorded perm moves with it. */
	r0 = corten_arena_test_named_counter(test, "mprotect_routes");
	mmap_write_lock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mprotect_route(mm, CORTEN_ARENA_TEST_BASE,
						    2 * PMD_SIZE, PROT_READ,
						    -1), 1);
	mmap_write_unlock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_BASE,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_PRIVATE_ANON);
	KUNIT_EXPECT_EQ(test, m.perm,
			CORTEN_PERM_READ | CORTEN_PERM_USER);
	KUNIT_EXPECT_GT(test,
			corten_arena_test_named_counter(test,
							"mprotect_routes"),
			r0);

	/* Whole arena to PROT_NONE: the FRESH gate's upper bound and the
	 * shadow-VMA flags follow, so a never-faulted page cannot
	 * resurrect the old permissions.
	 */
	mmap_write_lock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mprotect_route(mm, CORTEN_ARENA_TEST_BASE,
						    CORTEN_ARENA_TEST_LEN,
						    PROT_NONE, -1), 1);
	mmap_write_unlock(mm);
	rcu_read_lock();
	ar = corten_arena_lookup(mm, CORTEN_ARENA_TEST_BASE);
	if (ar)
		prot = READ_ONCE(ar->prot);
	else
		prot = CORTEN_PERM_ALL;
	rcu_read_unlock();
	KUNIT_EXPECT_EQ(test, prot, CORTEN_PERM_USER);
	vma = vma_lookup(mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_EXPECT_FALSE(test, vma->vm_flags &
			   (VM_READ | VM_WRITE | VM_EXEC));
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_BASE,
					       &m), 0);
	/* PROT_NONE: the R/W/E bits drop, the USER valid bit stays -- the
	 * same USER-only encoding the ar->prot check above read back.
	 */
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_PERM_USER);

	/* And back up: the chunk perm transaction is reversible. */
	mmap_write_lock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mprotect_route(mm, CORTEN_ARENA_TEST_BASE,
						    CORTEN_ARENA_TEST_LEN,
						    PROT_READ | PROT_WRITE,
						    -1), 1);
	mmap_write_unlock(mm);
	vma = vma_lookup(mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_EXPECT_TRUE(test, vma->vm_flags & VM_READ);
	KUNIT_EXPECT_TRUE(test, vma->vm_flags & VM_WRITE);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_BASE,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_ARENA_TEST_PERM_RW);
}

static void corten_arena_test_madvise_route(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct corten_pte_meta m;

	if (!corten_enabled_static())
		kunit_skip(test, "madvise route requires corten=on");

	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_mark(mm, CORTEN_ARENA_TEST_BASE,
					       CORTEN_ARENA_TEST_PERM_RW), 0);

	/* MODE matrix: a MODE-targeted arena (mode=0) keeps the S6
	 * behaviour -- FREE and hints reject on it.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_FREE,
						   CORTEN_ARENA_TEST_BASE,
						   PAGE_SIZE), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_NORMAL,
						   CORTEN_ARENA_TEST_BASE,
						   PAGE_SIZE), -EOPNOTSUPP);

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* MADV_FREE folded into the DONTNEED transaction: the content is
	 * dropped (an eager drop is a compliance superset of lazy-free),
	 * the shadow-VMA and its VA stay.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_FREE,
						   CORTEN_ARENA_TEST_BASE,
						   PAGE_SIZE), 1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_BASE,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);
	KUNIT_EXPECT_NOT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_BASE));

	/* Pure hints: counted no-op successes inside the arena. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_NORMAL,
						   CORTEN_ARENA_TEST_BASE,
						   PAGE_SIZE), 1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_SEQUENTIAL,
						   CORTEN_ARENA_TEST_BASE +
						   PMD_SIZE, PMD_SIZE), 1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_RANDOM,
						   CORTEN_ARENA_TEST_BASE,
						   CORTEN_ARENA_TEST_LEN), 1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_COLD,
						   CORTEN_ARENA_TEST_BASE,
						   PAGE_SIZE), 1);

	/* WILLNEED stays rejected (sec 3.4 matrix). */
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_WILLNEED,
						   CORTEN_ARENA_TEST_BASE,
						   PAGE_SIZE), -EOPNOTSUPP);

	/* A hint fully outside any arena: legacy (0). */
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_NORMAL,
						   CORTEN_ARENA_TEST_NOWHERE,
						   PAGE_SIZE), 0);

	/* A hint crossing the arena boundary: reject. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_NORMAL,
						   CORTEN_ARENA_TEST_BASE +
						   3 * PMD_SIZE, 2 * PMD_SIZE),
			-EOPNOTSUPP);

	/* DONTNEED keeps its S6 routing. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_DONTNEED,
						   CORTEN_ARENA_TEST_BASE,
						   PAGE_SIZE), 1);
}

/* mremap routing needs the calling task's mm attached (the move path
 * copies through the user addresses), so the seed/move/verify steps run
 * on the dedicated op thread like the RELEASE cases do.
 */
static void corten_arena_test_op_mremap(struct corten_arena_test_op *o)
{
	o->retl = corten_arena_mremap_route(o->mm, o->addr, o->len, o->len2,
					    o->flags, o->new_addr);
}

/* Seed/verify the payload through the arena's own fault paths. */
static void corten_arena_test_op_copy_out(struct corten_arena_test_op *o)
{
	o->ret = copy_to_user((void __user *)o->addr, o->buf, 8) ? -EIO : 0;
}

static void corten_arena_test_op_copy_in(struct corten_arena_test_op *o)
{
	o->ret = copy_from_user(o->buf, (void __user *)o->addr, 8) ? -EIO : 0;
}

/* run_op variant carrying the full T0b argument block. */
static int corten_arena_test_run_op_full(struct kunit *test,
					 struct corten_arena_test_op *o)
{
	struct task_struct *tsk;

	init_completion(&o->done);
	tsk = kthread_run(corten_arena_test_op_thread, o, "corten_arena_op");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tsk);
	wait_for_completion(&o->done);

	return o->ret;
}

static void corten_arena_test_mremap_route(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct corten_pte_meta m;
	struct vm_area_struct *vma;
	char pattern[9] = "T0BMOVE1";
	char back[9] = { 0 };
	struct corten_arena_test_op o;
	long new_addr, r0;
	int ret;

	if (!corten_enabled_static())
		kunit_skip(test, "mremap route requires corten=on");

	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* Decision table (no user memory touched; the direct call is the
	 * same context the syscall hook runs in).  Explicit target onto
	 * the arena, VA donation, a no-MAYMOVE grow and a boundary-
	 * crossing old range are all counted rejects.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_mremap_route(mm, CORTEN_ARENA_TEST_BASE,
						  2 * PMD_SIZE, 4 * PMD_SIZE,
						  MREMAP_FIXED | MREMAP_MAYMOVE,
						  CORTEN_ARENA_TEST_BASE +
						  PMD_SIZE), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mremap_route(mm, CORTEN_ARENA_TEST_BASE,
						  2 * PMD_SIZE, 4 * PMD_SIZE,
						  MREMAP_DONTUNMAP |
						  MREMAP_MAYMOVE, 0),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mremap_route(mm, CORTEN_ARENA_TEST_BASE,
						  2 * PMD_SIZE, 4 * PMD_SIZE,
						  0, 0), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mremap_route(mm,
						  CORTEN_ARENA_TEST_BASE +
						  3 * PMD_SIZE, 2 * PMD_SIZE,
						  6 * PMD_SIZE,
						  MREMAP_MAYMOVE, 0),
			-EOPNOTSUPP);
	KUNIT_EXPECT_GT(test,
			corten_arena_test_named_counter(test,
							"mremap_rejects"), 0);

	/* In-place shrink: same address back, tail content dropped
	 * through the chunk-zap transaction.
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_mark(mm, CORTEN_ARENA_TEST_BASE,
					       CORTEN_ARENA_TEST_PERM_RW), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_mark(mm, CORTEN_ARENA_TEST_BASE +
					       PMD_SIZE,
					       CORTEN_ARENA_TEST_PERM_RW), 0);
	r0 = corten_arena_test_named_counter(test, "mremap_routes");
	new_addr = corten_arena_mremap_route(mm, CORTEN_ARENA_TEST_BASE,
					     2 * PMD_SIZE, PMD_SIZE, 0, 0);
	KUNIT_EXPECT_EQ(test, new_addr, (long)CORTEN_ARENA_TEST_BASE);
	KUNIT_EXPECT_GT(test,
			corten_arena_test_named_counter(test, "mremap_routes"),
			r0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_BASE,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_PRIVATE_ANON);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_BASE +
					       PMD_SIZE, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);

	/* Grow: kernel-copy into a fresh window arena, the old arena is
	 * retired.  The content must survive the move.
	 */
	vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_WIN,
				     CORTEN_ARENA_TEST_WIN +
				     CORTEN_ARENA_TEST_WIN_LEN,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	/* auto_attach() is the do_mmap() tail hook: DECLARE's locked body
	 * runs under the caller's mmap_write (mmap_region()'s contract).
	 */
	mmap_write_lock(mm);
	ret = corten_arena_auto_attach(mm, CORTEN_ARENA_TEST_WIN,
				       CORTEN_ARENA_TEST_WIN_LEN);
	mmap_write_unlock(mm);
	KUNIT_ASSERT_EQ(test, ret, 0);

	o = (struct corten_arena_test_op) {
		.mm = mm,
		.fn = corten_arena_test_op_copy_out,
		.addr = CORTEN_ARENA_TEST_WIN,
		.buf = pattern,
	};
	KUNIT_ASSERT_EQ(test, corten_arena_test_run_op_full(test, &o), 0);

	o = (struct corten_arena_test_op) {
		.mm = mm,
		.fn = corten_arena_test_op_mremap,
		.addr = CORTEN_ARENA_TEST_WIN,
		.len = CORTEN_ARENA_TEST_WIN_LEN,
		.len2 = 2 * CORTEN_ARENA_TEST_WIN_LEN,
		.flags = MREMAP_MAYMOVE,
	};
	KUNIT_ASSERT_EQ(test, corten_arena_test_run_op_full(test, &o), 0);
	new_addr = o.retl;
	/* The cursor placement skipped the WIN arena obstacle. */
	KUNIT_EXPECT_GE(test, new_addr,
			(long)(CORTEN_ARENA_TEST_WIN +
			       CORTEN_ARENA_TEST_WIN_LEN));
	KUNIT_EXPECT_EQ(test, new_addr & (PMD_SIZE - 1), 0);

	/* The content survived the kernel copy. */
	o = (struct corten_arena_test_op) {
		.mm = mm,
		.fn = corten_arena_test_op_copy_in,
		.addr = new_addr,
		.buf = back,
	};
	KUNIT_ASSERT_EQ(test, corten_arena_test_run_op_full(test, &o), 0);
	KUNIT_EXPECT_EQ(test, memcmp(back, pattern, 8), 0);

	/* The old arena is retired, the new one is a live arena. */
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_WIN),
			0);
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, new_addr), 1);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_NOT_NULL(test, vma_lookup(mm, new_addr));
}

static struct kunit_case corten_arena_test_cases[] = {
	KUNIT_CASE(corten_arena_test_declare_reject),
	KUNIT_CASE(corten_arena_test_declare_reject_flags),
	KUNIT_CASE(corten_arena_test_declare_query),
	KUNIT_CASE(corten_arena_test_range_overlaps),
	KUNIT_CASE(corten_arena_test_release),
	KUNIT_CASE(corten_arena_test_ref_born_atomic),
	KUNIT_CASE(corten_arena_test_release_classify),
	KUNIT_CASE(corten_arena_test_shadow_vma),
	KUNIT_CASE(corten_arena_test_exit),
	KUNIT_CASE(corten_arena_test_prctl),
	KUNIT_CASE(corten_arena_test_mode),
	KUNIT_CASE(corten_arena_test_auto_classify),
	KUNIT_CASE(corten_arena_test_auto_place),
	KUNIT_CASE(corten_arena_test_auto_route),
	KUNIT_CASE(corten_arena_test_auto_attach_release),
	KUNIT_CASE(corten_arena_test_mprotect_route),
	KUNIT_CASE(corten_arena_test_madvise_route),
	KUNIT_CASE(corten_arena_test_mremap_route),
	KUNIT_CASE(corten_arena_test_fork_demote),
	KUNIT_CASE(corten_arena_test_fork_demote_perm),
	KUNIT_CASE(corten_arena_test_concurrent),
	KUNIT_CASE(corten_arena_test_concurrent_window),
	KUNIT_CASE(corten_arena_test_obs_ledger),
	KUNIT_CASE(corten_arena_test_drain_timeout_stat),
	{}
};

static struct kunit_suite corten_arena_test_suite = {
	.name = "corten_arena",
	.test_cases = corten_arena_test_cases,
};

kunit_test_suites(&corten_arena_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for the CortenMM arena registration layer");
