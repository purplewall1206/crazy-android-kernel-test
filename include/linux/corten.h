/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CortenMM - page-descriptor based transactional memory management.
 *
 * Linux port of the CortenMM design (Sec. 3.3/Sec. 4 of the CortenMM paper): every
 * user page-table page ("PT page") is associated with a page descriptor
 * (struct corten_ptdesc) holding the PT-page lock, a back-link to the owning
 * address space and the VA window it covers, plus an on-demand per-PTE
 * metadata array (struct corten_pte_meta) that carries the state which does
 * not fit in the MMU (paper Figure 3).  All memory operations on an address
 * space run as transactions on the "covering" PT page (paper Sec. 4.1).
 *
 * Engineering adaptations w.r.t. the paper (documented for reviewers):
 *
 *  - The paper allocates all page descriptors from one contiguous region at
 *    boot and indexes them directly by PFN.  A general-purpose kernel cannot
 *    reserve that array up front, so we use an xarray keyed by PFN
 *    (PFN -> struct corten_ptdesc) which gives the same O(1) PFN lookup
 *    while allocating descriptors dynamically.  The lookup cost is paid only
 *    on the transaction entry path, never on the page-fault fast path.
 *
 *  - The per-PT-page lock starts as rwlock_t, matching the CortenMMrw
 *    protocol shape (paper Figure 5 uses a readers-writer lock).  The paper's
 *    production locks (BRAVO-pfqlock for CortenMMrw, MCS for CortenMMadv)
 *    are an M4+ optimization; access the field only through the protocol
 *    helpers so the lock type can be swapped without touching callers.
 *
 *  - The paper's RCursor holds the root-to-covering path read-locked for the
 *    whole transaction (paper Figure 5 L13 releases everything at scope
 *    exit).  Linux rwlock_t readers run with preemption disabled, so a
 *    transaction that may allocate or copy a page cannot hold them that
 *    long.  corten_lock_range() therefore releases the path read locks as
 *    soon as the covering write lock is secured (see mm/corten.c); mutual
 *    exclusion comes from the covering write lock alone.  Consequence: this
 *    port does not get the paper's "parent read lock blocks concurrent
 *    PT-page removal for the whole transaction" guarantee; that is covered
 *    by the pin/stale protocol below instead.
 *
 * Pin / TLB-order discipline (invariant, reviewers read this first):
 *
 *   The PT-page uninstall hook runs from the TLB-batched free funnels, i.e.
 *   a PT page can be torn down and batched for freeing BEFORE the flush that
 *   makes its removal globally visible (paper Figure 7).  Therefore:
 *
 *   INVARIANT: every protocol reader (lock walk, transaction operation,
 *   debugfs) must obtain a descriptor through corten_ptdesc_get() -- a pin
 *   taken under rcu_read_lock() against a kfree_rcu() releaser -- and must
 *   not retain any descriptor pointer beyond the matching
 *   corten_ptdesc_put().  Un-pinned pointers into a descriptor are invalid
 *   across any sleep/preemption point.  A descriptor found stale
 *   (desc->stale != 0) means its PT page is being torn down: transaction
 *   entry re-checks staleness under the descriptor locks and fails with
 *   -EAGAIN so the caller can retry (paper Figure 7 "gets the stale PT page
 *   and then retry").
 *
 *   INVARIANT (M3a interlock, paper Figure 7): uninstall publishes
 *   staleness UNDER the descriptor write lock, so a PT page cannot be
 *   retired while any transaction holds that page's write lock.  A
 *   transaction body therefore owns its covering PT page for the whole
 *   write-lock hold time: the page's memory cannot be handed back under
 *   it, and ensure-alloc (paper Figure 5 L5'/L8) can create child pages
 *   under the parent's write lock with the paper's "lock one implicitly
 *   locks all" reasoning -- a freshly installed child has no competing
 *   writer because every protocol path to it is gated by the parent's
 *   write lock.  Every desc->lock acquisition is BH-symmetric (the _bh
 *   rwlock variants on both the read and the write side, uninstall
 *   included), which is what keeps the uninstaller's waits bounded: a
 *   softirq-context uninstaller (khugepaged pte_free_defer() ->
 *   RCU_SOFTIRQ -> pte_free_now -> pte_free) never waits for a task that
 *   is itself stopped in the same CPU's softirq processing, because
 *   BH-symmetric holders keep softirqs disabled on their CPU for the
 *   whole hold, so that softirq cannot start under them; and the
 *   remaining holders (other CPUs, other softirqs) always finish their
 *   hold without sleeping -- write-lock holders do metadata work with
 *   GFP_NOWAIT only, walk read locks cover one stale check plus one
 *   non-sleeping child lookup.
 *
 * Nothing here has any effect unless CONFIG_CORTEN_MM is enabled AND the
 * kernel is booted with corten=on (static branch, default off).
 */

#ifndef _LINUX_CORTEN_H
#define _LINUX_CORTEN_H

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/types.h>

struct mm_struct;
struct page;

/*
 * State of one virtual page as stored in the per-PTE metadata array.
 * Mirrors the paper's Status enum (Figure 4).  The states are produced and
 * consumed through the transaction API below (corten_query/map/mark/unmap).
 */
enum corten_page_state {
	CORTEN_INVALID = 0,	/* paper: Invalid -- page not allocated */
	CORTEN_MAPPED,		/* paper: Mapped(PhysPage, Perm) */
	CORTEN_PRIVATE_ANON,	/* paper: PrivateAnon(Perm), virtually alloc'd */
	CORTEN_FILE_MAPPED,	/* paper: PrivateFileMapped(File, Offset, Perm) */
	CORTEN_SWAPPED,		/* paper: Swapped(BlockDev, BlockNum, Perm) */
	CORTEN_SHARED_ANON,	/* paper: shared anonymous */
};

/* Permission bits for corten_pte_meta.perm (paper: Perm). */
#define CORTEN_PERM_READ	_BITUL(0)
#define CORTEN_PERM_WRITE	_BITUL(1)
#define CORTEN_PERM_EXEC	_BITUL(2)
#define CORTEN_PERM_USER	_BITUL(3)
#define CORTEN_PERM_ALL		(CORTEN_PERM_READ | CORTEN_PERM_WRITE | \
				 CORTEN_PERM_EXEC | CORTEN_PERM_USER)

/*
 * COW flags for corten_pte_meta.flags (paper Sec. 4.3): the shared bit marks
 * pages that may have more than one sharer after fork(), the writable bit
 * records whether the virtual page was actually writable before the fork
 * read-only protection.  A write fault on (shared && !writable) is a genuine
 * fault -- the page was read-only before the fork, so the fault handler goes
 * through the COW copy / FOLL_FORCE branch and must not be short-circuited
 * into a plain "re-enable write" fast path.  (A write fault on
 * (shared && writable) takes the ordinary COW copy branch.)  Rule enforced
 * by corten_mark(): a logically writable page marked shared must carry
 * CORTEN_PF_WRITABLE.
 */
#define CORTEN_PF_SHARED	_BITUL(0)
#define CORTEN_PF_WRITABLE	_BITUL(1)
#define CORTEN_PF_ALL		(CORTEN_PF_SHARED | CORTEN_PF_WRITABLE)

/*
 * Flags for corten_map().
 *
 * CORTEN_MAP_FORCE: allow replacing an existing CORTEN_MAPPED metadata
 * (e.g. a COW copy-in or a swap-in overwrite).  Without it corten_map() on
 * an already-mapped page fails with -EEXIST so that double-mapping bugs
 * surface as errors instead of silent refcount leaks.
 */
#define CORTEN_MAP_FORCE	_BITUL(0)
#define CORTEN_MAP_ALL		CORTEN_MAP_FORCE

/*
 * Flags for corten_unmap().
 *
 * CORTEN_UNMAP_KEEP_PERM: move the state back to CORTEN_INVALID but keep
 * the recorded permission bits in the slot.  The arena content-drop path
 * (chunk munmap / MADV_DONTNEED routing) uses this: the VA reservation and
 * every routed mprotect() permission committed on it survive the drop, so
 * a later fault re-derives the committed contract instead of the
 * DECLARE-time arena bound (paper Fig.8 L9-13: unmap clears content, keeps
 * the VA -- the committed permission is part of that reservation).  The
 * default scrubs the whole slot (the arena contract itself ends).
 */
#define CORTEN_UNMAP_KEEP_PERM	_BITUL(1)
#define CORTEN_UNMAP_ALL	CORTEN_UNMAP_KEEP_PERM

/*
 * Entries per PTE page on x86-64.  Kept as a literal so this header stays
 * architecture-independent; mm/corten.c BUILD_BUG_ONs it against
 * PTRS_PER_PTE.
 */
#define CORTEN_PTES_PER_PT_PAGE	512

#define CORTEN_META_ARRAY_BYTES	\
	(CORTEN_PTES_PER_PT_PAGE * sizeof(struct corten_pte_meta))

/*
 * Per-PTE metadata, one entry per virtual 4K page (paper Figure 3 ".aux").
 * Deliberately 8 bytes so the whole array for one PT page is exactly 4K and
 * comes from the order-0 kmalloc caches.
 *
 * The 5 reserved bytes are where the payloads of the non-resident states
 * will live (M3+): the backing file reference for CORTEN_FILE_MAPPED, the
 * (device, block) pair plus on-disk offset for CORTEN_SWAPPED, and the
 * physical-page identity for CORTEN_MAPPED (M3 writes it together with the
 * hardware PTE).  Whether those stay inline (compact IDs) or become indices
 * into an out-of-line table is decided in M3; the struct size is not
 * expected to grow.
 */
struct corten_pte_meta {
	/* enum corten_page_state */
	u8	state;
	/* CORTEN_PERM_* */
	u8	perm;
	/* CORTEN_PF_* (COW shared/writable, paper Sec. 4.3) */
	u8	flags;
	/* reserved: file ref / swap (dev, blk, off) / page identity payload */
	u8	__resv[5];
} __packed;

/* PT-page levels, root first (matches the x86-64 descent order). */
enum corten_pt_level {
	CORTEN_LEVEL_PGD = 0,
	CORTEN_LEVEL_P4D,
	CORTEN_LEVEL_PUD,
	CORTEN_LEVEL_PMD,
	CORTEN_LEVEL_PTE,
};

/*
 * Slots for the descent path of one transaction.  A 5-level x86-64 walk
 * passes through at most 5 PT pages from the root to the covering page
 * (PGD, P4D, PUD, PMD, PTE); during the walk all of them are held in
 * @path read-locked, and once the covering write lock is secured the
 * covering page is moved to @covering, leaving at most the 4 intermediate
 * levels behind in @path (which the protocol releases before returning).
 */
#define CORTEN_TXN_PATH_MAX	5

/**
 * struct corten_txn - transaction cursor over a locked VA range.
 *
 * Paper analogue: RCursor (paper Figure 4).  Produced by corten_lock_range(),
 * consumed by corten_query()/corten_map()/corten_mark()/corten_unmap(), and
 * released by corten_unlock() (the paper releases on RCursor scope exit; the
 * C port makes the release explicit).
 *
 * ATOMICITY: the covering PT page's write lock is held continuously from a
 * successful corten_lock_range() until corten_unlock().  All operations on
 * the handle therefore run atomically with respect to any other transaction
 * whose range overlaps this one (they serialize on the same descriptor
 * lock); transactions on disjoint ranges touch different descriptors and do
 * not contend (paper Sec. 3.3 concurrency semantics).
 *
 * Allocate on the stack (about 80 bytes) of the thread running the
 * transaction; it must not be shared between threads.
 */
struct corten_txn {
	/** @mm: address space the range belongs to. */
	struct mm_struct	*mm;
	/** @start: first VA of the locked range. */
	unsigned long		start;
	/** @end: first VA past the locked range. */
	unsigned long		end;
	/**
	 * @covering: descriptor of the covering PT page, write-locked and
	 * pinned.  All transaction operations act on its metadata array.
	 * NULL while @txn is not locked.
	 */
	struct corten_ptdesc	*covering;
	/**
	 * @path: descent-path bookkeeping used by corten_lock_range() only
	 * (root first).  On entry into the operations below it is always
	 * empty; it is kept in the struct so the walk needs no separate
	 * allocation and so a future sleepable path-lock scheme (or the
	 * advanced protocol's locked descendants, paper Figure 6) can hold
	 * state here.  Entries are pinned while stored.
	 */
	struct corten_ptdesc	*path[CORTEN_TXN_PATH_MAX];
	/** @nr_path: number of valid entries in @path. */
	u8			nr_path;
	/** @level: enum corten_pt_level of @covering (informational). */
	u8			level;
};

#ifdef CONFIG_CORTEN_MM

#include <linux/jump_label.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/spinlock.h>

DECLARE_STATIC_KEY_FALSE(corten_enabled_key);

/**
 * corten_enabled_static - fast runtime gate for the CortenMM hooks.
 *
 * Backed by a static branch enabled by the corten=on boot parameter
 * (default off), so an un-instrumented kernel pays only a nop.
 */
static inline bool corten_enabled_static(void)
{
	return static_branch_unlikely(&corten_enabled_key);
}

#define CORTEN_PTDESC_MAGIC	0xc077e57d

/*
 * Per-PT-page descriptor (paper Figure 3; reference implementation's
 * PageTablePageMeta).  One per user PTE-level page, indexed by the PT
 * page's PFN in a global xarray.  The descriptor is created in the
 * pte_alloc_one() hook and destroyed in the pte_free()/pte_free_tlb()
 * hooks, i.e. it lives exactly as long as the PT page.
 *
 * Note: the kernel's struct ptdesc is the kernel's own page-table
 * descriptor and is unrelated to this struct despite the name.
 */
struct corten_ptdesc {
	/*
	 * Protects this PT page and its metadata array for the transaction
	 * protocols (paper Sec. 4.1).  rwlock_t is the starting point for the
	 * CortenMMrw protocol; M4+ may swap in a pfq/BRAVO-based lock, so
	 * take it only through the protocol helpers.  The write side also
	 * implements the M3a uninstall interlock: corten_ptdesc_uninstall()
	 * publishes staleness under it, so a write-lock holder owns the PT
	 * page for its whole hold time.
	 */
	rwlock_t		lock;
	/* Back-link: owning address space (descriptor -> AddrSpace). */
	struct mm_struct	*mm;
	/*
	 * Base VA of the window this PT page covers once installed
	 * (PMD_SIZE-aligned for a PTE-level page).  The pte_alloc_one() hook
	 * does not know the VA; the locking protocol fills this in the first
	 * time the page becomes a covering page (until then it is 0 --
	 * synthetic descriptor trees set it at construction).
	 */
	unsigned long		va_base;
	/* Per-PTE metadata array, on demand, freed with the descriptor. */
	struct corten_pte_meta	*meta;
	/* Lookup/transaction pins on top of the xarray's base reference. */
	refcount_t		refs;
	/* Paper Figure 6 L32: set when the PT page is being torn down. */
	u8			stale;
	/* enum corten_pt_level of the page this descriptor is attached to. */
	u8			level;
	/*
	 * Populated child entries of this PT page.  Maintained by the
	 * transaction operations that create/remove lower-level PT pages
	 * (M3 ensure-alloc / M4 unmap pruning, paper Figure 6); the locking
	 * protocol itself only reads it (debugfs).  Protected by @lock.
	 */
	u16			nr_children;
	/*
	 * M6.T3 shrinker bookkeeping: number of slots in the
	 * CORTEN_MAPPED (resident content, swap-out candidate) and
	 * CORTEN_SWAPPED (entry recorded) states, maintained by
	 * corten_map()/corten_swap_out()/corten_unmap()/
	 * corten_txn_meta_drop() under the covering write lock -- every
	 * state transition of those two shapes goes through exactly
	 * those operations, so the counts are exact by construction.
	 * Written under @lock; lockless readers use READ_ONCE() and
	 * treat the result as a snapshot (debugfs, shrinker count).
	 */
	long			nr_mapped;
	long			nr_swapped;
	/* Debug: always CORTEN_PTDESC_MAGIC while alive. */
	u32			magic;
	/* Deferred free of the descriptor itself (kfree_rcu). */
	struct rcu_head		rcu;
};

/**
 * corten_lock_range - lock a VA range for a transaction (paper:
 *                     AddrSpace::lock, Figure 5).
 * @mm: address space to operate on.
 * @start: first VA of the range (page aligned).
 * @len: length of the range in bytes (> 0).
 * @txn: uninitialized handle receiving the transaction cursor.
 *
 * Implements the CortenMMrw locking protocol (paper Figure 5): descend from
 * the root taking a read lock on every PT page whose single child fully
 * covers [@start, start+len), and write-lock the lowest possible covering PT
 * page.  On success @txn->covering identifies the covering page; all
 * operations through @txn are atomic w.r.t. other transactions overlapping
 * the range; transactions on disjoint ranges proceed in parallel.
 *
 * 2b scope limits (see mm/corten.c):
 *  - only PTE-level pages carry descriptors so far (the M2a hooks), so the
 *    range must fit into one PMD window (2M); wider ranges return
 *    -EOPNOTSUPP (M4+ turns the cursor into an iterator over per-covering-
 *    page transactions);
 *  - a missing PT page reports -ENOENT when the tree view cannot allocate
 *    (no @alloc callback, or a root-level hole with no parent page to
 *    upgrade); ensure-alloc below a tracked page runs under that page's
 *    write lock (paper Figure 5 L5'/L8 "lock one implicitly locks all")
 *    and reports the view's error (-ENOMEM, -EOPNOTSUPP) otherwise; the
 *    real x86 view declines allocation until 3b wires it from the fault
 *    path;
 *  - the caller must keep the walked page-table hierarchy alive for the
 *    duration of the call (e.g. hold mmap_lock for read or own a private
 *    mm); M3's arena design retires upper-level PT pages only through the
 *    transaction protocol and drops this requirement there.
 *
 * The caller must balance a successful lock with corten_unlock().  On error
 * @txn is not locked and must not be passed to corten_unlock().
 *
 * Return: 0 on success, negative error otherwise.
 */
int corten_lock_range(struct mm_struct *mm, unsigned long start,
		      unsigned long len, struct corten_txn *txn);

/**
 * corten_query - read the state of one virtual page in a transaction
 *                (paper: RCursor::query).
 * @txn: locked transaction handle.
 * @addr: virtual address to query (page aligned, inside the locked range).
 * @out: receives the page state (state, perm, COW flags).
 *
 * A PT page without a metadata array queries as CORTEN_INVALID (nothing has
 * been recorded for it yet).
 *
 * Return: 0 on success, -ERANGE if @addr is outside the locked range,
 * -EINVAL on a misaligned @addr.
 */
int corten_query(struct corten_txn *txn, unsigned long addr,
		 struct corten_pte_meta *out);

/**
 * corten_map - program the MMU mapping for one virtual page in a
 *              transaction (paper: RCursor::map, e.g. a page fault
 *              installing a page, paper Figure 8 L22).
 * @txn: locked transaction handle.
 * @addr: virtual address to map (page aligned, inside the locked range).
 * @page: physical page to map.
 * @perm: CORTEN_PERM_* for the mapping.
 * @flags: CORTEN_MAP_FORCE or 0.
 *
 * State machine: any state may transition to CORTEN_MAPPED (the paper's
 * fault path maps onto PrivateAnon/Swapped/... pages alike), except an
 * already CORTEN_MAPPED page, which requires %CORTEN_MAP_FORCE and fails
 * with -EEXIST otherwise.
 *
 * 2b scope: this updates the metadata layer only.  Writing the hardware
 * PTE (set_pte) and the TLB bookkeeping is wired in by M3 when the fault
 * path learns to call this under the covering write lock.
 *
 * Return: 0 on success, negative error otherwise.
 */
int corten_map(struct corten_txn *txn, unsigned long addr, struct page *page,
	       u8 perm, unsigned int flags);

/**
 * corten_mark - change the recorded state of a VA range in a transaction
 *               (paper: RCursor::mark, e.g. virtually-allocate an anonymous
 *               range on mmap(), or record the fork COW bits, paper
 *               Sec. 4.3).
 * @txn: locked transaction handle.
 * @start: first VA of the sub-range (page aligned, inside the locked range).
 * @len: length of the sub-range in bytes (> 0, multiple of PAGE_SIZE).
 * @meta: the state to record for every page in the sub-range (state, perm,
 *        COW flags).
 *
 * State machine: @meta->state == CORTEN_INVALID is rejected (-EINVAL; use
 * corten_unmap()); a state change is legal from CORTEN_INVALID to one of
 * PRIVATE_ANON/FILE_MAPPED/SHARED_ANON (virtual allocation) and from any
 * state to itself (permission / COW-flag updates, e.g. mprotect or fork).
 * A logically writable page marked shared must carry CORTEN_PF_WRITABLE
 * (paper Sec. 4.3), else -EINVAL.
 *
 * The whole sub-range is validated before anything is written; on error no
 * metadata is modified.
 *
 * Return: 0 on success, negative error otherwise.
 */
int corten_mark(struct corten_txn *txn, unsigned long start, unsigned long len,
		const struct corten_pte_meta *meta);

/**
 * corten_swap_out - record the Swapped state of one virtual page in a
 *                   transaction (M6.T2, M6_RMAP_SPEC.md sec 2.1 D1/D6).
 * @txn: locked transaction handle.
 * @addr: virtual address to update (page aligned, inside the locked range).
 * @meta: the state to record: @meta->state must be %CORTEN_SWAPPED, its
 *        @perm is the contract that survives the swap-out and its
 *        @__resv bytes carry the swap entry encoding of the caller
 *        (corten_swap_encode(), mm/corten_arena.h).
 *
 * Dedicated operation (corten_mark() deliberately refuses the Swapped
 * state): the only legal transition is from %CORTEN_MAPPED -- a page with
 * content, already installed by a fault -- to %CORTEN_SWAPPED, performed
 * by the reclaim-side transaction while it rewrites the hardware PTE to
 * the swap entry.  %CORTEN_PRIVATE_ANON slots (zero-page reads) have no
 * content to swap and stay out of the swap-out path.
 *
 * Return: 0 on success, -EINVAL on a bad state/permission shape,
 * -ERANGE/-EINVAL for an out-of-range or misaligned @addr.
 */
int corten_swap_out(struct corten_txn *txn, unsigned long addr,
		    const struct corten_pte_meta *meta);

/**
 * corten_swap_replay - record a CORTEN_SWAPPED slot payload verbatim
 *                      (M6.T3 fork replay arm).
 * @txn: locked transaction handle.
 * @addr: virtual address to update (page aligned, inside the range).
 * @meta: the payload to record: %CORTEN_SWAPPED + the entry encoding,
 *        no COW flags (the fork-shared shape never swaps out).
 *
 * The fork mirror's child replay: the child's swap PTE was installed
 * by copy_nonpresent_pte() (the entry duplicated there), so the slot
 * records the snapshot payload directly instead of transitioning from
 * CORTEN_MAPPED -- the only transition corten_swap_out() allows.  Only
 * a non-resident slot (Invalid / virtually allocated, the fresh child
 * shapes) can be replayed into; every live-content shape must go
 * through the state machine.
 *
 * Return: 0 on success, -EEXIST if the slot holds resident content or
 * a recorded entry, -EINVAL on a bad payload shape, -ERANGE/-EINVAL
 * for an out-of-range or misaligned @addr, -ENOMEM when the metadata
 * array cannot be ensured.
 */
int corten_swap_replay(struct corten_txn *txn, unsigned long addr,
		       const struct corten_pte_meta *meta);

/**
 * corten_unmap - remove the mapping of a VA range in a transaction
 *                (paper: RCursor::unmap; retiring emptied PT pages and
 *                marking them stale, paper Figure 6, is M4 scope).
 * @txn: locked transaction handle.
 * @start: first VA of the sub-range (page aligned, inside the locked range).
 * @len: length of the sub-range in bytes (> 0, multiple of PAGE_SIZE).
 * @flags: CORTEN_UNMAP_KEEP_PERM or 0.
 *
 * Every page in the sub-range must be recorded (state != CORTEN_INVALID),
 * else -ENOENT and nothing is changed.  The whole sub-range is validated
 * before anything is written.  Dropping the physical page references is
 * wired in by M3/M4; 2b only flips the metadata back to CORTEN_INVALID
 * (with %CORTEN_UNMAP_KEEP_PERM, keeping the recorded permission -- see
 * the flag's documentation), else scrubbing the slot.
 *
 * Return: 0 on success, negative error otherwise.
 */
int corten_unmap(struct corten_txn *txn, unsigned long start,
		 unsigned long len, unsigned int flags);

/**
 * corten_unlock - release a transaction (paper: AddrSpace::unlock; releases
 *                 the acquired locks in reverse acquisition order).
 * @txn: handle returned by corten_lock_range(); invalid afterwards.
 */
void corten_unlock(struct corten_txn *txn);

/* PT-page lifecycle hooks, called from the pgtable alloc/free funnels. */
void corten_on_pte_alloc(struct mm_struct *mm, struct page *pte_page);
void corten_on_pte_free(struct page *pte_page);

#else /* !CONFIG_CORTEN_MM */

static inline bool corten_enabled_static(void)
{
	return false;
}

static inline void corten_on_pte_alloc(struct mm_struct *mm,
				       struct page *pte_page)
{
}

static inline void corten_on_pte_free(struct page *pte_page)
{
}

static inline int corten_lock_range(struct mm_struct *mm, unsigned long start,
				    unsigned long len, struct corten_txn *txn)
{
	return -EOPNOTSUPP;
}

static inline int corten_query(struct corten_txn *txn, unsigned long addr,
			       struct corten_pte_meta *out)
{
	return -EOPNOTSUPP;
}

static inline int corten_map(struct corten_txn *txn, unsigned long addr,
			     struct page *page, u8 perm, unsigned int flags)
{
	return -EOPNOTSUPP;
}

static inline int corten_mark(struct corten_txn *txn, unsigned long start,
			      unsigned long len,
			      const struct corten_pte_meta *meta)
{
	return -EOPNOTSUPP;
}

static inline int corten_swap_out(struct corten_txn *txn, unsigned long addr,
				  const struct corten_pte_meta *meta)
{
	return -EOPNOTSUPP;
}

static inline int corten_swap_replay(struct corten_txn *txn,
				     unsigned long addr,
				     const struct corten_pte_meta *meta)
{
	return -EOPNOTSUPP;
}

static inline int corten_unmap(struct corten_txn *txn, unsigned long start,
			       unsigned long len, unsigned int flags)
{
	return -EOPNOTSUPP;
}

static inline void corten_unlock(struct corten_txn *txn)
{
}

#endif /* CONFIG_CORTEN_MM */

/*
 * M3b red lines (carried from the M3a review; do not regress):
 *
 *  1. desc->lock is a BH-safe lock class: every acquisition and release
 *     uses the _bh rwlock variants (read_lock_bh/read_unlock_bh,
 *     write_lock_bh/write_unlock_bh), on both the protocol and the
 *     uninstall side.  The deferred free funnel reaches
 *     corten_ptdesc_uninstall() from softirq context (khugepaged:
 *     pte_free_defer() -> RCU_SOFTIRQ -> pte_free_now -> pte_free), so a
 *     plain task-side read_lock() holder could deadlock the very softirq
 *     it is stopped in (same CPU: irq_exit -> do_softirq spins on the
 *     interrupted holder's lock).  Any new desc->lock taker must stay
 *     BH-symmetric.
 *
 *  2. Before M4 retires a PT-page subtree, staleness must be published
 *     to the DESCENDANTS first (paper Figure 6: rev_dfs marks the deepest
 *     pages stale before the ancestors are rcu_delay_free()d).  A
 *     transaction is only guaranteed to fail with -EAGAIN when its
 *     covering page is stale, so an "ancestor stale first" retirement
 *     would let transactions keep running on pages already queued for
 *     freeing.
 */

#endif /* _LINUX_CORTEN_H */
