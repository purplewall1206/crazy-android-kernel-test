/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CortenMM arena layer internal interfaces, shared between mm/corten_arena.c,
 * the fault/space-operation hooks and mm/corten_fault_test.c.  Not exported
 * to the rest of the kernel; the public surface lives in
 * include/linux/corten_arena.h.
 *
 * M3B_DESIGN.md sec 4 (fault hooks + dispatch), sec 4.4 (fill_upper),
 * sec 4.6 (map sequence) and sec 5 (space-operation matrix).
 */
#ifndef _MM_CORTEN_ARENA_H
#define _MM_CORTEN_ARENA_H

#include <linux/corten.h>
#include <linux/corten_arena.h>
#include <linux/mm_types.h>

struct pt_regs;
struct vm_area_struct;

#ifdef CONFIG_CORTEN_MM_ARENA

/*
 * ------------------------------------------------------------------ *
 * S4: fault dispatch (M3B_DESIGN.md sec 4.3 switch table, pure)
 * ------------------------------------------------------------------
 */

/*
 * Outcome of the dispatch state machine for one queried metadata entry.
 * Pure function of (metadata, write, instruction): mm/corten_fault_test.c
 * drives it without any mm (the S4 test contract is the state machine plus
 * a guest smoke test of the real fault path).
 */
enum corten_disp {
	CORTEN_DISP_MAP_ANON,	/* map a fresh anonymous page (S5) */
	CORTEN_DISP_ZERO_PAGE,	/* read fault: install the shared zero page */
	CORTEN_DISP_RESTORE,	/* CORTEN_MAPPED: rebuild PTE from metadata */
	CORTEN_DISP_FRESH,	/* CORTEN_INVALID: synthesize the PrivateAnon
				 * virtual allocation (Fig.8 L26-38).  The
				 * permission gate needs the arena prot (the
				 * zeroed metadata carries none), so the
				 * caller completes the dispatch after the
				 * corten_mark() synthesis
				 */
	CORTEN_DISP_ACCERR,	/* permission mismatch -> SEGV_ACCERR */
	CORTEN_DISP_STUB,	/* M5/M6/M4+ state -> WARN + SIGSEGV */
	CORTEN_DISP_MAPERR,	/* undecodable state -> SEGV_MAPERR */
};

enum corten_disp corten_arena_dispatch(const struct corten_pte_meta *m,
				       bool write, bool instruction);

/*
 * Slow path (M3B_DESIGN.md sec 4.2): called from handle_mm_fault() for
 * VM_CORTEN vmas with both the VMA-lock and in_atomic exits already taken
 * by the hook.  Returns the vm_fault_t the caller must pass on; the internal
 * CORTEN_FAULT_FALLBACK_BIT marks "transaction layer cannot own this fault
 * (untracked PT page / huge leaf): let the legacy body take over", which
 * is safe because the caller *is* the legacy path.
 */
#define CORTEN_FAULT_FALLBACK_BIT	((__force vm_fault_t)0x100000)

vm_fault_t corten_arena_handle_mm_fault(struct vm_area_struct *vma,
					unsigned long address,
					unsigned int flags,
					struct pt_regs *regs);

/*
 * S4: ensure the upper page tables (PGD..PMD, and with them the tracked
 * PTE-level page) exist for @addr without mmap_lock (M3B_DESIGN.md sec 4.4).
 * Return: 0 (present or filled), -ENOMEM, -EOPNOTSUPP (huge leaf).
 */
int corten_arena_fill_upper(struct corten_arena *ar, unsigned long addr);

/* Lookup + percpu_ref pin and the transactional chunk zap (sec 5.5);
 * also the entry points mm/corten_fault_test.c drives.
 */
struct corten_arena *corten_arena_lookup_get(struct mm_struct *mm,
					     unsigned long addr);
int corten_arena_unmap_chunk(struct mm_struct *mm, struct corten_arena *ar,
			     unsigned long start, unsigned long len);

/*
 * ------------------------------------------------------------------ *
 * S6: space-operation routing (M3B_DESIGN.md sec 5)
 * ------------------------------------------------------------------
 */

/* Classification of a range against one arena's bounds (pure, testable). */
enum corten_unmap_class {
	CORTEN_UNMAP_OUTSIDE = 0,	/* no overlap with this arena */
	CORTEN_UNMAP_CHUNK,		/* strictly inside: transaction zap */
	CORTEN_UNMAP_EXACT,		/* == arena: RELEASE territory */
	CORTEN_UNMAP_PARTIAL,		/* crosses the arena boundary */
};

enum corten_unmap_class corten_arena_unmap_classify(unsigned long start,
						    unsigned long end,
						    unsigned long ar_start,
						    unsigned long ar_end);

/*
 * sys_munmap() entry routing (sec 5.5).  Return: 0 = run legacy, 1 = range
 * already handled (chunk zap or exact-range RELEASE), -errno = reject.
 * Runs without mmap_lock: the chunk path works purely through transactions,
 * the exact path is prctl RELEASE semantics.
 */
int corten_arena_munmap_route(struct mm_struct *mm, unsigned long start,
			      unsigned long len);

/*
 * Same routing for callers that already hold mmap_lock for writing
 * (__vm_munmap).  The chunk path runs its transactions under the write
 * lock (a legal lock-order edge, sec 6.1); the exact range is rejected
 * because RELEASE must drain before taking mmap_lock itself.
 */
int corten_arena_munmap_guard(struct mm_struct *mm, unsigned long start,
			      unsigned long len);

/*
 * The deepest munmap funnel guard (do_vmi_align_munmap): reject any legacy
 * zap whose range still overlaps a shadow-VMA (VM_CORTEN).  This is the
 * safety net behind brk-shrink, mremap and the MAP_FIXED overlap removal;
 * arena chunks never reach here (routed above), and the RELEASE teardown
 * clears the flag before its own do_munmap().
 * Must be called with mmap_lock held for writing.  Returns -EOPNOTSUPP if
 * the range overlaps a shadow-VMA, 0 otherwise.
 */
int corten_arena_munmap_vma_guard(struct mm_struct *mm, unsigned long start,
				  unsigned long end);

/*
 * mmap MAP_FIXED routing gate (sec 5.6, [P1-4]).  The classification is
 * pure; corten_arena_mmap_route() adds the single-arena containment check
 * and drives the corten_mark() transaction.
 * Return: 0 = run legacy, 1 = mapped via transactions (*ret_addr set),
 * -errno = reject.
 */
enum corten_mmap_class {
	CORTEN_MMAP_LEGACY = 0,		/* not arena-markable */
	CORTEN_MMAP_MARK,		/* MAP_FIXED private anon: markable */
};

enum corten_mmap_class corten_arena_mmap_classify(unsigned long flags,
						  bool file);

int corten_arena_mmap_route(struct mm_struct *mm, unsigned long addr,
			    unsigned long len, unsigned long prot,
			    unsigned long flags, bool file);

/*
 * True if [start, start+len) intersects any declared arena of @mm.  RCU
 * xarray walk (xa_for_each_range), no mmap_lock needed; safe to call with
 * or without locks held.  !corten=on or no arenas -> false in O(1).
 */
bool corten_arena_range_overlaps(struct mm_struct *mm, unsigned long start,
				 unsigned long len);

/*
 * MADV_DONTNEED-equivalent routing (sec 5.8): a range strictly inside one
 * arena is zapped through transactions (content dropped, shadow-VMA kept).
 * Return convention as corten_arena_munmap_route().  Called with whatever
 * madvise_lock() provides; the decision itself is xarray-only.
 */
int corten_arena_dontneed_route(struct mm_struct *mm, unsigned long start,
				unsigned long len);

/* Behaviour-level madvise gate (madvise_do_behavior): 0 = legacy,
 * 1 = handled, -errno = reject.
 */
int corten_arena_madvise_route(struct mm_struct *mm, int behavior,
			       unsigned long start, unsigned long len);

/* hwpoison anchor (sec 5.20): WARN_ONCE + count when @folio maps into a
 * shadow-VMA.  Legacy-invisible (one static-branch read on the common
 * path).  Called from hwpoison_user_mappings().
 */
void corten_arena_hwpoison_check(struct folio *folio);

/* Exported for mm/corten_fault_test.c (same translation unit family). */
struct corten_mm_state *corten_arena_state(struct mm_struct *mm);

#else /* !CONFIG_CORTEN_MM_ARENA */

static inline vm_fault_t corten_arena_handle_mm_fault(struct vm_area_struct *vma,
						      unsigned long address,
						      unsigned int flags,
						      struct pt_regs *regs)
{
	return VM_FAULT_FALLBACK;
}

static inline int corten_arena_munmap_route(struct mm_struct *mm,
					    unsigned long start,
					    unsigned long len)
{
	return 0;
}

static inline int corten_arena_munmap_guard(struct mm_struct *mm,
					    unsigned long start,
					    unsigned long len)
{
	return 0;
}

static inline int corten_arena_munmap_vma_guard(struct mm_struct *mm,
						unsigned long start,
						unsigned long end)
{
	return 0;
}

static inline int corten_arena_mmap_route(struct mm_struct *mm,
					  unsigned long addr,
					  unsigned long len,
					  unsigned long prot,
					  unsigned long flags, bool file)
{
	return 0;
}

static inline bool corten_arena_range_overlaps(struct mm_struct *mm,
					       unsigned long start,
					       unsigned long len)
{
	return false;
}

static inline int corten_arena_dontneed_route(struct mm_struct *mm,
					      unsigned long start,
					      unsigned long len)
{
	return 0;
}

static inline int corten_arena_madvise_route(struct mm_struct *mm,
					     int behavior,
					     unsigned long start,
					     unsigned long len)
{
	return 0;
}

static inline void corten_arena_hwpoison_check(struct folio *folio)
{
}

#endif /* CONFIG_CORTEN_MM_ARENA */

#endif /* _MM_CORTEN_ARENA_H */
