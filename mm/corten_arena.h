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
#include <linux/rmap.h>		/* enum ttu_flags (M6.T1 walker guard) */
#include <linux/swap.h>		/* MAX_SWAPFILES (swapops prerequisite) */
#include <linux/swapops.h>	/* swp_type/offset/entry (M6.T2 encode) */
#include <linux/unaligned.h>	/* corten_swap_encode/decode (M6.T2) */

struct pt_regs;
struct vm_area_struct;

#ifdef CONFIG_CORTEN_MM_ARENA

/*
 * Swapped payload encoding in corten_pte_meta.__resv (M6.T2, spec D6):
 * __resv[0] = swap type (u8 -- MAX_SWAPFILES is far below 256; zram is a
 * single type), __resv[1..4] = swap offset, little-endian u32 (a 4G/4K
 * device is 1M slots, three orders below the field), __resv[5] stays
 * zero.  The paper's Swapped(BlockDev, BlockNum, Perm) maps to
 * (type, offset) here: Linux abstracts the block device into the swap
 * type and the perm rides in corten_pte_meta.perm.
 */
static inline void corten_swap_encode(struct corten_pte_meta *m,
				      swp_entry_t entry)
{
	m->__resv[0] = (u8)swp_type(entry);
	put_unaligned_le32(swp_offset(entry), &m->__resv[1]);
	m->__resv[5] = 0;
}

static inline swp_entry_t corten_swap_decode(const struct corten_pte_meta *m)
{
	return swp_entry(m->__resv[0], get_unaligned_le32(&m->__resv[1]));
}

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
				 * artifact; SIGSEGV for every writer,
				 * the process's own and kernel paths
				 * alike (M5.T3: no FOLL_FORCE survivor)
				 */
	CORTEN_DISP_ACCERR,	/* permission mismatch -> SEGV_ACCERR */
	CORTEN_DISP_FILE_READ,	/* V-B.3: CORTEN_FILE_MAPPED, read fault --
				 * the filemap_fault() core: the handler
				 * drops the covering lock (the pagecache
				 * fetch sleeps), takes the folio at
				 * region.rpoff + offset, re-locks and
				 * installs the clean translation in one
				 * transaction (H5)
				 */
	CORTEN_DISP_SWAPIN,	/* M6.T2: CORTEN_SWAPPED -- the fault is a
				 * swap-in (M6_RMAP_SPEC.md sec 2.1 D5).  The
				 * handler drops the covering lock (I/O and
				 * folio waits sleep), reads the entry back
				 * through the swap cache, re-locks and
				 * commits PTE + rmap + metadata in one
				 * transaction
				 */
	CORTEN_DISP_STUB,	/* M4+ state -> WARN + SIGSEGV */
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
 * W1.b (W1_NATIVE_RMAP_SPEC.md sec 4.2) invalidation enumeration: called
 * from unmap_mapping_pages()/unmap_mapping_folio() inside their
 * i_mmap_lock_read() section, this enumerates the mapping's registered
 * corten regions from the per-inode registry (the i_mmap replacement for
 * published FILE regions -- a carrier never joins the interval tree
 * anymore) and runs the B.2 chunk-zap transaction on every intersecting
 * VA range: KEEP_PERM with the walker's @even_cows verdict selecting the
 * CORTEN_UNMAP_FILE_EVENT demotion shape (truncate drops private COW
 * copies, invalidation spares them).  Zero regions (the whole legacy
 * world) costs one xarray probe.
 */
void corten_arena_unmap_file_range(struct address_space *mapping,
				   pgoff_t first_index, pgoff_t last_index,
				   bool even_cows);

/*
 * W1.b stale-node backstop (the demoted V-B.2 route gate): a VM_CORTEN
 * VMA surfacing through the mapping's i_mmap walk means a stale
 * interval-tree node survived its teardown (R-W1-2's exact shape -- the
 * enumeration above never hands carriers over anymore).  WARN, count
 * (imap_stale_refuses, must stay 0) and refuse the bare legacy writer,
 * exactly like the zap_page_range_single() backstop below.  The =n stub
 * folds to false so the call site compiles away.
 */
bool corten_arena_imap_stale_guard(struct vm_area_struct *vma);

/*
 * V-B.2 (H7) defensive backstop for zap_page_range_single(): a VM_CORTEN
 * VMA arriving there is a routing hole, not a shape to unmap -- WARN,
 * count (zap_single_refuses, must stay 0) and refuse.  The =n stub
 * folds to false so the call site compiles away.
 */
bool corten_zap_single_guard(struct vm_area_struct *vma);

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
	CORTEN_MMAP_AUTO_FILE,		/* MODE auto-arena candidate, file arm
					 * (V-B.1: private file mapping)
					 */
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
 *
 * V-B.1: @file flips the whitelist to its file mirror -- MAP_PRIVATE
 * with at most MAP_NORESERVE (MAP_ANONYMOUS is absent by construction
 * for a file request and also rejects defensively) classifies
 * CORTEN_MMAP_AUTO_FILE; every other bit keeps the mapping legacy,
 * MAP_FIXED included (the OQ-MV-2 implant exception keeps the D-G''
 * punch route).  MAP_SHARED in any spelling never enters the window
 * (MV_VMA_FREE_SPEC.md sec 3.2.1).
 */
enum corten_mmap_class corten_arena_auto_mmap_classify(unsigned long flags,
						       bool file);

/*
 * The do_mmap() file-validation chain replayed for the MODE window route
 * (V-B.1, pure; mm/corten_fault_test.c drives it without an mm).
 * Mirrors mm/mmap.c's file arm order and errnos: file_mmap_ok() (size
 * overflow, -EOVERFLOW), the DAX gate (-EOPNOTSUPP), FMODE_READ
 * (-EACCES), path_noexec x PROT_EXEC (-EPERM, and EXEC leaves the MAY
 * bound), can_mmap_file() (-ENODEV) and memfd_check_seals_mmap()
 * (errno through; a private mapping always passes the seal check
 * itself).  GROWSDOWN/GROWSUP are unreachable (the whitelist rejects
 * those flag bits up stream).
 *
 * On success *@may_prot receives the region's MAY bound: the full
 * private-mapping superset with EXEC removed for a noexec mount (the
 * only bit the open mode can take away -- legacy do_mmap() grants
 * VM_MAYREAD|MAYWRITE|MAYEXEC for private mappings regardless of prot).
 *
 * Return: 0 = file may enter the window, -errno = degrade to legacy
 * (which answers the caller with this same errno through its own chain).
 */
int corten_file_may(struct file *file, unsigned long prot,
		    unsigned long pgoff, unsigned long len, u8 *may_prot);

/*
 * Pure cursor arithmetic of the auto-arena window (sec 3.1): place a
 * PMD-rounded @len at the @next_va cursor.  Return 0 with *@addr set on
 * success, -ENOSPC when the window is exhausted (the caller degrades to
 * the legacy path, counted).
 */
int corten_arena_auto_place(unsigned long next_va, unsigned long len,
			    unsigned long *addr);

/*
 * do_mmap() hook (mm/mmap.c, first hook in the function): decide the
 * auto-arena takeover for one addr==0 anonymous private mapping of a MODE
 * process, run the mmap_region() verification checklist
 * (corten_auto_validate()), place a window and rewrite the request onto
 * it.  Runs with this mm's mmap_lock held for writing (do_mmap's
 * contract).
 *
 * V-B.1: a @file (classified CORTEN_MMAP_AUTO_FILE by the whitelist)
 * additionally passes corten_file_may() before placement and skips the
 * resident pool (a parked window's reactivation is the ANON reuse
 * contract -- a FILE mapping always takes a fresh window).
 *
 * Return: 2 = pool take (the mmap completes; do_mmap returns the window
 * address immediately), 1 = fresh window placed, *@addr / *@lenp /
 * *@flagsp rewritten (the caller completes with
 * corten_arena_auto_attach() or corten_arena_file_attach() -- V-A.2a:
 * WITHOUT mmap_region(); only a declare failure degrades to the legacy
 * MAP_FIXED flow), 0 = legacy (not a whitelist hit, gate refusal,
 * window exhausted, or an obstacle in the window), -errno = internal
 * error only.
 */
int corten_arena_auto_mmap_route(struct mm_struct *mm, struct file *file,
				 unsigned long pgoff, unsigned long len,
				 unsigned long prot, unsigned long *addr,
				 unsigned long *lenp, unsigned long *flagsp);

/*
 * V-A.2a: the mmap_region() verification checklist the auto takeover
 * must cover now that it does not run mmap_region() at all -- def_flags
 * VM_LOCKED (mlock_future_ok parity), the execute-only pkey shape, and
 * may_expand_vm() (RLIMIT_AS/RLIMIT_DATA; the total_vm charge itself
 * rides the declare's success path).  Pure over (mm, len, prot, flags).
 * Return: 0 = proceed, -errno = the legacy flow must answer this one.
 */
int corten_auto_validate(struct mm_struct *mm, unsigned long len,
			 unsigned long prot, unsigned long flags);

/*
 * do_mmap() completion hook (V-A.2a: runs where mmap_region() used to
 * be for the auto takeover): register the window at @addr as an auto
 * arena without creating any VMA -- DECLARE's locked body with the
 * detached carrier (V-A.2b), the frames and the metadata, plus the
 * total_vm charge mmap_region() used to make.  Safe because the caller
 * already holds the mmap_write lock that DECLARE would otherwise take
 * first (DEV-13).  Failure degrades to the caller's legacy flow: a
 * plain anonymous VMA at a window address (counted, harmless).
 * Return: 0 on success, -errno otherwise.
 */
int corten_arena_auto_attach(struct mm_struct *mm, unsigned long addr,
			     unsigned long len, unsigned long prot);

/*
 * V-B.1: the FILE counterpart of corten_arena_auto_attach() -- the
 * do_mmap() completion body for a CORTEN_MMAP_AUTO_FILE takeover.
 * Declares the window region (DECLARE's locked body, carrier arm) with
 * the FILE payload: the region record takes the file reference
 * (corten_region_register_file()), the carrier is born in FILE shape
 * (vm_file = rfile, vma_set_range() carries @pgoff, anon_vma_prepare()
 * kept -- private COW pages still need the avc anchor), the whole
 * region is virtually allocated CORTEN_FILE_MAPPED up front (a fault
 * must never fall into the FRESH synthesizer, which would hand out
 * anonymous zero pages for a file range; until B.3 the dispatch
 * answers STUB -> SEGV_MAPERR), and the carrier joins the mapping's
 * i_mmap interval tree (the __vma_link_file() shape, minus
 * mapping_allow_writable() -- VM_SHARED is never set).  The
 * total_vm/RLIMIT accounting mirrors the A.2a auto attach
 * (corten_auto_validate() ran in the route, the charge rides the
 * declare's success path).
 *
 * Safe because the caller (do_mmap) already holds the mmap_write lock
 * DECLARE would otherwise take first (DEV-13).  Failure degrades to
 * the caller's legacy flow (counted, harmless -- the established
 * attach-failure contract).
 * Return: 0 on success, -errno otherwise.
 */
int corten_arena_file_attach(struct mm_struct *mm, unsigned long addr,
			     unsigned long len, unsigned long prot,
			     struct file *file, unsigned long pgoff);

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
 * V-A.3a placement truth (MV_VMA_FREE_SPEC.md sec 1.2, audit #14-#17):
 * true if [start, start+len) intersects ANY registered frame of @mm --
 * a live arena, a parked (idle) arena or a magazine reserve sentinel all
 * count as occupied.  This answers "is this VA mine", while
 * corten_arena_range_overlaps() above answers "is there live arena state
 * here" (the 16+ reject-family hooks depend on that skip-idle meaning --
 * do not merge the two).  Same RCU walk shape and O(1) gates as the
 * active-only variant; callers hold mmap_write (the placement funnels).
 */
bool corten_arena_range_occupied_incl_idle(struct mm_struct *mm,
					   unsigned long start,
					   unsigned long len);

/*
 * V-A.3a P3 helper (the __mmap_prepare zero-VMA backstop branch,
 * mm/vma.c): the live+parked arena occupancy test (reserve sentinels
 * excluded -- a sentinel under a legacy VMA is a legal shape the magazine
 * re-verifies at serve time) plus the audit counter, one call.  True
 * (with corten_nr_placement_backstop bumped) = arena frames are still in
 * the registry although the range carries no VMA -- the placement guards
 * upstream (NOREPLACE -EEXIST, punch idle-eject) make this unreachable,
 * a hit is an audit event, not a semantic answer.  mmap_write context.
 */
bool corten_arena_placement_backstop(struct mm_struct *mm,
				     unsigned long start, unsigned long len);

/*
 * V-A.3a implant registry (D24): register [start, start+len) (clipped to
 * the window domain) as legacy-funnel-owned VA -- the producers are the
 * punch route's success arms and the P1b idle-eject, both under mmap_write;
 * @ctl_lock is taken inside.  Allocation failure degrades to an unmarked
 * implant (counted) -- a false J2 candidate, never a false clearance.
 * corten_implant_covers(): is [start, start+len) fully inside the union of
 * registered ranges?  Read under mmap_read or better.
 * corten_implant_covers_lockless(): the same predicate for RCU-section
 * readers (V-A.3d J1 exemption) -- the registry images are retired via
 * kfree_rcu and the count/pointer snapshot protocol is barrier-paired
 * with the mark() publisher; read the .c comment before adding callers.
 */
void corten_implant_mark(struct mm_struct *mm, unsigned long start,
			 unsigned long len);
bool corten_implant_covers(struct mm_struct *mm, unsigned long start,
			   unsigned long len);
bool corten_implant_covers_lockless(struct mm_struct *mm,
				    unsigned long start, unsigned long len);

/*
 * V-A.3c INV-MV2 audit walker (j2-audit hook list, MV_VMA_FREE_SPEC.md
 * sec 1.3 J2): one read-only pass over the mm's maple tree asserting
 * that every VMA intersecting the window domain is either the arena's
 * own (VM_CORTEN) or inside the implant registry above.  A violation
 * WARNs once, counts, and archives the first address; registry entries
 * carrying no tree VMA (mark-then-fail installs, munmapped implants)
 * count as stale -- a benign classification, never a WARN.
 *
 * corten_audit_j2_walk(): self-sufficient form for callers holding no
 * lock of @mm (the syscall-route tails, debugfs, KUnit) -- takes the
 * registry ctl_lock internally, which is also DEV-13-legal under a
 * held mmap_read/write.
 * corten_audit_j2_walk_locked(): for callers whose context already
 * fences every registry writer -- this mm's mmap_write held, the
 * registry ctl_lock held, or mm_users == 0.
 * Read-only in the INV6 sense: no PTE is walked or written.
 * Return: violations found in this pass (0 = INV-MV2 holds).
 */
int corten_audit_j2_walk(struct mm_struct *mm);
int corten_audit_j2_walk_locked(struct mm_struct *mm);

/*
 * V-A.3c debugfs backends (mm/corten.c owns the files): the "j2_walk
 * <pid>" manual trigger and the "j2_walk_every" hot-path sampling
 * switch (a static key, default off -- the park/take/reactivate/route
 * triggers only walk when it is on; mm_exit and fork_commit always
 * walk).
 */
int corten_arena_j2_walk_pid(pid_t pid);
void corten_arena_j2_sample_set(bool on);

/*
 * V-E whitelist classification (MV_VMA_FREE_SPEC.md sec 1.3 J2, the
 * complete form; sec 3.5): the J2-complete audit classifies *every*
 * tree VMA of a MODE mm, not only the window segment.  A VMA that
 * intersects the window domain must be the arena's own (SHADOW) or
 * inside the implant registry (IMPLANT) -- anything else is a
 * VIOLATION, the same invariant the A.3c walker asserts over the
 * window segment.  A VMA outside the window is delegated-domain
 * whitelist: the heap VMA (BRK, the sec 3.5 V-E.1 registration),
 * grows-flag stacks (STACK), arch-named specials vdso/vvar/vsyscall
 * (SPECIAL), any file mapping (FILE -- exec-time mappings and
 * MAP_SHARED alike), anonymous leftovers (ANON), or UNCLASSIFIED when
 * no predicate matched (disclosure-only: the legacy VMA layer
 * legitimately serves anything outside the window, so an unclassified
 * delegated VMA is a counting bucket, never a verdict).
 */
enum corten_wl_class {
	CORTEN_WL_SHADOW = 0,
	CORTEN_WL_IMPLANT,
	CORTEN_WL_BRK,
	CORTEN_WL_STACK,
	CORTEN_WL_SPECIAL,
	CORTEN_WL_FILE,
	CORTEN_WL_ANON,
	CORTEN_WL_UNCLASSIFIED,
	CORTEN_WL_VIOLATION,
	CORTEN_WL_NR_CLASSES,
};

/*
 * One whitelist pass.  Same read-only/locking contract as the J2
 * walkers above (RCU tree walk; the implants image must be stable).
 * Return: window-domain violations found (0 = the J2-complete form
 * holds; the heap-VMA anomaly -- more than one BRK-classified VMA in
 * one mm -- is counted into the wl ledger separately, never a WARN).
 */
int corten_audit_whitelist_walk(struct mm_struct *mm);
int corten_audit_whitelist_walk_locked(struct mm_struct *mm);

/* V-E debugfs backend (mm/corten.c owns the file): the "whitelist
 * <pid>" manual trigger, same shape as corten_arena_j2_walk_pid().
 */
int corten_arena_wl_audit_pid(pid_t pid);

/*
 * V-E brk delegation observation (spec sec 3.5): the sys_brk arm
 * counters.  sys_brk notes which of its four arms answered -- GROW
 * (do_brk_flags installed), SHRINK (do_vmi_align_munmap trimmed), NOOP
 * (break unchanged), REJECT (a refused request left the break alone) --
 * on every MODE-mm call; the OQ-MV-7 adjudication reads the split next
 * to the heap probe below to decide whether brk region-ization ever
 * becomes a project (no threshold crossed = the delegated-domain
 * verdict stands).  The arm enum itself lives in the shared header:
 * mm/mmap.c names it under both Kconfig faces.
 */
void corten_brk_note_slow(struct mm_struct *mm, enum corten_brk_arm arm);

static inline void corten_brk_note(struct mm_struct *mm,
				   enum corten_brk_arm arm)
{
	if (corten_enabled_static() && READ_ONCE(mm->corten_mode))
		corten_brk_note_slow(mm, arm);
}

/*
 * V-A.2a J1 prelude (MV_VMA_FREE_SPEC.md sec 1.3): the find_vma-family
 * window probe.  The exported find_vma()/find_vma_intersection() and
 * lock_vma_under_rcu() call corten_j1_probe() after their lookup; the
 * slow-path function counts the call (probes) and, when it *found* a
 * tree VMA overlapping the window domain, the hit (the J2 negative
 * probe: post-A.2 the only legal window find is a punch implant, which
 * the slice report discloses).  The inline gate pays one static-branch
 * read + one byte load for every mm -- the house double-gate shape.
 *
 * V-E appends the OQ-MV-7 heap arm: a MODE-mm lookup that missed the
 * window domain but landed in [start_brk, brk) counts as one
 * heap-domain find_vma -- the numerator of the heap-fault share the
 * brk adjudication measures (the denominator is the bpftrace total;
 * observation only, no behavior hangs off the count).  The brk fields
 * are written under mmap_write and read here with plain loads: a torn
 * read can only mis-bucket one call, and the metric is a ratio.
 */
void corten_j1_slow(struct mm_struct *mm, unsigned long start,
		    unsigned long end, struct vm_area_struct *vma);
void corten_j1_heap_note(struct mm_struct *mm);

static inline void corten_j1_probe(struct mm_struct *mm, unsigned long start,
				   unsigned long end,
				   struct vm_area_struct *vma)
{
	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode))
		return;
	if (end > CORTEN_MODE_WINDOW_START && start < CORTEN_MODE_WINDOW_END) {
		corten_j1_slow(mm, start, end, vma);
		return;
	}
	if (start >= READ_ONCE(mm->start_brk) && start < READ_ONCE(mm->brk))
		corten_j1_heap_note(mm);
}

/*
 * The untracked maple lookup for corten's own walkers (the placement
 * obstacle scan, the punch split scan, the fault tier-2 check): the
 * probe above counts external consumers, so the registered internal
 * ones must not go through it.  Defined in mm/mmap.c next to
 * find_vma_intersection(); mmap_write/read lock contract identical.
 */
struct vm_area_struct *corten_vma_find(struct mm_struct *mm,
				       unsigned long start,
				       unsigned long end);

/*
 * V-A.3b J1 hygiene (j2-audit #1/#2/#3/#7/#29): the window-domain
 * funnels that keep the probe pair above clean.  fault_window_maperr()
 * runs under the mmap_read just taken by lock_mm_and_find_vma() and
 * answers the parked/hole shape without the find_vma(); it returns
 * false for a live arena (the ownership-fallback shape must still walk
 * and serve the punch implant).  uffd_window_reject() is the mfill/move
 * entry short-circuit (-ENOENT, the C12 terminal verdict); the two note
 * helpers are observation-only counters for the V-C defect families.
 * The fast-hook arm (#1, corten_fault_window_fallback) lives in
 * include/linux/corten_arena.h next to its caller.
 */
bool corten_fault_window_maperr(struct mm_struct *mm, unsigned long addr);
bool corten_uffd_window_reject(struct mm_struct *mm,
			       unsigned long dst_start, unsigned long dst_len,
			       unsigned long src_start, unsigned long src_len);
void corten_gup_note_window_miss(struct mm_struct *mm, unsigned long addr);
void corten_remote_note_window_short(struct mm_struct *mm,
				     unsigned long addr);

/*
 * V-C GUP-slow MODE branch (j2-audit #3/#7/#8, MV_VMA_FREE_SPEC.md
 * sec 3.3.2).  corten_gup_probe() sits in front of gup_vma_lookup()'s
 * find_vma(): for a MODE mm's window-domain address it answers from
 * the region registry so the tree is never walked (J1 stays zero).
 *
 * Returns the region's carrier (the walk proceeds on it exactly like
 * it did on the shadow-VMA: check_vma_flags()'s corten_own arm, the
 * PTE follow and the faultin slow hook all take the carrier
 * verbatim), NULL when the address is not the window stream's to
 * answer (non-MODE, outside the window, or an implant range whose
 * real tree VMA find_vma() must find), or ERR_PTR(-EFAULT) for the
 * window shapes whose tree lookup is a guaranteed miss (parked S-1
 * window, magazine reserve, hole) -- the errno check_vma_flags()'s
 * own miss would have produced.  One emulation corner the carrier's
 * shape cannot express: FOLL_ANON on a FILE region answers -EFAULT
 * (check_vma_flags()' vma_is_anonymous() verdict).
 *
 * Carrier lifetime: the caller holds mmap_lock for read (the
 * __get_user_pages contract); carriers are created and freed only
 * under mmap_lock for writing, and an arena fault never returns
 * VM_FAULT_RETRY (the mm/memory.c slow-hook contract), so the carrier
 * pointer is never carried across a lock drop.
 */
struct vm_area_struct *corten_gup_probe(struct mm_struct *mm,
					unsigned long addr,
					unsigned int gup_flags);

/*
 * V-C #7/#8: true for a MODE mm's window-domain address that no tree
 * VMA can cover (not an implant) -- __access_remote_vm() and
 * __copy_remote_vm_str() skip their vma_lookup()+expand_stack()
 * pre-checks there (a guaranteed miss, and expand_stack() would drop
 * the mmap_read on failure) and let the GUP loop's probe answer.
 */
bool corten_remote_vm_window(struct mm_struct *mm, unsigned long addr);

/*
 * V-A.2a: the window-domain fence for the generic gap walkers
 * (mm/mmap.c generic_get_unmapped_area{,_topdown}() and the x86 twins
 * in arch/x86/kernel/sys_x86_64.c).  With the live and parked windows
 * both tree-free, the allocator's gap scan can no longer be trusted to
 * stay out of the window domain on its own -- the fence pins every
 * hint-less allocation to the delegator side of the dominion line
 * (topdown searches below the window, bottom-up above it), which is
 * exactly sec 1.2's "window outside, delegated domain" contract.
 * MAP_FIXED bypasses the walkers by design (the punch/implant routes
 * own those shapes); an explicit hint into the window fails -ENOMEM
 * via the walker's limit check.
 */
static inline void corten_fence_unmapped_area(struct vm_unmapped_area_info *info)
{
	if (!corten_enabled_static() ||
	    !READ_ONCE(current->mm->corten_mode))
		return;

	if (info->low_limit >= CORTEN_MODE_WINDOW_END ||
	    info->high_limit <= CORTEN_MODE_WINDOW_START)
		return;		/* no window intersection */

	if (info->flags & VM_UNMAPPED_AREA_TOPDOWN)
		info->high_limit = CORTEN_MODE_WINDOW_START;
	else
		info->low_limit = CORTEN_MODE_WINDOW_END;
}

/* The matching hint check: a hinted range overlapping the window must
 * not take the fast accept path (the range is tree-free there).
 */
static inline bool corten_addr_in_window(unsigned long addr,
					 unsigned long len)
{
	return corten_enabled_static() &&
	       READ_ONCE(current->mm->corten_mode) &&
	       addr < CORTEN_MODE_WINDOW_END &&
	       addr + len > CORTEN_MODE_WINDOW_START;
}

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
 * V-A.3d S-5 (j2-audit #13/#23/#28, D24): the query-syscall window
 * terminals.  All three run under the caller's mmap_read of @mm and
 * are pure reads (INV6).
 *
 * corten_arena_msync_skip(): advance @start past the leading
 * registered (active-or-parked) window segment; returns @start
 * unchanged when the leading frame is a hole/implant or @start is
 * outside the window.  sys_msync() calls it before each find_vma()
 * and treats a fully-skipped range as the anonymous no-op (0).
 *
 * corten_arena_mincore_route(): answer one do_mincore() chunk whose
 * window frames are all registered -- parked frames -> zero vector,
 * active frames -> the real residency vector read off the page tables
 * (present 1, swap per the swap-cache truth, none 0).  Returns the
 * byte count, or -EAGAIN for the legacy tree walk (hole/implant
 * frames, non-MODE mm, chunk outside the window).
 *
 * corten_arena_move_pages_window(): true when do_pages_stat_array()
 * should answer -EFAULT for @addr without the vma_lookup() (a MODE
 * mm's window address that is not implant-covered); false keeps the
 * legacy lookup.
 */
unsigned long corten_arena_msync_skip(struct mm_struct *mm,
				      unsigned long start,
				      unsigned long end);
long corten_arena_mincore_route(struct mm_struct *mm, unsigned long addr,
				unsigned long pages, unsigned char *vec);
bool corten_arena_move_pages_window(struct mm_struct *mm,
				    unsigned long addr);

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

/*
 * M6.T1 reclaim-path guards (M6_RMAP_SPEC.md sec 1.3 V1/V2, sec 2.1
 * D1/D3): keep the bare reclaim writers off arena PTEs.  Both are one
 * flag test on their common paths (VM_CORTEN is only ever set by
 * shadowize on a corten=on kernel).
 */

/*
 * V1: the OOM reaper calls this for each VMA it would unmap_page_range().
 * Return: true = shadow-VMA, skip it (counted); the arena memory is
 * reclaimed by the transaction-ordered exit_mmap() teardown instead.
 */
bool corten_oom_reap_skip_vma(struct vm_area_struct *vma);

/*
 * V2: the rmap-walker guard / transaction slow path (spec D1 interface).
 * Called from try_to_unmap_one()/try_to_migrate_one() behind the
 * corten_enabled_static() && VM_CORTEN gate, holding the folio lock and
 * reference, before the notifier invalidation window opens.
 * Return: true = the shape can be taken transactionally -- the walker
 * opens its notifier window and calls corten_rmap_swap_out() (M6.T2);
 * false = declined -- the walker aborts without writing anything and
 * the folio stays resident (hwpoison/migration shapes, mlocked VMAs,
 * pinned folios, folios without a swap entry: reclaim without an entry
 * would destroy content).  Counted (rmap_rejects) on every refusal.
 */
bool corten_rmap_unmap_one(struct folio *folio, struct vm_area_struct *vma,
			   unsigned long address, enum ttu_flags flags,
			   bool migrate);

/*
 * The M6.T2 completion arm (spec D1 steps 4-6): swap the arena page out
 * transactionally.  Called from try_to_unmap_one() INSIDE its
 * mmu_notifier invalidate window, still holding the folio lock and
 * reference.  Runs corten_lock_range() > ptl (DEV-13 direction), installs
 * the swap PTE, moves the counters, removes the rmap and rewrites the
 * metadata to CORTEN_SWAPPED with the entry encoded in __resv -- all
 * under the covering desc write lock.
 *
 * @defer: the caller's should_defer_flush() verdict.  When true the
 * transaction skips flush_tlb_range() and hands the cleared PTE back in
 * *@old_pte so the walker can do the upstream set_tlb_ubc_flush_pending()
 * bookkeeping (R6-1: the batching statics are rmap.c-local; registering
 * after the transaction is a happens-after strengthening of the upstream
 * clear-before-register order -- both flush by address, and the swap PTE
 * installed in between is read only through the ptl the transaction
 * holds).
 *
 * Return: true = this VMA side is swapped out (PTE holds the entry,
 * metadata = CORTEN_SWAPPED); false = declined inside the transaction
 * (metadata shape, PTE mismatch, -EAGAIN/-ENOMEM), nothing written.
 */
bool corten_rmap_swap_out(struct folio *folio, struct vm_area_struct *vma,
			  unsigned long address, bool defer, pte_t *old_pte);

/*
 * M6.T2 swapoff parity (spec 1.2 P12): unuse_pte() replaces a swap PTE
 * with a present PTE holding @folio (already locked, I/O settled).  On a
 * shadow-VMA the metadata must follow (CORTEN_SWAPPED -> CORTEN_MAPPED)
 * or the next fault would dispatch a swap-in for a resident page.
 * Called from unuse_pte() before its pte_offset_map_lock() so the
 * DEV-13 direction (desc write lock > ptl) is preserved; @entry
 * identifies the page in the metadata payload.
 *
 * Return: 0 on success (or nothing to do), -EAGAIN on a transient race
 * (the caller ignores it; the fault path self-heals the leftover shape),
 * -ENOMEM when the transaction cannot be locked.
 */
int corten_swapin_sync_meta(struct mm_struct *mm, unsigned long addr,
			    swp_entry_t entry, struct folio *folio);

/*
 * W1.a: vma-free rmap bookkeeping (W1_NATIVE_RMAP_SPEC.md sec 2.2), the
 * order-0 mirrors of the add/remove rmap family with the vma-dependent
 * parts (anon mapping/index, mlock_vma) left to the caller's transaction.
 * Call sites pair add/remove symmetrically; the wrappers own mapcount,
 * the lruvec stat bucket (explicit, not folio_test_anon() -- a novma
 * folio has no mapping to dispatch on), swapbacked and AnonExclusive.
 */
void folio_add_anon_rmap_novma(struct folio *folio);
void folio_remove_anon_rmap_novma(struct folio *folio);
void folio_add_file_rmap_novma(struct folio *folio);
void folio_remove_file_rmap_novma(struct folio *folio);

/*
 * M6.T2 eviction driver (spec slice table: "debugfs evict N pages
 * entry, T2 ships a minimal shrink stub"): pick up to @nr resident,
 * unshared, unpinned arena pages of the process @pid and push them
 * through the upstream reclaim machinery (folio_alloc_swap ->
 * try_to_unmap -> the swap-out transaction above -> swap_writeout).
 * The real pressure channel (shrinker + aging) is M6.T3; this entry is
 * the correctness driver for the swap round-trip and the guest
 * acceptance path.
 *
 * Called from the debugfs "evict" control file.  Return: the number of
 * pages that reached CORTEN_SWAPPED, or a negative error.
 */
int corten_arena_evict_pid(pid_t pid, int nr);

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

static inline void corten_arena_unmap_file_range(struct address_space *mapping,
						 pgoff_t first_index,
						 pgoff_t last_index,
						 bool even_cows)
{
}

static inline bool corten_arena_imap_stale_guard(struct vm_area_struct *vma)
{
	return false;
}

static inline bool corten_zap_single_guard(struct vm_area_struct *vma)
{
	return false;
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

static inline bool
corten_arena_range_occupied_incl_idle(struct mm_struct *mm,
				      unsigned long start, unsigned long len)
{
	return false;
}

static inline bool
corten_arena_placement_backstop(struct mm_struct *mm, unsigned long start,
				unsigned long len)
{
	return false;
}

static inline void corten_implant_mark(struct mm_struct *mm,
				       unsigned long start, unsigned long len)
{
}

static inline bool corten_implant_covers(struct mm_struct *mm,
					 unsigned long start,
					 unsigned long len)
{
	return false;
}

static inline bool corten_implant_covers_lockless(struct mm_struct *mm,
						  unsigned long start,
						  unsigned long len)
{
	return false;
}

/* V-A.3c INV-MV2 walker + debugfs backends: no window domain exists. */
static inline int corten_audit_j2_walk(struct mm_struct *mm)
{
	return 0;
}

static inline int corten_audit_j2_walk_locked(struct mm_struct *mm)
{
	return 0;
}

static inline int corten_arena_j2_walk_pid(pid_t pid)
{
	return -EOPNOTSUPP;
}

static inline void corten_arena_j2_sample_set(bool on)
{
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

/* V-A.3d S-5 terminals: no window domain exists. */
static inline unsigned long corten_arena_msync_skip(struct mm_struct *mm,
						    unsigned long start,
						    unsigned long end)
{
	return start;
}

static inline long
corten_arena_mincore_route(struct mm_struct *mm, unsigned long addr,
			   unsigned long pages, unsigned char *vec)
{
	return -EAGAIN;
}

static inline bool corten_arena_move_pages_window(struct mm_struct *mm,
						  unsigned long addr)
{
	return false;
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

static inline bool corten_oom_reap_skip_vma(struct vm_area_struct *vma)
{
	return false;
}

static inline bool corten_rmap_unmap_one(struct folio *folio,
					 struct vm_area_struct *vma,
					 unsigned long address,
					 enum ttu_flags flags, bool migrate)
{
	return false;
}

static inline bool corten_rmap_swap_out(struct folio *folio,
					struct vm_area_struct *vma,
					unsigned long address, bool defer,
					pte_t *old_pte)
{
	return false;
}

/* W1.a novma rmap wrappers: no arena, nothing borrows folio->_mapcount. */
static inline void folio_add_anon_rmap_novma(struct folio *folio)
{
}

static inline void folio_remove_anon_rmap_novma(struct folio *folio)
{
}

static inline void folio_add_file_rmap_novma(struct folio *folio)
{
}

static inline void folio_remove_file_rmap_novma(struct folio *folio)
{
}

static inline int corten_swapin_sync_meta(struct mm_struct *mm,
					  unsigned long addr,
					  swp_entry_t entry,
					  struct folio *folio)
{
	return 0;
}

static inline int corten_arena_evict_pid(pid_t pid, int nr)
{
	return -EOPNOTSUPP;
}

static inline int corten_arena_auto_mmap_route(struct mm_struct *mm,
					       struct file *file,
					       unsigned long pgoff,
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
					   unsigned long len,
					   unsigned long prot)
{
	return 0;
}

static inline int corten_arena_file_attach(struct mm_struct *mm,
					   unsigned long addr,
					   unsigned long len,
					   unsigned long prot,
					   struct file *file,
					   unsigned long pgoff)
{
	return 0;
}

static inline int corten_file_may(struct file *file, unsigned long prot,
				  unsigned long pgoff, unsigned long len,
				  u8 *may_prot)
{
	return 0;
}

static inline void corten_j1_probe(struct mm_struct *mm, unsigned long start,
				   unsigned long end,
				   struct vm_area_struct *vma)
{
}

/* V-E brk arm note: no MODE mm can exist, never taken. */
static inline void corten_brk_note(struct mm_struct *mm,
				   enum corten_brk_arm arm)
{
}

static inline struct vm_area_struct *corten_vma_find(struct mm_struct *mm,
						     unsigned long start,
						     unsigned long end)
{
	return NULL;
}

/* V-A.3b J1-hygiene funnels: no MODE mm can exist, never taken. */
static inline bool corten_fault_window_maperr(struct mm_struct *mm,
					      unsigned long addr)
{
	return false;
}

static inline bool
corten_uffd_window_reject(struct mm_struct *mm, unsigned long dst_start,
			  unsigned long dst_len, unsigned long src_start,
			  unsigned long src_len)
{
	return false;
}

static inline void corten_gup_note_window_miss(struct mm_struct *mm,
					       unsigned long addr)
{
}

static inline void corten_remote_note_window_short(struct mm_struct *mm,
						   unsigned long addr)
{
}

static inline struct vm_area_struct *
corten_gup_probe(struct mm_struct *mm, unsigned long addr,
		 unsigned int gup_flags)
{
	return NULL;
}

static inline bool corten_remote_vm_window(struct mm_struct *mm,
					   unsigned long addr)
{
	return false;
}

struct vm_unmapped_area_info;

static inline void corten_fence_unmapped_area(struct vm_unmapped_area_info *info)
{
}

static inline bool corten_addr_in_window(unsigned long addr,
					 unsigned long len)
{
	return false;
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
