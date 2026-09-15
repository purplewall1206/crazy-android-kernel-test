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
 * Concurrency contract (sec 6.1/6.3):
 *
 *   - DECLARE/RELEASE serialize on state->ctl_lock; the fault path only
 *     touches the xarray (RCU) and the arena percpu_ref, never ctl_lock,
 *     which is what makes the drain wait under ctl_lock deadlock-free.
 *   - Lock order: ctl_lock -> mmap_lock -> vma write marks.  Neither
 *     desc->lock nor the PTE lock is ever taken here in this slice.
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
#include <linux/mmap_lock.h>
#include <linux/mutex.h>
#include <linux/oom.h>
#include <linux/perf_event.h>
#include <linux/percpu-refcount.h>
#include <linux/percpu.h>
#include <linux/pgtable.h>
#include <linux/rmap.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/uaccess.h>

#include <asm/tlb.h>

#include <uapi/linux/mman.h>
#include <uapi/linux/prctl.h>

#include "corten.h"		/* corten_meta_ensure_locked() */
#include "corten_arena.h"	/* S4-S7 internal interface */

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

int corten_arena_declare(struct mm_struct *mm, unsigned long addr,
			 unsigned long len)
{
	unsigned long frame, first_frame, last_frame;
	struct corten_mm_state *state;
	struct vm_area_struct *vma;
	struct corten_arena *arena;
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

	arena = kzalloc(sizeof(*arena), GFP_KERNEL_ACCOUNT);
	if (!arena)
		return -ENOMEM;

	arena->start = addr;
	arena->end = addr + len;
	arena->mm = mm;
	mutex_init(&arena->fill_lock);
	init_completion(&arena->drained);
	ret = percpu_ref_init(&arena->active, corten_arena_active_release, 0,
			      GFP_KERNEL);
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

	/* Validation and conversion happen together under the write lock:
	 * the VMA must survive as one piece up to the flag flip anyway, so
	 * a separate read-lock pre-pass would only duplicate the check.
	 * Publish with the shadow-VMA in place before the xarray entries,
	 * so a concurrent S4 lookup never resolves an arena whose VMA is
	 * not yet converted ("arena addresses always have their shadow-VMA",
	 * sec 2.3/4.3).  Nothing can fault through the arena path yet: the
	 * frames only become visible to lookup after the stores below.
	 */
	mmap_write_lock(mm);
	vma = vma_lookup(mm, addr);
	ret = corten_arena_validate_vma(vma, addr, len);
	if (ret)
		goto out_write_unlock;

	/* [C1] the range must be empty (see the checker's comment). */
	ret = corten_arena_check_empty_locked(mm, addr, addr + len);
	if (ret)
		goto out_write_unlock;

	arena->prot = corten_arena_prot_from_vma(vma);
	ret = corten_arena_shadowize(vma);
	if (ret)
		goto out_write_unlock;

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
	mmap_write_unlock(mm);
	mutex_unlock(&state->ctl_lock);

	return 0;

out_unwind:
	while (frame > first_frame)
		xa_erase(&state->arenas, --frame);
	corten_arena_unshadow(arena, vma);
out_write_unlock:
	mmap_write_unlock(mm);
out_free_arena:
	mutex_unlock(&state->ctl_lock);
	percpu_ref_exit(&arena->active);
	mutex_destroy(&arena->fill_lock);
	kfree(arena);
	return ret;
}

int corten_arena_release(struct mm_struct *mm, unsigned long addr,
			 unsigned long len)
{
	unsigned long frame, first_frame, last_frame;
	struct corten_mm_state *state;
	struct corten_arena *arena;
	bool drained;
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
		if (WARN_ON_ONCE(xa_erase(&state->arenas, frame) != arena))
			break;
	}
	refcount_set(&state->nr, refcount_read(&state->nr) - 1);
	corten_arena_stat_add(state, CORTEN_ARENA_STAT_RELEASES, 1);

	/* Drain in-flight transactions, then tear the shadow-VMA down with
	 * the plain legacy munmap: pages are zapped and the PT pages travel
	 * through the regular free funnels (M2a uninstall).  The mmap lock
	 * is taken only after the drain, when no transaction exists any
	 * more, so it never forms a wait cycle with a transaction (sec 6.3).
	 * On a drain timeout (leaked reference, a kernel bug) the teardown
	 * still runs: a runnable process with a counted leak beats an
	 * unkillable D-state zombie; the in-flight-transaction safety
	 * argument above is void in that case, which is why the leak is
	 * counted loudly (CORTEN_ARENA_STAT_DRAIN_TIMEOUTS).
	 */
	if (!corten_arena_drain(arena)) {
		corten_arena_stat_add(state, CORTEN_ARENA_STAT_DRAIN_TIMEOUTS,
				      1);
		drained = false;
	} else {
		drained = true;
	}

	mmap_write_lock(mm);
	{
		/* [M3b S6 transition] Strip the shadow decoration before the
		 * legacy teardown: the do_vmi_align_munmap() arena guard
		 * (corten_arena_munmap_vma_guard()) rejects every range that
		 * still overlaps a VM_CORTEN vma, and RELEASE's own munmap
		 * must go through the regular funnel.  Clearing the cached
		 * shadow-VMA is safe here: the arena is drained, so no
		 * transaction can be reading arena->vma ([FAIL-2]).  The
		 * error path below re-strips idempotently.
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
	mmap_write_unlock(mm);

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
		if (corten_arena_drain(arena))
			corten_arena_free(arena);
		else
			corten_arena_stat_add(state,
					      CORTEN_ARENA_STAT_DRAIN_TIMEOUTS,
					      1);
	}

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
		if (!corten_arena_perm_ok(m, write, instruction)) {
			if (m->flags & CORTEN_PF_SHARED)
				return CORTEN_DISP_STUB;	/* fork COW: M5 */
			return CORTEN_DISP_ACCERR;
		}
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
	entry = folio_mk_pte(arena_folio, vma->vm_page_prot);
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
				      vma->vm_page_prot));

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

	entry = mk_pte(pfn_to_page(pte_pfn(cur)), vma->vm_page_prot);
	entry = pte_mkyoung(entry);
	if (ctx->write)
		entry = pte_mkwrite(pte_mkdirty(entry), vma);

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
		 * pte_alloc time) or a huge leaf: the shadow-VMA keeps
		 * the legacy path self-consistent, so hand the fault
		 * over (sec 4.3 err_legacy_fallback).  Count the
		 * untracked-window drift for both codes -- the next
		 * fill_upper() re-arms the descriptor, and the
		 * space-operation funnels zap untracked windows by PTE
		 * content (r03 defect C).
		 */
		corten_legacy_drift_inc();
		return CORTEN_F_FALLBACK;
	default:
		WARN_ON_ONCE(1);
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
	 * metadata is zeroed, so dispatch's own permission check cannot
	 * run), then record the allocation -- from here on the fault is
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
			.perm = ctx->ar->prot,
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
	case CORTEN_DISP_ACCERR:
		corten_unlock(&txn);
		return CORTEN_F_ACCERR;
	case CORTEN_DISP_STUB:
		/* M5 COW / M6 swap / M4+ shared: M3 refuses loudly. */
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
		/* Success: the speculative reference became the PTE
		 * reference.
		 */
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

				folio_remove_rmap_pte(folio, page, vma);
				add_mm_counter(mm, MM_ANONPAGES, -1);
				tlb_remove_page(tlb, page);
			}
		}

		if (recorded) {
			ret = corten_unmap(txn, addr, PAGE_SIZE);
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

			if (pte_present(oldpte) && !pte_special(oldpte)) {
				page = pte_page(oldpte);
				folio = page_folio(page);

				folio_remove_rmap_pte(folio, page, vma);
				add_mm_counter(mm, MM_ANONPAGES, -1);
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
			 */
			if (ar_start)
				percpu_ref_put(&ar_start->active);
			if (ar_end)
				percpu_ref_put(&ar_end->active);
			ret = corten_arena_release(mm, start, len);
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
 * legal lock-order edge (sec 6.1: mmap_lock outermost, desc->lock
 * inner, and no transaction ever takes mmap_lock back).  The exact
 * range cannot route to RELEASE here (RELEASE drains while holding
 * ctl_lock *before* taking mmap_lock), so it is rejected; kernel
 * callers never munmap declared arenas anyway.
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
 * unmaps, the MAP_FIXED overlap removal inside mmap_region) must reject
 * ranges overlapping a shadow-VMA, because zap_pte_range() writes PTEs
 * without the covering desc write lock.  RELEASE clears the flag before
 * its own do_munmap(), so teardown still works.
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
		return 0;

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
		ret = corten_arena_unmap_chunk(mm, ar, start, len);
		break;
	default:
		ret = 0;
		break;
	}
	percpu_ref_put(&ar->active);

	return ret;
}

/*
 * Behaviour-level madvise routing (sec 5.8), called from
 * madvise_do_behavior() under madvise_lock(): only MADV_DONTNEED /
 * MADV_DONTNEED_LOCKED ranges fully inside one arena take the
 * transactional path; all other behaviours are rejected when the range
 * overlaps an arena (the per-VMA check in madvise_vma_behavior() is the
 * backstop for ranges that miss the frame lookups).  MADV_POPULATE_*
 * would be safe (it faults through the arena hook), but M3 keeps the
 * matrix minimal per the design ("rest: reject"; revisit in M4).
 * Return: 0 = legacy, 1 = handled, -errno = reject.
 */
int corten_arena_madvise_route(struct mm_struct *mm, int behavior,
			       unsigned long start, unsigned long len)
{
	if (!corten_enabled_static() || !READ_ONCE(mm->corten_state))
		return 0;

	switch (behavior) {
	case MADV_DONTNEED:
	case MADV_DONTNEED_LOCKED:
		return corten_arena_dontneed_route(mm, start, len);
	default:
		return corten_arena_range_overlaps(mm, start, len) ?
		       -EOPNOTSUPP : 0;
	}
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
