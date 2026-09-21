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
#include <linux/swap.h>
#include <linux/swapops.h>
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

/* The glibc thread-arena shape: a PROT_NONE MAP_NORESERVE reserve
 * (new_heap()), committed sub-ranges arrive later as routed
 * mprotect() calls.
 */
#define CORTEN_ARENA_TEST_FLAGS_NONE	(VM_MAYREAD | VM_MAYWRITE | \
					 VM_MAYEXEC | VM_NORESERVE)

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
	/* A5: ENTER is registry-free -- the mode bit lives in the
	 * mm_struct and the registry is only built by the first arena
	 * work (route/DECLARE/fork mirror).  This is the G5 root-cause
	 * fix: an ENTER-only MODE lifecycle leaves no state behind and
	 * its exit pays no grace period.
	 */
	KUNIT_EXPECT_NULL(test, READ_ONCE(mm->corten_state));
	/* Re-enter is idempotent. */
	KUNIT_EXPECT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_mode_get(mm), 1);

	/* Exit with no arenas (and, since A5, no registry at all): mode
	 * cleared, the NULL state is a valid no-op.
	 */
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
	int cpu, i, ncpus;

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
	/* A5: ENTER created no registry; a declined route must not
	 * create one either (its gate runs before get_state()).
	 */
	KUNIT_EXPECT_NULL(test, READ_ONCE(mm->corten_state));

	/* Non-whitelisted flags: legacy, window untouched. */
	flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
	addr = 0;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_auto_route_locked(mm, len, PROT_READ |
						     PROT_WRITE, &addr, &len,
						     &flags), 0);
	KUNIT_EXPECT_EQ(test, addr, 0);
	KUNIT_EXPECT_NULL(test, READ_ONCE(mm->corten_state));

	/* Plain whitelist hit: rewritten onto the window base, 2M-rounded,
	 * MAP_FIXED|MAP_NORESERVE forced.  The running cpu's magazine
	 * claims the very first segment of the window, so the handed-out
	 * address is the window base (deterministic: the claim cursor
	 * starts there no matter which cpu runs this thread).
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
	/* The whitelist hit built the registry lazily (A5). */
	state = READ_ONCE(mm->corten_state);
	KUNIT_ASSERT_NOT_NULL(test, state);
	/* The claim advanced the global cursor past the whole segment. */
	KUNIT_EXPECT_EQ(test, state->next_va,
			CORTEN_MODE_WINDOW_START + CORTEN_VA_SEG_SIZE);

	/*
	 * Deterministic per-cpu bump semantics via the explicit-cpu test
	 * hook (migration-proof, unlike the running cpu): the hook cpu's
	 * first claim takes the NEXT segment (window base + segment
	 * size), and its bump advances frame-wise.  The hook cpu must be
	 * one that has not claimed a segment yet -- the whitelist hit
	 * above claimed the *running* cpu's segment (that is where the
	 * window-base address came from), so a hardcoded cpu 0 would
	 * just continue that cpu's bump whenever the suite happens to
	 * run on cpu 0 (exactly what the first boot of this test
	 * logged).  Pick any online cpu whose segment is still
	 * all-zero, i.e. unclaimed.
	 */
	ncpus = num_online_cpus();
	cpu = ncpus;
	for (i = 0; i < ncpus; i++) {
		struct corten_va_seg *seg = per_cpu_ptr(state->va_segs, i);

		if (!seg->base && !seg->end && !seg->next) {
			cpu = i;
			break;
		}
	}
	if (cpu == ncpus)
		kunit_skip(test, "no unclaimed cpu segment (single-cpu?)");

	#define T12_SEG2 (CORTEN_MODE_WINDOW_START + CORTEN_VA_SEG_SIZE)
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_mag_alloc_cpu(mm, cpu, PMD_SIZE,
							&addr), 0);
	KUNIT_EXPECT_EQ(test, addr, T12_SEG2);
	KUNIT_EXPECT_EQ(test, state->next_va,
			CORTEN_MODE_WINDOW_START + 2 * CORTEN_VA_SEG_SIZE);

	/* Second bump: one frame onward. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_mag_alloc_cpu(mm, cpu, PMD_SIZE,
							&addr), 0);
	KUNIT_EXPECT_EQ(test, addr, T12_SEG2 + PMD_SIZE);

	/* Arena obstacle (a MODE-targeted DECLARE on the bump's next
	 * slot): the magazine skips the occupied frame instead of
	 * MAP_FIXED-destroying it.
	 */
	{
		struct vm_area_struct *vma;

		vma = corten_arena_test_mkvm(mm, T12_SEG2 + 2 * PMD_SIZE,
					     T12_SEG2 + 3 * PMD_SIZE,
					     CORTEN_ARENA_TEST_FLAGS_OK);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
		KUNIT_ASSERT_EQ(test,
				corten_arena_declare(mm,
						     T12_SEG2 + 2 * PMD_SIZE,
						     PMD_SIZE), 0);
	}

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_mag_alloc_cpu(mm, cpu, PMD_SIZE,
							&addr), 0);
	KUNIT_EXPECT_EQ(test, addr, T12_SEG2 + 3 * PMD_SIZE);

	/* Plain legacy VMA obstacle: also skipped, never overwritten
	 * (the T0 obstacle contract holds inside claimed segments).
	 */
	{
		struct vm_area_struct *vma;

		vma = corten_arena_test_mkvm(mm, T12_SEG2 + 4 * PMD_SIZE,
					     T12_SEG2 + 5 * PMD_SIZE,
					     CORTEN_ARENA_TEST_FLAGS_OK);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	}
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_mag_alloc_cpu(mm, cpu, PMD_SIZE,
							&addr), 0);
	KUNIT_EXPECT_EQ(test, addr, T12_SEG2 + 5 * PMD_SIZE);
	#undef T12_SEG2

	/* Multi-cpu segments: one allocation per online cpu -- pairwise
	 * disjoint, window-bound, frame-aligned (the PS-E1 contract the
	 * magazine implements: no two cpus share a VA span).
	 */
	{
		unsigned long addrs[8] = { 0 };
		int i, j, ncpus;

		ncpus = num_online_cpus();
		KUNIT_ASSERT_LE(test, ncpus, 8);
		for (i = 0; i < ncpus; i++) {
			KUNIT_EXPECT_EQ(test,
					corten_arena_test_mag_alloc_cpu(mm,
									i,
									PMD_SIZE,
									&addrs[i]),
					0);
			KUNIT_EXPECT_FALSE(test, addrs[i] <
					   CORTEN_MODE_WINDOW_START ||
					   addrs[i] >= CORTEN_MODE_WINDOW_END);
			KUNIT_EXPECT_EQ(test, addrs[i] & (PMD_SIZE - 1), 0);
			for (j = 0; j < i; j++)
				KUNIT_EXPECT_FALSE(test,
						   addrs[i] < addrs[j] +
						   PMD_SIZE &&
						   addrs[j] < addrs[i] +
						   PMD_SIZE);
		}
	}

	/* Oversized request (> one segment): the magazine declines and
	 * the global cursor path is consulted; with the cursor parked at
	 * the window end it degrades to legacy, counted.
	 */
	fallbacks = corten_arena_test_stat_sum(state,
					       CORTEN_ARENA_STAT_FALLBACKS);
	WRITE_ONCE(state->next_va, CORTEN_MODE_WINDOW_END);
	addr = 0xdead0000UL;
	len = CORTEN_VA_SEG_SIZE + PMD_SIZE;
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

/*
 * M4.T1 recycle: a released auto-arena's frames return to the magazine
 * and the next allocation re-uses the SAME address (markers restored, no
 * tree churn).  T1c note: the route-level churn now PARKS full-coverage
 * munmaps (the pool cases below), so this case anchors the magazine's
 * marker/counter mechanics through the direct RELEASE -- the teardown
 * path the pool's ejections, the MODE-exit and the non-MODE churn still
 * take.
 */
static void corten_arena_test_mag_recycle(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	unsigned long addr = 0, len = 2 * PAGE_SIZE, flags;
	struct vm_area_struct *vma;
	long recycles;
	int ret;

	if (!corten_enabled_static())
		kunit_skip(test, "auto-mmap route requires corten=on");
	if (sysctl_overcommit_memory == OVERCOMMIT_NEVER)
		kunit_skip(test, "auto takeover degraded (OVERCOMMIT_NEVER)");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* Take one auto arena at the window base. */
	addr = 0;
	len = 2 * PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_auto_route_locked(mm, len, PROT_READ |
						     PROT_WRITE, &addr, &len,
						     &flags), 1);
	KUNIT_EXPECT_EQ(test, addr, CORTEN_MODE_WINDOW_START);

	/* Attach it (the do_mmap tail hook body) so the release can see a
	 * real arena, then release it directly (the prctl RELEASE body:
	 * drain + teardown + magazine recycle).
	 * The route front half does not create the VMA -- mmap_region()
	 * does, between the two halves -- so stand one in first, exactly
	 * what the kernel flow would have installed.
	 */
	vma = corten_arena_test_mkvm(mm, CORTEN_MODE_WINDOW_START,
				     CORTEN_MODE_WINDOW_START + PMD_SIZE,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	mmap_write_lock(mm);
	ret = corten_arena_auto_attach(mm, CORTEN_MODE_WINDOW_START, PMD_SIZE);
	mmap_write_unlock(mm);
	KUNIT_ASSERT_EQ(test, ret, 0);

	recycles = corten_arena_test_va_recycles();
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_release,
						 CORTEN_MODE_WINDOW_START,
						 PMD_SIZE),
			0);
	/* The release restored the frame's reserve marker and pushed it
	 * onto the recycle list.  The recycle COUNTER only moves when a
	 * placement SERVES the block (release is bookkeeping-only).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_va_recycles(), recycles);
	KUNIT_EXPECT_TRUE(test, corten_arena_test_frame_reserved(mm,
								 CORTEN_MODE_WINDOW_START));
	/* The state exists (MODE entered); the released frame reads as
	 * "not inside an arena" == 0.  -ENOENT is reserved for mms that
	 * never created a registry.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_MODE_WINDOW_START),
			0);

	/* The next placement serves the recycled frame: same address. */
	addr = 0;
	len = 2 * PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_auto_route_locked(mm, len, PROT_READ |
						     PROT_WRITE, &addr, &len,
						     &flags), 1);
	KUNIT_EXPECT_EQ(test, addr, CORTEN_MODE_WINDOW_START);
	KUNIT_EXPECT_EQ(test, corten_arena_test_va_recycles(), recycles + 1);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
}

/*
 * M4.T1 marker discipline: claimed-but-unallocated magazine frames read
 * as "no arena" to the fault/munmap lookups while still blocking a
 * targeted DECLARE from inside the registry.
 */
static void corten_arena_test_mag_marker(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	unsigned long addr = 0, len = 2 * PAGE_SIZE, flags;

	if (!corten_enabled_static())
		kunit_skip(test, "auto-mmap route requires corten=on");
	if (sysctl_overcommit_memory == OVERCOMMIT_NEVER)
		kunit_skip(test, "auto takeover degraded (OVERCOMMIT_NEVER)");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	addr = 0;
	len = 2 * PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_auto_route_locked(mm, len, PROT_READ |
						     PROT_WRITE, &addr, &len,
						     &flags), 1);

	/* Reserve marker on the handed-out-but-unallocated frame. */
	KUNIT_EXPECT_TRUE(test, corten_arena_test_frame_reserved(mm,
								 CORTEN_MODE_WINDOW_START));
	/* Lookups report no arena; a fault there falls back to the legacy
	 * funnel on the (plain) VMA the caller is about to create.
	 */
	rcu_read_lock();
	KUNIT_EXPECT_NULL(test,
			  corten_arena_lookup(mm, CORTEN_MODE_WINDOW_START));
	rcu_read_unlock();
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_MODE_WINDOW_START),
			0);

	/* A targeted DECLARE overlapping the claimed span's unallocated
	 * frames still fails on the VMA validation chain (no VMA there).
	 * Overlap-rejection of the marker itself is exercised by the
	 * registry walk: re-DECLARE must not silently succeed.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_declare(mm, CORTEN_MODE_WINDOW_START,
					     PMD_SIZE), -EINVAL);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
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

	/* A5: ENTER creates no registry -- the attach below builds it on
	 * demand (get_state in the do_mmap tail hook body), exactly the
	 * shape of a MODE process whose first registry touch is its
	 * first glibc-shaped mapping.
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
	 * the full-coverage rule.  T1c: in a MODE process the arena parks
	 * instead of tearing down: lookup-invisible (query 0), the VMA
	 * stays as a reserved PROT_NONE mapping, the pool holds it.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE),
			1);
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_WIN),
			0);
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_pool_idle(mm,
						      CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_NOT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));

	/*
	 * M4.T2 regression anchor (r06-m4t12 drain-timeout): cross-frame
	 * full-coverage release.  start and end-1 fall in DIFFERENT 2M
	 * frames of one arena, so the route holds TWO references to the
	 * same descriptor and the EXACT branch must drop both, or the
	 * RELEASE drain times out (the r03 DoD-B leak shape; the JVM's
	 * 64MB heap free is its real-world form).  The tail is 1 page
	 * < 2M, so the release-on-full-coverage rule stays in force.
	 * The same-frame glibc one-pager is the WIN block above.  A
	 * released arena reads query()==0 ("state exists, not in an
	 * arena"), never -ENOENT (reserved for registry-less mms).
	 */
	{
		long drains = corten_arena_test_drain_timeouts();

		vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_START2,
					     CORTEN_ARENA_TEST_START2 +
					     2 * PMD_SIZE,
					     CORTEN_ARENA_TEST_FLAGS_OK);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
		mmap_write_lock(mm);
		ret = corten_arena_auto_attach(mm, CORTEN_ARENA_TEST_START2,
					       2 * PMD_SIZE);
		mmap_write_unlock(mm);
		KUNIT_ASSERT_EQ(test, ret, 0);

		KUNIT_EXPECT_EQ(test,
				corten_arena_test_run_op(test, mm,
							 corten_arena_test_op_munmap_route,
							 CORTEN_ARENA_TEST_START2,
							 2 * PMD_SIZE - PAGE_SIZE),
				1);
		KUNIT_EXPECT_EQ(test,
				corten_arena_query(mm, CORTEN_ARENA_TEST_START2),
				0);
		KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(),
				drains);
	}

	/* T1c: MODE exit tears both parked reservations down through the
	 * idle-aware release body -- the teardown anchor the two munmaps
	 * above used to carry (the whole arena, VMA included, is gone).
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_START2));
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 0);

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
 * M5.T1a: faithful fork (M5_FORK_SPEC.md sec 1.3, DEV-14/DEV-15) --
 * the OQ-D replacement for the T0 fork_demote.  The hooks run inside a
 * dup_mmap()-shaped window (oldmm mmap_write held, child nested); the
 * child-side VMA/PTE shapes that the real dup_mmap() produces are
 * simulated by the helpers below, the established ft_* scaffolding
 * pattern (a KUnit thread cannot fork a real user process).
 * ------------------------------------------------------------------
 */

#define CORTEN_ARENA_TEST_FORK_LEN	(2UL * PMD_SIZE)

/* Metadata of @addr through the transaction API; defined with the T0b
 * routing cases below, used by the fork cases above.
 */
static int corten_arena_test_meta(struct mm_struct *mm, unsigned long addr,
				  struct corten_pte_meta *out);

/* The gated PMD walk (the test-side twin of corten_arena_pmd(); that one
 * is static to mm/corten_arena.c).
 */
static pmd_t *corten_arena_test_pmd(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgdp = pgd_offset(mm, addr);
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;

	if (!pgd_present(READ_ONCE(*pgdp)))
		return NULL;
	p4dp = p4d_offset(pgdp, addr);
	if (!p4d_present(READ_ONCE(*p4dp)))
		return NULL;
	pudp = pud_offset(p4dp, addr);
	if (!pud_present(READ_ONCE(*pudp)))
		return NULL;
	pmdp = pmd_offset(pudp, addr);
	if (!pmd_present(READ_ONCE(*pmdp)))
		return NULL;

	return pmdp;
}

/* Seed one CORTEN_MAPPED slot: allocate a folio, install the writable
 * PTE (the map_anon() store sequence, hand-driven) and record the
 * metadata (map + mark).  The folio reference becomes the PTE
 * reference.
 */
static int corten_arena_test_fork_seed_mapped(struct mm_struct *mm,
					      unsigned long addr)
{
	struct corten_txn txn;
	struct vm_area_struct *vma;
	struct folio *folio;
	struct page *page;
	pte_t *ptep, entry;
	spinlock_t *ptl;		/* guards the map_anon-style install */
	pmd_t *pmdp;
	pgprot_t pgprot;
	struct corten_pte_meta m = { };
	int ret;

	vma = vma_lookup(mm, addr);
	if (!vma)
		return -ENOENT;

	folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	if (!folio)
		return -ENOMEM;
	page = folio_page(folio, 0);
	__folio_mark_uptodate(folio);

	ret = corten_lock_range(mm, addr, PAGE_SIZE, &txn);
	if (ret) {
		folio_put(folio);
		return ret;
	}
	ret = corten_map(&txn, addr, page,
			 CORTEN_PERM_READ | CORTEN_PERM_WRITE |
			 CORTEN_PERM_USER, 0);
	if (ret) {
		corten_unlock(&txn);
		folio_put(folio);
		return ret;
	}
	m.state = CORTEN_MAPPED;
	m.perm = CORTEN_PERM_READ | CORTEN_PERM_WRITE | CORTEN_PERM_USER;
	ret = corten_mark(&txn, addr, PAGE_SIZE, &m);
	corten_unlock(&txn);
	if (ret) {
		folio_put(folio);
		return ret;
	}

	pmdp = corten_arena_test_pmd(mm, addr);
	if (!pmdp)
		return -ENOENT;
	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	if (!ptep) {
		folio_put(folio);
		return -EAGAIN;
	}
	pgprot = vm_get_page_prot(VM_READ | VM_WRITE);
	entry = pte_mkwrite(pte_mkdirty(mk_pte(page, pgprot)), vma);
	add_mm_counter(mm, MM_ANONPAGES, 1);
	folio_add_new_anon_rmap(folio, vma, addr, RMAP_EXCLUSIVE);
	set_ptes(mm, addr, ptep, entry, 1);
	pte_unmap_unlock(ptep, ptl);

	return 0;
}

/* Seed one CORTEN_PRIVATE_ANON slot (the virtual allocation): fill the
 * window and mark.  No page, no PTE.
 */
static int corten_arena_test_fork_seed_anon(struct mm_struct *mm,
					    unsigned long addr)
{
	struct corten_txn txn;
	struct corten_pte_meta m = { };
	struct corten_arena *ar;
	int ret;

	ar = corten_arena_lookup_get(mm, addr);
	if (!ar)
		return -ENOENT;
	ret = corten_arena_fill_upper(ar, addr);
	percpu_ref_put(&ar->active);
	if (ret)
		return ret;

	ret = corten_lock_range(mm, addr, PAGE_SIZE, &txn);
	if (ret)
		return ret;
	m.state = CORTEN_PRIVATE_ANON;
	m.perm = CORTEN_PERM_READ | CORTEN_PERM_WRITE | CORTEN_PERM_USER;
	ret = corten_mark(&txn, addr, PAGE_SIZE, &m);
	corten_unlock(&txn);

	return ret;
}

/* Track @addr's window (fill_upper()) without recording anything: the
 * seed_mapped() lock_range() needs a tracked PT page, which fork_faithful
 * gets for free by seeding an anon slot first.  The M2a descriptor
 * install fires inside the pX_alloc hook on a corten=on boot; re-arm
 * covers a corten=off one.
 */
static int corten_arena_test_fill_window(struct mm_struct *mm,
					 unsigned long addr)
{
	struct corten_arena *ar = corten_arena_lookup_get(mm, addr);
	int ret;

	if (!ar)
		return -ENOENT;
	ret = corten_arena_fill_upper(ar, addr);
	percpu_ref_put(&ar->active);

	return ret;
}

/* The child's page-table page for @addr: copy_page_range() pte_allocs
 * one for every present parent PT page (metadata-only windows included,
 * which is how routed-mprotect pending perms get a carrier in the
 * child).  The M2a descriptor install runs inside pte_alloc_one on a
 * corten=on boot, and re-arm covers a corten=off one (the same
 * gate-free convention as the suites).
 */
static int corten_arena_test_fork_ensure_pt(struct kunit *test,
					    struct mm_struct *dst,
					    unsigned long addr)
{
	pgd_t *pgdp = pgd_offset(dst, addr);
	p4d_t *p4dp = p4d_alloc(dst, pgdp, addr);
	pud_t *pudp = p4dp ? pud_alloc(dst, p4dp, addr) : NULL;
	pmd_t *dpmdp = pudp ? pmd_alloc(dst, pudp, addr) : NULL;

	if (!dpmdp) {
		KUNIT_FAIL(test, "child PT allocation failed");
		return -ENOMEM;
	}
	if (pte_alloc(dst, dpmdp)) {
		KUNIT_FAIL(test, "child PTE alloc failed");
		return -ENOMEM;
	}
	corten_ptdesc_rearm(dst, pmd_pgtable(*dpmdp));

	return 0;
}

/* The copy_page_range() leg for one PTE: ensure the child's PT page
 * (with its gate-free descriptor re-arm), wrprotect the source and dup
 * the mapping into the child -- folio_try_dup_anon_rmap_pte is the very
 * primitive __copy_present_ptes() uses; it drops PageAnonExclusive and
 * raises the mapcount.  Returns the source folio, or NULL.
 */
static struct folio *
corten_arena_test_fork_copy_pte(struct kunit *test, struct mm_struct *dst,
				struct mm_struct *src, unsigned long addr)
{
	struct vm_area_struct *svma, *dvma;
	struct folio *folio = NULL;
	pte_t *sptep, *dptep, pte;
	/* sptl: the wrprotect + child-leg dup; dptl: the child install. */
	spinlock_t *sptl, *dptl;
	pmd_t *pmdp;

	svma = vma_lookup(src, addr);
	dvma = vma_lookup(dst, addr);
	if (!svma || !dvma)
		return NULL;

	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_ensure_pt(test, dst,
							       addr), 0);

	pmdp = corten_arena_test_pmd(src, addr);
	if (!pmdp)
		return NULL;
	sptep = pte_offset_map_lock(src, pmdp, addr, &sptl);
	if (!sptep)
		return NULL;

	pte = ptep_get(sptep);
	if (!pte_present(pte) || pte_special(pte)) {
		pte_unmap_unlock(sptep, sptl);
		return NULL;
	}
	folio = page_folio(pte_page(pte));
	set_ptes(src, addr, sptep, pte_wrprotect(pte), 1);
	/* folio_get pairs with the child's PTE reference, exactly like
	 * copy_present_pte()'s folio_get before the dup; 0 = the mapping
	 * was duplicated (nonzero means "copy the page instead").
	 */
	folio_get(folio);
	KUNIT_EXPECT_EQ(test,
			folio_try_dup_anon_rmap_pte(folio,
						    folio_page(folio, 0),
						    dvma, svma),
			0);
	pte_unmap_unlock(sptep, sptl);

	pmdp = corten_arena_test_pmd(dst, addr);
	if (!pmdp)
		return folio;
	dptep = pte_offset_map_lock(dst, pmdp, addr, &dptl);
	if (!dptep)
		return folio;
	set_ptes(dst, addr, dptep, pte_wrprotect(pte), 1);
	add_mm_counter(dst, MM_ANONPAGES, 1);
	pte_unmap_unlock(dptep, dptl);

	return folio;
}

/* The fork window: oldmm mmap_write held with the child's nested
 * behind it, exactly like dup_mmap() at both hook points (the DEV-13
 * outermost lock; mmap_write_lock_nested(mm, SINGLE_DEPTH_NESTING)).
 */
static int corten_arena_test_fork_begin(struct mm_struct *child,
					struct mm_struct *parent)
{
	int ret;

	mmap_write_lock(parent);
	mmap_write_lock_nested(child, SINGLE_DEPTH_NESTING);
	ret = corten_arena_fork_begin(child, parent);
	mmap_write_unlock(child);
	mmap_write_unlock(parent);

	return ret;
}

static int corten_arena_test_fork_commit(struct mm_struct *child,
					 struct mm_struct *parent)
{
	int ret;

	mmap_write_lock(parent);
	mmap_write_lock_nested(child, SINGLE_DEPTH_NESTING);
	ret = corten_arena_fork_commit(child, parent);
	mmap_write_unlock(child);
	mmap_write_unlock(parent);

	return ret;
}

static void corten_arena_test_fork_faithful(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm, *child;
	struct vm_area_struct *cvma;
	struct corten_arena *car;
	struct corten_mm_state *cstate;
	struct corten_pte_meta m;
	unsigned long anon_addr = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	unsigned long map_addr = CORTEN_ARENA_TEST_BASE + 2 * PAGE_SIZE;
	long timeouts_before = corten_arena_test_drain_timeouts();
	long faithful_before = corten_arena_test_fork_faithful_count();

	/* The metadata mirror runs real transactions: on a corten=off
	 * boot the PT pages are untracked (re-arm is gated) and the
	 * off-boot contract is the plain-VMA body -- nothing to observe
	 * (the S4 degraded scope, like the real fault-chain cases).
	 */
	if (!corten_enabled_static())
		kunit_skip(test, "metadata mirror requires corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	/* The harness VMA only covers the primary range; the second
	 * arena needs its own DECLARE-contract VMA.
	 */
	cvma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_START2,
				      CORTEN_ARENA_TEST_START2 +
				      CORTEN_ARENA_TEST_LEN2,
				      CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_START2,
					     CORTEN_ARENA_TEST_LEN2),
			0);

	/* Two committed shapes: a virtual allocation (no content) and a
	 * mapped page (real folio), both in the first arena's first
	 * window.
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_fork_seed_anon(mm, anon_addr), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_fork_seed_mapped(mm, map_addr), 0);

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);

	/* dup_mmap(): begin before any VMA is copied... */
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);

	/* The freeze window (DEV-15): both parent arenas frozen, the
	 * MODE bit inherited, the child registry created.
	 */
	KUNIT_EXPECT_TRUE(test, READ_ONCE(child->corten_mode));
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_arena_frozen(mm,
							 CORTEN_ARENA_TEST_BASE));
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_arena_frozen(mm,
							 CORTEN_ARENA_TEST_START2));
	KUNIT_ASSERT_NOT_NULL(test, READ_ONCE(child->corten_state));

	/* ... then the child's shadow pieces appear (vm_area_dup), and
	 * the PTE layer copies (copy_page_range, the DEV-14 glue).
	 */
	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_BASE,
				      CORTEN_ARENA_TEST_BASE +
				      CORTEN_ARENA_TEST_LEN,
				      CORTEN_ARENA_TEST_FLAGS_OK |
				      VM_CORTEN | VM_NOHUGEPAGE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_START2,
				      CORTEN_ARENA_TEST_START2 +
				      CORTEN_ARENA_TEST_LEN2,
				      CORTEN_ARENA_TEST_FLAGS_OK |
				      VM_CORTEN | VM_NOHUGEPAGE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	KUNIT_EXPECT_NOT_NULL(test,
			      corten_arena_test_fork_copy_pte(test, child, mm,
							      map_addr));

	/* ... and commit mirrors the metadata + registry. */
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);

	/* Unfreeze closure ran: no frozen arena survives the fork. */
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_test_arena_frozen(mm,
							  CORTEN_ARENA_TEST_BASE));
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_test_arena_frozen(mm,
							  CORTEN_ARENA_TEST_START2));

	/* The child mirrors the registry, one descriptor per arena. */
	KUNIT_EXPECT_EQ(test, corten_arena_query(child,
						 CORTEN_ARENA_TEST_BASE), 1);
	KUNIT_EXPECT_EQ(test, corten_arena_query(child,
						 CORTEN_ARENA_TEST_START2), 1);

	cstate = READ_ONCE(child->corten_state);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cstate);
	rcu_read_lock();
	car = xa_load(&cstate->arenas,
		      CORTEN_ARENA_TEST_BASE >> PMD_SHIFT);
	if (car) {
		KUNIT_EXPECT_TRUE(test,
				  car->start == CORTEN_ARENA_TEST_BASE &&
				  car->end == CORTEN_ARENA_TEST_BASE +
				  CORTEN_ARENA_TEST_LEN);
		KUNIT_EXPECT_TRUE(test, READ_ONCE(car->mm) == child);
		KUNIT_EXPECT_FALSE(test, READ_ONCE(car->frozen));
	}
	rcu_read_unlock();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, car);

	/* The mapped page: SHARED producer on the parent, SHARED +
	 * WRITABLE mirror on the child (corten_mark's WRITABLE rule).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, map_addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, m.flags,
			CORTEN_PF_SHARED | CORTEN_PF_WRITABLE);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, map_addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, m.flags,
			CORTEN_PF_SHARED | CORTEN_PF_WRITABLE);
	KUNIT_EXPECT_EQ(test, m.perm,
			CORTEN_PERM_READ | CORTEN_PERM_WRITE |
			CORTEN_PERM_USER);

	/* The virtual allocation keeps its perm, gains no SHARED flag
	 * (only CORTEN_MAPPED content is shared), and mirrors to the
	 * child unchanged.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, anon_addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_PRIVATE_ANON);
	KUNIT_EXPECT_EQ(test, m.flags, 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, anon_addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_PRIVATE_ANON);
	KUNIT_EXPECT_EQ(test, m.flags, 0);

	/* An untouched arena has no PT page and no metadata on either
	 * side (-ENOENT): the pmd presence gate skipped the window, and
	 * the reservation costs nothing across the fork.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm,
					       CORTEN_ARENA_TEST_START2 +
					       PAGE_SIZE, &m), -ENOENT);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(child,
					       CORTEN_ARENA_TEST_START2 +
					       PAGE_SIZE, &m), -ENOENT);

	/* The commit counted. */
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_faithful_count(),
			faithful_before + 1);

	/* The child exits: its arenas drain through exit_mmap()
	 * (corten_arena_mm_exit), no leaks, no drain timeouts.
	 */
	mmput(child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(),
			timeouts_before);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
}

/* R-A (M5_FORK_SPEC.md sec 6): a frozen arena that never unfreezes is a
 * permanent, silent degradation of the parent -- the worst failure the
 * faithful fork can have.  Both commit-failure stages must run the
 * unfreeze closure, and the fork must abort.
 */
static void corten_arena_test_fork_unwind(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct mm_struct *child;
	long timeouts_before = corten_arena_test_drain_timeouts();
	long faithful_before = corten_arena_test_fork_faithful_count();

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);

	/* Stage 1: fail at commit entry -- nothing mirrored yet. */
	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_arena_frozen(mm,
							 CORTEN_ARENA_TEST_BASE));
	corten_arena_test_fork_fail_arm(1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm),
			-ENOMEM);
	corten_arena_test_fork_fail_arm(0);

	/* The red line: the parent is unfrozen, the fork aborted, the
	 * commit did not count.
	 */
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_test_arena_frozen(mm,
							  CORTEN_ARENA_TEST_BASE));
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_faithful_count(),
			faithful_before);
	KUNIT_EXPECT_EQ(test, corten_arena_query(child,
						 CORTEN_ARENA_TEST_BASE), 0);
	mmput(child);

	/* Stage 2: fail after the first arena was mirrored -- the child
	 * keeps the partial registry (the MMF_UNSTABLE exit path drains
	 * it) and the parent still unfreezes.
	 */
	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	{
		struct vm_area_struct *cvma;

		cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_BASE,
					      CORTEN_ARENA_TEST_BASE +
					      CORTEN_ARENA_TEST_LEN,
					      CORTEN_ARENA_TEST_FLAGS_OK |
					      VM_CORTEN | VM_NOHUGEPAGE);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	}
	corten_arena_test_fork_fail_arm(2);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm),
			-ENOMEM);
	corten_arena_test_fork_fail_arm(0);

	KUNIT_EXPECT_FALSE(test,
			   corten_arena_test_arena_frozen(mm,
							  CORTEN_ARENA_TEST_BASE));
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_faithful_count(),
			faithful_before);
	KUNIT_EXPECT_EQ(test, corten_arena_query(child,
						 CORTEN_ARENA_TEST_BASE), 1);

	mmput(child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(),
			timeouts_before);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
}

/* [F-B/OQ-1, M5 report] Since the D-G'' punch route a declared arena can
 * span several shadow pieces separated by legacy holes.  The fork
 * traversal must treat the pieces as one arena (the descriptor covers
 * every frame), mirror metadata per touched window, and leave the holes
 * to the standard copy_page_range() of their plain VMAs.  The punch
 * shape is synthesized by hand (split + hole flags + frame erase) --
 * the real punch route is guest-covered.
 */
static void corten_arena_test_fork_multipiece(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm, *child;
	struct vm_area_struct *vma, *head, *tail, *cvma;
	struct corten_mm_state *state;
	struct corten_pte_meta m;
	unsigned long hole = CORTEN_ARENA_TEST_BASE + PMD_SIZE;
	long skips_before;
	int ret;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);
	kunit_add_action(test, corten_arena_test_mm_destroy, t);
	t->test = test;
	t->mm = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->mm);
	mm = t->mm;

	vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_BASE,
				     CORTEN_ARENA_TEST_BASE +
				     CORTEN_ARENA_TEST_FORK_LEN,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_FORK_LEN),
			0);

	/* Punch a hole into the second frame: split the shadow piece and
	 * turn the tail into a plain VMA whose registry frames are
	 * erased (the punch route's end state).
	 */
	mmap_write_lock(mm);
	{
		VMA_ITERATOR(vmi, mm, hole);

		ret = __split_vma(&vmi, vma, hole, /* new_below = */ 0);
		KUNIT_ASSERT_EQ(test, ret, 0);
		tail = vma_lookup(mm, hole);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tail);
		vm_flags_clear(tail, VM_CORTEN | VM_NOHUGEPAGE);
		head = vma_lookup(mm, CORTEN_ARENA_TEST_BASE);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, head);
		KUNIT_ASSERT_TRUE(test, head->vm_flags & VM_CORTEN);

		state = corten_arena_state(mm);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);
		mutex_lock(&state->ctl_lock);
		xa_erase(&state->arenas, hole >> PMD_SHIFT);
		mutex_unlock(&state->ctl_lock);
	}
	mmap_write_unlock(mm);

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);

	/* The child inherits both pieces with their post-punch flags. */
	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_BASE, hole,
				      head->vm_flags);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	cvma = corten_arena_test_mkvm(child, hole,
				      CORTEN_ARENA_TEST_BASE +
				      CORTEN_ARENA_TEST_FORK_LEN,
				      tail->vm_flags);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);

	skips_before = corten_arena_test_fork_skips();
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);

	/* One surviving shadow piece is enough: the arena registers in
	 * the child (it was NOT skipped), and the hole's window has no
	 * metadata to mirror (the child never got a PT page for it: the
	 * hole is copied as a plain VMA by the standard loop).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_skips(),
			skips_before);
	KUNIT_EXPECT_EQ(test, corten_arena_query(child,
						 CORTEN_ARENA_TEST_BASE), 1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(child,
					       CORTEN_ARENA_TEST_BASE +
					       PAGE_SIZE, &m), -ENOENT);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(child, hole, &m), -ENOENT);

	/* The child descriptor's cached piece is the surviving head (the
	 * hole cannot be the cache: vma_lookup on its start finds a
	 * non-VM_CORTEN VMA).
	 */
	{
		struct corten_arena *car;

		rcu_read_lock();
		car = xa_load(&corten_arena_state(child)->arenas,
			      CORTEN_ARENA_TEST_BASE >> PMD_SHIFT);
		if (car)
			KUNIT_EXPECT_PTR_EQ(test, READ_ONCE(car->vma),
					    vma_lookup(child,
						       CORTEN_ARENA_TEST_BASE));
		rcu_read_unlock();
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, car);
	}

	mmput(child);

	/* Now the skip shape (④-1): the child inherits NO shadow piece --
	 * the MADV_DONTFORK/VM_DONTCOPY shape (E3, simulated by handing
	 * the child plain pieces).  The parent arena stays fully
	 * registered (its frames untouched), counted as a skip, and the
	 * child gets no registry entry.
	 */
	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_BASE, hole,
				      head->vm_flags & ~(VM_CORTEN |
							 VM_NOHUGEPAGE));
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	cvma = corten_arena_test_mkvm(child, hole,
				      CORTEN_ARENA_TEST_BASE +
				      CORTEN_ARENA_TEST_FORK_LEN,
				      tail->vm_flags);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_skips(),
			skips_before + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_query(child,
						 CORTEN_ARENA_TEST_BASE), 0);
	/* The parent side is untouched: still registered, still serving. */
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm,
						 CORTEN_ARENA_TEST_BASE), 1);
	mmput(child);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
}

/* OQ-D closure (M5_FORK_SPEC.md sec 0): the routed mprotect() commits
 * live in the metadata, and the faithful fork mirrors the metadata --
 * so the child sees the committed contract without any VMA surgery.
 * The T0 demote re-expressed the commits as split plain VMAs; the M5
 * fork leaves the shadow-VMA one piece, bounds intact.
 */
static void corten_arena_test_fork_perm(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm, *child;
	struct vm_area_struct *vma, *cvma;
	struct corten_arena *ar, *par;
	struct corten_pte_meta m;
	unsigned long commit;
	long timeouts_before = corten_arena_test_drain_timeouts();

	if (!corten_enabled_static())
		kunit_skip(test, "mprotect route requires corten=on");

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t);
	kunit_add_action(test, corten_arena_test_mm_destroy, t);
	t->test = test;
	t->mm = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->mm);
	mm = t->mm;

	/* The reservation: PROT_NONE-shaped, the JVM/glibc heap shape. */
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

	/* Commit pages 1..2 (inclusive) through the route. */
	commit = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	KUNIT_ASSERT_EQ(test,
			corten_arena_mprotect_route(mm, commit,
						    2 * PAGE_SIZE,
						    PROT_READ | PROT_WRITE,
						    -1),
			1);

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_BASE,
				      CORTEN_ARENA_TEST_BASE +
				      CORTEN_ARENA_TEST_LEN,
				      (CORTEN_ARENA_TEST_FLAGS_OK &
				       ~(VM_READ | VM_WRITE)) |
				      VM_CORTEN | VM_NOHUGEPAGE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	/* The mprotect route fill_upper'ed the first window on the
	 * parent, so copy_page_range() gives the child a PT page for it
	 * (the pending-perm metadata carrier) even though no PTE is
	 * copied.
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_fork_ensure_pt(test, child,
							 CORTEN_ARENA_TEST_BASE),
			0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);

	/* The parent: still ONE shadow-VMA over the whole reservation --
	 * no demote-style split, the routed commits stay in the
	 * metadata (the source of truth).
	 */
	vma = vma_lookup(mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_EXPECT_EQ(test, vma->vm_start, CORTEN_ARENA_TEST_BASE);
	KUNIT_EXPECT_EQ(test, vma->vm_end,
			CORTEN_ARENA_TEST_BASE + CORTEN_ARENA_TEST_LEN);
	KUNIT_EXPECT_TRUE(test, vma->vm_flags & VM_CORTEN);
	KUNIT_EXPECT_FALSE(test, vma->vm_flags & VM_READ);
	KUNIT_EXPECT_FALSE(test, vma->vm_flags & VM_WRITE);

	/* The committed chunk is recorded on BOTH sides identically. */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, commit, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_PRIVATE_ANON);
	KUNIT_EXPECT_EQ(test, m.perm,
			CORTEN_PERM_READ | CORTEN_PERM_WRITE |
			CORTEN_PERM_USER);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, commit, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_PRIVATE_ANON);
	KUNIT_EXPECT_EQ(test, m.perm,
			CORTEN_PERM_READ | CORTEN_PERM_WRITE |
			CORTEN_PERM_USER);

	/* Outside the commit: the reservation bound applies on both
	 * sides (INVALID slots -- the FRESH gate derives ar->prot).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm,
						     CORTEN_ARENA_TEST_BASE,
						     &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child,
						     CORTEN_ARENA_TEST_BASE,
						     &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);

	/* The arena upper bounds mirror. */
	rcu_read_lock();
	ar = xa_load(&corten_arena_state(mm)->arenas,
		     CORTEN_ARENA_TEST_BASE >> PMD_SHIFT);
	par = xa_load(&corten_arena_state(child)->arenas,
		      CORTEN_ARENA_TEST_BASE >> PMD_SHIFT);
	if (ar && par)
		KUNIT_EXPECT_EQ(test, READ_ONCE(par->prot),
				READ_ONCE(ar->prot));
	rcu_read_unlock();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ar);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, par);

	mmput(child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(),
			timeouts_before);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
}

/* ------------------------------------------------------------------ *
 * M5.T1b/T2' (M5_FORK_SPEC.md sec 2.2/sec 4.2): the fork closure
 * tests -- the INV7 shared-vs-PTE checker, the F2 over-SHARED gate,
 * the F3 drain-timeout injection, the OQ-4 UNSHARE mapping and the
 * OQ-5 exclusive-reuse anchor.
 * ------------------------------------------------------------------
 */

/*
 * INV7 (M5_FORK_SPEC.md sec 8) checker body: every translated arena page
 * whose metadata carries SHARED must sit read-only in hardware.  The
 * registered exemption is exactly the checked shape's mirror: a shared
 * page whose perm carries WRITE is ALLOWED to sit read-only (the fork's
 * wrprotect, restored only inside the COW transaction); what the
 * invariant forbids is SHARED together with a writable PTE -- the
 * cross-process write-through.  Walks every arena of @mm through the
 * registry xarray with the fork-mirror's pmd presence gate, one covering
 * transaction per window, and returns the number of violations found
 * (of @checked pages that actually carried SHARED).  No KUnit asserts
 * inside: the walk holds an RCU read lock, and the assert macros return.
 */
static void corten_arena_test_inv7_walk(struct mm_struct *mm, long *violated,
					long *checked)
{
	struct corten_mm_state *state = corten_arena_state(mm);
	struct corten_arena *arena;
	unsigned long frame = 0, seen_until = 0;

	*violated = 0;
	*checked = 0;
	if (!state)
		return;

	rcu_read_lock();
	xa_for_each(&state->arenas, frame, arena) {
		unsigned long addr;

		/* M4.T1: reserve markers are not arenas. */
		if (arena == &corten_va_reserve_sentinel)
			continue;
		if (frame < seen_until)
			continue;
		seen_until = arena->end >> PMD_SHIFT;

		for (addr = arena->start; addr < arena->end;
		     addr = min((addr | (PMD_SIZE - 1)) + 1, arena->end)) {
			unsigned long win_end = min((addr | (PMD_SIZE - 1)) + 1,
						    arena->end);
			pmd_t *pmdp = corten_arena_test_pmd(mm, addr);
			struct corten_txn txn;
			unsigned long a;
			int ret;

			if (!pmdp || !pmd_present(READ_ONCE(*pmdp)) ||
			    pmd_leaf(READ_ONCE(*pmdp)))
				continue;
			ret = corten_lock_range(mm, addr, win_end - addr,
						&txn);
			if (ret == -ENOENT || ret == -EOPNOTSUPP)
				continue;	/* untracked: nothing live */
			if (ret)
				break;		/* counted as not-checked */

			for (a = addr; a < win_end; a += PAGE_SIZE) {
				struct corten_pte_meta m;
				pte_t *ptep, pte;
				spinlock_t *ptl;	/* guards the PTE read */

				if (corten_query(&txn, a, &m))
					continue;
				if (m.state != CORTEN_MAPPED &&
				    m.state != CORTEN_SWAPPED)
					continue;
				if (m.state == CORTEN_MAPPED &&
				    !(m.flags & CORTEN_PF_SHARED))
					continue;
				ptep = pte_offset_map_lock(mm, pmdp, a, &ptl);
				if (!ptep)
					break;
				pte = ptep_get(ptep);
				pte_unmap_unlock(ptep, ptl);
				(*checked)++;
				if (m.state == CORTEN_MAPPED) {
					/* The fork shape: shared page read-
					 * only in hardware.
					 */
					if (pte_present(pte) && pte_write(pte))
						(*violated)++;
				} else {
					/* M6.T2 (spec D6) swapped half:
					 * CORTEN_SWAPPED <=> the PTE is a
					 * non-present, non-none swap entry
					 * holding exactly the __resv
					 * encoding; a none PTE behind a
					 * Swapped slot is the lost-content
					 * shape.
					 */
					if (pte_none(pte) || pte_present(pte) ||
					    pte_to_swp_entry(pte).val !=
					    corten_swap_decode(&m).val)
						(*violated)++;
				}
			}
			corten_unlock(&txn);
		}
	}
	rcu_read_unlock();
}

/* INV7 closure: after the faithful fork both sides hold SHARED marks and
 * read-only PTEs -- the exemption shape.  A COW write on one side clears
 * its SHARED and re-arms the write bit; the peer's contract stays.
 */
static void corten_arena_test_inv7_shared_ro(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm, *child;
	struct vm_area_struct *pvma, *cvma;
	struct folio *folio;
	struct corten_pte_meta m;
	pte_t *ptep, pte;
	unsigned long addr = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	long violated, checked;

	if (!corten_enabled_static())
		kunit_skip(test, "metadata mirror requires corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr),
			0);

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_BASE,
				      CORTEN_ARENA_TEST_BASE +
				      CORTEN_ARENA_TEST_LEN,
				      CORTEN_ARENA_TEST_FLAGS_OK |
				      VM_CORTEN | VM_NOHUGEPAGE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	folio = corten_arena_test_fork_copy_pte(test, child, mm, addr);
	KUNIT_EXPECT_NOT_NULL(test, folio);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);

	/* Both sides: SHARED meta, read-only PTE -- zero violations, and
	 * the walk must actually have seen the shared pages.
	 */
	corten_arena_test_inv7_walk(mm, &violated, &checked);
	KUNIT_EXPECT_EQ(test, violated, 0);
	KUNIT_EXPECT_GE(test, checked, 1);
	corten_arena_test_inv7_walk(child, &violated, &checked);
	KUNIT_EXPECT_EQ(test, violated, 0);
	KUNIT_EXPECT_GE(test, checked, 1);

	/* The exemption row, explicit: perm carries WRITE, hardware is
	 * read-only (the fork's wrprotect), metadata flags carry
	 * SHARED|WRITABLE.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.flags,
			CORTEN_PF_SHARED | CORTEN_PF_WRITABLE);
	ptep = corten_arena_test_pmd(mm, addr) ?
			pte_offset_map(corten_arena_test_pmd(mm, addr),
				       addr) : NULL;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));
	KUNIT_EXPECT_FALSE(test, pte_write(pte));

	/* The parent COWs (the peer still maps: copy branch).  The
	 * parent's SHARED clears and its PTE re-arms; the child keeps
	 * both shapes.
	 */
	pvma = vma_lookup(mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pvma);
	KUNIT_EXPECT_EQ(test,
			corten_arena_handle_mm_fault(pvma, addr,
						     FAULT_FLAG_WRITE, NULL),
			0);
	/* The peer still maps (mapcount 2): the copy branch, whose
	 * corten_map() resets the flags -- SHARED gone with the shared
	 * folio, and the WRITABLE record only has meaning alongside it.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.flags, 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.flags,
			CORTEN_PF_SHARED | CORTEN_PF_WRITABLE);
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 1);

	corten_arena_test_inv7_walk(mm, &violated, &checked);
	KUNIT_EXPECT_EQ(test, violated, 0);
	corten_arena_test_inv7_walk(child, &violated, &checked);
	KUNIT_EXPECT_EQ(test, violated, 0);
	KUNIT_EXPECT_GE(test, checked, 1);

	mmput(child);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
}

/* M6.T2 (spec D6): the swapped half of the INV7 checker.  Seed a
 * CORTEN_SWAPPED slot with a synthetic entry (bits only -- the case
 * never dereferences a swap device, so it runs on swap-less boots),
 * then verify both directions: the intact shape passes, a none PTE
 * behind the Swapped slot is flagged.
 */
static void corten_arena_test_inv7_swapped(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned long addr = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	struct folio *folio;
	struct corten_txn txn;
	struct corten_pte_meta sm, m;
	pte_t *ptep, pte, swp_pte;
	spinlock_t *ptl;
	pmd_t *pmdp;
	swp_entry_t entry = swp_entry(1, 0x77);
	long violated, checked;

	if (!corten_enabled_static())
		kunit_skip(test, "metadata mirror requires corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;
	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr),
			0);

	/* Hand-rolled swap-out shape: PTE -> synthetic swap entry, the
	 * metadata -> CORTEN_SWAPPED with the encoding; the resident
	 * folio reference goes back (the shape no longer holds it).
	 */
	vma = vma_lookup(mm, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	pmdp = corten_arena_test_pmd(mm, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get_and_clear(mm, addr, ptep);
	KUNIT_ASSERT_TRUE(test, pte_present(pte));
	swp_pte = swp_entry_to_pte(entry);
	set_pte_at(mm, addr, ptep, swp_pte);
	pte_unmap_unlock(ptep, ptl);
	folio = page_folio(pte_page(pte));
	folio_remove_rmap_pte(folio, folio_page(folio, 0), vma);
	add_mm_counter(mm, MM_ANONPAGES, -1);
	folio_put(folio);

	/* Inside the transaction only EXPECT: an ASSERT abort would
	 * return with the desc write lock held and self-deadlock the
	 * mm teardown (the lesson this case taught the hard way).
	 */
	KUNIT_ASSERT_EQ(test, corten_lock_range(mm, addr, PAGE_SIZE, &txn),
			0);
	memset(&sm, 0, sizeof(sm));
	sm.state = CORTEN_SWAPPED;
	sm.perm = CORTEN_PERM_READ | CORTEN_PERM_WRITE | CORTEN_PERM_USER;
	corten_swap_encode(&sm, entry);
	KUNIT_EXPECT_EQ(test, corten_swap_out(&txn, addr, &sm), 0);
	corten_unlock(&txn);

	/* Intact shape: no violation, the walker saw the slot. */
	corten_arena_test_inv7_walk(mm, &violated, &checked);
	KUNIT_EXPECT_EQ(test, violated, 0);
	KUNIT_EXPECT_GE(test, checked, 1);

	/* Break it (the lost-content shape: none PTE, Swapped slot) --
	 * the walker must flag exactly one violation.
	 */
	pmdp = corten_arena_test_pmd(mm, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	ptep_get_and_clear(mm, addr, ptep);
	pte_unmap_unlock(ptep, ptl);

	corten_arena_test_inv7_walk(mm, &violated, &checked);
	KUNIT_EXPECT_EQ(test, violated, 1);
	KUNIT_EXPECT_GE(test, checked, 1);

	/* Cleanup: scrub the slot (no zap: the synthetic entry must not
	 * reach free_swap_and_cache()).
	 */
	KUNIT_ASSERT_EQ(test, corten_lock_range(mm, addr, PAGE_SIZE, &txn),
			0);
	KUNIT_EXPECT_EQ(test, corten_unmap(&txn, addr, PAGE_SIZE,
					   CORTEN_UNMAP_KEEP_PERM), 0);
	corten_unlock(&txn);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
}

/* F2 (STATE D18, the over-SHARED corner): a window the child never got a
 * PT page for -- a VM_WIPEONFORK piece (VMA inherited, copy skipped) or a
 * VM_DONTCOPY piece (VMA absent) -- has no second mapper.  The fork
 * mirror must NOT mark those pages SHARED on the parent: the parent PTE
 * stays writable (copy_page_range never wrprotected the piece), so a
 * SHARED mark there is precisely the INV7-drift shape the checker hunts.
 */
static void corten_arena_test_fork_f2_gate(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm, *child;
	struct vm_area_struct *cvma;
	struct corten_pte_meta m;
	unsigned long addr0 = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	unsigned long addr1 = CORTEN_ARENA_TEST_BASE + PMD_SIZE + PAGE_SIZE;
	long skips_before, violated, checked;

	if (!corten_enabled_static())
		kunit_skip(test, "metadata mirror requires corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr0), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr1), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr0),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr1),
			0);

	/* Shape A: the WIPEONFORK piece -- the child's shadow-VMA covers
	 * the whole arena, but only window 1 receives the copy (window
	 * 0's translations were wiped: no child PT page there).
	 */
	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_BASE,
				      CORTEN_ARENA_TEST_BASE +
				      CORTEN_ARENA_TEST_LEN,
				      CORTEN_ARENA_TEST_FLAGS_OK |
				      VM_CORTEN | VM_NOHUGEPAGE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	KUNIT_EXPECT_NOT_NULL(test,
			      corten_arena_test_fork_copy_pte(test, child, mm,
							      addr1));
	skips_before = corten_arena_test_fork_skips();
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_skips(), skips_before);

	/* Window 0: no SHARED mark (the F2 gate), writable parent PTE,
	 * unrecorded child slot.  Window 1: the normal mirror.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr0, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, m.flags, 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, addr0, &m),
			-ENOENT);
	{
		pte_t *ptep = corten_arena_test_pmd(mm, addr0) ?
			pte_offset_map(corten_arena_test_pmd(mm, addr0),
				       addr0) : NULL;

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
		KUNIT_EXPECT_TRUE(test, pte_write(ptep_get(ptep)));
		pte_unmap(ptep);
	}
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr1, &m), 0);
	KUNIT_EXPECT_EQ(test, m.flags,
			CORTEN_PF_SHARED | CORTEN_PF_WRITABLE);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, addr1, &m), 0);
	KUNIT_EXPECT_EQ(test, m.flags,
			CORTEN_PF_SHARED | CORTEN_PF_WRITABLE);

	/* The checker agrees: no shared page sits writable anywhere. */
	corten_arena_test_inv7_walk(mm, &violated, &checked);
	KUNIT_EXPECT_EQ(test, violated, 0);
	corten_arena_test_inv7_walk(child, &violated, &checked);
	KUNIT_EXPECT_EQ(test, violated, 0);

	mmput(child);

	/* Shape B: the DONTCOPY piece -- window 0 has no VMA at all in
	 * the child (the dup_mmap loop skipped it); window 1's shadow
	 * piece keeps the arena registered (no skip).
	 */
	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	cvma = corten_arena_test_mkvm(child,
				      CORTEN_ARENA_TEST_BASE + PMD_SIZE,
				      CORTEN_ARENA_TEST_BASE +
				      CORTEN_ARENA_TEST_LEN,
				      CORTEN_ARENA_TEST_FLAGS_OK |
				      VM_CORTEN | VM_NOHUGEPAGE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	KUNIT_EXPECT_NOT_NULL(test,
			      corten_arena_test_fork_copy_pte(test, child, mm,
							      addr1));
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_skips(), skips_before);

	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr0, &m), 0);
	KUNIT_EXPECT_EQ(test, m.flags, 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, addr0, &m),
			-ENOENT);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr1, &m), 0);
	KUNIT_EXPECT_EQ(test, m.flags,
			CORTEN_PF_SHARED | CORTEN_PF_WRITABLE);

	corten_arena_test_inv7_walk(mm, &violated, &checked);
	KUNIT_EXPECT_EQ(test, violated, 0);
	corten_arena_test_inv7_walk(child, &violated, &checked);
	KUNIT_EXPECT_EQ(test, violated, 0);

	mmput(child);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
}

/* F3 (STATE D18, M5_FORK_SPEC.md sec 2.3 E5): a transaction reference
 * alive across the freeze window makes fork_begin()'s drain time out.
 * The contract is "continue with the leak": the fork proceeds, the
 * timeout is counted, the arena stays frozen until the commit's unfreeze
 * closure re-arms the refcount -- and the straggler's put must land
 * safely on the resurrected ref.
 */
static void corten_arena_test_fork_drain_leak(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm, *child;
	struct vm_area_struct *cvma;
	struct corten_arena *ar;
	struct corten_pte_meta m;
	unsigned long addr = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	long timeouts_before, faithful_before;

	if (!corten_enabled_static())
		kunit_skip(test, "metadata mirror requires corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr),
			0);

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	timeouts_before = corten_arena_test_drain_timeouts();
	faithful_before = corten_arena_test_fork_faithful_count();

	/* The leaked reference: one transaction liveness ref held across
	 * the freeze (what a straggler transaction holds).  lookup_get
	 * must run before fork_begin() -- frozen lookups fail.
	 */
	ar = corten_arena_lookup_get(mm, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ar);

	/* The drain times out (10*HZ) and is counted; the fork continues
	 * with the leak and the arena stays frozen.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(),
			timeouts_before + 1);
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_arena_frozen(mm,
							 CORTEN_ARENA_TEST_BASE));

	/* The straggler releases onto the killed ref: the put lands,
	 * the completion fires, and the commit below re-arms the ref.
	 */
	percpu_ref_put(&ar->active);

	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_BASE,
				      CORTEN_ARENA_TEST_BASE +
				      CORTEN_ARENA_TEST_LEN,
				      CORTEN_ARENA_TEST_FLAGS_OK |
				      VM_CORTEN | VM_NOHUGEPAGE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	KUNIT_EXPECT_NOT_NULL(test,
			      corten_arena_test_fork_copy_pte(test, child, mm,
							      addr));
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);

	/* The unfreeze closure ran and the fork counted as faithful. */
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_test_arena_frozen(mm,
							  CORTEN_ARENA_TEST_BASE));
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_faithful_count(),
			faithful_before + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.flags,
			CORTEN_PF_SHARED | CORTEN_PF_WRITABLE);

	mmput(child);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
}

/* OQ-4 (M5_FORK_SPEC.md sec 4.2): GUP's read-PIN of a fork-shared page
 * (gup_must_unshare() -> -EMLINK) faults in with FAULT_FLAG_UNSHARE and
 * no write bit; the slow gate maps it onto the COW write dispatch.  The
 * pinned side must come out on its own private copy, never spinning
 * against a re-armed read-only translation.
 */
static void corten_arena_test_unshare_pin(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm, *child;
	struct vm_area_struct *pvma, *cvma;
	struct folio *folio;
	struct page *newpage;
	struct corten_pte_meta m;
	pte_t *ptep, pte;
	unsigned long addr = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	unsigned long pfn;

	if (!corten_enabled_static())
		kunit_skip(test, "metadata mirror requires corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr),
			0);

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_BASE,
				      CORTEN_ARENA_TEST_BASE +
				      CORTEN_ARENA_TEST_LEN,
				      CORTEN_ARENA_TEST_FLAGS_OK |
				      VM_CORTEN | VM_NOHUGEPAGE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	folio = corten_arena_test_fork_copy_pte(test, child, mm, addr);
	KUNIT_EXPECT_NOT_NULL(test, folio);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);

	/* The fork-shared shape: mapcount 2, exclusive cleared on both
	 * sides (folio_try_dup_anon_rmap_pte), SHARED marks mirrored.
	 */
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 2);
	KUNIT_EXPECT_FALSE(test,
			   PageAnonExclusive(folio_page(folio, 0)));

	pvma = vma_lookup(mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pvma);
	KUNIT_EXPECT_EQ(test,
			corten_arena_handle_mm_fault(pvma, addr,
						     FAULT_FLAG_UNSHARE, NULL),
			0);

	/* The parent pinned side now owns a private, exclusive, writable
	 * copy (the contract perm carries WRITE); the child keeps the
	 * shared original read-only, and the folio's remaining mapper is
	 * the child alone.
	 */
	ptep = corten_arena_test_pmd(mm, addr) ?
			pte_offset_map(corten_arena_test_pmd(mm, addr),
				       addr) : NULL;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte) && pte_write(pte));
	newpage = pte_page(pte);
	KUNIT_EXPECT_TRUE(test, newpage != folio_page(folio, 0));
	KUNIT_EXPECT_TRUE(test, PageAnonExclusive(newpage));
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 1);

	/* The copy branch's corten_map() reset the flags. */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.flags, 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.flags,
			CORTEN_PF_SHARED | CORTEN_PF_WRITABLE);

	ptep = corten_arena_test_pmd(child, addr) ?
			pte_offset_map(corten_arena_test_pmd(child, addr),
				       addr) : NULL;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));
	KUNIT_EXPECT_FALSE(test, pte_write(pte));
	pfn = pte_pfn(pte);
	KUNIT_EXPECT_EQ(test, pfn, page_to_pfn(folio_page(folio, 0)));

	mmput(child);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
}

/* OQ-5 (M5_FORK_SPEC.md sec 8), the reuse leg: after the peer leaves
 * (fork + child exit), the survivor's first write takes the map_count==1
 * branch -- and marks the page exclusive, exactly like do_wp_page()'s
 * reuse path in front of wp_page_reuse().  Without the mark a later GUP
 * PIN of the survivor would trip gup.c's PIN && !PageAnonExclusive WARN.
 */
static void corten_arena_test_fork_reuse_exclusive(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm, *child;
	struct vm_area_struct *pvma, *cvma;
	struct folio *folio;
	struct corten_pte_meta m;
	pte_t *ptep, pte;
	unsigned long addr = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	unsigned long pfn;

	if (!corten_enabled_static())
		kunit_skip(test, "metadata mirror requires corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr),
			0);

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_BASE,
				      CORTEN_ARENA_TEST_BASE +
				      CORTEN_ARENA_TEST_LEN,
				      CORTEN_ARENA_TEST_FLAGS_OK |
				      VM_CORTEN | VM_NOHUGEPAGE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	folio = corten_arena_test_fork_copy_pte(test, child, mm, addr);
	KUNIT_EXPECT_NOT_NULL(test, folio);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 2);

	/* The peer leaves: mapcount back to 1, the exclusive mark stays
	 * cleared (fork's dup), the SHARED mark is the residue the COW
	 * reuse branch exists to clear.
	 */
	mmput(child);
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 1);
	KUNIT_EXPECT_FALSE(test, PageAnonExclusive(folio_page(folio, 0)));

	pvma = vma_lookup(mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pvma);
	KUNIT_EXPECT_EQ(test,
			corten_arena_handle_mm_fault(pvma, addr,
						     FAULT_FLAG_WRITE, NULL),
			0);

	ptep = corten_arena_test_pmd(mm, addr) ?
			pte_offset_map(corten_arena_test_pmd(mm, addr),
				       addr) : NULL;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte) && pte_write(pte));
	pfn = pte_pfn(pte);
	KUNIT_EXPECT_EQ(test, pfn, page_to_pfn(folio_page(folio, 0)));
	KUNIT_EXPECT_TRUE(test, PageAnonExclusive(folio_page(folio, 0)));

	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.flags, CORTEN_PF_WRITABLE);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
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

/* ------------------------------------------------------------------ *
 * M5.T3: the GUP interop matrix (M5_FORK_SPEC.md sec 4).  KUnit cannot
 * drive the lockless gup_fast walk (it needs a remote-CPU TLB race
 * window), so these pin down the slow-gate and PTE-shape contract the
 * fast walk and its bail-to-slow paths are verified against in the
 * guest; gup.c itself is untouched by this slice (the r06 gupfix gate
 * deferral is the only arena-specific line it carries).
 * ------------------------------------------------------------------
 */

/* State 1 (SPEC sec 4.1 row 1): an uncommitted page of a PROT_NONE
 * reservation.  GUP FOLL_WRITE/FOLL_READ reach the arena slow gate via
 * faultin_page() (check_vma_flags() defers to metadata for VM_CORTEN,
 * the r06 gupfix), and the FRESH verdict for the never-routed slot is
 * the DECLARE bound (USER only) -- VM_FAULT_SIGSEGV, the ACCERR
 * -EFAULT observation a legacy PROT_NONE mapping gives the same GUP.
 */
static void corten_arena_test_gup_state1_uncommitted(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct corten_pte_meta m;
	unsigned long addr = CORTEN_ARENA_TEST_START2 + PAGE_SIZE;

	if (!corten_enabled_static())
		kunit_skip(test, "metadata mirror requires corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;

	/* The glibc new_heap() shape: PROT_NONE MAP_NORESERVE reserve.
	 * setup() already mapped the FLAGS_OK default VMA over BASE, so
	 * this shape lives in the spare START2 range (the
	 * protect_flags_kernel_gate convention) -- overlapping VMAs in
	 * the test mm's tree would corrupt its exit_mmap().
	 */
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test,
				     corten_arena_test_mkvm(mm,
							    CORTEN_ARENA_TEST_START2,
							    CORTEN_ARENA_TEST_START2 +
							    CORTEN_ARENA_TEST_LEN2,
							    CORTEN_ARENA_TEST_FLAGS_NONE));
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_START2,
					     CORTEN_ARENA_TEST_LEN2),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr), 0);

	vma = vma_lookup(mm, CORTEN_ARENA_TEST_START2);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);

	/* The GUP write (FAULT_FLAG_WRITE, no USER bit -- the
	 * faultin_page() shape) and the GUP read both die at the gate:
	 * nothing was ever committed, the contract says inaccessible.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_handle_mm_fault(vma, addr,
						     FAULT_FLAG_WRITE, NULL),
			VM_FAULT_SIGSEGV);
	KUNIT_EXPECT_EQ(test,
			corten_arena_handle_mm_fault(vma, addr, 0, NULL),
			VM_FAULT_SIGSEGV);

	/* Nothing was installed and nothing was recorded. */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);
}

/* State 2 (SPEC sec 4.1 row 2): a committed RW page.  The write fault
 * (the GUP slow path's faultin) hands back exactly the PTE the fast
 * walk needs: present, pte_write(), exclusive -- pte_access_permitted()
 * and try_grab_folio_fast() pass on it without arena awareness.
 */
static void corten_arena_test_gup_state2_committed_rw(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct folio *folio;
	struct corten_pte_meta m;
	pte_t *ptep, pte;
	unsigned long addr = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;

	if (!corten_enabled_static())
		kunit_skip(test, "metadata mirror requires corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;

	/* setup()'s default FLAGS_OK VMA over BASE is the arena. */
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr), 0);
	/* The commit route: metadata-only, shadow-VMA untouched (the
	 * gupfix invariant -- the gates defer to this record).
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_mark(mm, addr,
					       CORTEN_ARENA_TEST_PERM_RW),
			0);
	vma = vma_lookup(mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_EXPECT_TRUE(test, vma->vm_flags & VM_WRITE);

	KUNIT_EXPECT_EQ(test,
			corten_arena_handle_mm_fault(vma, addr,
						     FAULT_FLAG_WRITE, NULL),
			0);

	ptep = corten_arena_test_pmd(mm, addr) ?
			pte_offset_map(corten_arena_test_pmd(mm, addr),
				       addr) : NULL;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte) && pte_write(pte));
	folio = page_folio(pte_page(pte));
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 1);	/* PTE ref */
	KUNIT_EXPECT_TRUE(test, folio_test_anon(folio));
	KUNIT_EXPECT_TRUE(test, PageAnonExclusive(folio_page(folio, 0)));

	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_ARENA_TEST_PERM_RW);
}

/* State 3 (SPEC sec 4.1 row 3/sec 4.2): a fork-shared RO page plus
 * FOLL_WRITE.  gup_fast bails on the RO PTE, the slow path's
 * can_follow_write_pte() fails, faultin_page() faults WRITE, and the
 * arena COW copy branch hands GUP its own private exclusive copy --
 * bitwise the legacy faultin->do_wp_page(wp_page_copy) contract.  The
 * UNSHARE producer of the same branch is anchored by
 * corten_arena_test_unshare_pin(); this is the plain-write producer.
 */
static void corten_arena_test_gup_state3_fork_cow(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm, *child;
	struct vm_area_struct *pvma, *cvma;
	struct folio *folio;
	struct corten_pte_meta m;
	pte_t *ptep, pte;
	unsigned long addr = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	unsigned long old_pfn;

	if (!corten_enabled_static())
		kunit_skip(test, "metadata mirror requires corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr),
			0);

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_BASE,
				      CORTEN_ARENA_TEST_BASE +
				      CORTEN_ARENA_TEST_LEN,
				      CORTEN_ARENA_TEST_FLAGS_OK |
				      VM_CORTEN | VM_NOHUGEPAGE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	folio = corten_arena_test_fork_copy_pte(test, child, mm, addr);
	KUNIT_EXPECT_NOT_NULL(test, folio);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 2);
	old_pfn = page_to_pfn(folio_page(folio, 0));

	pvma = vma_lookup(mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pvma);
	KUNIT_EXPECT_EQ(test,
			corten_arena_handle_mm_fault(pvma, addr,
						     FAULT_FLAG_WRITE, NULL),
			0);

	/* The writer left the shared folio behind: private, exclusive,
	 * writable -- GUP's retry pins this new PTE.
	 */
	ptep = corten_arena_test_pmd(mm, addr) ?
			pte_offset_map(corten_arena_test_pmd(mm, addr),
				       addr) : NULL;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte) && pte_write(pte));
	KUNIT_EXPECT_NE(test, pte_pfn(pte), old_pfn);
	KUNIT_EXPECT_TRUE(test, PageAnonExclusive(pte_page(pte)));

	/* The peer keeps the original read-only, alone. */
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 1);
	ptep = corten_arena_test_pmd(child, addr) ?
			pte_offset_map(corten_arena_test_pmd(child, addr),
				       addr) : NULL;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));
	KUNIT_EXPECT_FALSE(test, pte_write(pte));
	KUNIT_EXPECT_EQ(test, pte_pfn(pte), old_pfn);

	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.flags, 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.flags,
			CORTEN_PF_SHARED | CORTEN_PF_WRITABLE);

	mmput(child);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
}

/* State 3b (SPEC sec 4.1 row 2, the routed mprotect arm): a committed
 * page after an mprotect downgrade.  The recorded perm moves to RO, so
 * the FOLL_WRITE fault is loudly denied (ACCERR) -- the documented
 * semantic fork from legacy's silent wp_page_copy ("an external write
 * must not silently repeal the process contract", T2' residue 2).
 * After an upgrade route back to RW the shape is "perm W, PTE RO" and
 * the same fault self-heals through CORTEN_DISP_RESTORE (mkwrite,
 * same folio) -- the shape gup_fast's pte_access_permitted() bail and
 * slow-path retry are verified against in the guest.
 */
static void corten_arena_test_gup_state3b_downgrade_restore(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct folio *folio;
	struct corten_pte_meta m;
	pte_t *ptep, pte;
	unsigned long addr = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	unsigned long pfn;

	if (!corten_enabled_static())
		kunit_skip(test, "metadata mirror requires corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr),
			0);

	ptep = corten_arena_test_pmd(mm, addr) ?
			pte_offset_map(corten_arena_test_pmd(mm, addr),
				       addr) : NULL;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	folio = page_folio(pte_page(pte));
	pfn = page_to_pfn(folio_page(folio, 0));

	/* Downgrade: routed chunk to PROT_READ. */
	mmap_write_lock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mprotect_route(mm, CORTEN_ARENA_TEST_BASE,
						    2 * PMD_SIZE, PROT_READ,
						    -1), 1);
	mmap_write_unlock(mm);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_PERM_READ | CORTEN_PERM_USER);

	vma = vma_lookup(mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_EXPECT_EQ(test,
			corten_arena_handle_mm_fault(vma, addr,
						     FAULT_FLAG_WRITE, NULL),
			VM_FAULT_SIGSEGV);

	/* Upgrade back: perm returns to RW, the PTE stays read-only --
	 * the self-heal is the next write fault's job (RESTORE).
	 */
	mmap_write_lock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mprotect_route(mm, CORTEN_ARENA_TEST_BASE,
						    2 * PMD_SIZE,
						    PROT_READ | PROT_WRITE,
						    -1), 1);
	mmap_write_unlock(mm);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_ARENA_TEST_PERM_RW);

	KUNIT_EXPECT_EQ(test,
			corten_arena_handle_mm_fault(vma, addr,
						     FAULT_FLAG_WRITE, NULL),
			0);
	ptep = corten_arena_test_pmd(mm, addr) ?
			pte_offset_map(corten_arena_test_pmd(mm, addr),
				       addr) : NULL;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte) && pte_write(pte));
	/* No copy: the folio is private and survived the round trip. */
	KUNIT_EXPECT_EQ(test, pte_pfn(pte), pfn);
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 1);
	KUNIT_EXPECT_TRUE(test, PageAnonExclusive(folio_page(folio, 0)));

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
}

/* State 4 (SPEC sec 4.3): FOLL_PIN keep-alive across the arena zap.
 * A GUP pin is refcount-native (GUP_PIN_COUNTING_BIAS on an order-0
 * folio): the zap drops the PTE reference through the tlb batch and
 * the pin carries the folio until unpin -- no WARN, no leak, and the
 * unpin drops it to its holder count so the last put frees.  The
 * zap_pinned counter (M6 migration's skip evidence) moves by one.
 */
static void corten_arena_test_gup_state4_pin_zap(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm;
	struct folio *folio;
	struct corten_arena *ar;
	struct corten_pte_meta m;
	pte_t *ptep, pte;
	unsigned long addr = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	long zaps;

	if (!corten_enabled_static())
		kunit_skip(test, "metadata mirror requires corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;

	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr),
			0);

	ptep = corten_arena_test_pmd(mm, addr) ?
			pte_offset_map(corten_arena_test_pmd(mm, addr),
				       addr) : NULL;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	folio = page_folio(pte_page(pte));
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 1);

	/* The test's own hold + the simulated FOLL_PIN (the slow-path
	 * pin shape on an order-0 folio: bias worth of plain refs).
	 */
	folio_get(folio);
	folio_ref_add(folio, GUP_PIN_COUNTING_BIAS);
	KUNIT_EXPECT_TRUE(test, folio_maybe_dma_pinned(folio));

	zaps = corten_arena_test_named_counter(test, "zap_pinned");

	ar = corten_arena_lookup_get(mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ar);
	KUNIT_EXPECT_EQ(test,
			corten_arena_unmap_chunk(mm, ar, addr, PAGE_SIZE), 0);
	percpu_ref_put(&ar->active);

	/* The VA is unmapped (translation gone, slot INVALID) but the
	 * pinned folio is alive: PTE ref dropped, pin + holder remain.
	 */
	ptep = corten_arena_test_pmd(mm, addr) ?
			pte_offset_map(corten_arena_test_pmd(mm, addr),
				       addr) : NULL;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap(ptep);
	KUNIT_EXPECT_TRUE(test, pte_none(pte));
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio),
			GUP_PIN_COUNTING_BIAS + 1);
	KUNIT_EXPECT_TRUE(test, folio_maybe_dma_pinned(folio));

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_named_counter(test, "zap_pinned"),
			zaps + 1);

	/* Unpin: the folio is back to the holder reference only, and
	 * that single reference is what the last put frees (the
	 * refcount reaching zero here is what no-WARN looks like).
	 * The zap dropped the PTE reference, so exactly ONE put
	 * remains -- a second would over-put a freed folio (the
	 * r07 M5.T3 follow-up).
	 */
	folio_ref_sub(folio, GUP_PIN_COUNTING_BIAS);
	KUNIT_EXPECT_FALSE(test, folio_maybe_dma_pinned(folio));
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 1);
	folio_put(folio);
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

/*
 * r06 gupfix invariant pin: a routed chunk mprotect must NOT touch the
 * shadow-VMA R/W/X flags -- they stay at the DECLARE bound, because the
 * fork demotion's materialize walk reads them as the unrecorded-page
 * baseline ("already encoded" fast path) and any routed bit would leak
 * the commit into every never-routed page of the demoted VMA.  The
 * kernel-side gates (GUP check_vma_flags, arch access_error) defer
 * shadow-VMA verdicts to the arena metadata instead.
 */
static void corten_arena_test_protect_flags_kernel_gate(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct vm_area_struct *vma;
	struct corten_arena *ar;
	u8 prot;

	if (!corten_enabled_static())
		kunit_skip(test, "mprotect route requires corten=on");

	/* The glibc thread-arena shape: a PROT_NONE MAP_NORESERVE reserve
	 * (DECLARE reads perm=USER from it), committed sub-ranges arrive
	 * as routed mprotect() calls.
	 */
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test,
				     corten_arena_test_mkvm(mm,
							    CORTEN_ARENA_TEST_START2,
							    CORTEN_ARENA_TEST_START2 +
							    CORTEN_ARENA_TEST_LEN2,
							    CORTEN_ARENA_TEST_FLAGS_NONE));
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_START2,
					     CORTEN_ARENA_TEST_LEN2), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* Chunk route (the first PMD only) up to RW: recorded perm lands
	 * in the metadata, the shadow-VMA flags stay at the DECLARE
	 * bound (the demote baseline -- see the invariant comment).
	 */
	mmap_write_lock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mprotect_route(mm,
						    CORTEN_ARENA_TEST_START2,
						    PMD_SIZE,
						    PROT_READ | PROT_WRITE,
						    -1), 1);
	mmap_write_unlock(mm);

	vma = vma_lookup(mm, CORTEN_ARENA_TEST_START2);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_EXPECT_FALSE(test, vma->vm_flags &
			   (VM_READ | VM_WRITE | VM_EXEC));

	/* The FRESH upper bound does not move with a chunk route:
	 * unrouted pages keep the DECLARE (PROT_NONE) verdict.
	 */
	rcu_read_lock();
	ar = corten_arena_lookup(mm, CORTEN_ARENA_TEST_START2);
	prot = ar ? READ_ONCE(ar->prot) : CORTEN_PERM_ALL;
	rcu_read_unlock();
	KUNIT_EXPECT_EQ(test, prot, CORTEN_PERM_USER);
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

/* ------------------------------------------------------------------
 * T1c: the resident arena pool.  Park on a full-coverage munmap,
 * same-window reactivation on the next auto mmap, the bounded pool's
 * LRU-overflow release, the MODE-exit teardown and the fork-begin
 * flush.
 * ------------------------------------------------------------------
 */

/* Attach one auto arena at [addr, addr+len): the harness VMA is the
 * takeover product, the attach is the do_mmap tail hook's body.
 */
static int corten_arena_test_pool_attach(struct mm_struct *mm,
					 unsigned long addr, unsigned long len)
{
	int ret;

	mmap_write_lock(mm);
	ret = corten_arena_auto_attach(mm, addr, len);
	mmap_write_unlock(mm);

	return ret;
}

/* The pool-flush fork window: the flush's release legs run do_munmap(),
 * which reads current->mm (vms_complete_munmap_vmas), so -- like the
 * release cases -- the window runs on an attached worker.  Production
 * dup_mmap() always has current->mm == oldmm; use_mm() reproduces that.
 */
struct corten_arena_test_pool_fork {
	struct mm_struct *parent;
	struct mm_struct *child;
	int begin_ret;
	int commit_ret;
	struct completion done;
};

static int corten_arena_test_pool_fork_thread(void *data)
{
	struct corten_arena_test_pool_fork *o = data;

	kthread_use_mm(o->parent);
	o->begin_ret = corten_arena_test_fork_begin(o->child, o->parent);
	o->commit_ret = corten_arena_test_fork_commit(o->child, o->parent);
	kthread_unuse_mm(o->parent);
	complete(&o->done);

	return 0;
}

static void corten_arena_test_pool_reuse(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct vm_area_struct *vma;
	struct corten_pte_meta m;
	unsigned long addr, lenp, flags;
	long parks = corten_arena_test_pool_parks();
	long hits = corten_arena_test_pool_hits();
	long misses = corten_arena_test_pool_misses();
	long timeouts = corten_arena_test_drain_timeouts();
	int ret;

	if (!corten_enabled_static())
		kunit_skip(test, "pool routing requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* One glibc-shaped 2M NORESERVE anonymous arena at the window
	 * base, with one committed page (real folio + CORTEN_MAPPED).
	 */
	vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_WIN,
				     CORTEN_ARENA_TEST_WIN + PMD_SIZE,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_ASSERT_EQ(test, corten_arena_test_pool_attach(mm,
							    CORTEN_ARENA_TEST_WIN,
							    PMD_SIZE), 0);
	/* seed_anon first: fill_upper() creates the tracked PT page the
	 * mapped seeding installs into (a fresh arena has none).
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_fork_seed_anon(mm,
							 CORTEN_ARENA_TEST_WIN),
			0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_fork_seed_mapped(mm,
							   CORTEN_ARENA_TEST_WIN),
			0);

	/* The free() shape: a full-coverage munmap parks the arena. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);

	/* Parked: lookup-invisible, reservation retained, pool occupancy
	 * 1 -- and zero drain activity, the percpu_ref never died.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_WIN),
			0);
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_pool_idle(mm,
						      CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_parks(), parks + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 1);
	KUNIT_EXPECT_NOT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);

	/* The park zap reset the committed page: perm-0 Invalid -- the
	 * munmap killed the mprotect contract with the mapping.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_WIN,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);
	KUNIT_EXPECT_EQ(test, m.perm, 0);

	/* The next auto mmap of the same size is served from the pool:
	 * ret 2 -- the re-warmed reservation IS the mapping, live before
	 * any do_mmap flow runs.
	 */
	addr = 0;
	lenp = PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	mmap_write_lock(mm);
	ret = corten_arena_auto_mmap_route(mm, PAGE_SIZE,
					   PROT_READ | PROT_WRITE, &addr,
					   &lenp, &flags);
	mmap_write_unlock(mm);
	KUNIT_EXPECT_EQ(test, ret, 2);
	KUNIT_EXPECT_EQ(test, addr, CORTEN_ARENA_TEST_WIN);
	KUNIT_EXPECT_EQ(test, lenp, PMD_SIZE);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_hits(), hits + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 0);
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_test_pool_idle(mm,
						       CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_WIN),
			1);
	/* The reused mapping carries the requested protection and the
	 * shadow identity again (park had stripped all three).
	 */
	vma = vma_lookup(mm, CORTEN_ARENA_TEST_WIN);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_EXPECT_TRUE(test, !!(vma->vm_flags & VM_CORTEN));
	KUNIT_EXPECT_TRUE(test, !!(vma->vm_flags & (VM_READ | VM_WRITE)));
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);

	/* Park it again and re-serve through the DECLARE-side probe: the
	 * safety net that also covers a prctl DECLARE over a parked
	 * range (the attach drives corten_arena_declare_locked()).
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_attach(mm,
							    CORTEN_ARENA_TEST_WIN,
							    PMD_SIZE), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_hits(), hits + 2);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);

	/* A size no slot parked is a counted miss; the magazine places
	 * fresh window past the (live) pool-hit arena.
	 */
	addr = 0;
	lenp = PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	mmap_write_lock(mm);
	ret = corten_arena_auto_mmap_route(mm, 2 * PMD_SIZE,
					   PROT_READ | PROT_WRITE, &addr,
					   &lenp, &flags);
	mmap_write_unlock(mm);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_EQ(test, addr, CORTEN_ARENA_TEST_WIN + PMD_SIZE);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_misses(), misses + 1);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 0);
}

static void corten_arena_test_pool_limit(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	unsigned long base = CORTEN_ARENA_TEST_WIN;
	unsigned long addr, lenp, flags;
	long parks = corten_arena_test_pool_parks();
	long over = corten_arena_test_pool_over();
	long timeouts = corten_arena_test_drain_timeouts();
	long i;
	int ret;

	if (!corten_enabled_static())
		kunit_skip(test, "pool routing requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* Park CORTEN_ARENA_POOL_MAX arenas, then one more full-coverage
	 * munmap on a full pool: the D12 fallback -- the pre-pool real
	 * RELEASE, counted as pool_over.
	 */
	for (i = 0; i <= CORTEN_ARENA_POOL_MAX; i++) {
		unsigned long va = base + i * PMD_SIZE;
		struct vm_area_struct *vma;

		vma = corten_arena_test_mkvm(mm, va, va + PMD_SIZE,
					     CORTEN_ARENA_TEST_FLAGS_OK);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
		KUNIT_ASSERT_EQ(test,
				corten_arena_test_pool_attach(mm, va,
							      PMD_SIZE), 0);
		KUNIT_EXPECT_EQ(test,
				corten_arena_test_run_op(test, mm,
							 corten_arena_test_op_munmap_route,
							 va, PAGE_SIZE), 1);
	}

	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_parks(),
			parks + CORTEN_ARENA_POOL_MAX);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_over(), over + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm),
			CORTEN_ARENA_POOL_MAX);

	/* Arena MAX found the pool full and is really gone; arenas 0 and
	 * MAX - 1 are the oldest and newest parked slots.
	 */
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, base +
					   CORTEN_ARENA_POOL_MAX * PMD_SIZE));
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_test_pool_idle(mm,
						       base +
						       CORTEN_ARENA_POOL_MAX *
						       PMD_SIZE));
	KUNIT_EXPECT_TRUE(test, corten_arena_test_pool_idle(mm, base));
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_pool_idle(mm,
						      base +
						      (CORTEN_ARENA_POOL_MAX -
						       1) * PMD_SIZE));

	/* The bounded pool still serves: the route takes the MRU slot
	 * (the park order's tail) for the same size, ret 2.
	 */
	addr = 0;
	lenp = PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	mmap_write_lock(mm);
	ret = corten_arena_auto_mmap_route(mm, PAGE_SIZE,
					   PROT_READ | PROT_WRITE, &addr,
					   &lenp, &flags);
	mmap_write_unlock(mm);
	KUNIT_EXPECT_EQ(test, ret, 2);
	KUNIT_EXPECT_EQ(test, addr,
			base + (CORTEN_ARENA_POOL_MAX - 1) * PMD_SIZE);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm),
			CORTEN_ARENA_POOL_MAX - 1);

	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 0);
}

static void corten_arena_test_pool_mode_exit(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct vm_area_struct *vma;
	unsigned long parked = CORTEN_ARENA_TEST_WIN;
	unsigned long live = CORTEN_ARENA_TEST_WIN + 4 * PMD_SIZE;
	long timeouts = corten_arena_test_drain_timeouts();

	if (!corten_enabled_static())
		kunit_skip(test, "pool routing requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* One parked and one live arena. */
	vma = corten_arena_test_mkvm(mm, parked, parked + PMD_SIZE,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, parked, PMD_SIZE),
			0);
	vma = corten_arena_test_mkvm(mm, live, live + PMD_SIZE,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, live, PMD_SIZE), 0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 parked, PAGE_SIZE), 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 1);

	/* MODE exit tears both shapes down: the live arena through the
	 * regular RELEASE, the parked one through the idle-aware
	 * teardown (pool node dropped, no double accounting).
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_mode_get(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 0);
	KUNIT_EXPECT_FALSE(test, corten_arena_test_pool_idle(mm, parked));
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, parked), 0);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, parked));
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, live), 0);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, live));
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);
}

static void corten_arena_test_pool_fork(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct corten_arena_test_pool_fork fk;
	struct vm_area_struct *vma;
	struct task_struct *tsk;
	struct mm_struct *child;
	long timeouts = corten_arena_test_drain_timeouts();

	if (!corten_enabled_static())
		kunit_skip(test, "pool routing requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_WIN,
				     CORTEN_ARENA_TEST_WIN + PMD_SIZE,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 1);

	/* dup_mmap()-shaped window on an attached worker (the flush
	 * releases the parked reservation through do_munmap, which reads
	 * current->mm); the child copies no shadow pieces -- with the
	 * parked arena flushed there is nothing left to mirror.
	 */
	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	fk.parent = mm;
	fk.child = child;
	init_completion(&fk.done);
	tsk = kthread_run(corten_arena_test_pool_fork_thread, &fk,
			  "corten_pool_fork");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tsk);
	wait_for_completion(&fk.done);

	KUNIT_EXPECT_EQ(test, fk.begin_ret, 0);
	KUNIT_EXPECT_EQ(test, fk.commit_ret, 0);

	/* The parent came out of the fork in the pre-pool layout: the
	 * parked reservation is really gone.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 0);
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_test_pool_idle(mm,
						       CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_WIN),
			0);

	/* The child inherited the MODE bit; with the pool flushed there
	 * was nothing to mirror, so A5 leaves it registry-free -- query
	 * answers -ENOENT, the registry-less answer, and its first
	 * arena work will build the registry then.
	 */
	KUNIT_EXPECT_TRUE(test, READ_ONCE(child->corten_mode));
	KUNIT_EXPECT_EQ(test, corten_arena_query(child, CORTEN_ARENA_TEST_WIN),
			-ENOENT);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(child), 0);

	mmput(child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* ------------------------------------------------------------------ *
 * V-A.0 region record (MV_VMA_FREE_SPEC.md sec 2/3.1.0): the register
 * truth table, the attach-time reflection off the declaring VMA, the
 * registry iterator's dedup/sentinel/exhaustion contract, the park
 * RESERVED class and the fork's record copy.  Zero behaviour change is
 * the slice's red line: these tests only observe the new fields.
 * ------------------------------------------------------------------
 */

/* Register truth table + the DECLARE attach reflection (the live
 * write point) + the point-lookup alias.
 */
static void corten_arena_test_region_record(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct corten_arena synth;
	struct corten_arena *ar, *ar2;
	struct vm_area_struct *vma;

	/* Truth table on a detached holder (no registry involvement):
	 * the class lands, the MAY bound absorbs the recorded prot (the
	 * superset rule, sec 2.2), the flag reflection round-trips, the
	 * record is born single-piece with clear FILE/carrier payloads.
	 */
	memset(&synth, 0, sizeof(synth));
	synth.prot = CORTEN_PERM_USER | CORTEN_PERM_READ | CORTEN_PERM_WRITE;
	corten_region_register(&synth, CORTEN_REGION_ANON,
			       CORTEN_PERM_USER | CORTEN_PERM_READ, 0);
	KUNIT_EXPECT_EQ(test, synth.rclass, CORTEN_REGION_ANON);
	KUNIT_EXPECT_EQ(test, synth.may_prot,
			CORTEN_PERM_USER | CORTEN_PERM_READ |
			CORTEN_PERM_WRITE);
	KUNIT_EXPECT_EQ(test, synth.rflags, 0);
	KUNIT_EXPECT_EQ(test, synth.npieces, 1);
	KUNIT_EXPECT_TRUE(test, list_empty(&synth.rpieces));
	KUNIT_EXPECT_NULL(test, synth.rfile);
	KUNIT_EXPECT_NULL(test, synth.carrier);

	/* Every CORTEN_RF_* round-trips; the empty-MAY park shape still
	 * satisfies may_prot >= prot by construction.
	 */
	corten_region_register(&synth, CORTEN_REGION_RESERVED, 0,
			       CORTEN_RF_ALL);
	KUNIT_EXPECT_EQ(test, synth.rclass, CORTEN_REGION_RESERVED);
	KUNIT_EXPECT_EQ(test, synth.rflags, CORTEN_RF_ALL);
	KUNIT_EXPECT_EQ(test, synth.may_prot,
			CORTEN_PERM_USER | CORTEN_PERM_READ |
			CORTEN_PERM_WRITE);

	/* The DECLARE write point: the declaring VMA's access/MAY bits
	 * land in the record; the region lookup answers exactly what the
	 * arena lookup answers.
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	rcu_read_lock();
	ar = corten_region_lookup(mm, CORTEN_ARENA_TEST_BASE + PAGE_SIZE);
	KUNIT_EXPECT_NOT_NULL(test, ar);
	if (ar) {
		KUNIT_EXPECT_EQ(test, ar->rclass, CORTEN_REGION_ANON);
		KUNIT_EXPECT_EQ(test, ar->prot,
				CORTEN_PERM_USER | CORTEN_PERM_READ |
				CORTEN_PERM_WRITE);
		KUNIT_EXPECT_EQ(test, ar->may_prot,
				CORTEN_PERM_USER | CORTEN_PERM_READ |
				CORTEN_PERM_WRITE);
		KUNIT_EXPECT_EQ(test, ar->rflags, 0);
		KUNIT_EXPECT_EQ(test, ar->npieces, 1);
		KUNIT_EXPECT_TRUE(test, list_empty(&ar->rpieces));
		KUNIT_EXPECT_PTR_EQ(test, ar,
				    corten_arena_lookup(mm,
							CORTEN_ARENA_TEST_BASE));
	}
	KUNIT_EXPECT_NULL(test,
			  corten_region_lookup(mm, CORTEN_ARENA_TEST_NOWHERE));
	rcu_read_unlock();

#ifdef CONFIG_MEM_SOFT_DIRTY
	/* The whitelist-tolerated VM_SOFTDIRTY is the one RF row with a
	 * live producer: it must land in the record's flag reflection.
	 */
	vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_START2,
				     CORTEN_ARENA_TEST_START2 +
				     CORTEN_ARENA_TEST_LEN2,
				     CORTEN_ARENA_TEST_FLAGS_OK |
				     VM_SOFTDIRTY);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_START2,
					     CORTEN_ARENA_TEST_LEN2),
			0);
	rcu_read_lock();
	ar2 = corten_region_lookup(mm, CORTEN_ARENA_TEST_START2);
	KUNIT_EXPECT_NOT_NULL(test, ar2);
	if (ar2)
		KUNIT_EXPECT_EQ(test, ar2->rflags, CORTEN_RF_SOFTDIRTY);
	rcu_read_unlock();
#else
	/* Without soft-dirty tracking the RF rows are reservation-only:
	 * the truth table above is the whole encoding coverage.
	 */
	(void)vma;
	(void)ar2;
#endif
}

/* The registry iterator (sec 2.4): pointer dedup across one region's
 * frame slots, reserve-marker skipping, address order, exhaustion and
 * cursor restart.  The sentinel span comes from a real magazine claim
 * (the M4.T1 hook), the regions from plain gate-free DECLAREs.
 */
static void corten_arena_test_region_iter(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct corten_region_iter it;
	struct corten_arena *r1, *r2;
	struct vm_area_struct *vma;
	unsigned long addr = 0;
	int ret;

	/* A registry-less mm enumerates empty. */
	corten_region_iter_init(&it);
	rcu_read_lock();
	KUNIT_EXPECT_NULL(test, corten_region_next(mm, &it));
	rcu_read_unlock();

	/* Arena A: four frame slots -- produced exactly once. */
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);

	/* Claimed magazine frames between (address-wise: far above) the
	 * arenas: 512 reserve markers the walk must skip.  One PMD frame
	 * is served from the fresh segment; the served frame keeps its
	 * marker (allocation is bump-pointer bookkeeping only).
	 */
	ret = corten_arena_test_mag_alloc_cpu(mm, 0, PMD_SIZE, &addr);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, corten_arena_test_frame_reserved(mm, addr));

	/* Arena B: two frame slots. */
	vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_START2,
				     CORTEN_ARENA_TEST_START2 +
				     CORTEN_ARENA_TEST_LEN2,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_START2,
					     CORTEN_ARENA_TEST_LEN2),
			0);

	rcu_read_lock();
	corten_region_iter_init(&it);
	r1 = corten_region_next(mm, &it);
	KUNIT_EXPECT_NOT_NULL(test, r1);
	if (r1) {
		KUNIT_EXPECT_EQ(test, r1->start, CORTEN_ARENA_TEST_BASE);
		/* Mid-region frames resolve to the same region. */
		KUNIT_EXPECT_PTR_EQ(test, r1,
				    corten_region_lookup(mm,
							 CORTEN_ARENA_TEST_BASE +
							 3 * PMD_SIZE +
							 PAGE_SIZE));
	}
	r2 = corten_region_next(mm, &it);
	KUNIT_EXPECT_NOT_NULL(test, r2);
	if (r2)
		KUNIT_EXPECT_EQ(test, r2->start, CORTEN_ARENA_TEST_START2);
	/* Exhaustion: the sentinel span is not regions. */
	KUNIT_EXPECT_NULL(test, corten_region_next(mm, &it));

	/* A fresh cursor replays the same two regions in the same order
	 * (exactly one region per arena, ascending).
	 */
	corten_region_iter_init(&it);
	KUNIT_EXPECT_PTR_EQ(test, corten_region_next(mm, &it), r1);
	KUNIT_EXPECT_PTR_EQ(test, corten_region_next(mm, &it), r2);
	KUNIT_EXPECT_NULL(test, corten_region_next(mm, &it));
	rcu_read_unlock();
}

/* The park write point: idle <=> CORTEN_REGION_RESERVED, and the
 * visibility split of sec 2.4 -- the point lookup treats a parked
 * window as not-covering (legacy semantics) while the registry
 * enumeration still produces it.  Reactivation (the pool take) flips
 * the record back to ANON.
 */
static void corten_arena_test_region_park(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct corten_region_iter it;
	struct corten_arena *ar;
	struct vm_area_struct *vma;
	unsigned long addr = 0, len = PMD_SIZE, flags;
	int ret;

	if (!corten_enabled_static())
		kunit_skip(test, "park routing requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_WIN,
				     CORTEN_ARENA_TEST_WIN + PMD_SIZE,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	mmap_write_lock(mm);
	ret = corten_arena_auto_attach(mm, CORTEN_ARENA_TEST_WIN, PMD_SIZE);
	mmap_write_unlock(mm);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* The glibc free() shape: full-coverage munmap parks. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE),
			1);
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_pool_idle(mm,
						      CORTEN_ARENA_TEST_WIN));

	rcu_read_lock();
	KUNIT_EXPECT_NULL(test, corten_region_lookup(mm,
						     CORTEN_ARENA_TEST_WIN));
	corten_region_iter_init(&it);
	ar = corten_region_next(mm, &it);
	KUNIT_EXPECT_NOT_NULL(test, ar);
	if (ar) {
		KUNIT_EXPECT_EQ(test, ar->start, CORTEN_ARENA_TEST_WIN);
		KUNIT_EXPECT_EQ(test, ar->rclass, CORTEN_REGION_RESERVED);
		KUNIT_EXPECT_TRUE(test, READ_ONCE(ar->idle));
	}
	KUNIT_EXPECT_NULL(test, corten_region_next(mm, &it));
	rcu_read_unlock();

	/* The pool take: the reservation becomes a live region again,
	 * record and visibility both.
	 */
	flags = MAP_PRIVATE | MAP_ANONYMOUS;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_auto_route_locked(mm, len,
							    PROT_READ |
							    PROT_WRITE,
							    &addr, &len,
							    &flags),
			2);
	KUNIT_EXPECT_EQ(test, addr, CORTEN_ARENA_TEST_WIN);
	rcu_read_lock();
	ar = corten_region_lookup(mm, CORTEN_ARENA_TEST_WIN);
	KUNIT_EXPECT_NOT_NULL(test, ar);
	if (ar) {
		KUNIT_EXPECT_EQ(test, ar->rclass, CORTEN_REGION_ANON);
		KUNIT_EXPECT_EQ(test, ar->prot,
				CORTEN_PERM_USER | CORTEN_PERM_READ |
				CORTEN_PERM_WRITE);
		KUNIT_EXPECT_FALSE(test, READ_ONCE(ar->idle));
	}
	rcu_read_unlock();

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
}

/* The fork write point: the child's region record is the parent's copy
 * (register_child's deep-copy), one region per mirrored arena.  The
 * registry mirror runs gate-free -- the metadata walk below it has
 * nothing to observe on an off-boot kernel (untracked PT pages skip the
 * window loop), the child registration is the assertion target.
 */
static void corten_arena_test_region_fork(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm, *child;
	struct corten_region_iter it;
	struct corten_arena *par = NULL, *car;
	struct vm_area_struct *cvma;

	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);

	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	/* The dup_mmap loop's vm_area_dup shape: the child inherits the
	 * shadow piece (the mirror's child-side skip test needs it).
	 */
	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_BASE,
				      CORTEN_ARENA_TEST_BASE +
				      CORTEN_ARENA_TEST_LEN,
				      CORTEN_ARENA_TEST_FLAGS_OK |
				      VM_CORTEN | VM_NOHUGEPAGE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);

	rcu_read_lock();
	par = corten_region_lookup(mm, CORTEN_ARENA_TEST_BASE);
	car = corten_region_lookup(child, CORTEN_ARENA_TEST_BASE);
	KUNIT_EXPECT_NOT_NULL(test, par);
	KUNIT_EXPECT_NOT_NULL(test, car);
	if (par && car) {
		KUNIT_EXPECT_PTR_NE(test, par, car);
		KUNIT_EXPECT_EQ(test, car->rclass, CORTEN_REGION_ANON);
		KUNIT_EXPECT_EQ(test, car->prot, par->prot);
		KUNIT_EXPECT_EQ(test, car->may_prot, par->may_prot);
		KUNIT_EXPECT_EQ(test, car->rflags, par->rflags);
		KUNIT_EXPECT_EQ(test, car->npieces, 1);
	}
	/* Exactly one region on the child side: one arena, one region,
	 * the registry's 1:1 shape.
	 */
	corten_region_iter_init(&it);
	KUNIT_EXPECT_PTR_EQ(test, corten_region_next(child, &it), car);
	KUNIT_EXPECT_NULL(test, corten_region_next(child, &it));
	rcu_read_unlock();

	mmput(child);
}

/* ------------------------------------------------------------------ *
 * M6.T3: the shrinker pressure channel (M6_RMAP_SPEC.md sec 2.1 D2)
 * and M6.T4 observability.  The KUnit side pins the machinery up to
 * the isolation boundary -- registry membership, the per-desc resident
 * totals behind count_objects, the two-pass young-bit aging state
 * machine, the victim-rotation cursor and the pick-gate counters.
 * A real swap device round-trip (data consistency, batch throughput)
 * needs zram and is the guest criterion (spec T5), exactly like the
 * M6.T2 split.
 * ------------------------------------------------------------------
 */

/* Registry membership + count_objects + the live per-desc totals
 * (resident vs recorded-swap slots, M6.T4).
 */
static void corten_arena_test_shrink_registry_count(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct folio *folio;
	struct corten_txn txn;
	struct corten_pte_meta sm;
	pte_t *ptep, pte, swp_pte;
	spinlock_t *ptl;
	pmd_t *pmdp;
	unsigned long addr1 = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	unsigned long addr2 = CORTEN_ARENA_TEST_BASE + 2 * PAGE_SIZE;
	swp_entry_t entry = swp_entry(1, 0x77);
	long reg0, cnt0, res0, swp0;

	if (!corten_enabled_static())
		kunit_skip(test, "shrinker bodies require corten=on");

	reg0 = corten_arena_test_registry_nr();
	cnt0 = corten_arena_test_shrink_count();
	res0 = corten_arena_test_resident_pages();
	swp0 = corten_arena_test_swapped_pages();

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr1), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr1),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr2),
			0);

	/* Registry: the published state joined; count sees the two
	 * resident content pages (per-desc nr_mapped, exact).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_registry_nr(), reg0 + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_shrink_count(), cnt0 + 2);
	KUNIT_EXPECT_EQ(test, corten_arena_test_resident_pages(), res0 + 2);
	KUNIT_EXPECT_EQ(test, corten_arena_test_swapped_pages(), swp0);

	/* Convert addr2 to the synthetic swapped shape (the M6.T2
	 * inv7 hand-rolled swap-out): the resident total drops by one,
	 * the swapped-slot total rises by one -- the __resv slot
	 * accounting the T4 report reads.
	 */
	vma = vma_lookup(mm, addr2);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	pmdp = corten_arena_test_pmd(mm, addr2);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp, addr2, &ptl);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get_and_clear(mm, addr2, ptep);
	KUNIT_ASSERT_TRUE(test, pte_present(pte));
	swp_pte = swp_entry_to_pte(entry);
	set_pte_at(mm, addr2, ptep, swp_pte);
	pte_unmap_unlock(ptep, ptl);
	folio = page_folio(pte_page(pte));
	folio_remove_rmap_pte(folio, folio_page(folio, 0), vma);
	add_mm_counter(mm, MM_ANONPAGES, -1);
	folio_put(folio);

	KUNIT_ASSERT_EQ(test, corten_lock_range(mm, addr2, PAGE_SIZE, &txn),
			0);
	memset(&sm, 0, sizeof(sm));
	sm.state = CORTEN_SWAPPED;
	sm.perm = CORTEN_PERM_READ | CORTEN_PERM_WRITE | CORTEN_PERM_USER;
	corten_swap_encode(&sm, entry);
	KUNIT_EXPECT_EQ(test, corten_swap_out(&txn, addr2, &sm), 0);
	corten_unlock(&txn);

	KUNIT_EXPECT_EQ(test, corten_arena_test_resident_pages(), res0 + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_swapped_pages(), swp0 + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_shrink_count(), cnt0 + 1);

	/* The T4 render carries the new lines (named-counter reads are
	 * the same strings the DoD evidence greps).
	 */
	KUNIT_EXPECT_GE(test,
			corten_arena_test_named_counter(test, "resident_pages"),
			0);
	KUNIT_EXPECT_GE(test,
			corten_arena_test_named_counter(test, "swapped_pages"),
			0);
	KUNIT_EXPECT_GE(test,
			corten_arena_test_named_counter(test, "shrink_scans"),
			0);
	KUNIT_EXPECT_GE(test,
			corten_arena_test_named_counter(test, "aging_passes"),
			0);
	KUNIT_EXPECT_GE(test,
			corten_arena_test_named_counter(test, "swapout_rate"),
			0);

	/* Scrub the synthetic entry's slot before teardown (no zap: the
	 * fabricated entry must not reach free_swap_and_cache()).
	 */
	KUNIT_ASSERT_EQ(test, corten_lock_range(mm, addr2, PAGE_SIZE, &txn),
			0);
	KUNIT_EXPECT_EQ(test, corten_unmap(&txn, addr2, PAGE_SIZE,
					   CORTEN_UNMAP_KEEP_PERM), 0);
	corten_unlock(&txn);
}

/* Two-pass young-bit aging: pass 1 flags the window, a "touched" page
 * is spared by pass 2 and a cold page reaches the pick gate (observed
 * through the DMA-pin skip: KUnit has no swap device, so the pick ends
 * as a kept folio -- the guest round-trip is the T5 criterion).
 */
static void corten_arena_test_shrink_aging_two_pass(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct folio *fb;
	struct corten_pte_meta m;
	pte_t *ptep, pte;
	spinlock_t *ptl;
	pmd_t *pmdp;
	unsigned long base = CORTEN_ARENA_TEST_BASE;
	unsigned long addr_a = base + PAGE_SIZE;
	unsigned long addr_b = base + 2 * PAGE_SIZE;
	long scans0, aging0, skipped0;

	if (!corten_enabled_static())
		kunit_skip(test, "shrinker bodies require corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr_a), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr_a),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr_b),
			0);

	scans0 = corten_arena_test_shrink_scans();
	aging0 = corten_arena_test_aging_passes();
	skipped0 = corten_arena_test_shrink_skipped();

	/* Pass 1: the scan ages the window's two candidates and swaps
	 * nothing (budget == candidate count: one slice, no re-aging).
	 */
	KUNIT_EXPECT_EQ(test, (int)corten_arena_test_shrink_scan(2), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_shrink_scans(), scans0 + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_aging_passes(), aging0 + 1);
	KUNIT_EXPECT_TRUE(test, corten_arena_test_window_aged(mm, base));

	/* A was touched between the passes: its young bit comes back. */
	vma = vma_lookup(mm, addr_a);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	pmdp = corten_arena_test_pmd(mm, addr_a);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp, addr_a, &ptl);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	set_pte_at(mm, addr_a, ptep, pte_mkyoung(pte));
	pte_unmap_unlock(ptep, ptl);

	/* B stays cold and gets DMA-pinned: pass 2 evaluates the window,
	 * spares A (young), and B reaches the pick path where the pin
	 * gate counts the skip.
	 */
	pmdp = corten_arena_test_pmd(mm, addr_b);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp, addr_b, &ptl);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	fb = page_folio(pte_page(pte));
	folio_get(fb);
	folio_ref_add(fb, GUP_PIN_COUNTING_BIAS);
	pte_unmap_unlock(ptep, ptl);

	corten_arena_test_shrink_scan(2);

	KUNIT_EXPECT_EQ(test, corten_arena_test_shrink_skipped(),
			skipped0 + 1);
	/* Evaluated windows leave the aging set and the rotation has
	 * nothing new to age (two windows' worth of candidates, one
	 * window: the cursor restarts at frame 0 for the next scan).
	 */
	KUNIT_EXPECT_FALSE(test, corten_arena_test_window_aged(mm, base));

	/* Neither page left its resident shape (no swap device here):
	 * spared A keeps its young bit, skipped B keeps its pin.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr_a, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr_b, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_TRUE(test, folio_maybe_dma_pinned(fb));

	pmdp = corten_arena_test_pmd(mm, addr_a);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp, addr_a, &ptl);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	/* The pass-2 evaluation CONSUMED A's young bit -- that is the
	 * epoch clear (the page was spared because it was young at
	 * evaluation time, proven above: only B reached the pick gate).
	 */
	KUNIT_EXPECT_FALSE(test, pte_young(ptep_get(ptep)));
	pte_unmap_unlock(ptep, ptl);

	/* Unpin: the test hold goes back, the PTE reference survives
	 * until the mm teardown (one put only -- the r07 folio_put
	 * lesson).
	 */
	folio_ref_sub(fb, GUP_PIN_COUNTING_BIAS);
	folio_put(fb);
}

/* Rotation across windows: one candidate per window, one page of scan
 * budget per invocation -- the two windows age and evaluate on
 * alternating scans (the cursor round-robin that replaced the T2
 * from-frame-0 rescan).
 */
static void corten_arena_test_shrink_rotation(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm;
	unsigned long base = CORTEN_ARENA_TEST_BASE;
	unsigned long w1 = base + PAGE_SIZE;
	unsigned long w2 = base + PMD_SIZE + PAGE_SIZE;
	long aging0;

	if (!corten_enabled_static())
		kunit_skip(test, "shrinker bodies require corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, w1), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, w1), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, w2), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, w2), 0);

	aging0 = corten_arena_test_aging_passes();

	/* Scan 1: pass 1 ages window 1 (budget spent on its candidate).
	 * Scan 2: pass 2 evaluates window 1, budget leaves nothing for
	 * pass 1.  Scans 3/4 do the same for window 2.
	 */
	corten_arena_test_shrink_scan(1);
	KUNIT_EXPECT_TRUE(test, corten_arena_test_window_aged(mm, base));
	KUNIT_EXPECT_FALSE(test, corten_arena_test_window_aged(mm, w2));

	corten_arena_test_shrink_scan(1);
	KUNIT_EXPECT_FALSE(test, corten_arena_test_window_aged(mm, base));

	corten_arena_test_shrink_scan(1);
	KUNIT_EXPECT_FALSE(test, corten_arena_test_window_aged(mm, base));
	KUNIT_EXPECT_TRUE(test, corten_arena_test_window_aged(mm, w2));

	corten_arena_test_shrink_scan(1);
	KUNIT_EXPECT_FALSE(test, corten_arena_test_window_aged(mm, w2));

	KUNIT_EXPECT_EQ(test, corten_arena_test_aging_passes(), aging0 + 2);
}

/* The child-leg swap-PTE copy (the copy_nonpresent_pte() shape).  The
 * entry duplication is upstream's and needs a real swap device; this
 * harness entry is synthetic (bits only, like inv7_swapped), so only
 * the PTE install and the MM_SWAPENTS move are simulated -- the fork
 * mirror under test consumes the child PTE, not the entry count.
 */
static int corten_arena_test_fork_copy_swap_pte(struct kunit *test,
						struct mm_struct *dst,
						struct mm_struct *src,
						unsigned long addr)
{
	pte_t *sptep, *dptep, pte;
	spinlock_t *sptl, *dptl;
	pmd_t *pmdp;

	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_ensure_pt(test, dst,
							       addr), 0);

	pmdp = corten_arena_test_pmd(src, addr);
	KUNIT_ASSERT_NOT_NULL(test, pmdp);
	sptep = pte_offset_map_lock(src, pmdp, addr, &sptl);
	KUNIT_ASSERT_NOT_NULL(test, sptep);
	pte = ptep_get(sptep);
	pte_unmap_unlock(sptep, sptl);
	KUNIT_ASSERT_FALSE(test, pte_present(pte));
	KUNIT_ASSERT_FALSE(test, pte_none(pte));

	pmdp = corten_arena_test_pmd(dst, addr);
	KUNIT_ASSERT_NOT_NULL(test, pmdp);
	dptep = pte_offset_map_lock(dst, pmdp, addr, &dptl);
	KUNIT_ASSERT_NOT_NULL(test, dptep);
	set_ptes(dst, addr, dptep, pte, 1);
	add_mm_counter(dst, MM_SWAPENTS, 1);
	pte_unmap_unlock(dptep, dptl);

	return 0;
}

/* Fork after a swap-out (the T2 replay-arm gap the M6.T3 pressure
 * channel closes): the child mirrors the CORTEN_SWAPPED slot with the
 * entry payload, both sides stay INV7-clean and both slots count in
 * the registry totals.
 */
static void corten_arena_test_fork_swapped(struct kunit *test)
{
	struct corten_arena_test_mm *t;
	struct mm_struct *mm, *child;
	struct vm_area_struct *vma, *cvma;
	struct folio *folio;
	struct corten_txn txn;
	struct corten_pte_meta m, sm;
	pte_t *ptep, pte, swp_pte;
	spinlock_t *ptl;
	pmd_t *pmdp;
	unsigned long addr = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	swp_entry_t entry = swp_entry(1, 0x77);
	long swp0, violated, checked;

	if (!corten_enabled_static())
		kunit_skip(test, "fork mirror requires corten=on");

	t = corten_arena_test_mm_setup(test);
	mm = t->mm;
	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, addr), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, addr),
			0);

	swp0 = corten_arena_test_swapped_pages();

	/* Hand-rolled swap-out shape (the inv7_swapped sequence). */
	vma = vma_lookup(mm, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	pmdp = corten_arena_test_pmd(mm, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get_and_clear(mm, addr, ptep);
	KUNIT_ASSERT_TRUE(test, pte_present(pte));
	swp_pte = swp_entry_to_pte(entry);
	set_pte_at(mm, addr, ptep, swp_pte);
	pte_unmap_unlock(ptep, ptl);
	folio = page_folio(pte_page(pte));
	folio_remove_rmap_pte(folio, folio_page(folio, 0), vma);
	add_mm_counter(mm, MM_ANONPAGES, -1);
	folio_put(folio);

	KUNIT_ASSERT_EQ(test, corten_lock_range(mm, addr, PAGE_SIZE, &txn),
			0);
	memset(&sm, 0, sizeof(sm));
	sm.state = CORTEN_SWAPPED;
	sm.perm = CORTEN_PERM_READ | CORTEN_PERM_WRITE | CORTEN_PERM_USER;
	corten_swap_encode(&sm, entry);
	KUNIT_EXPECT_EQ(test, corten_swap_out(&txn, addr, &sm), 0);
	corten_unlock(&txn);

	KUNIT_EXPECT_EQ(test, corten_arena_test_swapped_pages(), swp0 + 1);

	/* The fork window (dup_mmap() shape). */
	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);

	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_BASE,
				      CORTEN_ARENA_TEST_BASE +
				      CORTEN_ARENA_TEST_LEN,
				      CORTEN_ARENA_TEST_FLAGS_OK |
				      VM_CORTEN | VM_NOHUGEPAGE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_fork_copy_swap_pte(test, child, mm,
							     addr), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);

	/* The child replayed the swapped slot: same state, same entry
	 * payload; the parent is unchanged.  Both count in the totals.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_SWAPPED);
	KUNIT_EXPECT_EQ(test, corten_swap_decode(&m).val, entry.val);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_SWAPPED);
	KUNIT_EXPECT_EQ(test, corten_swap_decode(&m).val, entry.val);
	KUNIT_EXPECT_EQ(test, m.perm,
			CORTEN_PERM_READ | CORTEN_PERM_WRITE |
			CORTEN_PERM_USER);
	KUNIT_EXPECT_EQ(test, corten_arena_test_swapped_pages(), swp0 + 2);

	/* INV7 both sides: the swapped half of the checker walks both
	 * descriptor trees with zero violations.
	 */
	corten_arena_test_inv7_walk(mm, &violated, &checked);
	KUNIT_EXPECT_EQ(test, violated, 0);
	corten_arena_test_inv7_walk(child, &violated, &checked);
	KUNIT_EXPECT_EQ(test, violated, 0);
	KUNIT_EXPECT_GE(test, checked, 1);

	/* Child exit: zap frees the (synthetic) entry shape -- the same
	 * tolerated teardown inv7_swapped exercises).
	 */
	mmput(child);

	/* Scrub the parent's slot before teardown (no zap path for the
	 * fabricated entry from this side either).
	 */
	KUNIT_ASSERT_EQ(test, corten_lock_range(mm, addr, PAGE_SIZE, &txn),
			0);
	KUNIT_EXPECT_EQ(test, corten_unmap(&txn, addr, PAGE_SIZE,
					   CORTEN_UNMAP_KEEP_PERM), 0);
	corten_unlock(&txn);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0),
			0);
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
	KUNIT_CASE(corten_arena_test_mag_recycle),
	KUNIT_CASE(corten_arena_test_mag_marker),
	KUNIT_CASE(corten_arena_test_pool_reuse),
	KUNIT_CASE(corten_arena_test_pool_limit),
	KUNIT_CASE(corten_arena_test_pool_mode_exit),
	KUNIT_CASE(corten_arena_test_pool_fork),
	KUNIT_CASE(corten_arena_test_region_record),
	KUNIT_CASE(corten_arena_test_region_iter),
	KUNIT_CASE(corten_arena_test_region_park),
	KUNIT_CASE(corten_arena_test_region_fork),
	KUNIT_CASE(corten_arena_test_mprotect_route),
	KUNIT_CASE(corten_arena_test_protect_flags_kernel_gate),
	KUNIT_CASE(corten_arena_test_madvise_route),
	KUNIT_CASE(corten_arena_test_mremap_route),
	KUNIT_CASE(corten_arena_test_fork_faithful),
	KUNIT_CASE(corten_arena_test_fork_unwind),
	KUNIT_CASE(corten_arena_test_fork_multipiece),
	KUNIT_CASE(corten_arena_test_fork_perm),
	KUNIT_CASE(corten_arena_test_inv7_shared_ro),
	KUNIT_CASE(corten_arena_test_inv7_swapped),
	KUNIT_CASE(corten_arena_test_shrink_registry_count),
	KUNIT_CASE(corten_arena_test_shrink_aging_two_pass),
	KUNIT_CASE(corten_arena_test_shrink_rotation),
	KUNIT_CASE(corten_arena_test_fork_swapped),
	KUNIT_CASE(corten_arena_test_fork_f2_gate),
	KUNIT_CASE(corten_arena_test_fork_drain_leak),
	KUNIT_CASE(corten_arena_test_unshare_pin),
	KUNIT_CASE(corten_arena_test_fork_reuse_exclusive),
	KUNIT_CASE(corten_arena_test_gup_state1_uncommitted),
	KUNIT_CASE(corten_arena_test_gup_state2_committed_rw),
	KUNIT_CASE(corten_arena_test_gup_state3_fork_cow),
	KUNIT_CASE(corten_arena_test_gup_state3b_downgrade_restore),
	KUNIT_CASE(corten_arena_test_gup_state4_pin_zap),
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
