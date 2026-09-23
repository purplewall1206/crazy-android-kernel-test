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
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mmap_lock.h>
#include <linux/mm_inline.h>
#include <linux/pgtable.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/shmem_fs.h>	/* shmem_file_setup (V-B.1 file lifecycle) */
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

/* F1 content anchor: pages checked per-page in the first 2M window. */
#define CORTEN_ARENA_TEST_PAGES		4

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
	/* V-A.1: the accounting a real mmap_region() would have done.
	 * The park now removes the reservation VMA (do_munmap), whose
	 * uncharge must land on a charged counter -- without this the
	 * harness's total_vm/data_vm underflow and the take's
	 * may_expand_vm() gate misfires.
	 */
	vm_stat_account(mm, flags, (end - start) >> PAGE_SHIFT);

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

	vm_stat_account(mm, vma->vm_flags, -vma_pages(vma));
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
	struct file *file;	/* the punch shape's mapping backend */
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
	ret = corten_arena_auto_mmap_route(mm, NULL, 0, len, prot, addr,
					   lenp, flagsp);
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

		/* V-A.2a: the window shape is the VMA-less auto takeover
		 * -- no declaring VMA exists (and none is unwound on
		 * failure).  The declare shape keeps its tree VMA.
		 */
		if (!c->use_window) {
			vma = corten_arena_test_mkvm(c->mm, c->start,
						     c->start + c->len,
						     CORTEN_ARENA_TEST_FLAGS_OK);
			if (!vma) {
				atomic_inc(&c->errors);
				break;
			}
		}

		/* The window shape runs the attach under the write lock,
		 * exactly as the do_mmap tail hook does (DEV-13 outermost
		 * acquisition); the declare shape takes the lock itself.
		 */
		if (c->use_window) {
			mmap_write_lock(c->mm);
			r = corten_arena_auto_attach(c->mm, c->start,
						     c->len,
						     PROT_READ | PROT_WRITE);
			mmap_write_unlock(c->mm);
		} else {
			r = corten_arena_declare(c->mm, c->start, c->len);
		}
		if (r) {
			atomic_inc(&c->errors);
			if (!c->use_window)
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

	/* V-B.1 flipped the file anchor: the whitelist's file mirror (no
	 * MAP_ANONYMOUS -- a file request never carries it) is the FILE
	 * arm's hit; the dedicated case below drives the full file truth
	 * table.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_classify(MAP_PRIVATE |
							MAP_NORESERVE, true),
			CORTEN_MMAP_AUTO_FILE);

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

/* V-B.1: the file mirror of the auto-mmap whitelist (MV_VMA_FREE_SPEC.md
 * sec 3.2.1 -- the dlopen shape is the canonical hit; MAP_SHARED in any
 * spelling and every extra flag bit stay legacy, MAP_FIXED included:
 * the OQ-MV-2 implant exception keeps the D-G'' punch route).
 */
static void corten_arena_test_auto_classify_file(struct kunit *test)
{
	static const unsigned long bad_bits[] = {
		MAP_FIXED, MAP_FIXED_NOREPLACE, MAP_HUGETLB, MAP_GROWSDOWN,
		MAP_POPULATE, MAP_LOCKED, MAP_SYNC, MAP_STACK,
		MAP_UNINITIALIZED, MAP_DENYWRITE, MAP_EXECUTABLE,
		MAP_NONBLOCK, MAP_DROPPABLE, MAP_32BIT,
	};
	unsigned long good = MAP_PRIVATE | MAP_NORESERVE;
	size_t i;

	/* The clean hits: bare MAP_PRIVATE and with MAP_NORESERVE. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_classify(MAP_PRIVATE, true),
			CORTEN_MMAP_AUTO_FILE);
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_classify(good, true),
			CORTEN_MMAP_AUTO_FILE);

	/* MAP_SHARED / _VALIDATE never enter the window (file or not);
	 * the MAP_DROPPABLE alias inside the type nibble rejects alike.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_classify(MAP_SHARED, true),
			CORTEN_MMAP_LEGACY);
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_classify(MAP_SHARED_VALIDATE,
							true),
			CORTEN_MMAP_LEGACY);
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_classify(MAP_PRIVATE |
							MAP_DROPPABLE, true),
			CORTEN_MMAP_LEGACY);

	/* Defensive: a file request carrying MAP_ANONYMOUS (the syscall
	 * cannot produce it) stays legacy instead of half-classifying.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_classify(MAP_PRIVATE |
							MAP_ANONYMOUS, true),
			CORTEN_MMAP_LEGACY);

	/* Every other flag bit keeps the mapping legacy -- MAP_FIXED is
	 * the OQ-MV-2 implant exception, POPULATE/DENYWRITE/STACK etc.
	 * the same single-bit whitelist as the anon arm.
	 */
	for (i = 0; i < ARRAY_SIZE(bad_bits); i++) {
		KUNIT_EXPECT_EQ_MSG(test,
				    corten_arena_auto_mmap_classify(good |
								    bad_bits[i],
								    true),
				    CORTEN_MMAP_LEGACY,
				    "file flag bit %#lx must stay legacy",
				    bad_bits[i]);
	}

	/* The anon arm itself is unchanged: no MAP_ANONYMOUS without a
	 * file backing stays legacy (the pre-B.1 anchors above already
	 * cover the full anon table).
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_classify(MAP_PRIVATE, false),
			CORTEN_MMAP_LEGACY);
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

	/* Attach it (the do_mmap completion body) so the release can see
	 * a real arena, then release it directly (the prctl RELEASE body:
	 * drain + teardown + magazine recycle).  V-A.2a: the takeover is
	 * complete after the route -- no VMA is (or needs to be)
	 * installed between the two halves.
	 */
	mmap_write_lock(mm);
	ret = corten_arena_auto_attach(mm, CORTEN_MODE_WINDOW_START,
				       PMD_SIZE, PROT_READ | PROT_WRITE);
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

	/* A glibc-shaped mmap: V-A.2a -- the takeover completes WITHOUT
	 * any VMA; the attach is the do_mmap completion body on a bare
	 * window.  The arena must be exactly one 2M chunk here: the
	 * release rule only applies while the leftover tail stays under
	 * 2M.
	 */
	mmap_write_lock(mm);
	ret = corten_arena_auto_attach(mm, CORTEN_ARENA_TEST_WIN, PMD_SIZE, PROT_READ | PROT_WRITE);
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
	 * instead of tearing down: lookup-invisible (query 0), the pool
	 * holds it.  V-A.1 (S-4): the reservation VMA is gone too -- the
	 * parked window reads as already-munmapped.
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
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));

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

		mmap_write_lock(mm);
		ret = corten_arena_auto_attach(mm, CORTEN_ARENA_TEST_START2,
					       2 * PMD_SIZE,
					       PROT_READ | PROT_WRITE);
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

	/* A genuine mid-arena chunk keeps the VA: chunk zap, not release.
	 * V-A.2a: the auto takeover is VMA-less -- the VA is kept by the
	 * frame table alone.
	 */
	mmap_write_lock(mm);
	ret = corten_arena_auto_attach(mm, CORTEN_ARENA_TEST_WIN,
				       CORTEN_ARENA_TEST_WIN_LEN,
				       PROT_READ | PROT_WRITE);
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
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));
}

/* ------------------------------------------------------------------ *
 * V-B.1: FILE region lifecycle -- the route's file arm three-state
 * truth, the file-reference ledger across every teardown point
 * (release / park / reactivate / mm_exit / mode_exit / fork), the
 * i_mmap visibility pairing, and the INV-MV3(d) payload pairing.
 * ------------------------------------------------------------------
 */

/* Helpers defined with the fork scaffolding below (used here). */
static int corten_arena_test_meta(struct mm_struct *mm, unsigned long addr,
				  struct corten_pte_meta *out);
static int corten_arena_test_page_word(struct mm_struct *mm,
				       unsigned long addr, u64 *val,
				       bool write);
static pmd_t *corten_arena_test_pmd(struct mm_struct *mm,
				    unsigned long addr);
static int corten_arena_test_fork_begin(struct mm_struct *child,
					struct mm_struct *parent);
static int corten_arena_test_fork_commit(struct mm_struct *child,
					 struct mm_struct *parent);
static long corten_arena_test_named_counter(struct kunit *test,
					    const char *name);
static void corten_arena_test_inv7_walk(struct mm_struct *mm, long *violated,
					long *checked);

/* Route wrapper: do_mmap()'s file front half, NULL pgoff caller shape.
 * V-B.3: the dark gate is gone -- this runs the real takeover (the
 * B.1-era override no longer exists; this wrapper is now the
 * route-takeover regression anchor).
 */
static int corten_arena_test_file_route(struct mm_struct *mm,
					struct file *file,
					unsigned long len,
					unsigned long *addr,
					unsigned long *lenp,
					unsigned long *flagsp)
{
	int ret;

	mmap_write_lock(mm);
	ret = corten_arena_auto_mmap_route(mm, file, 0, len, PROT_READ |
					   PROT_EXEC, addr, lenp, flagsp);
	mmap_write_unlock(mm);

	return ret;
}

/* Attach wrapper: do_mmap()'s file completion body, with the prot the
 * caller wants (the R|X shape below is the common one).
 */
static int corten_arena_test_file_attach_prot(struct mm_struct *mm,
					      unsigned long addr,
					      struct file *file,
					      unsigned long pgoff,
					      unsigned long prot)
{
	int ret;

	mmap_write_lock(mm);
	ret = corten_arena_file_attach(mm, addr, PMD_SIZE, prot, file, pgoff);
	mmap_write_unlock(mm);

	return ret;
}

static int corten_arena_test_file_attach(struct mm_struct *mm,
					 unsigned long addr,
					 struct file *file,
					 unsigned long pgoff)
{
	return corten_arena_test_file_attach_prot(mm, addr, file, pgoff,
						  PROT_READ | PROT_EXEC);
}

static void corten_arena_test_file_lifecycle(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm, *child;
	struct vm_area_struct *carrier;
	struct corten_pte_meta m;
	struct corten_arena *ar;
	struct file *file;
	struct address_space *mapping;
	const unsigned long pgoff = 3;
	unsigned long addr = 0, lenp = 2 * PAGE_SIZE, flags;
	long base, timeouts;

	if (!corten_enabled_static())
		kunit_skip(test, "FILE lifecycle requires corten=on");

	/* A real file: the reference ledger needs a live f_ref, the i_mmap
	 * anchor a real address space (shmem passes corten_file_may: read
	 * mode, mmap hook, no DAX, no noexec seal).
	 */
	file = shmem_file_setup("corten_vb1", PMD_SIZE, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	mapping = file->f_mapping;
	base = file_count(file);
	timeouts = corten_arena_test_drain_timeouts();

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* Route three-state truth: a non-whitelisted file shape answers 0
	 * without touching the request; the whitelist hit places a fresh
	 * window (ret 1) and rewrites the request like the anon arm.
	 */
	flags = MAP_PRIVATE | MAP_POPULATE;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_file_route(mm, file, lenp, &addr,
						     &lenp, &flags), 0);
	KUNIT_EXPECT_EQ(test, addr, 0);
	KUNIT_EXPECT_EQ(test, lenp, 2 * PAGE_SIZE);

	addr = 0;
	lenp = 2 * PAGE_SIZE;
	flags = MAP_PRIVATE;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_file_route(mm, file, lenp, &addr,
						     &lenp, &flags), 1);
	KUNIT_EXPECT_EQ(test, addr, CORTEN_ARENA_TEST_WIN);
	KUNIT_EXPECT_EQ(test, lenp, PMD_SIZE);
	KUNIT_EXPECT_EQ(test, flags, MAP_PRIVATE | MAP_FIXED |
				     MAP_NORESERVE);

	/* Attach (the do_mmap completion): +1 file reference (the
	 * region's own), the FILE record fully paired.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_file_attach(mm,
						      CORTEN_ARENA_TEST_WIN,
						      file, pgoff), 0);
	KUNIT_EXPECT_EQ(test, file_count(file), base + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_WIN),
			1);

	rcu_read_lock();
	ar = xa_load(&mm->corten_state->arenas,
		     CORTEN_ARENA_TEST_WIN >> PMD_SHIFT);
	KUNIT_ASSERT_NOT_NULL(test, ar);
	KUNIT_EXPECT_EQ(test, ar->rclass, CORTEN_REGION_FILE);
	KUNIT_EXPECT_PTR_EQ(test, ar->rfile, file);
	KUNIT_EXPECT_EQ(test, ar->rpoff, pgoff);
	rcu_read_unlock();

	carrier = corten_arena_test_carrier_of(mm, CORTEN_ARENA_TEST_WIN);
	KUNIT_ASSERT_NOT_NULL(test, carrier);
	KUNIT_EXPECT_PTR_EQ(test, carrier->vm_file, file);
	KUNIT_EXPECT_EQ(test, carrier->vm_pgoff, pgoff);

	mmap_read_lock(mm);
	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(mm));
	mmap_read_unlock(mm);

	/* The whole-region virtual allocation: FILE_MAPPED with the
	 * recorded perm; V-B.3 routes a read fault on it to the pagecache
	 * read arm (the B.1 STUB boundary is gone with the dark gate).
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_WIN, &m),
			0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_FILE_MAPPED);
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_PERM_READ | CORTEN_PERM_EXEC |
				       CORTEN_PERM_USER);
	KUNIT_EXPECT_EQ(test, corten_arena_dispatch(&m, false, false),
			CORTEN_DISP_FILE_READ);

	/* INV-MV3(d) negative anchors: each torn payload shape is
	 * reported by the registry walk.
	 */
	mmap_read_lock(mm);
	rcu_read_lock();
	ar = xa_load(&mm->corten_state->arenas,
		     CORTEN_ARENA_TEST_WIN >> PMD_SHIFT);
	if (ar) {
		struct file *rfile = ar->rfile;

		WRITE_ONCE(ar->rfile, NULL);
		KUNIT_EXPECT_FALSE(test, corten_region_invariants_ok(mm));
		WRITE_ONCE(ar->rfile, rfile);

		WRITE_ONCE(carrier->vm_pgoff, pgoff + 1);
		KUNIT_EXPECT_FALSE(test, corten_region_invariants_ok(mm));
		WRITE_ONCE(carrier->vm_pgoff, pgoff);

		WRITE_ONCE(ar->rclass, CORTEN_REGION_ANON);
		KUNIT_EXPECT_FALSE(test, corten_region_invariants_ok(mm));
		WRITE_ONCE(ar->rclass, CORTEN_REGION_FILE);
	}
	rcu_read_unlock();
	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(mm));
	mmap_read_unlock(mm);

	/* i_mmap visibility: an interval-tree query over [pgoff,
	 * pgoff + pages) finds the carrier (the __vma_link_file pairing;
	 * B.2's truncate gate walks exactly this).
	 */
	i_mmap_lock_read(mapping);
	KUNIT_EXPECT_PTR_EQ(test,
			    vma_interval_tree_iter_first(&mapping->i_mmap,
							 pgoff,
							 pgoff + (PMD_SIZE >>
								  PAGE_SHIFT) -
							 1),
			    carrier);
	i_mmap_unlock_read(mapping);

	/* Park (the munmap full-coverage shape): the payload dies with
	 * the park -- reference back, carrier disarmed, i_mmap empty,
	 * window reusable as the ANON contract.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_pool_idle(mm,
						      CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_EQ(test, file_count(file), base);
	KUNIT_EXPECT_NULL(test, carrier->vm_file);
	i_mmap_lock_read(mapping);
	KUNIT_EXPECT_NULL(test,
			  vma_interval_tree_iter_first(&mapping->i_mmap, 0,
						       ULONG_MAX));
	i_mmap_unlock_read(mapping);
	mmap_read_lock(mm);
	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(mm));
	mmap_read_unlock(mm);

	/* Reactivation is the ANON contract: the parked window serves an
	 * anon auto mmap (ret 2) and never resurrects the file.
	 */
	addr = 0;
	lenp = PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_auto_route_locked(mm, lenp,
							    PROT_READ |
							    PROT_WRITE, &addr,
							    &lenp, &flags),
			2);
	KUNIT_EXPECT_EQ(test, addr, CORTEN_ARENA_TEST_WIN);
	KUNIT_EXPECT_EQ(test, file_count(file), base);

	/* Real RELEASE (the prctl arm): +1 then teardown -- reference
	 * back, descriptor gone.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_file_attach(mm,
						      CORTEN_ARENA_TEST_START2,
						      file, 0), 0);
	KUNIT_EXPECT_EQ(test, file_count(file), base + 1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_release,
						 CORTEN_ARENA_TEST_START2,
						 PMD_SIZE), 0);
	KUNIT_EXPECT_EQ(test, file_count(file), base);

	/* MODE exit (RELEASE per arena): the FILE arena and the
	 * reactivated ANON one both tear down -- ledger back at base.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_file_attach(mm,
						      CORTEN_ARENA_TEST_START2,
						      file, 0), 0);
	KUNIT_EXPECT_EQ(test, file_count(file), base + 1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
	KUNIT_EXPECT_EQ(test, file_count(file), base);
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);

	/* Fork: the child mirrors the payload -- its own reference (same
	 * file object), the same pgoff, a FILE carrier of its own; the
	 * child's exit (the mm_exit arm) drops the mirror.
	 */
	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_file_attach(mm,
						      CORTEN_ARENA_TEST_WIN,
						      file, pgoff), 0);
	KUNIT_EXPECT_EQ(test, file_count(file), base + 1);

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);
	KUNIT_EXPECT_EQ(test, file_count(file), base + 2);

	carrier = corten_arena_test_carrier_of(child,
					       CORTEN_ARENA_TEST_WIN);
	KUNIT_ASSERT_NOT_NULL(test, carrier);
	KUNIT_EXPECT_PTR_EQ(test, carrier->vm_file, file);
	KUNIT_EXPECT_EQ(test, carrier->vm_pgoff, pgoff);
	mmap_read_lock(child);
	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(child));
	mmap_read_unlock(child);

	/* mm_exit arm: the child's whole registry tears down with its mm
	 * -- the mirror reference returns.
	 */
	mmput(child);
	KUNIT_EXPECT_EQ(test, file_count(file), base + 1);

	/* Error-path anchor (zero leak): a forced fork_commit failure
	 * discards the half-mirrored child through the same mm_exit --
	 * the half-armed payload's disarm is exercised by the child's own
	 * teardown.
	 */
	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	corten_arena_test_fork_fail_arm(2);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	KUNIT_EXPECT_NE(test, corten_arena_test_fork_commit(child, mm), 0);
	corten_arena_test_fork_fail_arm(0);
	mmput(child);
	KUNIT_EXPECT_EQ(test, file_count(file), base + 1);

	/* Parent teardown: the last FILE reference returns with the
	 * arena; the tree ends empty.  The test's own fput below drops
	 * the final reference (a leak here would keep it above 1).
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
	KUNIT_EXPECT_EQ(test, file_count(file), base);
	i_mmap_lock_read(mapping);
	KUNIT_EXPECT_NULL(test,
			  vma_interval_tree_iter_first(&mapping->i_mmap, 0,
						       ULONG_MAX));
	i_mmap_unlock_read(mapping);
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);

	fput(file);
}

/* ------------------------------------------------------------------ *
 * V-B.2: the H7 route gate -- a file-side unmap event (truncate,
 * invalidation, single-folio re-invalidation) arriving on a FILE
 * carrier is demoted through the chunk-zap transaction (KEEP_PERM:
 * content dropped, VA and recorded perm kept), never through the bare
 * legacy PTE writer; the zap_page_range_single backstop refuses a
 * carrier loudly; the B.1 dark-gate fault verdict (SEGV_MAPERR)
 * survives the demotion instead of synthesizing anonymous zero pages.
 * ------------------------------------------------------------------
 */
static void corten_arena_test_truncate_route(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct vm_area_struct *carrier, *plain;
	const unsigned long pgoff = 3;
	struct address_space *mapping;
	struct corten_pte_meta m;
	struct file *file;
	struct folio *folio;
	unsigned int fflags;
	long routes, refuses, base;

	if (!corten_enabled_static())
		kunit_skip(test, "H7 gate requires corten=on");

	file = shmem_file_setup("corten_vb2", 4 * PMD_SIZE, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	mapping = file->f_mapping;
	base = file_count(file);
	routes = corten_arena_test_truncate_routes();
	refuses = corten_arena_test_zap_single_refuses();

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_file_attach(mm,
						      CORTEN_ARENA_TEST_WIN,
						      file, pgoff), 0);
	carrier = corten_arena_test_carrier_of(mm, CORTEN_ARENA_TEST_WIN);
	KUNIT_ASSERT_NOT_NULL(test, carrier);

	/* Pre-state: the whole region is the B.1 FILE_MAPPED virtual
	 * allocation with the recorded perm.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_WIN, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_FILE_MAPPED);
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_PERM_READ | CORTEN_PERM_EXEC |
				       CORTEN_PERM_USER);

	/* The gate's negative arm: a plain (non-carrier) VMA is none of
	 * its business -- both the direct call and the backstop stay
	 * quiet, and the legacy zap of an untouched range changes nothing.
	 */
	plain = vma_lookup(mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_NULL(test, plain);
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_unmap_file_event(plain,
							 CORTEN_ARENA_TEST_BASE,
							 CORTEN_ARENA_TEST_BASE +
							 PAGE_SIZE, true));
	zap_page_range_single(plain, CORTEN_ARENA_TEST_BASE, PAGE_SIZE, NULL);
	KUNIT_EXPECT_EQ(test, corten_arena_test_zap_single_refuses(), refuses);
	KUNIT_EXPECT_EQ(test, corten_arena_test_truncate_routes(), routes);

	/* Truncate (even_cows): the whole-window file event -- the real
	 * upstream walker hands the carrier over under i_mmap_lock_read.
	 * Every slot demotes to Invalid with the perm kept; the region
	 * record, the carrier's i_mmap membership and the file reference
	 * all survive (a truncate is not an munmap).
	 */
	unmap_mapping_pages(mapping, pgoff, PMD_SIZE >> PAGE_SHIFT, true);
	KUNIT_EXPECT_EQ(test, corten_arena_test_truncate_routes(), routes + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_zap_single_refuses(), refuses);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_WIN, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_PERM_READ | CORTEN_PERM_EXEC |
				       CORTEN_PERM_USER);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_WIN +
						  PMD_SIZE - PAGE_SIZE, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);
	i_mmap_lock_read(mapping);
	KUNIT_EXPECT_PTR_EQ(test,
			    vma_interval_tree_iter_first(&mapping->i_mmap,
							 pgoff, pgoff), carrier);
	i_mmap_unlock_read(mapping);
	KUNIT_EXPECT_PTR_EQ(test, carrier->vm_file, file);
	KUNIT_EXPECT_EQ(test, file_count(file), base + 1);

	/* Re-fault on a demoted slot: V-B.3's rclass-aware FRESH arm
	 * re-synthesizes FILE_MAPPED (never PrivateAnon -- that would
	 * present anonymous zero bytes where the file's new content
	 * belongs) and the read arm re-reads the file's new content.
	 * The B.2-era dark-gate verdict (SEGV_MAPERR) is gone with the
	 * gate.
	 */
	{
		u64 pat = 0xa5b6c7d8e9f01122ULL;
		loff_t pos = (loff_t)pgoff << PAGE_SHIFT;

		/* The file's "new content" (written through the pagecache
		 * after the truncation event).
		 */
		KUNIT_ASSERT_EQ(test,
				kernel_write(file, &pat, sizeof(pat), &pos),
				(ssize_t)sizeof(pat));
	}
	fflags = 0;
	KUNIT_EXPECT_EQ(test,
			corten_arena_user_fault(mm, CORTEN_ARENA_TEST_WIN, 0,
						NULL, &fflags),
			CORTEN_FAULT_HANDLED);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_WIN, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_FILE_MAPPED);
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_PERM_READ | CORTEN_PERM_EXEC |
				       CORTEN_PERM_USER);
	{
		u64 back = 0;

		KUNIT_ASSERT_EQ(test,
				corten_arena_test_page_word(mm,
							    CORTEN_ARENA_TEST_WIN,
							    &back, false), 0);
		KUNIT_EXPECT_EQ(test, back, 0xa5b6c7d8e9f01122ULL);
	}

	/* Partial invalidation (even_cows == false, the reclaim arm): a
	 * fresh second region at pgoff 0, one page of the file event --
	 * exactly the intersecting VA slot demotes, the neighbor keeps
	 * its FILE_MAPPED virtual allocation.
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_file_attach(mm,
						      CORTEN_ARENA_TEST_START2,
						      file, 0), 0);
	unmap_mapping_pages(mapping, 0, 1, false);
	KUNIT_EXPECT_EQ(test, corten_arena_test_truncate_routes(), routes + 2);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_START2,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_PERM_READ | CORTEN_PERM_EXEC |
				       CORTEN_PERM_USER);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_START2 +
						  PAGE_SIZE, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_FILE_MAPPED);

	/* Single-folio invalidation (the hwpoison/migrate re-check shape):
	 * a locked pagecache folio inside the second carrier's window
	 * walks the same i_mmap tree with a one-page VA window.
	 * filemap_add_folio() hands the folio over locked -- exactly the
	 * unmap_mapping_folio() contract.
	 */
	folio = filemap_alloc_folio(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	KUNIT_ASSERT_EQ(test,
			filemap_add_folio(mapping, folio, 2, GFP_KERNEL), 0);
	unmap_mapping_folio(folio);
	KUNIT_EXPECT_EQ(test, corten_arena_test_truncate_routes(), routes + 3);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_START2 +
						  2 * PAGE_SIZE, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_START2 +
						  3 * PAGE_SIZE, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_FILE_MAPPED);
	filemap_remove_folio(folio);
	folio_unlock(folio);
	folio_put(folio);

	/* The backstop's negative sample: a carrier arriving at the bare
	 * legacy writer is refused, counted, and changes nothing -- the
	 * still-FILE_MAPPED slot must survive the refused zap verbatim.
	 */
	zap_page_range_single(carrier, CORTEN_ARENA_TEST_START2 + PAGE_SIZE,
			      PAGE_SIZE, NULL);
	KUNIT_EXPECT_EQ(test, corten_arena_test_zap_single_refuses(),
			refuses + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_truncate_routes(), routes + 3);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_START2 +
						  PAGE_SIZE, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_FILE_MAPPED);

	/* The registry is still INV-MV3 clean after the whole event
	 * family, and the teardown returns the ledger.
	 */
	mmap_read_lock(mm);
	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(mm));
	mmap_read_unlock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
	KUNIT_EXPECT_EQ(test, file_count(file), base);

	fput(file);
}

/* V-B.3: the rss counters are percpu_counter-batched; get_mm_counter()
 * only reads the folded global, so a handful of +/-1 deltas on one CPU
 * stays invisible.  The exact-sum walk is the assertion-grade read.
 */
static long corten_arena_test_mm_counter(struct mm_struct *mm, int member)
{
	return percpu_counter_sum_positive(&mm->rss_stat[member]);
}

/* V-B.4: the file rss family of a mapping -- mm_counter_file()'s answer
 * for its folios (a shmem/tmpfs host is MM_SHMEMPAGES, a real file
 * MM_FILEPAGES).  The read arm, the COW split and the fork PTE copy
 * (copy_page_range over the carriers) all account this family; the
 * B.3-era everything-is-FILEPAGES spelling drifted the child's counters
 * against its own zap at fork time.
 */
static int corten_arena_test_file_rss(struct address_space *mapping)
{
	return shmem_mapping(mapping) ? MM_SHMEMPAGES : MM_FILEPAGES;
}

/* ------------------------------------------------------------------ *
 * V-B.3: the FILE_MAPPED fault arms -- the read path (attach ->
 * fault -> the translation serves the file's content, meta stays
 * FILE_MAPPED, clean read-only PTE, MM_FILEPAGES accounting, the
 * pagecache folio's rmap) and the EOF verdict (pgoff past i_size ->
 * CORTEN_FAULT_BUS, the filemap_fault() SIGBUS shape).
 * ------------------------------------------------------------------
 */
static void corten_arena_test_file_read(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct address_space *mapping;
	struct vm_area_struct *carrier;
	struct corten_pte_meta m;
	struct file *file;
	struct folio *folio;
	struct page *page;
	pmd_t *pmdp;
	pte_t *ptep, pte;
	spinlock_t *ptl;	/* guards the read-arm install read */
	unsigned int fflags;
	long base, filepg;
	int filepg_family;
	u64 pat = 0x1122334455667788ULL, back = 0;
	loff_t pos = PAGE_SIZE;

	if (!corten_enabled_static())
		kunit_skip(test, "FILE read arm requires corten=on");

	/* Three pages of shmem: page 0 stays a hole (zero-fill read),
	 * page 1 carries a pattern, page 2 is the last page.
	 */
	file = shmem_file_setup("corten_vb3r", 3 * PAGE_SIZE, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	mapping = file->f_mapping;
	base = file_count(file);
	KUNIT_ASSERT_EQ(test,
			kernel_write(file, &pat, sizeof(pat), &pos),
			(ssize_t)sizeof(pat));

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_file_attach(mm,
						      CORTEN_ARENA_TEST_WIN,
						      file, 0), 0);
	carrier = corten_arena_test_carrier_of(mm, CORTEN_ARENA_TEST_WIN);
	KUNIT_ASSERT_NOT_NULL(test, carrier);
	filepg_family = corten_arena_test_file_rss(mapping);
	filepg = corten_arena_test_mm_counter(mm, filepg_family);

	/* The read fault: handled, and the metadata is NOT rewritten --
	 * FILE_MAPPED is both the virtual allocation and the resident
	 * form (no __resv payload, no slot transition).
	 */
	fflags = 0;
	KUNIT_EXPECT_EQ(test,
			corten_arena_user_fault(mm,
						CORTEN_ARENA_TEST_WIN + PAGE_SIZE,
						0, NULL, &fflags),
			CORTEN_FAULT_HANDLED);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm,
					       CORTEN_ARENA_TEST_WIN + PAGE_SIZE,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_FILE_MAPPED);
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_PERM_READ | CORTEN_PERM_EXEC |
				       CORTEN_PERM_USER);

	/* The translation itself: present, clean, read-only (a private
	 * file mapping is never written through -- the RW contract is
	 * satisfied by the COW, and PAGE_COPY keeps the installed PTE
	 * RO), on the pagecache folio of pgoff 1.
	 */
	pmdp = corten_arena_test_pmd(mm, CORTEN_ARENA_TEST_WIN + PAGE_SIZE);
	KUNIT_ASSERT_NOT_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp,
				   CORTEN_ARENA_TEST_WIN + PAGE_SIZE, &ptl);
	KUNIT_ASSERT_NOT_NULL(test, ptep);
	pte = ptep_get(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));
	KUNIT_EXPECT_FALSE(test, pte_write(pte));
	KUNIT_EXPECT_FALSE(test, pte_dirty(pte));
	page = pte_page(pte);
	pte_unmap_unlock(ptep, ptl);
	folio = filemap_get_folio(mapping, 1);
	KUNIT_ASSERT_FALSE(test, IS_ERR(folio));
	KUNIT_EXPECT_TRUE(test, page_folio(page) == folio);
	/* The read arm's rmap: the PTE's map on the file mapping. */
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 1);
	folio_put(folio);

	/* The content is the file's content. */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(mm,
						    CORTEN_ARENA_TEST_WIN +
						    PAGE_SIZE, &back, false),
			0);
	KUNIT_EXPECT_EQ(test, back, pat);

	/* Accounting: exactly one file page (the hole page was not
	 * touched yet).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, filepg_family),
			filepg + 1);

	/* A hole page inside EOF reads zeros (the shmem clear: shape of
	 * the fetch).
	 */
	fflags = 0;
	KUNIT_EXPECT_EQ(test,
			corten_arena_user_fault(mm, CORTEN_ARENA_TEST_WIN,
						0, NULL, &fflags),
			CORTEN_FAULT_HANDLED);
	back = ~0ULL;
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(mm,
						    CORTEN_ARENA_TEST_WIN,
						    &back, false), 0);
	KUNIT_EXPECT_EQ(test, back, 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, filepg_family),
			filepg + 2);

	/* Re-fault of an installed page: the redundant-reference arm of
	 * the read path answers handled without a second install.
	 */
	fflags = 0;
	KUNIT_EXPECT_EQ(test,
			corten_arena_user_fault(mm,
						CORTEN_ARENA_TEST_WIN + PAGE_SIZE,
						0, NULL, &fflags),
			CORTEN_FAULT_HANDLED);

	/* Beyond EOF: the last file page is index 2; pgoff 3 answers
	 * BUS (the do_read_fault() SIGBUS verdict through the arena's
	 * delivery), and the metadata keeps its FILE_MAPPED shape.
	 */
	fflags = 0;
	KUNIT_EXPECT_EQ(test,
			corten_arena_user_fault(mm,
						CORTEN_ARENA_TEST_WIN +
						3 * PAGE_SIZE, 0, NULL,
						&fflags),
			CORTEN_FAULT_BUS);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm,
					       CORTEN_ARENA_TEST_WIN +
					       3 * PAGE_SIZE, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_FILE_MAPPED);
	/* No stray translation and no accounting for the refused page. */
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, filepg_family),
			filepg + 2);

	/* The zap side of the ledger: teardown drops both file pages
	 * and the registry stays INV-MV3 clean.
	 */
	mmap_read_lock(mm);
	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(mm));
	mmap_read_unlock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, filepg_family), filepg);
	KUNIT_EXPECT_EQ(test, file_count(file), base);

	fput(file);
}

/* ------------------------------------------------------------------ *
 * V-B.3: the FILE_MAPPED write arm -- the private-copy COW.  First
 * write on a read-installed page (the in-place shape) and on a
 * never-faulted slot (the do_cow_fault() fetch+copy shape) both
 * migrate the metadata FILE_MAPPED -> MAPPED, hand out an exclusive
 * private page with the file's content, and leave the pagecache folio
 * (the "file side" of the isolation) untouched; a second write runs
 * the reuse arm on the now-anon page.
 * ------------------------------------------------------------------
 */
static void corten_arena_test_file_cow(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct address_space *mapping;
	struct corten_pte_meta m;
	struct file *file;
	struct folio *folio;
	pmd_t *pmdp;
	pte_t *ptep, pte;
	spinlock_t *ptl;	/* guards the COW install reads */
	unsigned int fflags;
	long base, filepg, anonpg, routes;
	int filepg_family;
	u64 pat0 = 0xdeadbeefcafe1234ULL, pat1 = 0x0123456789abcdefULL;
	u64 back = 0, wr = 0xf00df00ddeadbeefULL;
	loff_t pos = 0;

	if (!corten_enabled_static())
		kunit_skip(test, "FILE COW arm requires corten=on");

	file = shmem_file_setup("corten_vb3c", 2 * PAGE_SIZE, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	mapping = file->f_mapping;
	base = file_count(file);
	KUNIT_ASSERT_EQ(test,
			kernel_write(file, &pat0, sizeof(pat0), &pos),
			(ssize_t)sizeof(pat0));
	pos = PAGE_SIZE;
	KUNIT_ASSERT_EQ(test,
			kernel_write(file, &pat1, sizeof(pat1), &pos),
			(ssize_t)sizeof(pat1));

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_file_attach_prot(mm,
							   CORTEN_ARENA_TEST_WIN,
							   file, 0,
							   PROT_READ |
							   PROT_WRITE), 0);
	filepg_family = corten_arena_test_file_rss(mapping);
	filepg = corten_arena_test_mm_counter(mm, filepg_family);
	anonpg = corten_arena_test_mm_counter(mm, MM_ANONPAGES);

	/* Read-install pgoff 0 first (the in-place COW needs the read
	 * arm's translation in place).  The install is read-only even
	 * though the contract carries WRITE (PAGE_COPY: the private COW
	 * shape -- the write must come through the fault).
	 */
	fflags = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_user_fault(mm, CORTEN_ARENA_TEST_WIN,
						0, NULL, &fflags),
			CORTEN_FAULT_HANDLED);
	pmdp = corten_arena_test_pmd(mm, CORTEN_ARENA_TEST_WIN);
	KUNIT_ASSERT_NOT_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp, CORTEN_ARENA_TEST_WIN, &ptl);
	KUNIT_ASSERT_NOT_NULL(test, ptep);
	pte = ptep_get(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));
	KUNIT_EXPECT_FALSE(test, pte_write(pte));
	pte_unmap_unlock(ptep, ptl);
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, filepg_family),
			filepg + 1);
	folio = filemap_get_folio(mapping, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(folio));
	folio_put(folio);

	/* First write on the read-installed page: the COW copy. */
	fflags = FAULT_FLAG_WRITE;
	KUNIT_EXPECT_EQ(test,
			corten_arena_user_fault(mm, CORTEN_ARENA_TEST_WIN,
						0, NULL, &fflags),
			CORTEN_FAULT_HANDLED);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_WIN,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_PERM_READ | CORTEN_PERM_WRITE |
				       CORTEN_PERM_USER);
	/* The counters split: one anon page in, the file page out. */
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, MM_ANONPAGES),
			anonpg + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, filepg_family), filepg);
	/* The private copy carries the file's content... */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(mm,
						    CORTEN_ARENA_TEST_WIN,
						    &back, false), 0);
	KUNIT_EXPECT_EQ(test, back, pat0);
	/* ...and the file side never saw the write: a later store
	 * through the private page leaves the pagecache folio's content
	 * and mapcount untouched (the isolation).
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(mm,
						    CORTEN_ARENA_TEST_WIN,
						    &wr, true), 0);
	folio = filemap_get_folio(mapping, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(folio));
	{
		u64 *kaddr = kmap_local_folio(folio, 0);

		back = *kaddr;
		kunmap_local(kaddr);
	}
	KUNIT_EXPECT_EQ(test, back, pat0);
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 0);
	folio_put(folio);

	/* Second write on the private page: the reuse arm (anon now,
	 * exclusive, mapcount 1) -- handled, still MAPPED, content
	 * readable back through the same translation.
	 */
	fflags = FAULT_FLAG_WRITE;
	KUNIT_EXPECT_EQ(test,
			corten_arena_user_fault(mm, CORTEN_ARENA_TEST_WIN,
						0, NULL, &fflags),
			CORTEN_FAULT_HANDLED);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_WIN,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	back = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(mm,
						    CORTEN_ARENA_TEST_WIN,
						    &back, false), 0);
	KUNIT_EXPECT_EQ(test, back, wr);
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, MM_ANONPAGES),
			anonpg + 1);

	/* First write on a NEVER-faulted slot (the do_cow_fault()
	 * fetch+copy shape): fetch outside the lock, copy, one
	 * transaction.
	 */
	fflags = FAULT_FLAG_WRITE;
	KUNIT_EXPECT_EQ(test,
			corten_arena_user_fault(mm,
						CORTEN_ARENA_TEST_WIN + PAGE_SIZE,
						0, NULL, &fflags),
			CORTEN_FAULT_HANDLED);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm,
					       CORTEN_ARENA_TEST_WIN + PAGE_SIZE,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, MM_ANONPAGES),
			anonpg + 2);
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, filepg_family), filepg);
	back = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(mm,
						    CORTEN_ARENA_TEST_WIN +
						    PAGE_SIZE, &back, false),
			0);
	KUNIT_EXPECT_EQ(test, back, pat1);
	/* The file side is untouched here too. */
	folio = filemap_get_folio(mapping, 1);
	KUNIT_ASSERT_FALSE(test, IS_ERR(folio));
	{
		u64 *kaddr = kmap_local_folio(folio, 0);

		back = *kaddr;
		kunmap_local(kaddr);
	}
	KUNIT_EXPECT_EQ(test, back, pat1);
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 0);
	folio_put(folio);

	/* A write fault past EOF on a writable FILE contract: the
	 * do_cow_fault() SIGBUS verdict (BUS, not a zeroed private
	 * page -- the silent-corruption red line).
	 */
	fflags = FAULT_FLAG_WRITE;
	KUNIT_EXPECT_EQ(test,
			corten_arena_user_fault(mm,
						CORTEN_ARENA_TEST_WIN +
						2 * PAGE_SIZE, 0, NULL,
						&fflags),
			CORTEN_FAULT_BUS);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm,
					       CORTEN_ARENA_TEST_WIN +
					       2 * PAGE_SIZE, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_FILE_MAPPED);
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, MM_ANONPAGES),
			anonpg + 2);

	/* Read-only contract: a write on the R-only region answers
	 * ACCERR (the access_error() verdict).
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_file_attach(mm,
						      CORTEN_ARENA_TEST_START2,
						      file, 0), 0);
	fflags = FAULT_FLAG_WRITE;
	KUNIT_EXPECT_EQ(test,
			corten_arena_user_fault(mm,
						CORTEN_ARENA_TEST_START2,
						0, NULL, &fflags),
			CORTEN_FAULT_ACCERR);

	/* V-B.3 (H7 follow-through): the invalidation family (!even_cows,
	 * the reclaim arm) spares the private copies -- a file-driven
	 * unmap_mapping_pages over pgoff 0 leaves the COWed MAPPED slot
	 * and its content alone (should_zap_cows()'s verdict).  Both
	 * regions' carriers sit on pgoff 0, so the event routes twice;
	 * START2's FILE_MAPPED virtual allocation demotes (that side is
	 * the file's business), WIN's private copy survives verbatim.
	 */
	routes = corten_arena_test_truncate_routes();
	unmap_mapping_pages(mapping, 0, 1, false);
	KUNIT_EXPECT_EQ(test, corten_arena_test_truncate_routes(),
			routes + 2);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_WIN,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	back = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(mm,
						    CORTEN_ARENA_TEST_WIN,
						    &back, false), 0);
	KUNIT_EXPECT_EQ(test, back, wr);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_START2,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_PERM_READ | CORTEN_PERM_EXEC |
				       CORTEN_PERM_USER);

	mmap_read_lock(mm);
	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(mm));
	mmap_read_unlock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, MM_ANONPAGES), anonpg);
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, filepg_family), filepg);
	KUNIT_EXPECT_EQ(test, file_count(file), base);

	fput(file);
}

/* ------------------------------------------------------------------ *
 * V-B.4: the FILE fork mirror -- the parent/child reconciliation the
 * faithful-fork contract promises for a FILE region: the child's own
 * file reference and pgoff (INV-MV3(d) on both sides), both carriers
 * on the mapping's i_mmap, the shared pagecache folio behind both PTEs
 * (mapcount/refcount differential: +1 mapper, +1 PTE reference), the
 * child's MM_FILEPAGES accounting, and the V-B.4 observability ledger
 * (file_mmaps / file_read_faults / file_fork_mirrors through the
 * rendered debugfs counters).
 * ------------------------------------------------------------------
 */
static void corten_arena_test_file_fork_mirror(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm, *child;
	struct vm_area_struct *carrier, *ccarrier, *ivma;
	struct address_space *mapping;
	struct corten_pte_meta m;
	struct corten_arena *ar;
	struct file *file;
	struct folio *folio;
	pmd_t *pmdp;
	pte_t *ptep, pte;
	spinlock_t *ptl;	/* guards the shared-PTE reads */
	unsigned int fflags;
	const unsigned long pgoff = 3;
	unsigned long addr = CORTEN_ARENA_TEST_WIN;
	unsigned long folio_pfn, ref, maps = 0;
	int filepg_family;
	long base, mmaps, reads, mirrors, timeouts;
	u64 pat = 0xa11cea5e7712c0deULL, back = 0;
	loff_t pos = (loff_t)pgoff << PAGE_SHIFT;

	if (!corten_enabled_static())
		kunit_skip(test, "FILE fork mirror requires corten=on");

	file = shmem_file_setup("corten_vb4m", 2 * PMD_SIZE, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	mapping = file->f_mapping;
	base = file_count(file);
	timeouts = corten_arena_test_drain_timeouts();
	mmaps = corten_arena_test_named_counter(test, "file_mmaps");
	reads = corten_arena_test_named_counter(test, "file_read_faults");
	mirrors = corten_arena_test_named_counter(test, "file_fork_mirrors");
	KUNIT_ASSERT_GE(test, mmaps, 0);
	KUNIT_ASSERT_GE(test, reads, 0);
	KUNIT_ASSERT_GE(test, mirrors, 0);
	KUNIT_ASSERT_EQ(test,
			kernel_write(file, &pat, sizeof(pat), &pos),
			(ssize_t)sizeof(pat));

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_file_attach_prot(mm, addr, file,
							   pgoff,
							   PROT_READ |
							   PROT_WRITE), 0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_named_counter(test, "file_mmaps"),
			mmaps + 1);
	carrier = corten_arena_test_carrier_of(mm, addr);
	KUNIT_ASSERT_NOT_NULL(test, carrier);

	/* The resident page: the read arm's install on the pagecache
	 * folio (mapcount 1, the parent's PTE the only mapper).  The
	 * filemap_get_folio() reference held for the audit keeps the
	 * folio addressable across the child's exit.
	 */
	fflags = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_user_fault(mm, addr, 0, NULL, &fflags),
			CORTEN_FAULT_HANDLED);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_named_counter(test,
							"file_read_faults"),
			reads + 1);
	folio = filemap_get_folio(mapping, pgoff);
	KUNIT_ASSERT_FALSE(test, IS_ERR(folio));
	ref = folio_ref_count(folio);
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 1);
	folio_pfn = page_to_pfn(folio_file_page(folio, pgoff));

	pmdp = corten_arena_test_pmd(mm, addr);
	KUNIT_ASSERT_NOT_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	KUNIT_ASSERT_NOT_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap_unlock(ptep, ptl);
	KUNIT_EXPECT_TRUE(test, pte_present(pte) && !pte_write(pte));
	KUNIT_EXPECT_EQ(test, pte_pfn(pte), folio_pfn);

	/* Fork: register_child mirrors the payload, copy_page_range rides
	 * the carrier pair, the window pass replays the metadata.
	 */
	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);

	/* The reference ledger: the child's own rfile, one more PTE
	 * reference on the shared folio, one more mapper.
	 */
	KUNIT_EXPECT_EQ(test, file_count(file), base + 2);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_named_counter(test,
							"file_fork_mirrors"),
			mirrors + 1);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), ref + 1);
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 2);
	filepg_family = corten_arena_test_file_rss(mapping);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_mm_counter(child, filepg_family), 1);

	/* The child's record: rclass FILE, its own rfile reference to the
	 * same object, the same pgoff (INV-MV3(d) both sides).
	 */
	rcu_read_lock();
	ar = xa_load(&child->corten_state->arenas, addr >> PMD_SHIFT);
	KUNIT_ASSERT_NOT_NULL(test, ar);
	KUNIT_EXPECT_EQ(test, ar->rclass, CORTEN_REGION_FILE);
	KUNIT_EXPECT_PTR_EQ(test, ar->rfile, file);
	KUNIT_EXPECT_EQ(test, ar->rpoff, pgoff);
	rcu_read_unlock();
	ccarrier = corten_arena_test_carrier_of(child, addr);
	KUNIT_ASSERT_NOT_NULL(test, ccarrier);
	KUNIT_EXPECT_PTR_EQ(test, ccarrier->vm_file, file);
	KUNIT_EXPECT_EQ(test, ccarrier->vm_pgoff, pgoff);
	mmap_read_lock(child);
	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(child));
	mmap_read_unlock(child);

	/* Both carriers are i_mmap members of the same pgoff (the fork
	 * duplication of __vma_link_file's pairing).
	 */
	i_mmap_lock_read(mapping);
	vma_interval_tree_foreach(ivma, &mapping->i_mmap, pgoff, pgoff) {
		maps++;
		if (ivma == carrier || ivma == ccarrier)
			continue;
		maps += 100;		/* a foreign member: loud */
	}
	i_mmap_unlock_read(mapping);
	KUNIT_EXPECT_EQ(test, maps, 2);

	/* The child's slot and translation: the same virtual allocation
	 * (FILE_MAPPED at the recorded perm), the same folio read-only.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_FILE_MAPPED);
	KUNIT_EXPECT_EQ(test, m.perm, CORTEN_PERM_READ | CORTEN_PERM_WRITE |
				       CORTEN_PERM_USER);
	pmdp = corten_arena_test_pmd(child, addr);
	KUNIT_ASSERT_NOT_NULL(test, pmdp);
	ptep = pte_offset_map_lock(child, pmdp, addr, &ptl);
	KUNIT_ASSERT_NOT_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap_unlock(ptep, ptl);
	KUNIT_EXPECT_TRUE(test, pte_present(pte) && !pte_write(pte));
	KUNIT_EXPECT_EQ(test, pte_pfn(pte), folio_pfn);

	/* Both sides serve the file's content. */
	back = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(mm, addr, &back, false),
			0);
	KUNIT_EXPECT_EQ(test, back, pat);
	back = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(child, addr, &back,
						    false), 0);
	KUNIT_EXPECT_EQ(test, back, pat);

	/* INV7 closure on both sides (FILE_MAPPED slots are outside the
	 * walk's scope; nothing MAPPED exists yet -- the anchor that the
	 * mirror itself drifts nothing).
	 */
	{
		long violated = 0, checked = 0;

		corten_arena_test_inv7_walk(mm, &violated, &checked);
		KUNIT_EXPECT_EQ(test, violated, 0);
		corten_arena_test_inv7_walk(child, &violated, &checked);
		KUNIT_EXPECT_EQ(test, violated, 0);
	}

	/* The child's exit returns its mirror reference and its PTE's
	 * hold on the folio; the parent's shape survives verbatim.
	 */
	mmput(child);
	KUNIT_EXPECT_EQ(test, file_count(file), base + 1);
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 1);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), ref);
	folio_put(folio);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
	KUNIT_EXPECT_EQ(test, file_count(file), base);
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);

	fput(file);
}

/* ------------------------------------------------------------------ *
 * V-B.4: the FILE fork COW isolation -- after the mirror, the parent's
 * and the child's first writes each take the private copy (a file page
 * COW is a copy to a private anon page, never a write through), and
 * the three sides stay distinguishable: the parent's private word, the
 * child's private word, and the pagecache content (never touched by
 * either write).  The never-faulted slot covers the child's
 * do_cow_fault() shape (fetch + copy), with the parent's later read
 * still serving the file's content off the untouched folio.
 * ------------------------------------------------------------------
 */
static void corten_arena_test_file_fork_cow(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm, *child;
	struct address_space *mapping;
	struct corten_pte_meta m;
	struct file *file;
	struct folio *folio;
	unsigned int fflags;
	long base, pfilepg, panonpg, cfilepg, canonpg, cows;
	int filepg_family;
	u64 pat0 = 0x5eed5eed5eed5eedULL, pat1 = 0x1badb002cafebeefULL;
	u64 pw = 0x1111111122222222ULL, cw = 0x3333333344444444ULL;
	u64 cw2 = 0x5555555566666666ULL, back = 0;
	loff_t pos = 0;

	if (!corten_enabled_static())
		kunit_skip(test, "FILE fork COW requires corten=on");

	file = shmem_file_setup("corten_vb4c", 2 * PAGE_SIZE, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	mapping = file->f_mapping;
	base = file_count(file);
	KUNIT_ASSERT_EQ(test,
			kernel_write(file, &pat0, sizeof(pat0), &pos),
			(ssize_t)sizeof(pat0));
	pos = PAGE_SIZE;
	KUNIT_ASSERT_EQ(test,
			kernel_write(file, &pat1, sizeof(pat1), &pos),
			(ssize_t)sizeof(pat1));
	cows = corten_arena_test_named_counter(test, "file_cow_copies");
	KUNIT_ASSERT_GE(test, cows, 0);

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_file_attach_prot(mm,
							   CORTEN_ARENA_TEST_WIN,
							   file, 0,
							   PROT_READ |
							   PROT_WRITE), 0);
	filepg_family = corten_arena_test_file_rss(mapping);
	pfilepg = corten_arena_test_mm_counter(mm, filepg_family);
	panonpg = corten_arena_test_mm_counter(mm, MM_ANONPAGES);

	/* Read-install page 0 (the shared base line), then fork. */
	fflags = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_user_fault(mm, CORTEN_ARENA_TEST_WIN,
						0, NULL, &fflags),
			CORTEN_FAULT_HANDLED);
	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);
	cfilepg = corten_arena_test_mm_counter(child, filepg_family);
	canonpg = corten_arena_test_mm_counter(child, MM_ANONPAGES);
	folio = filemap_get_folio(mapping, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(folio));

	/* The parent's first write: the private copy off the shared folio
	 * (the in-place cow_write file branch).
	 */
	fflags = FAULT_FLAG_WRITE;
	KUNIT_ASSERT_EQ(test,
			corten_arena_user_fault(mm, CORTEN_ARENA_TEST_WIN,
						0, NULL, &fflags),
			CORTEN_FAULT_HANDLED);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_named_counter(test,
							"file_cow_copies"),
			cows + 1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_WIN,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, MM_ANONPAGES),
			panonpg + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_mm_counter(mm, filepg_family),
			pfilepg);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(mm, CORTEN_ARENA_TEST_WIN,
						    &pw, true), 0);
	/* The child's mapper is the folio's only survivor: the parent's
	 * copy left it behind.
	 */
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 1);

	/* The child's first write on the same page: its own private copy,
	 * a different word -- the isolation anchor.
	 */
	fflags = FAULT_FLAG_WRITE;
	KUNIT_ASSERT_EQ(test,
			corten_arena_user_fault(child, CORTEN_ARENA_TEST_WIN,
						0, NULL, &fflags),
			CORTEN_FAULT_HANDLED);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_named_counter(test,
							"file_cow_copies"),
			cows + 2);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(child,
					       CORTEN_ARENA_TEST_WIN, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_mm_counter(child, MM_ANONPAGES),
			canonpg + 1);
	/* The child's only file page went private: its file-family count
	 * drops below the fork baseline (the shared page it replaced).
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_mm_counter(child, filepg_family),
			cfilepg - 1);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(child,
						    CORTEN_ARENA_TEST_WIN,
						    &cw, true), 0);
	KUNIT_EXPECT_EQ(test, folio_mapcount(folio), 0);

	/* The three-way reconciliation on page 0: each side reads its own
	 * word back, and the pagecache content is exactly the file's.
	 */
	back = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(mm, CORTEN_ARENA_TEST_WIN,
						    &back, false), 0);
	KUNIT_EXPECT_EQ(test, back, pw);
	back = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(child,
						    CORTEN_ARENA_TEST_WIN,
						    &back, false), 0);
	KUNIT_EXPECT_EQ(test, back, cw);
	{
		u64 *kaddr = kmap_local_folio(folio, 0);

		back = *kaddr;
		kunmap_local(kaddr);
	}
	KUNIT_EXPECT_EQ(test, back, pat0);
	folio_put(folio);

	/* The child's first write on the never-faulted slot (the
	 * do_cow_fault() fetch+copy shape): its private copy of page 1's
	 * content; the parent's later read still serves the file's page
	 * off the pagecache (the fork never made the file side private).
	 */
	fflags = FAULT_FLAG_WRITE;
	KUNIT_ASSERT_EQ(test,
			corten_arena_user_fault(child,
						CORTEN_ARENA_TEST_WIN +
						PAGE_SIZE, 0, NULL, &fflags),
			CORTEN_FAULT_HANDLED);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_named_counter(test,
							"file_cow_copies"),
			cows + 3);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(child,
						    CORTEN_ARENA_TEST_WIN +
						    PAGE_SIZE, &cw2, true), 0);
	fflags = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_user_fault(mm, CORTEN_ARENA_TEST_WIN +
						PAGE_SIZE, 0, NULL, &fflags),
			CORTEN_FAULT_HANDLED);
	back = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(mm,
						    CORTEN_ARENA_TEST_WIN +
						    PAGE_SIZE, &back, false),
			0);
	KUNIT_EXPECT_EQ(test, back, pat1);
	back = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(child,
						    CORTEN_ARENA_TEST_WIN +
						    PAGE_SIZE, &back, false),
			0);
	KUNIT_EXPECT_EQ(test, back, cw2);

	mmap_read_lock(mm);
	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(mm));
	mmap_read_unlock(mm);
	mmap_read_lock(child);
	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(child));
	mmap_read_unlock(child);

	/* Teardown: the child first (its mirror reference and its private
	 * copies), then the parent -- the ledger returns to base.
	 */
	mmput(child);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
	KUNIT_EXPECT_EQ(test, file_count(file), base);

	fput(file);
}

/* ------------------------------------------------------------------ *
 * V-B.4: the pinned fork copy path -- a GUP-pinned private page (the
 * FILE region's COW copy) is copied for the child, not shared
 * (copy_present_pte()'s folio_try_dup_anon_rmap_pte() refusal ->
 * copy_present_page()): the child's PTE lands on a different, exclusive
 * page carrying the content, the parent's stays writable on the pinned
 * original, and NEITHER side records SHARED behind a writable PTE (the
 * INV7-drift shape the mark_window's pinned-aware refinement closes).
 * The 6.18 base shares pagecache folios unconditionally at fork
 * (folio_dup_file_rmap_pte() has no failure path), so the pinned-copy
 * arm is reachable only through the COW'd private copy -- exactly this
 * shape.
 * ------------------------------------------------------------------
 */
static void corten_arena_test_file_fork_pinned(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm, *child;
	struct address_space *mapping;
	struct corten_pte_meta m;
	struct file *file;
	struct page *ppage, *cpage;
	pmd_t *pmdp;
	pte_t *ptep, pte;
	spinlock_t *ptl;	/* guards the pinned-shape PTE reads */
	unsigned int fflags;
	unsigned long addr = CORTEN_ARENA_TEST_WIN;
	unsigned long ppfn;
	long base, timeouts;
	u64 pat = 0x0ff1e1e5c04d1701ULL, back = 0;
	loff_t pos = 0;

	if (!corten_enabled_static())
		kunit_skip(test, "FILE fork pinned copy requires corten=on");

	file = shmem_file_setup("corten_vb4p", PAGE_SIZE, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	mapping = file->f_mapping;
	base = file_count(file);
	timeouts = corten_arena_test_drain_timeouts();
	KUNIT_ASSERT_EQ(test,
			kernel_write(file, &pat, sizeof(pat), &pos),
			(ssize_t)sizeof(pat));

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_file_attach_prot(mm, addr, file, 0,
							   PROT_READ |
							   PROT_WRITE), 0);

	/* The private copy: a write fault COWs the file page into an
	 * exclusive anon page (the pinned-copy arm's only reachable base).
	 */
	fflags = FAULT_FLAG_WRITE;
	KUNIT_ASSERT_EQ(test,
			corten_arena_user_fault(mm, addr, 0, NULL, &fflags),
			CORTEN_FAULT_HANDLED);
	pmdp = corten_arena_test_pmd(mm, addr);
	KUNIT_ASSERT_NOT_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	KUNIT_ASSERT_NOT_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap_unlock(ptep, ptl);
	KUNIT_ASSERT_TRUE(test, pte_present(pte) && pte_write(pte));
	ppage = pte_page(pte);
	ppfn = page_to_pfn(ppage);
	KUNIT_ASSERT_TRUE(test, PageAnonExclusive(ppage));

	/* The simulated FOLL_PIN (the slow-path pin shape on an order-0
	 * folio: bias worth of plain refs) -- the gup state4 convention,
	 * plus the mm's MMF_HAS_PINNED mark a real pin_page_*() leaves
	 * behind (folio_needs_cow_for_dma()'s fast gate -- without it the
	 * dup never consults the pin).
	 */
	mm_flags_set(MMF_HAS_PINNED, mm);
	folio_get(page_folio(ppage));
	folio_ref_add(page_folio(ppage), GUP_PIN_COUNTING_BIAS);
	KUNIT_EXPECT_TRUE(test, folio_maybe_dma_pinned(page_folio(ppage)));

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);

	/* The child got the copy: a different page, exclusive, writable,
	 * carrying the content.
	 */
	pmdp = corten_arena_test_pmd(child, addr);
	KUNIT_ASSERT_NOT_NULL(test, pmdp);
	ptep = pte_offset_map_lock(child, pmdp, addr, &ptl);
	KUNIT_ASSERT_NOT_NULL(test, ptep);
	pte = ptep_get(ptep);
	cpage = pte_page(pte);
	pte_unmap_unlock(ptep, ptl);
	KUNIT_EXPECT_TRUE(test, pte_present(pte) && pte_write(pte));
	KUNIT_EXPECT_NE(test, page_to_pfn(cpage), ppfn);
	KUNIT_EXPECT_TRUE(test, PageAnonExclusive(cpage));
	back = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(child, addr, &back,
						    false), 0);
	KUNIT_EXPECT_EQ(test, back, pat);

	/* The parent keeps the pinned original verbatim: still writable
	 * (copy_present_page() never wrprotects a page the child does not
	 * map), still exclusive (the GUP-pin contract), sole mapper.
	 */
	pmdp = corten_arena_test_pmd(mm, addr);
	KUNIT_ASSERT_NOT_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	KUNIT_ASSERT_NOT_NULL(test, ptep);
	pte = ptep_get(ptep);
	pte_unmap_unlock(ptep, ptl);
	KUNIT_EXPECT_TRUE(test, pte_present(pte) && pte_write(pte));
	KUNIT_EXPECT_EQ(test, pte_pfn(pte), ppfn);
	KUNIT_EXPECT_TRUE(test, PageAnonExclusive(ppage));
	KUNIT_EXPECT_EQ(test, folio_mapcount(page_folio(ppage)), 1);

	/* The V-B.4 metadata shape: neither side records SHARED behind a
	 * writable PTE -- the parent's mark and the child's replay both
	 * carry the WRITABLE record only (INV7-clean pinned fork).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, m.flags, CORTEN_PF_WRITABLE);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, m.flags, CORTEN_PF_WRITABLE);
	{
		long violated = 0, checked = 0;

		/* The walk examines only MAPPED+SHARED slots: the pinned
		 * fork leaves none on either side (without the mark_window
		 * refinement each side would show checked==1, violated==1
		 * -- a SHARED record behind a writable PTE).
		 */
		corten_arena_test_inv7_walk(mm, &violated, &checked);
		KUNIT_EXPECT_EQ(test, violated, 0);
		KUNIT_EXPECT_EQ(test, checked, 0);
		corten_arena_test_inv7_walk(child, &violated, &checked);
		KUNIT_EXPECT_EQ(test, violated, 0);
		KUNIT_EXPECT_EQ(test, checked, 0);
	}

	/* Unpin and tear both sides down: the ledger returns clean.
	 * (MMF_HAS_PINNED is documented "never cleared" -- the test mm
	 * dies with the mark, exactly like a real GUP process.)
	 */
	folio_ref_sub(page_folio(ppage), GUP_PIN_COUNTING_BIAS);
	KUNIT_EXPECT_FALSE(test, folio_maybe_dma_pinned(page_folio(ppage)));
	folio_put(page_folio(ppage));

	mmput(child);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
	KUNIT_EXPECT_EQ(test, file_count(file), base);
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);

	fput(file);
}

/* ------------------------------------------------------------------ *
 * V-B.4: the hugetlbfs fd gate at the route -- an explicitly opened
 * hugetlbfs fd (no MAP_HUGETLB bit for the classify whitelist to
 * reject, the B.3 disclosure item 6) is refused by corten_file_may()
 * and the takeover degrades to the legacy flow (ret 0, request
 * untouched, no window consumed, no FILE ledger movement).
 * ------------------------------------------------------------------
 */
static int corten_arena_test_hugepages_mmap(struct file *file,
					    struct vm_area_struct *vma)
{
	return 0;	/* never called: the route refuses before attach */
}

static const struct file_operations corten_arena_test_hugepages_fops = {
	.mmap = corten_arena_test_hugepages_mmap,
	.fop_flags = FOP_HUGE_PAGES,
};

static void corten_arena_test_file_hugetlb_route(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	/* Heap, not stack: a super_block alone blows the 2K frame budget
	 * (the file_may_matrix convention).
	 */
	struct inode *inode = kunit_kzalloc(test, sizeof(*inode), GFP_KERNEL);
	struct address_space *mapping =
		kunit_kzalloc(test, sizeof(*mapping), GFP_KERNEL);
	struct super_block *sb = kunit_kzalloc(test, sizeof(*sb), GFP_KERNEL);
	struct dentry *de = kunit_kzalloc(test, sizeof(*de), GFP_KERNEL);
	struct vfsmount mnt = { .mnt_sb = sb, };
	struct path path = { .mnt = &mnt, .dentry = de, };
	struct file f = { .f_path = path, };
	unsigned long addr = 0, lenp = 2 * PAGE_SIZE;
	unsigned long flags;
	long mmaps;

	if (!corten_enabled_static())
		kunit_skip(test, "hugetlb route gate requires corten=on");

	KUNIT_ASSERT_NOT_NULL(test, inode);
	KUNIT_ASSERT_NOT_NULL(test, mapping);
	KUNIT_ASSERT_NOT_NULL(test, sb);
	KUNIT_ASSERT_NOT_NULL(test, de);

	/* The fabricated hugetlbfs shape: a regular-file inode whose
	 * f_op carries FOP_HUGE_PAGES -- is_file_hugepages()'s exact
	 * discriminator (hugetlbfs_file_operation's only marking).
	 */
	inode->i_mode = S_IFREG;
	inode->i_mapping = mapping;
	mapping->host = inode;
	de->d_inode = inode;
	f.f_inode = inode;
	f.f_mapping = mapping;
	f.f_mode = FMODE_READ | FMODE_WRITE;
	f.f_op = &corten_arena_test_hugepages_fops;

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	mmaps = corten_arena_test_named_counter(test, "file_mmaps");
	KUNIT_ASSERT_GE(test, mmaps, 0);

	/* The route's verdict: not ours (legacy serves the fd through
	 * hugetlbfs_mmap()); the request is handed back untouched.
	 */
	flags = MAP_PRIVATE;
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_file_route(mm, &f, lenp, &addr,
						     &lenp, &flags), 0);
	KUNIT_EXPECT_EQ(test, addr, 0);
	KUNIT_EXPECT_EQ(test, lenp, 2 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, flags, MAP_PRIVATE);
	KUNIT_EXPECT_NULL(test,
			  corten_arena_test_carrier_of(mm,
						       CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_named_counter(test, "file_mmaps"),
			mmaps);

	/* A plain file still takes the window afterwards (the refusal
	 * consumed nothing).
	 */
	{
		struct file *file = shmem_file_setup("corten_vb4h", PMD_SIZE, 0);

		KUNIT_ASSERT_FALSE(test, IS_ERR(file));
		KUNIT_ASSERT_EQ(test,
				corten_arena_test_file_attach(mm,
							      CORTEN_ARENA_TEST_WIN,
							      file, 0), 0);
		KUNIT_EXPECT_EQ(test,
				corten_arena_test_named_counter(test,
								"file_mmaps"),
				mmaps + 1);
		KUNIT_EXPECT_EQ(test,
				corten_arena_test_run_op(test, mm,
							 corten_arena_test_op_mode_exit,
							 0, 0), 0);
		fput(file);
	}
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
		/* V-A.2b: an auto arena's rmap anchor is its detached
		 * carrier -- seed onto it, exactly like map_anon().
		 */
		vma = corten_arena_test_carrier_of(mm, addr);
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

/* kmap-based page word access: the definition lives with the fork
 * battery below; the mremap route test needs it first.
 */
static int corten_arena_test_page_word(struct mm_struct *mm,
					unsigned long addr, u64 *val,
					bool write);

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
	u64 pattern = 0x3154424D4F564554ULL;	/* "T0BMOVE1"-shaped */
	u64 back = 0;
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
	/* V-A.2a: the auto takeover is VMA-less and the mremap move
	 * target is a direct VMA-less declare -- no takeover VMA either.
	 */
	mmap_write_lock(mm);
	ret = corten_arena_auto_attach(mm, CORTEN_ARENA_TEST_WIN,
				       CORTEN_ARENA_TEST_WIN_LEN,
				       PROT_READ | PROT_WRITE);
	mmap_write_unlock(mm);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* V-A.2a: the window is VMA-less -- a copy_to_user() in the op
	 * thread cannot fault it in (the x86 arena hook gates on
	 * user_mode(regs)); the content runs through the arena fault
	 * entry + the kernel-side word helper, and the move's own
	 * prefault leg carries the copy.
	 */
	{
		unsigned int fault_flags = FAULT_FLAG_WRITE;

		KUNIT_ASSERT_EQ(test,
				corten_arena_user_fault(mm,
							CORTEN_ARENA_TEST_WIN,
							0, NULL,
							&fault_flags),
				CORTEN_FAULT_HANDLED);
	}
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(mm, CORTEN_ARENA_TEST_WIN,
						    &pattern, true), 0);

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
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_page_word(mm, new_addr, &back,
						    false), 0);
	KUNIT_EXPECT_EQ(test, back, pattern);

	/* The old arena is retired, the new one is a live arena. */
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_WIN),
			0);
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, new_addr), 1);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));
	/* V-A.2a: the move target carries no VMA either (the VA is kept
	 * by the frame table; the S-4 reading covers it).
	 */
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, new_addr));
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
	ret = corten_arena_auto_attach(mm, addr, len,
				       PROT_READ | PROT_WRITE);
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
	/* V-A.2a: VMA-less auto takeover -- no declaring VMA. */
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

	/* Parked: lookup-invisible, pool occupancy 1 -- and zero drain
	 * activity, the percpu_ref never died.  V-A.1 (S-4): the
	 * reservation VMA is gone; the window carries no tree VMA at
	 * all.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_WIN),
			0);
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_pool_idle(mm,
						      CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_parks(), parks + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 1);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);

	/* S-1: a fault on the parked window misses the lookup -- the hot
	 * hook falls back to the legacy funnel, which (no VMA any more)
	 * answers MAPERR, exactly like a real munmap.
	 */
	{
		unsigned int fault_flags = FAULT_FLAG_WRITE;

		KUNIT_EXPECT_EQ(test,
				corten_arena_user_fault(mm,
							CORTEN_ARENA_TEST_WIN,
							0, NULL,
							&fault_flags),
				CORTEN_FAULT_FALLBACK);
	}

	/* The pure reservation (sec 3.1.1): frames + idle descriptor,
	 * no VMA -- and no PT pages either; the park's VMA removal
	 * retired the window's tables.
	 */
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_test_pt_present(mm,
							CORTEN_ARENA_TEST_WIN));

	/* The park zap reset the committed page (perm-0 Invalid) and the
	 * VMA removal retired the window's PT pages: the metadata slot
	 * itself is gone, so the read answers -ENOENT.  Pristine by
	 * absence -- there is nothing left to resurrect a stale contract
	 * from.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_WIN,
					       &m), -ENOENT);

	/* The next auto mmap of the same size is served from the pool:
	 * ret 2 -- the pure-metadata reactivation IS the mapping, live
	 * before any do_mmap flow runs.
	 */
	addr = 0;
	lenp = PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	mmap_write_lock(mm);
	ret = corten_arena_auto_mmap_route(mm, NULL, 0, PAGE_SIZE,
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
	/* V-A.1: the reactivated window is VMA-less -- no shadow piece
	 * comes back; the mapping is the metadata domain alone.
	 */
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);

	/* The FRESH gate derives from the re-recorded ar->prot: a write
	 * fault on the never-touched page maps a fresh page purely from
	 * metadata (no VMA anywhere in the window).
	 */
	{
		unsigned int fault_flags = FAULT_FLAG_WRITE;

		KUNIT_EXPECT_EQ(test,
				corten_arena_user_fault(mm,
							CORTEN_ARENA_TEST_WIN,
							0, NULL,
							&fault_flags),
				CORTEN_FAULT_HANDLED);
	}
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(mm, CORTEN_ARENA_TEST_WIN,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_EQ(test, m.perm,
			CORTEN_PERM_USER | CORTEN_PERM_READ |
			CORTEN_PERM_WRITE);
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_pt_present(mm,
						       CORTEN_ARENA_TEST_WIN));

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
	ret = corten_arena_auto_mmap_route(mm, NULL, 0, 2 * PMD_SIZE,
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

		/* V-A.2a: VMA-less auto takeover (no declaring VMA). */
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
	ret = corten_arena_auto_mmap_route(mm, NULL, 0, PAGE_SIZE,
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
	unsigned long parked = CORTEN_ARENA_TEST_WIN;
	unsigned long live = CORTEN_ARENA_TEST_WIN + 4 * PMD_SIZE;
	long timeouts = corten_arena_test_drain_timeouts();

	if (!corten_enabled_static())
		kunit_skip(test, "pool routing requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* One parked and one live arena (V-A.2a: VMA-less attaches). */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, parked, PMD_SIZE),
			0);
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
	struct task_struct *tsk;
	struct mm_struct *child;
	long timeouts = corten_arena_test_drain_timeouts();

	if (!corten_enabled_static())
		kunit_skip(test, "pool routing requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* V-A.2a: VMA-less auto takeover -- no declaring VMA. */
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
 * V-A.3a: the placement surface (j2-audit #14-#17, the corruption-class
 * holes A.1 opened when the reservation VMA stopped being the window
 * domain's occupancy truth).  Every case drives the real do_mmap() funnel
 * -- the NOREPLACE gate, the mmap routing gate and the __mmap_prepare
 * gather -- on the attached op worker (mmap shapes run the full chain,
 * and the worker's current->mm is the mm under test).
 * ------------------------------------------------------------------
 */

/* One do_mmap() shape on the attached worker: takes the write lock the
 * syscall wrapper would, records the raw return (address or -errno).
 */
static void corten_arena_test_op_do_mmap(struct corten_arena_test_op *o)
{
	unsigned long populate;
	LIST_HEAD(uf);

	mmap_write_lock(o->mm);
	o->retl = (long)do_mmap(NULL, o->addr, o->len,
				PROT_READ | PROT_WRITE,
				o->flags | MAP_ANONYMOUS | MAP_PRIVATE,
				0, 0, &populate, &uf);
	mmap_write_unlock(o->mm);
}

/* One file MAP_FIXED punch shape on the attached worker: the real
 * do_mmap() funnel -- the punch route, the overlap gather and the
 * implant VMA install exactly as the guest's memfd MAP_FIXED runs it.
 */
static void corten_arena_test_op_punch(struct corten_arena_test_op *o)
{
	unsigned long populate;
	LIST_HEAD(uf);

	mmap_write_lock(o->mm);
	o->retl = (long)do_mmap(o->file, o->addr, o->len,
				PROT_READ | PROT_WRITE,
				MAP_FIXED | MAP_SHARED, 0, 0, &populate, &uf);
	mmap_write_unlock(o->mm);
}

static long corten_arena_test_punch(struct kunit *test, struct mm_struct *mm,
				    struct file *file, unsigned long addr,
				    unsigned long len)
{
	struct corten_arena_test_op o = {
		.mm = mm,
		.fn = corten_arena_test_op_punch,
		.file = file,
		.addr = addr,
		.len = len,
	};

	KUNIT_ASSERT_EQ(test, corten_arena_test_run_op_full(test, &o), 0);
	return o.retl;
}

static long corten_arena_test_vm_mmap(struct kunit *test, struct mm_struct *mm,
				      unsigned long addr, unsigned long len,
				      unsigned long flags)
{
	struct corten_arena_test_op o = {
		.mm = mm,
		.fn = corten_arena_test_op_do_mmap,
		.addr = addr,
		.len = len,
		.flags = flags,
	};

	KUNIT_ASSERT_EQ(test, corten_arena_test_run_op_full(test, &o), 0);
	return o.retl;
}

/* Line-length alias: the full placement-truth name defeats argument
 * alignment in the matrix below.
 */
static bool occupied_incl_idle(struct mm_struct *mm, unsigned long addr,
			       unsigned long len)
{
	return corten_arena_range_occupied_incl_idle(mm, addr, len);
}

/* Declare + park one targeted arena at the harness base (the low-domain
 * shape: outside every magazine segment, so an ejection erases the frames
 * instead of restoring markers -- the clean occupied()=false teardown).
 */
static void corten_arena_test_park_targeted(struct kunit *test,
					    struct mm_struct *mm)
{
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_BASE,
						 CORTEN_ARENA_TEST_LEN), 1);
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_pool_idle(mm,
						      CORTEN_ARENA_TEST_BASE));
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_BASE));
}

/* Audit #14: MAP_FIXED_NOREPLACE into a parked window must answer
 * -EEXIST (the contract's literal errno, unchanged from the
 * reservation-VMA era) although the range is tree-free -- only the
 * incl-idle registry probe can see the parked frames.  The rejection
 * changes nothing: no VMA appears, the window stays parked.
 */
static void corten_arena_test_noreplace_parked(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;

	if (!corten_enabled_static())
		kunit_skip(test, "placement guards require corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm,
						      CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_vm_mmap(test, mm,
						  CORTEN_ARENA_TEST_WIN,
						  PMD_SIZE,
						  MAP_FIXED_NOREPLACE),
			-EEXIST);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_TRUE(test,
			  occupied_incl_idle(mm, CORTEN_ARENA_TEST_WIN,
					     PMD_SIZE));
	/* The parked window is invisible to the live-state probe (the
	 * reject family's skip-idle meaning is unchanged) and stays in the
	 * pool for the next handout.
	 */
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_range_overlaps(mm,
						       CORTEN_ARENA_TEST_WIN,
						       PMD_SIZE));
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_pool_idle(mm,
						      CORTEN_ARENA_TEST_WIN));

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* A.2's guard regression anchor: a LIVE window is VMA-less post-A.2a --
 * the NOREPLACE gate must keep answering -EEXIST off the registry alone.
 */
static void corten_arena_test_noreplace_active(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;

	if (!corten_enabled_static())
		kunit_skip(test, "placement guards require corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm,
						      CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_vm_mmap(test, mm,
						  CORTEN_ARENA_TEST_WIN,
						  PMD_SIZE,
						  MAP_FIXED_NOREPLACE),
			-EEXIST);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_WIN),
			1);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* D24 (audit #14's plain-MAP_FIXED sibling): legacy MAP_FIXED is
 * *replace*, so a parked window under one is ejected-and-admitted, never
 * rejected -- the incoming VMA becomes a registered implant on freed VA,
 * and the ejected slot can never be handed out again.
 */
static void corten_arena_test_mapfixed_over_parked(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	long ejects = corten_arena_test_placement_idle_ejects();
	long misses = corten_arena_test_pool_misses();
	unsigned long addr = 0, lenp = PAGE_SIZE, flags;
	long r;
	int ret;

	if (!corten_enabled_static())
		kunit_skip(test, "placement guards require corten=on");

	/* The production shape: an in-window auto arena (magazine frames),
	 * parked by the free() munmap.
	 */
	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm,
						      CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_pool_idle(mm,
						      CORTEN_ARENA_TEST_WIN));

	/* The replace: admitted at the requested address, no new errno. */
	r = corten_arena_test_vm_mmap(test, mm, CORTEN_ARENA_TEST_WIN,
				      PMD_SIZE, MAP_FIXED);
	KUNIT_EXPECT_EQ(test, r, CORTEN_ARENA_TEST_WIN);
	KUNIT_EXPECT_NOT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));

	/* The window left the registry whole -- the erase-eject leaves no
	 * restored markers under the incoming VMA (the recycle-eject would
	 * re-mark frames the placement backstop then counts occupied and
	 * the magazine would later serve under the implant), so both
	 * probes read the freed range as unoccupied.
	 */
	KUNIT_EXPECT_FALSE(test,
			   occupied_incl_idle(mm, CORTEN_ARENA_TEST_WIN,
					      PMD_SIZE));
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_range_overlaps(mm,
						       CORTEN_ARENA_TEST_WIN,
						       PMD_SIZE));
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 0);
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_test_pool_idle(mm,
						       CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_EQ(test, corten_arena_test_placement_idle_ejects(),
			ejects + 1);

	/* The implant registry whitelists the placement (the J2 walker's
	 * legal shape, D24).
	 */
	KUNIT_EXPECT_TRUE(test,
			  corten_implant_covers(mm, CORTEN_ARENA_TEST_WIN,
						PMD_SIZE));
	KUNIT_EXPECT_GE(test, corten_arena_test_implant_nr(mm), 1);

	/* The next same-size auto request cannot hit the ejected window:
	 * a counted pool miss, fresh window placement elsewhere.
	 */
	flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	mmap_write_lock(mm);
	ret = corten_arena_auto_mmap_route(mm, NULL, 0, PMD_SIZE,
					   PROT_READ | PROT_WRITE, &addr,
					   &lenp, &flags);
	mmap_write_unlock(mm);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_NE(test, addr, CORTEN_ARENA_TEST_WIN);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_misses(), misses + 1);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* The occupancy matrix: occupied_incl_idle() mirrors range_overlaps()
 * on the live axis and flips on the idle and sentinel axes -- a parked
 * window and a claimed-but-unallocated magazine frame are both
 * spoken-for VA (a later reactivation / magazine serve would hand them
 * out), invisible to the live-state probe the reject family depends on.
 * The backstop predicate rings on live/parked frames (counter disclosed)
 * and ignores sentinels (the T0 obstacle contract's legal shape).
 */
static void corten_arena_test_occupied_incl_idle(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	unsigned long seg_tail = 0;
	long backstops = corten_arena_test_placement_backstop();

	if (!corten_enabled_static())
		kunit_skip(test, "placement guards require corten=on");

	/* Hole: nothing registered anywhere. */
	KUNIT_EXPECT_FALSE(test, occupied_incl_idle(mm, 0, TASK_SIZE));

	/* Live (targeted): both probes agree. */
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN), 0);
	KUNIT_EXPECT_TRUE(test,
			  occupied_incl_idle(mm, CORTEN_ARENA_TEST_BASE,
					     PAGE_SIZE));
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_range_overlaps(mm,
						      CORTEN_ARENA_TEST_BASE,
						      PAGE_SIZE));
	KUNIT_EXPECT_TRUE(test,
			  occupied_incl_idle(mm,
					     CORTEN_ARENA_TEST_BASE +
					     CORTEN_ARENA_TEST_LEN - PAGE_SIZE,
					     PAGE_SIZE));
	KUNIT_EXPECT_TRUE(test,
			  occupied_incl_idle(mm,
					     CORTEN_ARENA_TEST_BASE +
					     3 * PMD_SIZE,
					     2 * PAGE_SIZE));
	KUNIT_EXPECT_TRUE(test, occupied_incl_idle(mm, 0, TASK_SIZE));
	KUNIT_EXPECT_FALSE(test,
			   occupied_incl_idle(mm,
					      CORTEN_ARENA_TEST_BASE - PAGE_SIZE,
					      PAGE_SIZE));
	KUNIT_EXPECT_FALSE(test,
			   occupied_incl_idle(mm,
					      CORTEN_ARENA_TEST_BASE +
					      CORTEN_ARENA_TEST_LEN,
					      PAGE_SIZE));
	KUNIT_EXPECT_FALSE(test,
			   occupied_incl_idle(mm,
					      CORTEN_ARENA_TEST_NOWHERE,
					      PMD_SIZE));
	KUNIT_EXPECT_FALSE(test,
			   occupied_incl_idle(mm, CORTEN_ARENA_TEST_BASE,
					      0));

	/* Backstop predicate: true (and counted) on live frames, silent
	 * on the hole.
	 */
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_placement_backstop(mm,
							  CORTEN_ARENA_TEST_BASE,
							  PAGE_SIZE));
	KUNIT_EXPECT_EQ(test, corten_arena_test_placement_backstop(),
			backstops + 1);
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_placement_backstop(mm,
							   CORTEN_ARENA_TEST_NOWHERE,
							   PMD_SIZE));
	KUNIT_EXPECT_EQ(test, corten_arena_test_placement_backstop(),
			backstops + 1);

	/* Parked: the semantic split -- the reject family's probe stays
	 * false, the placement truth flips true.
	 */
	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_BASE,
						 CORTEN_ARENA_TEST_LEN), 1);
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_range_overlaps(mm,
						       CORTEN_ARENA_TEST_BASE,
						       PAGE_SIZE));
	KUNIT_EXPECT_TRUE(test,
			  occupied_incl_idle(mm, CORTEN_ARENA_TEST_BASE,
					     PAGE_SIZE));
	KUNIT_EXPECT_TRUE(test, occupied_incl_idle(mm, 0, TASK_SIZE));

	/* Sentinel: a real auto-route placement claims a whole 1GiB
	 * magazine segment and carves its first frames -- the claim's
	 * un-allocated tail is reserve markers: occupied VA to the
	 * placement truth, invisible to the live probe, and legal legacy
	 * terrain to the backstop (the magazine re-verifies marker + tree
	 * at every serve, the T0 obstacle contract).
	 */
	{
		unsigned long a2 = 0, len2 = PAGE_SIZE;
		unsigned long flags2 = MAP_PRIVATE | MAP_ANONYMOUS |
				       MAP_NORESERVE;
		int ret2;

		mmap_write_lock(mm);
		ret2 = corten_arena_auto_mmap_route(mm, NULL, 0, PMD_SIZE,
						    PROT_READ | PROT_WRITE,
						    &a2, &len2, &flags2);
		mmap_write_unlock(mm);
		KUNIT_ASSERT_EQ(test, ret2, 1);
		seg_tail = a2 + PMD_SIZE;
	}
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_range_overlaps(mm, seg_tail,
						       PMD_SIZE));
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_range_occupied_incl_idle(mm, seg_tail,
								PMD_SIZE));
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_placement_backstop(mm, seg_tail,
							   PMD_SIZE));
	KUNIT_EXPECT_EQ(test, corten_arena_test_placement_backstop(),
			backstops + 1);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* P4: a parked window must be VMA-free at handout.  A foreign VMA inside
 * one (a placement guard failed upstream) makes the reactivation eject
 * the slot and answer the caller as a plain pool miss -- the auto route
 * places fresh window, the counter discloses the defensive ejection.
 */
static void corten_arena_test_p4_eject(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct vm_area_struct *vma;
	unsigned long addr = 0, lenp = CORTEN_ARENA_TEST_LEN, flags;
	long p4 = corten_arena_test_p4_ejects();
	long misses = corten_arena_test_pool_misses();
	int ret;

	if (!corten_enabled_static())
		kunit_skip(test, "placement guards require corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	corten_arena_test_park_targeted(test, mm);

	/* Inject the illegal shape directly (vma_link bypasses every
	 * placement guard -- only a guard failure upstream can produce it
	 * for real).
	 */
	vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_BASE,
				     CORTEN_ARENA_TEST_BASE + PMD_SIZE,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);

	/* The same-size request the parked slot would serve: P4 ejects
	 * (WARN_ONCE + counter), pool_take degrades to a miss, the fresh
	 * placement lands elsewhere.  The WARN fires once per boot; the
	 * counter is the assertion surface.
	 */
	flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	mmap_write_lock(mm);
	ret = corten_arena_auto_mmap_route(mm, NULL, 0, CORTEN_ARENA_TEST_LEN,
					   PROT_READ | PROT_WRITE, &addr,
					   &lenp, &flags);
	mmap_write_unlock(mm);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_NE(test, addr, CORTEN_ARENA_TEST_BASE);
	KUNIT_EXPECT_EQ(test, corten_arena_test_p4_ejects(), p4 + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_misses(), misses + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 0);
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_test_pool_idle(mm,
						       CORTEN_ARENA_TEST_BASE));

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* A.2's hint fence, end-to-end through the placement funnel: a hinted
 * (non-FIXED) anonymous mmap whose hint lands inside the window domain
 * is NOT accepted at the hint -- the walker replays it outside the
 * window.  A non-MODE mm keeps the hint (the double gate's zero-disturb
 * anchor).
 */
static void corten_arena_test_hint_fence(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	unsigned long hint_plain = CORTEN_ARENA_TEST_WIN + PMD_SIZE;
	unsigned long hint_mode = CORTEN_ARENA_TEST_WIN + 2 * PMD_SIZE;
	long r;

	if (!corten_enabled_static())
		kunit_skip(test, "placement guards require corten=on");

	/* Non-MODE: the hint is honoured as-is. */
	r = corten_arena_test_vm_mmap(test, mm, hint_plain, PAGE_SIZE, 0);
	KUNIT_EXPECT_EQ(test, r, hint_plain);
	KUNIT_EXPECT_NOT_NULL(test, vma_lookup(mm, hint_plain));

	/* MODE: a different in-window hint, replayed outside [16T, 64T). */
	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	r = corten_arena_test_vm_mmap(test, mm, hint_mode, PAGE_SIZE, 0);
	KUNIT_EXPECT_FALSE(test, IS_ERR_VALUE(r));
	if (!IS_ERR_VALUE(r)) {
		bool outside = r < CORTEN_MODE_WINDOW_START ||
			       r >= CORTEN_MODE_WINDOW_END;

		KUNIT_EXPECT_TRUE(test, outside);
		KUNIT_EXPECT_NOT_NULL(test, vma_lookup(mm, r));
		KUNIT_EXPECT_NULL(test, vma_lookup(mm, hint_mode));
	}

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* ------------------------------------------------------------------ *
 * V-A.1 (MV_VMA_FREE_SPEC.md sec 3.1.1): the park de-VMA surgery.
 * The parked window is a pure metadata domain; the reuse is a metadata
 * flip; every consumer runs off the frame table + region record.
 * ------------------------------------------------------------------
 */

/* ------------------------------------------------------------------ *
 * V-A.3b (j2-audit J1 hygiene): the five-hook J1 prelude, the corten
 * exemption channels, and the fault/uffd window short-circuits.
 * ------------------------------------------------------------------
 */

/* mm-internal (mm/userfaultfd.c), no header: hook 5/5's funnel entry,
 * compiled only under CONFIG_USERFAULTFD.
 */
#ifdef CONFIG_USERFAULTFD
extern int find_vmas_mm_locked(struct mm_struct *mm,
			       unsigned long dst_start,
			       unsigned long src_start,
			       struct vm_area_struct **dst_vmap,
			       struct vm_area_struct **src_vmap);
#endif

/* The five probed primitives, one pass each on a MODE mm's window
 * domain: every call counts exactly one probe and none counts a hit
 * (the live window is VMA-free post-A.2), the delegated domain and the
 * non-MODE door stay silent.  Hook 5 (find_vma_and_prepare_anon)
 * exists only under CONFIG_USERFAULTFD, so the expected count adapts.
 */
static void corten_arena_test_j1_hooks(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct vm_area_struct *prev = NULL;
	long p0, p1, h0, h1;
	int expected = 4;

	if (!corten_enabled_static())
		kunit_skip(test, "J1 hooks require corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm,
						      CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);
	/* The pure-MODE shape: a live, VMA-free window. */
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));

	p0 = corten_arena_test_j1_probes();
	h0 = corten_arena_test_j1_hits();

	mmap_read_lock(mm);
	KUNIT_EXPECT_NULL(test,
			  find_vma_intersection(mm, CORTEN_ARENA_TEST_WIN,
						CORTEN_ARENA_TEST_WIN +
						PAGE_SIZE));
	KUNIT_EXPECT_NULL(test, find_vma(mm, CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_NULL(test,
			  find_vma_prev(mm, CORTEN_ARENA_TEST_WIN, &prev));
	mmap_read_unlock(mm);
	/* lock_vma_under_rcu() manages its own RCU section. */
	KUNIT_EXPECT_NULL(test,
			  lock_vma_under_rcu(mm, CORTEN_ARENA_TEST_WIN));
#ifdef CONFIG_USERFAULTFD
	{
		struct vm_area_struct *dv, *sv;

		expected++;
		mmap_read_lock(mm);
		KUNIT_EXPECT_EQ(test,
				find_vmas_mm_locked(mm,
						    CORTEN_ARENA_TEST_WIN,
						    CORTEN_ARENA_TEST_NOWHERE,
						    &dv, &sv), -ENOENT);
		mmap_read_unlock(mm);
	}
#endif

	p1 = corten_arena_test_j1_probes();
	h1 = corten_arena_test_j1_hits();
	KUNIT_EXPECT_EQ(test, p1 - p0, (long)expected);
	KUNIT_EXPECT_EQ(test, h1 - h0, 0);

	/* The delegated domain is beyond the first door's window term. */
	mmap_read_lock(mm);
	find_vma(mm, CORTEN_ARENA_TEST_NOWHERE);
	mmap_read_unlock(mm);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j1_probes() - p1, 0);

	/* The second door: with MODE off, the same window call is silent. */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
	mmap_read_lock(mm);
	find_vma(mm, CORTEN_ARENA_TEST_WIN);
	mmap_read_unlock(mm);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j1_probes() - p1, 0);
}

/* The exemption channels (audit #52): a MODE mm drives the two corten
 * tree-walk consumers -- the auto route's obstacle scan (through the
 * untracked corten_vma_find alias) and the fenced window-hint walkers
 * -- with the J1 counters silent throughout; only an explicit external
 * lookup counts.
 */
static void corten_arena_test_j1_self_exempt(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	unsigned long hint = CORTEN_MODE_WINDOW_START + 4 * PMD_SIZE;
	long p0, p1;

	if (!corten_enabled_static())
		kunit_skip(test, "J1 exemption requires corten=on");
	if (sysctl_overcommit_memory == OVERCOMMIT_NEVER)
		kunit_skip(test, "auto takeover degraded (OVERCOMMIT_NEVER)");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	p0 = corten_arena_test_j1_probes();

	/* Auto takeover (addr == 0): the magazine obstacle scan reaches
	 * the maple tree through the alias only.  A degraded-to-legacy
	 * outcome walks the delegated side and is equally silent.
	 */
	KUNIT_ASSERT_FALSE(test,
			   IS_ERR_VALUE(corten_arena_test_vm_mmap(test, mm,
								  0, PMD_SIZE,
								  0)));
	/* A window hint: the walkers fence before any lookup on the
	 * hinted address (the V-A.3b fence-before-lookup order).
	 */
	KUNIT_ASSERT_FALSE(test,
			   IS_ERR_VALUE(corten_arena_test_vm_mmap(test, mm,
								  hint,
								  PAGE_SIZE,
								  0)));

	p1 = corten_arena_test_j1_probes();
	KUNIT_EXPECT_EQ(test, p1 - p0, 0);

	/* Positive control: the same window address, probed explicitly. */
	mmap_read_lock(mm);
	find_vma(mm, hint);
	mmap_read_unlock(mm);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j1_probes() - p1, 1);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* The mfill/move entry short-circuit (audit #29): a window-domain
 * destination (or, for move, either leg) answers -ENOENT off the two
 * doors alone.  The funnel-level shapes need a live userfaultfd
 * context; the helper is the funnel's verdict (brief B10's degraded
 * form) and its counter is the observable.
 */
static void corten_arena_test_uffd_window_reject(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	long r0, r1;

	if (!corten_enabled_static())
		kunit_skip(test, "uffd short-circuit requires corten=on");

	/* MODE is the takeover: the short-circuit is live from mode_enter
	 * on, before any arena exists (a hole window answers -ENOENT no
	 * less -- it cannot host an uffd registration either).
	 */
	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_EXPECT_TRUE(test,
			  corten_uffd_window_reject(mm,
						    CORTEN_MODE_WINDOW_START,
						    PAGE_SIZE, 0, 0));

	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm,
						      CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);

	r0 = corten_arena_test_uffd_rejects();

	/* mfill shape (the src leg is unused: 0/0). */
	KUNIT_EXPECT_TRUE(test,
			  corten_uffd_window_reject(mm, CORTEN_ARENA_TEST_WIN,
						    PAGE_SIZE, 0, 0));
	/* move shape: either leg inside the window rejects. */
	KUNIT_EXPECT_TRUE(test,
			  corten_uffd_window_reject(mm,
						    CORTEN_ARENA_TEST_NOWHERE,
						    PAGE_SIZE,
						    CORTEN_ARENA_TEST_WIN,
						    PAGE_SIZE));
	/* A delegated range straddling the window's lower edge. */
	KUNIT_EXPECT_TRUE(test,
			  corten_uffd_window_reject(mm,
						    CORTEN_MODE_WINDOW_START -
						    PAGE_SIZE,
						    2 * PAGE_SIZE, 0, 0));

	r1 = corten_arena_test_uffd_rejects();
	KUNIT_EXPECT_EQ(test, r1 - r0, 3);

	/* Boundary exactness: touching is not intersecting, and a fully
	 * delegated range never rejects.
	 */
	KUNIT_EXPECT_FALSE(test,
			   corten_uffd_window_reject(mm,
						     CORTEN_MODE_WINDOW_START -
						     PAGE_SIZE,
						     PAGE_SIZE, 0, 0));
	KUNIT_EXPECT_FALSE(test,
			   corten_uffd_window_reject(mm,
						     CORTEN_MODE_WINDOW_END,
						     PAGE_SIZE, 0, 0));
	KUNIT_EXPECT_FALSE(test,
			   corten_uffd_window_reject(mm,
						     CORTEN_ARENA_TEST_NOWHERE,
						     PAGE_SIZE, 0, 0));
	KUNIT_EXPECT_EQ(test, corten_arena_test_uffd_rejects(), r1);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
	/* The second door: with MODE off, the same window call is silent. */
	KUNIT_EXPECT_FALSE(test,
			   corten_uffd_window_reject(mm, CORTEN_ARENA_TEST_WIN,
						     PAGE_SIZE, 0, 0));
	KUNIT_EXPECT_EQ(test, corten_arena_test_uffd_rejects(), r1);
}

/* The fault short-circuit pair (audit #1/#2) with the S-1 invariance:
 * a parked window's slow-path lookup answers NULL with ZERO J1 probes
 * (the terminus replaced the walk), an active window still walks (the
 * ownership-fallback carve-out -- legacy must serve a punch implant),
 * and the fast-hook arm diverts without terminating anything by
 * itself.
 */
static void corten_arena_test_fault_window_shorts(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	unsigned long active = CORTEN_MODE_WINDOW_START + 2 * PMD_SIZE;
	long p0, p1, f0, f1;

	if (!corten_enabled_static())
		kunit_skip(test, "fault short-circuit requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* The parked window at the base (the noreplace_parked recipe). */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm,
						      CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_pool_idle(mm,
						      CORTEN_ARENA_TEST_WIN));
	/* The active window one slot up. */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, active, PMD_SIZE),
			0);

	f0 = corten_arena_test_fault_fallback_window();
	p0 = corten_arena_test_j1_probes();

	/* #1: the fast-hook arm is domain-wide (the slow path decides);
	 * it counts and diverts, never terminates.
	 */
	KUNIT_EXPECT_TRUE(test,
			  corten_fault_window_fallback(mm,
						       CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_TRUE(test, corten_fault_window_fallback(mm, active));
	KUNIT_EXPECT_FALSE(test,
			   corten_fault_window_fallback(mm,
							CORTEN_ARENA_TEST_NOWHERE));

	/* #2 end-to-end: the parked window's slow-path lookup answers
	 * NULL without touching the maple tree.
	 */
	KUNIT_EXPECT_NULL(test,
			  lock_mm_and_find_vma(mm, CORTEN_ARENA_TEST_WIN,
					       NULL));
	p1 = corten_arena_test_j1_probes();
	f1 = corten_arena_test_fault_fallback_window();
	KUNIT_EXPECT_EQ(test, p1 - p0, 0);
	KUNIT_EXPECT_EQ(test, f1 - f0, 3);

	/* The carve-out: an active arena keeps the walk (the probe below
	 * is the ownership-fallback shape's legal cost -- it would find a
	 * punch implant there).
	 */
	KUNIT_EXPECT_NULL(test, lock_mm_and_find_vma(mm, active, NULL));
	KUNIT_EXPECT_EQ(test, corten_arena_test_j1_probes() - p1, 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fault_fallback_window(), f1);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* ------------------------------------------------------------------
 * V-A.3c (j2-audit INV-MV2): the J2 walker -- clean shapes, injected
 * violations, the stale classification, the trigger-point wiring and
 * the fork implant mirror.
 * ------------------------------------------------------------------
 */

/* The sampling static key is global: a test that arms it must disarm it
 * whatever its assertions found.
 */
static void corten_arena_test_j2_sample_off(void *ctx)
{
	corten_arena_j2_sample_set(false);
}

/* The clean anchor (brief C11): a MODE mm through its real lifecycle --
 * attach, madvise hint, park -- holds INV-MV2; the delegated-domain
 * harness VMA never counts, and the exit-gate render exposes the
 * J1+J2 numbers the A-series guest acceptance greps.
 */
static void corten_arena_test_inv_mv2_clean(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	long w0, v0;

	if (!corten_enabled_static())
		kunit_skip(test, "J2 walker requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm,
						      CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);

	/* The pure-MODE shape: a live VMA-free window beside the harness's
	 * delegated-domain VMA -- nothing in the tree is outside the
	 * dominion's own shapes.
	 */
	w0 = corten_arena_test_j2_walks();
	v0 = corten_arena_test_j2_violations();
	KUNIT_EXPECT_EQ(test, corten_audit_j2_walk(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_walks(), w0 + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_violations(), v0);

	/* An in-window madvise hint (the route's handled arm) and back: a
	 * full park/free cycle later the invariant still holds.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_NORMAL,
						   CORTEN_ARENA_TEST_WIN,
						   PAGE_SIZE), 1);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_pool_idle(mm,
						      CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_EQ(test, corten_audit_j2_walk(mm), 0);

	/* The exit-gate one-stop read: the guest interface must carry the
	 * J1 pair, the J2 ledger and the verdict line.
	 */
	{
		char *gate = corten_test_render_dbg(CORTEN_DBG_AUDIT_GATE);

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, gate);
		KUNIT_ASSERT_NOT_NULL(test, strstr(gate, "j1_probes"));
		KUNIT_ASSERT_NOT_NULL(test, strstr(gate, "j1_hits"));
		KUNIT_ASSERT_NOT_NULL(test, strstr(gate, "j2_walks"));
		KUNIT_ASSERT_NOT_NULL(test, strstr(gate, "j2_violations"));
		KUNIT_ASSERT_NOT_NULL(test, strstr(gate, "j2_stale"));
		KUNIT_ASSERT_NOT_NULL(test, strstr(gate, "gate_pass"));
		kfree(gate);
	}

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* The violation anchor (brief C12): a foreign VMA inside the window that
 * no placement guard admitted and no producer registered (mkvm links it
 * raw -- only an upstream guard failure could produce this shape) is
 * exactly one violation with its address archived; registering the
 * range after the fact clears it (the whitelist mechanism's
 * self-proof); dropping the VMA again leaves the entry as stale, not as
 * a violation.
 */
static void corten_arena_test_inv_mv2_inject(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	unsigned long at = CORTEN_ARENA_TEST_WIN + PMD_SIZE;
	struct vm_area_struct *vma;
	long v0, s0, first0;

	if (!corten_enabled_static())
		kunit_skip(test, "J2 injection requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	/* Live dominion state beside the injected piece. */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm,
						      CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);

	vma = corten_arena_test_mkvm(mm, at, at + PAGE_SIZE,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);

	v0 = corten_arena_test_j2_violations();
	s0 = corten_arena_test_j2_stale();
	first0 = corten_arena_test_j2_first_violation();

	KUNIT_EXPECT_EQ(test, corten_audit_j2_walk(mm), 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_violations(), v0 + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_stale(), s0);
	/* First-writer-wins archive: set when this boot had none yet. */
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_j2_first_violation() == at ||
			  first0 != 0);

	/* The whitelist self-proof. */
	mmap_write_lock(mm);
	corten_implant_mark(mm, at, PAGE_SIZE);
	mmap_write_unlock(mm);
	KUNIT_EXPECT_EQ(test, corten_arena_test_implant_nr(mm), 1);
	KUNIT_EXPECT_EQ(test, corten_audit_j2_walk(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_violations(), v0 + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_stale(), s0);

	/* The entry outliving its VMA is the stale classification, never
	 * a violation.
	 */
	corten_arena_test_drop_vma(vma);
	KUNIT_EXPECT_EQ(test, corten_audit_j2_walk(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_violations(), v0 + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_stale(), s0 + 1);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* The stale classification on its own producer (the A.3b handoff): a
 * registry entry whose mmap never installed -- mark-then-fail residue
 * -- is collected as stale without a WARN; installing the VMA after
 * the fact stops the counting (registry-before-VMA tolerance).
 */
static void corten_arena_test_inv_mv2_stale(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct vm_area_struct *vma;
	long s0, v0;

	if (!corten_enabled_static())
		kunit_skip(test, "J2 stale class requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	s0 = corten_arena_test_j2_stale();
	v0 = corten_arena_test_j2_violations();

	/* Registered, never installed.  The mark creates the registry
	 * (V-A.3c: an ENTER-only mm owns the window domain too), so it
	 * runs under the mmap_write every producer holds.
	 */
	mmap_write_lock(mm);
	corten_implant_mark(mm, CORTEN_ARENA_TEST_WIN, PAGE_SIZE);
	mmap_write_unlock(mm);
	KUNIT_EXPECT_EQ(test, corten_arena_test_implant_nr(mm), 1);
	KUNIT_EXPECT_EQ(test, corten_audit_j2_walk(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_stale(), s0 + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_violations(), v0);

	/* The VMA arrives late: the entry stops being stale and the VMA
	 * is legal against it.
	 */
	vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_WIN,
				     CORTEN_ARENA_TEST_WIN + PAGE_SIZE,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_EXPECT_EQ(test, corten_audit_j2_walk(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_stale(), s0 + 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_violations(), v0);

	corten_arena_test_drop_vma(vma);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* The legal implant end-to-end (D24) plus the fork mirror (V-A.3c): a
 * plain MAP_FIXED over a parked window audits clean, and the child
 * inherits the registry entry -- dup_mmap copies the implant VMA as an
 * ordinary legacy VMA, and without the mirror both the child's fault
 * terminus and this walker would misread it (the harness fork does not
 * dup tree VMAs, so the copied piece is simulated with mkvm).
 */
static void corten_arena_test_inv_mv2_implant_fork(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm, *child;
	struct vm_area_struct *cvma;

	if (!corten_enabled_static())
		kunit_skip(test, "J2 implant fork requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm,
						      CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_vm_mmap(test, mm,
						  CORTEN_ARENA_TEST_WIN,
						  PMD_SIZE, MAP_FIXED),
			CORTEN_ARENA_TEST_WIN);

	/* The walker's core legal shape: zero violations, entry not
	 * stale.
	 */
	KUNIT_EXPECT_EQ(test, corten_audit_j2_walk(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_implant_nr(mm), 1);

	/* Fork: the registry mirrors.  The parent holds no live arena at
	 * this point (the window was ejected) -- the implant alone builds
	 * the child registry (V-A.3c).
	 */
	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_implant_nr(child), 1);

	/* The dup'd piece: a plain window VMA in the child, audited
	 * against the mirrored entry.
	 */
	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_WIN,
				      CORTEN_ARENA_TEST_WIN + PMD_SIZE,
				      CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	KUNIT_EXPECT_EQ(test, corten_audit_j2_walk(child), 0);

	corten_arena_test_drop_vma(cvma);
	mmput(child);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* The trigger wiring: fork_commit/mm_exit always walk, the hot-path
 * points (park, take/reactivate, route tails) only under the sampling
 * key, and each fires exactly its own count.
 */
static void corten_arena_test_j2_triggers(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm, *child;
	unsigned long win2 = CORTEN_ARENA_TEST_WIN + PMD_SIZE;
	unsigned long addr = 0, lenp = PMD_SIZE;
	unsigned long flags = MAP_PRIVATE | MAP_ANONYMOUS;
	long w;

	if (!corten_enabled_static())
		kunit_skip(test, "J2 triggers require corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm,
						      CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);

	/* Lifecycle: the fork_commit tail walks the child (its registry
	 * was just built by the mirror).
	 */
	w = corten_arena_test_j2_walks();
	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_walks(), w + 1);
	/* mm_exit walks the dying registry (the mirrored arena's child).
	 */
	mmput(child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_walks(), w + 2);

	/* Default: the hot path is dark.  A full park (park_locked tail +
	 * the route's EXACT exit) and a pool take (reactivate tail + the
	 * take tail) both stay silent.
	 */
	w = corten_arena_test_j2_walks();
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_walks(), w);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 1);
	addr = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_auto_route_locked(mm, PMD_SIZE,
							    PROT_READ |
							    PROT_WRITE,
							    &addr, &lenp,
							    &flags),
			2);
	KUNIT_EXPECT_EQ(test, addr, CORTEN_ARENA_TEST_WIN);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_walks(), w);

	/* Sampling on: every hot point fires its own count. */
	corten_arena_j2_sample_set(true);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action(test, corten_arena_test_j2_sample_off,
					 NULL), 0);

	w = corten_arena_test_j2_walks();
	/* Park: park_locked tail + the munmap route's EXACT exit. */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_walks(), w + 2);

	/* Take: reactivate tail + the take tail. */
	addr = 0;
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_auto_route_locked(mm, PMD_SIZE,
							    PROT_READ |
							    PROT_WRITE,
							    &addr, &lenp,
							    &flags),
			2);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_walks(), w + 4);

	/* A chunk munmap inside a live window: the route's chunk exit. */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, win2, PMD_SIZE), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 win2 + PAGE_SIZE, PAGE_SIZE),
			1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_walks(), w + 5);

	/* The madvise hint arm. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_NORMAL, win2,
						   PAGE_SIZE), 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j2_walks(), w + 6);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* ------------------------------------------------------------------
 * V-A.3d (D-group): the S-5 query-syscall terminals (j2-audit
 * #13/#20/#23/#28, D24) and the J1 implant exemption -- the A-series
 * exit-gate closure.
 * ------------------------------------------------------------------
 */

/* One ksys_msync() call on the attached worker: the syscall body reads
 * current->mm for both the lock and the loop.
 */
static void corten_arena_test_op_msync(struct corten_arena_test_op *o)
{
	o->ret = ksys_msync(o->addr, o->len, o->flags);
}

static int corten_arena_test_msync(struct kunit *test, struct mm_struct *mm,
				   unsigned long addr, unsigned long len,
				   int flags)
{
	struct corten_arena_test_op o = {
		.mm = mm,
		.fn = corten_arena_test_op_msync,
		.addr = addr,
		.len = len,
		.flags = flags,
	};

	/* run_op_full() returns o.ret, which IS the msync verdict -- the
	 * callers assert that value, not the thread's success (the runner
	 * itself already fails the case if the worker cannot start).
	 */
	return corten_arena_test_run_op_full(test, &o);
}

/* S-5① (audit #23): the registered window segment is not an msync gap.
 * A fully-occupied window chunk answers 0 (the anonymous-reservation
 * no-op), a delegated-VMA + occupied-window mix answers 0 with the
 * window part skipped, and holes inside the window keep the legacy
 * -ENOMEM -- all with the J1 pair silent (the pre-A.3d walk cost one
 * window find_vma() per loop entry).
 */
static void corten_arena_test_msync_window_segments(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct vm_area_struct *vma;
	unsigned long win = CORTEN_ARENA_TEST_WIN;
	long p0;

	if (!corten_enabled_static())
		kunit_skip(test, "msync terminals require corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	/* Two parked frames at the window base + one delegated VMA ending
	 * exactly at the window's lower edge.
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, win, 2 * PMD_SIZE),
		0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 win, 2 * PMD_SIZE), 1);
	KUNIT_EXPECT_TRUE(test, corten_arena_test_pool_idle(mm, win));
	vma = corten_arena_test_mkvm(mm, win - PMD_SIZE, win,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);

	p0 = corten_arena_test_j1_probes();

	/* Pure parked window: the A.1-pre reservation no-op. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_msync(test, mm, win, 2 * PMD_SIZE,
						MS_SYNC), 0);
	/* Delegated VMA + parked window: both legs processed/skipped, 0. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_msync(test, mm, win - PMD_SIZE,
						3 * PMD_SIZE, MS_SYNC), 0);
	/* Parked head + hole tail: skip stops at the first unregistered
	 * frame, find_vma() misses, the legacy -ENOMEM.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_msync(test, mm, win, 4 * PMD_SIZE,
						MS_SYNC), -ENOMEM);

	/* The J1 payoff for #23: the occupied/VMA legs above never walked
	 * the window tree -- the only probe so far is the single
	 * find_vma() of the hole tail (one walk, the unchanged legacy
	 * hole shape: a hole query paid the same walk before A.1).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_j1_probes() - p0, 1);
	KUNIT_EXPECT_GT(test,
			corten_arena_test_named_counter(test,
							"msync_window_skips"),
			0);

	/* Pure window hole (either sync flag): unchanged legacy verdict
	 * and one walk each.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_msync(test, mm, win + 3 * PMD_SIZE,
						PMD_SIZE, MS_SYNC), -ENOMEM);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_msync(test, mm, win + 3 * PMD_SIZE,
						PMD_SIZE, MS_ASYNC), -ENOMEM);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j1_probes() - p0, 3);

	corten_arena_test_drop_vma(vma);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* S-5② (audit #13, D24): the mincore window terminal answers the real
 * residency vector -- present pages 1, never-faulted slots 0, a swapped
 * slot the swap-cache truth (an uncached entry reads 0) -- while a
 * parked span answers the all-zero reservation vector and hole/implant
 * chunks keep the legacy funnel (-EAGAIN here; do_mincore() turns the
 * hole into its -ENOMEM, the implant chunk walks the tree truth).
 */
static void corten_arena_test_mincore_route(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct vm_area_struct *vma;
	unsigned long win = CORTEN_ARENA_TEST_WIN;
	unsigned char vec[4];
	long r0;

	if (!corten_enabled_static())
		kunit_skip(test, "mincore terminal requires corten=on");

	/* Not a MODE mm yet: the route is transparent. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_mincore_route(mm, win, 1, vec), -EAGAIN);

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, win, PMD_SIZE), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, win), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_fork_seed_mapped(mm, win), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_fork_seed_mapped(mm, win + PAGE_SIZE),
		0);

	/* The truth walk: two present, one never-faulted slot. */
	r0 = corten_arena_test_named_counter(test, "mincore_routes");
	memset(vec, 0xa5, sizeof(vec));
	KUNIT_EXPECT_EQ(test, corten_arena_mincore_route(mm, win, 3, vec), 3);
	KUNIT_EXPECT_EQ(test, vec[0], 1);
	KUNIT_EXPECT_EQ(test, vec[1], 1);
	KUNIT_EXPECT_EQ(test, vec[2], 0);
	KUNIT_EXPECT_GT(test,
			corten_arena_test_named_counter(test, "mincore_routes"),
			r0);

	/* The swap leg: hand-roll one swap-out (the fork_swapped
	 * sequence); the uncached entry answers 0 -- mincore_swap()'s
	 * anon-arm verdict on the same PTE.
	 */
	{
		struct corten_txn txn;
		struct corten_pte_meta sm = { };
		swp_entry_t entry = swp_entry(1, 0x77);
		struct folio *folio;
		pte_t *ptep, pte;
		spinlock_t *ptl;	/* the swap-out hand-roll's PTE lock */
		pmd_t *pmdp = corten_arena_test_pmd(mm, win + PAGE_SIZE);

		KUNIT_ASSERT_NOT_NULL(test, pmdp);
		vma = corten_arena_test_carrier_of(mm, win + PAGE_SIZE);
		KUNIT_ASSERT_NOT_NULL(test, vma);
		ptep = pte_offset_map_lock(mm, pmdp, win + PAGE_SIZE, &ptl);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
		pte = ptep_get_and_clear(mm, win + PAGE_SIZE, ptep);
		KUNIT_ASSERT_TRUE(test, pte_present(pte));
		set_pte_at(mm, win + PAGE_SIZE, ptep, swp_entry_to_pte(entry));
		pte_unmap_unlock(ptep, ptl);
		folio = page_folio(pte_page(pte));
		folio_remove_rmap_pte(folio, folio_page(folio, 0), vma);
		add_mm_counter(mm, MM_ANONPAGES, -1);
		add_mm_counter(mm, MM_SWAPENTS, 1);
		folio_put(folio);

		KUNIT_ASSERT_EQ(test,
				corten_lock_range(mm, win + PAGE_SIZE,
						  PAGE_SIZE, &txn), 0);
		sm.state = CORTEN_SWAPPED;
		sm.perm = CORTEN_PERM_READ | CORTEN_PERM_WRITE |
			  CORTEN_PERM_USER;
		corten_swap_encode(&sm, entry);
		KUNIT_EXPECT_EQ(test,
				corten_swap_out(&txn, win + PAGE_SIZE, &sm), 0);
		corten_unlock(&txn);
	}
	KUNIT_EXPECT_EQ(test, corten_arena_mincore_route(mm, win, 3, vec), 3);
	KUNIT_EXPECT_EQ(test, vec[0], 1);
	KUNIT_EXPECT_EQ(test, vec[1], 0);
	KUNIT_EXPECT_EQ(test, vec[2], 0);

	/* The parked span: the all-zero reservation vector. */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 win, PMD_SIZE), 1);
	memset(vec, 0xa5, sizeof(vec));
	KUNIT_EXPECT_EQ(test, corten_arena_mincore_route(mm, win, 2, vec), 2);
	KUNIT_EXPECT_EQ(test, vec[0], 0);
	KUNIT_EXPECT_EQ(test, vec[1], 0);

	/* A hole chunk and an implant chunk keep the legacy funnel. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_mincore_route(mm, win + 2 * PMD_SIZE, 1,
						   vec), -EAGAIN);
	vma = corten_arena_test_mkvm(mm, win + PMD_SIZE,
				     win + PMD_SIZE + PAGE_SIZE,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	mmap_write_lock(mm);
	corten_implant_mark(mm, win + PMD_SIZE, PAGE_SIZE);
	mmap_write_unlock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mincore_route(mm, win + PMD_SIZE, 1,
						   vec), -EAGAIN);
	/* The delegated domain never enters the route. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_mincore_route(mm, CORTEN_ARENA_TEST_BASE, 1,
						   vec), -EAGAIN);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* S-5③ (audit #20): the fully-parked window span is a terminal 0 for
 * the contract behaviours (content already dropped at park / hints
 * that were no-ops on the live arm); every other behaviour, and every
 * non-parked shape (active frame, hole, mixed), keeps the route's
 * legacy arm untouched.
 */
static void corten_arena_test_madvise_parked_terminal(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	unsigned long win = CORTEN_ARENA_TEST_WIN;
	long c0;

	if (!corten_enabled_static())
		kunit_skip(test, "madvise terminal requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, win, PMD_SIZE), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 win, PMD_SIZE), 1);
	KUNIT_EXPECT_TRUE(test, corten_arena_test_pool_idle(mm, win));

	c0 = corten_arena_test_named_counter(test, "madvise_parked");
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_DONTNEED, win,
						   PMD_SIZE), 1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_DONTNEED_LOCKED,
						   win, PAGE_SIZE), 1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_FREE, win,
						   PAGE_SIZE), 1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_NORMAL, win,
						   PAGE_SIZE), 1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_SEQUENTIAL, win,
						   PAGE_SIZE), 1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_RANDOM, win,
						   PAGE_SIZE), 1);
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_COLD, win,
						   PAGE_SIZE), 1);
	KUNIT_EXPECT_GT(test,
			corten_arena_test_named_counter(test, "madvise_parked"),
			c0);

	/* The disclosed residual: WILLNEED on a parked span keeps the
	 * legacy verdict (the walk's -ENOMEM downstream).
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_WILLNEED, win,
						   PAGE_SIZE), 0);

	/* Not a parked span: a window hole (never registered) and an
	 * active-adjacent span both keep the legacy route verdict -- the
	 * hole's -ENOMEM is the pre-A.1 behaviour too.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_DONTNEED,
						   win + 2 * PMD_SIZE,
						   PMD_SIZE), 0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_pool_attach(mm, win + PMD_SIZE,
						      PMD_SIZE), 0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_DONTNEED, win,
						   2 * PMD_SIZE), 0);
	/* A DONTNEED strictly inside the live arena is the existing
	 * transactional arm (regression guard for the new head).
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_madvise_route(mm, MADV_DONTNEED,
						   win + PMD_SIZE,
						   PAGE_SIZE), 1);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* The J1 implant exemption (D24): an implant access is a legal tree
 * lookup inside the contract -- the query and the found-VMA shapes
 * both leave the J1 pair silent, so the audit gate reads gate_pass 1
 * on the exact smoke shape that kept it red after A.3c.  An
 * unregistered foreign VMA still counts (the exemption is
 * registry-driven, not blind).
 */
static void corten_arena_test_j1_implant_exempt(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	unsigned long win = CORTEN_ARENA_TEST_WIN;
	struct vm_area_struct *vma, *locked, *prev = NULL;
	long p0, h0;

	if (!corten_enabled_static())
		kunit_skip(test, "J1 implant exemption requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	/* A live frame0 beside a registered implant on frame1. */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, win, PMD_SIZE), 0);
	vma = corten_arena_test_mkvm(mm, win + PMD_SIZE,
				     win + PMD_SIZE + PAGE_SIZE,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	mmap_write_lock(mm);
	corten_implant_mark(mm, win + PMD_SIZE, PAGE_SIZE);
	mmap_write_unlock(mm);

	p0 = corten_arena_test_j1_probes();
	h0 = corten_arena_test_j1_hits();

	/* The smoke contract shape: lookups on the implant VA find the
	 * registered VMA -- all exempt, probes and hits both silent.
	 */
	mmap_read_lock(mm);
	KUNIT_EXPECT_PTR_EQ(test, find_vma(mm, win + PMD_SIZE), vma);
	KUNIT_EXPECT_PTR_EQ(test,
			    find_vma_intersection(mm, win + PMD_SIZE,
						  win + PMD_SIZE + PAGE_SIZE),
			    vma);
	KUNIT_EXPECT_PTR_EQ(test, find_vma_prev(mm, win + PMD_SIZE, &prev),
			    vma);
	mmap_read_unlock(mm);
	locked = lock_vma_under_rcu(mm, win + PMD_SIZE);
	KUNIT_EXPECT_PTR_EQ(test, locked, vma);
	if (locked)
		vma_end_read(locked);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j1_probes() - p0, 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j1_hits() - h0, 0);

	/* The neighbour shape: a query on the VMA-free active frame that
	 * merely *returns* the implant is the same legal access.
	 */
	mmap_read_lock(mm);
	KUNIT_EXPECT_PTR_EQ(test, find_vma(mm, win), vma);
	mmap_read_unlock(mm);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j1_probes() - p0, 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j1_hits() - h0, 0);

	/* The exit gate: with the exemption in place the smoke shape
	 * leaves both hard invariants green.
	 */
	KUNIT_EXPECT_EQ(test, corten_audit_j2_walk(mm), 0);
	{
		char *gate = corten_test_render_dbg(CORTEN_DBG_AUDIT_GATE);

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, gate);
		KUNIT_ASSERT_NOT_NULL(test,
				      strstr(gate, "gate_pass          1"));
		kfree(gate);
	}

	/* The negative control: an unregistered foreign VMA at another
	 * window frame still counts its hit -- the whitelist, not the
	 * window, drives the exemption.
	 */
	{
		struct vm_area_struct *fv;
		long h1;

		fv = corten_arena_test_mkvm(mm, win + 2 * PMD_SIZE,
					    win + 2 * PMD_SIZE + PAGE_SIZE,
					    CORTEN_ARENA_TEST_FLAGS_OK);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, fv);
		h1 = corten_arena_test_j1_hits();
		mmap_read_lock(mm);
		KUNIT_EXPECT_PTR_EQ(test, find_vma(mm, win + 2 * PMD_SIZE), fv);
		mmap_read_unlock(mm);
		KUNIT_EXPECT_EQ(test, corten_arena_test_j1_hits() - h1, 1);
		KUNIT_EXPECT_EQ(test, corten_arena_test_j1_probes() - p0, 1);
		corten_arena_test_drop_vma(fv);
	}

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* S-5 #28: the move_pages query leg's window short-circuit -- every
 * non-implant window address answers -EFAULT without the tree walk;
 * implant VA keeps the lookup (the nid is real); the delegated domain
 * and non-MODE mms never enter.
 */
static void corten_arena_test_move_pages_window(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	unsigned long win = CORTEN_ARENA_TEST_WIN;
	struct vm_area_struct *vma;
	long c0;

	if (!corten_enabled_static())
		kunit_skip(test, "move_pages terminal requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, win, PMD_SIZE), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 win, PMD_SIZE), 1);
	vma = corten_arena_test_mkvm(mm, win + PMD_SIZE,
				     win + PMD_SIZE + PAGE_SIZE,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	mmap_write_lock(mm);
	corten_implant_mark(mm, win + PMD_SIZE, PAGE_SIZE);
	mmap_write_unlock(mm);

	c0 = corten_arena_test_named_counter(test, "move_pages_window");
	KUNIT_EXPECT_TRUE(test, corten_arena_move_pages_window(mm, win));
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_move_pages_window(mm,
							 win + 2 * PMD_SIZE));
	KUNIT_EXPECT_GT(test,
			corten_arena_test_named_counter(test,
							"move_pages_window"),
			c0);
	/* The implant keeps the lookup; the delegated domain, the window's
	 * lower edge and a non-MODE mm never enter.
	 */
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_move_pages_window(mm, win + PMD_SIZE));
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_move_pages_window(mm,
							  CORTEN_ARENA_TEST_BASE));
	KUNIT_EXPECT_FALSE(test,
			   corten_arena_move_pages_window(mm,
							  CORTEN_MODE_WINDOW_START -
							  PAGE_SIZE));

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
	KUNIT_EXPECT_FALSE(test, corten_arena_move_pages_window(mm, win));
}

/* The VMA-free consumption surface: chunk munmap without a VMA split,
 * the FRESH re-dispatch after it, the re-park, and the mprotect route's
 * miss on a parked window (legacy -ENOMEM, S-4's "already munmapped"
 * reading).
 */
static void corten_arena_test_vma_free_reuse(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct corten_arena *ar;
	struct corten_region_iter it;
	struct corten_pte_meta m;
	unsigned long addr = CORTEN_ARENA_TEST_WIN + PAGE_SIZE;
	unsigned long addr2 = 0, lenp, flags;
	long parks = corten_arena_test_pool_parks();
	int ret;

	if (!corten_enabled_static())
		kunit_skip(test, "VMA-free reuse requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* vma'd arena with one committed page, then parked (the cycle's
	 * entry shape; the park anchors themselves live in pool_reuse).
	 */
	/* V-A.2a: the attach is the VMA-less auto takeover -- no
	 * declaring VMA exists (the mkvm the pre-A.2 shape needed is
	 * gone).
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);
	/* seed_anon first: fill_upper() creates the tracked PT page (the
	 * pool_reuse convention).
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_fork_seed_anon(mm, addr), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_fork_seed_mapped(mm, addr), 0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_pool_idle(mm,
						      CORTEN_ARENA_TEST_WIN));

	/* Reactivate (pure metadata flip, no VMA). */
	lenp = PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	mmap_write_lock(mm);
	ret = corten_arena_auto_mmap_route(mm, NULL, 0, PAGE_SIZE,
					   PROT_READ | PROT_WRITE, &addr2,
					   &lenp, &flags);
	mmap_write_unlock(mm);
	KUNIT_EXPECT_EQ(test, ret, 2);
	KUNIT_EXPECT_EQ(test, addr2, CORTEN_ARENA_TEST_WIN);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));

	/* The chunk munmap (the madvise(DONTNEED)/guard shape) on the
	 * VMA-less arena: pure metadata content drop.  Nothing can
	 * split -- there is no VMA -- and the frame table stays intact.
	 */
	/* The first touch on the reactivated window materializes its PT
	 * pages (retired by the park) straight off the metadata.
	 */
	{
		unsigned int fault_flags = FAULT_FLAG_WRITE;

		KUNIT_EXPECT_EQ(test,
				corten_arena_user_fault(mm, addr, 0, NULL,
							&fault_flags),
				CORTEN_FAULT_HANDLED);
	}
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);

	/* The chunk munmap (the madvise(DONTNEED)/guard shape) on the
	 * VMA-less arena: pure metadata content drop.  Nothing can split
	 * -- there is no VMA -- and the frame table stays intact.
	 */
	ar = corten_arena_lookup_get(mm, CORTEN_ARENA_TEST_WIN);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ar);
	KUNIT_EXPECT_EQ(test,
			corten_arena_unmap_chunk(mm, ar, addr, PAGE_SIZE), 0);
	percpu_ref_put(&ar->active);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, CORTEN_ARENA_TEST_WIN),
			1);
	/* KEEP_PERM: the dropped slot keeps the committed contract. */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_INVALID);
	KUNIT_EXPECT_EQ(test, m.perm,
			CORTEN_PERM_USER | CORTEN_PERM_READ |
			CORTEN_PERM_WRITE);

	/* The FRESH gate re-derives the committed perm after the drop:
	 * the next fault maps again, straight off the metadata.
	 */
	{
		unsigned int fault_flags = FAULT_FLAG_WRITE;

		KUNIT_EXPECT_EQ(test,
				corten_arena_user_fault(mm, addr, 0, NULL,
							&fault_flags),
				CORTEN_FAULT_HANDLED);
	}
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);

	/* The mprotect route on a *parked* window misses (lookup
	 * invisible): 0 = legacy, whose find_vma answers -ENOMEM --
	 * "already munmapped", the S-4 reading.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);
	/* Two parks in this test: the entry munmap and the re-park. */
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_parks(), parks + 2);
	KUNIT_EXPECT_EQ(test,
			corten_arena_mprotect_route(mm, CORTEN_ARENA_TEST_WIN,
						    PAGE_SIZE, PROT_READ, -1),
			0);

	/* INV-MV3 registry walk: the parked record holds. */
	mmap_read_lock(mm);
	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(mm));
	mmap_read_unlock(mm);
	rcu_read_lock();
	corten_region_iter_init(&it);
	ar = corten_region_next(mm, &it);
	KUNIT_EXPECT_NOT_NULL(test, ar);
	if (ar)
		KUNIT_EXPECT_EQ(test, ar->rclass, CORTEN_REGION_RESERVED);
	rcu_read_unlock();

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* INV-MV3 (sec 2.6, the V-A.1 anchors): the record pairings -- may >=
 * prot and idle <=> RESERVED -- hold across the lifecycle, and a torn
 * record is reported by the registry walk.
 */
static void corten_arena_test_inv_mv3(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct corten_mm_state *state;
	struct corten_arena *ar;
	unsigned long addr2 = 0, lenp, flags;
	int perm;

	if (!corten_enabled_static())
		kunit_skip(test, "INV-MV3 requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	/* V-A.2a: the attach is the VMA-less auto takeover -- no
	 * declaring VMA exists (the mkvm the pre-A.2 shape needed is
	 * gone).
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);

	/* The encoding anchor: shadow-derived and pure perm-derived PTE
	 * encodings agree for every perm combination.
	 */
	for (perm = CORTEN_PERM_USER;
	     perm <= (CORTEN_PERM_USER | CORTEN_PERM_READ |
		      CORTEN_PERM_WRITE | CORTEN_PERM_EXEC);
	     perm++)
		KUNIT_EXPECT_TRUE(test,
				  corten_arena_test_perm_pgprot_pure_eq(perm));

	/* Live: may >= prot, ANON, not idle. */
	mmap_read_lock(mm);
	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(mm));
	mmap_read_unlock(mm);

	/* Park: RESERVED <=> idle. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);
	mmap_read_lock(mm);
	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(mm));
	mmap_read_unlock(mm);

	/* Tear the idle<=>RESERVED pairing by hand: the walk must
	 * report it (the test owns the mm; no reader can race).
	 */
	state = READ_ONCE(mm->corten_state);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);
	rcu_read_lock();
	ar = xa_load(&state->arenas, CORTEN_ARENA_TEST_WIN >> PMD_SHIFT);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ar);
	if (ar)
		WRITE_ONCE(ar->rclass, CORTEN_REGION_ANON);
	rcu_read_unlock();
	mmap_read_lock(mm);
	KUNIT_EXPECT_FALSE(test, corten_region_invariants_ok(mm));
	mmap_read_unlock(mm);
	rcu_read_lock();
	ar = xa_load(&state->arenas, CORTEN_ARENA_TEST_WIN >> PMD_SHIFT);
	if (ar)
		WRITE_ONCE(ar->rclass, CORTEN_REGION_RESERVED);
	rcu_read_unlock();

	/* Tear the MAY bound: may < prot must be reported. */
	rcu_read_lock();
	ar = xa_load(&state->arenas, CORTEN_ARENA_TEST_WIN >> PMD_SHIFT);
	if (ar)
		WRITE_ONCE(ar->prot, (u8)(ar->may_prot + 1));
	rcu_read_unlock();
	mmap_read_lock(mm);
	KUNIT_EXPECT_FALSE(test, corten_region_invariants_ok(mm));
	mmap_read_unlock(mm);
	/* Restore: the slot's own records are dead (parked, metadata
	 * reset), but the take must not resurrect a torn bound -- the
	 * reactivation re-stamps both fields wholesale.
	 */
	rcu_read_lock();
	ar = xa_load(&state->arenas, CORTEN_ARENA_TEST_WIN >> PMD_SHIFT);
	if (ar)
		WRITE_ONCE(ar->prot, CORTEN_PERM_USER);
	rcu_read_unlock();

	/* Reactivation (declare-side probe arm) re-stamps the record:
	 * the invariant holds again.
	 */
	lenp = PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	mmap_write_lock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_route(mm, NULL, 0,
						     PAGE_SIZE,
						     PROT_READ | PROT_WRITE,
						     &addr2, &lenp, &flags),
			2);
	mmap_write_unlock(mm);
	KUNIT_EXPECT_EQ(test, addr2, CORTEN_ARENA_TEST_WIN);
	mmap_read_lock(mm);
	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(mm));
	mmap_read_unlock(mm);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* The fork mirror of a VMA-less arena: dup_mmap() copies no VMA, the
 * explicit PTE copy arm supplies the content, and the COW protocol runs
 * on rmap-less pages.
 */
static void corten_arena_test_fork_vma_free(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm, *child;
	struct corten_pte_meta m;
	unsigned long addr = CORTEN_ARENA_TEST_WIN + PAGE_SIZE;
	unsigned long addr2 = 0, lenp, flags;
	long timeouts = corten_arena_test_drain_timeouts();
	unsigned int fault_flags = FAULT_FLAG_WRITE;

	if (!corten_enabled_static())
		kunit_skip(test, "VMA-free fork requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	/* V-A.2a: the attach is the VMA-less auto takeover -- no
	 * declaring VMA exists (the mkvm the pre-A.2 shape needed is
	 * gone).
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_fork_seed_anon(mm, addr), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_fork_seed_mapped(mm, addr), 0);

	/* Park + reactivate: the arena is live and VMA-less with real
	 * content -- the shape dup_mmap() cannot see.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);
	lenp = PAGE_SIZE;
	flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	mmap_write_lock(mm);
	KUNIT_EXPECT_EQ(test,
			corten_arena_auto_mmap_route(mm, NULL, 0,
						     PAGE_SIZE,
						     PROT_READ | PROT_WRITE,
						     &addr2, &lenp, &flags),
			2);
	mmap_write_unlock(mm);

	/* Materialize content on the VMA-less window (FRESH off the
	 * re-recorded ar->prot): the fork mirror copies what the parent
	 * actually holds, and the park retired the seeded PT pages.
	 */
	{
		unsigned int fault_flags = FAULT_FLAG_WRITE;

		KUNIT_ASSERT_EQ(test,
				corten_arena_user_fault(mm, addr, 0, NULL,
							&fault_flags),
				CORTEN_FAULT_HANDLED);
	}
	KUNIT_ASSERT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_ASSERT_EQ(test, m.state, CORTEN_MAPPED);

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);

	/* No parked arenas left (the pool is empty), so the flush is a
	 * no-op and the harness thread is unnecessary: begin/commit run
	 * inline like fork_faithful.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	/* No child VMA is created for the window: the mirror must find
	 * the content without one.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);

	/* Child: registry hit, mirrored MAPPED+SHARED record, and the
	 * copied PTE (the PT page the metadata replay gated on).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_query(child,
						 CORTEN_ARENA_TEST_WIN), 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_TRUE(test, m.flags & CORTEN_PF_SHARED);
	KUNIT_EXPECT_TRUE(test, corten_arena_test_pt_present(child, addr));

	/* Parent: SHARED mark set; the first write takes the COW copy
	 * branch (a rmap-less folio has mapcount 0 -- reuse is
	 * impossible) and clears it.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_TRUE(test, m.flags & CORTEN_PF_SHARED);
	KUNIT_EXPECT_EQ(test,
			corten_arena_user_fault(mm, addr, 0, NULL,
						&fault_flags),
			CORTEN_FAULT_HANDLED);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_FALSE(test, m.flags & CORTEN_PF_SHARED);

	/* Child keeps its read-only shared shape (its content is intact
	 * after the parent's COW).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_TRUE(test, m.flags & CORTEN_PF_SHARED);

	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(child));

	mmput(child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}


/* ------------------------------------------------------------------ *
 * V-A.1 二审回归锚（review B1/B2）: 两个 blocker 漏网形状的用例。
 * ------------------------------------------------------------------
 */

/* B1 回归锚: 无 VMA（reactivated）窗的 shrinker eval pick。  修复前
 * 该形状在已解锁后再解锁（double ptl unlock）且走 pick 路径; 修复后
 * 页被计数跳过、PT/metadata/folio 全部原样。
 *
 * 前置: pool 复活窗（无 VMA）+ 一页 rmap-less 驻留内容（map_anon 的
 * 无 VMA 形态: 计数 +1、无 folio_add_new_anon_rmap）。
 */
/* F1 重写: 同一 declare→park→re-declare 构造, 但修复后的前提是
 * reactivate 无条件重武装 carrier -- 活窗不再 anchor-less。  该形状
 * 因此与 carrier_shrink_pick 同为 pick 放行形态; B1 的 ptl 纪律回归锚
 * （两遍 scan 走 pick 路径, 不再出现解锁后二次解锁）保留, 内容改用
 * 真实 fault（rmap 锚在重武装的 carrier 上, 不能再用 rmap-less 植入
 * -- 那是在有锚窗口上制造 B1 修复所要杜绝的 freed-under-live-PTE）。
 */
static void corten_arena_test_vma_free_shrink_pick(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct folio *folio;
	struct corten_pte_meta m;
	pte_t *ptep, pte;
	pmd_t *pmdp;
	spinlock_t *ptl;
	unsigned long addr = CORTEN_ARENA_TEST_WIN + PAGE_SIZE;
	long skipped0 = corten_arena_test_shrink_skipped();
	unsigned int fault_flags = FAULT_FLAG_WRITE;

	if (!corten_enabled_static())
		kunit_skip(test, "shrinker bodies require corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	/* V-A.2a: the attach is the VMA-less auto takeover -- no
	 * declaring VMA exists (the mkvm the pre-A.2 shape needed is
	 * gone).
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);

	/* Park + DECLARE-side reactivation: the arena is live and
	 * VMA-less (no tree shadow -- the park removed it), and since
	 * F1 re-arms unconditionally it CARRIES a carrier: the
	 * anchor-less mixed state this test used to assert is gone
	 * (its fork shape lost content -- see
	 * corten_arena_test_fork_redeclare_content).
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_WIN,
						 PAGE_SIZE), 1);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_WIN,
					     PMD_SIZE), 0);
	KUNIT_EXPECT_NOT_NULL(test,
			      corten_arena_test_carrier_of(mm,
							   CORTEN_ARENA_TEST_WIN));
	mmap_read_lock(mm);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));
	mmap_read_unlock(mm);

	/* One resident page, rmap-anchored on the re-armed carrier. */
	KUNIT_ASSERT_EQ(test,
			corten_arena_user_fault(mm, addr, 0, NULL,
						&fault_flags),
			CORTEN_FAULT_HANDLED);

	/* Pass 1 ages the window; pass 2 evaluates it and reaches the
	 * pick: the gate is open (anchor present), so the pre-B1
	 * double-unlock shape ran exactly here.
	 */
	KUNIT_EXPECT_EQ(test, (int)corten_arena_test_shrink_scan(1), 0);
	KUNIT_EXPECT_EQ(test, (int)corten_arena_test_shrink_scan(1), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_shrink_skipped(), skipped0);

	/* No swap in the KUnit boot: the pick is kept -- the folio
	 * reference returns and every shape survives.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	pmdp = corten_arena_test_pmd(mm, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));
	folio = page_folio(pte_page(pte));
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 1);
	pte_unmap_unlock(ptep, ptl);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* V-A.2b: the carrier reopens the pick gate -- a carrier arena's
 * faulted page is rmap-anchored, so the pick runs (no skip), the ttu
 * leg of reclaim finds its mapping, and without swap the page comes
 * back kept: translation, metadata and folio intact, refcount
 * restored.
 */
static void corten_arena_test_carrier_shrink_pick(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct folio *folio;
	struct corten_pte_meta m;
	pte_t *ptep, pte;
	pmd_t *pmdp;
	spinlock_t *ptl;
	unsigned long addr = CORTEN_ARENA_TEST_WIN + PAGE_SIZE;
	long skipped0 = corten_arena_test_shrink_skipped();
	unsigned int fault_flags = FAULT_FLAG_WRITE;

	if (!corten_enabled_static())
		kunit_skip(test, "shrinker bodies require corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);

	/* Real fault content: map_anon() anchored it on the carrier. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_user_fault(mm, addr, 0, NULL,
						&fault_flags),
			CORTEN_FAULT_HANDLED);

	/* Pass 1 ages the window; pass 2 evaluates and picks. */
	KUNIT_EXPECT_EQ(test, (int)corten_arena_test_shrink_scan(1), 0);
	KUNIT_EXPECT_EQ(test, (int)corten_arena_test_shrink_scan(1), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_shrink_skipped(), skipped0);

	/* No swap in the KUnit boot: the pick is kept -- the folio
	 * reference returns and every shape survives.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	pmdp = corten_arena_test_pmd(mm, addr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get(ptep);
	KUNIT_EXPECT_TRUE(test, pte_present(pte));
	folio = page_folio(pte_page(pte));
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 1);
	pte_unmap_unlock(ptep, ptl);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* Kernel-side word access to one arena page: the fault itself runs
 * through the arena entry (the only legal producer), the payload rides
 * kmap -- a copy_to_user()/copy_from_user() in a kernel thread cannot
 * fault a VMA-less window in (the x86 arena hook gates on
 * user_mode(regs)), so the uaccess route only works for pages already
 * present.  @write stamps *@val into the page, else reads it back.
 */
static int corten_arena_test_page_word(struct mm_struct *mm,
					unsigned long addr, u64 *val,
					bool write)
{
	pmd_t *pmdp;
	pte_t *ptep, pte;
	spinlock_t *ptl;
	struct folio *folio;
	u64 *kaddr;
	int ret = 0;

	pmdp = corten_arena_test_pmd(mm, addr);
	if (!pmdp)
		return -ENOENT;
	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	if (!ptep)
		return -EAGAIN;
	pte = ptep_get(ptep);
	if (!pte_present(pte) || pte_special(pte)) {
		pte_unmap_unlock(ptep, ptl);
		return -ENOENT;
	}
	folio = page_folio(pte_page(pte));
	folio_get(folio);
	pte_unmap_unlock(ptep, ptl);

	kaddr = kmap_local_folio(folio, addr & (PAGE_SIZE - 1));
	if (write)
		*kaddr = *val;
	else
		*val = *kaddr;
	kunmap_local(kaddr);
	folio_put(folio);

	return ret;
}

/* C1 回归锚: fork 子 carrier 的 anon_vma 链必须是 dup_mmap 同形 --
 * 恰好两条（克隆到父 carrier 的 anon_vma + 子自己的 fork 根）, 绝无
 * 第三条来自 alloc 侧 prepare 的孤儿链。  孤儿根的 num_active_vmas
 * 无人回减, unlink_anon_vmas() pass-2 在 CONFIG_DEBUG_VM 下 WARN
 * （本轮基线实测还先触发 rmap.c lock_anon_vma_root 的 WARN_ON_ONCE
 * (root) -- 同一根因）; 裸化（fork 侧 alloc 跳过 prepare）后链形
 * 唯一, 两个锚的 active 计数各自恰好为 1。
 */
static void corten_arena_test_child_carrier_chain(struct kunit *test,
						  struct vm_area_struct *ccarrier,
						  struct vm_area_struct *pcarrier)
{
	struct anon_vma_chain *avc;
	int nr = 0;
	bool parent_link = false, own_link = false;

	KUNIT_ASSERT_NOT_NULL(test, ccarrier->anon_vma);
	KUNIT_ASSERT_NOT_NULL(test, pcarrier->anon_vma);
	KUNIT_EXPECT_TRUE(test, ccarrier->anon_vma != pcarrier->anon_vma);

	list_for_each_entry(avc, &ccarrier->anon_vma_chain, same_vma) {
		nr++;
		if (avc->anon_vma == pcarrier->anon_vma)
			parent_link = true;
		if (avc->anon_vma == ccarrier->anon_vma)
			own_link = true;
	}
	KUNIT_EXPECT_EQ(test, nr, 2);
	KUNIT_EXPECT_TRUE(test, parent_link);
	KUNIT_EXPECT_TRUE(test, own_link);
	KUNIT_EXPECT_TRUE(test,
			  ccarrier->anon_vma->parent == pcarrier->anon_vma);
	KUNIT_EXPECT_TRUE(test,
			  ccarrier->anon_vma->root == pcarrier->anon_vma->root);
	/* Each anchor's active count is exactly its one carrier -- the
	 * WARN shape is an orphan with a count nothing will drop.
	 */
	KUNIT_EXPECT_EQ(test, ccarrier->anon_vma->num_active_vmas, 1);
	KUNIT_EXPECT_EQ(test, pcarrier->anon_vma->num_active_vmas, 1);
}

/* F1 回归锚（阻断项）: declare→park→re-declare 混合构造的 fork 内容
 * 继承。  修复前 reactivate 对 declare 侧（novma=false）不重武装
 * carrier -- 活窗无锚, fork_mirror 既不跳过也不复制（copy_page_range
 * 无源）, 子窗真空, 首次 fault 拿全新页: fork 对私有匿名内存的继承
 * 语义被静默破坏（A.1 的 novma 复制臂删除后该形状失去一切覆盖）。
 * 修复后 reactivate 无条件重武装: 子窗逐页内容非空且与父窗 checksum
 * 一致（M5 判据 1 口径）, 父窗内容 fork 后原样。
 */
static void corten_arena_test_fork_redeclare_content(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm, *child;
	struct vm_area_struct *pcarrier, *ccarrier;
	struct corten_pte_meta m;
	unsigned long addr;
	long timeouts = corten_arena_test_drain_timeouts();
	unsigned int fault_flags = FAULT_FLAG_WRITE;
	u64 seed[CORTEN_ARENA_TEST_PAGES * 2];
	u64 parent[CORTEN_ARENA_TEST_PAGES * 2];
	u64 got;
	int i;

	if (!corten_enabled_static())
		kunit_skip(test, "fork mirror requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	/* The first incarnation is a targeted DECLARE: the harness VMA
	 * declares the whole range (tree shadow, no carrier).
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN), 0);

	/* Park: the EXACT munmap route, tree shadow removed with it.
	 * (Full-arena coverage: a PAGE_SIZE munmap is a CHUNK zap and
	 * leaves the arena live -- the re-declare below would then
	 * overlap-reject.  The test's documented intent is the park.)
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 CORTEN_ARENA_TEST_BASE,
						 CORTEN_ARENA_TEST_LEN), 1);

	/* Re-declare the same window: declare_locked's pool probe
	 * short-circuits the reactivation before any vma_lookup -- F1
	 * makes it re-arm the carrier unconditionally, so the live
	 * window is VMA-less but anchored.
	 */
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     CORTEN_ARENA_TEST_LEN), 0);
	pcarrier = corten_arena_test_carrier_of(mm, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pcarrier);
	mmap_read_lock(mm);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_BASE));
	mmap_read_unlock(mm);

	/* Content: FRESH write faults through the carrier anchor, then
	 * a distinct pattern at both ends of every page of the first
	 * window (page-granular, per-page checksum).
	 */
	for (i = 0; i < CORTEN_ARENA_TEST_PAGES; i++) {
		addr = CORTEN_ARENA_TEST_BASE + i * PAGE_SIZE;
		fault_flags = FAULT_FLAG_WRITE;
		KUNIT_ASSERT_EQ(test,
				corten_arena_user_fault(mm, addr, 0, NULL,
							&fault_flags),
				CORTEN_FAULT_HANDLED);
		seed[i * 2] = 0xD00DFEED00000000ULL | (unsigned int)i;
		seed[i * 2 + 1] = ~seed[i * 2];
		KUNIT_ASSERT_EQ(test,
				corten_arena_test_page_word(mm, addr,
							    &seed[i * 2],
							    true), 0);
		KUNIT_ASSERT_EQ(test,
				corten_arena_test_page_word(mm,
							    addr + PAGE_SIZE -
							    sizeof(u64),
							    &seed[i * 2 + 1],
							    true), 0);
	}

	/* The parent snapshot (the checksum the child must reproduce). */
	for (i = 0; i < CORTEN_ARENA_TEST_PAGES * 2; i++) {
		unsigned long a = CORTEN_ARENA_TEST_BASE +
				  (i / 2) * PAGE_SIZE +
				  (i % 2 ? PAGE_SIZE - sizeof(u64) : 0);

		KUNIT_ASSERT_EQ(test,
				corten_arena_test_page_word(mm, a,
							    &parent[i],
							    false), 0);
		KUNIT_EXPECT_NE(test, parent[i], 0ULL);
		KUNIT_EXPECT_EQ(test, parent[i], seed[i]);
	}

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);

	/* No parked arenas left (the slot was reactivated), so the flush
	 * is a no-op and the mirror runs inline like fork_vma_free; no
	 * child VMA exists -- the carrier pair carries the copy.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);

	KUNIT_EXPECT_EQ(test, corten_arena_query(child,
						 CORTEN_ARENA_TEST_BASE), 1);
	ccarrier = corten_arena_test_carrier_of(child, CORTEN_ARENA_TEST_BASE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ccarrier);

	/* C1: the child carrier's chain is the dup_mmap shape. */
	corten_arena_test_child_carrier_chain(test, ccarrier, pcarrier);

	/* 逐页校验: 子窗每页两端内容非空且与父窗 checksum 一致 --
	 * the pre-fix shape answered zero pages (silent data loss).
	 */
	for (i = 0; i < CORTEN_ARENA_TEST_PAGES * 2; i++) {
		unsigned long a = CORTEN_ARENA_TEST_BASE +
				  (i / 2) * PAGE_SIZE +
				  (i % 2 ? PAGE_SIZE - sizeof(u64) : 0);

		KUNIT_ASSERT_EQ(test,
				corten_arena_test_page_word(child, a, &got,
							    false), 0);
		KUNIT_EXPECT_EQ(test, got, parent[i]);
	}
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_meta(child,
					       CORTEN_ARENA_TEST_BASE,
					       &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_TRUE(test, m.flags & CORTEN_PF_SHARED);
	KUNIT_EXPECT_TRUE(test,
			  corten_arena_test_pt_present(child,
						       CORTEN_ARENA_TEST_BASE));

	/* The parent kept its bytes through the COW wrprotect. */
	for (i = 0; i < CORTEN_ARENA_TEST_PAGES * 2; i++) {
		unsigned long a = CORTEN_ARENA_TEST_BASE +
				  (i / 2) * PAGE_SIZE +
				  (i % 2 ? PAGE_SIZE - sizeof(u64) : 0);

		KUNIT_ASSERT_EQ(test,
				corten_arena_test_page_word(mm, a, &got,
							    false), 0);
		KUNIT_EXPECT_EQ(test, got, parent[i]);
	}

	KUNIT_EXPECT_TRUE(test, corten_region_invariants_ok(child));

	mmput(child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* V-A.2a: the mmap_region() verification checklist the takeover now
 * runs itself -- the truth table over (def_flags, prot, limits).
 * SPEC sec 3.1.2 wants >= 16 rows: 8 prot encodings x {def_flags
 * VM_LOCKED, none} x {RLIMIT_AS normal / exactly-enough / zero} = 48,
 * plus the caller-side len2 rounding equivalence below.
 */
static void corten_arena_test_auto_validate(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	unsigned long flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	static const unsigned long prots[] = {
		PROT_NONE,
		PROT_READ,
		PROT_WRITE,
		PROT_READ | PROT_WRITE,
		PROT_EXEC,
		PROT_READ | PROT_EXEC,
		PROT_WRITE | PROT_EXEC,
		PROT_READ | PROT_WRITE | PROT_EXEC,
	};
	struct rlimit saved_as = current->signal->rlim[RLIMIT_AS];
	unsigned long total = mm->total_vm;
	unsigned long len = PMD_SIZE;
	int i, j, k, cases = 0;

	/* The verdict precedence is pinned exactly as the gate encodes
	 * it: mlockall first (-EAGAIN), execute-only second
	 * (-EOPNOTSUPP), the may_expand_vm() verdict last.  The
	 * exactly-enough row pins the > (not >=) semantics of the
	 * legacy limit check.
	 */
	for (i = 0; i < ARRAY_SIZE(prots); i++) {
		bool exec_only = prots[i] == PROT_EXEC;

		for (j = 0; j < 2; j++) {
			mm->def_flags = j ? VM_LOCKED : 0;

			for (k = 0; k < 3; k++) {
				int expect;

				switch (k) {
				case 0:
					current->signal->rlim[RLIMIT_AS].rlim_cur =
						RLIM_INFINITY;
					expect = 0;
					break;
				case 1:
					current->signal->rlim[RLIMIT_AS].rlim_cur =
						(total +
						 (len >> PAGE_SHIFT)) <<
						PAGE_SHIFT;
					expect = 0;
					break;
				default:
					current->signal->rlim[RLIMIT_AS].rlim_cur = 0;
					expect = -ENOMEM;
					break;
				}
				if (j)
					expect = -EAGAIN;
				else if (exec_only)
					expect = -EOPNOTSUPP;

				KUNIT_EXPECT_EQ(test,
						corten_auto_validate(mm, len,
								     prots[i],
								     flags),
						expect);
				cases++;
			}
		}
	}
	mm->def_flags = 0;
	KUNIT_EXPECT_GE(test, cases, 16);

	/* Caller-side rounding equivalence: the route feeds
	 * round_up(*lenp, PMD_SIZE), so len 1 and PMD_SIZE-1 must gate
	 * identically to the rounded window in every regime.
	 */
	for (k = 0; k < 2; k++) {
		unsigned long lens[2] = {
			round_up(1UL, PMD_SIZE),
			round_up(PMD_SIZE - 1, PMD_SIZE),
		};

		current->signal->rlim[RLIMIT_AS].rlim_cur =
			k ? 0 : RLIM_INFINITY;
		for (i = 0; i < 2; i++)
			KUNIT_EXPECT_EQ(test,
					corten_auto_validate(mm, lens[i],
							     PROT_READ |
							     PROT_WRITE,
							     flags),
					k ? -ENOMEM : 0);
	}
	current->signal->rlim[RLIMIT_AS] = saved_as;

	/* The gate is pure: nothing was charged or placed. */
	KUNIT_EXPECT_EQ(test, mm->total_vm, total);
	KUNIT_EXPECT_NULL(test, READ_ONCE(mm->corten_state));
}

/* V-A.2b: the detached carrier -- creation shape, detachment
 * assertions, the total_vm charge the declare now owes, and the
 * lifecycle (RELEASE frees the carrier; park/reactivate keeps it).
 */
static void corten_arena_test_carrier_vma(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct vm_area_struct *carrier;
	struct corten_arena *ar;
	/* mm_setup's harness VMA is charged (the park's do_munmap uncharge
	 * must land on a charged counter) -- the deltas below are on top
	 * of this baseline.
	 */
	unsigned long base_vm = mm->total_vm;
	long carriers0 = corten_arena_test_carriers();

	if (!corten_enabled_static())
		kunit_skip(test, "carrier lifecycle requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_pool_attach(mm, CORTEN_ARENA_TEST_WIN,
						      PMD_SIZE), 0);

	carrier = corten_arena_test_carrier_of(mm, CORTEN_ARENA_TEST_WIN);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, carrier);
	KUNIT_EXPECT_EQ(test, corten_arena_test_carriers(), carriers0 + 1);

	/* Detached: not in the tree (J1), never attached (upstream's own
	 * detached refcount shape), bound to the arena extent.
	 */
	mmap_read_lock(mm);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, CORTEN_ARENA_TEST_WIN));
	mmap_read_unlock(mm);
	KUNIT_EXPECT_EQ(test, refcount_read(&carrier->vm_refcnt), 0);
	KUNIT_EXPECT_EQ(test, carrier->vm_start, CORTEN_ARENA_TEST_WIN);
	KUNIT_EXPECT_EQ(test, carrier->vm_end,
			CORTEN_ARENA_TEST_WIN + PMD_SIZE);

	/* The anchor shape: private anonymous, CORTEN+NOHUGEPAGE, the
	 * full private MAY bound; the anon_vma chain is prepared (the
	 * rmap anchor duties).
	 */
	KUNIT_EXPECT_TRUE(test, vma_is_anonymous(carrier));
	KUNIT_EXPECT_TRUE(test, carrier->vm_flags & VM_CORTEN);
	KUNIT_EXPECT_TRUE(test, carrier->vm_flags & VM_NOHUGEPAGE);
	KUNIT_EXPECT_TRUE(test, carrier->vm_flags & VM_MAYREAD);
	KUNIT_EXPECT_TRUE(test, carrier->vm_flags & VM_MAYWRITE);
	KUNIT_EXPECT_TRUE(test, carrier->vm_flags & VM_MAYEXEC);
	KUNIT_EXPECT_TRUE(test, carrier->vm_flags & VM_NORESERVE);
	KUNIT_EXPECT_NOT_NULL(test, carrier->anon_vma);

	/* The accounting mmap_region() used to do. */
	KUNIT_EXPECT_EQ(test, mm->total_vm,
			base_vm + (PMD_SIZE >> PAGE_SHIFT));

	/* The record knows its anchor; the descriptor's tree cache is
	 * empty (the two anchor kinds are mutually exclusive).
	 */
	rcu_read_lock();
	ar = xa_load(&READ_ONCE(mm->corten_state)->arenas,
		     CORTEN_ARENA_TEST_WIN >> PMD_SHIFT);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ar);
	if (ar) {
		KUNIT_EXPECT_PTR_EQ(test, READ_ONCE(ar->carrier), carrier);
		KUNIT_EXPECT_NULL(test, READ_ONCE(ar->vma));
	}
	rcu_read_unlock();

	/* RELEASE frees the carrier with the descriptor. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_release(mm, CORTEN_ARENA_TEST_WIN,
					     PMD_SIZE), 0);
	KUNIT_EXPECT_NULL(test,
			  corten_arena_test_carrier_of(mm,
						       CORTEN_ARENA_TEST_WIN));
	mmap_read_lock(mm);
	KUNIT_EXPECT_EQ(test, mm->total_vm, base_vm);
	mmap_read_unlock(mm);
	/* The counter is cumulative creations, not liveness -- the free
	 * shows up as the descriptor's carrier probe going NULL above.
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_carriers(), carriers0 + 1);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* B2 回归锚: 部分 punch + 缓存 NULL（punch_split 全吞形态）+ fork。
 * 修复前 fork_mirror 的窗口门只看 arena 级缓存指针 → 对 dup_mmap
 * 已复制的幸存片再复制（每页双 folio_get + 计数双记）; 修复后窗口级
 * 树 VMA 覆盖检查放行恰好一次。
 */
static void corten_arena_test_fork_punch_novma(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm, *child;
	struct vm_area_struct *vma, *head, *tail, *cvma;
	struct corten_arena *ar;
	struct corten_mm_state *state;
	struct corten_pte_meta m;
	struct folio *folio;
	struct page *page;
	unsigned long addr = CORTEN_ARENA_TEST_BASE + PAGE_SIZE;
	unsigned long split = CORTEN_ARENA_TEST_BASE + PMD_SIZE;
	long timeouts = corten_arena_test_drain_timeouts();

	if (!corten_enabled_static())
		kunit_skip(test, "fork mirror requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	vma = corten_arena_test_mkvm(mm, CORTEN_ARENA_TEST_BASE,
				     CORTEN_ARENA_TEST_BASE + 2 * PMD_SIZE,
				     CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vma);
	KUNIT_ASSERT_EQ(test,
			corten_arena_declare(mm, CORTEN_ARENA_TEST_BASE,
					     2 * PMD_SIZE), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_fill_window(mm, addr), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_fork_seed_mapped(mm, addr), 0);

	/* End state of punch①+②: W2 is a punched hole (plain VMA, frame
	 * erased) and the W1 piece survives as VM_CORTEN while the cached
	 * pointer went NULL (the punch_split full-swallow arm).
	 */
	mmap_write_lock(mm);
	{
		VMA_ITERATOR(vmi, mm, split);

		KUNIT_ASSERT_EQ(test, __split_vma(&vmi, vma, split, 0), 0);
		tail = vma_lookup(mm, split);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tail);
		vm_flags_clear(tail, VM_CORTEN | VM_NOHUGEPAGE);
		head = vma_lookup(mm, CORTEN_ARENA_TEST_BASE);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, head);
		KUNIT_ASSERT_TRUE(test, head->vm_flags & VM_CORTEN);

		state = corten_arena_state(mm);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);
		mutex_lock(&state->ctl_lock);
		xa_erase(&state->arenas, split >> PMD_SHIFT);
		mutex_unlock(&state->ctl_lock);
	}
	mmap_write_unlock(mm);
	rcu_read_lock();
	ar = xa_load(&state->arenas,
		     CORTEN_ARENA_TEST_BASE >> PMD_SHIFT);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ar);
	if (ar)
		WRITE_ONCE(ar->vma, NULL);
	rcu_read_unlock();

	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);

	/* The child inherits BOTH tree VMAs (head piece + punched-hole
	 * plain VMA): dup_mmap() copies the head piece via the corten
	 * divert -- exactly once.
	 */
	cvma = corten_arena_test_mkvm(child, CORTEN_ARENA_TEST_BASE, split,
				      CORTEN_ARENA_TEST_FLAGS_OK | VM_CORTEN |
				      VM_NOHUGEPAGE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	cvma = corten_arena_test_mkvm(child, split,
				      CORTEN_ARENA_TEST_BASE + 2 * PMD_SIZE,
				      CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cvma);
	KUNIT_EXPECT_NOT_NULL(test,
			      corten_arena_test_fork_copy_pte(test, child, mm,
							      addr));
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);

	/* The surviving piece's page was copied exactly once: the child
	 * holds one reference (parent PTE + child PTE = 2), and the
	 * child's anon counter moved by exactly one page.  The pre-B2
	 * code double-copied here (ref 3, counter +2).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_query(child,
						 CORTEN_ARENA_TEST_BASE), 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(child, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	KUNIT_EXPECT_TRUE(test, m.flags & CORTEN_PF_SHARED);
	/* Counter discriminator (the harness's copy_pte does not count;
	 * the explicit arm does): pre-B2 the arm double-copied the window
	 * and the counter read 1; single-copy leaves it 0.
	 */
	KUNIT_EXPECT_EQ(test, (long)get_mm_counter(child, MM_ANONPAGES), 0);

	KUNIT_EXPECT_EQ(test, corten_arena_test_meta(mm, addr, &m), 0);
	KUNIT_EXPECT_EQ(test, m.state, CORTEN_MAPPED);
	{
		pmd_t *pmdp = corten_arena_test_pmd(mm, addr);
		pte_t *ptep;
		spinlock_t *ptl;

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pmdp);
		ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
		page = pte_page(ptep_get(ptep));
		pte_unmap_unlock(ptep, ptl);
	}
	folio = page_folio(page);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 2);

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
	struct corten_arena synth = { };
	struct vm_area_struct *vma;
	struct corten_arena *ar, *ar2;

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
	 * satisfies may_prot >= prot by construction.  V-A.1: the park
	 * shape pairs RESERVED with idle (the INV-MV3 record check),
	 * so set the parked flag exactly like the park does.
	 */
	WRITE_ONCE(synth.idle, true);
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
	unsigned long addr = 0, len = PMD_SIZE, flags;
	int ret;

	if (!corten_enabled_static())
		kunit_skip(test, "park routing requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* V-A.2a: the takeover is VMA-less (the pre-A.2 harness mkvm is
	 * gone, as in fork_vma_free) -- a hand-linked tree VMA surviving
	 * the park is the foreign-VMA-in-a-parked-window shape P4 (V-A.3a)
	 * ejects on handout, not a legal state this test may build.
	 */
	mmap_write_lock(mm);
	ret = corten_arena_auto_attach(mm, CORTEN_ARENA_TEST_WIN, PMD_SIZE, PROT_READ | PROT_WRITE);
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

/* ------------------------------------------------------------------ *
 * V-C (MV_VMA_FREE_SPEC.md sec 3.3): the dual-source consumers -- the
 * window row stream (/proc maps rendering source), the GUP-slow MODE
 * probe and the smaps/pagemap aggregations.  All cases run the real
 * producers (auto route + attach, munmap-route park, punch-shaped
 * frame erase + implant mark).
 * ------------------------------------------------------------------
 */

/* One window region via the real takeover pair (route + attach),
 * placed wherever the magazine hands the next window.
 */
static unsigned long corten_arena_test_mvc_attach(struct kunit *test,
						  struct mm_struct *mm,
						  unsigned long frames,
						  unsigned long prot)
{
	unsigned long addr = 0, len = frames * PMD_SIZE;
	unsigned long flags = MAP_PRIVATE | MAP_ANONYMOUS;

	KUNIT_ASSERT_EQ(test,
			corten_arena_test_auto_route_locked(mm, len, prot,
						     &addr, &len, &flags), 1);
	mmap_write_lock(mm);
	KUNIT_ASSERT_EQ(test, corten_arena_auto_attach(mm, addr,
						       frames * PMD_SIZE,
						       prot), 0);
	mmap_write_unlock(mm);

	return addr;
}

static void corten_arena_test_mvc_row_stream(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct corten_mm_state *state;
	struct corten_row_iter it;
	struct corten_region_row row;
	unsigned long a, b, c, rows_before;

	if (!corten_enabled_static())
		kunit_skip(test, "row stream requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	a = corten_arena_test_mvc_attach(test, mm, 2, PROT_READ | PROT_WRITE);
	b = corten_arena_test_mvc_attach(test, mm, 1, PROT_READ);
	c = corten_arena_test_mvc_attach(test, mm, 1, 0);
	KUNIT_ASSERT_EQ(test, b, a + 2 * PMD_SIZE);
	KUNIT_ASSERT_EQ(test, c, b + PMD_SIZE);
	/* With regions live the /proc walk is dual-source. */
	KUNIT_ASSERT_TRUE(test, corten_maps_dual_source(mm));

	/* Order + exact field shapes (the J3 render inputs): two frames
	 * of one region dedup into a single row; prot/rclass derive from
	 * the record; the carrier is the render context.
	 */
	rows_before = corten_arena_test_maps_window_rows();
	mmap_read_lock(mm);
	corten_row_iter_init(&it);

	KUNIT_EXPECT_TRUE(test, corten_row_next(mm, &it, &row));
	KUNIT_EXPECT_EQ(test, row.start, a);
	KUNIT_EXPECT_EQ(test, row.end, a + 2 * PMD_SIZE);
	KUNIT_EXPECT_EQ(test, row.ar->rclass, CORTEN_REGION_ANON);
	KUNIT_EXPECT_EQ(test, row.ar->prot,
			CORTEN_PERM_USER | CORTEN_PERM_READ | CORTEN_PERM_WRITE);
	KUNIT_EXPECT_NOT_NULL(test, row.ar->carrier);

	KUNIT_EXPECT_TRUE(test, corten_row_next(mm, &it, &row));
	KUNIT_EXPECT_EQ(test, row.start, b);
	KUNIT_EXPECT_EQ(test, row.end, b + PMD_SIZE);
	KUNIT_EXPECT_EQ(test, row.ar->prot,
			CORTEN_PERM_USER | CORTEN_PERM_READ);

	KUNIT_EXPECT_TRUE(test, corten_row_next(mm, &it, &row));
	KUNIT_EXPECT_EQ(test, row.start, c);
	KUNIT_EXPECT_EQ(test, row.end, c + PMD_SIZE);
	KUNIT_EXPECT_EQ(test, row.ar->prot, CORTEN_PERM_USER);
	KUNIT_EXPECT_FALSE(test, corten_row_next(mm, &it, &row));

	/* Covering-or-next: mid-region hits the covering row, a region
	 * end advances to the next row, past the stream is false.
	 */
	KUNIT_EXPECT_TRUE(test, corten_row_query(mm, b, &row));
	KUNIT_EXPECT_EQ(test, row.start, b);
	KUNIT_EXPECT_TRUE(test, corten_row_query(mm, b + PMD_SIZE, &row));
	KUNIT_EXPECT_EQ(test, row.start, c);
	KUNIT_EXPECT_FALSE(test,
			   corten_row_query(mm, c + 2 * PMD_SIZE, &row));
	mmap_read_unlock(mm);
	/* 3 streamed rows + 2 query resolutions (b covers itself; the
	 * b-end query lands straight on c's frame).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_maps_window_rows(),
			rows_before + 5);

	/* Punch shape: erase the region's second frame and register the
	 * implant over it (the punch route's end state) -- the row must
	 * split, exactly like the shadow era's split tree VMA rows.
	 */
	state = corten_arena_state(mm);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);
	mmap_write_lock(mm);
	mutex_lock(&state->ctl_lock);
	xa_erase(&state->arenas, (a + PMD_SIZE) >> PMD_SHIFT);
	mutex_unlock(&state->ctl_lock);
	corten_implant_mark(mm, a + PMD_SIZE, PMD_SIZE);
	mmap_write_unlock(mm);

	mmap_read_lock(mm);
	corten_row_iter_init(&it);
	KUNIT_EXPECT_TRUE(test, corten_row_next(mm, &it, &row));
	KUNIT_EXPECT_EQ(test, row.start, a);
	KUNIT_EXPECT_EQ(test, row.end, a + PMD_SIZE);
	KUNIT_EXPECT_TRUE(test, corten_row_next(mm, &it, &row));
	KUNIT_EXPECT_EQ(test, row.start, b);
	mmap_read_unlock(mm);

	/* S-4: a parked window does not render. */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 b, PAGE_SIZE), 1);
	KUNIT_EXPECT_TRUE(test, corten_arena_test_pool_idle(mm, b));

	mmap_read_lock(mm);
	corten_row_iter_init(&it);
	KUNIT_EXPECT_TRUE(test, corten_row_next(mm, &it, &row));
	KUNIT_EXPECT_EQ(test, row.start, a);
	KUNIT_EXPECT_TRUE(test, corten_row_next(mm, &it, &row));
	KUNIT_EXPECT_EQ(test, row.start, c);
	KUNIT_EXPECT_FALSE(test, corten_row_next(mm, &it, &row));
	mmap_read_unlock(mm);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* The J3 first-row anchor (the m_start/proc_get_vma merge cursor,
 * fs/proc/task_mmu.c): a delegated-domain VMA ending exactly where the
 * FIRST window region begins, plus a second region above it -- the
 * oracle's delegated face.  The cursor's emission path leans on three
 * stream contracts, each pinned here against the real producers:
 *   1. a fresh prime (pos 0) heads at the FIRST region's row;
 *   2. the merged two-stream order is [delegated, row1, row2]: the row
 *	head is held while the delegated VMA wins, and the outbid tree
 *	head is re-fetched (iterator re-pinned to the row's end), never
 *	swallowed -- driving the cursor's discipline below emits exactly
 *	that sequence, no duplicates, no omissions;
 *   3. the restart/rewind dedup (corten_maps_prime's shape): a
 *	re-prime at the delegated VMA's end -- the position m_start
 *	rewinds a seq refill to -- reproduces row1, not row2, and each
 *	emitted row's end re-primes to exactly the remaining suffix.
 */
static void corten_arena_test_mvc_merge_first_row(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct vm_area_struct *dv;
	struct corten_row_iter it;
	struct corten_region_row row, peek;
	unsigned long a1, a2, pos;
	unsigned long order[5];
	int n = 0;
	bool valid;

	if (!corten_enabled_static())
		kunit_skip(test, "row stream requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* The delegated face: a plain tree VMA whose end is the first
	 * window region's start -- the maple tree's last row immediately
	 * below the window stream's first one.
	 */
	dv = corten_arena_test_mkvm(mm, CORTEN_MODE_WINDOW_START - PMD_SIZE,
				    CORTEN_MODE_WINDOW_START,
				    CORTEN_ARENA_TEST_FLAGS_OK);
	KUNIT_ASSERT_NOT_NULL(test, dv);

	/* Region 1 spans 4 frames (the oracle's 8M RW shape) and is the
	 * delegated VMA's immediate right neighbor; region 2 rides above.
	 */
	a1 = corten_arena_test_mvc_attach(test, mm, 4, PROT_READ | PROT_WRITE);
	a2 = corten_arena_test_mvc_attach(test, mm, 1, PROT_READ | PROT_WRITE);
	KUNIT_ASSERT_EQ(test, a1, CORTEN_MODE_WINDOW_START);
	KUNIT_ASSERT_EQ(test, dv->vm_end, a1);
	KUNIT_ASSERT_EQ(test, a2, a1 + 4 * PMD_SIZE);
	KUNIT_ASSERT_TRUE(test, corten_maps_dual_source(mm));

	mmap_read_lock(mm);

	/* (1) fresh prime: the stream heads at the FIRST region's row. */
	corten_row_iter_init(&it);
	KUNIT_ASSERT_TRUE(test, corten_row_next(mm, &it, &row));
	KUNIT_EXPECT_EQ(test, row.start, a1);
	KUNIT_EXPECT_EQ(test, row.end, a1 + 4 * PMD_SIZE);

	/* (2) the merged emission order, driven with proc_get_vma's exact
	 * discipline: fetch the tree head, row wins while it starts below
	 * (or the tree is exhausted), and a row emission re-pins the tree
	 * iterator so the outbid head returns on the next fetch.  The
	 * harness's own base VMA rides below everything as a second
	 * delegated row.
	 */
	{
		VMA_ITERATOR(vmi, mm, 0);

		corten_row_iter_init(&it);
		it.rit.frame = 0;
		valid = false;
		while (corten_row_next(mm, &it, &peek)) {
			if (peek.end > 0) {
				valid = true;
				break;
			}
		}
		KUNIT_ASSERT_TRUE(test, valid);

		for (;;) {
			struct vm_area_struct *vma = vma_next(&vmi);

			if (valid &&
			    (!vma || peek.start < vma->vm_start)) {
				row = peek;
				valid = corten_row_next(mm, &it, &peek);
				order[n++] = row.start;
				vma_iter_set(&vmi, row.end);
				continue;
			}
			if (!vma)
				break;
			order[n++] = vma->vm_start;
		}
	}
	KUNIT_EXPECT_EQ(test, n, 4);
	KUNIT_EXPECT_EQ(test, order[0], CORTEN_ARENA_TEST_BASE);
	KUNIT_EXPECT_EQ(test, order[1], dv->vm_start);
	KUNIT_EXPECT_EQ(test, order[2], a1);
	KUNIT_EXPECT_EQ(test, order[3], a2);

	/* (3) the restart/rewind dedup, at every entry boundary: the
	 * delegated VMA's end (the rewind target right after the tree's
	 * last row -- where the oracle lost its 8M first row) reproduces
	 * row1; each row's end re-primes to exactly the remaining suffix.
	 */
	pos = dv->vm_end;
	corten_row_iter_init(&it);
	it.rit.frame = pos >> PMD_SHIFT;
	valid = false;
	while (corten_row_next(mm, &it, &row)) {
		if (row.end > pos) {
			valid = true;
			break;
		}
	}
	KUNIT_EXPECT_TRUE(test, valid);
	KUNIT_EXPECT_EQ(test, row.start, a1);

	pos = a1 + 4 * PMD_SIZE;
	corten_row_iter_init(&it);
	it.rit.frame = pos >> PMD_SHIFT;
	valid = false;
	while (corten_row_next(mm, &it, &row)) {
		if (row.end > pos) {
			valid = true;
			break;
		}
	}
	KUNIT_EXPECT_TRUE(test, valid);
	KUNIT_EXPECT_EQ(test, row.start, a2);

	pos = a2 + PMD_SIZE;
	corten_row_iter_init(&it);
	it.rit.frame = pos >> PMD_SHIFT;
	valid = false;
	while (corten_row_next(mm, &it, &row)) {
		if (row.end > pos) {
			valid = true;
			break;
		}
	}
	KUNIT_EXPECT_FALSE(test, valid);

	mmap_read_unlock(mm);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

static void corten_arena_test_mvc_gup_probe(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct vm_area_struct *carrier, *probe;
	struct file *file;
	unsigned long a, faddr, flen, fflags;
	long probes, rejects;

	if (!corten_enabled_static())
		kunit_skip(test, "gup probe requires corten=on");

	/* Pre-MODE: inert (NULL, the legacy walk answers). */
	KUNIT_EXPECT_TRUE(test,
			  IS_ERR_OR_NULL(corten_gup_probe(mm,
							  CORTEN_MODE_WINDOW_START,
							  0)));

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	a = corten_arena_test_mvc_attach(test, mm, 1, PROT_READ | PROT_WRITE);

	probes = corten_arena_test_gup_probes();
	rejects = corten_arena_test_gup_probe_rejects();

	/* Below-window and above-window addresses stay the tree's. */
	mmap_read_lock(mm);
	KUNIT_EXPECT_NULL(test, corten_gup_probe(mm, CORTEN_ARENA_TEST_BASE, 0));
	KUNIT_EXPECT_NULL(test, corten_gup_probe(mm, CORTEN_MODE_WINDOW_END, 0));

	/* Active region: the carrier (check_vma_flags' corten_own arm,
	 * the PTE follow and the faultin hook all consume it verbatim).
	 */
	carrier = corten_arena_test_carrier_of(mm, a);
	KUNIT_ASSERT_NOT_NULL(test, carrier);
	probe = corten_gup_probe(mm, a, 0);
	KUNIT_EXPECT_FALSE(test, IS_ERR_OR_NULL(probe));
	KUNIT_EXPECT_PTR_EQ(test, probe, carrier);
	probe = corten_gup_probe(mm, a, FOLL_WRITE);
	KUNIT_EXPECT_FALSE(test, IS_ERR_OR_NULL(probe));
	KUNIT_EXPECT_PTR_EQ(test, probe, carrier);

	/* Parked/hole: the loud window reject (find_vma()'s own errno). */
	KUNIT_EXPECT_PTR_EQ(test, corten_gup_probe(mm, a + 8 * PMD_SIZE, 0),
			    ERR_PTR(-EFAULT));
	mmap_read_unlock(mm);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 a, PAGE_SIZE), 1);
	mmap_read_lock(mm);
	KUNIT_EXPECT_PTR_EQ(test, corten_gup_probe(mm, a, 0),
			    ERR_PTR(-EFAULT));

	/* Implant range: NULL -- find_vma() must see the real tree VMA
	 * (the registry mark alone decides, no VMA needed here).
	 */
	corten_implant_mark(mm, a, PMD_SIZE);
	KUNIT_EXPECT_NULL(test, corten_gup_probe(mm, a, 0));
	mmap_read_unlock(mm);

	KUNIT_EXPECT_EQ(test, corten_arena_test_gup_probes(), probes + 2);
	KUNIT_EXPECT_EQ(test, corten_arena_test_gup_probe_rejects(),
			rejects + 2);

	/* FOLL_ANON on a FILE region: the one emulation corner the
	 * carrier's anon shape cannot express (-EFAULT, the
	 * vma_is_anonymous() verdict).  The FILE takeover pair (route +
	 * file attach) places it wherever the window serves next -- the
	 * parked frame above is pool-recycled, so the address is read
	 * back from the route instead of assumed.
	 */
	file = shmem_file_setup("corten_mvc", PMD_SIZE, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	faddr = 0;
	flen = 2 * PAGE_SIZE;
	fflags = MAP_PRIVATE;
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_file_route(mm, file, flen, &faddr,
						     &flen, &fflags), 1);
	mmap_write_lock(mm);
	KUNIT_ASSERT_EQ(test,
			corten_arena_file_attach(mm, faddr, PMD_SIZE,
						 PROT_READ, file, 1), 0);
	mmap_write_unlock(mm);
	fput(file);

	mmap_read_lock(mm);
	probe = corten_gup_probe(mm, faddr, 0);
	KUNIT_EXPECT_FALSE(test, IS_ERR_OR_NULL(probe));
	probe = corten_gup_probe(mm, faddr, FOLL_ANON);
	KUNIT_EXPECT_TRUE(test, IS_ERR(probe));
	KUNIT_EXPECT_PTR_EQ(test, probe, ERR_PTR(-EFAULT));
	mmap_read_unlock(mm);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/* pagemap emit sink: records up to 8 entries. */
struct corten_arena_test_pm_sink {
	u64 pme[8];
	int n;
};

static int corten_arena_test_pm_emit(void *ctx, u64 pme)
{
	struct corten_arena_test_pm_sink *s = ctx;

	if (s->n >= 8)
		return 1;
	s->pme[s->n++] = pme;

	return 0;
}

static void corten_arena_test_mvc_smaps_pagemap(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	struct corten_region_row row;
	struct corten_smap_stats st;
	struct corten_arena_test_pm_sink sink = { };
	struct vm_area_struct *carrier;
	unsigned long a;

	if (!corten_enabled_static())
		kunit_skip(test, "smaps/pagemap aggregation requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);
	a = corten_arena_test_mvc_attach(test, mm, 1, PROT_READ | PROT_WRITE);

	/* The state-2 shape: commit RW, write-fault the first page
	 * through the carrier (the GUP-slow fault arm's entry).
	 */
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, a), 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_mark(mm, a,
					       CORTEN_PERM_USER |
					       CORTEN_PERM_READ |
					       CORTEN_PERM_WRITE), 0);
	carrier = corten_arena_test_carrier_of(mm, a);
	KUNIT_ASSERT_NOT_NULL(test, carrier);
	KUNIT_EXPECT_EQ(test,
			corten_arena_handle_mm_fault(carrier, a,
						     FAULT_FLAG_WRITE, NULL),
			0);

	mmap_read_lock(mm);
	KUNIT_EXPECT_TRUE(test, corten_row_query(mm, a, &row));
	KUNIT_EXPECT_EQ(test, row.start, a);

	corten_region_smap_stats(mm, &row, &st);
	KUNIT_EXPECT_EQ(test, st.resident, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, st.anon, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, st.swapped, 0);
	KUNIT_EXPECT_EQ(test, st.pss, (u64)PAGE_SIZE << CORTEN_PSS_SHIFT);

	/* pagemap truth: the faulted page is present (+exclusive, the
	 * fresh COW-free folio), the second page of the frame is a
	 * never-faulted zero entry.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_pagemap_fill(mm, a, a + 2 * PAGE_SIZE, true,
					    corten_arena_test_pm_emit, &sink),
			0);
	KUNIT_EXPECT_EQ(test, sink.n, 2);
	KUNIT_EXPECT_TRUE(test, sink.pme[0] & CORTEN_PM_PRESENT);
	KUNIT_EXPECT_TRUE(test, sink.pme[0] & CORTEN_PM_MMAP_EXCLUSIVE);
	KUNIT_EXPECT_NE(test, sink.pme[0] & CORTEN_PM_PFRAME_MASK, 0);
	KUNIT_EXPECT_EQ(test, sink.pme[1], 0);
	mmap_read_unlock(mm);

	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_mode_exit,
						 0, 0), 0);
}

/*
 * V-D (MV_VMA_FREE_SPEC.md sec 3.4): the spec's terminal anchor -- one
 * synthetic mm through the whole MODE lifecycle
 * (mmap->fault->mprotect->madvise->mremap->fork->swap->park->exit),
 * then the exit walk itself, then the ledger must close on every axis:
 *
 *   - the accounting triple: global ptdescs/meta_arrays deltas are 0
 *     (every descriptor and meta array the lifecycle created came
 *     back) and the folio side reads from the mm (MM_ANONPAGES and
 *     MM_SWAPENTS both 0 -- the walk's zap released the mapped page
 *     and the swap entry);
 *   - the B-2 criterion: mm_pgtables_bytes(mm) == 0 after the walk
 *     (PTE pages dec'd on retirement, the window-exclusive PMD/PUD
 *     pages self-torn) -- the harness VMA is never faulted, so the
 *     window domain is the only page-table footprint;
 *   - the upper-table count: exit_upper_pmds/puds advance by exactly
 *     the footprint (one 1GiB PMD page and one 512GiB PUD page per mm
 *     that ever filled the window; parent + forked child = 2 each),
 *     and p4ds stays 0 (runtime-folded p4d; on la57 the 256TB span
 *     test keeps the shared p4d page with the harness VMA, matching
 *     upstream free_pgtables' boundary discipline);
 *   - the whitelist: the INV-MV2 walker answers 0 violations with the
 *     lifecycle done, and the J1 pair never recorded a window hit.
 *
 * The forked child exits through the real mmput()->exit_mmap() funnel
 * before the parent is walked, so its ledger closes under the same
 * walk the guest exercises.
 */
static void corten_arena_test_exit_lifecycle(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm, *child;
	struct corten_arena *ar;
	struct vm_area_struct *carrier;
	struct corten_pte_meta sm;
	struct folio *folio;
	struct corten_txn txn;
	pte_t *ptep, pte, swp_pte;
	spinlock_t *ptl;	/* guards the swap-out PTE rewrite */
	pmd_t *pmdp;
	swp_entry_t entry = swp_entry(1, 0x5d);
	unsigned long win = CORTEN_ARENA_TEST_WIN;
	unsigned long win2 = CORTEN_ARENA_TEST_WIN + 2 * PMD_SIZE;
	unsigned long keep = win + PAGE_SIZE;
	unsigned long die = win + PMD_SIZE + PAGE_SIZE;
	unsigned long anon = win + 2 * PAGE_SIZE;
	long ptdescs0, arrays0, j1h0, pmds0, puds0, p4ds0, swaps0, timeouts;
	long upper_pmds, upper_puds;

	if (!corten_enabled_static())
		kunit_skip(test, "lifecycle ledger requires corten=on");

	ptdescs0 = corten_arena_test_named_counter(test, "ptdescs");
	arrays0 = corten_arena_test_named_counter(test, "meta_arrays");
	j1h0 = corten_arena_test_j1_hits();
	pmds0 = corten_arena_test_exit_upper_pmds();
	puds0 = corten_arena_test_exit_upper_puds();
	p4ds0 = corten_arena_test_exit_upper_p4ds();
	swaps0 = corten_arena_test_named_counter(test, "zap_swap_frees");
	timeouts = corten_arena_test_drain_timeouts();

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* mmap: two VMA-less carrier windows (the auto takeover). */
	KUNIT_ASSERT_EQ(test, corten_arena_test_pool_attach(mm, win,
							    2 * PMD_SIZE),
			0);
	/* fault: one mapped page per window plus a virtual allocation. */
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, keep), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, keep),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, die), 0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_mapped(mm, die),
			0);
	KUNIT_ASSERT_EQ(test, corten_arena_test_fork_seed_anon(mm, anon),
			0);
	KUNIT_EXPECT_TRUE(test, corten_arena_test_pt_present(mm, keep));

	/* mprotect: the routed permission downgrade commits (1 = routed). */
	KUNIT_EXPECT_EQ(test,
			corten_arena_mprotect_route(mm, win, PAGE_SIZE,
						    PROT_READ, -1), 1);

	/* madvise(DONTNEED shape): the chunk zap drops one slot's content,
	 * keeping its committed perm.
	 */
	ar = corten_arena_lookup_get(mm, anon);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ar);
	KUNIT_EXPECT_EQ(test,
			corten_arena_unmap_chunk(mm, ar, anon, PAGE_SIZE), 0);
	percpu_ref_put(&ar->active);

	/* mremap: the in-place shrink retires the second window (its
	 * mapped page drops with the tail).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_mremap_route(mm, win,
							2 * PMD_SIZE,
							PMD_SIZE, 0, 0),
			(long)win);
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, win));

	/* fork: the mirror carries the window to a child registry with
	 * its own page tables; the child then exits through the real
	 * mmput()->exit_mmap() funnel (the walk under test).
	 */
	child = mm_alloc();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_begin(child, mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_fork_commit(child, mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_query(child, win), 1);
	KUNIT_EXPECT_TRUE(test, corten_arena_test_pt_present(child, win));
	mmput(child);

	/* swap: the M6.T2 hand-rolled swap-out shape on the surviving
	 * page -- synthetic entry, counted MM_SWAPENTS, Swapped metadata.
	 */
	carrier = corten_arena_test_carrier_of(mm, keep);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, carrier);
	pmdp = corten_arena_test_pmd(mm, keep);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pmdp);
	ptep = pte_offset_map_lock(mm, pmdp, keep, &ptl);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptep);
	pte = ptep_get_and_clear(mm, keep, ptep);
	KUNIT_ASSERT_TRUE(test, pte_present(pte));
	swp_pte = swp_entry_to_pte(entry);
	set_pte_at(mm, keep, ptep, swp_pte);
	pte_unmap_unlock(ptep, ptl);
	folio = page_folio(pte_page(pte));
	folio_remove_rmap_pte(folio, folio_page(folio, 0), carrier);
	add_mm_counter(mm, MM_ANONPAGES, -1);
	add_mm_counter(mm, MM_SWAPENTS, 1);
	folio_put(folio);
	KUNIT_ASSERT_EQ(test, corten_lock_range(mm, keep, PAGE_SIZE, &txn),
			0);
	memset(&sm, 0, sizeof(sm));
	sm.state = CORTEN_SWAPPED;
	sm.perm = CORTEN_PERM_READ | CORTEN_PERM_WRITE | CORTEN_PERM_USER;
	corten_swap_encode(&sm, entry);
	KUNIT_EXPECT_EQ(test, corten_swap_out(&txn, keep, &sm), 0);
	corten_unlock(&txn);

	/* park: a third window parks through the exact munmap route. */
	KUNIT_ASSERT_EQ(test, corten_arena_test_pool_attach(mm, win2,
							    PMD_SIZE),
			0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 win2, PMD_SIZE), 1);
	KUNIT_EXPECT_EQ(test, corten_arena_test_pool_nr(mm), 1);

	/* The whitelist pair: the tree answers the INV-MV2 walker clean
	 * and the J1 probe never found a window VMA.
	 */
	KUNIT_EXPECT_EQ(test, corten_audit_j2_walk(mm), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_j1_hits(), j1h0);

	/* exit: the pure-PT walk itself (the guest reaches it through
	 * exit_mmap(); the direct call keeps the assertions on this side
	 * of the deferred mm free).
	 */
	corten_arena_mm_exit(mm);

	KUNIT_EXPECT_EQ(test, get_mm_counter(mm, MM_ANONPAGES), 0);
	KUNIT_EXPECT_EQ(test, get_mm_counter(mm, MM_SWAPENTS), 0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_named_counter(test,
							      "zap_swap_frees"),
			swaps0 + 1);

	/* The B-2 criterion: the window domain's whole page-table
	 * footprint came back (PTE pages dec'd, exclusive upper tables
	 * self-torn); the never-faulted harness VMA holds none.
	 */
	KUNIT_EXPECT_EQ(test, mm_pgtables_bytes(mm), 0);

	upper_pmds = corten_arena_test_exit_upper_pmds() - pmds0;
	upper_puds = corten_arena_test_exit_upper_puds() - puds0;
	KUNIT_EXPECT_EQ(test, upper_pmds, 2);
	KUNIT_EXPECT_EQ(test, upper_puds, 2);
	/* la57: the child (tree-free mm) frees its p4d page, the parent
	 * shares its 256TB span with the harness VMA and keeps it (the
	 * upstream free_pgtables boundary discipline).
	 */
	KUNIT_EXPECT_EQ(test, corten_arena_test_exit_upper_p4ds() - p4ds0,
			mm_p4d_folded(mm) ? 0 : 1);

	/* The accounting triple: descriptors and meta arrays came back
	 * (the child's exit closed its share), and no drain degraded.
	 */
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_named_counter(test, "ptdescs"),
			ptdescs0);
	KUNIT_EXPECT_EQ(test,
			corten_arena_test_named_counter(test, "meta_arrays"),
			arrays0);
	KUNIT_EXPECT_EQ(test, corten_arena_test_drain_timeouts(), timeouts);
	KUNIT_EXPECT_NULL(test, READ_ONCE(mm->corten_state));
	KUNIT_EXPECT_NULL(test, vma_lookup(mm, win));
	/* The registry is gone: query answers the no-state -ENOENT. */
	KUNIT_EXPECT_EQ(test, corten_arena_query(mm, win), -ENOENT);
}

/*
 * V-D follow-up (the guest punchfork leak probe): the battery interleave
 * the mva1_probe runs, driven through the real do_mmap()/munmap-route
 * funnels -- a parked-window prelude (the S-4 shape), then eight
 * iterations, each an 8MiB four-window carrier arena placed by the auto
 * route (pool take or magazine placement), content in every window, two
 * window-aligned memfd MAP_FIXED punches through the real punch funnel
 * (frames erased, content zapped, implant VMAs installed by mmap_region),
 * a fork whose child exits through the real mmput()->exit_mmap() funnel,
 * and the iteration-end munmap that answers -ENOENT (the punched arena's
 * start frame is a hole, so the pool release's start-frame lookup
 * misses -- the guest's silent munmap failure; the arena and its
 * implants stay live until process exit).  The B-2 criterion is checked
 * per side: pgtables_bytes == 0 for every child and the parent.
 */
static void corten_arena_test_exit_punchfork(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm, *child;
	struct vm_area_struct *cvma;
	struct file *memfd;
	unsigned long w = 0;
	int i, j;

	if (!corten_enabled_static())
		kunit_skip(test, "punchfork exit ledger requires corten=on");

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* The S-4 prelude: one 8MiB window placed by the auto route and
	 * parked by its munmap -- the pool slot the first punchfork
	 * iteration reactivates.
	 */
	w = corten_arena_test_vm_mmap(test, mm, 0, 4 * PMD_SIZE,
				      MAP_PRIVATE | MAP_ANONYMOUS |
				      MAP_NORESERVE);
	KUNIT_ASSERT_EQ(test, w & ~PAGE_MASK, 0);
	KUNIT_ASSERT_EQ(test,
			corten_arena_test_run_op(test, mm,
						 corten_arena_test_op_munmap_route,
						 w, 4 * PMD_SIZE), 1);

	memfd = shmem_file_setup("corten-punchfork", 2 * PMD_SIZE, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, memfd);

	for (j = 0; j < 8; j++) {
		/* The 8MiB punchfork window: four PMD windows, one mapped
		 * page (and thus one PTE page) each.  The first iteration
		 * reactivates the parked prelude window; the rest place
		 * fresh (the magazine).
		 */
		w = corten_arena_test_vm_mmap(test, mm, 0, 4 * PMD_SIZE,
					      MAP_PRIVATE | MAP_ANONYMOUS |
					      MAP_NORESERVE);
		KUNIT_ASSERT_EQ(test, w & ~PAGE_MASK, 0);
		for (i = 0; i < 4; i++) {
			unsigned long a = w + i * PMD_SIZE + PAGE_SIZE;

			KUNIT_ASSERT_EQ(test,
					corten_arena_test_fill_window(mm, a),
					0);
			KUNIT_ASSERT_EQ(test,
					corten_arena_test_fork_seed_mapped(mm, a),
					0);
		}

		/* punch①+②: memfd MAP_FIXED over windows 0 and 1 -- the
		 * real punch route (frames erased, content zapped) and
		 * the real implant install by mmap_region().
		 */
		KUNIT_ASSERT_EQ(test,
				corten_arena_test_punch(test, mm, memfd, w,
							PMD_SIZE), w);
		KUNIT_ASSERT_EQ(test,
				corten_arena_test_punch(test, mm, memfd,
							w + PMD_SIZE, PMD_SIZE),
				w + PMD_SIZE);

		/* fork: the child mirrors the punched arena (register_child
		 * re-fills the hole frames) and inherits the implants as
		 * plain tree VMAs, exactly like dup_mmap() copies them.
		 */
		child = mm_alloc();
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, child);
		KUNIT_ASSERT_EQ(test,
				corten_arena_test_fork_begin(child, mm), 0);
		cvma = corten_arena_test_mkvm(child, w, w + PMD_SIZE,
					      CORTEN_ARENA_TEST_FLAGS_OK);
		KUNIT_ASSERT_NOT_NULL(test, cvma);
		cvma = corten_arena_test_mkvm(child, w + PMD_SIZE,
					      w + 2 * PMD_SIZE,
					      CORTEN_ARENA_TEST_FLAGS_OK);
		KUNIT_ASSERT_NOT_NULL(test, cvma);
		KUNIT_ASSERT_EQ(test,
				corten_arena_test_fork_commit(child, mm), 0);

		/* The child exits first (the guest's waitpid order): the
		 * real funnel, with the mm's lifetime held for the B-2
		 * assertion.
		 */
		mmgrab(child);
		mmput(child);
		KUNIT_EXPECT_EQ(test, mm_pgtables_bytes(child), 0);
		mmdrop(child);

		/* The iteration-end munmap over the punched arena answers
		 * -ENOENT (the start frame is a hole; the guest's silent
		 * munmap failure -- the arena stays live until exit).
		 */
		KUNIT_ASSERT_EQ(test,
				corten_arena_test_run_op(test, mm,
							 corten_arena_test_op_munmap_route,
							 w, 4 * PMD_SIZE),
				-ENOENT);
	}

	fput(memfd);

	/* Process exit: eight interleaved punched arenas, sixteen real
	 * implants, the harness VMA -- one real exit_mmap() funnel.
	 */
	mmgrab(mm);
	mmput(mm);
	KUNIT_EXPECT_EQ(test, mm_pgtables_bytes(mm), 0);
	mmdrop(mm);
	t->mm = NULL;
}

/*
 * V-D follow-up #2 (the guest 4x4096 residue, root cause anchor): two
 * live arenas sharing one P4D span from DIFFERENT PUD segments -- the
 * metis_eq shape (an 8MiB corpus region at the window base and a
 * chunk-carved 128MiB arena one 1GiB segment up).  The phase-B
 * self-teardown retires each arena's PMD page in ascending registry
 * order, but the FIRST arena's PUD-page decision ran while the second
 * arena's PMD page still sat in the shared PUD page: the all-none scan
 * failed, the PUD page was skipped for good and its account surfaced
 * as exactly one "non-zero pgtables_bytes: 4096" BUG line per mm --
 * legacy could not recover it (the window domain is tree-free, so
 * free_pgtables never descends there).  The B1/B2/B3 pass separation
 * retires every PMD page before any PUD page is judged; the anchor
 * drives the real mmput()->exit_mmap() funnel and requires the whole
 * account back, upper-table counters included (two PMD pages, one PUD
 * page for the shared 512GiB span).
 */
static void corten_arena_test_exit_multi_segment(struct kunit *test)
{
	struct corten_arena_test_mm *t = corten_arena_test_mm_setup(test);
	struct mm_struct *mm = t->mm;
	unsigned long win = CORTEN_ARENA_TEST_WIN;
	unsigned long win2 = CORTEN_ARENA_TEST_WIN + PUD_SIZE;
	long pmds0, puds0;
	int i;

	if (!corten_enabled_static())
		kunit_skip(test, "multi-segment exit requires corten=on");

	pmds0 = corten_arena_test_exit_upper_pmds();
	puds0 = corten_arena_test_exit_upper_puds();

	KUNIT_ASSERT_EQ(test, corten_arena_mode_enter(mm), 0);

	/* Arena A at the window base (its own PUD segment), one mapped
	 * page -- and PTE page -- per window.
	 */
	KUNIT_ASSERT_EQ(test, corten_arena_test_pool_attach(mm, win,
							    2 * PMD_SIZE),
			0);
	for (i = 0; i < 2; i++) {
		unsigned long a = win + i * PMD_SIZE + PAGE_SIZE;

		KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, a), 0);
		KUNIT_ASSERT_EQ(test,
				corten_arena_test_fork_seed_mapped(mm, a), 0);
	}

	/* Arena B one PUD segment up: same P4D span, different PMD page.
	 * The chunk-carve (a window-aligned inner munmap) keeps the frames
	 * and resets the slots, exactly like the guest's PROT_NONE arena
	 * tail -- the carve is not load-bearing for the leak, the segment
	 * distance is.
	 */
	KUNIT_ASSERT_EQ(test, corten_arena_test_pool_attach(mm, win2,
							    2 * PMD_SIZE),
			0);
	for (i = 0; i < 2; i++) {
		unsigned long a = win2 + i * PMD_SIZE + PAGE_SIZE;

		KUNIT_ASSERT_EQ(test, corten_arena_test_fill_window(mm, a), 0);
		KUNIT_ASSERT_EQ(test,
				corten_arena_test_fork_seed_mapped(mm, a), 0);
	}

	/* The real funnel: the walk owns the tree-free window domain
	 * (free_pgtables cannot reach it), so the shared PUD page's
	 * retirement is the walk's alone to finish.
	 */
	mmgrab(mm);
	mmput(mm);
	KUNIT_EXPECT_EQ(test, mm_pgtables_bytes(mm), 0);
	mmdrop(mm);
	t->mm = NULL;

	KUNIT_EXPECT_EQ(test, corten_arena_test_exit_upper_pmds(),
			pmds0 + 2);
	KUNIT_EXPECT_EQ(test, corten_arena_test_exit_upper_puds(),
			puds0 + 1);
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
	KUNIT_CASE(corten_arena_test_auto_classify_file),
	KUNIT_CASE(corten_arena_test_auto_place),
	KUNIT_CASE(corten_arena_test_auto_route),
	KUNIT_CASE(corten_arena_test_auto_attach_release),
	KUNIT_CASE(corten_arena_test_file_lifecycle),
	KUNIT_CASE(corten_arena_test_truncate_route),
	KUNIT_CASE(corten_arena_test_file_read),
	KUNIT_CASE(corten_arena_test_file_cow),
	KUNIT_CASE(corten_arena_test_file_fork_mirror),
	KUNIT_CASE(corten_arena_test_file_fork_cow),
	KUNIT_CASE(corten_arena_test_file_fork_pinned),
	KUNIT_CASE(corten_arena_test_file_hugetlb_route),
	KUNIT_CASE(corten_arena_test_mag_recycle),
	KUNIT_CASE(corten_arena_test_mag_marker),
	KUNIT_CASE(corten_arena_test_pool_reuse),
	KUNIT_CASE(corten_arena_test_pool_limit),
	KUNIT_CASE(corten_arena_test_pool_mode_exit),
	KUNIT_CASE(corten_arena_test_pool_fork),
	KUNIT_CASE(corten_arena_test_noreplace_parked),
	KUNIT_CASE(corten_arena_test_noreplace_active),
	KUNIT_CASE(corten_arena_test_mapfixed_over_parked),
	KUNIT_CASE(corten_arena_test_occupied_incl_idle),
	KUNIT_CASE(corten_arena_test_p4_eject),
	KUNIT_CASE(corten_arena_test_hint_fence),
	KUNIT_CASE(corten_arena_test_j1_hooks),
	KUNIT_CASE(corten_arena_test_j1_self_exempt),
	KUNIT_CASE(corten_arena_test_uffd_window_reject),
	/* V-A.3d: the J1 implant exemption runs before the C-group's
	 * deliberate violation injection -- the exit-gate verdict is
	 * global-cumulative, and this anchor asserts the clean-boot
	 * gate_pass the smoke shape now leaves behind.
	 */
	KUNIT_CASE(corten_arena_test_j1_implant_exempt),
	KUNIT_CASE(corten_arena_test_fault_window_shorts),
	/* V-A.3c (C-group): the INV-MV2 walker. */
	KUNIT_CASE(corten_arena_test_inv_mv2_clean),
	KUNIT_CASE(corten_arena_test_inv_mv2_inject),
	KUNIT_CASE(corten_arena_test_inv_mv2_stale),
	KUNIT_CASE(corten_arena_test_inv_mv2_implant_fork),
	KUNIT_CASE(corten_arena_test_j2_triggers),
	/* V-A.3d (D-group): the S-5 terminals (the J1 implant exemption
	 * is registered with the B-group, see the note there).
	 */
	KUNIT_CASE(corten_arena_test_msync_window_segments),
	KUNIT_CASE(corten_arena_test_mincore_route),
	KUNIT_CASE(corten_arena_test_madvise_parked_terminal),
	KUNIT_CASE(corten_arena_test_move_pages_window),
	/* V-C: the dual-source faces (row stream, GUP-slow probe,
	 * smaps/pagemap aggregation; the merge-first-row anchor pins the
	 * cursor contracts behind the J3 first-row fix).
	 */
	KUNIT_CASE(corten_arena_test_mvc_row_stream),
	KUNIT_CASE(corten_arena_test_mvc_merge_first_row),
	KUNIT_CASE(corten_arena_test_mvc_gup_probe),
	KUNIT_CASE(corten_arena_test_mvc_smaps_pagemap),
	/* V-D: the full-lifecycle ledger anchor (B-2 closure). */
	KUNIT_CASE(corten_arena_test_exit_lifecycle),
	/* V-D follow-up: the punchfork interleave both funnels must meet
	 * (the guest battery's residue probe family).
	 */
	KUNIT_CASE(corten_arena_test_exit_punchfork),
	/* V-D follow-up #2: two arenas, one P4D span, two PUD segments --
	 * the metis shape that stranded the shared PUD page (4x4096).
	 */
	KUNIT_CASE(corten_arena_test_exit_multi_segment),
	KUNIT_CASE(corten_arena_test_vma_free_reuse),
	KUNIT_CASE(corten_arena_test_inv_mv3),
	KUNIT_CASE(corten_arena_test_fork_vma_free),
	KUNIT_CASE(corten_arena_test_vma_free_shrink_pick),
	KUNIT_CASE(corten_arena_test_carrier_shrink_pick),
	KUNIT_CASE(corten_arena_test_auto_validate),
	KUNIT_CASE(corten_arena_test_carrier_vma),
	KUNIT_CASE(corten_arena_test_fork_punch_novma),
	KUNIT_CASE(corten_arena_test_fork_redeclare_content),
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
