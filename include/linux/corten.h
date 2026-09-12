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
 * Mirrors the paper's Status enum (Figure 4).  This slice only defines the
 * state space; the fault/mmap paths that produce these states arrive with
 * slice 2b.
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

/*
 * COW flags for corten_pte_meta.flags (paper Sec. 4.3): the shared bit marks
 * pages that may have more than one sharer after fork(), the writable bit
 * records whether the virtual page was actually writable before the fork
 * read-only protection.  A write fault on (shared && writable) copies the
 * page instead of just re-enabling write access.
 */
#define CORTEN_PF_SHARED	_BITUL(0)
#define CORTEN_PF_WRITABLE	_BITUL(1)

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
 * will live (slice 2b+): the backing file reference for CORTEN_FILE_MAPPED,
 * and the (device, block) pair plus on-disk offset for CORTEN_SWAPPED.
 * Whether those stay inline (compact IDs) or become indices into an
 * out-of-line table is decided in slice 2b; the struct size is not expected
 * to grow.
 */
struct corten_pte_meta {
	/* enum corten_page_state */
	u8	state;
	/* CORTEN_PERM_* */
	u8	perm;
	/* CORTEN_PF_* (COW shared/writable, paper Sec. 4.3) */
	u8	flags;
	/* reserved: file ref / swap (dev, blk, off) payload, see above */
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

/* Slots for the PT pages pinned by one transaction (slice 2b placeholder). */
#define CORTEN_HANDLE_MAX_PAGES	9

/**
 * struct corten_handle - transaction cursor over a locked VA range.
 *
 * Paper analogue: RCursor (paper Figure 4).  Produced by corten_lock_range(),
 * consumed by corten_query()/corten_map()/corten_mark()/corten_unmap(), and
 * released by corten_unlock() (the paper releases on RCursor scope exit; the
 * C port makes the release explicit).  Every operation on the handle is
 * atomic w.r.t. other transactions overlapping the range; transactions on
 * disjoint ranges do not contend (paper Sec. 3.3 concurrency semantics).
 */
struct corten_handle {
	/** @mm: address space the range belongs to. */
	struct mm_struct	*mm;
	/** @start: first VA of the locked range. */
	unsigned long		start;
	/** @end: first VA past the locked range. */
	unsigned long		end;
	/**
	 * @pages: PT pages pinned by the locking protocol -- the covering PT
	 * page first, then (advanced protocol, slice 2b) its locked
	 * descendants in DFS order.  Fixed-size placeholder; sized for one
	 * root-to-leaf path of a 5-level x86-64 table plus margin.
	 */
	struct corten_ptdesc	*pages[CORTEN_HANDLE_MAX_PAGES];
	/** @nr_pages: number of valid entries in @pages. */
	u8			nr_pages;
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
	 * take it only through the protocol helpers (slice 2b).
	 */
	rwlock_t		lock;
	/* Back-link: owning address space (descriptor -> AddrSpace). */
	struct mm_struct	*mm;
	/*
	 * Base VA of the window this PT page covers once installed
	 * (PMD_SIZE-aligned for a PTE-level page).  pte_alloc_one() runs
	 * before the page is placed in the tree, so the installer fills
	 * this in during slice 2b; it is 0 until then.
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
	/* Populated child entries; slice 2b DFS locking / unmap pruning. */
	u16			nr_children;
	/* Debug: always CORTEN_PTDESC_MAGIC while alive. */
	u32			magic;
	/* Deferred free of the descriptor itself (kfree_rcu). */
	struct rcu_head		rcu;
};

/**
 * corten_lock_range - lock a VA range for a transaction (paper:
 *                     AddrSpace::lock, Figure 5).
 * @mm: address space to operate on.
 * @start: first VA of the range.
 * @end: first VA past the range.
 * @handle: uninitialized handle receiving the transaction cursor.
 *
 * Walks the page table from the root while a single child PT page covers
 * the whole range, then takes the covering PT page's write lock (rw
 * protocol; the advanced protocol additionally locks descendants, slice
 * 2b).  On success all operations through @handle are atomic w.r.t. other
 * transactions overlapping [@start, @end); transactions on disjoint ranges
 * proceed in parallel.  The caller must balance with corten_unlock().
 *
 * Return: 0 on success, negative error otherwise.  On error @handle is not
 * locked and must not be used.
 */
int corten_lock_range(struct mm_struct *mm, unsigned long start,
		      unsigned long end, struct corten_handle *handle);

/**
 * corten_query - read the state of one virtual page in a transaction
 *                (paper: RCursor::query).
 * @handle: locked transaction handle.
 * @addr: virtual address to query, inside the locked range.
 * @out: receives the page state (state, perm, COW flags).
 *
 * Return: 0 on success, negative error otherwise.
 */
int corten_query(struct corten_handle *handle, unsigned long addr,
		 struct corten_pte_meta *out);

/**
 * corten_map - program the MMU mapping for one virtual page in a
 *              transaction (paper: RCursor::map).
 * @handle: locked transaction handle.
 * @addr: virtual address to map, inside the locked range.
 * @page: physical page to map.
 * @perm: CORTEN_PERM_* for the mapping.
 *
 * Return: 0 on success, negative error otherwise.
 */
int corten_map(struct corten_handle *handle, unsigned long addr,
	       struct page *page, u8 perm);

/**
 * corten_mark - change the recorded state of a VA range in a transaction
 *               (paper: RCursor::mark, e.g. virtually-allocate an anonymous
 *               range on mmap()).
 * @handle: locked transaction handle.
 * @start: first VA of the sub-range, inside the locked range.
 * @end: first VA past the sub-range.
 * @meta: the state to record for every page in the sub-range.
 *
 * Return: 0 on success, negative error otherwise.
 */
int corten_mark(struct corten_handle *handle, unsigned long start,
		unsigned long end, const struct corten_pte_meta *meta);

/**
 * corten_unmap - remove the mapping of a VA range in a transaction
 *                (paper: RCursor::unmap; also retires PT pages that become
 *                empty, marking them stale per paper Figure 6).
 * @handle: locked transaction handle.
 * @start: first VA of the sub-range, inside the locked range.
 * @end: first VA past the sub-range.
 *
 * Return: 0 on success, negative error otherwise.
 */
int corten_unmap(struct corten_handle *handle, unsigned long start,
		 unsigned long end);

/**
 * corten_unlock - release a transaction (paper: AddrSpace::unlock; releases
 *                 the acquired locks in reverse acquisition order).
 * @handle: handle returned by corten_lock_range(); invalid afterwards.
 */
void corten_unlock(struct corten_handle *handle);

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
				    unsigned long end,
				    struct corten_handle *handle)
{
	return -EOPNOTSUPP;
}

static inline int corten_query(struct corten_handle *handle, unsigned long addr,
			       struct corten_pte_meta *out)
{
	return -EOPNOTSUPP;
}

static inline int corten_map(struct corten_handle *handle, unsigned long addr,
			     struct page *page, u8 perm)
{
	return -EOPNOTSUPP;
}

static inline int corten_mark(struct corten_handle *handle, unsigned long start,
			      unsigned long end,
			      const struct corten_pte_meta *meta)
{
	return -EOPNOTSUPP;
}

static inline int corten_unmap(struct corten_handle *handle,
			       unsigned long start, unsigned long end)
{
	return -EOPNOTSUPP;
}

static inline void corten_unlock(struct corten_handle *handle)
{
}

#endif /* CONFIG_CORTEN_MM */

#endif /* _LINUX_CORTEN_H */
