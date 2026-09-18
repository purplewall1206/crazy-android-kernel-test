// SPDX-License-Identifier: GPL-2.0
/*
 * CortenMM arena layer - prctl registration, per-mm lookup and
 * shadow-VMA conversion (M3B_DESIGN.md sec 2/3/6).
 *
 * DECLARE turns an existing MAP_NORESERVE private anonymous VMA that
 * exactly matches [addr, addr+len) into a shadow-VMA (VM_CORTEN |
 * VM_NOHUGEPAGE, added in place; the VMA keeps its plain anonymous
 * semantics for every other consumer) and publishes the range in a
 * per-mm xarray keyed by 2M frame index.  RELEASE drains the arena's
 * transaction refcount and then removes the range with the ordinary
 * legacy munmap, so page-table teardown flows through the regular
 * pte_free funnels and the M2a uninstall hooks.
 *
 * Concurrency contract (sec 6.1/6.3, order amended by DEV-13):
 *
 *   - DECLARE/RELEASE serialize on state->ctl_lock; the fault path only
 *     touches the xarray (RCU) and the arena percpu_ref, never ctl_lock,
 *     which is what makes the drain wait deadlock-free.
 *   - Lock order: mmap_lock (W) -> ctl_lock -> vma write marks.  DECLARE
 *     and RELEASE take the write lock first and nest ctl_lock inside it
 *     (corten_arena_declare_locked()/corten_arena_release_locked()); the
 *     do_mmap auto-attach runs with the write lock already held.  The
 *     drain waits under both locks -- deadlock-free because a
 *     transaction only tryget/puts the percpu_ref and never acquires
 *     either lock.  Neither desc->lock nor the PTE lock is ever taken
 *     directly in this file (the transaction layer owns them, nested
 *     below).
 *   - An arena descriptor is reachable through the xarray until the last
 *     frame is erased (under ctl_lock) and is then freed with kfree_rcu()
 *     after its refcount drained, so an RCU lookup racing RELEASE either
 *     misses (falls back to legacy) or pins the still-alive descriptor.
 *
 * This slice (M3b S1-S3) delivers registration, lookup and the shadow-VMA
 * conversion; the fault path starts consuming corten_arena_lookup() in S4,
 * which is why every lookup already honours the "an arena address always
 * has its shadow-VMA" invariant by converting the VMA before publishing.
 *
 * Transitional semantics of the S3 delivery state (no fault hooks yet):
 * every access inside a declared arena is served by the ordinary
 * anonymous-fault path through the shadow-VMA, i.e. the arena behaves as
 * plain memory.  VMA-splitting operations (mprotect/mremap/MAP_FIXED over
 * a sub-range) are not rejected until S6; in this state they only create
 * stale registry entries and can corrupt nothing, but once S4 routes
 * faults through the transactions they become part of the interlock the
 * design closes with the S6 rejection hooks (M3B_DESIGN.md sec 5).
 */
#include <linux/capability.h>
#include <linux/corten.h>
#include <linux/corten_arena.h>
#include <linux/errno.h>
#include <linux/hugetlb.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/mman.h>
#include <linux/mmap_lock.h>
#include <linux/mmu_notifier.h>
#include <linux/mutex.h>
#include <linux/oom.h>
#include <linux/perf_event.h>
#include <linux/percpu-refcount.h>
#include <linux/percpu.h>
#include <linux/pgtable.h>
#include <linux/rmap.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/uaccess.h>

#include <asm/tlb.h>

#include <uapi/linux/mman.h>
#include <uapi/linux/prctl.h>

#include "corten.h"		/* corten_meta_ensure_locked() */
#include "corten_arena.h"	/* S4-S7 internal interface */
#include "vma.h"		/* __split_vma() (punch surgery, D-G'' B1) */

#ifdef CONFIG_ANON_VMA_NAME
#define CORTEN_ARENA_VMA_NAME	"corten_arena"
#endif

/*
 * Serializes lazy creation of the per-mm state so that two concurrent
 * DECLAREs do not both install a registry.  Everything else on the state
 * is protected by its own ctl_lock.
 */
static DEFINE_MUTEX(corten_arena_alloc_lock);

static void corten_arena_stat_add(struct corten_mm_state *state,
				  enum corten_arena_stat which, long val)
{
	this_cpu_add(state->stats[which], val);
}

/*
 * S8 observability ledger: every live arena of every mm, registered at
 * the end of a successful DECLARE and unlinked at deregistration.  This
 * is deliberately a second bookkeeping of the per-mm xarrays: debugfs
 * cannot enumerate mm_structs, so a global list is the only way to show
 * the cross-process arena population.  Writers (DECLARE/RELEASE/exit)
 * are cold paths serialized by @corten_arena_list_lock; readers walk it
 * under RCU and never block or contend anything on the hot path (the
 * fault path does not know this list exists).  The list is observational
 * only -- correctness lives in the per-mm xarrays.
 */
static LIST_HEAD(corten_arena_list);
static DEFINE_SPINLOCK(corten_arena_list_lock);

/*
 * Global mirror of CORTEN_ARENA_STAT_DRAIN_TIMEOUTS (r03 final-smoke
 * legacy item 1): the per-mm counters are not reachable from debugfs
 * (states are not enumerable either), so the drain-timeout recording
 * keeps this aggregate in step.  Only ever touched on the timeout path,
 * which by definition is a cold kernel-bug path -- no hot-path cost.
 */
static atomic_long_t corten_arena_nr_drain_timeouts;

/*
 * T0b named route counters (M4T0_SPEC.md sec 3/10).  Like the drain
 * timeout above, these are global aggregates: per-mm states are not
 * enumerable from debugfs, so the counters the DoD evidence reads
 * (auto_mmaps, mprotect_routes, ...) are recorded at the route decision
 * points and rendered by corten_arena_stats_report() (S8 renderer).
 * Every increment sits on a cold space-operation path -- none on the
 * fault hot path -- so the atomic updates are not measurable.
 */
static atomic_long_t corten_nr_auto_mmaps;	/* auto arenas attached */
static atomic_long_t corten_nr_auto_attach_fails; /* DECLARE in attach failed */
static atomic_long_t corten_nr_auto_fallbacks;	/* total legacy degradations */
static atomic_long_t corten_nr_auto_exhausted;	/* window exhausted */
static atomic_long_t corten_nr_mprotect_routes;	/* routed mprotect txns */
static atomic_long_t corten_nr_madvise_free_txns; /* FREE folded to DONTNEED */
static atomic_long_t corten_nr_madvise_hints;	/* hint behaviours no-op'd */
static atomic_long_t corten_nr_mremap_routes;	/* routed (moved) mremaps */
static atomic_long_t corten_nr_mremap_rejects;	/* unroutable mremaps */
static atomic_long_t corten_nr_munmap_releases;	/* release-rule munmaps */
static atomic_long_t corten_nr_fork_demotes;	/* forks that demoted arenas */
static atomic_long_t corten_nr_rearm_recovered;	/* -ENOENT windows re-armed */
static atomic_long_t corten_nr_rearm_failed;	/* windows still untracked */
static atomic_long_t corten_nr_eagain_retries;	/* route -EAGAIN retry wins */
static atomic_long_t corten_nr_eagain_leaked;	/* -EAGAIN still escaping */
static atomic_long_t corten_nr_mremap_release_fail; /* grow RELEASE fails */
static atomic_long_t corten_nr_mmap_punches;	/* file-MAP_FIXED punch routes */
static atomic_long_t corten_nr_mmap_punch_rejects; /* unroutable MAP_FIXED */
/* M5 faithful-fork counters (M5_FORK_SPEC.md 1.3-④6): fork_demotes stays
 * as the historical T0 telemetry and no longer grows.
 */
static atomic_long_t corten_nr_fork_faithful;	/* commits that mirrored */
static atomic_long_t corten_nr_fork_skips;	/* child-less arena skips */

/*
 * KUnit injection point for the commit failure unwinds (R-A): 1 = fail at
 * commit entry, 2 = fail after the first arena was mirrored.  Written
 * only by corten_arena_test_fork_fail_arm() (KUnit, single-threaded).
 */
static int corten_fork_fail_stage;

/* Link @arena into the observability ledger.  Called with state->ctl_lock
 * held, after the arena is fully published in its per-mm xarray; the
 * caller's failure paths all precede this point, so no unlinked arena is
 * ever left dangling behind.
 */
static void corten_arena_obs_add(struct corten_arena *arena)
{
	spin_lock(&corten_arena_list_lock);
	list_add_rcu(&arena->obs, &corten_arena_list);
	spin_unlock(&corten_arena_list_lock);
}

/* Unlink @arena from the observability ledger.  Called at deregistration
 * (under state->ctl_lock from RELEASE; from mm_exit with mm_users already
 * 0 -- only the list lock itself is required for list consistency): after
 * this an RCU reader can no longer find the arena, and the kfree_rcu() in
 * the (successful) teardown keeps even an in-flight walk safe.
 */
static void corten_arena_obs_remove(struct corten_arena *arena)
{
	spin_lock(&corten_arena_list_lock);
	list_del_rcu(&arena->obs);
	spin_unlock(&corten_arena_list_lock);
}

/* Record one drain timeout (leaked transaction reference -- a kernel
 * bug): the per-mm counter when @state is available, always the global
 * aggregate the arena_stats file shows.
 */
static void corten_arena_note_drain_timeout(struct corten_mm_state *state)
{
	if (state)
		corten_arena_stat_add(state, CORTEN_ARENA_STAT_DRAIN_TIMEOUTS,
				      1);
	atomic_long_inc(&corten_arena_nr_drain_timeouts);
}

static void corten_arena_state_free(struct corten_mm_state *state)
{
	xa_destroy(&state->arenas);
	free_percpu(state->stats);
	mutex_destroy(&state->ctl_lock);
	kfree(state);
}

/*
 * Create and publish the registry for @mm.  mm->corten_state is published
 * once (release store) and never replaced or removed while the mm is alive
 * (exit_mmap() takes it down when no faults can be in flight), so readers
 * pair the acquire load with the xa_load / refcount read below without any
 * further synchronization.
 */
static struct corten_mm_state *corten_arena_state_create(struct mm_struct *mm)
{
	struct corten_mm_state *state, *raced;

	state = kzalloc(sizeof(*state), GFP_KERNEL_ACCOUNT);
	if (!state)
		return NULL;

	xa_init(&state->arenas);
	refcount_set(&state->nr, 0);
	mutex_init(&state->ctl_lock);
	/* The MODE auto-arena cursor starts at the window base regardless
	 * of which entry point created the registry (M4T0_SPEC.md 1.3).
	 */
	state->next_va = CORTEN_MODE_WINDOW_START;
	state->stats = __alloc_percpu(sizeof(unsigned long) *
				      CORTEN_ARENA_NR_STATS,
				      __alignof__(unsigned long));
	if (!state->stats) {
		corten_arena_state_free(state);
		return NULL;
	}

	mutex_lock(&corten_arena_alloc_lock);
	/* Pairs with the stores below and in corten_arena_mm_exit(). */
	raced = smp_load_acquire(&mm->corten_state);
	if (!raced)
		/* Publish the registry; readers pair with this store. */
		smp_store_release(&mm->corten_state, state);
	mutex_unlock(&corten_arena_alloc_lock);

	if (raced) {
		corten_arena_state_free(state);
		state = raced;
	}

	return state;
}

/* The release callback of arena->active: fires exactly once, when the
 * killed refcount reaches zero, from the RCU callback that completes the
 * atomic switch (or from the final put).  It runs in atomic context and
 * must only complete().
 */
/* Bound on the drain wait (sec 6.3): a healthy transaction is bounded by
 * the descriptor locks, so completion is immediate; the timeout only
 * fires when an active percpu_ref leaked (a pairing bug).  A leaked
 * reference must degrade to a counted, diagnosed leak -- a hang would
 * turn the process into an unkillable D-state zombie (r03 DoD failure
 * B), which is strictly worse than leaking one ~150-byte descriptor.
 */
#define CORTEN_ARENA_DRAIN_TIMEOUT	(10 * HZ)

static void corten_arena_active_release(struct percpu_ref *ref)
{
	struct corten_arena *arena =
		container_of(ref, struct corten_arena, active);

	complete(&arena->drained);
}

/* Kill @arena and wait until no transaction can still hold a reference
 * on it (sec 6.3).  Called with state->ctl_lock held, which is safe
 * because the fault path never takes that lock.
 *
 * Return: true when the drain completed (the caller must free the
 * descriptor), false on timeout (the reference is leaked: the caller
 * must NOT percpu_ref_exit()/free -- a straggler put would UAF -- and
 * proceeds with the legacy teardown so the process stays runnable).
 */
static bool corten_arena_drain(struct corten_arena *arena)
{
	percpu_ref_kill_and_confirm(&arena->active, NULL);

	if (!wait_for_completion_timeout(&arena->drained,
					 CORTEN_ARENA_DRAIN_TIMEOUT)) {
		WARN_ONCE(1,
			  "corten: arena [%lx,%lx) drain timed out: leaked transaction reference, leaking the descriptor\n",
			  arena->start, arena->end);
		return false;
	}

	return true;
}

static void corten_arena_free(struct corten_arena *arena)
{
	percpu_ref_exit(&arena->active);
	mutex_destroy(&arena->fill_lock);
	kfree_rcu(arena, rcu);
}

/* ------------------------------------------------------------------ *
 * shadow-VMA conversion (sec 3)
 * ------------------------------------------------------------------
 */

static u8 corten_arena_prot_from_vma(struct vm_area_struct *vma)
{
	u8 perm = CORTEN_PERM_USER;

	if (vma->vm_flags & VM_READ)
		perm |= CORTEN_PERM_READ;
	if (vma->vm_flags & VM_WRITE)
		perm |= CORTEN_PERM_WRITE;
	if (vma->vm_flags & VM_EXEC)
		perm |= CORTEN_PERM_EXEC;

	return perm;
}

/* Inverse of corten_arena_prot_from_vma(): CORTEN_PERM_* -> PROT_*. */
static unsigned long corten_arena_perm_to_prot(u8 perm)
{
	unsigned long prot = 0;

	if (perm & CORTEN_PERM_READ)
		prot |= PROT_READ;
	if (perm & CORTEN_PERM_WRITE)
		prot |= PROT_WRITE;
	if (perm & CORTEN_PERM_EXEC)
		prot |= PROT_EXEC;

	return prot;
}

/*
 * T0b: the PTE prot encoding a metadata permission maps to on @vma's
 * shadow-VMA.  The metadata is the arena's source of truth (PS-B2), so
 * every arena PTE install derives its hardware protection from the
 * *recorded* perm, not from the shadow-VMA flags: after a routed
 * mprotect() the pages of the protected chunk must come out with the
 * new protection even though the shadow-VMA (one per arena) cannot
 * carry sub-VMA permissions.
 */
static pgprot_t corten_arena_perm_pgprot(struct vm_area_struct *vma, u8 perm)
{
	vm_flags_t flags = vma->vm_flags & ~(VM_READ | VM_WRITE | VM_EXEC);

	if (perm & CORTEN_PERM_READ)
		flags |= VM_READ;
	if (perm & CORTEN_PERM_WRITE)
		flags |= VM_WRITE;
	if (perm & CORTEN_PERM_EXEC)
		flags |= VM_EXEC;

	return vm_get_page_prot(flags);
}

/* Convert @vma into the arena's shadow-VMA.  Called with mmap_lock held
 * for writing; vm_flags_set() takes the per-VMA write mark itself.  The
 * VMA keeps its plain anonymous identity: no vm_ops, no file, no split,
 * no merge -- the flag word and the anon_vma_name make it unmergeable
 * with any non-shadow neighbour (see is_mergeable_vma()).  Everything
 * that can fail happens before the first irreversible store, so there is
 * no partial conversion.
 */
static int corten_arena_shadowize(struct vm_area_struct *vma)
{
#ifdef CONFIG_ANON_VMA_NAME
	struct anon_vma_name *name;
#endif
	int ret;

#ifdef CONFIG_ANON_VMA_NAME
	name = anon_vma_name_alloc(CORTEN_ARENA_VMA_NAME);
	if (!name)
		return -ENOMEM;
#endif

	/* The shadow-VMA is rmap-referenced (anon_vma) and must never be
	 * collapsed: VM_NOHUGEPAGE is the single switch khugepaged/THP
	 * honour (sec 4.7).
	 */
	ret = anon_vma_prepare(vma);
	if (ret) {
#ifdef CONFIG_ANON_VMA_NAME
		anon_vma_name_put(name);
#endif
		return ret;
	}

	vm_flags_set(vma, VM_CORTEN | VM_NOHUGEPAGE);

#ifdef CONFIG_ANON_VMA_NAME
	{
		struct anon_vma_name *orig = vma->anon_name;

		/* Freshly mmap()ed arena candidates carry no name; replace
		 * just in case PR_SET_VMA named it in the meantime.
		 */
		vma->anon_name = name;
		if (orig)
			anon_vma_name_put(orig);
	}
#endif

	return 0;
}

/* Undo a shadow conversion after a publication failure or in RELEASE.
 * Same locking as corten_arena_shadowize(); also drops the descriptor's
 * cached shadow-VMA pointer ([FAIL-2]; @arena may be unpublished on the
 * DECLARE unwind path, where clearing is a no-op for readers).
 */
static void corten_arena_unshadow(struct corten_arena *arena,
				  struct vm_area_struct *vma)
{
	WRITE_ONCE(arena->vma, NULL);
#ifdef CONFIG_ANON_VMA_NAME
	anon_vma_name_put(vma->anon_name);
	vma->anon_name = NULL;
#endif
	vm_flags_clear(vma, VM_CORTEN | VM_NOHUGEPAGE);
}

/* ------------------------------------------------------------------ *
 * validation (DECLARE contract, sec 2.1)
 * ------------------------------------------------------------------
 */

/* Leaf PTE walk; defined in the S4 section below. */
static pmd_t *corten_arena_pmd(struct mm_struct *mm, unsigned long addr);

/*
 * Flag whitelist for the declared VMA: access rights, their mirroring
 * MAY bits, and MAP_NORESERVE.  VM_SOFTDIRTY is tolerated because it is
 * transient dirty-tracking bookkeeping (vma_merge excludes it from flag
 * comparisons for the same reason), not a memory-type property.
 * Everything else -- VM_SPECIAL and friends, VM_ACCOUNT (the caller must
 * have passed MAP_NORESERVE), VM_HUGETLB, VM_PKEY_*, VM_SHADOW_STACK,
 * VM_SEQ_READ/VM_RAND_READ, VM_UFFD_*, VM_SEALED -- rejects the DECLARE.
 */
#define CORTEN_ARENA_VMA_FLAG_MASK			\
	(VM_READ | VM_WRITE | VM_EXEC |			\
	 VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC |	\
	 VM_NORESERVE | VM_SOFTDIRTY)

static int corten_arena_validate_vma(struct vm_area_struct *vma,
				     unsigned long addr, unsigned long len)
{
	if (!vma)
		return -EINVAL;

	/* The range must be exactly one existing VMA (no splitting). */
	if (vma->vm_start != addr || vma->vm_end != addr + len)
		return -EINVAL;

	/* Private anonymous only: vma_is_anonymous() is the mmap() path's
	 * own identity for private anonymous mappings (special mappings
	 * carry vm_ops without any file, so vm_file alone is not enough).
	 */
	if (!vma_is_anonymous(vma) || (vma->vm_flags & VM_SHARED))
		return -EINVAL;

	/* MAP_NORESERVE is part of the DECLARE contract. */
	if (vma->vm_flags & VM_ACCOUNT)
		return -EINVAL;

	if (vma->vm_flags & ~CORTEN_ARENA_VMA_FLAG_MASK)
		return -EINVAL;

#ifdef CONFIG_USERFAULTFD
	/* Registered userfaultfd context never occurs on a VMA that passes
	 * the flag whitelist above, but check it explicitly: uffd on a
	 * shadow-VMA is rejected at registration time instead (sec 5.13).
	 */
	if (vma->vm_userfaultfd_ctx.ctx)
		return -EINVAL;
#endif

	return 0;
}

static bool corten_arena_overlaps(struct corten_mm_state *state,
				  unsigned long addr, unsigned long len)
{
	unsigned long frame = addr >> PMD_SHIFT;
	unsigned long last = (addr + len - 1) >> PMD_SHIFT;

	for (; frame <= last; frame++) {
		if (xa_load(&state->arenas, frame))
			return true;
	}

	return false;
}

/*
 * [C1/R1 closure] DECLARE requires an *empty* range.  Adopting a range
 * with resident pages would leave hardware PTEs whose metadata is
 * CORTEN_INVALID: the fault path would MAPERR the process's own data,
 * and the metadata-driven chunk zap would skip those pages (leak +
 * content surviving the arena contract).  Stricter than the design text
 * (sec 2.1 says nothing about emptiness) -- deviation recorded for the
 * M3B notes.
 *
 * Called with mmap_write_lock held (no legacy fault can enter a
 * write-locked VMA), but the walk still takes the PTE lock per window:
 * before publication this range is not yet arena-managed, so ptl makes
 * the check airtight.
 */
static int corten_arena_check_empty_locked(struct mm_struct *mm,
					   unsigned long start,
					   unsigned long end)
{
	unsigned long addr;

	for (addr = start; addr < end;
	     addr = min((addr | (PMD_SIZE - 1)) + 1, end)) {
		unsigned long win_end = min((addr | (PMD_SIZE - 1)) + 1, end);
		unsigned long a;
		pmd_t *pmdp;
		pte_t *ptep;
		spinlock_t *ptl;

		pmdp = corten_arena_pmd(mm, addr);
		if (!pmdp)
			continue;		/* upper levels absent: empty */
		if (!pmd_present(READ_ONCE(*pmdp)))
			continue;		/* no PT page: empty */
		if (pmd_leaf(READ_ONCE(*pmdp)))
			return -EBUSY;		/* THP content */

		ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
		if (!ptep)
			continue;
		for (a = addr; a < win_end; a += PAGE_SIZE) {
			if (pte_present(ptep[pte_index(a)])) {
				pte_unmap_unlock(ptep, ptl);
				return -EBUSY;
			}
		}
		pte_unmap_unlock(ptep, ptl);
	}

	return 0;
}

/* ------------------------------------------------------------------ *
 * DECLARE / RELEASE / QUERY
 * ------------------------------------------------------------------
 */

/*
 * The DECLARE body proper (P0, DEV-13).  The caller holds @mm's mmap_lock
 * for writing; this nests state->ctl_lock inside it -- the inversion of
 * the M3b order, so that the do_mmap auto-attach (which runs with the
 * write lock already held) can drive the very same body without forming
 * the reverse edge against a concurrent DECLARE.  Allocates the
 * descriptor; on error the caller's range is untouched.
 */
static int corten_arena_declare_locked(struct mm_struct *mm,
				       struct corten_mm_state *state,
				       unsigned long addr, unsigned long len)
{
	unsigned long frame, first_frame, last_frame;
	struct corten_arena *arena;
	struct vm_area_struct *vma;
	int ret;

	arena = kzalloc(sizeof(*arena), GFP_KERNEL_ACCOUNT);
	if (!arena)
		return -ENOMEM;

	arena->start = addr;
	arena->end = addr + len;
	arena->mm = mm;
	mutex_init(&arena->fill_lock);
	init_completion(&arena->drained);
	/* Born atomic (D15): a percpu-born ref forces percpu_ref_kill()'s
	 * atomic switch through a full RCU grace period, and RELEASE drains
	 * with mmap_write held -- every arena munmap paid one GP
	 * (4.9-20ms measured, r06-t5), which ran the dedup_eq tcmalloc arm
	 * at 12x its base wall time.  An atomic-born ref makes the kill
	 * synchronous: the drain only ever waits for in-flight
	 * transactions, never for grace.  The fault path's tryget/put pay
	 * one contended atomic op instead of a percpu one -- the
	 * transactions serialize on the covering desc write lock anyway.
	 */
	ret = percpu_ref_init(&arena->active, corten_arena_active_release,
			      PERCPU_REF_INIT_ATOMIC, GFP_KERNEL);
	if (ret) {
		mutex_destroy(&arena->fill_lock);
		kfree(arena);
		return ret;
	}

	mutex_lock(&state->ctl_lock);

	/* Arena ranges must not overlap; also rejects re-declaring the
	 * same range.  Exclusive under ctl_lock.
	 */
	if (corten_arena_overlaps(state, addr, len)) {
		ret = -EEXIST;
		goto out_free_arena;
	}

	/* Validation and conversion happen together under the write lock
	 * (held by the caller): the VMA must survive as one piece up to
	 * the flag flip anyway, so a separate read-lock pre-pass would
	 * only duplicate the check.  Publish with the shadow-VMA in place
	 * before the xarray entries, so a concurrent S4 lookup never
	 * resolves an arena whose VMA is not yet converted ("arena
	 * addresses always have their shadow-VMA", sec 2.3/4.3).  Nothing
	 * can fault through the arena path yet: the frames only become
	 * visible to lookup after the stores below.
	 */
	vma = vma_lookup(mm, addr);
	ret = corten_arena_validate_vma(vma, addr, len);
	if (ret)
		goto out_free_arena;

	/* [C1] the range must be empty (see the checker's comment). */
	ret = corten_arena_check_empty_locked(mm, addr, addr + len);
	if (ret)
		goto out_free_arena;

	arena->prot = corten_arena_prot_from_vma(vma);
	ret = corten_arena_shadowize(vma);
	if (ret)
		goto out_free_arena;

	/* Publish the cached shadow-VMA before the arena becomes visible
	 * in the xarray below ([FAIL-2]): every reader that resolves the
	 * arena also resolves the VMA.
	 */
	WRITE_ONCE(arena->vma, vma);

	first_frame = addr >> PMD_SHIFT;
	last_frame = (addr + len - 1) >> PMD_SHIFT;
	for (frame = first_frame; frame <= last_frame; frame++) {
		ret = xa_err(xa_store(&state->arenas, frame, arena,
				      GFP_KERNEL));
		if (ret)
			goto out_unwind;
	}

	/* @nr is a plain 0..N counter that legitimately passes through 0
	 * (between the release of the last arena and the next DECLARE),
	 * so refcount_t's add/inc-from-zero saturation is not applicable:
	 * update it with set semantics -- safe because both writers hold
	 * ctl_lock, while readers only do a lockless read.
	 */
	refcount_set(&state->nr, refcount_read(&state->nr) + 1);
	corten_arena_stat_add(state, CORTEN_ARENA_STAT_DECLARES, 1);

	/* Last publishing step: the observability ledger (S8).  All
	 * failure paths are behind us, so the ledger only ever contains
	 * fully registered arenas.
	 */
	corten_arena_obs_add(arena);

	mutex_unlock(&state->ctl_lock);

	return 0;

out_unwind:
	while (frame > first_frame)
		xa_erase(&state->arenas, --frame);
	corten_arena_unshadow(arena, vma);
out_free_arena:
	mutex_unlock(&state->ctl_lock);
	percpu_ref_exit(&arena->active);
	mutex_destroy(&arena->fill_lock);
	kfree(arena);
	return ret;
}

int corten_arena_declare(struct mm_struct *mm, unsigned long addr,
			 unsigned long len)
{
	struct corten_mm_state *state;
	int ret;

	if (!mm)
		return -EINVAL;
	if (addr & (PMD_SIZE - 1))
		return -EINVAL;
	if (!len || (len & (PMD_SIZE - 1)) || len > TASK_SIZE - addr)
		return -EINVAL;

	/* Pairs with the store in corten_arena_state_create(). */
	state = smp_load_acquire(&mm->corten_state);
	if (!state) {
		state = corten_arena_state_create(mm);
		if (!state)
			return -ENOMEM;
	}

	/* P0 (DEV-13): mmap_write is the outermost lock, ctl_lock nests
	 * inside it.
	 */
	mmap_write_lock(mm);
	ret = corten_arena_declare_locked(mm, state, addr, len);
	mmap_write_unlock(mm);

	return ret;
}

/*
 * The RELEASE body proper (P0, DEV-13).  The caller holds @mm's mmap_lock
 * for writing; this nests state->ctl_lock inside it.
 *
 * The drain now waits under BOTH locks and is still deadlock-free
 * (see the lock-order contract in include/linux/corten_arena.h): a
 * transaction pins the arena with percpu_ref tryget/put only -- the S4
 * fast hook (arch/x86/mm/fault.c) runs before any mmap_lock action and
 * the S4 slow gate (mm/memory.c handle_mm_fault) may hold the read lock
 * or a per-VMA lock, but a reader holding mmap_lock(R) cannot exist while
 * we hold it for writing, and neither variant ever waits for mmap_write
 * or ctl_lock -- so the drain converges.  The percpu_ref release callback
 * fires from RCU softirq context and only completes().  Holding
 * mmap_write across the wait merely delays legacy space operations for
 * the remaining lifetime of the in-flight transactions (microseconds)
 * and guarantees the teardown below sees a quiesced range.  (The M3b
 * order -- drain under ctl_lock, then take mmap_write -- formed exactly
 * that reverse edge against the do_mmap auto-attach and was inverted.)
 */
static int corten_arena_release_locked(struct mm_struct *mm,
				       struct corten_mm_state *state,
				       unsigned long addr, unsigned long len)
{
	unsigned long frame, first_frame, last_frame;
	struct corten_arena *arena;
	bool drained;
	int ret;

	mutex_lock(&state->ctl_lock);

	/* RELEASE must hit one declared arena exactly (by start). */
	first_frame = addr >> PMD_SHIFT;
	arena = xa_load(&state->arenas, first_frame);
	if (!arena || arena->start != addr || arena->end != addr + len) {
		ret = -ENOENT;
		goto out_unlock;
	}

	last_frame = (addr + len - 1) >> PMD_SHIFT;
	for (frame = first_frame; frame <= last_frame; frame++) {
		struct corten_arena *stale = xa_erase(&state->arenas, frame);

		/* [F-B] An already-NULL frame is legal since the D-G''
		 * punch route: a file MAP_FIXED may have erased the hole's
		 * frames earlier.  Only a foreign entry is a kernel bug.
		 */
		if (WARN_ON_ONCE(stale && stale != arena))
			break;
	}
	refcount_set(&state->nr, refcount_read(&state->nr) - 1);
	corten_arena_stat_add(state, CORTEN_ARENA_STAT_RELEASES, 1);

	/* Deregister from the observability ledger before the drain: a
	 * RELEASE that has made the arena unreachable in its own registry
	 * must not stay visible to debugfs either (the drain can wait up
	 * to CORTEN_ARENA_DRAIN_TIMEOUT on a broken kernel).
	 */
	corten_arena_obs_remove(arena);

	/* Drain in-flight transactions (see the locking argument above),
	 * then tear the shadow-VMA down with the plain legacy munmap:
	 * pages are zapped and the PT pages travel through the regular
	 * free funnels (M2a uninstall).  On a drain timeout (leaked
	 * reference, a kernel bug) the teardown still runs: a runnable
	 * process with a counted leak beats an unkillable D-state zombie;
	 * the in-flight-transaction safety argument above is void in that
	 * case, which is why the leak is counted loudly
	 * (CORTEN_ARENA_STAT_DRAIN_TIMEOUTS + the global arena_stats
	 * aggregate).
	 */
	if (!corten_arena_drain(arena)) {
		corten_arena_note_drain_timeout(state);
		drained = false;
	} else {
		drained = true;
	}

	{
		/* [M3b S6 transition] Strip the shadow decoration before
		 * the legacy teardown: the do_vmi_align_munmap() arena
		 * guard (corten_arena_munmap_vma_guard()) rejects every
		 * range that still overlaps a VM_CORTEN vma, and RELEASE's
		 * own munmap must go through the regular funnel.  Clearing
		 * the cached shadow-VMA is safe here: the arena is
		 * drained, so no transaction can be reading arena->vma
		 * ([FAIL-2]).  The error path below re-strips
		 * idempotently.
		 */
		VMA_ITERATOR(vmi, mm, arena->start);
		struct vm_area_struct *vma;

		for_each_vma_range(vmi, vma, arena->end)
			corten_arena_unshadow(arena, vma);
	}
	ret = do_munmap(mm, arena->start, arena->end - arena->start, NULL);
	if (ret) {
		/* Memory pressure: the range survived (whole or in pieces).
		 * Strip the shadow decoration so no VM_CORTEN VMA outlives
		 * its arena descriptor; the pieces keep working as plain
		 * anonymous memory.
		 */
		VMA_ITERATOR(vmi, mm, arena->start);
		struct vm_area_struct *vma;

		for_each_vma_range(vmi, vma, arena->end)
			corten_arena_unshadow(arena, vma);
	}

	/* The descriptor is unreachable and drained; percpu_ref_exit()
	 * before the kfree_rcu() it is embedded in (sec 2.1).  On the
	 * (memory-pressure) do_munmap() error path the arena is already
	 * deregistered and the stale shadow-VMA simply behaves as a plain
	 * anonymous VMA.  On a drain timeout the descriptor is
	 * deliberately leaked instead: percpu_ref_exit() with references
	 * outstanding would turn the straggler put into a UAF.
	 */
	if (drained)
		corten_arena_free(arena);
	mutex_unlock(&state->ctl_lock);

	return ret;

out_unlock:
	mutex_unlock(&state->ctl_lock);
	return ret;
}

int corten_arena_release(struct mm_struct *mm, unsigned long addr,
			 unsigned long len)
{
	struct corten_mm_state *state;
	int ret;

	if (!mm)
		return -EINVAL;
	if (addr & (PMD_SIZE - 1))
		return -EINVAL;
	if (!len || (len & (PMD_SIZE - 1)))
		return -EINVAL;

	/* Pairs with the store in corten_arena_state_create(). */
	state = smp_load_acquire(&mm->corten_state);
	if (!state)
		return -ENOENT;

	/* P0 (DEV-13): mmap_write is the outermost lock, ctl_lock nests
	 * inside it.
	 */
	mmap_write_lock(mm);
	ret = corten_arena_release_locked(mm, state, addr, len);
	mmap_write_unlock(mm);

	return ret;
}

int corten_arena_query(struct mm_struct *mm, unsigned long addr)
{
	struct corten_arena *arena;

	if (!mm)
		return -EINVAL;
	if (!smp_load_acquire(&mm->corten_state)) /* pairs with the publish store */
		return -ENOENT;

	rcu_read_lock();
	arena = corten_arena_lookup(mm, addr);
	rcu_read_unlock();

	return arena ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 * S8 observability renderers (debugfs, M3B_DESIGN.md sec 7.4)
 * ------------------------------------------------------------------
 */

/*
 * One line per live arena across every mm: owning mm, cached shadow-VMA,
 * range, recorded prot and the liveness of the transaction refcount
 * (active: transactions may enter; dying: kill issued, draining).  In
 * practice RELEASE deregisters before the drain's kill, so entries are
 * normally observed active -- the dying branch is defensive.  The
 * percpu transaction count itself has no race-free reader by design, so
 * the observable liveness states are what the file reports.  RCU walk:
 * arenas unlinked concurrently simply do not show up.
 */
void corten_arena_arenas_report(struct seq_file *m)
{
	struct corten_arena *ar;

	seq_puts(m, "              mm              vma [start,end)                prot status\n");

	rcu_read_lock();
	list_for_each_entry_rcu(ar, &corten_arena_list, obs) {
		seq_printf(m, "%016lx %016lx [%lx,%lx)           %02x %s\n",
			   (unsigned long)READ_ONCE(ar->mm),
			   (unsigned long)READ_ONCE(ar->vma),
			   ar->start, ar->end, ar->prot,
			   percpu_ref_is_dying(&ar->active) ?
					"dying" : "active");
	}
	rcu_read_unlock();
}

/*
 * Arena-layer aggregate: number of live arenas plus the drain-timeout
 * total (the global mirror of the per-mm CORTEN_ARENA_STAT_DRAIN_TIMEOUTS
 * counters, which are not reachable from debugfs -- r03 final-smoke
 * legacy item 1).  Callers prepend the protocol-layer lines
 * (corten_stats_lines() in mm/corten.c), so this file alone gives the
 * full picture.
 */
void corten_arena_stats_report(struct seq_file *m)
{
	struct corten_arena *ar;
	int nr = 0;

	rcu_read_lock();
	list_for_each_entry_rcu(ar, &corten_arena_list, obs)
		nr++;
	rcu_read_unlock();

	seq_printf(m, "arenas              %d\n", nr);
	seq_printf(m, "drain_timeout       %ld\n",
		   atomic_long_read(&corten_arena_nr_drain_timeouts));
	/* T0b named route counters (M4T0_SPEC.md sec 7 DoD evidence). */
	seq_printf(m, "auto_mmaps          %ld\n",
		   atomic_long_read(&corten_nr_auto_mmaps));
	seq_printf(m, "auto_attach_fail    %ld\n",
		   atomic_long_read(&corten_nr_auto_attach_fails));
	seq_printf(m, "auto_fallbacks      %ld\n",
		   atomic_long_read(&corten_nr_auto_fallbacks));
	seq_printf(m, "auto_exhausted      %ld\n",
		   atomic_long_read(&corten_nr_auto_exhausted));
	seq_printf(m, "mprotect_routes     %ld\n",
		   atomic_long_read(&corten_nr_mprotect_routes));
	seq_printf(m, "madvise_free_txns   %ld\n",
		   atomic_long_read(&corten_nr_madvise_free_txns));
	seq_printf(m, "madvise_hints       %ld\n",
		   atomic_long_read(&corten_nr_madvise_hints));
	seq_printf(m, "mremap_routes       %ld\n",
		   atomic_long_read(&corten_nr_mremap_routes));
	seq_printf(m, "mremap_rejects      %ld\n",
		   atomic_long_read(&corten_nr_mremap_rejects));
	seq_printf(m, "munmap_releases     %ld\n",
		   atomic_long_read(&corten_nr_munmap_releases));
	seq_printf(m, "fork_demotes        %ld\n",
		   atomic_long_read(&corten_nr_fork_demotes));
	seq_printf(m, "fork_faithful       %ld\n",
		   atomic_long_read(&corten_nr_fork_faithful));
	seq_printf(m, "fork_skips          %ld\n",
		   atomic_long_read(&corten_nr_fork_skips));
	seq_printf(m, "rearm_recovered     %ld\n",
		   atomic_long_read(&corten_nr_rearm_recovered));
	seq_printf(m, "rearm_failed        %ld\n",
		   atomic_long_read(&corten_nr_rearm_failed));
	seq_printf(m, "eagain_retries      %ld\n",
		   atomic_long_read(&corten_nr_eagain_retries));
	seq_printf(m, "eagain_leaked       %ld\n",
		   atomic_long_read(&corten_nr_eagain_leaked));
	seq_printf(m, "mremap_release_fail %ld\n",
		   atomic_long_read(&corten_nr_mremap_release_fail));
	seq_printf(m, "mmap_punches        %ld\n",
		   atomic_long_read(&corten_nr_mmap_punches));
	seq_printf(m, "mmap_punch_rejects  %ld\n",
		   atomic_long_read(&corten_nr_mmap_punch_rejects));
}

#ifdef CONFIG_CORTEN_MM_ARENA_KUNIT_TEST
void corten_arena_test_inject_drain_timeout(void)
{
	corten_arena_note_drain_timeout(NULL);
}

long corten_arena_test_drain_timeouts(void)
{
	return atomic_long_read(&corten_arena_nr_drain_timeouts);
}

void corten_arena_test_fork_fail_arm(int stage)
{
	corten_fork_fail_stage = stage;
}

bool corten_arena_test_arena_frozen(struct mm_struct *mm, unsigned long addr)
{
	struct corten_arena *ar;
	bool frozen;

	rcu_read_lock();
	ar = corten_arena_lookup(mm, addr);
	frozen = ar && READ_ONCE(ar->frozen);
	rcu_read_unlock();

	return frozen;
}

long corten_arena_test_fork_faithful_count(void)
{
	return atomic_long_read(&corten_nr_fork_faithful);
}

long corten_arena_test_fork_skips(void)
{
	return atomic_long_read(&corten_nr_fork_skips);
}
#endif

/* ------------------------------------------------------------------ *
 * process exit (sec 5.2)
 * ------------------------------------------------------------------
 */

/*
 * Called from exit_mmap() before the legacy teardown: drain and free every
 * arena, then drop the registry.  mm_users is already 0 here, so no fault
 * can be in flight and the drain is purely defensive; the unmap_vmas() that
 * follows retires the shadow-VMAs' page tables through the regular free
 * funnels.
 */
void corten_arena_mm_exit(struct mm_struct *mm)
{
	struct corten_mm_state *state;
	unsigned long frame = 0, drained_until = 0;
	struct corten_arena *arena;

	/* Pairs with the store in corten_arena_state_create(). */
	state = smp_load_acquire(&mm->corten_state);
	if (!state)
		return;
	/* Unpublish; no reader can be racing (mm_users == 0). */
	smp_store_release(&mm->corten_state, NULL);

	/* DEV-13: exit drains with mm_users already 0 and takes only the
	 * lower ctl_lock -- "the lower lock alone" cannot form a cycle.
	 */
	mutex_lock(&state->ctl_lock);

	xa_for_each(&state->arenas, frame, arena) {
		/* Every frame of an arena holds the same descriptor; drain
		 * each one once, at its first frame.  A drain timeout (a
		 * leaked reference -- mm_users is 0 here, so only a kernel
		 * bug can cause it) must not hang exit_mmap: count the
		 * leak, keep the descriptor alive (a straggler put would
		 * otherwise UAF), and let the teardown finish.  A leaked
		 * ~150-byte descriptor on a broken kernel beats an
		 * unkillable dying process.
		 */
		if (frame < drained_until)
			continue;

		drained_until = arena->end >> PMD_SHIFT;
		corten_arena_obs_remove(arena);
		if (corten_arena_drain(arena))
			corten_arena_free(arena);
		else
			corten_arena_note_drain_timeout(state);
	}

	mutex_unlock(&state->ctl_lock);

	corten_arena_state_free(state);
}

/* ------------------------------------------------------------------ *
 * lookup and prctl dispatch
 * ------------------------------------------------------------------
 */

struct corten_arena *corten_arena_lookup(struct mm_struct *mm,
					 unsigned long addr)
{
	struct corten_mm_state *state;

	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			 "arena lookup without rcu_read_lock() protection");

	/* Two-load fast negation: processes without arenas (and every
	 * non-arena access in arena processes) never reach the xarray.
	 */
	state = smp_load_acquire(&mm->corten_state);
	if (!state || !refcount_read(&state->nr))
		return NULL;

	return xa_load(&state->arenas, addr >> PMD_SHIFT);
}

int corten_prctl_arena(unsigned int op, unsigned long addr, unsigned long len,
		       unsigned long arg5)
{
	struct mm_struct *mm = current->mm;

	if (arg5)
		return -EINVAL;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	/* Boot-time gate: with corten=off the prctl behaves like on a
	 * kernel without the feature -- probeable, but without side
	 * effects (sec 2.1-2).
	 */
	if (!corten_enabled_static())
		return -EOPNOTSUPP;

	switch (op) {
	case CORTEN_ARENA_DECLARE:
		return corten_arena_declare(mm, addr, len);
	case CORTEN_ARENA_RELEASE:
		return corten_arena_release(mm, addr, len);
	case CORTEN_ARENA_QUERY:
		return corten_arena_query(mm, addr);
	default:
		return -EINVAL;
	}
}

/* ------------------------------------------------------------------ *
 * T0a: MODE-process transparent takeover (M4T0_SPEC.md sec 1/3/5)
 * ------------------------------------------------------------------
 */

enum corten_mmap_class corten_arena_auto_mmap_classify(unsigned long flags,
						       bool file)
{
	if (file)
		return CORTEN_MMAP_LEGACY;

	/* The type bits must be exactly MAP_PRIVATE: this rejects
	 * MAP_SHARED/MAP_SHARED_VALIDATE and absorbs the MAP_DROPPABLE
	 * alias bit (0x08 lives inside the MAP_TYPE nibble).
	 */
	if ((flags & MAP_TYPE) != MAP_PRIVATE)
		return CORTEN_MMAP_LEGACY;
	if (!(flags & MAP_ANONYMOUS))
		return CORTEN_MMAP_LEGACY;

	/* Single-bit whitelist: any other flag-word bit -- MAP_FIXED,
	 * MAP_FIXED_NOREPLACE, MAP_HUGETLB, MAP_GROWSDOWN, MAP_POPULATE,
	 * MAP_LOCKED, MAP_SYNC, MAP_STACK, MAP_UNINITIALIZED,
	 * MAP_DENYWRITE, MAP_EXECUTABLE, MAP_NONBLOCK, ... -- keeps the
	 * mapping legacy.  MAP_STACK stays excluded per OQ-B until the
	 * mprotect routing (T0b) can serve thread-stack guard pages.
	 */
	if (flags & ~(MAP_TYPE | MAP_ANONYMOUS | MAP_NORESERVE))
		return CORTEN_MMAP_LEGACY;

	return CORTEN_MMAP_AUTO;
}

int corten_arena_auto_place(unsigned long next_va, unsigned long len,
			    unsigned long *addr)
{
	unsigned long addr2;

	/* Window-sized or larger requests can never fit. */
	if (!len || len > CORTEN_MODE_WINDOW_END - CORTEN_MODE_WINDOW_START)
		return -ENOSPC;

	addr2 = round_up(next_va, PMD_SIZE);
	if (addr2 < CORTEN_MODE_WINDOW_START)
		addr2 = CORTEN_MODE_WINDOW_START;

	/* Overflow-safe bounds check: addr2 <= WIN_END - len <=> the whole
	 * [addr2, addr2+len) fits in the window.
	 */
	if (addr2 > CORTEN_MODE_WINDOW_END - len)
		return -ENOSPC;

	*addr = addr2;
	return 0;
}

/*
 * Window placement under this mm's mmap_write lock (T0b): hand out the
 * next PMD-rounded [addr2, addr2+len2) from the MODE cursor, skipping
 * past any obstacle (a MODE-targeted DECLARE inside the window, or a
 * plain legacy VMA that ended up here -- explicit-address mappings stay
 * legacy by contract) so the caller's MAP_FIXED install can never
 * destroy an existing mapping.  The write lock we hold is what keeps
 * obstacles from multiplying under our feet; each skip step strictly
 * advances.  Shared by the auto-mmap route (T0a) and the mremap move
 * route (T0b).  The cursor is advanced only on success (T0 never
 * recycles window VA -- T1's per-cpu magazine does).
 *
 * Return: 0 with *@addr2 set, -ENOSPC when the window cannot hold the
 * request (the caller degrades, counted).
 */
static int corten_arena_window_place(struct mm_struct *mm,
				     struct corten_mm_state *state,
				     unsigned long len2, unsigned long *addr2)
{
	unsigned long a2;
	int ret;

	ret = corten_arena_auto_place(state->next_va, len2, &a2);
	if (ret)
		return ret;

	for (;;) {
		struct corten_arena *ar;
		struct vm_area_struct *vma;
		unsigned long obstacle_end = 0;

		rcu_read_lock();
		ar = corten_arena_lookup(mm, a2);
		if (ar)
			obstacle_end = ar->end;
		rcu_read_unlock();

		if (!ar) {
			vma = find_vma_intersection(mm, a2, a2 + len2);
			if (vma)
				obstacle_end = vma->vm_end;
		}

		if (!obstacle_end)
			break;

		a2 = round_up(obstacle_end, PMD_SIZE);
		if (a2 < obstacle_end ||	/* overflow */
		    a2 > CORTEN_MODE_WINDOW_END - len2)
			return -ENOSPC;
	}

	state->next_va = a2 + len2;
	*addr2 = a2;

	return 0;
}

/* The registry, ensured to exist (the auto cursor lives in it).  Called
 * under this mm's mmap_write lock; the creation itself is serialized by
 * the global corten_arena_alloc_lock.
 */
static struct corten_mm_state *corten_arena_get_state(struct mm_struct *mm)
{
	/* Pairs with the store in corten_arena_state_create(). */
	struct corten_mm_state *state = smp_load_acquire(&mm->corten_state);

	if (!state)
		state = corten_arena_state_create(mm);

	return state;
}

static void corten_arena_auto_fallback(struct corten_mm_state *state)
{
	/* Degrade to the legacy mmap path, counted (per-mm bucket plus the
	 * T0b named auto_fallbacks aggregate the arena_stats file shows).
	 */
	if (state)
		this_cpu_inc(state->stats[CORTEN_ARENA_STAT_FALLBACKS]);
	atomic_long_inc(&corten_nr_auto_fallbacks);
}

/* Window exhaustion (T0-R2) -- its own named bucket on top of the total
 * fallback count, so "graceful degradation" stays distinguishable from
 * "attach failed".
 */
static void corten_arena_auto_exhausted(struct corten_mm_state *state)
{
	corten_arena_auto_fallback(state);
	atomic_long_inc(&corten_nr_auto_exhausted);
}

int corten_arena_auto_mmap_route(struct mm_struct *mm, unsigned long len,
				 unsigned long prot, unsigned long *addr,
				 unsigned long *lenp, unsigned long *flagsp)
{
	struct corten_mm_state *state;
	unsigned long addr2, len2;
	int ret;

	if (!corten_enabled_static() || !mm || !READ_ONCE(mm->corten_mode))
		return 0;
	/* PROT_NONE and every prot combination are arena-able
	 * (corten_arena_prot_from_vma() mirrors the VMA bits); @prot is
	 * not part of the decision.
	 */

	/* OVERCOMMIT_NEVER does not honour MAP_NORESERVE (mm/mmap.c:563),
	 * so the created VMA would carry VM_ACCOUNT and fail the DECLARE
	 * validation chain wholesale -- degrade the auto takeover of this
	 * configuration to legacy up front (DEV-12).
	 */
	if (sysctl_overcommit_memory == OVERCOMMIT_NEVER) {
		corten_arena_auto_fallback(NULL);
		return 0;
	}

	if (corten_arena_auto_mmap_classify(*flagsp, false) !=
	    CORTEN_MMAP_AUTO)
		return 0;

	state = corten_arena_get_state(mm);
	if (!state)
		return -ENOMEM;

	/* The application's page-rounded length is rounded up to the 2M
	 * arena granularity; the tail is kernel-private padding, invisible
	 * to the application and covered by the munmap release rule
	 * (corten_arena_release_classify()).
	 */
	len2 = round_up(*lenp, PMD_SIZE);
	if (!len2) {
		/* Nothing arena-able left after rounding (len was 0 or the
		 * window-relayed request degenerated): let the normal path
		 * produce its -ENOMEM/-EINVAL.
		 */
		corten_arena_auto_fallback(state);
		return 0;
	}

	ret = corten_arena_window_place(mm, state, len2, &addr2);
	if (ret) {
		/* Window exhausted or obstacle-skip ran out of window
		 * (T0-R2): graceful degradation, the mapping goes to the
		 * legacy mmap_base area.  The cursor is not rewound --
		 * T1's per-cpu magazine does VA reuse.
		 */
		corten_arena_auto_exhausted(state);
		return 0;
	}

	/* Commit: hand the range out and rewrite the request onto the
	 * window.  The caller creates the VMA through the normal
	 * MAP_FIXED flow (__get_unmapped_area() verifies the segment is
	 * free) and then attaches it with corten_arena_auto_attach().
	 */
	*addr = addr2;
	*lenp = len2;
	*flagsp |= MAP_FIXED | MAP_NORESERVE;

	return 1;
}

int corten_arena_auto_attach(struct mm_struct *mm, unsigned long addr,
			     unsigned long len)
{
	struct corten_mm_state *state;
	int ret;

	mmap_assert_write_locked(mm);

	/* Pairs with the store in corten_arena_state_create(); the route
	 * has created the registry for this MODE mm already.
	 */
	state = smp_load_acquire(&mm->corten_state);
	if (!state)
		return -ENOENT;

	/* DECLARE's locked body under the write lock the caller (do_mmap)
	 * already holds -- exactly the nesting the P0 inversion (DEV-13)
	 * enables.  Failure degrades to a plain legacy anonymous VMA at a
	 * window address: faults and munmaps on it resolve no arena and
	 * run the legacy paths (harmless, counted).
	 */
	ret = corten_arena_declare_locked(mm, state, addr, len);
	if (ret) {
		/* T0a counted attach failures in the per-mm fallback
		 * bucket; T0b adds the named aggregates on top.
		 */
		corten_arena_auto_fallback(state);
		atomic_long_inc(&corten_nr_auto_attach_fails);
	} else {
		atomic_long_inc(&corten_nr_auto_mmaps);
	}

	return ret;
}

int corten_arena_mode_enter(struct mm_struct *mm)
{
	struct corten_mm_state *state;

	if (!mm)
		return -EINVAL;

	mmap_write_lock(mm);
	if (READ_ONCE(mm->corten_mode)) {
		mmap_write_unlock(mm);
		return 0;		/* idempotent */
	}

	/* The cursor registry must exist before the first auto mmap hits
	 * the route; creating it here keeps the hot path allocation-free.
	 */
	state = corten_arena_get_state(mm);
	if (!state) {
		mmap_write_unlock(mm);
		return -ENOMEM;
	}

	WRITE_ONCE(mm->corten_mode, true);
	mmap_write_unlock(mm);

	return 0;
}

int corten_arena_mode_exit(struct mm_struct *mm)
{
	struct corten_mm_state *state;
	int ret = 0;

	if (!mm)
		return -EINVAL;

	/* Pairs with the store in corten_arena_state_create(); NULL is a
	 * valid answer here (never-entered MODE process).
	 */
	state = smp_load_acquire(&mm->corten_state);

	/* Hold the write lock across the whole operation so that no mmap
	 * can slip in behind our back and re-enter MODE territory; each
	 * release steals one arena from the registry, so the loop
	 * terminates.  Nothing can contend ctl_lock meanwhile (every other
	 * user of it needs this write lock first).
	 */
	mmap_write_lock(mm);

	if (state) {
		for (;;) {
			unsigned long frame = 0;
			struct corten_arena *ar;
			int r;

			ar = xa_find(&state->arenas, &frame, ULONG_MAX,
				     XA_PRESENT);
			if (!ar)
				break;

			r = corten_arena_release_locked(mm, state, ar->start,
							ar->end - ar->start);
			if (r) {
				/* Memory pressure mid-teardown: keep the
				 * mode cleared (the process asked to leave)
				 * and report the first failure; the
				 * remaining arenas stay registered and are
				 * retired by exit_mmap().
				 */
				if (!ret)
					ret = r;
				break;
			}
		}
	}

	WRITE_ONCE(mm->corten_mode, false);
	mmap_write_unlock(mm);

	return ret;
}

int corten_arena_mode_get(struct mm_struct *mm)
{
	if (!mm)
		return -EINVAL;

	return READ_ONCE(mm->corten_mode) ? 1 : 0;
}

int corten_prctl_mode(unsigned int op, unsigned long arg3,
		      unsigned long arg4, unsigned long arg5)
{
	struct mm_struct *mm = current->mm;

	if (arg3 || arg4 || arg5)
		return -EINVAL;
	if (op != CORTEN_MODE_ENTER && op != CORTEN_MODE_EXIT &&
	    op != CORTEN_MODE_GET)
		return -EINVAL;
	/* Same boot gate as PR_CORTEN_ARENA (corten=off == feature off),
	 * same capability posture for ENTER; EXIT/GET only need the
	 * feature on (M4T0_SPEC.md sec 1.1).
	 */
	if (!corten_enabled_static())
		return -EOPNOTSUPP;

	switch (op) {
	case CORTEN_MODE_ENTER:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		return corten_arena_mode_enter(mm);
	case CORTEN_MODE_EXIT:
		return corten_arena_mode_exit(mm);
	default:
		return corten_arena_mode_get(mm);
	}
}

/* ------------------------------------------------------------------ *
 * M5.T1a: faithful fork (M5_FORK_SPEC.md sec 1.3, DEV-14/DEV-15).
 *
 * The T0 fork_demote exit strategy is replaced wholesale (its
 * scrub/materialize/unshadow teardown is deleted, not kept as a
 * fallback): fork no longer touches the arena lifecycle.  The parent
 * keeps its arenas and the child receives a full semantic mirror --
 * shadow-VMAs by vm_area_dup(), page contents by the ordinary
 * copy_page_range() (whose wrprotect is the corten_glue_pte_write
 * whitelist entry #1, DEV-14) and the metadata/registry by the two
 * hooks below.  A failure aborts the fork with the upstream dup_mmap
 * precedent; the one red line is that the parent must never stay
 * frozen (R-A), which is why every exit funnels through the single
 * unfreeze closure.
 * ------------------------------------------------------------------
 */

/*
 * The single R-A unwind closure: re-arm the transaction refcount the
 * freeze-drain killed and clear the frozen bit, for every arena this
 * fork froze.  Called with oldmm's mmap_write and @state->ctl_lock
 * held, so no lookup can observe an intermediate state: a fault either
 * still sees frozen (falls back and blocks on the write lock) or a live
 * ref and a clear bit.  Child arenas have no frozen bit (born live; the
 * child cannot schedule until dup_mmap() returns), so "unfreeze" only
 * ever touches parent-side descriptors.
 */
static void corten_arena_fork_unfreeze_locked(struct corten_mm_state *state)
{
	unsigned long frame = 0, unfrozen_until = 0;
	struct corten_arena *arena;

	xa_for_each(&state->arenas, frame, arena) {
		/* Every frame of an arena holds the same descriptor. */
		if (frame < unfrozen_until)
			continue;
		unfrozen_until = arena->end >> PMD_SHIFT;

		if (!READ_ONCE(arena->frozen))
			continue;
		/* The drain-timeout shape (E5, a kernel bug): reinit
		 * WARNs once on a non-zero count (leaked transaction
		 * reference), the loud signal the E5 contract asks for;
		 * the resurrect itself puts the ref into a consistent
		 * live state so a straggler's put still lands safely.
		 */
		percpu_ref_reinit(&arena->active);
		WRITE_ONCE(arena->frozen, false);
	}
}

void corten_arena_fork_abort(struct mm_struct *oldmm)
{
	struct corten_mm_state *state;

	/* Pairs with the store in corten_arena_state_create(). */
	state = smp_load_acquire(&oldmm->corten_state);
	if (!state || !refcount_read(&state->nr))
		return;

	mmap_assert_write_locked(oldmm);
	mutex_lock(&state->ctl_lock);
	corten_arena_fork_unfreeze_locked(state);
	mutex_unlock(&state->ctl_lock);
}

int corten_arena_fork_begin(struct mm_struct *mm, struct mm_struct *oldmm)
{
	struct corten_mm_state *old_state;
	unsigned long frame = 0, drained_until = 0;
	struct corten_arena *arena;

	/* MODE-bit inheritance (the T0 sec 5.2 rule, moved here by
	 * M5_FORK_SPEC.md 1.3-2): both sides keep auto-attaching their
	 * *new* mmaps; the existing arenas are mirrored by fork_commit.
	 * The child mm is not visible to anyone yet, the parent's is
	 * write-locked by dup_mmap() -- the MODE writer contract holds.
	 */
	WRITE_ONCE(mm->corten_mode, READ_ONCE(oldmm->corten_mode));

	/* R-G: a dying process aborts at the dup_mmap() loop's
	 * fatal_signal_pending() checkpoint before any mirroring work --
	 * do not pay for the freeze.  (A signal arriving after this point
	 * is covered by fork_commit's checks and fork_abort().)
	 */
	if (fatal_signal_pending(current))
		return -EINTR;

	/* Pairs with the store in corten_arena_state_create(); NULL means
	 * the parent never had arenas: one load, a no-arena fork is
	 * unchanged (E1).
	 */
	old_state = smp_load_acquire(&oldmm->corten_state);
	if (!old_state)
		return 0;

	mmap_assert_write_locked(oldmm);
	mmap_assert_write_locked(mm);

	/* The child registry is created eagerly so that fork_commit() can
	 * register the mirrored arenas into it; the child's mmap_write is
	 * held nested by dup_mmap(), which is the MODE writer contract
	 * for the cursor copy below (the window cursor must match, or the
	 * child's new mmaps would collide with the inherited arenas).
	 */
	{
		struct corten_mm_state *state = corten_arena_get_state(mm);

		if (!state)
			return -ENOMEM;
		state->next_va = READ_ONCE(old_state->next_va);
	}

	/* DEV-13: dup_mmap holds oldmm's mmap_write; nesting ctl_lock
	 * inside it is the legal order.
	 */
	mutex_lock(&old_state->ctl_lock);

	/* Freeze window (DEV-15): refuse new transactions, then drain
	 * the in-flight ones.  The frozen write and the drain pair per
	 * arena; a drained arena stays frozen until fork_commit()'s (or
	 * fork_abort()'s) unfreeze closure.  Only the child-state
	 * allocation above can fail, so nothing frozen is ever unwound
	 * inside begin itself.
	 */
	xa_for_each(&old_state->arenas, frame, arena) {
		if (frame < drained_until)
			continue;
		drained_until = arena->end >> PMD_SHIFT;

		WRITE_ONCE(arena->frozen, true);
		if (!corten_arena_drain(arena)) {
			/* Leaked transaction reference -- a kernel bug
			 * (E5).  Count it and proceed: the snapshot below
			 * is taken after the drain, so a straggler can
			 * only surface as this count, and the unfreeze
			 * closure's reinit WARN covers the leak loudly.
			 * The descriptor stays alive (a straggler put
			 * would otherwise UAF).
			 */
			corten_arena_note_drain_timeout(old_state);
		}
	}

	mutex_unlock(&old_state->ctl_lock);

	return 0;
}

/*
 * Parent-side 2M-window pass (M5_FORK_SPEC.md 1.3 ④-2): snapshot ONE
 * window's metadata and turn every CORTEN_MAPPED page in it into a
 * shared one (SHARED, plus the WRITABLE record corten_mark() requires
 * for logically writable pages).  Private-anon and dropped-content
 * slots are snapshot unchanged.  The snapshot is consumed by the child
 * replay of the SAME window right after (corten_arena_fork_mirror's
 * window loop) -- one buffer, one window at a time, and the parent's
 * and child's descriptor locks still never nest.  The pmd presence
 * gate skips windows with no PT page: no PT page means no metadata and
 * no content (INV4 same birth), which is what keeps the fork cost
 * proportional to the *touched* windows, not the reserved size.
 * Called frozen and drained: no transaction can race.
 */
static int
corten_arena_fork_mark_window(struct corten_arena *ar, unsigned long addr,
			      unsigned long win_end,
			      struct corten_pte_meta *snap)
{
	pmd_t *pmdp = corten_arena_pmd(ar->mm, addr);
	struct corten_txn txn;
	unsigned long a;
	int tries = 0;
	int ret;

	memset(snap, 0, CORTEN_META_ARRAY_BYTES);
	if (!pmdp || !pmd_present(READ_ONCE(*pmdp)) ||
	    pmd_leaf(READ_ONCE(*pmdp)))
		return 0;

	for (;;) {
		ret = corten_lock_range(ar->mm, addr, win_end - addr, &txn);
		if (ret != -EAGAIN || ++tries >= 2)
			break;
	}
	/* Untracked window (hole/defective install): nothing recorded --
	 * the shadow pieces still copy as plain VMAs.
	 */
	if (ret == -ENOENT || ret == -EOPNOTSUPP)
		return 0;
	if (ret)
		return ret;

	/* Snapshot first, then the SHARED marks: the snapshot is what the
	 * child replay of this window reads, under the child's own locks
	 * -- the parent's and the child's descriptor trees are never
	 * locked at the same time (④-4 discipline).  A mark failure must
	 * abort the fork (the caller's ⑤ unwind runs): replaying a
	 * snapshot whose parent-side marks are missing would hand the
	 * child a writable-looking page that is still hardware-shared.
	 */
	for (a = addr; a < win_end; a += PAGE_SIZE) {
		struct corten_pte_meta m, nm;

		ret = corten_query(&txn, a, &m);
		if (unlikely(ret))
			break;

		if (m.state != CORTEN_MAPPED) {
			snap[pte_index(a)] = m;
			continue;
		}
		/* The child mirrors the SHARED shape, so the snapshot
		 * records the post-mark state, not the pre-mark one.
		 */
		nm = m;
		nm.flags = m.flags | CORTEN_PF_SHARED;
		if (m.perm & CORTEN_PERM_WRITE)
			nm.flags |= CORTEN_PF_WRITABLE;
		snap[pte_index(a)] = nm;
		if (nm.flags == m.flags)
			continue;
		ret = corten_mark(&txn, a, PAGE_SIZE, &nm);
		if (unlikely(ret))
			break;
	}
	corten_unlock(&txn);
	return ret;
}

/*
 * Child arena registration (④-3): the declare_locked() construction
 * sequence minus the shadowize (the child's shadow pieces already carry
 * VM_CORTEN via vm_area_dup(), and their anon_vma is already forked).
 * The cached shadow-VMA mirrors the parent's cache exactly: the piece
 * the parent cached, when the child inherited it; NULL on a punch-hole
 * cache (the multi-piece degradation the fault paths already handle).
 * Registered under the child state's ctl_lock.  The caller holds the
 * parent registry's lock nowhere on this path: the two mutexes share
 * one lockdep class, so nesting them -- however singleton -- is not
 * expressible and is avoided (see fork_commit).
 */
static int corten_arena_fork_register_child(struct mm_struct *mm,
					    struct corten_mm_state *state,
					    struct corten_arena *ar,
					    struct vm_area_struct *cvma)
{
	unsigned long frame, first, last;
	struct corten_arena *child;
	int ret;

	child = kzalloc(sizeof(*child), GFP_KERNEL_ACCOUNT);
	if (!child)
		return -ENOMEM;

	child->start = ar->start;
	child->end = ar->end;
	child->mm = mm;
	child->prot = READ_ONCE(ar->prot);
	mutex_init(&child->fill_lock);
	init_completion(&child->drained);
	WRITE_ONCE(child->frozen, false);
	ret = percpu_ref_init(&child->active, corten_arena_active_release,
			      PERCPU_REF_INIT_ATOMIC, GFP_KERNEL);
	if (ret) {
		mutex_destroy(&child->fill_lock);
		kfree(child);
		return ret;
	}

	/* Publish the cached shadow-VMA before the arena becomes visible
	 * in the xarray below ([FAIL-2], the DECLARE order).
	 */
	WRITE_ONCE(child->vma, cvma);

	mutex_lock(&state->ctl_lock);

	first = ar->start >> PMD_SHIFT;
	last = (ar->end - 1) >> PMD_SHIFT;
	for (frame = first; frame <= last; frame++) {
		ret = xa_err(xa_store(&state->arenas, frame, child,
				      GFP_KERNEL));
		if (ret)
			goto out_unwind;
	}
	refcount_set(&state->nr, refcount_read(&state->nr) + 1);

	/* Last publishing step: the observability ledger (S8).  On the
	 * unwind below the arena was never published, so it must not be
	 * linked (the ledger only ever contains registered arenas).
	 */
	corten_arena_obs_add(child);

	mutex_unlock(&state->ctl_lock);

	return 0;

out_unwind:
	while (frame > first)
		xa_erase(&state->arenas, --frame);
	mutex_unlock(&state->ctl_lock);
	percpu_ref_exit(&child->active);
	mutex_destroy(&child->fill_lock);
	kfree(child);
	return ret;
}

/*
 * Child-side 2M-window pass (④-4): replay the parent snapshot into the
 * child's descriptors.  Runs after the parent window pass finished and
 * unlocked, so the two descriptor trees are never locked together.  The
 * child's PT pages mirror the parent's (copy_page_range() pte_alloc's
 * every present parent PT page), so the same pmd presence gate applies --
 * a VM_WIPEONFORK piece skipped the PTE copy and therefore shows up here
 * as an absent PT page: its windows stay unrecorded, the child keeps the
 * clean-slate contract.  The child cannot schedule inside dup_mmap(), so
 * no transaction can race this walk.
 *
 * Slot mapping:
 *   - CORTEN_MAPPED: corten_map() (Invalid->MAPPED, the only legal
 *     producer) + corten_mark() for the snapshot's flags (SHARED, plus
 *     the WRITABLE record).  The folio identity comes from the copied
 *     child PTE -- metadata-wise the protocol records no page identity
 *     (the PTE is the identity), but map() requires a live page and the
 *     copied one is the honest answer.
 *   - CORTEN_PRIVATE_ANON: mark() as-is (state to itself).
 *   - CORTEN_INVALID with a recorded perm (a dropped-content slot, the
 *     CORTEN_UNMAP_KEEP_PERM shape): the protocol cannot write an
 *     Invalid slot's perm, so it is re-expressed as PRIVATE_ANON with
 *     the same perm -- behaviourally identical for every consumer (the
 *     FRESH gate reads the perm either way, the mprotect route treats
 *     the two spellings alike).  DEVIATION from the spec letter
 *     ("INVALID pages skipped"), registered in the M5 report: skipping
 *     would lose the child's committed mprotect contract, the r06
 *     "rogue ACCERR" shape re-created on the child side.
 *   - CORTEN_INVALID without perm: never recorded, skipped.
 */
static int corten_arena_fork_copy_window(struct mm_struct *mm,
					 unsigned long addr,
					 unsigned long win_end,
					 struct corten_pte_meta *snap)
{
	pmd_t *pmdp = corten_arena_pmd(mm, addr);
	struct corten_txn txn;
	unsigned long a;
	int tries = 0;
	int ret;

	/* The child PT page mirrors the parent's (copy_page_range()
	 * pte_allocs one for every present parent PT page); a VM_WIPEONFORK
	 * piece skipped the PTE copy and therefore shows up here as an
	 * absent PT page: its windows stay unrecorded, the child keeps
	 * the clean-slate contract.  The child cannot schedule inside
	 * dup_mmap(), so no transaction can race this walk.
	 */
	if (!pmdp || !pmd_present(READ_ONCE(*pmdp)) ||
	    pmd_leaf(READ_ONCE(*pmdp)))
		return 0;

	for (;;) {
		ret = corten_lock_range(mm, addr, win_end - addr, &txn);
		if (ret != -EAGAIN || ++tries >= 2)
			break;
	}
	if (ret == -ENOENT || ret == -EOPNOTSUPP)
		return 0;
	if (ret)
		return ret;

	/* Ensure the metadata array before the PTE lock: the allocation
	 * is GFP_NOWAIT under the desc write lock and must not happen
	 * with the PTE lock already held (the map_anon() ordering).
	 */
	if (corten_meta_ensure_locked(txn.covering)) {
		corten_unlock(&txn);
		return -ENOMEM;
	}

	for (a = addr; a < win_end; a += PAGE_SIZE) {
		struct corten_pte_meta m = snap[pte_index(a)];
		struct corten_pte_meta nm;
		pte_t *ptep, cur;
		spinlock_t *ptl;	/* ptl nests below desc lock */
		struct page *page;

		if (m.state == CORTEN_INVALID && !m.perm)
			continue;

		ptep = pte_offset_map_lock(mm, pmdp, a, &ptl);
		if (!ptep) {
			corten_unlock(&txn);
			return -EAGAIN;
		}
		cur = ptep_get(ptep);
		pte_unmap_unlock(ptep, ptl);

		page = NULL;
		if (pte_present(cur) && !pte_special(cur))
			page = pte_page(cur);

		switch (m.state) {
		case CORTEN_MAPPED:
			if (WARN_ON_ONCE(!page))
				/* MAPPED without a translation: the
				 * restore-side INV7 drift; leave the child
				 * slot unrecorded (its fault warns the same
				 * way).
				 */
				continue;
			/* Invalid->MAPPED needs map(); mark() then replays
			 * the snapshot flags (SHARED, plus the WRITABLE
			 * record) on top of the state map() reset.
			 */
			ret = corten_map(&txn, a, page, m.perm, 0);
			nm = m;
			if (!ret)
				ret = corten_mark(&txn, a, PAGE_SIZE, &nm);
			break;
		case CORTEN_INVALID:
			/* Dropped-content slot: re-express as PRIVATE_ANON
			 * + perm (see the comment).
			 */
			nm.state = CORTEN_PRIVATE_ANON;
			nm.perm = m.perm;
			nm.flags = 0;
			memset(nm.__resv, 0, sizeof(nm.__resv));
			ret = corten_mark(&txn, a, PAGE_SIZE, &nm);
			break;
		default:
			nm = m;
			ret = corten_mark(&txn, a, PAGE_SIZE, &nm);
			break;
		}
		if (WARN_ON_ONCE(ret)) {
			corten_unlock(&txn);
			return ret;
		}
	}
	corten_unlock(&txn);

	return 0;
}

/*
 * The per-arena mirror body of fork_commit(): the child-side skip test
 * (④-1), the child arena registration (④-3) and then the per-window
 * loop of parent SHARED marks + snapshot (④-2) followed immediately by
 * the child metadata replay (④-4) of the same window.  The parent and
 * child transactions of one window are strictly sequential, so the two
 * descriptor trees are never locked together.  Returns 0 (including a
 * legitimate skip) or the first error; on error the caller aborts the
 * fork and the already-mirrored child arenas are left to the
 * MMF_UNSTABLE exit path (⑤).
 */
static int corten_arena_fork_mirror(struct mm_struct *mm,
				    struct corten_mm_state *state,
				    struct corten_arena *ar,
				    struct corten_pte_meta *snap)
{
	struct vm_area_struct *vma, *pvma, *cvma = NULL;
	unsigned long addr;
	bool any_piece = false;
	int ret;

	/* ④-1 child-side skip.  [F-B] Since the D-G'' punch route an arena
	 * can span several shadow pieces separated by legacy holes (a
	 * hole is an ordinary file/anon VMA and the fork loop copies it
	 * with the standard copy_page_range()); every inherited piece
	 * carries VM_CORTEN via vm_area_dup().  A VM_DONTCOPY piece is
	 * absent from the child altogether (the dup_mmap loop cleared
	 * it).  No surviving piece means the arena's only potential
	 * second mapper is gone: the arena stays parent-only, no SHARED
	 * marks are needed, and any residue self-heals through the COW
	 * reuse branch.  Counted as fork_skips.
	 */
	VMA_ITERATOR(vmi, mm, ar->start);
	for_each_vma_range(vmi, vma, ar->end) {
		if (vma->vm_start >= ar->end)
			break;
		if (vma->vm_flags & VM_CORTEN) {
			any_piece = true;
			break;
		}
	}
	if (!any_piece) {
		atomic_long_inc(&corten_nr_fork_skips);
		return 0;
	}

	/* Mirror the parent's cached shadow-VMA: the same piece, when the
	 * child inherited it (its start is the identity); NULL otherwise,
	 * the established safe degradation (fault paths fall back, the
	 * F-A tier-2 walk still finds the surviving pieces).
	 */
	pvma = READ_ONCE(ar->vma);
	if (pvma) {
		cvma = vma_lookup(mm, pvma->vm_start);
		if (!cvma || !(cvma->vm_flags & VM_CORTEN))
			cvma = NULL;
	}

	/* ④-3 child arena object + registry frames.  Registration before
	 * the window loop: a mid-window failure leaves a registered child
	 * whose metadata replay is partial -- acceptable, the child mm is
	 * discarded (MMF_UNSTABLE) and its arenas drained by
	 * corten_arena_mm_exit(); the parent's SHARED bits stay as
	 * residue that the COW reuse branch clears on first write.
	 */
	ret = corten_arena_fork_register_child(mm, state, ar, cvma);
	if (ret)
		return ret;

	/* ④-2 + ④-4 per window: parent snapshot + SHARED marks, then the
	 * child replay of the same window.
	 */
	for (addr = ar->start; addr < ar->end;
	     addr = min((addr | (PMD_SIZE - 1)) + 1, ar->end)) {
		unsigned long win_end = min((addr | (PMD_SIZE - 1)) + 1,
					    ar->end);

		ret = corten_arena_fork_mark_window(ar, addr, win_end, snap);
		if (ret)
			return ret;

		ret = corten_arena_fork_copy_window(mm, addr, win_end, snap);
		if (ret)
			return ret;
	}

	return 0;
}

int corten_arena_fork_commit(struct mm_struct *mm, struct mm_struct *oldmm)
{
	struct corten_mm_state *old_state, *state;
	struct corten_pte_meta *snap;
	unsigned long frame = 0, drained_until = 0;
	struct corten_arena *arena;
	bool did_arena = false;
	int ret = 0;

	/* Pairs with the store in corten_arena_state_create(). */
	old_state = smp_load_acquire(&oldmm->corten_state);
	if (!old_state)
		return 0;
	/* Pairs with the store in corten_arena_state_create(). */
	state = smp_load_acquire(&mm->corten_state);
	if (!state)			/* fork_begin created it */
		return 0;

	mmap_assert_write_locked(oldmm);
	/* dup_mmap() holds @mm's write lock nested (mmap.c): the child
	 * registry stores and the child-side VMA walk below rely on it,
	 * exactly like the for_each_vma() loop that just ran.
	 */
	mmap_assert_write_locked(mm);

	/* R-G: the dup_mmap loop aborts on a fatal signal before reaching
	 * us (fork_abort() unwinds there); a signal arriving between that
	 * checkpoint and now is checked here and per arena below.
	 */
	if (fatal_signal_pending(current))
		ret = -EINTR;

	/* KUnit injection (R-A): fail before any arena work -- the unwind
	 * below must still release the freeze, and the child (whose
	 * registry fork_begin created) keeps no arena.
	 */
	if (corten_fork_fail_stage == 1)
		ret = -ENOMEM;

	snap = kmalloc(CORTEN_META_ARRAY_BYTES, GFP_KERNEL_ACCOUNT);
	if (!snap)
		ret = -ENOMEM;

	/* The mirror loop runs WITHOUT the parent registry's ctl_lock:
	 * register_child() nests the CHILD state's ctl_lock inside it and
	 * the two mutexes share one lockdep class -- the nesting is
	 * singleton (nowhere else do two ctl_locks meet) but not
	 * expressible to lockdep.  It is also simply not needed: no
	 * registry writer can run concurrently (every one of them holds
	 * oldmm's mmap_write), and the frozen-bit accounting below is
	 * re-taken under the lock.
	 */
	xa_for_each(&old_state->arenas, frame, arena) {
		if (frame < drained_until)
			continue;
		drained_until = arena->end >> PMD_SHIFT;

		if (ret)
			break;
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}

		ret = corten_arena_fork_mirror(mm, state, arena, snap);
		if (ret)
			break;
		did_arena = true;

		/* KUnit injection (R-A): fail after the first arena was
		 * fully mirrored, to exercise the unwind below plus the
		 * MMF_UNSTABLE child-teardown path.
		 */
		if (corten_fork_fail_stage == 2) {
			ret = -ENOMEM;
			break;
		}
	}

	/* ⑤ The red line (R-A): whatever happened above, the parent is
	 * unfrozen before the error (or success) is reported.  SHARED-bit
	 * residue needs no rollback: the failed child is discarded, its
	 * copies never happen, every affected folio ends up mapcount==1
	 * and the COW reuse branch clears the bits on first write.
	 */
	mutex_lock(&old_state->ctl_lock);
	corten_arena_fork_unfreeze_locked(old_state);
	mutex_unlock(&old_state->ctl_lock);

	kfree(snap);

	if (!ret && did_arena)
		atomic_long_inc(&corten_nr_fork_faithful);

	return ret;
}

/* ------------------------------------------------------------------ *
 * S4/S5: fault path (M3B_DESIGN.md sec 4)
 * ------------------------------------------------------------------
 */

static inline void corten_arena_fault_stat(struct corten_mm_state *state,
					   enum corten_arena_stat which)
{
	if (state)
		this_cpu_inc(state->stats[which]);
}

/*
 * Look up and pin the arena covering @addr.  A failed tryget_live() means
 * the arena is being RELEASEd/destroyed: the caller must run the legacy
 * path (which remains correct for as long as the shadow-VMA exists).
 * Non-static: mm/corten_fault_test.c drives it directly.
 */
struct corten_arena *corten_arena_lookup_get(struct mm_struct *mm,
					     unsigned long addr)
{
	struct corten_arena *ar;

	rcu_read_lock();
	ar = corten_arena_lookup(mm, addr);
	/* M5 fork freeze window (DEV-15): a frozen arena refuses new
	 * transactions, so the fault (or the space operation behind this
	 * lookup) falls back to the legacy funnel -- which needs
	 * mmap_lock and therefore blocks against dup_mmap()'s write lock.
	 * That is what makes the freeze window a static snapshot: no new
	 * transaction can appear while the fork mirrors the metadata.
	 */
	if (ar && READ_ONCE(ar->frozen))
		ar = NULL;
	if (ar && !percpu_ref_tryget_live(&ar->active))
		ar = NULL;
	rcu_read_unlock();

	return ar;
}

/* Test-only accessor to the per-mm registry (mm/corten_fault_test.c). */
struct corten_mm_state *corten_arena_state(struct mm_struct *mm)
{
	return READ_ONCE(mm->corten_state);
}

/*
 * The arena's shadow-VMA, from the descriptor's cache ([FAIL-2]: the
 * previous version ran vma_lookup() -- a maple-tree walk -- on the fault
 * hot path, which violates the M3 DoD "zero find_vma / zero maple walk").
 *
 * Cache lifecycle: set at DECLARE under mmap_write_lock before the arena
 * is published in the xarray; cleared at RELEASE/DECLARE-unwind under
 * mmap_write_lock.  Callers run either inside a transaction (the active
 * reference they hold is a drain barrier: RELEASE must wait for them
 * before it is allowed to take that lock, sec 6.3) or otherwise while
 * holding an active reference, so the pointer cannot be freed or
 * flag-changed for the whole lifetime of the reference and a NULL read
 * is impossible for a pinned arena.
 */
static inline struct vm_area_struct *corten_arena_shadow_vma(struct corten_arena *ar)
{
	return READ_ONCE(ar->vma);
}

/*
 * [F-A, D-G''] Tier 1 of the fault-side ownership self-check: the cached
 * shadow-VMA bounds, with zero tree walk (the common case -- every fault of
 * an unpunched arena lands here).
 */
static inline bool corten_arena_vma_spans(const struct vm_area_struct *vma,
					  unsigned long addr)
{
	return vma && addr >= vma->vm_start && addr < vma->vm_end;
}

/*
 * [F-A] The ownership decision, pure and testable (the D-G'' regression
 * table): may the arena serve a fault at @addr given the cached
 * shadow-VMA and the VMA that actually covers the address (NULL if the
 * range is unmapped, e.g. torn down by a concurrent RELEASE)?
 *   - inside the cached shadow-VMA: ours, zero walk;
 *   - outside it but covered by another VM_CORTEN piece (the tail a
 *     punch split off): still ours -- routed mprotect metadata must keep
 *     applying;
 *   - covered by anything else (the file mapping a punch installed) or
 *     by nothing: not ours.
 */
bool corten_arena_fault_covered(const struct vm_area_struct *cached,
				const struct vm_area_struct *covering,
				unsigned long addr)
{
	if (corten_arena_vma_spans(cached, addr))
		return true;

	return covering && !!(covering->vm_flags & VM_CORTEN);
}

/*
 * [F-A] Is @addr still arena property?  The frame table is address-keyed
 * (corten_arena_lookup), so a legacy punch that carves a hole out of the
 * arena leaves the frames claiming a range whose covering VMA is no longer
 * ours -- serving that fault from the arena would synthesize a fresh
 * anonymous page on top of a file mapping (r05 dg2-analysis.md D2: the JVM
 * CDS map_archive() SIGSEGV; even with the FRESH gate opened the result is
 * silent zero-page corruption, which is worse).
 *
 * Tier 1 is the cached-pointer bounds test.  Only when the address falls
 * outside the cached shadow-VMA (a punch split the arena, or a concurrent
 * RELEASE is between unshadow() and its munmap) does tier 2 walk the tree:
 * an RCU find_vma_intersection() plus the VM_CORTEN bit test.  The walk
 * under plain rcu_read_lock() is the lock_vma_under_rcu() read pattern: the
 * vm_area_cachep is SLAB_TYPESAFE_BY_RCU, so the object stays valid for the
 * whole grace period, and a stale answer is bounded in both directions --
 * a false "not owned" hands a fault to the legacy funnel (today's fallback
 * behaviour, always safe), a false "owned" is the pre-fix behaviour.
 *
 * The slow hook (corten_arena_handle_mm_fault) needs no counterpart: its
 * caller in mm/memory.c is vma-keyed (diverts only on VM_CORTEN), so a
 * punched-out hole never reaches it by construction.
 *
 * Non-static: mm/corten_fault_test.c drives it (D-G'' regression anchor).
 */
bool corten_arena_fault_owned(struct corten_arena *ar, struct mm_struct *mm,
			      unsigned long addr)
{
	struct vm_area_struct *vma;
	bool owned;

	if (corten_arena_vma_spans(corten_arena_shadow_vma(ar), addr))
		return true;

	/* Tier 2: rare -- only punched or tearing-down arenas get here. */
	rcu_read_lock();
	vma = find_vma_intersection(mm, addr, addr + 1);
	owned = corten_arena_fault_covered(NULL, vma, addr);
	rcu_read_unlock();

	return owned;
}

/*
 * [F-B seal] Transaction-granularity counterpart of the hot-hook check:
 * the frame table must still point at @ar when a transaction commits,
 * otherwise a concurrent punch (which erases frames before its zap) would
 * race a fresh anonymous install under the incoming legacy VMA -- silent
 * corruption of one page.  The window's covering desc write lock held by
 * the caller is what makes the answer sticky: the punch's zap serializes
 * behind the same lock, so a commit that passed this check is zapped by
 * the punch rather than surviving it.
 */
static bool corten_arena_txn_owned(struct corten_arena *ar,
				   struct mm_struct *mm, unsigned long addr)
{
	/* Pairs with the smp_store_release() publisher in
	 * corten_arena_state_create(): the state pointer is published
	 * once and never replaced while the mm is alive.
	 */
	struct corten_mm_state *state = smp_load_acquire(&mm->corten_state);
	struct corten_arena *frame;
	bool owned = false;

	if (!state)
		return false;

	rcu_read_lock();
	frame = xa_load(&state->arenas, addr >> PMD_SHIFT);
	owned = frame == ar;
	rcu_read_unlock();

	return owned;
}

/*
 * Speculative allocation (M3B_DESIGN.md sec 4.3 [P1-1]/[P1-2]): order-0
 * zeroed folio plus memcg charge and swap-rate throttling, all before the
 * covering write lock is taken so that the lock hold section never
 * allocates and never sleeps (the include/linux/corten.h "write-lock
 * holders do GFP_NOWAIT only" red line).  Mirrors folio_prealloc() in
 * mm/memory.c:1190 with need_zero forced (a fresh anonymous page must not
 * leak stale kernel memory).
 */
static struct folio *corten_arena_folio_prealloc(struct mm_struct *mm,
						 struct vm_area_struct *vma,
						 unsigned long addr)
{
	struct folio *folio;

	folio = vma_alloc_zeroed_movable_folio(vma, addr);
	if (!folio)
		return NULL;

	if (mem_cgroup_charge(folio, mm, GFP_KERNEL)) {
		folio_put(folio);
		return NULL;
	}

	folio_throttle_swaprate(folio, GFP_KERNEL);

	return folio;
}

/* x86 access_error() semantics against the metadata permission bits. */
static bool corten_arena_perm_ok(const struct corten_pte_meta *m,
				 bool write, bool instruction)
{
	if (write)
		return m->perm & CORTEN_PERM_WRITE;
	if (instruction)
		return m->perm & CORTEN_PERM_EXEC;

	/* Data reads are allowed on execute-only VMAs (same as
	 * arch/x86 access_error()).
	 */
	return m->perm & (CORTEN_PERM_READ | CORTEN_PERM_EXEC);
}

/*
 * The dispatch state machine (sec 4.3 switch table).  Pure: driven by
 * mm/corten_fault_test.c without an mm; the real fault paths add the
 * side effects around it.
 */
enum corten_disp corten_arena_dispatch(const struct corten_pte_meta *m,
				       bool write, bool instruction)
{
	switch (m->state) {
	case CORTEN_PRIVATE_ANON:
		if (!corten_arena_perm_ok(m, write, instruction))
			return CORTEN_DISP_ACCERR;
		return write ? CORTEN_DISP_MAP_ANON : CORTEN_DISP_ZERO_PAGE;
	case CORTEN_MAPPED:
		/* M5 (M5_FORK_SPEC.md sec 3.1): a SHARED page is never
		 * restored writable outside the COW transaction -- the
		 * RESTORE bypass would let one side write through the
		 * fork's wrprotect and break the parent/child isolation.
		 * A write fault on a shared page is either the wrprotect
		 * artifact (contract writable -> COW_MAYBE) or a genuine
		 * permission fault (contract read-only -> COW_COPY).
		 * corten_mark()'s WRITABLE rule guarantees the WRITABLE
		 * flag is set whenever a shared page's perm carries
		 * WRITE, so the two shapes are disjoint.
		 */
		if (!corten_arena_perm_ok(m, write, instruction)) {
			if (m->flags & CORTEN_PF_SHARED)
				return CORTEN_DISP_COW_COPY;
			return CORTEN_DISP_ACCERR;
		}
		if (write && (m->flags & CORTEN_PF_SHARED))
			return CORTEN_DISP_COW_MAYBE;
		return CORTEN_DISP_RESTORE;
	case CORTEN_SWAPPED:			/* M6 producer: unreachable */
	case CORTEN_FILE_MAPPED:		/* M4+ producers: unreachable */
	case CORTEN_SHARED_ANON:
		/* Deliberately silent here: this is a pure classifier and
		 * the dispatch-table KUnit case drives it with every state
		 * value.  The loud WARN_ONCE lives at the STUB caller in
		 * corten_arena_fault_once().
		 */
		return CORTEN_DISP_STUB;
	case CORTEN_INVALID:
		/* An unmarked page inside a declared arena is the paper's
		 * "virtual allocation on first access" (Fig.8 L26-38), not
		 * a mapping error -- faults outside any arena never reach
		 * this classifier (the xarray lookup gates them).  The
		 * caller synthesizes the PrivateAnon allocation and
		 * re-dispatches: the permission gate must run against the
		 * arena's prot, which this pure function cannot see (the
		 * zeroed metadata carries no permission bits).
		 */
		return CORTEN_DISP_FRESH;
	default:
		return CORTEN_DISP_MAPERR;
	}
}

/*
 * Walk to the PMD entry of @addr gated top-down: pud_offset() and
 * pmd_offset() turn a non-present upper ENTRY into a garbage pointer, so
 * every level's presence is checked before the next is computed (this
 * was the first KUnit run's GPF: DECLARE's emptiness check walked a
 * fresh region whose PGD entry was still zero).  Callers treat NULL as
 * "nothing mapped below" or retry through fill_upper().
 */
static pmd_t *corten_arena_pmd(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgdp = pgd_offset(mm, addr);
	p4d_t *p4dp;
	pud_t *pudp;

	if (!pgd_present(READ_ONCE(*pgdp)))
		return NULL;
	p4dp = p4d_offset(pgdp, addr);
	if (!p4d_present(READ_ONCE(*p4dp)))
		return NULL;
	pudp = pud_offset(p4dp, addr);
	if (!pud_present(READ_ONCE(*pudp)))
		return NULL;

	return pmd_offset(pudp, addr);
}

/*
 * S4 (sec 4.4): ensure the upper page tables down to the tracked PTE-level
 * page exist for @addr, without mmap_lock.  Serialised per arena by
 * fill_lock (a cold-path mutex, never held with any descriptor lock); the
 * pX_alloc() primitives themselves are race-safe against concurrent fills
 * (install under the level spinlock with a re-check), and legacy writers
 * cannot reach an arena window because both fault hooks divert faults and
 * the S6 hooks reject/route every space operation.
 */
int corten_arena_fill_upper(struct corten_arena *ar, unsigned long addr)
{
	struct mm_struct *mm = ar->mm;
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	int ret = -ENOMEM;

	mutex_lock(&ar->fill_lock);

	pgdp = pgd_offset(mm, addr);

	/*
	 * Each level's pointer comes from the pX_alloc() return value,
	 * never from an offset computed before the parent entry is
	 * populated: unlike pgd_offset()/p4d_offset() (pure arithmetic on
	 * the top-level page), pud_offset() and pmd_offset() dereference
	 * the parent entry (p4d_pgtable() and pud_pgtable()) to find the
	 * next table.
	 * On a fresh mm the pgd entry is still zero, so a pre-computed
	 * pud_offset() yields __va(0) + index -- a bogus direct-map
	 * pointer into the first physical page, whose stale content (a
	 * set present bit makes pud_none() false) turns pmd_alloc() into
	 * a no-op and pmd_offset() into a non-canonical address.  That is
	 * exactly the "stack segment" (#SS) oops the first corten=on
	 * KUnit run of this suite hit on the pmd_leaf() read.
	 *
	 * p4d_alloc()/pud_alloc()/pmd_alloc() populate the parent entry
	 * and return the level pointer (NULL on failure), so chaining the
	 * return values keeps every dereference on a populated entry.
	 * With 4-level paging the p4d folds into the pgd (p4d_alloc() is
	 * a no-op and pud_alloc() installs the pud table into the pgd
	 * entry); with 5-level the same code allocates the real p4d
	 * table.  pte_alloc() returns nonzero on failure.
	 *
	 * GFP_KERNEL is legal here: no descriptor lock is held, only the
	 * cold-path fill_lock (R4 note in sec 8).  The freshly installed
	 * PTE-level page gains its descriptor in pte_alloc_one() (M2a
	 * hook), so the subsequent corten_lock_range() finds it.
	 */
	p4dp = p4d_alloc(mm, pgdp, addr);
	if (!p4dp)
		goto out;
	pudp = pud_alloc(mm, p4dp, addr);
	if (!pudp)
		goto out;
	if (unlikely(pud_leaf(READ_ONCE(*pudp)))) {
		/* 1G leaf: cannot happen in an arena (anonymous private
		 * only, VM_NOHUGEPAGE); fall back to legacy.
		 */
		ret = -EOPNOTSUPP;
		goto out;
	}
	pmdp = pmd_alloc(mm, pudp, addr);
	if (!pmdp)
		goto out;
	if (unlikely(pmd_leaf(READ_ONCE(*pmdp)))) {
		/* THP leaf: legacy fallback (defensive; VM_NOHUGEPAGE
		 * excludes it for real arenas).
		 */
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (pte_alloc(mm, pmdp))
		goto out;

	/* Root fix for the untracked-window drift (r03 defect C): the M2a
	 * descriptor install inside pte_alloc_one() can fail (GFP_NOWAIT),
	 * and it never retried -- the window stayed untracked forever, its
	 * faults fell back to the legacy body, and legacy-written PTEs had
	 * no metadata behind them.  Every fill now re-arms an untracked PT
	 * page (idempotent: a tracked page is left untouched, so the
	 * fresh-install WARN_ON(old) contract cannot fire), which makes
	 * the window transactional again at the next touch.
	 */
	corten_ptdesc_rearm(mm, pmd_pgtable(*pmdp));

	corten_arena_fault_stat(READ_ONCE(ar->mm->corten_state),
				CORTEN_ARENA_STAT_FILLS);
	ret = 0;
out:
	mutex_unlock(&ar->fill_lock);
	return ret;
}

/*
 * Replicates mm_account_fault() (mm/memory.c:6409) for faults the arena
 * resolves outside handle_mm_fault() ([P2-8] counting parity, not a
 * documented deviation): PGFAULT counts successful and failed faults,
 * min_flt/perf only successful ones.  Arena faults never page in from
 * storage, so there is no major-fault path; FAULT_FLAG_TRIED-major is
 * therefore not reproduced (the arena retry loop is internal).
 */
static void corten_arena_account_fault(struct mm_struct *mm,
				       struct pt_regs *regs,
				       unsigned long address, bool success)
{
	count_vm_event(PGFAULT);
	count_memcg_event_mm(mm, PGFAULT);
	if (!success)
		return;

	current->min_flt++;
	if (regs)
		perf_sw_event(PERF_COUNT_SW_PAGE_FAULTS_MIN, 1, regs, address);
}

/* Internal status of one transaction attempt. */
enum corten_fault_status {
	CORTEN_F_HANDLED = 0,
	CORTEN_F_RETRY,		/* race lost: caller may retry (Fig.7) */
	CORTEN_F_ACCERR,	/* SEGV_ACCERR */
	CORTEN_F_MAPERR,	/* SEGV_MAPERR */
	CORTEN_F_OOM,
	CORTEN_F_FALLBACK,	/* legacy must run (PT page untracked, ...) */
};

struct corten_fault_ctx {
	struct mm_struct	*mm;
	struct vm_area_struct	*vma;	/* shadow-VMA if already known */
	struct corten_arena	*ar;
	struct folio		*folio;	/* speculative alloc, NULL if none */
	unsigned long		addr;	/* page-aligned fault address */
	bool			write;
	bool			instruction;
	struct pt_regs		*regs;
};

/*
 * The shadow-VMA of the faulting address, looked up on first use.  Only
 * cold branches need it (every hot install path got it with the
 * speculative allocation); see corten_arena_shadow_vma() for the
 * stability argument.
 */
static struct vm_area_struct *corten_arena_get_vma(struct corten_fault_ctx *ctx)
{
	if (!ctx->vma)
		ctx->vma = corten_arena_shadow_vma(ctx->ar);

	return ctx->vma;
}

/*
 * S5 (sec 4.6): install a fresh anonymous page for a write fault (or a
 * zero-page-forbidden read fault).  Line-by-line mirror of
 * do_anonymous_page() (mm/memory.c:5166-5279) with the locking context
 * replaced by the arena transaction: the covering desc write lock is held
 * for the whole function, so the PTE store and the corten_map() metadata
 * transition become visible to other transactions atomically.
 *
 * @folio is the speculative allocation of the caller ([P1-1]); on success
 * its single reference becomes the PTE reference (order-0, so the
 * legacy folio_ref_add(folio, nr_pages - 1) is +0).  On failure the
 * reference is dropped by the caller's epilogue.
 *
 * Return: 0, -EAGAIN (race lost / transient; caller retries per Fig.7),
 * -ENOMEM, -EFAULT (inconsistent metadata, WARNed).
 */
static int corten_arena_map_anon(struct corten_fault_ctx *ctx,
				 struct corten_txn *txn,
				 const struct corten_pte_meta *m,
				 struct folio *folio)
{
	struct mm_struct *mm = ctx->mm;
	struct vm_area_struct *vma;
	struct folio *arena_folio = folio;
	pmd_t *pmdp;
	pte_t *ptep;
	pte_t entry, cur;
	spinlock_t *ptl;
	int ret;

	vma = corten_arena_get_vma(ctx);
	if (!vma)
		return -EFAULT;

	/* S5 assertion: THP/mTHP/khugepaged are excluded by the shadow
	 * VM_NOHUGEPAGE flag (sec 4.7); a set bit would mean the
	 * exclusion is broken and the covering-lock protocol is in
	 * danger.
	 */
	if (WARN_ON_ONCE(vma->vm_flags & (VM_HUGEPAGE | VM_HUGETLB)))
		return -EFAULT;

	/* Ensure the metadata array exists before touching the PTE: the
	 * allocation is GFP_NOWAIT under the desc write lock, and doing
	 * it first means the corten_map() below cannot fail after the
	 * PTE is already installed (no torn PTE/metadata pair).
	 */
	ret = corten_meta_ensure_locked(txn->covering);
	if (ret)
		return ret == -ENOMEM ? -ENOMEM : -EFAULT;

	/* ① pre-build the entry (memory.c:5229-5235, lockless).  The
	 * barrier in __folio_mark_uptodate() makes the zeroed page
	 * contents visible before the set_ptes() store.
	 */
	__folio_mark_uptodate(arena_folio);
	/* T0b: the encoding follows the *recorded* perm, not the
	 * shadow-VMA flags (corten_arena_perm_pgprot()).
	 */
	entry = folio_mk_pte(arena_folio,
			     corten_arena_perm_pgprot(vma, m->perm));
	entry = pte_sw_mkyoung(entry);
	if (ctx->write)
		entry = pte_mkwrite(pte_mkdirty(entry), vma);

	/* ② PTE install (memory.c:5237-5246).  The mutual exclusion point
	 * is desc->lock (write) plus the PTE lock (sec 6.1 rule R2).
	 */
	pmdp = corten_arena_pmd(mm, ctx->addr);
	if (!pmdp)
		return -EAGAIN;
	ptep = pte_offset_map_lock(mm, pmdp, ctx->addr, &ptl);
	if (!ptep)
		return -EAGAIN;		/* transient; retry re-walks */

	cur = ptep_get(ptep);
	if (!pte_none(cur)) {
		/*
		 * The PTE is not empty.  If it is the shared zero page
		 * (a concurrent read fault installed it after our
		 * metadata said PRIVATE_ANON), this write fault upgrades
		 * it: clear + flush under the PTE lock -- the read-only
		 * zero translation may be cached -- then install the real
		 * page (the break-before-make ordering of wp_page_copy;
		 * a concurrent reader cannot re-install in the window
		 * because it needs the desc write lock we hold).  Any
		 * other content is a genuine race loser
		 * (vmf_pte_changed equivalent): retry re-dispatches
		 * through the state machine and finds CORTEN_MAPPED.
		 */
		if (!pte_present(cur) || !pte_special(cur) ||
		    pte_pfn(cur) != my_zero_pfn(ctx->addr)) {
			pte_unmap_unlock(ptep, ptl);
			return -EAGAIN;
		}
		ptep_clear_flush(vma, ctx->addr, ptep);
	}

	/* ③ mm stability check + accounting (memory.c:5248/5259-5263).
	 * Fails during coredump/unstable periods: retry then fall out.
	 */
	if (unlikely(check_stable_address_space(mm))) {
		pte_unmap_unlock(ptep, ptl);
		return -EAGAIN;
	}

	/* order-0: the single alloc reference becomes the PTE reference;
	 * no folio_ref_add() (legacy nr_pages - 1 == 0).
	 */
	add_mm_counter(mm, MM_ANONPAGES, 1);
	folio_add_new_anon_rmap(arena_folio, vma, ctx->addr, RMAP_EXCLUSIVE);
	/* M3 difference vs do_anonymous_page(): no folio_add_lru_vma() --
	 * arena pages stay off the LRU so reclaim/migration can never
	 * write to them outside a transaction (sec 4.6, matrix 5.17).
	 */

	set_ptes(mm, ctx->addr, ptep, entry, 1);
	/* No TLB invalidate needed (memory.c:5269 argument): the PTE was
	 * pte_none() above, so no CPU can hold a stale translation.
	 */

	update_mmu_cache_range(NULL, vma, ctx->addr, ptep, 1);
	pte_unmap_unlock(ptep, ptl);

	/* ④ metadata transition in the same transaction: the window
	 * between the PTE store and corten_map() is invisible to every
	 * other transaction (they serialise on the covering desc write
	 * lock).  Crash-consistency: a kernel crash in that window loses
	 * volatile state only (TLB/page contents); on the next boot the
	 * metadata (already PRIVATE_ANON from the mmap mark) and the PTE
	 * both start absent, so no torn pair survives.  Cannot fail:
	 * the metadata array was ensured above.
	 */
	ret = corten_map(txn, ctx->addr, folio_page(arena_folio, 0), m->perm,
			 0);
	if (WARN_ON_ONCE(ret))
		return -EFAULT;

	corten_arena_fault_stat(READ_ONCE(ctx->mm->corten_state),
				CORTEN_ARENA_STAT_MAPPED);

	return 0;
}

/*
 * S5: read fault on CORTEN_PRIVATE_ANON -- install the shared zero page
 * (mirror of memory.c:5186-5207).  No rmap, no MM_ANONPAGES accounting,
 * metadata stays PRIVATE_ANON.  A later write fault re-dispatches through
 * CORTEN_DISP_MAP_ANON, whose pte_none() re-check will find the zero-page
 * PTE and take the replace path in corten_arena_map_anon().
 */
static int corten_arena_zero_page(struct corten_fault_ctx *ctx,
				  struct corten_txn *txn,
				  const struct corten_pte_meta *m)
{
	struct mm_struct *mm = ctx->mm;
	struct vm_area_struct *vma;
	pmd_t *pmdp;
	pte_t *ptep;
	pte_t entry;
	spinlock_t *ptl;
	int ret = 0;

	vma = corten_arena_get_vma(ctx);
	if (!vma)
		return -EFAULT;

	entry = pte_mkspecial(pfn_pte(my_zero_pfn(ctx->addr),
				      corten_arena_perm_pgprot(vma,
							       m->perm)));

	pmdp = corten_arena_pmd(mm, ctx->addr);
	if (!pmdp)
		return -EAGAIN;
	ptep = pte_offset_map_lock(mm, pmdp, ctx->addr, &ptl);
	if (!ptep)
		return -EAGAIN;

	if (unlikely(!pte_none(ptep_get(ptep)))) {
		pte_unmap_unlock(ptep, ptl);
		return -EAGAIN;		/* raced winner; re-dispatch */
	}
	if (unlikely(check_stable_address_space(mm))) {
		pte_unmap_unlock(ptep, ptl);
		return -EAGAIN;
	}

	set_ptes(mm, ctx->addr, ptep, entry, 1);
	update_mmu_cache_range(NULL, vma, ctx->addr, ptep, 1);
	pte_unmap_unlock(ptep, ptl);

	corten_arena_fault_stat(READ_ONCE(ctx->mm->corten_state),
				CORTEN_ARENA_STAT_ZERO_PAGES);

	return ret;
}

/*
 * S5 (sec 4.3 CORTEN_MAPPED / sec 4.7 tail): the metadata says the page
 * is mapped but the PTE's permissions deny the access -- the NUMA
 * balancing protnone case (change_prot_numa() is an unhooked, accepted
 * legacy writer; the arena self-heals by rebuilding the PTE from the
 * metadata).  Nothing is allocated; the folio is the one the PTE points
 * at already.
 */
static int corten_arena_restore_pte(struct corten_fault_ctx *ctx,
				    struct corten_txn *txn,
				    const struct corten_pte_meta *m)
{
	struct mm_struct *mm = ctx->mm;
	struct vm_area_struct *vma;
	pmd_t *pmdp;
	pte_t *ptep;
	pte_t cur, entry;
	spinlock_t *ptl;

	vma = corten_arena_get_vma(ctx);
	if (!vma)
		return -EFAULT;

	pmdp = corten_arena_pmd(mm, ctx->addr);
	if (!pmdp)
		return -EAGAIN;
	ptep = pte_offset_map_lock(mm, pmdp, ctx->addr, &ptl);
	if (!ptep)
		return -EAGAIN;

	cur = ptep_get(ptep);
	if (unlikely(!pte_present(cur))) {
		/* CORTEN_MAPPED without a PTE: no M3 path produces this
		 * (all zaps go through transactions).
		 */
		pte_unmap_unlock(ptep, ptl);
		WARN_ON_ONCE(1);
		return -EFAULT;
	}

	/* T0b: rebuild with the recorded perm's encoding (the metadata is
	 * the source of truth), not the shadow-VMA flags.
	 */
	entry = mk_pte(pfn_to_page(pte_pfn(cur)),
		       corten_arena_perm_pgprot(vma, m->perm));
	entry = pte_mkyoung(entry);
	if (ctx->write)
		entry = pte_mkwrite(pte_mkdirty(entry), vma);

	/* M5: a SHARED page must never regain its write bit outside the
	 * COW transaction.  Reaching here with SHARED set means a
	 * read/instruction fault (the dispatch routes write faults on
	 * shared pages to the COW transaction), so keep the read-only
	 * shape the fork's wrprotect left -- this is the INV7 exemption
	 * "a shared page may sit read-only while its perm carries
	 * WRITE" (M5_FORK_SPEC.md sec 8).  Without this guard the
	 * restore bypass would break the parent/child isolation.
	 */
	if (m->flags & CORTEN_PF_SHARED)
		entry = pte_wrprotect(entry);

	/* ptep_set_access_flags() flushes only when something actually
	 * changed and the old translation could be cached (the protnone
	 * case); a pure no-op rebuild stays flush-free.
	 */
	ptep_set_access_flags(vma, ctx->addr, ptep, entry, ctx->write);
	update_mmu_cache_range(NULL, vma, ctx->addr, ptep, 1);
	pte_unmap_unlock(ptep, ptl);

	corten_arena_fault_stat(READ_ONCE(ctx->mm->corten_state),
				CORTEN_ARENA_STAT_RESTORES);

	return 0;
}

/*
 * M5.T2 core carried by T1a (M5_FORK_SPEC.md sec 3.2/3.4): the write-fault
 * transaction on a CORTEN_MAPPED + SHARED page whose contract is writable.
 * Entered from fault_once with the covering desc write lock held.  A write
 * fault on a logically-writable shared page can only be the fork's
 * wrprotect artifact (the dispatch guarantees perm W, and corten_mark()'s
 * rule guarantees the WRITABLE record); the paper's two answers (Sec. 4.3):
 *
 *   map_count == 1: the last mapper writes first -- clear SHARED in the
 *   same transaction and re-arm the write bit on the live PTE (the
 *   restore_pte skeleton plus the meta-bit clear).  No copy.
 *
 *   map_count > 1: copy the folio into the fault path's speculative
 *   allocation, break the old read-only translation (it may be cached),
 *   rebuild rmap/accounting around the private copy and re-map() the
 *   metadata (MAPPED->MAPPED needs FORCE -- the COW copy-in hole the
 *   protocol reserved).
 *
 * R-B discipline (sec 6): folio_mapcount() is read under the PTE lock.
 * Every other mapper's PTE removal clears the PTE under ITS ptl before
 * decrementing the mapcount (the folio_remove_rmap_pte order), so a count
 * observed under our ptl is >= the true number of live mappers: the race
 * direction is bounded to "copy where a reuse would also have been safe"
 * (one wasted page).  An under-count -- two processes handed one writable
 * page, the unforgivable cross-process corruption -- is impossible, which
 * is why the branch is == 1 and not <= 1 with a relaxed read.
 */
static int corten_arena_cow_write(struct corten_fault_ctx *ctx,
				  struct corten_txn *txn,
				  const struct corten_pte_meta *m)
{
	struct mm_struct *mm = ctx->mm;
	struct vm_area_struct *vma;
	struct folio *old;
	struct page *page;
	pmd_t *pmdp;
	pte_t *ptep;
	pte_t cur, entry;
	spinlock_t *ptl;	/* ptl nests below the desc write lock (R2) */
	int ret;

	vma = corten_arena_get_vma(ctx);
	if (!vma)
		return -EFAULT;

	pmdp = corten_arena_pmd(mm, ctx->addr);
	if (!pmdp)
		return -EAGAIN;
	ptep = pte_offset_map_lock(mm, pmdp, ctx->addr, &ptl);
	if (!ptep)
		return -EAGAIN;

	cur = ptep_get(ptep);
	if (unlikely(!pte_present(cur) || pte_special(cur))) {
		/* MAPPED without a translation: no M3 path produces this
		 * (the restore-side WARN owns the diagnosis).
		 */
		pte_unmap_unlock(ptep, ptl);
		WARN_ON_ONCE(1);
		return -EFAULT;
	}

	page = pte_page(cur);
	old = page_folio(page);

	if (folio_mapcount(old) == 1) {
		/* Reuse (paper: map_count==1).  The meta flag clear and
		 * the PTE re-arm are one transaction: every observer is
		 * excluded by the covering desc write lock.
		 */
		struct corten_pte_meta nm = *m;

		nm.flags = m->flags & ~CORTEN_PF_SHARED;
		ret = corten_mark(txn, ctx->addr, PAGE_SIZE, &nm);
		if (unlikely(ret)) {
			pte_unmap_unlock(ptep, ptl);
			return ret == -ENOMEM ? -ENOMEM : -EFAULT;
		}

		entry = mk_pte(page, corten_arena_perm_pgprot(vma, m->perm));
		entry = pte_mkyoung(entry);
		entry = pte_mkwrite(pte_mkdirty(entry), vma);
		/* Flushes only when the old translation could be cached
		 * (the fork's RO shape always qualifies).
		 */
		ptep_set_access_flags(vma, ctx->addr, ptep, entry, 1);
		update_mmu_cache_range(NULL, vma, ctx->addr, ptep, 1);
		pte_unmap_unlock(ptep, ptl);

		corten_arena_fault_stat(READ_ONCE(mm->corten_state),
					CORTEN_ARENA_STAT_COW_REUSE);
		return 0;
	}

	/* Copy branch: the fault path preallocates for every write
	 * fault; a lost race consumed the folio -- retry re-arms it.
	 */
	if (!ctx->folio) {
		pte_unmap_unlock(ptep, ptl);
		return -EAGAIN;
	}

	/* Copy while holding the PTE lock, like wp_page_copy(): the page
	 * contents are copied from the old folio, which stays alive for
	 * the whole critical section (our PTE reference is not dropped
	 * before the new translation is in).
	 */
	copy_user_highpage(folio_page(ctx->folio, 0), page, ctx->addr, vma);
	__folio_mark_uptodate(ctx->folio);

	if (unlikely(check_stable_address_space(mm))) {
		pte_unmap_unlock(ptep, ptl);
		return -EAGAIN;
	}

	/* Break-before-make (the map_anon zero-upgrade precedent): the old
	 * read-only translation may be cached in any CPU's TLB.
	 */
	ptep_clear_flush(vma, ctx->addr, ptep);

	entry = folio_mk_pte(ctx->folio, corten_arena_perm_pgprot(vma,
								  m->perm));
	entry = pte_mkyoung(entry);
	entry = pte_mkwrite(pte_mkdirty(entry), vma);

	/* The speculative single reference becomes the new PTE reference
	 * (order-0); the old folio's PTE reference is released only after
	 * the flush, in the zap ordering.
	 */
	add_mm_counter(mm, MM_ANONPAGES, 1);
	folio_add_new_anon_rmap(ctx->folio, vma, ctx->addr, RMAP_EXCLUSIVE);
	set_ptes(mm, ctx->addr, ptep, entry, 1);
	update_mmu_cache_range(NULL, vma, ctx->addr, ptep, 1);

	folio_remove_rmap_pte(old, page, vma);
	add_mm_counter(mm, MM_ANONPAGES, -1);
	pte_unmap_unlock(ptep, ptl);
	folio_put(old);

	/* MAPPED->MAPPED needs FORCE (corten.h); corten_map() resets the
	 * flags -- SHARED is gone with the shared folio, which is exactly
	 * the private copy's semantics.
	 */
	ret = corten_map(txn, ctx->addr, folio_page(ctx->folio, 0), m->perm,
			 CORTEN_MAP_FORCE);
	if (WARN_ON_ONCE(ret))
		return -EFAULT;

	corten_arena_fault_stat(READ_ONCE(mm->corten_state),
				CORTEN_ARENA_STAT_COW_COPY);

	/* The speculative single reference became the new PTE reference
	 * (see the fault_once success epilogue for the other handlers'
	 * ownership).
	 */
	ctx->folio = NULL;

	return 0;
}

/*
 * One transaction attempt (sec 4.3 for(;;) body).  On entry ctx->folio is
 * the speculative allocation or NULL; on any return the epilogue of the
 * caller owns it (success transferred it -- see map_anon).
 */
static enum corten_fault_status
corten_arena_fault_once(struct corten_fault_ctx *ctx)
{
	struct corten_txn txn;
	struct corten_pte_meta m;
	enum corten_disp disp;
	int ret;

	ret = corten_arena_fill_upper(ctx->ar, ctx->addr);
	switch (ret) {
	case 0:
		break;
	case -EAGAIN:
		return CORTEN_F_RETRY;
	case -ENOMEM:
		return CORTEN_F_OOM;
	default:	/* -EOPNOTSUPP: huge leaf -- legacy is self-consistent */
		return CORTEN_F_FALLBACK;
	}

	ret = corten_lock_range(ctx->mm, ctx->addr, PAGE_SIZE, &txn);
	switch (ret) {
	case 0:
		break;
	case -EAGAIN:			/* PT page retirement race (Fig.7) */
		return CORTEN_F_RETRY;
	case -ENOMEM:
		return CORTEN_F_OOM;
	case -ENOENT:
	case -EOPNOTSUPP:
		/* PT page not tracked (descriptor install failed at
		 * pte_alloc time, or the install raced a retirement):
		 * fill_upper() just re-armed the descriptor -- retry the
		 * lock once before handing the fault to the legacy body
		 * (sec 4.3 err_legacy_fallback).  Only a window that
		 * stays untrackable counts as drift; a recovered one is
		 * named-counted instead.
		 */
		corten_arena_fill_upper(ctx->ar, ctx->addr);
		ret = corten_lock_range(ctx->mm, ctx->addr, PAGE_SIZE, &txn);
		if (!ret) {
			atomic_long_inc(&corten_nr_rearm_recovered);
			break;		/* txn held: proceed */
		}
		corten_legacy_drift_inc();
		atomic_long_inc(&corten_nr_rearm_failed);
		return ret == -ENOMEM ? CORTEN_F_OOM : CORTEN_F_FALLBACK;
	default:
		WARN_ON_ONCE(1);
		return CORTEN_F_FALLBACK;
	}

	/* [F-B seal] Re-check ownership under the covering desc write lock:
	 * the hot-hook lookup ran before any concurrent punch erased the
	 * frame; committing a fresh anonymous page here would land under
	 * the incoming legacy VMA (r05 dg2-analysis.md D2, punch-window
	 * race).  A rejected commit falls back to the legacy funnel, which
	 * sees the post-punch VMA topology and serves correctly.
	 */
	if (unlikely(!corten_arena_txn_owned(ctx->ar, ctx->mm, ctx->addr))) {
		corten_unlock(&txn);
		return CORTEN_F_FALLBACK;
	}

	if (corten_query(&txn, ctx->addr, &m)) {
		corten_unlock(&txn);
		WARN_ON_ONCE(1);
		return CORTEN_F_FALLBACK;
	}

	/* CORTEN_DISP_FRESH (Fig.8 L26-38, the missing-page body of the
	 * arena fault cycle): a page inside a declared arena that no
	 * producer ever recorded is a fresh PrivateAnon virtual
	 * allocation.  Gate the access on the arena contract first (the
	 * metadata carries no state, so dispatch's own permission check
	 * cannot run), then record the allocation -- from here on the fault is
	 * indistinguishable from an mmap-marked one, which keeps the
	 * map/zero-page/upgrade machinery single-sourced.  This was the
	 * guest-smoke MAPERR: the churn mark path bypassed the synthesis,
	 * and the KUnit cases seeded their metadata, so both the review
	 * and the suite sailed past it (r03 DoD lesson: every real-chain
	 * test must cover at least one unseeded page).
	 */
	if (m.state == CORTEN_INVALID) {
		struct corten_pte_meta gate = {
			.state = CORTEN_PRIVATE_ANON,
			/* A routed mprotect() rewrites this upper bound
			 * under the arena's active-ref barrier (T0b).  A
			 * slot that carries a perm despite being Invalid
			 * is a dropped-content page (chunk munmap /
			 * MADV_DONTNEED route, CORTEN_UNMAP_KEEP_PERM):
			 * the committed mprotect contract outlives the
			 * content, so it wins over the DECLARE bound.
			 */
			.perm = m.perm ? m.perm :
					 READ_ONCE(ctx->ar->prot),
		};
		struct corten_pte_meta fresh = gate;

		if (!corten_arena_perm_ok(&gate, ctx->write,
					  ctx->instruction)) {
			corten_unlock(&txn);
			return CORTEN_F_ACCERR;
		}
		ret = corten_mark(&txn, ctx->addr, PAGE_SIZE, &fresh);
		switch (ret) {
		case 0:
			break;
		case -ENOMEM:
			corten_unlock(&txn);
			return CORTEN_F_OOM;
		case -EAGAIN:
			corten_unlock(&txn);
			return CORTEN_F_RETRY;
		default:
			WARN_ON_ONCE(1);
			corten_unlock(&txn);
			return CORTEN_F_FALLBACK;
		}
		m = fresh;
	}

	disp = corten_arena_dispatch(&m, ctx->write, ctx->instruction);
	/* A pre-allocated folio means the zero page is forbidden
	 * (mm_forbids_zeropage, OQ-6): install a real read-only page
	 * instead of the shared zero page.
	 */
	if (disp == CORTEN_DISP_ZERO_PAGE && ctx->folio)
		disp = CORTEN_DISP_MAP_ANON;

	switch (disp) {
	case CORTEN_DISP_MAP_ANON:
		ret = corten_arena_map_anon(ctx, &txn, &m, ctx->folio);
		break;
	case CORTEN_DISP_ZERO_PAGE:
		ret = corten_arena_zero_page(ctx, &txn, &m);
		break;
	case CORTEN_DISP_RESTORE:
		ret = corten_arena_restore_pte(ctx, &txn, &m);
		break;
	case CORTEN_DISP_COW_MAYBE:
		ret = corten_arena_cow_write(ctx, &txn, &m);
		break;
	case CORTEN_DISP_COW_COPY:
		/* T1a: a write against a shared page whose contract is
		 * read-only is a genuine permission fault (the page was
		 * RO before the fork -- the wrprotect changed nothing).
		 * The STUB-era WARN is gone: this is a normal, legal
		 * outcome.  The FOLL_FORCE-forced copy (ptrace POKE on
		 * such a page) is M5.T2' scope (M5_FORK_SPEC.md sec 3.4).
		 */
		corten_unlock(&txn);
		return CORTEN_F_ACCERR;
	case CORTEN_DISP_ACCERR:
		corten_unlock(&txn);
		return CORTEN_F_ACCERR;
	case CORTEN_DISP_STUB:
		/* M6 swap / M4+ shared-anon: M3 refuses loudly. */
		WARN_ONCE(1, "corten: unhandled arena metadata state %u\n",
			  m.state);
		corten_unlock(&txn);
		return CORTEN_F_MAPERR;
	case CORTEN_DISP_MAPERR:
	default:
		corten_unlock(&txn);
		return CORTEN_F_MAPERR;
	}

	corten_unlock(&txn);

	if (ret == 0) {
		/* Success.  Only MAP_ANON installed a *fresh* page and
		 * transferred the speculative reference into the PTE.
		 * The zero-page, restore and COW-reuse handlers re-arm
		 * an existing translation and never touch the folio --
		 * the reference stays with the caller, whose epilogue
		 * folio_put() releases it.  Blanket-NULLing here leaked
		 * exactly one preallocated page per such fault: with
		 * T1a every post-fork parent write re-arms a shared
		 * page, which is the 1k-page round-trip OOM (r06/m5t1a
		 * fork_roundtrip: ~4 MB per fork leaked, with MemFree
		 * flat in AnonPages/PageTables/Percpu -- pure allocator
		 * loss).  cow_write's copy branch NULLs @ctx->folio
		 * itself.
		 */
		if (disp == CORTEN_DISP_MAP_ANON)
			ctx->folio = NULL;
		return CORTEN_F_HANDLED;
	}
	if (ret == -EAGAIN || ret == -ENOMEM)
		return ret == -EAGAIN ? CORTEN_F_RETRY : CORTEN_F_OOM;

	return CORTEN_F_FALLBACK;
}

/*
 * S4 hot path (sec 4.1/4.3).  Called for user-mode faults only, before
 * any VMA/mmap_lock/per-VMA-lock action in do_user_addr_fault().  On
 * CORTEN_FAULT_FALLBACK the legacy path runs unchanged.
 */
enum corten_fault_action corten_arena_user_fault(struct mm_struct *mm,
						 unsigned long address,
						 unsigned long error_code,
						 struct pt_regs *regs,
						 unsigned int *flags)
{
	struct corten_fault_ctx ctx = { };
	struct corten_mm_state *state;
	struct corten_arena *ar;
	bool need_folio;
	int tries = 0;
	int st;

	/* Second half of the double gate: one load for the common
	 * "process never declared an arena" case.  Pairs with the
	 * smp_store_release() publisher (S1-S3 re-verify convention).
	 */
	state = smp_load_acquire(&mm->corten_state);
	if (!state || !refcount_read(&state->nr))
		return CORTEN_FAULT_FALLBACK;

	ar = corten_arena_lookup_get(mm, address);
	if (!ar)
		return CORTEN_FAULT_FALLBACK;

	/* [F-A, D-G''] Ownership self-check: the frame table is
	 * address-keyed, so a legacy punch (file MAP_FIXED into the arena,
	 * the r05 JVM CDS crash shape) leaves stale frames claiming a range
	 * whose covering VMA is a file mapping.  Serving that fault here
	 * would ACCERR on the FRESH gate -- or, with the gate opened, hand
	 * the file mapping a fresh anonymous zero page (silent corruption).
	 * A non-owned address runs the legacy funnel, which sees the real
	 * VMA and serves file data.  The slow hook is vma-keyed upstream and
	 * needs no counterpart; the transaction body re-checks under the
	 * window lock (corten_arena_txn_owned()).
	 */
	if (unlikely(!corten_arena_fault_owned(ar, mm, address))) {
		this_cpu_inc(state->stats[CORTEN_ARENA_STAT_FALLBACKS]);
		percpu_ref_put(&ar->active);
		return CORTEN_FAULT_FALLBACK;
	}

	ctx.mm = mm;
	ctx.ar = ar;
	ctx.addr = address & PAGE_MASK;
	ctx.write = *flags & FAULT_FLAG_WRITE;
	ctx.instruction = *flags & FAULT_FLAG_INSTRUCTION;
	ctx.regs = regs;

	this_cpu_inc(state->stats[CORTEN_ARENA_STAT_FAULTS]);

	/* [P1-1] Speculative allocation, before the covering write lock.
	 * [P1-2] need_zero is implicit in the zeroed allocator.  Read
	 * faults normally take the shared zero page; a folio is only
	 * needed when mm forbids it (OQ-6).
	 */
	need_folio = ctx.write || mm_forbids_zeropage(mm);
	if (need_folio) {
		struct folio *folio;

		/* [FAIL-2] cached descriptor pointer, no maple walk. */
		ctx.vma = corten_arena_shadow_vma(ar);
		if (!ctx.vma) {
			st = CORTEN_F_FALLBACK;
			goto out;
		}
		folio = corten_arena_folio_prealloc(mm, ctx.vma, ctx.addr);
		if (!folio) {
			st = CORTEN_F_OOM;
			goto out;
		}
		ctx.folio = folio;
	}

	for (;;) {
		st = corten_arena_fault_once(&ctx);
		if (st != CORTEN_F_RETRY || ++tries >= 2)
			break;	/* Fig.7 retry cap, x86 fault.c:1408 style */

		if (need_folio && !ctx.folio) {
			struct folio *folio;

			folio = corten_arena_folio_prealloc(mm, ctx.vma,
							    ctx.addr);
			if (!folio) {
				st = CORTEN_F_OOM;
				break;
			}
			ctx.folio = folio;
		}
	}

out:
	if (ctx.folio)
		folio_put(ctx.folio);
	percpu_ref_put(&ar->active);

	switch (st) {
	case CORTEN_F_HANDLED:
		corten_arena_account_fault(mm, regs, ctx.addr, true);
		return CORTEN_FAULT_HANDLED;
	case CORTEN_F_ACCERR:
		this_cpu_inc(state->stats[CORTEN_ARENA_STAT_ACCERR]);
		break;
	case CORTEN_F_MAPERR:
		this_cpu_inc(state->stats[CORTEN_ARENA_STAT_MAPERR]);
		return CORTEN_FAULT_MAPERR;
	case CORTEN_F_OOM:
		corten_arena_account_fault(mm, regs, ctx.addr, false);
		return CORTEN_FAULT_OOM;
	default:
		this_cpu_inc(state->stats[CORTEN_ARENA_STAT_FALLBACKS]);
		return CORTEN_FAULT_FALLBACK;
	}

	corten_arena_account_fault(mm, regs, ctx.addr, false);
	return CORTEN_FAULT_ACCERR;
}

/*
 * S4 slow path (sec 4.2): GUP-slow, fixup_user_fault, ptrace, get_user
 * and every other handle_mm_fault() caller on a shadow-VMA.  The VMA-lock
 * and in_atomic exits are the hook's (mm/memory.c); this function runs
 * with no retry contract on mmap_lock -- it never returns VM_FAULT_RETRY.
 */
vm_fault_t corten_arena_handle_mm_fault(struct vm_area_struct *vma,
					unsigned long address,
					unsigned int flags,
					struct pt_regs *regs)
{
	struct corten_fault_ctx ctx = { };
	struct corten_mm_state *state;
	struct corten_arena *ar;
	bool need_folio;
	int tries = 0;
	int st;

	ar = corten_arena_lookup_get(vma->vm_mm, address);
	if (!ar) {
		/* Raced with RELEASE (frames erased): the shadow-VMA is
		 * being torn down anyway -- legacy will fail naturally.
		 * [FAIL-1] The marker bit must be set: a bare
		 * VM_FAULT_FALLBACK is a VM_FAULT_ERROR value and would
		 * be reported to the caller instead of running the
		 * legacy body.
		 */
		return CORTEN_FAULT_FALLBACK_BIT | VM_FAULT_FALLBACK;
	}

	ctx.mm = vma->vm_mm;
	ctx.ar = ar;
	ctx.vma = vma;
	ctx.addr = address & PAGE_MASK;
	ctx.write = flags & FAULT_FLAG_WRITE;
	ctx.instruction = flags & FAULT_FLAG_INSTRUCTION;
	ctx.regs = regs;

	/* Pairs with the smp_store_release() publisher in the DECLARE
	 * path: the state pointer and its refcount must be visible
	 * together with the shadow-VMA/xarray contents read below.
	 */
	state = smp_load_acquire(&ctx.mm->corten_state);
	this_cpu_inc(state->stats[CORTEN_ARENA_STAT_FAULTS]);

	need_folio = ctx.write || mm_forbids_zeropage(ctx.mm);
	st = CORTEN_F_HANDLED;
	if (need_folio) {
		struct folio *folio;

		folio = corten_arena_folio_prealloc(ctx.mm, vma, ctx.addr);
		if (!folio)
			st = CORTEN_F_OOM;
		else
			ctx.folio = folio;
	}

	if (st == CORTEN_F_HANDLED) {
		for (;;) {
			st = corten_arena_fault_once(&ctx);
			if (st != CORTEN_F_RETRY || ++tries >= 2)
				break;
			if (need_folio && !ctx.folio) {
				struct folio *folio;

				folio = corten_arena_folio_prealloc(ctx.mm,
								    vma,
								    ctx.addr);
				if (!folio) {
					st = CORTEN_F_OOM;
					break;
				}
				ctx.folio = folio;
			}
		}
	}

	if (ctx.folio)
		folio_put(ctx.folio);
	percpu_ref_put(&ar->active);

	/* Accounting replication ([P2-8]): the memcg-OOM wrapper and
	 * mm_account_fault() of the normal handle_mm_fault() body are
	 * below the hook, so the arena path does its own.
	 */
	if (flags & FAULT_FLAG_USER)
		mem_cgroup_enter_user_fault();
	corten_arena_account_fault(ctx.mm, regs, ctx.addr,
				   st == CORTEN_F_HANDLED);
	if (flags & FAULT_FLAG_USER)
		mem_cgroup_exit_user_fault();

	switch (st) {
	case CORTEN_F_HANDLED:
		return 0;
	case CORTEN_F_ACCERR:
	case CORTEN_F_MAPERR:
		return VM_FAULT_SIGSEGV;
	case CORTEN_F_OOM:
		return VM_FAULT_OOM;
	default:
		/* Untracked PT page / exhausted retries: this function is
		 * called from inside handle_mm_fault(), so the FALLBACK
		 * marker continues the hook into the legacy body -- which
		 * is fully consistent for a shadow-VMA (sec 4.2; the
		 * design's "fallback must be fatal" applies to the
		 * FOLL_FORCE COW stub, which returns VM_FAULT_SIGSEGV
		 * above instead).  [FAIL-1] The marker bit, not the bare
		 * VM_FAULT_ERROR value, is what routes the hook into the
		 * legacy body.
		 */
		return CORTEN_FAULT_FALLBACK_BIT | VM_FAULT_FALLBACK;
	}
}

/* ------------------------------------------------------------------ *
 * S6: space-operation routing (M3B_DESIGN.md sec 5)
 * ------------------------------------------------------------------
 */

/*
 * Range-vs-arena classification (pure; table-driven KUnit in
 * mm/corten_fault_test.c).
 */
enum corten_unmap_class corten_arena_unmap_classify(unsigned long start,
						    unsigned long end,
						    unsigned long ar_start,
						    unsigned long ar_end)
{
	if (end <= ar_start || start >= ar_end)
		return CORTEN_UNMAP_OUTSIDE;
	if (start == ar_start && end == ar_end)
		return CORTEN_UNMAP_EXACT;
	if (start >= ar_start && end <= ar_end)
		return CORTEN_UNMAP_CHUNK;

	return CORTEN_UNMAP_PARTIAL;
}

enum corten_unmap_class corten_arena_release_classify(enum corten_unmap_class
						      class,
						      unsigned long start,
						      unsigned long end,
						      unsigned long ar_start,
						      unsigned long ar_end)
{
	/* T0 release-on-full-coverage (M4T0_SPEC.md sec 3.2): the user
	 * munmaps the length it asked for at mmap time (page-rounded),
	 * while the arena was rounded up to 2M granularity -- the natural
	 * allocator churn shape (glibc free() on an mmap'd block) leaves
	 * a tail of up to 2M-PAGE_SIZE.  That is a full-coverage release
	 * of the user-visible mapping, not a chunk zap: the tail is
	 * kernel-private padding.  Without this rule every such munmap
	 * would keep the VMA + 2M of window VA alive and dedup_eq-style
	 * churn would exhaust the window.
	 */
	if (class == CORTEN_UNMAP_CHUNK && start == ar_start &&
	    ar_end - end < PMD_SIZE)
		return CORTEN_UNMAP_EXACT;

	return class;
}

/*
 * Zap the PTEs of the recorded pages in [start, end) (at most one PMD
 * window; the caller holds the covering desc write lock obtained through
 * @txn) and flip the recorded metadata back to CORTEN_INVALID.  Page
 * references are handed to @tlb, so every folio_put() happens after the
 * TLB flush -- the same ordering guarantee unmap_vmas() relies on
 * (sec 5.5 boundary argument: a re-map of the same VA during the window
 * only produces a harmless extra fault, the old page's reference is
 * dropped after the flush).
 */
static int corten_arena_zap_window(struct mm_struct *mm,
				   struct vm_area_struct *vma,
				   struct corten_txn *txn,
				   unsigned long start, unsigned long end,
				   struct mmu_gather *tlb)
{
	unsigned long addr;
	unsigned long flush_start = 0, flush_end = 0;
	bool flushed = false;
	pmd_t *pmdp;
	int ret = 0;

	pmdp = corten_arena_pmd(mm, start);
	if (WARN_ON_ONCE(!pmdp))
		return -EAGAIN;

	/*
	 * Drive the walk by PTE content, not metadata: the metadata only
	 * decides whether a corten_unmap() reset is needed.  A page can
	 * have a PTE with no (or INVALID) metadata behind it -- the legacy
	 * fallback body writes PTEs on a shadow-VMA when the transaction
	 * layer hands a fault over -- and trusting the metadata there
	 * leaves the old translation live through the munmap (r03 defect
	 * C: the arena_stress zerocheck read the previous cycle's magic).
	 * Every producer of an in-arena PTE (arena map/zero page, legacy
	 * fault, GUP) attaches a private anonymous page with rmap, so the
	 * present-and-not-special release below is correct for all of
	 * them; the shared zero page is pte_special()d and owns nothing.
	 *
	 * The mmu_gather session here is range-based (tlb_gather_mmu(),
	 * not _fullmm) and this walk bypasses tlb_start_vma(); the
	 * tlb_remove_tlb_entry() calls below are what feed the gather's
	 * flush range, so both tlb_finish_mmu() and any mid-batch
	 * tlb_flush_mmu() (page-batch overflow, discontiguous spans)
	 * invalidate exactly the cleared span.  The explicit
	 * flush_tlb_range() on every exit -- error paths included -- keeps
	 * the guarantee local to this walk.
	 */
	for (addr = start; addr < end; addr += PAGE_SIZE) {
		struct corten_pte_meta m;
		struct folio *folio;
		struct page *page;
		pte_t *ptep;
		pte_t oldpte;
		bool recorded;
		spinlock_t *ptl;

		recorded = corten_query(txn, addr, &m) == 0 &&
			   m.state != CORTEN_INVALID;

		ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
		if (!ptep) {
			ret = -EAGAIN;
			goto out_flush;
		}

		oldpte = ptep_get_and_clear(mm, addr, ptep);
		if (!pte_none(oldpte))
			tlb_remove_tlb_entry(tlb, ptep, addr);
		pte_unmap_unlock(ptep, ptl);

		if (!pte_none(oldpte)) {
			if (!flushed) {
				flush_start = addr;
				flushed = true;
			}
			flush_end = addr + PAGE_SIZE;

			/* Zero-page entries are pte_special()d and not
			 * ours to release; swap entries cannot exist in
			 * M3.
			 */
			if (pte_present(oldpte) && !pte_special(oldpte)) {
				page = pte_page(oldpte);
				folio = page_folio(page);

				/* [F-B] A re-punch over an already-punched
				 * window can find file pages installed
				 * there by the previous punch's legacy
				 * mapping; only the counter differs for
				 * those (the zap_pte_range() convention --
				 * the !anon rmap removal is
				 * vma-insensitive).  Every other producer
				 * of an in-arena PTE (arena map/zero page,
				 * legacy fault, GUP) attaches a private
				 * anonymous page.
				 */
				folio_remove_rmap_pte(folio, page, vma);
				if (folio_test_anon(folio))
					add_mm_counter(mm, MM_ANONPAGES, -1);
				else
					add_mm_counter(mm, MM_FILEPAGES, -1);
				tlb_remove_page(tlb, page);
			}
		}

		if (recorded) {
			/* KEEP_PERM: the content drop must not dissolve the
			 * mprotect contract committed on the VA.  The slot
			 * comes back Invalid but keeps the recorded perm, so
			 * the FRESH fault gate re-derives the committed
			 * permission (zero-fill-on-demand semantics, like the
			 * legacy MADV_DONTNEED/munmap of a committed chunk)
			 * instead of the DECLARE-time arena bound -- a
			 * PROT_NONE reservation with routed RW commits died
			 * with SEGV_ACCERR on the next write here (the r06
			 * dedup_eq "rogue" ACCERR family).
			 */
			ret = corten_unmap(txn, addr, PAGE_SIZE,
					   CORTEN_UNMAP_KEEP_PERM);
			if (WARN_ON_ONCE(ret))
				goto out_flush;

			this_cpu_inc(READ_ONCE(mm->corten_state)->stats[
					CORTEN_ARENA_STAT_UNMAP_PAGES]);
		}
	}

out_flush:
	if (flushed)
		flush_tlb_range(vma, flush_start, flush_end);

	return ret;
}

/*
 * Chunk unmap (sec 5.5) / MADV_DONTNEED content drop (sec 5.8): drop the
 * *contents* of [start, start+len) -- strictly inside one arena -- while
 * the shadow-VMA and its VA reservation stay untouched (paper Fig.8
 * L9-13: unmap clears content, keeps VA).  Iterates 2M windows, one
 * covering-write-lock transaction each; folio_put()s are TLB-ordered by
 * the mmu_gather.  @ar must be pinned by the caller (active reference).
 */
/*
 * Content-driven zap of a window whose PT page has no descriptor (the
 * M2a install failed at pte_alloc time, or the fault path handed the
 * window to the legacy body).  The old assumption -- "no tracked PT
 * page: no page of it can be mapped" -- is false for exactly those
 * windows: the legacy fault funnel writes plain PTEs on the shadow-VMA.
 * Skipping the window would leave the previous cycle's translations and
 * pages alive through the munmap, so zap whatever is actually there.
 * No transaction and no metadata exist; every present non-special PTE
 * is a private anonymous page with rmap (see corten_arena_zap_window()).
 */
static int corten_arena_zap_untracked_window(struct mm_struct *mm,
					     struct vm_area_struct *vma,
					     unsigned long start,
					     unsigned long end,
					     struct mmu_gather *tlb)
{
	unsigned long addr;
	unsigned long flush_start = 0, flush_end = 0;
	bool flushed = false;
	pmd_t *pmdp;
	int ret = 0;

	pmdp = corten_arena_pmd(mm, start);
	if (!pmdp || !pmd_present(READ_ONCE(*pmdp)))
		return 0;		/* nothing was ever mapped here */
	if (pmd_leaf(READ_ONCE(*pmdp)))
		return -EOPNOTSUPP;	/* THP: not ours in M3 */

	for (addr = start; addr < end; addr += PAGE_SIZE) {
		struct folio *folio;
		struct page *page;
		pte_t *ptep;
		pte_t oldpte;
		spinlock_t *ptl;

		ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
		if (!ptep) {
			ret = -EAGAIN;
			goto out_flush;
		}

		oldpte = ptep_get_and_clear(mm, addr, ptep);
		if (!pte_none(oldpte))
			tlb_remove_tlb_entry(tlb, ptep, addr);
		pte_unmap_unlock(ptep, ptl);

		if (!pte_none(oldpte)) {
			if (!flushed) {
				flush_start = addr;
				flushed = true;
			}
			flush_end = addr + PAGE_SIZE;

			/* [F-B] Anon/file split as in
			 * corten_arena_zap_window(): an already-punched
			 * window can carry file pages from the legacy
			 * mapping that filled it.
			 */
			if (pte_present(oldpte) && !pte_special(oldpte)) {
				page = pte_page(oldpte);
				folio = page_folio(page);

				folio_remove_rmap_pte(folio, page, vma);
				if (folio_test_anon(folio))
					add_mm_counter(mm, MM_ANONPAGES, -1);
				else
					add_mm_counter(mm, MM_FILEPAGES, -1);
				tlb_remove_page(tlb, page);
			}
		}
	}

out_flush:
	if (flushed) {
		corten_legacy_drift_inc();
		flush_tlb_range(vma, flush_start, flush_end);
	}

	return ret;
}

/* Non-static: mm/corten_fault_test.c drives the chunk zap directly. */
int corten_arena_unmap_chunk(struct mm_struct *mm, struct corten_arena *ar,
			     unsigned long start, unsigned long len)
{
	unsigned long end = start + len;
	struct vm_area_struct *vma;
	struct mmu_gather tlb;
	int ret = 0;

	vma = corten_arena_shadow_vma(ar);
	if (!vma)
		return -EOPNOTSUPP;

	tlb_gather_mmu(&tlb, mm);

	while (start < end) {
		unsigned long win_end = min((start | (PMD_SIZE - 1)) + 1, end);
		struct corten_txn txn;
		int tries = 0;

		for (;;) {
			ret = corten_lock_range(mm, start, win_end - start,
						&txn);
			if (ret != -EAGAIN || ++tries >= 2)
				break;
		}

		switch (ret) {
		case 0:
			ret = corten_arena_zap_window(mm, vma, &txn, start,
						      win_end, &tlb);
			corten_unlock(&txn);
			if (ret)
				goto out;
			this_cpu_inc(READ_ONCE(mm->corten_state)->stats[
					CORTEN_ARENA_STAT_MUNMAP_TXNS]);
			break;
		case -ENOENT:
		case -EOPNOTSUPP:
			/* No tracked PT page in this window.  Both codes
			 * mean "nothing transactional here": -ENOENT is a
			 * hole, -EOPNOTSUPP a PT page whose descriptor
			 * install failed (or a huge leaf).  The old
			 * assumption -- "nothing tracked: nothing mapped"
			 * -- is false for the first two: the legacy fault
			 * fallback writes plain PTEs on untracked windows
			 * (r03 defect C).  Zap by PTE content so the
			 * munmap drops whatever is actually there; the
			 * helper re-checks the huge-leaf case itself.
			 */
			ret = corten_arena_zap_untracked_window(mm, vma,
								start, win_end,
								&tlb);
			if (ret)
				goto out;
			ret = 0;
			break;
		case -EAGAIN:
			/* Exhausted retries: PT page retiring; nothing may
			 * be zapped through the legacy funnel while the
			 * descriptor is stale.
			 */
			fallthrough;
		default:
			tlb_finish_mmu(&tlb);
			return ret == -EOPNOTSUPP ? -EOPNOTSUPP : -EAGAIN;
		}

		start = win_end;
	}

out:
	tlb_finish_mmu(&tlb);
	return ret;
}

/*
 * T0b review C1: unmap_chunk() -EAGAIN is a transient PT-page
 * retirement race (Fig.7's "caller retries" contract), but libc never
 * retries mprotect/mremap/madvise, so a leaked -EAGAIN is a permanent
 * application failure.  The three syscall-route exits that surface the
 * chunk transaction retry once here instead; a persistent race is
 * counted (eagain_leaked) and still returned, because dropping to the
 * legacy funnel (ret = 0) would zap arena PTEs outside a transaction
 * -- the r03 defect-C shape -- while the descriptor is stale.
 */
static int corten_arena_unmap_chunk_retry(struct mm_struct *mm,
					  struct corten_arena *ar,
					  unsigned long start, unsigned long len)
{
	int ret = corten_arena_unmap_chunk(mm, ar, start, len);

	if (ret != -EAGAIN)
		return ret;
	atomic_long_inc(&corten_nr_eagain_retries);
	ret = corten_arena_unmap_chunk(mm, ar, start, len);
	if (ret == -EAGAIN)
		atomic_long_inc(&corten_nr_eagain_leaked);
	return ret;
}

/*
 * sys_munmap() entry routing (sec 5.5).  Runs with no locks held.
 * Return: 0 = run the legacy munmap, 1 = handled, -errno = reject.
 */
int corten_arena_munmap_route(struct mm_struct *mm, unsigned long start,
			      unsigned long len)
{
	struct corten_arena *ar_start, *ar_end;
	enum corten_unmap_class class;
	unsigned long end = start + len;
	int ret = 0;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_state))
		return 0;
	if (!len || (len & ~PAGE_MASK) || end <= start)
		return 0;

	ar_start = corten_arena_lookup_get(mm, start);
	ar_end = corten_arena_lookup_get(mm, end - 1);

	if (!ar_start && !ar_end)
		return 0;

	if (ar_start && ar_end && ar_start != ar_end) {
		ret = -EOPNOTSUPP;	/* crosses an arena boundary */
	} else {
		struct corten_arena *ar = ar_start ?: ar_end;

		class = corten_arena_unmap_classify(start, end, ar->start,
						    ar->end);
		/* T0: a CHUNK that spans from the arena base and leaves a
		 * sub-2M tail is RELEASE territory (see the classify
		 * helper's comment).
		 */
		{
			enum corten_unmap_class raw = class;

			class = corten_arena_release_classify(class, start,
							      end, ar->start,
							      ar->end);
			if (class != raw)
				atomic_long_inc(&corten_nr_munmap_releases);
		}
		switch (class) {
		case CORTEN_UNMAP_EXACT:
			/* The exact arena range: identical to prctl
			 * RELEASE (drain + legacy teardown removes the
			 * shadow-VMA).  Drop our active references
			 * first: the drain inside RELEASE must reach
			 * zero.  Both lookups pinned the same descriptor
			 * when start and end fall in one arena, so each
			 * pointer is put (the old "!= ar_start" guard
			 * leaked one reference per call and RELEASE's
			 * drain hung forever -- r03 DoD failure B).
			 * RELEASE (like DECLARE) takes the write lock
			 * itself -- we run lockless here, so this is the
			 * DEV-13 outermost acquisition, not a re-entry.
			 */
			if (ar_start)
				percpu_ref_put(&ar_start->active);
			if (ar_end)
				percpu_ref_put(&ar_end->active);
			ret = corten_arena_release(mm, ar->start,
						   ar->end - ar->start);
			return ret < 0 ? ret : 1;
		case CORTEN_UNMAP_CHUNK:
			ret = corten_arena_unmap_chunk(mm, ar, start, len);
			ret = ret ? ret : 1;
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
	}

	/* One put per lookup: when start and end-1 resolve to the same
	 * arena (the common in-arena chunk), both pointers are equal and
	 * each holds one of the two references taken above.
	 */
	if (ar_start)
		percpu_ref_put(&ar_start->active);
	if (ar_end)
		percpu_ref_put(&ar_end->active);

	return ret;
}

/*
 * Routing for callers already holding mmap_lock for writing
 * (__vm_munmap): the chunk transaction runs under the write lock, a
 * legal lock-order edge (DEV-13: mmap_lock outermost, desc->lock inner,
 * and no transaction ever takes mmap_lock back).  The exact range cannot
 * route to RELEASE here: RELEASE acquires mmap_write itself and the
 * semaphore is not recursive; kernel callers never munmap declared
 * arenas anyway.
 */
int corten_arena_munmap_guard(struct mm_struct *mm, unsigned long start,
			      unsigned long len)
{
	struct corten_arena *ar;
	enum corten_unmap_class class;
	unsigned long end = start + len;
	int ret;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_state))
		return 0;
	if (!len || (len & ~PAGE_MASK) || end <= start)
		return 0;

	ar = corten_arena_lookup_get(mm, start);
	if (!ar)
		ar = corten_arena_lookup_get(mm, end - 1);
	if (!ar)
		return 0;

	class = corten_arena_unmap_classify(start, end, ar->start, ar->end);
	switch (class) {
	case CORTEN_UNMAP_CHUNK:
		ret = corten_arena_unmap_chunk(mm, ar, start, len);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	percpu_ref_put(&ar->active);

	return ret;
}

/*
 * The deepest guard, do_vmi_align_munmap() (sec 5.5/5.10): every legacy
 * zap funnel that is not routed above (brk shrink, mremap's internal
 * unmaps) must reject ranges overlapping a shadow-VMA, because
 * zap_pte_range() writes PTEs without the covering desc write lock.  The
 * MAP_FIXED overlap removal inside mmap_region() bypasses this funnel
 * entirely -- it has its own backstop in __mmap_prepare(), and the D-G''
 * punch route keeps live arena state out of its reach
 * (r05 dg2-analysis.md D1).  RELEASE clears the flag before its own
 * do_munmap(), so teardown still works.
 */
int corten_arena_munmap_vma_guard(struct mm_struct *mm, unsigned long start,
				  unsigned long end)
{
	struct vm_area_struct *vma;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_state))
		return 0;

	VMA_ITERATOR(vmi, mm, start);
	for_each_vma_range(vmi, vma, end) {
		if (vma->vm_flags & VM_CORTEN)
			return -EOPNOTSUPP;
	}

	return 0;
}

/*
 * [F-B, D-G''] Punch classification (pure, testable): what a non-markable
 * MAP_FIXED range overlapping one arena must do.  The geometry answers are
 * the munmap ones -- a page-rounded whole-arena overwrite is RELEASE
 * territory (the tail rule applies), a strictly-inside range is a punch,
 * a boundary-crossing range cannot be owned without cross-range VMA
 * surgery and is rejected.  Table-driven in mm/corten_fault_test.c.
 */
enum corten_unmap_class corten_arena_punch_classify(unsigned long start,
						    unsigned long end,
						    unsigned long ar_start,
						    unsigned long ar_end)
{
	enum corten_unmap_class raw;

	raw = corten_arena_unmap_classify(start, end, ar_start, ar_end);
	return corten_arena_release_classify(raw, start, end, ar_start,
					     ar_end);
}

/*
 * Carve [ps, pe) (the punch range clipped to one shadow piece @v) out of
 * @v with explicit __split_vma() calls, and keep the descriptor's cached
 * shadow-VMA pointer pointing only at objects the overlap gather will
 * NOT free (r05 dg2-analysis.md B1: the gather's own splits leave the
 * original first VMA and the last VMA as the *doomed* pieces --
 * __split_vma(new_below=1) at the range start makes the original the
 * kept-above piece, i.e. doomed; same for the last VMA at the range end
 * -- and remove_vma() frees them.  A cached pointer left on the original
 * dangles the moment mmap_region() runs).
 *
 * The gather frees @doomed; the survivors are the below head piece (the
 * original object when the lower split ran) and the above tail piece.
 * If the cached pointer references the doomed piece (a head punch dooms
 * the original shadow-VMA itself), it is re-pointed at the tail, or
 * cleared -- NULL is the established safe degradation: the fault paths
 * fall back, the chunk/mprotect routes reject, and the F-A tier-2 walk
 * still finds surviving shadow pieces.
 *
 * Timing: transactions that entered before the frame erase finish first
 * (the desc write lock serializes them against the punch's own zap),
 * the zap runs under the same locks, and the gather's free comes last
 * under the caller's mmap_write -- so no observer can dereference a
 * cached pointer at a freed object in between.  A surgery failure is
 * reported to the route (the mmap fails, the gather never runs, nothing
 * is freed, and RELEASE's piece-wise teardown cleans any extra piece
 * up), so no failure path leaves a dangling cache either.
 *
 * Called under the mmap write lock with @ar pinned; @ps/@pe are clipped
 * to @v by the caller.
 *
 * Return: 0 on success, -errno otherwise.
 */
static int corten_arena_punch_split(struct mm_struct *mm,
				    struct corten_arena *ar,
				    struct vm_area_struct *v,
				    unsigned long ps, unsigned long pe)
{
	struct vm_area_struct *doomed = v, *tail = NULL;
	int ret;

	/* Lower split (new = the above piece): the original object stays
	 * in the tree as the head piece, so a cached ar->vma pointing at
	 * the pre-split shadow survives the gather.
	 */
	if (ps > v->vm_start) {
		VMA_ITERATOR(vmi, mm, ps);

		ret = __split_vma(&vmi, v, ps, /* new_below = */ 0);
		if (ret)
			return ret;
		doomed = vma_lookup(mm, ps);
		if (WARN_ON_ONCE(!doomed))
			return -EIO;
	}

	/* Upper split (new = the above tail piece): the doomed middle is
	 * now exactly [ps, pe) and the gather removes it without touching
	 * the tail.  Done here rather than left to the gather so the
	 * head-punch case can re-point ar->vma at the surviving tail
	 * before any fault can observe the teardown.
	 */
	if (pe < doomed->vm_end) {
		VMA_ITERATOR(vmi, mm, pe);

		ret = __split_vma(&vmi, doomed, pe, /* new_below = */ 0);
		if (ret)
			return ret;
		tail = vma_lookup(mm, pe);
		if (WARN_ON_ONCE(!tail))
			return -EIO;
	}

	if (READ_ONCE(ar->vma) == doomed)
		WRITE_ONCE(ar->vma, tail);

	return 0;
}

/*
 * Drop the arena's claim on [start, end) so the legacy mmap_region() that
 * follows can install the incoming mapping over it (r05 dg2-analysis.md
 * D1: today the overlap removal inside __mmap_prepare() silently punches
 * the shadow-VMA without touching the arena's bookkeeping -- the frames
 * keep routing the hole's faults into the arena and the JVM CDS
 * relocation read dies on the stale FRESH gate).
 *
 * Caller contract: runs under this mm's mmap_write lock (do_mmap's
 * contract, the DEV-13 outermost lock) with @ar pinned by an active
 * reference.
 *
 *   1. Erase the hole's frames first -- the RELEASE order.  From here the
 *      address-keyed hooks (fault/mprotect/munmap routes) miss inside the
 *      hole, so the range is self-consistently legacy for every later
 *      space operation, and the F-A transaction seal keeps an in-flight
 *      fault from committing a fresh page into the hole after the erase.
 *      The erases serialize against DECLARE/RELEASE/mm_exit through the
 *      caller's mmap_write; the xarray's internal lock covers the
 *      single-word store.  An already-NULL frame is a re-punch of a
 *      previously punched window (overlapping MAP_FIXEDs), not an error.
 *   2. Zap the arena-tracked content transactionally (the chunk zap drives
 *      by PTE content, so it also drops whatever a legacy fallback or a
 *      previous punch's mapping left in the range -- the r03 defect-C
 *      lesson).  The un-split shadow-VMA is still the rmap/TLB anchor
 *      here, which is what the pages were mapped under.
 *   3. Split surgery (corten_arena_punch_split): pre-split every shadow
 *      piece the range intersects so the doomed middle is exactly
 *      [start, end) and the descriptor's cached pointer survives the
 *      gather's remove_vma() (B1).
 *
 * The gather below us then has nothing of ours to touch: it removes the
 * doomed middle and installs the mapping; ar->vma references only
 * surviving pieces.
 *
 * Return: 0 on success (legacy installs the mapping), -errno on failure
 * (the mmap fails; the hole stays arena-blind -- later accesses fall to
 * the legacy funnel on the still-reserved shadow pieces, which matches
 * the plain-Linux behaviour of a failed mapping over a reservation).
 */
static int corten_arena_mmap_punch(struct mm_struct *mm,
				   struct corten_mm_state *state,
				   struct corten_arena *ar,
				   unsigned long start, unsigned long end)
{
	unsigned long frame, last = (end - 1) >> PMD_SHIFT;
	unsigned long a;
	int ret;

	for (frame = start >> PMD_SHIFT; frame <= last; frame++) {
		struct corten_arena *stale = xa_erase(&state->arenas, frame);

		if (WARN_ON_ONCE(stale && stale != ar))
			return -EIO;
	}

	ret = corten_arena_unmap_chunk_retry(mm, ar, start, end - start);
	if (ret)
		return ret;

	/* Split every shadow piece the range intersects.  Non-shadow VMAs
	 * in the range (file mappings of earlier punches) are the gather's
	 * own legacy business -- nothing in the descriptor references
	 * them.  a always advances past the processed piece by its
	 * pre-surgery vm_end: the splits only carve inside [a, vend).
	 */
	for (a = start; a < end;) {
		struct vm_area_struct *v = find_vma_intersection(mm, a, a + 1);
		unsigned long vend;

		if (!v || v->vm_start >= end)
			break;
		vend = v->vm_end;
		if (v->vm_flags & VM_CORTEN) {
			ret = corten_arena_punch_split(mm, ar, v,
						       max(v->vm_start, a),
						       min(vend, end));
			if (ret)
				return ret;
		}
		a = vend;
	}

	atomic_long_inc(&corten_nr_mmap_punches);
	return 0;
}

/*
 * Routing for every MAP_FIXED shape corten_arena_mmap_classify() does not
 * mark (file-backed, shared, hugetlb, ...).  A range that overlaps arena
 * state must have its overlap removed through the arena's own teardown
 * (frames + transactional zap, RELEASE for the whole-arena shape) instead
 * of the unguarded legacy gather -- that is the D1 half of the D-G''
 * root cause.  Ranges the arena cannot own (boundary crossing, two
 * arenas) are cleanly rejected, same verdict as the munmap route.
 * Runs under mmap_write (do_mmap's contract).
 * Return: 0 = legacy, -errno = reject.
 */
static int corten_arena_mmap_punch_route(struct mm_struct *mm,
					 unsigned long addr, unsigned long len,
					 unsigned long flags)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);
	struct corten_arena *ar_start, *ar_end, *ar;
	enum corten_unmap_class class;
	unsigned long end = addr + len;
	int ret;

	/* Plain mmaps cannot overlap an arena (__get_unmapped_area() only
	 * hands out gaps), and MAP_FIXED_NOREPLACE already returned -EEXIST
	 * on the shadow-VMA at the do_mmap gate -- so only a real MAP_FIXED
	 * overwrite gets here.
	 */
	if (!(flags & MAP_FIXED) || (flags & MAP_FIXED_NOREPLACE))
		return 0;

	ar_start = corten_arena_lookup_get(mm, addr);
	ar_end = corten_arena_lookup_get(mm, end - 1);
	if (!ar_start && !ar_end)
		return 0;		/* no arena involved */

	ar = ar_start ?: ar_end;
	class = corten_arena_punch_classify(addr, end, ar->start, ar->end);
	if (class != CORTEN_UNMAP_CHUNK && class != CORTEN_UNMAP_EXACT) {
		if (ar_start)
			percpu_ref_put(&ar_start->active);
		if (ar_end)
			percpu_ref_put(&ar_end->active);
		atomic_long_inc(&corten_nr_mmap_punch_rejects);
		return -EOPNOTSUPP;
	}

	if (class == CORTEN_UNMAP_EXACT) {
		/* The whole arena is overwritten: RELEASE semantics (drain
		 * + legacy teardown removes the shadow-VMA), then the
		 * legacy funnel installs the mapping into the emptied
		 * range.  Drop the pinned references first -- the drain
		 * inside RELEASE must reach zero.  release_locked() takes
		 * only the ctl_lock: the caller's mmap_write is already
		 * the DEV-13 outermost acquisition and the semaphore is
		 * not recursive.
		 */
		if (ar_start)
			percpu_ref_put(&ar_start->active);
		if (ar_end)
			percpu_ref_put(&ar_end->active);
		ret = corten_arena_release_locked(mm, state, ar->start,
						  ar->end - ar->start);
		if (!ret)
			atomic_long_inc(&corten_nr_mmap_punches);
		return ret < 0 ? ret : 0;
	}

	/* CHUNK: punch the hole, keep the rest of the arena. */
	ret = corten_arena_mmap_punch(mm, state, ar, addr, end);
	if (ar_start)
		percpu_ref_put(&ar_start->active);
	if (ar_end)
		percpu_ref_put(&ar_end->active);

	return ret < 0 ? ret : 0;
}

/*
 * mmap routing classification (sec 5.6, [P1-4] gate, pure): only
 * MAP_FIXED private-anonymous (no hugetlb, no growsdown, no populate or
 * locked) mappings are arena-markable.  Everything else -- including
 * MAP_FIXED_NOREPLACE, which the uapi forbids from silently overwriting
 * -- stays legacy (NOREPLACE intersecting an arena naturally returns
 * -EEXIST via find_vma_intersection() on the shadow-VMA).
 */
enum corten_mmap_class corten_arena_mmap_classify(unsigned long flags,
						  bool file)
{
	if (!(flags & MAP_FIXED) || (flags & MAP_FIXED_NOREPLACE))
		return CORTEN_MMAP_LEGACY;
	if (file || !(flags & MAP_ANONYMOUS))
		return CORTEN_MMAP_LEGACY;
	if ((flags & MAP_TYPE) != MAP_PRIVATE)
		return CORTEN_MMAP_LEGACY;
	if (flags & (MAP_HUGETLB | MAP_GROWSDOWN | MAP_POPULATE | MAP_LOCKED))
		return CORTEN_MMAP_LEGACY;

	return CORTEN_MMAP_MARK;
}

/*
 * mmap MAP_FIXED into an arena (sec 5.6, paper Fig.8 L1-7): mark the
 * range as a fresh PRIVATE_ANON allocation; pages already recorded in
 * the range are first dropped (legacy MAP_FIXED discards previous
 * mappings) through the same transactional zap as the chunk unmap.
 * Runs under mmap_write_lock (do_mmap's contract); no VMA is created,
 * split or merged, no vm_stat_account() runs (the shadow-VMA already
 * carries total_vm).
 * Every other MAP_FIXED shape goes through the punch route, so the
 * overlap removal never reaches the legacy funnel with live arena state
 * (r05 dg2-analysis.md D1, the D-G'' root cause).
 * Return: 0 = legacy, 1 = marked, -errno = reject.
 */
int corten_arena_mmap_route(struct mm_struct *mm, unsigned long addr,
			    unsigned long len, unsigned long prot,
			    unsigned long flags, bool file)
{
	struct corten_arena *ar;
	enum corten_unmap_class class;
	struct corten_pte_meta meta = { };
	struct mmu_gather tlb;
	struct vm_area_struct *vma;
	unsigned long a, end = addr + len, start = addr;
	u8 perm = CORTEN_PERM_USER;
	int ret = 0;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_state))
		return 0;
	if (corten_arena_mmap_classify(flags, file) != CORTEN_MMAP_MARK)
		return corten_arena_mmap_punch_route(mm, addr, len, flags);

	ar = corten_arena_lookup_get(mm, addr);
	if (!ar)
		return 0;

	class = corten_arena_unmap_classify(start, end, ar->start, ar->end);
	if (class != CORTEN_UNMAP_CHUNK && class != CORTEN_UNMAP_EXACT) {
		percpu_ref_put(&ar->active);
		return -EOPNOTSUPP;
	}

	if (prot & PROT_READ)
		perm |= CORTEN_PERM_READ;
	if (prot & PROT_WRITE)
		perm |= CORTEN_PERM_WRITE;
	if (prot & PROT_EXEC)
		perm |= CORTEN_PERM_EXEC;

	vma = corten_arena_shadow_vma(ar);
	if (!vma) {
		percpu_ref_put(&ar->active);
		return -EOPNOTSUPP;
	}

	meta.state = CORTEN_PRIVATE_ANON;
	meta.perm = perm;
	meta.flags = 0;

	/* Make every window's tracked PT page exist up front: corten_mark()
	 * records into the PT page's metadata array, so a hole here would
	 * fail the transaction with -ENOENT.  Cold: only windows never
	 * touched since DECLARE allocate.
	 */
	for (a = addr; a < end; a = min((a | (PMD_SIZE - 1)) + 1, end)) {
		ret = corten_arena_fill_upper(ar, a);
		if (ret) {
			/* [C3b] pass the real errno through (-ENOMEM vs
			 * -EOPNOTSUPP for a huge leaf).
			 */
			percpu_ref_put(&ar->active);
			return ret;
		}
	}

	tlb_gather_mmu(&tlb, mm);

	while (start < end) {
		unsigned long win_end = min((start | (PMD_SIZE - 1)) + 1, end);
		struct corten_txn txn;
		int tries = 0;

		for (;;) {
			ret = corten_lock_range(mm, start, win_end - start,
						&txn);
			if (ret != -EAGAIN || ++tries >= 2)
				break;
		}

		switch (ret) {
		case 0:
			/* Discard whatever the range held (legacy MAP_FIXED
			 * semantics), then mark the fresh allocation.
			 */
			ret = corten_arena_zap_window(mm, vma, &txn, start,
						      win_end, &tlb);
			if (ret) {
				corten_unlock(&txn);
				goto out;
			}
			ret = corten_mark(&txn, start, win_end - start, &meta);
			corten_unlock(&txn);
			if (ret)
				goto out;
			this_cpu_inc(READ_ONCE(mm->corten_state)->stats[
					CORTEN_ARENA_STAT_MMAP_MARK_TXNS]);
			break;
		case -ENOENT:
			/* Cannot happen after the fill pass above. */
			WARN_ON_ONCE(1);
			ret = -EOPNOTSUPP;
			fallthrough;
		default:
			goto out;
		}

		start = win_end;
	}

out:
	tlb_finish_mmu(&tlb);
	percpu_ref_put(&ar->active);

	return ret ? ret : 1;
}

/* ------------------------------------------------------------------ *
 * T0b: mprotect routing (M4T0_SPEC.md sec 3.3).  A permission change
 * inside one arena is a corten_mark() transaction: the recorded perm of
 * every page in the range moves to the requested protection, and
 * already-installed PTEs are rewritten to the matching encoding under
 * the covering desc write lock (R2).  Unlike the install-from-none
 * fault paths, rewriting a live translation requires a TLB flush (a
 * downgraded write bit must not survive cached in the TLB), so the walk
 * ends with one flush_tlb_range() over exactly the touched span.
 *
 * The entry point runs from do_mprotect_pkey() with this mm's
 * mmap_write lock held -- the same DEV-13 nesting the chunk zap uses.
 * ------------------------------------------------------------------
 */

/*
 * Rewrite permissions of [start, end) (at most one PMD window; the
 * caller holds the covering desc write lock through @txn) and the
 * recorded metadata behind them.  PTE changes accumulate in
 * *@flush_start/@flush_end so the driver can issue a single flush.
 */
static int corten_arena_protect_window(struct mm_struct *mm,
				       struct vm_area_struct *vma,
				       struct corten_txn *txn,
				       unsigned long start, unsigned long end,
				       pgprot_t new_pgprot, u8 perm,
				       unsigned long *flush_start,
				       unsigned long *flush_end, bool *flushed)
{
	unsigned long addr;
	pmd_t *pmdp;
	int ret = 0;

	pmdp = corten_arena_pmd(mm, start);
	if (WARN_ON_ONCE(!pmdp))
		return -EAGAIN;

	for (addr = start; addr < end; addr += PAGE_SIZE) {
		struct corten_pte_meta m, nm;
		pte_t *ptep, cur, newpte;
		/* ptl nests below the covering desc write lock (R2), the
		 * same exclusion set the chunk-zap walk uses.
		 */
		spinlock_t *ptl;
		bool recorded;

		/* Metadata first: a permission update is a mark with the
		 * same state (corten.h).  An unmarked (CORTEN_INVALID)
		 * page is recorded as a fresh PrivateAnon allocation with
		 * the new perm -- the spec's "pending perm": the FRESH
		 * fault gate must not resurrect the pre-mprotect arena
		 * upper bound for pages nothing has faulted in yet.
		 * STUB-family states (no M3 producer) are left alone.
		 */
		recorded = corten_query(txn, addr, &m) == 0;
		if (recorded && (m.state == CORTEN_INVALID ||
				 m.state == CORTEN_PRIVATE_ANON ||
				 m.state == CORTEN_MAPPED)) {
			nm = m;
			if (nm.state == CORTEN_INVALID)
				nm.state = CORTEN_PRIVATE_ANON;
			nm.perm = perm;
			/* M5: a shared page whose contract gains WRITE
			 * must carry the WRITABLE record (corten_mark's
			 * rule).  The hardware stays read-only until the
			 * COW transaction clears SHARED -- this only
			 * records that the write fault is the wrprotect
			 * artifact shape (COW_MAYBE), not a genuine one.
			 */
			if ((nm.perm & CORTEN_PERM_WRITE) &&
			    (nm.flags & CORTEN_PF_SHARED))
				nm.flags |= CORTEN_PF_WRITABLE;
			if (nm.state != m.state || nm.perm != m.perm ||
			    nm.flags != m.flags) {
				ret = corten_mark(txn, addr, PAGE_SIZE, &nm);
				if (WARN_ON_ONCE(ret))
					return ret;
			}
		}

		ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
		if (!ptep)
			return -EAGAIN;
		cur = ptep_get(ptep);
		if (pte_present(cur)) {
			if (pte_special(cur)) {
				/* The shared zero page must never become
				 * writable: drop the translation and let
				 * the next fault install a real page
				 * (map_anon's zero-upgrade path).  A
				 * read-only new prot leaves it working.
				 */
				if (perm & CORTEN_PERM_WRITE) {
					ptep_get_and_clear(mm, addr, ptep);
					goto flush_this;
				}
				pte_unmap_unlock(ptep, ptl);
				continue;
			}
			newpte = ptep_modify_prot_start(vma, addr, ptep);
			newpte = pte_modify(newpte, new_pgprot);
			/* Preserve write on already-writable pages (no
			 * spurious refault); a downgrade clears the write
			 * bit through the pgprot, an upgrade on a
			 * read-only page self-heals through the fault
			 * path's CORTEN_DISP_RESTORE (mkwrite+dirty).
			 */
			if ((perm & CORTEN_PERM_WRITE) && pte_write(cur))
				newpte = pte_mkwrite(newpte, vma);
			ptep_modify_prot_commit(vma, addr, ptep, cur, newpte);
flush_this:
			if (!*flushed) {
				*flush_start = addr;
				*flushed = true;
			}
			*flush_end = addr + PAGE_SIZE;
		}
		/* !pte_present: no swap entries exist in M3; nothing to
		 * re-protect.
		 */
		pte_unmap_unlock(ptep, ptl);
	}

	return ret;
}

/*
 * Window driver: one covering desc write lock per 2M window, then the
 * PTE/metadata rewrite; a single TLB flush and (for the whole-arena
 * case) the arena upper-bound + shadow-VMA flag update close it.
 */
static int corten_arena_protect_range(struct mm_struct *mm,
				      struct corten_arena *ar,
				      struct vm_area_struct *vma,
				      unsigned long start, unsigned long end,
				      u8 perm, bool whole)
{
	struct mmu_notifier_range range;
	unsigned long flush_start = 0, flush_end = 0;
	unsigned long addr = start;
	pgprot_t new_pgprot;
	bool flushed = false;
	int ret = 0;

	new_pgprot = corten_arena_perm_pgprot(vma, perm);

	/* Secondary-MMU parities: the legacy funnel wraps
	 * change_protection() in notifier invalidation; the arena walk
	 * writes the same PTEs and owes the same courtesy.
	 */
	mmu_notifier_range_init(&range, MMU_NOTIFY_PROTECTION_VMA, 0,
				vma->vm_mm, start, end);
	mmu_notifier_invalidate_range_start(&range);

	while (addr < end) {
		unsigned long win_end = min((addr | (PMD_SIZE - 1)) + 1, end);
		struct corten_txn txn;
		int tries = 0;

		for (;;) {
			ret = corten_lock_range(mm, addr, win_end - addr,
						&txn);
			if (ret != -EAGAIN || ++tries >= 2)
				break;
		}

		switch (ret) {
		case 0:
			ret = corten_arena_protect_window(mm, vma, &txn, addr,
							  win_end, new_pgprot,
							  perm, &flush_start,
							  &flush_end,
							  &flushed);
			corten_unlock(&txn);
			if (ret)
				goto out;
			break;
		case -ENOENT:
		case -EOPNOTSUPP: {
			int arm;

			for (arm = 0; arm < 2; arm++) {
				ret = corten_arena_fill_upper(ar, addr);
				if (ret)
					break;
				ret = corten_lock_range(mm, addr,
							win_end - addr, &txn);
				if (ret != -ENOENT && ret != -EOPNOTSUPP)
					break;
			}
			if (ret == -ENOMEM)
				goto out;
			if (ret) {
				/* -ENOENT after both arms, or a huge leaf:
				 * the window cannot be tracked.  Keep the
				 * legacy self-heal contract, counted.
				 */
				atomic_long_inc(&corten_nr_rearm_failed);
				ret = 0;
				break;
			}
			atomic_long_inc(&corten_nr_rearm_recovered);
			ret = corten_arena_protect_window(mm, vma, &txn, addr,
							  win_end, new_pgprot,
							  perm, &flush_start,
							  &flush_end,
							  &flushed);
			corten_unlock(&txn);
			if (ret)
				goto out;
			break;
		}
		default:
			goto out;
		}

		addr = win_end;
	}

out:
	/* Flush even on error: earlier windows may already have rewritten
	 * live translations.
	 */
	if (flushed)
		flush_tlb_range(vma, flush_start, flush_end);
	mmu_notifier_invalidate_range_end(&range);

	if (ret)
		return ret;

	if (whole) {
		/* The whole arena changed protection: move the upper
		 * bound the FRESH fault gate reads (lockless, under the
		 * active-ref barrier) and re-encode the shadow-VMA flags
		 * for every non-PTE consumer (proc/smaps, mprotect's own
		 * future legacy calls on other ranges, munmap release
		 * validation).
		 */
		vm_flags_t flags = vma->vm_flags &
				   ~(VM_READ | VM_WRITE | VM_EXEC);

		WRITE_ONCE(ar->prot, perm);
		if (perm & CORTEN_PERM_READ)
			flags |= VM_READ;
		if (perm & CORTEN_PERM_WRITE)
			flags |= VM_WRITE;
		if (perm & CORTEN_PERM_EXEC)
			flags |= VM_EXEC;
		/* vm_flags_set() only ORs bits in: a downshift must clear
		 * the R/W/X bits the old encoding carried before the
		 * re-assignment, or stale permissions outlive the call.
		 */
		vm_flags_clear(vma, VM_READ | VM_WRITE | VM_EXEC);
		vm_flags_set(vma, flags | VM_SOFTDIRTY);
		WRITE_ONCE(vma->vm_page_prot, new_pgprot);
	}
	/* A chunk route deliberately leaves the shadow-VMA R/W/X flags
	 * at the DECLARE bound: fork demotion's materialize walk uses
	 * them as the unrecorded-page baseline (corten_arena_demote_
	 * apply_run's "already encoded" fast path).  Kernel-side access
	 * to routed pages is opened by the gates instead (r06 gupfix:
	 * GUP check_vma_flags() and the arch kernel-mode access_error()
	 * consult VM_CORTEN and defer to the arena metadata).
	 */
	return 0;
}

/*
 * do_mprotect_pkey() entry routing (sec 3.3).  Runs with this mm's
 * mmap_write lock held (the caller took it before the hook).
 * Return: 0 = run the legacy mprotect (no arena in range), 1 = routed,
 * -errno = reject (boundary crossing, out-of-scope combination, or a
 * MODE-targeted arena keeping the S6 behaviour).
 */
int corten_arena_mprotect_route(struct mm_struct *mm, unsigned long start,
				unsigned long len, unsigned long prot,
				int pkey)
{
	struct corten_arena *ar_start, *ar_end, *ar;
	enum corten_unmap_class class;
	struct vm_area_struct *vma;
	unsigned long end = start + len;
	bool whole;
	u8 perm;
	int ret = 0;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_state))
		return 0;
	if (!len || (len & ~PAGE_MASK) || end <= start)
		return 0;

	ar_start = corten_arena_lookup_get(mm, start);
	ar_end = corten_arena_lookup_get(mm, end - 1);
	ar = ar_start ?: ar_end;

	if (!ar)
		return 0;			/* not ours: legacy */

	if (ar_start != ar_end) {
		ret = -EOPNOTSUPP;		/* crosses an arena boundary */
		goto out;
	}

	class = corten_arena_unmap_classify(start, end, ar->start, ar->end);
	if (class != CORTEN_UNMAP_CHUNK && class != CORTEN_UNMAP_EXACT) {
		/* PARTIAL: the range reaches past the arena boundary
		 * (sec 3.3 keeps the S6 verdict: window isolation means a
		 * sane allocator never allocates across the boundary).
		 */
		ret = -EOPNOTSUPP;
		goto out;
	}

	/* MODE matrix (sec 3.5): a MODE-targeted arena (mode=0) keeps the
	 * S6 reject; only MODE-process takeover routes permissions.
	 */
	if (!READ_ONCE(mm->corten_mode)) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	/* Out-of-scope combinations stay rejects: pkey-mprotect has no
	 * metadata encoding, and PROT_GROWSDOWN/GROWSUP/SEM (bits outside
	 * RWX) have no arena meaning -- the legacy validation chain owns
	 * them.
	 */
	if (pkey != -1 || (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	vma = corten_arena_shadow_vma(ar);
	if (!vma) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	perm = CORTEN_PERM_USER;
	if (prot & PROT_READ)
		perm |= CORTEN_PERM_READ;
	if (prot & PROT_WRITE)
		perm |= CORTEN_PERM_WRITE;
	if (prot & PROT_EXEC)
		perm |= CORTEN_PERM_EXEC;

	whole = (class == CORTEN_UNMAP_EXACT);
	ret = corten_arena_protect_range(mm, ar, vma, start, end, perm, whole);
	if (ret == -EAGAIN) {
		/* C1: same transient PT-retirement race as the chunk
		 * path -- one retry beats leaking -EAGAIN past
		 * mprotect(2), where libc gives up for good.
		 */
		atomic_long_inc(&corten_nr_eagain_retries);
		ret = corten_arena_protect_range(mm, ar, vma, start, end,
						 perm, whole);
	}
	if (!ret) {
		atomic_long_inc(&corten_nr_mprotect_routes);
		ret = 1;
	} else if (ret == -EAGAIN) {
		atomic_long_inc(&corten_nr_eagain_leaked);
	}

out:
	if (ar_start)
		percpu_ref_put(&ar_start->active);
	if (ar_end)
		percpu_ref_put(&ar_end->active);

	return ret;
}

/* ------------------------------------------------------------------ *
 * T0b: mremap routing (M4T0_SPEC.md sec 8 T0-R1, STATE D12/OQ-A).
 * glibc's mmrealloc drives large reallocs through mremap(): a plain
 * -EOPNOTSUPP breaks every compiled program that grows an mmap'd
 * block.  Two routed shapes cover the allocator surface:
 *
 *   - shrink (new <= old): in place, same address back -- the tail
 *     [addr+new, addr+old) is dropped through the chunk-zap
 *     transaction.  DEVIATION (documented): the tail reads as fresh
 *     zero afterwards instead of SIGSEGV, because the arena keeps the
 *     VA reservation (Fig.8 L9-13 semantics).
 *   - grow: the old range cannot grow inside its arena (no shadow-VMA
 *     surgery), so the content is kernel-copied to a fresh window
 *     arena (declare + copy_to_user + RELEASE of the old arena).  The
 *     copy runs with no locks held -- faults on both sides are served
 *     by the arena paths; the source may not be concurrently written
 *     during the move (same contract legacy mremap assumes).
 *
 * Everything else on an arena (MREMAP_FIXED with an explicit target,
 * MREMAP_DONTUNMAP, boundary crossing, no-MAYMOVE grows) stays a
 * counted -EOPNOTSUPP: move_ptes() would need both descriptors' write
 * locks (unordered), and the shadow-VMA has no split story yet.
 * ------------------------------------------------------------------
 */

/*
 * The grow path: fresh window arena + kernel copy.  Runs with no locks
 * held and returns the new address, or -errno with the old arena
 * untouched (the copy never destroys the source on failure).
 */
static long corten_arena_mremap_move(struct mm_struct *mm,
				     struct corten_arena *ar,
				     unsigned long addr, unsigned long old_len,
				     unsigned long new_len)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);
	unsigned long len2, addr2, ncopy, populate = 0;
	unsigned long prot;
	LIST_HEAD(uf);
	long ret;
	u8 perm;

	/* OVERCOMMIT_NEVER does not honour MAP_NORESERVE, so the new VMA
	 * would carry VM_ACCOUNT and fail the DECLARE validation
	 * wholesale (DEV-12, same posture as the auto-mmap route).
	 */
	if (sysctl_overcommit_memory == OVERCOMMIT_NEVER)
		return -EOPNOTSUPP;

	perm = READ_ONCE(ar->prot);
	prot = corten_arena_perm_to_prot(perm);
	len2 = round_up(new_len, PMD_SIZE);

	mmap_write_lock(mm);
	ret = corten_arena_window_place(mm, state, len2, &addr2);
	if (ret) {
		mmap_write_unlock(mm);
		/* Window exhausted (T0-R2): the legacy funnel cannot run
		 * over an arena either (the guard would reject), so the
		 * honest answer is the arena reject, counted by the
		 * caller.
		 */
		return -EOPNOTSUPP;
	}

	/* A plain private anonymous NORESERVE VMA at the fresh slot:
	 * do_mmap's contract is "mmap_write already held" (vm_mmap_pgoff
	 * precedent).  The auto-mmap route does not fire (addr != 0) and
	 * the MAP_FIXED mark route finds no arena at the fresh slot, so
	 * the plain legacy body creates the VMA.
	 */
	ret = do_mmap(NULL, addr2, len2, prot,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
		      0, 0, &populate, &uf);
	if (IS_ERR_VALUE(ret)) {
		mmap_write_unlock(mm);
		return -ENOMEM;
	}

	/* DECLARE the new arena -- the auto-attach body (write lock
	 * held).  Failure unwinds to the plain VMA, which we remove.
	 */
	ret = corten_arena_declare_locked(mm, state, addr2, len2);
	if (ret) {
		do_munmap(mm, addr2, len2, NULL);
		mmap_write_unlock(mm);
		return -ENOMEM;
	}
	mmap_write_unlock(mm);

	/* Kernel copy, no locks held: dest faults are served by the new
	 * arena's FRESH path (upper bound = the old arena's recorded
	 * perm), source faults by the old arena.  A partial copy means a
	 * source page is not readable at its recorded permission (e.g.
	 * content mprotect'ed down to PROT_NONE): undo the new arena --
	 * the old one and its data are untouched.
	 */
	ncopy = min(old_len, new_len);
	if (copy_to_user((void __user *)addr2, (void __user *)addr, ncopy)) {
		corten_arena_release(mm, addr2, len2);
		return -EFAULT;
	}

	return addr2;
}

/*
 * sys_mremap() entry routing (sec 3.5, D12).  Runs with no locks held
 * (before do_mremap takes the write lock); prechecks have NOT run yet,
 * so anything questionable is returned to the legacy funnel for its
 * documented errno.
 * Return: 0 = run the legacy mremap, >0 = routed (the new address),
 * -errno = reject.
 */
long corten_arena_mremap_route(struct mm_struct *mm, unsigned long addr,
			       unsigned long old_len, unsigned long new_len,
			       unsigned long flags, unsigned long new_addr)
{
	struct corten_arena *ar_start, *ar_end, *ar;
	enum corten_unmap_class class;
	unsigned long old_len_p, new_len_p, end;
	unsigned long old_start, old_len2;
	long ret;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_state))
		return 0;
	if (!addr || (addr & ~PAGE_MASK) || !old_len || !new_len)
		return 0;
	if (flags & ~(MREMAP_FIXED | MREMAP_MAYMOVE | MREMAP_DONTUNMAP))
		return 0;
	old_len_p = PAGE_ALIGN(old_len);
	new_len_p = PAGE_ALIGN(new_len);
	if (!old_len_p || old_len_p < old_len ||
	    !new_len_p || new_len_p < new_len)
		return 0;			/* overflow: legacy errno */
	end = addr + old_len_p;
	if (end <= addr)
		return 0;

	/* MREMAP_FIXED onto an arena stays a reject whether or not the
	 * old range is in one (the S6 hook's second check).
	 */
	if ((flags & MREMAP_FIXED) &&
	    corten_arena_range_overlaps(mm, new_addr, new_len_p)) {
		atomic_long_inc(&corten_nr_mremap_rejects);
		return -EOPNOTSUPP;
	}

	ar_start = corten_arena_lookup_get(mm, addr);
	ar_end = corten_arena_lookup_get(mm, end - 1);
	ar = ar_start ?: ar_end;

	if (!ar)
		return 0;			/* legacy mremap */

	if (ar_start != ar_end) {
		ret = -EOPNOTSUPP;		/* boundary crossing */
		goto out_reject;
	}

	class = corten_arena_unmap_classify(addr, end, ar->start, ar->end);
	if (class != CORTEN_UNMAP_CHUNK && class != CORTEN_UNMAP_EXACT) {
		ret = -EOPNOTSUPP;
		goto out_reject;
	}

	/* MODE matrix (sec 3.5): MODE-targeted arenas keep the S6
	 * reject; only MODE-process takeover routes.
	 */
	if (!READ_ONCE(mm->corten_mode)) {
		ret = -EOPNOTSUPP;
		goto out_reject;
	}

	if (flags & MREMAP_DONTUNMAP) {
		/* VA donation with page accounting has no arena
		 * transaction (M5+).
		 */
		ret = -EOPNOTSUPP;
		goto out_reject;
	}

	if (flags & MREMAP_FIXED) {
		/* Explicit target: move_ptes() would need both covering
		 * descriptors' write locks at once (no order between
		 * them -- deadlock surface).
		 */
		ret = -EOPNOTSUPP;
		goto out_reject;
	}

	if (new_len_p <= old_len_p) {
		/* In-place shrink: same address back (glibc's mmrealloc
		 * shrink leg passes no MAYMOVE and needs exactly that).
		 * The tail content is dropped through the chunk-zap
		 * transaction; the tail then reads as fresh zero (see
		 * the section comment).
		 */
		if (new_len_p < old_len_p) {
			int r = corten_arena_unmap_chunk_retry(mm, ar,
							       addr + new_len_p,
							       old_len_p - new_len_p);

			if (r) {
				ret = r;
				goto out_reject;
			}
		}
		atomic_long_inc(&corten_nr_mremap_routes);
		if (ar_start)
			percpu_ref_put(&ar_start->active);
		if (ar_end)
			percpu_ref_put(&ar_end->active);
		return addr;
	}

	/* Grow: a move is mandatory.  Without MAYMOVE the caller demanded
	 * the same address -- the legacy funnel would answer -ENOMEM;
	 * -EOPNOTSUPP marks the arena reason and is counted.
	 */
	if (!(flags & MREMAP_MAYMOVE)) {
		ret = -EOPNOTSUPP;
		goto out_reject;
	}

	ret = corten_arena_mremap_move(mm, ar, addr, old_len_p, new_len_p);
	if (ret < 0)
		goto out_reject;

	/* Success: drop the lookup references so the drain can reach zero,
	 * then retire the old arena (the munmap_route EXACT precedent).
	 * The old range is captured before the puts: a concurrent RELEASE
	 * may free the descriptor once the references are gone.  A
	 * RELEASE failure here is memory pressure: the old range
	 * degrades to a plain anonymous VMA with its content intact, the
	 * new arena keeps the copy -- the caller sees the errno and keeps
	 * using the old block.
	 */
	old_start = ar->start;
	old_len2 = ar->end - ar->start;
	if (ar_start)
		percpu_ref_put(&ar_start->active);
	if (ar_end)
		percpu_ref_put(&ar_end->active);
	{
		int r = corten_arena_release(mm, old_start, old_len2);

		if (r) {
			/* T0b review C2: the old arena's RELEASE failed
			 * (memory pressure).  Roll the fresh window
			 * back so the caller sees only the error and
			 * keeps using the old block, whose content the
			 * RELEASE-failure contract leaves intact.  The
			 * rollback is best-effort, like the copy-fault
			 * unwind in mremap_move(); we cannot hop to
			 * out_reject -- the lookup refs are already put.
			 */
			atomic_long_inc(&corten_nr_mremap_release_fail);
			corten_arena_release(mm, ret,
					     round_up(new_len_p, PMD_SIZE));
			return r;
		}
	}
	atomic_long_inc(&corten_nr_mremap_routes);

	return ret;

out_reject:
	atomic_long_inc(&corten_nr_mremap_rejects);
	if (ar_start)
		percpu_ref_put(&ar_start->active);
	if (ar_end)
		percpu_ref_put(&ar_end->active);
	return ret;
}

/*
 * Lockless overlap test for the reject-family hooks (sec 5.7-5.15): one
 * load for "no arenas at all", then an RCU xarray walk over the 2M
 * frames of the range (sparse: empty spans are skipped at node
 * granularity).
 */
bool corten_arena_range_overlaps(struct mm_struct *mm, unsigned long start,
				 unsigned long len)
{
	struct corten_mm_state *state;
	struct corten_arena *ar;
	unsigned long frame;
	bool ret = false;

	state = READ_ONCE(mm->corten_state);
	if (!corten_enabled_static() || !state || !refcount_read(&state->nr))
		return false;
	if (!len)
		return false;

	rcu_read_lock();
	xa_for_each_range(&state->arenas, frame, ar, start >> PMD_SHIFT,
			  (start + len - 1) >> PMD_SHIFT) {
		if (start < ar->end && start + len > ar->start) {
			ret = true;
			break;
		}
	}
	rcu_read_unlock();

	return ret;
}

/*
 * MADV_DONTNEED routing (sec 5.8): a range strictly inside one arena is
 * the transactional content drop (identical to the chunk unmap);
 * everything else on a shadow-VMA is rejected by the per-VMA check in
 * madvise_vma_behavior().  Return convention: 0 legacy, 1 handled,
 * -errno.
 */
int corten_arena_dontneed_route(struct mm_struct *mm, unsigned long start,
				unsigned long len)
{
	struct corten_arena *ar;
	enum corten_unmap_class class;
	unsigned long end;
	int ret;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_state))
		return 0;
	if (!len || (len & ~PAGE_MASK))
		return 0;
	end = start + len;
	if (end <= start)
		return 0;

	ar = corten_arena_lookup_get(mm, start);
	if (!ar)
		return 0;

	class = corten_arena_unmap_classify(start, end, ar->start, ar->end);
	switch (class) {
	case CORTEN_UNMAP_CHUNK:
	case CORTEN_UNMAP_EXACT:
		/* DONTNEED keeps the VA even for the whole arena, so the
		 * exact range also takes the chunk path.
		 */
		ret = corten_arena_unmap_chunk_retry(mm, ar, start, len);
		break;
	default:
		ret = 0;
		break;
	}
	percpu_ref_put(&ar->active);

	/* Normalize like munmap_route(): unmap_chunk() returns 0 on a
	 * successful zap, but this function's contract (and the
	 * madvise_route counting) is 0 = legacy, 1 = routed.
	 */
	return ret < 0 ? ret : 1;
}

/*
 * Behaviour-level madvise routing (sec 5.8 / M4T0_SPEC.md sec 3.4),
 * called from madvise_do_behavior() under madvise_lock().  Decision
 * table (MODE matrix sec 3.5):
 *
 *   DONTNEED / DONTNEED_LOCKED	transactional content drop (S6, kept)
 *   FREE / FREE_LOCKED		T0: folded into the DONTNEED transaction
 *				(lazy-free permits dropping the content
 *				at any time; an eager drop is a
 *				compliance superset -- counted)
 *   NORMAL/SEQUENTIAL/RANDOM/COLD	T0: pure hints, no-op success in-arena
 *   everything else		reject when the range overlaps an arena
 *
 * MODE-targeted arenas (mode=0) keep the S6 behaviour for the new rows:
 * FREE/hints on them stay rejects, so only MODE-process takeover sees
 * the relaxation.  MADV_POPULATE_* would be safe (it faults through the
 * arena hook) but keeps its M3 reject per the matrix.  process_madvise
 * (OQ-C) flows through this same decision point for its supported
 * behaviours (COLD/PAGEOUT), so a remote hint on a MODE arena is the
 * same counted no-op.
 * Return: 0 = legacy, 1 = handled, -errno = reject.
 */
int corten_arena_madvise_route(struct mm_struct *mm, int behavior,
			       unsigned long start, unsigned long len)
{
	struct corten_arena *ar_start, *ar_end, *ar;
	unsigned long end = start + len;
	bool in_arena;
	int ret;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_state))
		return 0;

	switch (behavior) {
	case MADV_DONTNEED:
	case MADV_DONTNEED_LOCKED:
		return corten_arena_dontneed_route(mm, start, len);

	case MADV_FREE:
		/* MADV_FREE only: there is no MADV_FREE_LOCKED in the uapi
		 * (M4T0_SPEC.md sec 3.4 erratum); the locked variant of the
		 * FREE behaviour does not exist upstream.
		 */
		if (!READ_ONCE(mm->corten_mode))
			break;		/* S6 reject via the overlap test */
		if (!len || (len & ~PAGE_MASK) || end <= start)
			return 0;
		ret = corten_arena_dontneed_route(mm, start, len);
		if (ret == 1)
			atomic_long_inc(&corten_nr_madvise_free_txns);
		return ret;

	case MADV_NORMAL:
	case MADV_SEQUENTIAL:
	case MADV_RANDOM:
	case MADV_COLD:
		/* Pure hints: success is contractual even when the kernel
		 * ignores them, so an in-arena range is a counted no-op.
		 * A range that merely touches an arena (boundary
		 * crossing, two arenas) is rejected like the other
		 * behaviours; fully outside is legacy.
		 */
		if (!READ_ONCE(mm->corten_mode))
			break;
		if (!len || (len & ~PAGE_MASK) || end <= start)
			return 0;
		ar_start = corten_arena_lookup_get(mm, start);
		ar_end = corten_arena_lookup_get(mm, end - 1);
		ar = ar_start ?: ar_end;
		in_arena = ar_start && ar_start == ar_end &&
			   corten_arena_unmap_classify(start, end, ar->start,
						       ar->end) !=
				      CORTEN_UNMAP_PARTIAL;
		if (ar_start)
			percpu_ref_put(&ar_start->active);
		if (ar_end)
			percpu_ref_put(&ar_end->active);
		if (!ar)
			return 0;
		if (!in_arena)
			return -EOPNOTSUPP;
		atomic_long_inc(&corten_nr_madvise_hints);
		return 1;

	default:
		break;
	}

	return corten_arena_range_overlaps(mm, start, len) ? -EOPNOTSUPP : 0;
}

/*
 * hwpoison anchor (sec 5.20): M3 neither implements hwpoison interop nor
 * may it stay silent.  hwpoison_user_mappings() calls this before
 * unmapping; a folio that maps into any shadow-VMA is WARNed once (the
 * page stays mapped; documented limitation, counted for M6).
 */
void corten_arena_hwpoison_check(struct folio *folio)
{
	struct anon_vma_chain *avc;
	struct anon_vma *anon_vma;
	pgoff_t pgoff;

	if (!corten_enabled_static())
		return;
	if (!folio_test_anon(folio) || !folio_mapped(folio))
		return;

	pgoff = folio_pgoff(folio);
	anon_vma = folio_get_anon_vma(folio);
	if (!anon_vma)
		return;

	anon_vma_lock_read(anon_vma);
	anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root, pgoff, pgoff) {
		if (avc->vma->vm_flags & VM_CORTEN) {
			WARN_ONCE(1,
				  "corten: hwpoison on arena page (arena vma %p): unhandled in M3 (M3B_DESIGN sec 5.20)\n",
				  avc->vma);
			break;
		}
	}
	anon_vma_unlock_read(anon_vma);
	put_anon_vma(anon_vma);
}
