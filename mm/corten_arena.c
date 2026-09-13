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
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/mmap_lock.h>
#include <linux/mutex.h>
#include <linux/percpu-refcount.h>
#include <linux/percpu.h>
#include <linux/rmap.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <uapi/linux/prctl.h>

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
	this_cpu_add(*state->stats, val);
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
	state->stats = __alloc_percpu(sizeof(unsigned long),
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
static void corten_arena_active_release(struct percpu_ref *ref)
{
	struct corten_arena *arena =
		container_of(ref, struct corten_arena, active);

	complete(&arena->drained);
}

/* Wait until no transaction can still hold a reference on @arena (sec 6.3).
 * Called with state->ctl_lock held, which is safe because the fault path
 * never takes that lock.
 */
static void corten_arena_drain(struct corten_arena *arena)
{
	percpu_ref_kill_and_confirm(&arena->active, NULL);
	wait_for_completion(&arena->drained);
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

/* Undo a shadow conversion after a publication failure.  Same locking as
 * corten_arena_shadowize().
 */
static void corten_arena_unshadow(struct vm_area_struct *vma)
{
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

	arena->prot = corten_arena_prot_from_vma(vma);
	ret = corten_arena_shadowize(vma);
	if (ret)
		goto out_write_unlock;

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
	corten_arena_unshadow(vma);
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
	 */
	corten_arena_drain(arena);

	mmap_write_lock(mm);
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
			corten_arena_unshadow(vma);
	}
	mmap_write_unlock(mm);

	/* The descriptor is unreachable and drained; percpu_ref_exit()
	 * before the kfree_rcu() it is embedded in (sec 2.1).  On the
	 * (memory-pressure) do_munmap() error path the arena is already
	 * deregistered and the stale shadow-VMA simply behaves as a plain
	 * anonymous VMA.
	 */
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
		 * each one once, at its first frame.
		 */
		if (frame < drained_until)
			continue;

		drained_until = arena->end >> PMD_SHIFT;
		corten_arena_drain(arena);
		corten_arena_free(arena);
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
