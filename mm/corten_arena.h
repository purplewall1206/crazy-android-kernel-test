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
	CORTEN_DISP_COW_MAYBE,	/* M5: write fault on a shared page whose
				 * contract is writable -- the fork
				 * wrprotect artifact.  The COW transaction
				 * reuses the page (map_count==1) or copies
				 * it (paper Sec. 4.3)
				 */
	CORTEN_DISP_COW_COPY,	/* M5: write fault on a shared page whose
				 * contract is read-only -- a genuine
				 * permission fault, not a wrprotect
				 * artifact; SIGSEGV in T1a (the
				 * FOLL_FORCE-forced copy unlock is M5.T2'
				 * scope, M5_FORK_SPEC.md sec 3.4)
				 */
	CORTEN_DISP_ACCERR,	/* permission mismatch -> SEGV_ACCERR */
	CORTEN_DISP_STUB,	/* M6/M4+ state -> WARN + SIGSEGV */
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
 * [F-A, D-G''] Fault-side ownership self-check: is @addr still arena
 * property?  Tier 1 is the cached shadow-VMA bounds test (zero walk),
 * tier 2 an RCU find_vma_intersection() + VM_CORTEN probe for addresses
 * outside the cached pointer (a punch split the arena).  Non-owned
 * addresses must run the legacy funnel -- the frame table is
 * address-keyed and keeps claiming punched-out holes (r05
 * dg2-analysis.md D2).  Exported for the D-G'' regression anchor.
 */
bool corten_arena_fault_owned(struct corten_arena *ar, struct mm_struct *mm,
			      unsigned long addr);

/*
 * The [F-A] decision, pure and table-testable: @cached is the arena's
 * cached shadow-VMA, @covering the VMA found over @addr by tier 2 (NULL
 * when tier 1 decides or the range is unmapped).
 */
bool corten_arena_fault_covered(const struct vm_area_struct *cached,
				const struct vm_area_struct *covering,
				unsigned long addr);

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
 * T0 release-on-full-coverage refinement (M4T0_SPEC.md sec 3.2): a CHUNK
 * that starts at the arena base and leaves a tail smaller than one PMD
 * window is really the user's page-rounded view of the whole arena (glibc
 * free() munmaps the request length, the kernel rounded the arena up to
 * 2M) and must be RELEASEd instead of chunk-zapped, otherwise allocator
 * churn leaks one VMA + 2M of window VA per cycle.  Pure; table-driven
 * in mm/corten_arena_test.c.
 */
enum corten_unmap_class corten_arena_release_classify(enum corten_unmap_class
						      class,
						      unsigned long start,
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
 * lock (a legal lock-order edge, DEV-13: mmap_write outermost); the exact
 * range is rejected because RELEASE acquires mmap_write itself and the
 * semaphore is not recursive.
 */
int corten_arena_munmap_guard(struct mm_struct *mm, unsigned long start,
			      unsigned long len);

/*
 * The deepest munmap funnel guard (do_vmi_align_munmap): reject any legacy
 * zap whose range still overlaps a shadow-VMA (VM_CORTEN).  This is the
 * safety net behind brk-shrink and mremap's internal unmaps; arena chunks
 * never reach here (routed above), and the RELEASE teardown clears the
 * flag before its own do_munmap().  The MAP_FIXED overlap removal in
 * mmap_region() does NOT pass this funnel -- it has its own frame-table
 * backstop in __mmap_prepare() (r05 dg2-analysis.md D1).
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
	CORTEN_MMAP_AUTO,		/* MODE auto-arena candidate (T0) */
};

enum corten_mmap_class corten_arena_mmap_classify(unsigned long flags,
						  bool file);

int corten_arena_mmap_route(struct mm_struct *mm, unsigned long addr,
			    unsigned long len, unsigned long prot,
			    unsigned long flags, bool file);

/*
 * [F-B, D-G''] Punch classification (pure, testable): what a non-markable
 * MAP_FIXED range overlapping one arena must do -- the geometry answers
 * are the munmap ones.  EXACT (incl. the release-on-full-coverage tail
 * rule) means RELEASE territory, CHUNK means punch (frames erased +
 * transactional zap, then the legacy funnel installs the mapping over
 * the emptied range), PARTIAL/OUTSIDE are reject/none-of-ours.
 */
enum corten_unmap_class corten_arena_punch_classify(unsigned long start,
						    unsigned long end,
						    unsigned long ar_start,
						    unsigned long ar_end);

/* ------------------------------------------------------------------ *
 * T0a: MODE-process transparent takeover (M4T0_SPEC.md sec 1/3)
 * ------------------------------------------------------------------
 */

/*
 * Auto-mmap whitelist classification (sec 3.1, pure): only an anonymous
 * MAP_PRIVATE mapping with no other flag word bits than MAP_NORESERVE is
 * auto-arena-able.  The type-bit check absorbs MAP_SHARED/_VALIDATE and
 * the MAP_DROPPABLE alias; any other bit (MAP_FIXED*, MAP_HUGETLB,
 * MAP_GROWSDOWN, MAP_POPULATE, MAP_LOCKED, MAP_SYNC, MAP_STACK,
 * MAP_UNINITIALIZED, MAP_DENYWRITE, ...) keeps the mapping legacy.
 * MAP_STACK stays excluded per OQ-B until the mprotect routing (T0b)
 * can serve thread-stack guard pages.
 */
enum corten_mmap_class corten_arena_auto_mmap_classify(unsigned long flags,
						       bool file);

/*
 * Pure cursor arithmetic of the auto-arena window (sec 3.1): place a
 * PMD-rounded @len at the @next_va cursor.  Return 0 with *@addr set on
 * success, -ENOSPC when the window is exhausted (the caller degrades to
 * the legacy path, counted).
 */
int corten_arena_auto_place(unsigned long next_va, unsigned long len,
			    unsigned long *addr);

/*
 * do_mmap() hook (mm/mmap.c, before __get_unmapped_area()): decide the
 * auto-arena takeover for one addr==0 anonymous private mapping of a MODE
 * process and rewrite the request onto the window.  Runs with this mm's
 * mmap_lock held for writing (do_mmap's contract).
 *
 * Return: 1 = takeover, *@addr / *@lenp / *@flagsp rewritten (MAP_FIXED
 * onto the window; the caller must attach with corten_arena_auto_attach()
 * once mmap_region() succeeded), 0 = legacy (not a whitelist hit, window
 * exhausted, or an obstacle in the window), -errno = internal error only.
 */
int corten_arena_auto_mmap_route(struct mm_struct *mm, unsigned long len,
				 unsigned long prot, unsigned long *addr,
				 unsigned long *lenp, unsigned long *flagsp);

/*
 * do_mmap() tail hook: register the freshly created VMA at @addr as an
 * auto arena -- DECLARE's locked body (validate/shadowize/publish), safe
 * because the caller already holds the mmap_write lock that DECLARE would
 * otherwise take first (DEV-13).  Failure degrades to a plain legacy
 * anonymous VMA at a window address (counted, harmless).
 * Return: 0 on success, -errno otherwise.
 */
int corten_arena_auto_attach(struct mm_struct *mm, unsigned long addr,
			     unsigned long len);

/*
 * Gate-free MODE internals; the syscall-context caller is
 * corten_prctl_mode() (capability + static-branch gates applied there).
 * ENTER creates the registry with the window cursor if needed and sets
 * mm->corten_mode; EXIT releases every arena and clears the mode; GET
 * reports the mode.  ENTER/EXIT take this mm's mmap_lock for writing
 * (the MODE-bit writer contract, sec 1.2).
 */
int corten_arena_mode_enter(struct mm_struct *mm);
int corten_arena_mode_exit(struct mm_struct *mm);
int corten_arena_mode_get(struct mm_struct *mm);

/*
 * Faithful fork (M5_FORK_SPEC.md sec 1.3, DEV-14/DEV-15; replaces the T0
 * fork_demote -- no fallback exists, a failure aborts the fork like any
 * other dup_mmap() error).  Both hooks run inside dup_mmap()'s window:
 * @oldmm's mmap_write is held (the DEV-13 outermost lock, so nesting the
 * registry ctl_lock is legal) and @mm's is held nested.
 *
 * corten_arena_fork_begin(): before any VMA is copied.  Inherits the MODE
 * bit, creates the child registry (next_va cursor copied) and freezes
 * every parent arena: frozen refuses new transactions at the fault-path
 * lookup, and the per-arena drain quiesces the in-flight ones -- the
 * copy_page_range()/fork_commit() below see a static snapshot.
 *
 * corten_arena_fork_commit(): after the for_each_vma()/copy_page_range()
 * loop (the PTE layer, the corten_glue_pte_write whitelist entry #1 per
 * DEV-14).  Marks the parent's CORTEN_MAPPED pages CORTEN_PF_SHARED from
 * the drained snapshot, registers the child arenas and deep-copies the
 * page metadata, then unfreezes the parent -- the single R-A closure:
 * whatever happens above, the parent is never left frozen.  A mid-commit
 * failure leaves the child's partial registry to the MMF_UNSTABLE exit
 * path (corten_arena_mm_exit()); SHARED-bit residue self-heals through
 * the COW reuse branch (the child that would have shared is gone).
 *
 * corten_arena_fork_abort(): the error-path counterpart for aborts that
 * skip fork_commit (fatal signal, copy_page_range failure); a cheap no-op
 * when no freeze is outstanding.  clone(CLONE_VM) never runs dup_mmap and
 * is unaffected (M5_FORK_SPEC.md sec 2.3 E2).
 *
 * Return (begin/commit): 0 on success, -errno (the fork fails; dup_mm()
 * collapses the error to -ENOMEM at the syscall boundary).
 */
int corten_arena_fork_begin(struct mm_struct *mm, struct mm_struct *oldmm);
int corten_arena_fork_commit(struct mm_struct *mm, struct mm_struct *oldmm);
void corten_arena_fork_abort(struct mm_struct *oldmm);

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

/*
 * T0b: do_mprotect_pkey() routing gate (M4T0_SPEC.md sec 3.3).  Runs
 * with this mm's mmap_write lock held.  0 = no arena in range (legacy),
 * 1 = routed (permission transaction + TLB flush done), -errno =
 * reject (boundary crossing, pkey/PROT_GROWS* combinations, or a
 * MODE-targeted arena keeping the S6 verdict).
 */
int corten_arena_mprotect_route(struct mm_struct *mm, unsigned long start,
				unsigned long len, unsigned long prot,
				int pkey);

/*
 * T0b: sys_mremap() routing gate (sec 8 T0-R1 / D12).  Runs with no
 * locks held, before do_mremap() and its prechecks -- questionable
 * requests return 0 for the legacy funnel's documented errno.  0 =
 * legacy, >0 = routed (the new address; the old arena is retired),
 * -errno = counted reject (MREMAP_FIXED/DONTUNMAP, boundary crossing,
 * no-MAYMOVE grow, window exhaustion).
 */
long corten_arena_mremap_route(struct mm_struct *mm, unsigned long addr,
			       unsigned long old_len, unsigned long new_len,
			       unsigned long flags, unsigned long new_addr);

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

static inline int corten_arena_mprotect_route(struct mm_struct *mm,
					      unsigned long start,
					      unsigned long len,
					      unsigned long prot, int pkey)
{
	return 0;
}

static inline long corten_arena_mremap_route(struct mm_struct *mm,
					     unsigned long addr,
					     unsigned long old_len,
					     unsigned long new_len,
					     unsigned long flags,
					     unsigned long new_addr)
{
	return 0;
}

static inline void corten_arena_hwpoison_check(struct folio *folio)
{
}

static inline int corten_arena_auto_mmap_route(struct mm_struct *mm,
					       unsigned long len,
					       unsigned long prot,
					       unsigned long *addr,
					       unsigned long *lenp,
					       unsigned long *flagsp)
{
	return 0;
}

static inline int corten_arena_auto_attach(struct mm_struct *mm,
					   unsigned long addr,
					   unsigned long len)
{
	return 0;
}

static inline int corten_arena_mode_enter(struct mm_struct *mm)
{
	return -EOPNOTSUPP;
}

static inline int corten_arena_mode_exit(struct mm_struct *mm)
{
	return -EOPNOTSUPP;
}

static inline int corten_arena_mode_get(struct mm_struct *mm)
{
	return 0;
}

static inline int corten_arena_fork_begin(struct mm_struct *mm,
					  struct mm_struct *oldmm)
{
	return 0;
}

static inline int corten_arena_fork_commit(struct mm_struct *mm,
					   struct mm_struct *oldmm)
{
	return 0;
}

static inline void corten_arena_fork_abort(struct mm_struct *oldmm)
{
}

#endif /* CONFIG_CORTEN_MM_ARENA */

#endif /* _MM_CORTEN_ARENA_H */
