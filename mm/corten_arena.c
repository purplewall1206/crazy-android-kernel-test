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
#include <linux/mempolicy.h>
#include <linux/memfd.h>		/* memfd_check_seals_mmap (V-B.1 may) */
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
#include <linux/pgalloc.h>	/* __pte_free_tlb (V-A.1 PT retirement) */
#include <linux/pgtable.h>
#include <linux/rmap.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/shmem_fs.h>	/* shmem_get_folio/SGP_CACHE (V-B.3 fetch) */
#include <linux/shrinker.h>	/* shrinker_alloc/register (M6.T3) */
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/swapops.h>	/* swp_entry_to_pte()/pte_swp_* (M6.T2) */
#include <linux/time.h>		/* ktime (M6.T4 rate lines) */
#include <linux/uaccess.h>

#include <asm/tlb.h>

#include <uapi/linux/mman.h>
#include <uapi/linux/prctl.h>

#include "internal.h"		/* vma_set_range() (V-A.2b carrier) */
#include "corten.h"		/* corten_meta_ensure_locked() */
#include "corten_arena.h"	/* S4-S7 internal interface */
#include "swap.h"		/* swap_writeout/readahead (M6.T2) */
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

/* Claimed-segment ledger entry: base/end of one cpu's magazine segment.
 * Walked on RELEASE (marker restoration) and in the debugfs report --
 * both cold paths.
 */
struct corten_va_segrec {
	struct list_head	list;
	unsigned long		base;
	unsigned long		end;
};

/* M4.T1 magazine internals, defined in the magazine section below. */
static int corten_va_mag_alloc_cpu(struct mm_struct *mm,
				   struct corten_mm_state *state,
				   unsigned long len2, unsigned long *addr2,
				   int cpu);
static void corten_va_release_frame(struct corten_mm_state *state,
				    unsigned long addr);

/* V-A.3c INV-MV2 audit walker samples, defined in the audit section
 * below (near the implant registry they read).
 */
static void corten_audit_j2_sample(struct mm_struct *mm);
static void corten_audit_j2_sample_locked(struct mm_struct *mm);

/* T1c resident arena pool, defined in the pool section below. */
static int corten_arena_pool_prepare_locked(struct mm_struct *mm,
					    struct corten_mm_state *state,
					    unsigned long addr,
					    unsigned long len,
					    u8 perm, bool novma, bool no_reuse);
static int corten_arena_pool_take(struct mm_struct *mm,
				  struct corten_mm_state *state,
				  unsigned long len2, unsigned long prot,
				  unsigned long *addr);
static int corten_arena_pool_release(struct mm_struct *mm, unsigned long start,
				     unsigned long len);
static void corten_arena_pool_flush_locked(struct mm_struct *mm,
					   struct corten_mm_state *state);

/* V-A.1 helpers, defined in the sections below. */
static vm_flags_t corten_take_vm_flags(u8 perm);
static int corten_arena_unmap_chunk_flags(struct mm_struct *mm,
					  struct corten_arena *ar,
					  unsigned long start, unsigned long len,
					  u8 zflags, struct mmu_gather *tlb);

/* V-B.3 (H7 follow-through): the file-event invalidation family
 * (!even_cows, the reclaim arm) spares the private copies -- only the
 * FILE_MAPPED slots are that event's business (should_zap_cows()'s
 * verdict).  Internal to the zap driver: masked before corten_unmap(),
 * which only accepts CORTEN_UNMAP_ALL.
 */
#define CORTEN_UNMAP_FILE_EVENT		BIT(2)

/* V-A.2b detached carrier, defined in the shadow-VMA section below. */
static void corten_arena_carrier_free(struct vm_area_struct *vma);

/* V-B.1 file-may MAY bound, defined in the auto-mmap section below. */
static u8 corten_file_may_bound(struct file *file);

/* V-B.1 FILE payload teardown, defined in the region-record section. */
static void corten_region_file_teardown(struct corten_arena *ar);

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
 * W1.b (W1_NATIVE_RMAP_SPEC.md sec 4.1): the global mapping ->
 * corten_inode_regions index, keyed by the address_space pointer.  A
 * head exists exactly while at least one published FILE region of that
 * mapping is alive: every region holds a file reference (rfile), which
 * pins the inode and its mapping, and the head is erased and freed when
 * its last region unlinks (under i_mmap_rwsem, so the entry can never
 * outlive nor preempt its mapping's lifetime).  All insert/erase happen
 * under the mapping's i_mmap_rwsem for writing -- the lock critical
 * sections of the interval-tree membership this registry replaced --
 * so the key space is stable while any writer holds it; the readers
 * (the invalidation enumeration) hold i_mmap_rwsem for reading, the
 * identical discipline the tree walk ran.  xa_lock is only ever taken
 * inside these i_mmap_write sections (xa_store/xa_erase internals) --
 * one directed edge i_mmap > xa_lock, nothing takes the reverse, no
 * cycle (INV2; the F8 edge order survives verbatim).
 */
static DEFINE_XARRAY(corten_inode_regions);

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
/* V-B.2 (H7) / W1.b: file-side unmap events (truncate / invalidation)
 * routed into the arena chunk-zap transaction -- since W1.b counted at
 * the per-inode registry enumeration (one per intersecting region), not
 * at the vma-keyed gate.  The two backstops behind it must stay 0
 * forever: a VM_CORTEN VMA arriving at the bare legacy writer
 * (zap_page_range_single), and a VM_CORTEN VMA surfacing through the
 * mapping's i_mmap walk (a stale interval-tree node -- carriers stopped
 * joining the tree in W1.b).
 */
static atomic_long_t corten_nr_truncate_routes;	/* file events routed */
static atomic_long_t corten_nr_zap_single_refuses; /* backstop firings */
static atomic_long_t corten_nr_imap_stale_refuses; /* stale-node backstop */
/* W1.d (W1_NATIVE_RMAP_SPEC.md sec 3.2): window file mappings demoted by
 * the try_to_unmap() hook -- the reclaim family of the file-event
 * consumers (the truncate/invalidation family above shares the same
 * registry enumeration and B.2 semantics).  One per demoted window slot,
 * so a folio mapped by several regions counts per mapper.
 */
static atomic_long_t corten_nr_ttu_routes;	/* ttu file demotions */
/* V-B.4 (H10): the FILE observability ledger.  Regions attached through
 * the takeover, read-arm installs, private COW copies off the file side
 * (both the in-place and the fetch+copy shapes), and fork mirrors of a
 * FILE region (the child's own rfile reference) -- the counters the
 * dlopen-shaped guest gate reads next to fork_faithful.
 */
static atomic_long_t corten_nr_file_mmaps;	/* FILE regions attached */
static atomic_long_t corten_nr_file_read_faults;/* read-arm installs */
static atomic_long_t corten_nr_file_cow_copies;	/* private copies off file */
static atomic_long_t corten_nr_file_fork_mirrors;/* fork child mirrors */
/* M5.T3 (M5_FORK_SPEC.md sec 4.3): zap_window() released a PTE whose
 * folio is FOLL_PIN/DMA-pinned.  The release itself is refcount-native
 * (the pin reference carries the folio until unpin, exactly like the
 * legacy zap), so this counts, not refuses: M6 migration/reclaim must
 * skip pinned pages, and this is the frequency evidence for that gate.
 */
static atomic_long_t corten_nr_zap_pinned;	/* zapped while pinned */

static void corten_arena_note_zap_pinned(void)
{
	atomic_long_inc(&corten_nr_zap_pinned);
}

/* M6.T1 reclaim-path guards (M6_RMAP_SPEC.md sec 1.3 V1/V2): external
 * walkers that found an arena page and were refused.  Global aggregates
 * like the route counters above: the walker's mm may die at any moment
 * and debugfs cannot enumerate per-mm states.  A non-zero reap_skips is
 * the OOM-pressure evidence.
 *
 * M6.T2 re-scopes rmap_rejects: the ttu guard gained the swap-out
 * completion arm, so reclaim legitimately reaches arena folios now
 * (swapped_out is the evidence).  The counter is the per-shape
 * refuse-and-keep tally -- hwpoison, migration caller, mlock, pin,
 * folio without a swap entry, order != 0 -- where the walker declines
 * the folio and it stays resident.
 */
static atomic_long_t corten_nr_reap_skips;	/* oom_reaper shadow-VMA skips */
static atomic_long_t corten_nr_rmap_rejects;	/* ttu walker refusals (V2) */
/* M6.T2 swap transaction observability (M6_RMAP_SPEC.md sec 2.1 D1/D5):
 * swapped_out/swapins are the zram round-trip evidence the guest
 * acceptance reads; swapin_retries counts unlock/relock re-queries of
 * the swap-in transaction (every "world changed while unlocked" race),
 * swapin_heals the "a bare writer already installed a present PTE"
 * self-repair (the swapoff-parity shape), zap_swap_frees the entries
 * released by the consumer-side zaps.  Global atomics like the
 * reclaim-path counters: the walker's mm may die at any moment.
 */
static atomic_long_t corten_nr_swapped_out;
static atomic_long_t corten_nr_swapins;
static atomic_long_t corten_nr_swapin_retries;
static atomic_long_t corten_nr_swapin_heals;
static atomic_long_t corten_nr_zap_swap_frees;
/* W1.e1 native swap-out driver attribution (W1_NATIVE_RMAP_SPEC.md sec
 * 3.4): driver_swapped counts the folios the driver freed end to end
 * (entry -> swapcache -> transaction -> writeout -> release), driver_kept
 * the picks it returned resident (every keep arm, counted where the ttu
 * path's "kept" bookkeeping used to be implicit in __reclaim_pages()).
 * The W1.f planned "driver_swaps" counter is this driver_swapped.
 */
static atomic_long_t corten_nr_driver_swapped;
static atomic_long_t corten_nr_driver_kept;
/* M4.T1 magazine observability: segments claimed (one per cpu per mm,
 * amortized over CORTEN_VA_SEG_FRAMES allocations), frames skipped at
 * allocation because a punch erased their reserve markers (they are
 * conservatively leaked rather than handed out over a foreign mapping),
 * and frames served from the recycle list (real VA reuse -- the T0-R2
 * follow-up the magazine enables).
 */
static atomic_long_t corten_nr_seg_claims;	/* per-cpu segments claimed */
static atomic_long_t corten_nr_mag_skips;	/* marker-lost frames skipped */
static atomic_long_t corten_nr_va_recycles;	/* frames from va_free */
/* T1c resident arena pool (M4T12_T1C): full-coverage munmaps of
 * MODE-process arenas that parked instead of releasing, auto mmaps
 * served from the park (vs. pool_misses: scans that found no candidate),
 * munmaps that found the pool full and took the real RELEASE (the D12
 * fallback, the pre-pool behaviour), and parked arenas deregistered for
 * cause (interference while parked, or a reuse candidate that failed
 * validation).
 */
static atomic_long_t corten_nr_pool_parks;
static atomic_long_t corten_nr_pool_hits;
static atomic_long_t corten_nr_pool_misses;
static atomic_long_t corten_nr_pool_over;
static atomic_long_t corten_nr_pool_ejects;
/* V-A.1: parks whose reservation-VMA removal hit memory pressure and
 * degraded to the real RELEASE (the sec 3.1.1 failure arm).
 */
static atomic_long_t corten_nr_park_unmap_fails;
/* V-A.2a: auto takeovers the pre-placement validation gate refused
 * (def_flags mlock / execute-only pkey / RLIMIT_AS) -- the request
 * degraded to the legacy flow, which delivers the identical verdict
 * through the full mmap_region() accounting chain.
 */
static atomic_long_t corten_nr_auto_vgate;

/* V-A.2a J1 prelude (MV_VMA_FREE_SPEC.md sec 1.3): find_vma-family
 * calls on a MODE mm whose query range intersects the window domain.
 * Every corten-internal walker reaches the maple tree through the
 * untracked alias below, so the counters see only non-corten
 * consumers: j1_probes counts the calls (the literal J1 metric -- the
 * legacy funnel's miss-leg on parked windows is expected and benign,
 * semantic change S-1), j1_hits counts the calls that *found* a tree
 * VMA overlapping the window (post-A.2 the only legal shape is a
 * punch implant; anything else is a dominion violation -- J2's
 * negative probe).  The full J1/J2 audit walker lands with V-A.3.
 */
static atomic_long_t corten_nr_j1_probes;
static atomic_long_t corten_nr_j1_hits;

/* V-A.3a placement-surface disclosure counters (audit #14-#17): both are
 * "normally zero" -- a non-zero value means a defensive layer, not a
 * semantic route, answered a placement request.  placement_backstop: the
 * __mmap_prepare zero-VMA arm fired (the NOREPLACE/-EEXIST and punch
 * idle-eject guards upstream make it unreachable); p4_ejects: a parked
 * window was handed out with a foreign VMA inside (a placement guard
 * failed upstream) and the P4 assertion ejected instead.
 */
static atomic_long_t corten_nr_placement_backstop;
static atomic_long_t corten_nr_p4_ejects;
/* V-A.3a P1b arm firings: plain-MAP_FIXED placements that ejected
 * parked arenas to admit the mapping (the D24 eject-and-admit shape).
 */
static atomic_long_t corten_nr_placement_idle_ejects;
/* V-A.3a: implant registrations dropped on allocation failure (the
 * implant stays functional, only the J2 whitelist entry is lost).
 */
static atomic_long_t corten_nr_implant_drops;

/* V-D (MV_VMA_FREE_SPEC.md sec 3.4, the B-2 closure ledger): upper-table
 * pages the exit walk self-retired, per level.  Every fill_upper() runs
 * the standard pX_alloc() funnels, so each page is pgtables_bytes
 * accounted on the way in; these counters prove the way out -- a MODE
 * lifecycle that ends with pgtables_bytes == 0 must show pmds+puds
 * (a runtime-unfolded p4d adds p4ds) matching the window footprint,
 * and the guest gate greps them next to the zero-BUG-line criterion.
 */
static atomic_long_t corten_nr_exit_upper_pmds;
static atomic_long_t corten_nr_exit_upper_puds;
static atomic_long_t corten_nr_exit_upper_p4ds;
/* V-D (S-3 disclosure): swapoff unuse passes that visited an mm whose
 * window domain they cannot walk -- unuse_mm() is VMA-bounded and the
 * post-A.2 carrier windows are tree-free, so their swap entries ride
 * until a fault or the exit walk releases them.  Counted per unuse_mm()
 * visit with arenas registered; the behavior characterization itself is
 * the guest retest's (spec sec 4, S-3).
 */
static atomic_long_t corten_nr_unuse_blind_mms;

/* V-A.3b J1-hygiene observation counters (audit #1/#2/#3/#7/#29): the
 * window-domain funnels around the J1 probe pair.  fault_fallback_window
 * counts the two fault short-circuit arms -- the fast hook's diversion
 * (#1) and the slow path's terminus (#2): one parked/hole window fault
 * traverses both, so the number counts arm firings, not faults.
 * uffd_window_reject counts mfill/move entries answered -ENOENT (#29).
 * gup_window_miss and remote_access_window_short quantify the #3/#7
 * defect families until V-C's corten_gup_probe routes them; they are
 * disclosure-only, no behavior hangs off any of the four.
 */
static atomic_long_t corten_nr_fault_fallback_window;
static atomic_long_t corten_nr_uffd_window_reject;
static atomic_long_t corten_nr_gup_window_miss;
static atomic_long_t corten_nr_remote_access_window_short;

/* V-A.3c INV-MV2 walker ledger (MV_VMA_FREE_SPEC.md sec 1.3 J2): walks
 * executed, window-domain tree VMAs found outside the implant registry
 * (a dominion violation -- must stay 0), and stale registry entries
 * (registered but carrying no tree VMA: a mark whose mmap never
 * installed, or an implant later munmapped -- the A.3b handoff's
 * benign classification, counted so the walker can collect the shape
 * instead of misreporting it).  j2_first_violation archives the first
 * violating address ever seen (0 = none) for the debugfs report.
 */
static atomic_long_t corten_nr_j2_walks;
static atomic_long_t corten_nr_j2_violations;
static atomic_long_t corten_nr_j2_stale;
static atomic_long_t corten_j2_first_violation;

/* V-A.3d S-5 terminal answers (j2-audit #13/#20/#23/#28, the D24
 * verdicts): the three query syscalls whose window-domain legs A.1's
 * reservation-VMA retirement turned from "anonymous no-op success" into
 * -ENOMEM, plus the move_pages stat leg's cheap window short-circuit.
 * All four are disclosure-only (each syscall's return value carries the
 * semantics); the guest exit gate reads them next to j1_probes to
 * attribute any surviving window probes.
 */
static atomic_long_t corten_nr_msync_window_skips;	/* msync segs answered 0 */
static atomic_long_t corten_nr_mincore_routes;	/* mincore chunks answered */
static atomic_long_t corten_nr_madvise_parked;	/* madvise parked-terminal 0s */
static atomic_long_t corten_nr_move_pages_window; /* stat legs short-circuited */

/* V-C observation (MV_VMA_FREE_SPEC.md sec 3.3): GUP-slow window
 * probes answered with a carrier (the io_uring/9p/process_vm/ptrace
 * data face restored), the loud window rejects (parked/hole -- the
 * errno find_vma()'s miss would have produced), and the window rows
 * the dual-source /proc readers produced (maps/smaps/rollup/numa and
 * the PROCMAP_QUERY resolutions share the row stream).
 */
static atomic_long_t corten_nr_gup_probes;	/* carrier answers */
static atomic_long_t corten_nr_gup_probe_rejects; /* window -EFAULTs */
static atomic_long_t corten_nr_maps_window_rows;	/* rows rendered */

/* V-E brk delegation ledger (MV_VMA_FREE_SPEC.md sec 3.5, OQ-MV-7):
 * sys_brk arm answers on MODE mms -- one of the four arms per call,
 * zero for every non-MODE mm -- plus the heap-domain find_vma-family
 * lookups the J1 probe's window miss routed to the heap arm (the
 * numerator of the heap-fault share; the denominator is the guest
 * bpftrace total, the J1 (a)/(b) dual-caliber shape).  Observation
 * only: nothing in the brk path reads them back.
 */
static atomic_long_t corten_nr_brk_grow;
static atomic_long_t corten_nr_brk_shrink;
static atomic_long_t corten_nr_brk_noop;
static atomic_long_t corten_nr_brk_reject;
static atomic_long_t corten_nr_heap_lookups;

/* V-E whitelist ledger (spec sec 1.3 J2, the complete form): walks
 * executed, window-domain classification violations (must stay 0 --
 * same invariant as j2_violations, asserted over the full tree), heap
 * VMAs classified (the sec 3.5 "brk VMA explicitly registered"
 * observable), delegated VMAs classified, the disclosure bucket for
 * delegated VMAs no predicate matched, and the anomaly count for an mm
 * whose tree carries more than one heap VMA (a split heap cannot be
 * produced by sys_brk; a positive count is a classification smell to
 * read alongside j2_stale, never a WARN).
 */
static atomic_long_t corten_nr_wl_walks;
static atomic_long_t corten_nr_wl_violations;
static atomic_long_t corten_nr_wl_brk_vmas;
static atomic_long_t corten_nr_wl_delegated_vmas;
static atomic_long_t corten_nr_wl_unclassified;
static atomic_long_t corten_nr_wl_brk_anomalies;

/* V-A.2b: detached carrier VMAs created (cumulative; the live count is
 * the arenas ledger minus the parked/pool descriptors).
 */
static atomic_long_t corten_nr_carriers;

/* M6.T3 shrinker pressure channel (M6_RMAP_SPEC.md sec 2.1 D2):
 * shrink_scans counts scan_objects() invocations, aging_passes the
 * pass-1 window ages (each is one two-pass aging transaction opening),
 * shrink_swapped the pages that reached swap through the shrinker
 * (subset of swapped_out), shrink_skipped the folio-level refusals of
 * the victim walker (pin/writeback/order -- the same gate families the
 * ttu guard counts as rmap_rejects upstream of it), and evict_busy the
 * evict-vs-shrinker mutual exclusions (the per-mm shrink trylock).
 * Global atomics like every reclaim-path counter: the scanned mm may
 * die at any moment and debugfs cannot enumerate states.
 */
static atomic_long_t corten_nr_shrink_scans;
static atomic_long_t corten_nr_aging_passes;
static atomic_long_t corten_nr_shrink_swapped;
static atomic_long_t corten_nr_shrink_skipped;
static atomic_long_t corten_nr_evict_busy;

/* M6.T4 swap-rate lines: arena_stats readers get pages/s deltas between
 * consecutive reads.  The snapshot is guarded because readers are not
 * serialized against each other.
 */
static atomic_long_t corten_rate_last_out;
static atomic_long_t corten_rate_last_in;
static ktime_t corten_rate_ts;
static DEFINE_SPINLOCK(corten_rate_lock);

/* The M6.T3 per-mm shrinker registry (spec D2/OQ-M6-1): one global RCU
 * list of the published corten_mm_state nodes.  Membership is created
 * in corten_arena_state_create() (under corten_arena_alloc_lock, so it
 * is atomic with the mm->corten_state publication) and removed at the
 * start of the exit teardown; the kfree of the state waits for a grace
 * period after the unlink, so shrinker readers can carry the node
 * pointer across their own RCU sections safely.
 */
static DEFINE_SPINLOCK(corten_mm_registry_lock);
static LIST_HEAD(corten_mm_registry);

/* Reserve sentinel for claimed-but-unallocated magazine frames (see
 * include/linux/corten_arena.h).  Never published as an arena: it has no
 * start/end/refs and is filtered at lookup; every other xarray walker
 * treats it as an obstacle exactly like a live arena.
 */
struct corten_arena corten_va_reserve_sentinel;
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

/* Recycle-list / claimed-segment ledger teardown (M4.T1).  Called from
 * corten_arena_state_free() only: no reader can hold these -- both lists
 * are mmap_write-serialized and the mm is going away.
 */
static void corten_va_lists_free(struct corten_mm_state *state)
{
	struct corten_va_freeblk *blk, *blk_n;
	struct corten_va_segrec *seg, *seg_n;

	list_for_each_entry_safe(blk, blk_n, &state->va_free, list) {
		list_del(&blk->list);
		kfree(blk);
	}
	list_for_each_entry_safe(seg, seg_n, &state->seg_list, list) {
		list_del(&seg->list);
		kfree(seg);
	}
}

static void corten_arena_state_free(struct corten_mm_state *state)
{
	xa_destroy(&state->arenas);
	/* Pass-1 aging flags: xa_store(GFP_NOWAIT) by the shrinker
	 * leaves xarray nodes behind for every mm that ever ran a
	 * scan, so the exit path must release them too.
	 */
	xa_destroy(&state->shrink_aged);
	free_percpu(state->va_segs);
	corten_va_lists_free(state);
	kfree(state->implants);
	free_percpu(state->stats);
	mutex_destroy(&state->ctl_lock);
	kfree(state);
}

/* A5 (G5-fix) deferred shape of the state teardown: the callback body
 * is the same free (xa_destroy/free_percpu are atomic-safe), it just
 * runs after the grace period the registry unlink owes the shrinker
 * readers instead of making the dying process wait for it.
 */
static void corten_arena_state_free_rcu(struct rcu_head *rcu)
{
	struct corten_mm_state *state;

	state = container_of(rcu, struct corten_mm_state, rcu);
	corten_arena_state_free(state);
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
	/* The MODE global cursor starts at the window base regardless of
	 * which entry point created the registry (M4T0_SPEC.md 1.3); the
	 * T1 magazine layers its per-cpu segments and recycle list on top
	 * (both consume the same cursor under this mm's mmap_write).
	 */
	state->next_va = CORTEN_MODE_WINDOW_START;
	INIT_LIST_HEAD(&state->va_free);
	INIT_LIST_HEAD(&state->seg_list);
	INIT_LIST_HEAD(&state->arena_pool);
	/* M6.T3 shrinker registry node (see the registry lock comment). */
	state->owner_mm = mm;
	INIT_LIST_HEAD(&state->shrink_reg);
	spin_lock_init(&state->shrink_lock);
	state->shrink_cursor = 0;
	xa_init(&state->shrink_aged);
	state->va_segs = __alloc_percpu(sizeof(struct corten_va_seg),
					__alignof__(unsigned long));
	state->stats = __alloc_percpu(sizeof(unsigned long) *
				      CORTEN_ARENA_NR_STATS,
				      __alignof__(unsigned long));
	if (!state->stats || !state->va_segs) {
		corten_arena_state_free(state);
		return NULL;
	}

	mutex_lock(&corten_arena_alloc_lock);
	/* Pairs with the stores below and in corten_arena_mm_exit(). */
	raced = smp_load_acquire(&mm->corten_state);
	if (!raced) {
		/* Publish the registry; readers pair with this store.
		 * The shrinker node joins atomically with the publish
		 * (same alloc_lock): a state is either fully invisible
		 * or fully a shrinker victim.
		 */
		smp_store_release(&mm->corten_state, state);
		spin_lock(&corten_mm_registry_lock);
		list_add_tail_rcu(&state->shrink_reg, &corten_mm_registry);
		spin_unlock(&corten_mm_registry_lock);
	}
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
	struct vm_area_struct *carrier = READ_ONCE(arena->carrier);

	/* V-B.1: the FILE payload dies with the descriptor -- out of the
	 * mapping's per-inode registry (W1.b), the carrier back to the
	 * anonymous NULL shape, the region's file reference dropped
	 * (corten_region_file_teardown).  Before the carrier free below
	 * (the unlink needs the registry head); every caller runs under
	 * the owner mm's mmap_write (or at mm_users == 0), the teardown's
	 * lock contract.  RELEASE / mode_exit / mm_exit all converge
	 * here; the park path tears the payload down by itself (the
	 * descriptor survives it).
	 */
	corten_region_file_teardown(arena);

	/* V-A.2b: the carrier dies with the descriptor (its lifetime
	 * contract) -- the anon_vma chain reference and the avc node live
	 * in the vma, so the unlink precedes the free.  Every caller runs
	 * under the owner mm's mmap_write (or at mm_users == 0).
	 */
	if (carrier) {
		WRITE_ONCE(arena->carrier, NULL);
		corten_arena_carrier_free(carrier);
	}

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

/*
 * V-A.1: the same encoding for a *VMA-less* arena (a reactivated pool
 * window -- MV_VMA_FREE_SPEC.md sec 3.1.1).  vm_get_page_prot() masks the
 * flag word down to the R/W/X/SHARED combination, and an arena's
 * shadow-VMA is never VM_SHARED and never carries VM_PKEY_* (the whitelist
 * rejects both), so the private-mapping protection-map entry derived from
 * the perm bits alone is byte-identical to the shadow-VMA-derived one.
 * INV-MV3's KUnit case pins that equivalence.
 */
static pgprot_t corten_arena_perm_pgprot_pure(u8 perm)
{
	vm_flags_t flags = 0;

	if (perm & CORTEN_PERM_READ)
		flags |= VM_READ;
	if (perm & CORTEN_PERM_WRITE)
		flags |= VM_WRITE;
	if (perm & CORTEN_PERM_EXEC)
		flags |= VM_EXEC;

	return vm_get_page_prot(flags);
}

/* The perm -> PTE encoding helper every fault/protect arm uses: @vma is
 * the shadow-VMA, and may be NULL for a VMA-less arena.  pte_mkwrite()
 * dereferences @vma (shadow-stack/pkey shaping on x86); shapes an arena
 * whitelist admits have neither, so the novma spelling is the exact
 * VMA-less counterpart.
 */
static inline pte_t corten_pte_mkwrite(pte_t pte, struct vm_area_struct *vma)
{
	if (vma)
		return pte_mkwrite(pte, vma);
	return pte_mkwrite_novma(pte);
}

/* The VMA-less flush: x86/arm64 both expose flush_tlb_mm_range(), and
 * asm-generic backs it with flush_tlb_mm(); a full-range bounded flush is
 * the honest counterpart of ptep_clear_flush()/ptep_set_access_flags(),
 * which dereference @vma for their ranged form.
 */
static inline void corten_tlb_flush_page_novma(struct mm_struct *mm,
					       unsigned long addr)
{
	flush_tlb_mm_range(mm, addr, addr + PAGE_SIZE, PAGE_SHIFT, false);
}

static inline void corten_pte_clear_flush(struct vm_area_struct *vma,
					  struct mm_struct *mm,
					  unsigned long addr, pte_t *ptep)
{
	if (vma) {
		ptep_clear_flush(vma, addr, ptep);
		return;
	}
	ptep_get_and_clear(mm, addr, ptep);
	corten_tlb_flush_page_novma(mm, addr);
}

/* Returns true when the caller owes a flush (the !vma arm always sets and
 * always reports the flush, the conservative direction).
 */
static inline bool corten_pte_set_access_flags(struct vm_area_struct *vma,
					       struct mm_struct *mm,
					       unsigned long addr,
					       pte_t *ptep, pte_t entry,
					       bool write)
{
	if (vma)
		return ptep_set_access_flags(vma, addr, ptep, entry, write);
	set_pte_at(mm, addr, ptep, entry);
	return true;
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
 * the detached carrier (MV_VMA_FREE_SPEC.md sec 2.3, V-A.2b)
 * ------------------------------------------------------------------
 *
 * The VMA *layer* is what a MODE window must not touch: the maple tree,
 * the tree walkers, the vma_merge/split/munmap funnel and the per-VMA
 * lock audience.  The rmap/PTE-API consumers, however, are all
 * vma-keyed -- folio_add_new_anon_rmap()/folio_remove_rmap_pte(),
 * pte_mkwrite(), anon_vma_prepare(), copy_page_range() -- and A.1's
 * rmap-less world (no anchor: reclaim could not pick, fork needed a
 * hand-rolled copy arm) is exactly the cost of dropping the object
 * instead of the layer.  The carrier is the answer: a fully
 * initialized vm_area_struct that never enters mm_mt (never
 * vma_mark_attached(), refcount stays 0/detached -- upstream's own
 * detached shape from the munmap gather) and is reachable only through
 * the descriptor cache, under the same [FAIL-2] contract as the old
 * shadow-VMA pointer: created/destroyed under mmap_write, read only
 * while an active reference fences RELEASE.
 *
 * One anchor per live arena, never both: a targeted-DECLARE arena
 * keeps its tree shadow-VMA (and no carrier), an auto arena gets a
 * carrier (and no tree VMA).  corten_arena_anchor_vma() is what every
 * rmap/PTE-API consumer reads; the tree-only readers (park's do_munmap
 * arm, punch split surgery, fork's piece scan, mm_exit's skip test)
 * keep reading ar->vma directly.
 */

/*
 * Allocate and initialize a carrier for one arena spanning
 * [start, end).  The MAY bits are the private-anonymous full bound and
 * the access bits mirror @perm -- PTE encoding re-derives the hardware
 * protection from the metadata perm either way (the perm_pgprot()
 * contract); the flag word exists so that the rmap/ttu/PTE-API readers
 * see exactly the shape a legacy private-anonymous VMA would carry
 * (plus VM_CORTEN|VM_NOHUGEPAGE, which keep the reclaim guards and the
 * THP exclusion working verbatim).
 *
 * @prepare arms the anon_vma anchor (anon_vma_prepare): every owner of
 * the carrier prepares its own root.  The fork child passes false --
 * anon_vma_fork() must start from a NULL chain (the dup_mmap() shape),
 * so the child never owns a prepare'd root whose num_active_vmas
 * nothing will drop at unlink (the CONFIG_DEBUG_VM WARN shape in
 * unlink_anon_vmas(), review C1).
 *
 * V-B.1: @file non-NULL selects the FILE shape -- the carrier carries
 * the mapping's page offset (vma_set_range's @pgoff argument, the
 * INV-MV3(d) pgoff derivation) and vm_file = @file (a *borrowed*
 * pointer: the region record owns the reference, register_file() takes
 * it, teardown drops it -- carrier and rfile same source, same drop).
 * VM_SHARED stays clear (private mapping, write goes COW) and
 * vma_set_anonymous() (vm_ops = NULL) still applies: the carrier is
 * detached, no fault/method dispatch may ever reach it.  The page
 * offset of an anon carrier stays 0 as before.
 *
 * Called under the owner mm's mmap_write (vma_start_write() needs it).
 * Return: the carrier, or NULL on allocation failure.
 */
static struct vm_area_struct *corten_arena_carrier_alloc(struct mm_struct *mm,
							 unsigned long start,
							 unsigned long end,
							 u8 perm,
							 struct file *file,
							 unsigned long pgoff,
							 bool prepare)
{
	vm_flags_t flags = VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC |
			   VM_NORESERVE;
	struct vm_area_struct *vma;
	int ret = 0;

	vma = vm_area_alloc(mm);
	if (!vma)
		return NULL;

	if (file) {
		vma_set_range(vma, start, end, pgoff);
		vma->vm_file = file;
	} else {
		vma_set_range(vma, start, end, 0);
	}
	/* vm_ops = NULL: the detached contract (no .fault/.open dispatch
	 * can reach a carrier), the same anonymous spelling as before --
	 * for a FILE carrier the backing is expressed by vm_file/rpoff,
	 * never by vm_ops.
	 */
	vma_set_anonymous(vma);

	if (perm & CORTEN_PERM_READ)
		flags |= VM_READ;
	if (perm & CORTEN_PERM_WRITE)
		flags |= VM_WRITE;
	if (perm & CORTEN_PERM_EXEC)
		flags |= VM_EXEC;
	flags |= VM_CORTEN | VM_NOHUGEPAGE;

	/* "Use when VMA is not part of the VMA tree and needs no locking"
	 * (mm.h) -- this is the detached-carrier contract in one line.
	 */
	vm_flags_init(vma, flags);
	vma->vm_page_prot = vm_get_page_prot(flags);

	/* The write mark pairs the descriptor's lifecycle writers
	 * (create/destroy/fork-dup), matching what mmap_region does for
	 * a fresh tree VMA; copy_page_range()'s
	 * vma_assert_write_locked(src) leans on it during fork.
	 */
	vma_start_write(vma);

	if (prepare) {
		ret = anon_vma_prepare(vma);
		if (ret) {
			vm_area_free(vma);
			return NULL;
		}
	}

	atomic_long_inc(&corten_nr_carriers);
	return vma;
}

/*
 * Destroy a carrier: detach the anon_vma chain (the prepared anon_vma
 * reference and the avc node live in the vma) and free the object.
 * Callers hold the owner mm's mmap_write (or run at mm_users==0);
 * the never-attached refcount (0) is exactly what vm_area_free()'s
 * vma_assert_detached() demands.  vm_file needs no handling here: a
 * FILE carrier borrows the region record's reference and
 * corten_region_file_teardown() (always run before this) already
 * returned it to the anonymous NULL shape.
 */
static void corten_arena_carrier_free(struct vm_area_struct *vma)
{
	unlink_anon_vmas(vma);
	vm_area_free(vma);
}

/*
 * The rmap/PTE-API anchor of @ar: the detached carrier for an auto
 * arena, the tree shadow-VMA for a targeted-DECLARE arena, NULL only
 * for a parked descriptor (whose carrier persists but whose anchor
 * duties ended with the content zap -- the zap that parked it ran with
 * the anchor, so the rmap symmetry is already closed).
 *
 * Same stability contract as corten_arena_shadow_vma(): callers hold
 * an active reference or mmap_write, so the pointer cannot be freed
 * underneath ([FAIL-2], carrier edition).
 */
static inline struct vm_area_struct *corten_arena_anchor_vma(struct corten_arena *ar)
{
	struct vm_area_struct *carrier = READ_ONCE(ar->carrier);

	if (carrier)
		return carrier;
	return READ_ONCE(ar->vma);
}

/* ------------------------------------------------------------------ *
 * region record (MV_VMA_FREE_SPEC.md sec 2, V-A.0)
 * ------------------------------------------------------------------
 */

/*
 * The MAY upper bound of @vma's flag word, in CORTEN_PERM_* encoding:
 * the region record's mprotect-upgrade contract (sec 2.2).  VM_MAY* are
 * the flags the whitelist admits alongside the access bits, so every
 * attach shape can record a bound.
 */
static u8 corten_region_may_from_vma(struct vm_area_struct *vma)
{
	u8 may = CORTEN_PERM_USER;

	if (vma->vm_flags & VM_MAYREAD)
		may |= CORTEN_PERM_READ;
	if (vma->vm_flags & VM_MAYWRITE)
		may |= CORTEN_PERM_WRITE;
	if (vma->vm_flags & VM_MAYEXEC)
		may |= CORTEN_PERM_EXEC;

	return may;
}

/*
 * The CORTEN_RF_* reflection of @vma's surviving flag-word semantics
 * (sec 2.6 encoding table).  Only VM_SOFTDIRTY passes today's whitelist;
 * the other rows are the reserved encodings for a future whitelist
 * widening (the flags are reject-reasons at attach, never carried).
 */
static u32 corten_region_rflags_from_vma(struct vm_area_struct *vma)
{
	u32 rflags = 0;

	if (vma->vm_flags & VM_SOFTDIRTY)
		rflags |= CORTEN_RF_SOFTDIRTY;
	if (vma->vm_flags & VM_DONTCOPY)
		rflags |= CORTEN_RF_DONTCOPY;
	if (vma->vm_flags & VM_WIPEONFORK)
		rflags |= CORTEN_RF_WIPEONFORK;
	if (vma->vm_flags & VM_SEQ_READ)
		rflags |= CORTEN_RF_SEQ_READ;
	if (vma->vm_flags & VM_RAND_READ)
		rflags |= CORTEN_RF_RAND_READ;

	return rflags;
}

/*
 * The MAY upper bound of a private anonymous mapping: do_mmap() always
 * grants VM_MAYREAD|MAYWRITE|MAYEXEC for that shape regardless of the
 * requested prot, so this is the bound the VMA-less (re)activation paths
 * record (V-A.1: there is no VMA to read the MAY bits from any more).
 */
static u8 corten_region_may_full(void)
{
	return CORTEN_PERM_READ | CORTEN_PERM_WRITE | CORTEN_PERM_EXEC;
}

/*
 * INV-MV3 (MV_VMA_FREE_SPEC.md sec 2.6) -- the VMA-bit encoding
 * completeness invariant over one region record.  V-A.1 anchors the two
 * pairings the park surgery exercises:
 *   - may >= prot: the recorded MAY bound covers the recorded access
 *     contract (register() ORs prot into the bound; a violation is a
 *     torn record);
 *   - idle <=> RESERVED: the parked flag and the region class agree.
 * V-A.2b adds the anchor pairing: a live ANON region carries at most
 * one anchor -- the detached carrier (auto arenas) or the tree
 * shadow-VMA (targeted DECLARE); both at once is a torn record.  A
 * parked one has no tree shadow (its reservation VMA went at park;
 * the carrier persists, which is why the check is on @vma only).
 * "At most one" and not "exactly one": a punched targeted-DECLARE
 * arena degrades to a NULL piece cache (its surviving pieces still
 * hold the rmap anchors in the tree), and since F1 every pool
 * reactivation re-arms a carrier -- the A.1 anchor-less mixed state
 * is gone.
 * V-B.1 adds the FILE payload pairing (sec 2.5, rule (d)): a FILE
 * region carries a live file reference, its carrier is armed with the
 * same file, and the pgoff derivation agrees (carrier->vm_pgoff ==
 * rpoff) -- and no other class carries a stale rfile.  Checked by
 * corten_region_invariants_ok() -- the anchor publish order (carrier
 * before the registry stores, [FAIL-2]) means register() itself can
 * still observe the pre-publish state.
 */
static bool corten_region_record_ok(const struct corten_arena *ar)
{
	u8 may = READ_ONCE(ar->may_prot);
	u8 prot = READ_ONCE(ar->prot);

	if ((may & prot) != prot)
		return false;
	if (!!READ_ONCE(ar->idle) !=
	    (READ_ONCE(ar->rclass) == CORTEN_REGION_RESERVED))
		return false;
	if (!READ_ONCE(ar->idle) &&
	    READ_ONCE(ar->carrier) && READ_ONCE(ar->vma))
		return false;
	if (READ_ONCE(ar->idle) && READ_ONCE(ar->vma))
		return false;

	if (READ_ONCE(ar->rclass) == CORTEN_REGION_FILE) {
		const struct vm_area_struct *carrier =
			READ_ONCE(ar->carrier);

		if (!READ_ONCE(ar->rfile) || !carrier ||
		    carrier->vm_file != ar->rfile ||
		    carrier->vm_pgoff != READ_ONCE(ar->rpoff))
			return false;
	} else if (READ_ONCE(ar->rfile)) {
		return false;
	}

	return true;
}

void corten_region_register(struct corten_arena *ar,
			    enum corten_region_class rclass, u8 may_prot,
			    u32 rflags)
{
	/* may_prot >= ar->prot holds by construction: the bound absorbs
	 * the recorded access contract (sec 2.2's superset rule).
	 */
	WRITE_ONCE(ar->rclass, rclass);
	WRITE_ONCE(ar->may_prot, may_prot | ar->prot);
	WRITE_ONCE(ar->rflags, rflags);

	/* A FILE payload must already be torn down before any non-FILE
	 * stamp (the park runs corten_region_file_teardown() first);
	 * silently dropping a live reference here would leak it, so the
	 * stale payload is loud instead.  The pieces table is born
	 * single-piece.  The carrier is NOT touched: its lifetime is the
	 * descriptor's (created at DECLARE/fork-dup, freed at
	 * RELEASE/mm_exit by corten_arena_free()) and the park/reactivate
	 * flips must not churn it.
	 */
	if (ar->rfile) {
		WARN_ON_ONCE(1);
		ar->rfile = NULL;
	}
	ar->rpoff = 0;
	ar->npieces = 1;
	INIT_LIST_HEAD(&ar->rpieces);

	/* INV-MV3 (sec 2.6, V-A.1 anchors): the two record pairings the
	 * park surgery exercises -- may >= prot, and idle <=> RESERVED.
	 * register() is the write chokepoint of the record, so this is
	 * where the encoding-completeness tripwire lives.
	 */
	WARN_ON_ONCE(!corten_region_record_ok(ar));
}

/*
 * The FILE stamp of the region record (V-B.1, sec 2.2's FILE variant):
 * register(FILE) + the file payload, inlined because the payload must
 * land before the class flip -- register()'s own tripwire (and every
 * lockless reader) sees a fully-armed FILE record or none at all.  The
 * record takes the file's reference here (get_file cannot fail -- the
 * caller's own reference pins the object); the caller's carrier must
 * already carry the same file pointer and pgoff (the carrier borrows
 * this reference; INV-MV3 rule (d) checks the pairing).  The registry
 * membership is deliberately NOT taken here: it is the publication's
 * last step (after every failure path is behind), so a never-published
 * region never dangles in the mapping's registry (W1.b: the per-inode
 * corten region list the interval-tree membership became).
 *
 * Caller contract: the owner mm's mmap_lock for writing, the carrier
 * published into @ar (the tripwire reads it), and the registry head
 * pre-allocated into @ar->rinodes (W1.b: the tripwire asserts it -- the
 * link at the publication's last step allocates nothing and must not
 * fail).
 */
void corten_region_register_file(struct corten_arena *ar, u8 may_prot,
				 u32 rflags, struct file *file,
				 unsigned long pgoff)
{
	/* A stale payload would leak below -- only a fresh (or torn-down)
	 * record may take the FILE stamp.
	 */
	WARN_ON_ONCE(ar->rfile);

	WRITE_ONCE(ar->rfile, get_file(file));
	WRITE_ONCE(ar->rpoff, pgoff);
	WRITE_ONCE(ar->rclass, CORTEN_REGION_FILE);
	WRITE_ONCE(ar->may_prot, may_prot | ar->prot);
	WRITE_ONCE(ar->rflags, rflags);
	ar->npieces = 1;
	INIT_LIST_HEAD(&ar->rpieces);
	INIT_LIST_HEAD(&ar->rfile_node);
	WARN_ON_ONCE(!ar->rinodes);

	WARN_ON_ONCE(!corten_region_record_ok(ar));
}

/*
 * W1.b: allocate a registry head for a FILE attach -- the arm's first
 * allocation, taken where a failure still unwinds cheaply (before the
 * carrier exists).  The head is NOT stored in the global index here:
 * ownership rides the arena descriptor, and the link at the publication's
 * last step publishes it, so a failed attach never leaves an empty head
 * behind.
 */
static struct corten_inode_regions *corten_inode_regions_new(void)
{
	struct corten_inode_regions *reg;

	reg = kzalloc(sizeof(*reg), GFP_KERNEL_ACCOUNT);
	if (reg)
		INIT_LIST_HEAD(&reg->regions);

	return reg;
}

/*
 * W1.b: publish @ar's FILE region into the per-inode registry -- the
 * interval-tree membership (__vma_link_file verbatim, V-B.1) became
 * this list link.  This is what makes the region visible to the
 * file-side invalidation (truncate, unmap_mapping_folio): the W1.b
 * enumeration reads exactly this registry, and the region record
 * (rfile -> f_mapping, rpoff, arena bounds) carries everything it
 * needs to resolve a file event back to the window range.
 *
 * Lock order: the caller holds the owner mm's mmap_write, i_mmap_rwsem
 * nests inside it -- the kernel's existing vma_link() order, no new
 * edge (INV2).  The xa_store/cmpxchg of the first region's head runs
 * inside the i_mmap_write section (see the index's comment: one
 * directed edge, acyclic), after a kzalloc that already happened in
 * the armable window -- the link itself allocates nothing and cannot
 * fail, which the "past every failure path" placement of the call
 * relies on.  The carrier must be fully published (this is the last
 * step of the FILE attach/fork arms) and the region record armed.
 */
static void corten_file_registry_insert(struct corten_arena *ar)
{
	struct address_space *mapping = ar->rfile->f_mapping;
	struct corten_inode_regions *reg;

	i_mmap_lock_write(mapping);
	reg = xa_load(&corten_inode_regions, (unsigned long)mapping);
	if (!reg) {
		reg = ar->rinodes;
		/* No allocation: the cmpxchg either publishes our head or
		 * loses to a concurrent first insert of the same mapping.
		 * Every writer holds i_mmap_write, so losing is a bug --
		 * loud, and the winner's head is adopted instead.
		 */
		if (xa_cmpxchg(&corten_inode_regions, (unsigned long)mapping,
			       NULL, reg, GFP_NOWAIT) != NULL) {
			WARN_ONCE(1, "corten: registry head race on %p\n",
				  mapping);
			reg = xa_load(&corten_inode_regions,
				      (unsigned long)mapping);
		}
	}
	if (reg != ar->rinodes)
		kfree(ar->rinodes);
	ar->rinodes = reg;
	list_add_tail(&ar->rfile_node, &reg->regions);
	reg->nr++;
	i_mmap_unlock_write(mapping);
}

/*
 * The registry membership out: unlink the region under the same lock
 * pair that inserted it, and retire the head with its last region (the
 * kfree is safe outside the write section: the enumeration reads the
 * list under i_mmap_read, which the write hold excluded, and the head
 * is out of the global index by then).  Called only from the teardown
 * paths (the region is unreachable or quiesced; see
 * corten_region_file_teardown()).
 */
static void corten_file_registry_remove(struct corten_arena *ar)
{
	struct address_space *mapping = ar->rfile->f_mapping;
	struct corten_inode_regions *reg = ar->rinodes;
	bool dead = false;

	if (WARN_ON_ONCE(!reg))
		return;

	i_mmap_lock_write(mapping);
	list_del(&ar->rfile_node);
	if (--reg->nr == 0) {
		xa_erase(&corten_inode_regions, (unsigned long)mapping);
		dead = true;
	}
	i_mmap_unlock_write(mapping);
	ar->rinodes = NULL;
	if (dead)
		kfree(reg);
}

/*
 * Tear down a live FILE payload: leave the per-inode registry (W1.b),
 * return the carrier to the anonymous NULL shape (it survives a park to
 * serve the next ANON incarnation -- vm_file/vm_pgoff must not dangle),
 * drop the region's file reference.  No-op for every non-FILE record.
 *
 * Called at each region death point, all under the owner mm's
 * mmap_write (or at mm_users == 0, where no walker can race):
 * corten_arena_free() (RELEASE / mm_exit / mode_exit converge there)
 * and corten_arena_pool_park_locked() (reactivation is the ANON reuse
 * contract -- a parked window must not pin a file).  Missing one of
 * these calls leaks the file reference and dangles the registry node:
 * the KUnit lifecycle case reconciles file_count() across every state.
 */
static void corten_region_file_teardown(struct corten_arena *ar)
{
	struct vm_area_struct *carrier = READ_ONCE(ar->carrier);
	struct file *file = ar->rfile;

	if (!file)
		return;

	if (carrier && carrier->vm_file == file) {
		corten_file_registry_remove(ar);
		carrier->vm_file = NULL;
		carrier->vm_pgoff = 0;
	}
	ar->rfile = NULL;
	ar->rpoff = 0;
	fput(file);
}

/*
 * Disarm a FILE payload whose carrier never joined the per-inode
 * registry -- the publication error paths of the FILE attach and the
 * fork child registration (a mark/store failure unwinds between
 * register_file() and the registry link).  The same drops as teardown
 * minus the registry unlink: the node was never linked, so the
 * pre-allocated head is simply returned.
 */
static void corten_region_file_disarm(struct corten_arena *ar)
{
	struct vm_area_struct *carrier = READ_ONCE(ar->carrier);
	struct file *file = ar->rfile;

	if (!file)
		return;
	if (carrier && carrier->vm_file == file) {
		carrier->vm_file = NULL;
		carrier->vm_pgoff = 0;
	}
	ar->rfile = NULL;
	ar->rpoff = 0;
	kfree(ar->rinodes);
	ar->rinodes = NULL;
	fput(file);
}

/*
 * W1.b (W1_NATIVE_RMAP_SPEC.md sec 4.2): the file-side invalidation
 * enumeration.  unmap_mapping_pages()/unmap_mapping_folio() call this
 * inside their i_mmap_lock_read() section, before the interval-tree
 * walk; the regions of @mapping come from the per-inode registry (the
 * enumeration source flip -- a carrier is no longer an i_mmap member,
 * so the walk sees only legacy VMAs) and every region intersecting
 * [first_index, last_index] takes the B.2 chunk-zap transaction on the
 * derived VA range.
 *
 * The VA derivation is the walker's own arithmetic reversed: a region
 * covers file pages [rpoff, rpoff + npages), so the intersect is
 * zba/zea in pgoff and [ar->start + (zba - rpoff) * PAGE_SIZE,
 * ar->start + (zea - rpoff + 1) * PAGE_SIZE) in the window -- O(1) per
 * region, O(#regions/inode) overall (dlopen-shaped loads: single
 * digits; sec 2.1 case B's query).
 *
 * Transaction semantics (V-B.2 verbatim): KEEP_PERM keeps the VA and
 * the recorded permission so a re-fault re-reads the file's new
 * content; @even_cows selects the demotion shape -- truncate (true)
 * drops private COW copies too, invalidation (false) spares them
 * (CORTEN_UNMAP_FILE_EVENT).  unmap_mapping_folio() derives the
 * one-page range from folio->index; like the B.2 gate, it does not
 * re-check zap_details.single_folio: the derived slot is the folio's
 * offset by construction, and a slot the file event finds there is
 * window business regardless of which pagecache generation it maps.
 *
 * Chunk errors are counted away from truncate_routes and swallowed:
 * the walker has no error channel, the pagecache folios are already
 * gone from the truncate's side, and the demotion is retried by the
 * next file event or at RELEASE.
 *
 * Lock-order declaration (the B.2 review focus, INV2, unchanged):
 *
 *	this body:	i_mmap_rwsem(read) > desc->lock(W) > ptl
 *	the attach:	mmap_lock(W) > i_mmap_rwsem(W), and separately
 *			mmap_lock(W) > desc->lock(W)
 *
 * Nothing ever acquires i_mmap or mmap_lock while holding a descriptor
 * lock (the "desc locks never climb" discipline the shrinker path
 * already obeys), and the attach side's i_mmap critical sections
 * (registry link/unlink, the W1.b xa ops included) contain no
 * descriptor work at all -- so nesting the chunk driver under
 * i_mmap_read adds exactly one new edge, i_mmap_read > desc, with no
 * path back.  The transactions may sleep (tlb flushes) inside the read
 * section exactly as the B.2 gate's did.
 *
 * Lifetime (the old i_mmap-membership pin, transplanted): the caller's
 * i_mmap read hold pins every region's registry membership --
 * teardown/park must take i_mmap_lock_write to unlink -- so the walk
 * never touches a dead descriptor, and the active reference taken per
 * region extends the pin past the section by the established [FAIL-2]
 * discipline.  A dying (RELEASE in drain), fork-frozen or parked arena
 * answers "handled" without zapping: its own teardown path owns the
 * content drop, and the one thing this enumeration must never do is
 * fall back to the legacy zap -- that would bare-write window PTEs
 * (INV6, the reason the route exists).
 */
void corten_arena_unmap_file_range(struct address_space *mapping,
				   pgoff_t first_index, pgoff_t last_index,
				   bool even_cows)
{
	struct corten_inode_regions *reg;
	struct corten_arena *ar;
	u8 zflags = CORTEN_UNMAP_KEEP_PERM;

	lockdep_assert_held_read(&mapping->i_mmap_rwsem);

	if (!even_cows)
		zflags |= CORTEN_UNMAP_FILE_EVENT;

	/* The fast gate: a mapping without corten regions -- the whole
	 * legacy world -- costs one xarray probe and returns.
	 */
	reg = xa_load(&corten_inode_regions, (unsigned long)mapping);
	if (!reg)
		return;

	list_for_each_entry(ar, &reg->regions, rfile_node) {
		unsigned long npages = (ar->end - ar->start) >> PAGE_SHIFT;
		unsigned long start, end;
		pgoff_t zba, zea;

		zba = max_t(pgoff_t, first_index, ar->rpoff);
		zea = min_t(pgoff_t, last_index, ar->rpoff + npages - 1);
		if (zba > zea)
			continue;	/* the event misses this region */

		start = ar->start + ((zba - ar->rpoff) << PAGE_SHIFT);
		end = ar->start + ((zea - ar->rpoff + 1) << PAGE_SHIFT);

		/* A parked window is unmapped VA (lookup-invisible, its
		 * payload torn down at park time); a frozen one belongs to
		 * its own teardown.  Neither takes a transaction here.
		 */
		if (READ_ONCE(ar->idle) || READ_ONCE(ar->frozen))
			continue;
		if (!percpu_ref_tryget_live(&ar->active))
			continue;

		if (!corten_arena_unmap_chunk_flags(ar->mm, ar, start,
						    end - start, zflags,
						    NULL))
			atomic_long_inc(&corten_nr_truncate_routes);
		percpu_ref_put(&ar->active);
	}
}

/*
 * W1.b stale-node backstop (the demoted V-B.2 route gate): with the
 * enumeration above owning the invalidation source, a published FILE
 * region is never an i_mmap member -- targeted-DECLARE shadow-VMAs are
 * private-anonymous only (corten_arena_validate_vma()) and were never
 * members either.  A VM_CORTEN VMA the mapping's i_mmap walk produces
 * is therefore a stale interval-tree node that survived its teardown:
 * R-W1-2's exact failure shape, arrived by the one door the registry
 * flip closed.  Loud on the first hit, counted on every hit (must stay
 * 0), and the zap is refused rather than half-done -- keeping the INV6
 * line intact is worth more than pretending to unmap.
 *
 * Return: true = refuse the legacy zap (caller returns without
 * touching the range); false = not arena property, run the legacy body.
 */
bool corten_arena_imap_stale_guard(struct vm_area_struct *vma)
{
	if (!(vma->vm_flags & VM_CORTEN))
		return false;

	atomic_long_inc(&corten_nr_imap_stale_refuses);
	WARN_ONCE(1, "corten: VM_CORTEN vma in a mapping i_mmap walk (stale node)\n");

	return true;
}

/*
 * H7 defensive backstop (V-B.2): a VM_CORTEN VMA arriving at
 * zap_page_range_single() is a design error -- every legitimate unmap
 * of arena address space is routed (munmap/madvise routes, the file
 * event gate above) or owned by a teardown that clears VM_CORTEN
 * before its own legacy munmap leg, so the bare legacy writer must
 * never see one.  The zap_page_range_single_batched() variant needs no
 * twin: its only external caller (madvise_dontneed_single_vma()) walks
 * the maple tree, which cannot produce a detached carrier, and arena
 * ranges never reach it unrouted.  Loud on the first hit, counted on
 * every hit, and the zap is refused rather than half-done -- keeping
 * the INV6 line intact is worth more than pretending to unmap.
 * (Punch implants are plain legacy VMAs without VM_CORTEN, so they
 * stay on the legacy path.)
 *
 * Return: true = refuse the zap (caller returns without touching the
 * range); false = not arena property, run the legacy body.
 */
bool corten_zap_single_guard(struct vm_area_struct *vma)
{
	if (!(vma->vm_flags & VM_CORTEN))
		return false;

	atomic_long_inc(&corten_nr_zap_single_refuses);
	WARN_ONCE(1, "corten: VM_CORTEN vma reached zap_page_range_single()\n");

	return true;
}

/*
 * The INV-MV3 registry walk (test/audit entry): checks the record
 * invariant on every arena of @mm's registry.  The caller holds
 * mmap_lock for read (the sec 2.4 walk contract).
 */
bool corten_region_invariants_ok(struct mm_struct *mm)
{
	struct corten_mm_state *state;
	unsigned long frame = 0;
	struct corten_arena *ar;
	bool ok = true;

	/* Pairs with the store in corten_arena_state_create(). */
	state = smp_load_acquire(&mm->corten_state);
	if (!state)
		return true;

	xa_for_each(&state->arenas, frame, ar) {
		if (ar == &corten_va_reserve_sentinel)
			continue;
		/* Dedup across one region's frames: check at the first. */
		if (frame && xa_load(&state->arenas, frame - 1) == ar)
			continue;
		if (!corten_region_record_ok(ar))
			ok = false;
	}

	return ok;
}

/* ------------------------------------------------------------------ *
 * validation (DECLARE contract, sec 2.1)
 * ------------------------------------------------------------------
 */

/* Leaf PTE walk; defined in the S4 section below. */
static pmd_t *corten_arena_pmd(struct mm_struct *mm, unsigned long addr);
/* Upper-entry walk (V-D exit); defined next to corten_arena_pmd(). */
static pud_t *corten_arena_pud(struct mm_struct *mm, unsigned long addr);

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
		struct corten_arena *ar = xa_load(&state->arenas, frame);

		/* Magazine reserve markers are not arenas: the magazine's
		 * own frames must not overlap-reject its own allocations.
		 * A targeted DECLARE hitting a markered frame still fails
		 * -- validate_vma finds no VMA there.
		 */
		if (ar && ar != &corten_va_reserve_sentinel)
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
 * The perm counterpart of corten_arena_perm_to_prot(): PROT_* ->
 * CORTEN_PERM_* (the auto-route input shape -- V-A.2a reads the mmap
 * prot directly instead of a VMA's flag word).
 */
static u8 corten_arena_perm_from_prot(unsigned long prot)
{
	u8 perm = CORTEN_PERM_USER;

	if (prot & PROT_READ)
		perm |= CORTEN_PERM_READ;
	if (prot & PROT_WRITE)
		perm |= CORTEN_PERM_WRITE;
	if (prot & PROT_EXEC)
		perm |= CORTEN_PERM_EXEC;

	return perm;
}

/*
 * V-B.1: virtually allocate [start, start+len) of a fresh arena as
 * CORTEN_FILE_MAPPED(perm) -- the whole-region mark the FILE attach
 * does before publication (corten_mark()'s INVALID -> FILE_MAPPED is
 * the legal virtual-allocation edge; the transaction layer is
 * unchanged).  The fill+mark loop is the mmap_route MAP_FIXED shape
 * (mark records into the PT page's metadata array, so every window's
 * tracked PT page is ensured first), minus the zap rounds: the range
 * is fresh, no content can be present.
 *
 * Every PTE write on the FILE paths goes through transactions (INV6);
 * this one writes metadata only -- the region has no resident page in
 * B.1 (a fault dispatches STUB -> SEGV_MAPERR until B.3's read/COW
 * arms land).
 *
 * Error unwinding: mark is all-or-nothing per window (validate-then-
 * write, mm/corten.c), so on failure exactly the windows before the
 * failing one carry marks -- scrub that prefix back to INVALID so the
 * failed declare leaves no orphaned state behind for the next
 * declaration of the same range.
 *
 * Called under the owner mm's mmap_write (the declare body's context).
 * Return: 0, or -errno (fill/mark failure; the caller unwinds).
 */
static int corten_arena_file_mark(struct mm_struct *mm,
				  struct corten_arena *ar,
				  unsigned long start, unsigned long len,
				  u8 perm)
{
	struct corten_pte_meta meta = {
		.state = CORTEN_FILE_MAPPED,
		.perm = perm,
	};
	unsigned long a, end = start + len;
	int ret;

	for (a = start; a < end; a = min((a | (PMD_SIZE - 1)) + 1, end)) {
		ret = corten_arena_fill_upper(ar, a);
		if (ret)
			return ret;
	}

	for (a = start; a < end;) {
		unsigned long win_end = min((a | (PMD_SIZE - 1)) + 1, end);
		struct corten_txn txn;

		ret = corten_lock_range(mm, a, win_end - a, &txn);
		if (ret)
			goto scrub;
		ret = corten_mark(&txn, a, win_end - a, &meta);
		corten_unlock(&txn);
		if (ret)
			goto scrub;
		a = win_end;
	}

	return 0;

scrub:
	/* Windows [start, a) are marked (mark is per-window atomic); drop
	 * their slots back to the pristine INVALID the declare found.
	 */
	while (start < a) {
		unsigned long win_end = min((start | (PMD_SIZE - 1)) + 1, a);
		struct corten_txn txn;

		if (!corten_lock_range(mm, start, win_end - start, &txn)) {
			corten_unmap(&txn, start, win_end - start, 0);
			corten_unlock(&txn);
		}
		start = win_end;
	}
	return ret;
}

/*
 * The DECLARE body proper (P0, DEV-13).  The caller holds @mm's mmap_lock
 * for writing; this nests state->ctl_lock inside it -- the inversion of
 * the M3b order, so that the do_mmap auto-attach (which runs with the
 * write lock already held) can drive the very same body without forming
 * the reverse edge against a concurrent DECLARE.  Allocates the
 * descriptor; on error the caller's range is untouched.
 *
 * V-A.2a adds the @novma arm: the auto takeover declares without any
 * VMA in the range.  The permission contract comes in as @perm (from
 * the mmap prot; the VMA arm derives it from the declaring VMA), the
 * MAY bound is the private-anonymous full bound, and the anchor is a
 * detached carrier (V-A.2b) instead of a shadowized tree VMA.  The
 * accounting mmap_region() would have done rides here too: the gate
 * ran in corten_auto_validate() before placement, the charge follows
 * the successful stores.
 *
 * V-B.1 adds the FILE arm (@file non-NULL, implies @novma): the region
 * record takes the file reference (corten_region_register_file), the
 * carrier is born in FILE shape (vm_file/@pgoff), and the whole region
 * is virtually allocated CORTEN_FILE_MAPPED up front -- a fault into it
 * dispatches STUB -> SEGV_MAPERR (the honest B.1 "no semantics yet"
 * verdict) instead of the FRESH synthesizer's anonymous zero pages.
 * The pool reuse is skipped (reactivation is the ANON contract), the
 * parked-arena eject half still runs, and the region joins the
 * mapping's per-inode registry as the very last publishing step (W1.b).
 */
static int corten_arena_declare_locked(struct mm_struct *mm,
				       struct corten_mm_state *state,
				       unsigned long addr, unsigned long len,
				       u8 perm, struct file *file,
				       unsigned long pgoff, bool novma)
{
	unsigned long frame, first_frame, last_frame;
	struct corten_arena *arena;
	struct vm_area_struct *vma = NULL;
	u8 may_prot;
	u32 rflags = 0;
	int ret;

	mutex_lock(&state->ctl_lock);

	/* T1c arena pool: a parked arena of exactly this extent is
	 * reactivated in place -- no descriptor allocation, no percpu_ref
	 * cycle, warm page tables.  Any other parked arena inside the
	 * range is deregistered first so the fresh path below sees a
	 * clean range (its registry slots would otherwise overlap-reject
	 * or collide with the stores below).
	 *
	 * V-B.1: the FILE arm passes no_reuse -- a parked window must not
	 * be reactivated for a file mapping (the reuse contract is the
	 * ANON metadata flip), but the eject half still applies: the
	 * global placer (oversized spans) can land a fresh range over a
	 * parked arena's frames.
	 */
	ret = corten_arena_pool_prepare_locked(mm, state, addr, len, perm,
					       novma, !!file);
	if (ret <= 0) {
		mutex_unlock(&state->ctl_lock);
		return ret;		/* 0 = reactivated, else errno */
	}

	arena = kzalloc(sizeof(*arena), GFP_KERNEL_ACCOUNT);
	if (!arena) {
		mutex_unlock(&state->ctl_lock);
		return -ENOMEM;
	}

	arena->start = addr;
	arena->end = addr + len;
	arena->mm = mm;
	mutex_init(&arena->fill_lock);
	init_completion(&arena->drained);
	INIT_LIST_HEAD(&arena->pool);
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

	/* Arena ranges must not overlap; also rejects re-declaring the
	 * same range.  Exclusive under ctl_lock.
	 */
	if (corten_arena_overlaps(state, addr, len)) {
		ret = -EEXIST;
		goto out_free_arena;
	}

	/* [C1] the range must be empty (see the checker's comment) -- the
	 * cheap safety net both arms share: a slot that carries content
	 * must never be declared over.
	 */
	ret = corten_arena_check_empty_locked(mm, addr, addr + len);
	if (ret)
		goto out_free_arena;

	if (novma) {
		/* V-A.2a: the auto takeover needs no VMA.  The validation
		 * checklist ran in corten_auto_validate() before placement
		 * (the may_expand_vm gate), [C1] ran above, and the anchor
		 * is the V-A.2b carrier -- anon_vma_prepare() can fail
		 * (-ENOMEM), so it happens before any publication.  V-B.1:
		 * a FILE carrier also carries vm_file/@pgoff (the region's
		 * borrowed pointer -- register_file takes the reference
		 * after this succeeds).  W1.b: a FILE attach also
		 * pre-allocates its registry head here, the arm's first
		 * allocation, so the link at the publication's last step
		 * (past every failure path) allocates nothing.
		 */
		if (file) {
			arena->rinodes = corten_inode_regions_new();
			if (!arena->rinodes) {
				ret = -ENOMEM;
				goto out_free_arena;
			}
		}
		vma = corten_arena_carrier_alloc(mm, addr, addr + len, perm,
						 file, pgoff, true);
		if (!vma) {
			ret = -ENOMEM;
			goto out_free_arena;
		}
		arena->prot = perm;
		may_prot = file ? corten_file_may_bound(file)
				: corten_region_may_full();
	} else {
		/* Validation and conversion happen together under the
		 * write lock (held by the caller): the VMA must survive as
		 * one piece up to the flag flip anyway, so a separate
		 * read-lock pre-pass would only duplicate the check.
		 * Publish with the shadow-VMA in place before the xarray
		 * entries, so a concurrent S4 lookup never resolves an
		 * arena whose VMA is not yet converted ("arena addresses
		 * always have their shadow-VMA", sec 2.3/4.3).  Nothing
		 * can fault through the arena path yet: the frames only
		 * become visible to lookup after the stores below.
		 */
		vma = vma_lookup(mm, addr);
		ret = corten_arena_validate_vma(vma, addr, len);
		if (ret)
			goto out_free_arena;

		arena->prot = corten_arena_prot_from_vma(vma);
		may_prot = corten_region_may_from_vma(vma);
		rflags = corten_region_rflags_from_vma(vma);

		ret = corten_arena_shadowize(vma);
		if (ret)
			goto out_free_arena;
	}

	/* Publish the anchor (cached shadow-VMA, or the detached carrier)
	 * before the arena becomes visible in the xarray below ([FAIL-2]):
	 * every reader that resolves the arena also resolves the anchor.
	 */
	if (novma)
		WRITE_ONCE(arena->carrier, vma);
	else
		WRITE_ONCE(arena->vma, vma);

	/* The region record is born with the arena (V-A.0 sec 3.1.0):
	 * live == CORTEN_REGION_ANON.  register()'s INV-MV3 tripwire
	 * reads the anchor pairings, so the publish above must precede
	 * it.  V-B.1: the FILE arm takes the file payload instead (the
	 * carrier above already carries the same pointer and pgoff).
	 */
	if (file)
		corten_region_register_file(arena, may_prot, rflags, file,
					    pgoff);
	else
		corten_region_register(arena, CORTEN_REGION_ANON, may_prot,
				       rflags);

	/* V-B.1: virtually allocate the whole region as FILE_MAPPED before
	 * publication (INVALID -> FILE_MAPPED is the transaction layer's
	 * legal virtual-allocation edge, mm/corten.c's mark state machine
	 * -- zero changes there).  Every window's PT page is filled up
	 * front (the mmap_route mark-loop shape): mark() records into the
	 * PT page's metadata array, a hole would fail the transaction.
	 * A failure unwinds with nothing published -- the marks of already
	 * finished windows are scrubbed inside.
	 */
	if (file) {
		ret = corten_arena_file_mark(mm, arena, addr, len, perm);
		if (ret)
			goto out_unwind;
	}

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

	/* V-A.2a: the total_vm charge mmap_region() used to make for the
	 * takeover mapping.  Behind every failure path; the may_expand_vm
	 * gate already ran in corten_auto_validate() under the same lock
	 * (nothing else charges between placement and here).
	 */
	if (novma)
		vm_stat_account(mm, corten_take_vm_flags(perm),
				(long)(len >> PAGE_SHIFT));

	/* Last publishing step: the observability ledger (S8).  All
	 * failure paths are behind us, so the ledger only ever contains
	 * fully registered arenas.  V-B.1's own last step follows -- W1.b
	 * reshaped it: the FILE region joins the mapping's per-inode
	 * registry (the interval-tree membership it replaced), past
	 * every failure path, so a registry only ever carries published
	 * regions.
	 */
	corten_arena_obs_add(arena);
	if (file)
		corten_file_registry_insert(arena);

	mutex_unlock(&state->ctl_lock);

	/* V-A.2a fidelity: mmap_region() emitted PERF_RECORD_MMAP for the
	 * takeover mapping; the carrier carries the same fields (bounds,
	 * pgoff, private-anon flags), so the observer contract (matrix
	 * C25) is preserved verbatim.  mmap_write is held, exactly the
	 * mmap_region() context.  The targeted-DECLARE arm skips this:
	 * the declaring VMA's own mmap() already reported it.
	 */
	if (novma)
		perf_event_mmap(arena->carrier);

	return 0;

out_unwind:
	/* The arena is dead here (nothing published but the anchor and
	 * some frame slots) -- undo the stores and the anchor; the record
	 * is never re-read after this (the descriptor is freed below),
	 * so no register() stamp is owed.  V-B.1: the file payload's
	 * reference (register_file's get_file) is disarmed before the
	 * carrier free; the region was never linked into the registry
	 * (the link is past every failure path -- W1.b).
	 */
	while (frame > first_frame)
		xa_erase(&state->arenas, --frame);
	if (file)
		corten_region_file_disarm(arena);
	if (novma) {
		WRITE_ONCE(arena->carrier, NULL);
		corten_arena_carrier_free(vma);
	} else {
		corten_arena_unshadow(arena, vma);
	}
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
	 * inside it.  The targeted-DECLARE shape: perm/file are unused
	 * (the declaring VMA carries the contract).
	 */
	mmap_write_lock(mm);
	ret = corten_arena_declare_locked(mm, state, addr, len, 0, NULL, 0,
					  false);
	mmap_write_unlock(mm);

	return ret;
}

/*
 * The PTE-page retirement core: drop every tracked-or-drift PTE page in
 * [start,end) through the pte_free_tlb() funnel (INV6: the funnel owns
 * the M2a descriptor uninstall and the TLB batching) against a
 * caller-owned gather.  V-D: mm_dec_nr_ptes() pairs the mm_inc_nr_ptes()
 * __pte_alloc() did for every install path (fill_upper() and the fault
 * fallbacks all go through pte_alloc()), so pgtables_bytes now returns
 * to zero with the pages -- the accounting half of the B-2 closure (the
 * upper-table half is the exit walk's self-teardown below).
 *
 * Upper-level note: only pmd entries the arena owned are cleared; a pud
 * shared with other mappings is untouched (the exit walk decides when a
 * whole upper page is window-exclusive).
 *
 * Callers hold mmap_write (release path) or the dying mm's read lock
 * (exit path), with the arena's content already zapped.
 */
static void corten_arena_free_ptes_span(struct mm_struct *mm,
					struct mmu_gather *tlb,
					unsigned long start,
					unsigned long end)
{
	unsigned long addr;

	for (addr = start; addr < end;
	     addr = min((addr | (PMD_SIZE - 1)) + 1, end)) {
		pmd_t *pmdp = corten_arena_pmd(mm, addr);

		if (!pmdp || !pmd_present(READ_ONCE(*pmdp)) ||
		    pmd_leaf(READ_ONCE(*pmdp)))
			continue;

		pte_free_tlb(tlb, pmd_pgtable(READ_ONCE(*pmdp)), addr);
		pmd_clear(pmdp);
		mm_dec_nr_ptes(mm);
	}
}

/*
 * V-A.1: retire the PT pages a VMA-less arena leaves behind.  The zap
 * cleared every PTE; free_pgtables() only walks tree VMAs, so a no-VMA
 * window's page tables would otherwise outlive the arena (leaked at mm
 * death).  The upper pages are left in place on the live paths -- warm
 * for the next arena in their span; the V-D exit walk retires them
 * wholesale at mm death.
 */
static void corten_arena_free_ptes_novma(struct mm_struct *mm,
					 unsigned long start,
					 unsigned long end)
{
	/* Pointer alias: the pte_free_tlb() macro substitutes its first
	 * argument unparenthesized, so a caller-side "&tlb" would expand
	 * to "&tlb->freed_tables" (the free_pte_range() shape passes a
	 * pointer variable).
	 */
	struct mmu_gather _tlb;
	struct mmu_gather *tlb = &_tlb;

	tlb_gather_mmu(tlb, mm);
	corten_arena_free_ptes_span(mm, tlb, start, end);
	tlb_finish_mmu(tlb);
}

/*
 * The teardown tail shared by RELEASE, the pool eviction/flush and the
 * parked-arena ejection (T1c): the arena is located, its extent is
 * verified and @mm's mmap_write + state->ctl_lock are held.  Deregisters
 * the arena, drains in-flight transactions, strips the shadow decoration
 * and removes the range with the plain legacy munmap, then frees the
 * descriptor.
 *
 * T1c: a *parked* arena tears down through the same body with two
 * differences -- the pool node goes, and the live-set accounting was
 * already done by its park (no @nr decrement, no second RELEASE stat;
 * the DECLARES/RELEASES pairing stays D-R == live arenas).  V-A.1: a
 * parked arena has no VMA at all, and neither does a reactivated one --
 * a range with zero tree VMAs takes the pure-metadata teardown (content
 * zap + PT-page retirement) instead of the do_munmap().
 *
 * The drain waits under BOTH locks and is still deadlock-free
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
 *
 * Return: the do_munmap() status (0, or -errno on memory pressure -- the
 * range survives as plain anonymous memory; see the error path below).
 */
static int corten_arena_release_arena_locked(struct mm_struct *mm,
					     struct corten_mm_state *state,
					     struct corten_arena *arena)
{
	unsigned long frame, first_frame, last_frame;
	bool drained, was_idle = READ_ONCE(arena->idle);
	bool any_vma = false;
	int ret = 0;

	if (was_idle) {
		WRITE_ONCE(arena->idle, false);
		list_del(&arena->pool);
		state->nr_pool--;
	}

	first_frame = arena->start >> PMD_SHIFT;
	last_frame = (arena->end - 1) >> PMD_SHIFT;
	for (frame = first_frame; frame <= last_frame; frame++) {
		struct corten_arena *stale = xa_load(&state->arenas, frame);

		/* [F-B] An already-NULL frame is legal since the D-G''
		 * punch route: a file MAP_FIXED may have erased the hole's
		 * frames earlier.  Only a foreign entry is a kernel bug.
		 */
		if (WARN_ON_ONCE(stale && stale != arena))
			break;
		/* M4.T1: magazine frames get their reserve marker back
		 * (and the range joins the recycle list) instead of a
		 * bare erase -- the warm xarray subtree and the recycled
		 * VA are the point of the magazine.  Hole frames stay
		 * erased: a punch carved them out precisely because a
		 * legacy VMA lives there, and restoring a marker would
		 * make them read as "claimed and free" to the magazine.
		 */
		if (stale == arena)
			corten_va_release_frame(state, frame << PMD_SHIFT);
	}
	if (!was_idle) {
		refcount_set(&state->nr, refcount_read(&state->nr) - 1);
		corten_arena_stat_add(state, CORTEN_ARENA_STAT_RELEASES, 1);
	}

	/* Deregister from the observability ledger before the drain: a
	 * RELEASE that has made the arena unreachable in its own registry
	 * must not stay visible to debugfs either (the drain can wait up
	 * to CORTEN_ARENA_DRAIN_TIMEOUT on a broken kernel).
	 */
	corten_arena_obs_remove(arena);

	/* Drain in-flight transactions, then tear the shadow-VMA down
	 * with the plain legacy munmap: pages are zapped and the PT pages
	 * travel through the regular free funnels (M2a uninstall).  On a
	 * drain timeout (leaked reference, a kernel bug) the teardown
	 * still runs: a runnable process with a counted leak beats an
	 * unkillable D-state zombie; the in-flight-transaction safety
	 * argument above is void in that case, which is why the leak is
	 * counted loudly (CORTEN_ARENA_STAT_DRAIN_TIMEOUTS + the global
	 * arena_stats aggregate).
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

		for_each_vma_range(vmi, vma, arena->end) {
			any_vma = true;
			corten_arena_unshadow(arena, vma);
		}
	}

	if (any_vma) {
		/* The tree VMAs in range -- surviving shadow pieces and
		 * punch implants alike -- leave through the regular
		 * funnel, which zaps their PTEs and refunds their own
		 * total_vm.
		 */
		struct mmu_gather tlb;

		ret = do_munmap(mm, arena->start, arena->end - arena->start,
				NULL);
		if (!ret) {
			/* V-A.2a (the mixed-shape fix): the arena's OWN
			 * windows carry no VMA, so the funnel above could
			 * not have dropped their content -- the
			 * transactional zap + PT retirement runs here for
			 * the remainder (over the emptied implant windows
			 * it no-ops).  The hand-refund is keyed on the
			 * carrier: a carrier arena's live charge was
			 * taken by reactivate/declare (no tree VMA ever
			 * carried it), while a shadow piece's charge rode
			 * the VMA the funnel just refunded.
			 */
			tlb_gather_mmu(&tlb, mm);
			corten_arena_unmap_chunk_flags(mm, arena,
						       arena->start,
						       arena->end -
						       arena->start, 0,
						       &tlb);
			tlb_finish_mmu(&tlb);
			corten_arena_free_ptes_novma(mm, arena->start,
						     arena->end);
			if (!was_idle && READ_ONCE(arena->carrier))
				vm_stat_account(mm,
						corten_take_vm_flags(READ_ONCE(arena->prot)),
						-(long)((arena->end -
							 arena->start) >>
							PAGE_SHIFT));
		} else {
			/* Memory pressure: the range survived (whole or in
			 * pieces).  Strip the shadow decoration so no
			 * VM_CORTEN VMA outlives its arena descriptor; the
			 * pieces keep working as plain anonymous memory.
			 * The carrier windows' pages stay until the mm's
			 * exit walk drops them.
			 */
			VMA_ITERATOR(vmi, mm, arena->start);
			struct vm_area_struct *vma;

			for_each_vma_range(vmi, vma, arena->end)
				corten_arena_unshadow(arena, vma);
		}
	}

	/* The pure-metadata remainder (no tree VMA anywhere in the
	 * range): the V-A.1 teardown -- the content zap drops whatever
	 * the window still holds and the PT-page retirement follows; the
	 * ranges' upper tables stay (warm, shared, V-D's exit walk owns
	 * them).  The hand-refund mirrors the take charge reactivate/
	 * declare made (no tree VMA carries it).
	 */
	if (!any_vma) {
		struct mmu_gather tlb;

		tlb_gather_mmu(&tlb, mm);
		corten_arena_unmap_chunk_flags(mm, arena, arena->start,
					       arena->end - arena->start, 0,
					       &tlb);
		tlb_finish_mmu(&tlb);
		corten_arena_free_ptes_novma(mm, arena->start, arena->end);
		if (!was_idle)
			vm_stat_account(mm,
					corten_take_vm_flags(READ_ONCE(arena->prot)),
					-(long)((arena->end - arena->start) >>
						PAGE_SHIFT));
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

	return ret;
}

/*
 * The RELEASE body proper (P0, DEV-13).  The caller holds @mm's mmap_lock
 * for writing; this nests state->ctl_lock inside it.
 */
static int corten_arena_release_locked(struct mm_struct *mm,
				       struct corten_mm_state *state,
				       unsigned long addr, unsigned long len)
{
	struct corten_arena *arena;
	int ret;

	mutex_lock(&state->ctl_lock);

	/* RELEASE must hit one declared arena exactly (by start). */
	arena = xa_load(&state->arenas, addr >> PMD_SHIFT);
	if (!arena || arena->start != addr || arena->end != addr + len) {
		mutex_unlock(&state->ctl_lock);
		return -ENOENT;
	}

	ret = corten_arena_release_arena_locked(mm, state, arena);

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
 * Region class label for the observability renderers (a parked arena
 * shows "rsvd", the V-B class has no producer yet).
 */
static const char *corten_region_class_name(enum corten_region_class rclass)
{
	switch (rclass) {
	case CORTEN_REGION_ANON:
		return "anon";
	case CORTEN_REGION_FILE:
		return "file";
	case CORTEN_REGION_RESERVED:
		return "rsvd";
	}
	return "????";
}

/*
 * One line per live arena across every mm: owning mm, cached shadow-VMA,
 * range, recorded prot and the liveness of the transaction refcount
 * (active: transactions may enter; dying: kill issued, draining).  In
 * practice RELEASE deregisters before the drain's kill, so entries are
 * normally observed active -- the dying branch is defensive.  The
 * percpu transaction count itself has no race-free reader by design, so
 * the observable liveness states are what the file reports.  RCU walk:
 * arenas unlinked concurrently simply do not show up.
 *
 * V-A.0 appends the embedded region record's summary (sec 3.1.0): class,
 * CORTEN_RF_* reflection and the piece count.  V-B.4 (H10) appends the
 * FILE payload (the region's file and its start pgoff, both zero for a
 * non-FILE region).  The region fields are
 * written under the owner mm's mmap_write next to prot/idle; this walker
 * reads them locklessly, so each column is a racing snapshot by design.
 */
/*
 * The out-of-line J1 prelude counter (see mm/corten_arena.h): @vma is
 * the lookup result.  probes counts every external window query on a
 * MODE mm; hits counts the queries that found a tree VMA overlapping
 * the window domain -- post-A.2a/A.2b those must be punch implants (a
 * non-corten file/anon VMA a MAP_FIXED punch installed) and nothing
 * else; the guest gate asserts the split.
 */
void corten_j1_slow(struct mm_struct *mm, unsigned long start,
		    unsigned long end, struct vm_area_struct *vma)
{
	unsigned long clip;

	/* V-A.3d J1 exemption (D24: "implant accesses are the legal tree
	 * lookups inside the contract").  The A.3c audit-gate first run
	 * showed the guest smoke's punch/implant contract shape landing
	 * here as exactly one hit -- a legal find that must not pollute
	 * the J1 ledger or the gate would stay red on a compliant
	 * workload.  Two shapes are exempt: the query itself lands in
	 * registered implant VA, or the lookup found a VMA whose window
	 * intersection is registered (find_vma() on a neighbouring
	 * window address returning the next VMA, an implant, is the same
	 * legal access).  The lockless registry query is safe in every
	 * probe context: callers hold this mm's mmap lock (excluded from
	 * every mark writer) or an RCU read-side section (the array is
	 * retired via kfree_rcu(), see corten_implant_mark()); torn
	 * entry reads can only mis-sort a counter, never leave bounds.
	 */
	clip = max(start, CORTEN_MODE_WINDOW_START);
	if (corten_implant_covers_lockless(mm, clip,
					   min(end, CORTEN_MODE_WINDOW_END) -
					   clip))
		return;
	if (vma && vma->vm_end > CORTEN_MODE_WINDOW_START &&
	    vma->vm_start < CORTEN_MODE_WINDOW_END) {
		clip = max(vma->vm_start, CORTEN_MODE_WINDOW_START);
		if (corten_implant_covers_lockless(mm, clip,
						   min(vma->vm_end,
						       CORTEN_MODE_WINDOW_END) -
						   clip))
			return;
	}

	atomic_long_inc(&corten_nr_j1_probes);

	if (vma && vma->vm_end > CORTEN_MODE_WINDOW_START &&
	    vma->vm_start < CORTEN_MODE_WINDOW_END)
		atomic_long_inc(&corten_nr_j1_hits);
}

/*
 * V-E (OQ-MV-7): the heap arm of the probe above -- a MODE-mm
 * find_vma-family call whose query address landed in [start_brk, brk).
 * The delegated-domain answer the legacy funnel gives is exactly what
 * the V-E.1 verdict keeps (spec sec 3.5): the count exists so the
 * heap-fault share of find_vma traffic is measurable, not optimized.
 */
void corten_j1_heap_note(struct mm_struct *mm)
{
	atomic_long_inc(&corten_nr_heap_lookups);
}

/* V-E (spec sec 3.5): the sys_brk arm ledger (see the enum in
 * include/linux/corten_arena.h; the inline gate has already answered
 * the double-gate question).
 */
void corten_brk_note_slow(struct mm_struct *mm, enum corten_brk_arm arm)
{
	switch (arm) {
	case CORTEN_BRK_GROW:
		atomic_long_inc(&corten_nr_brk_grow);
		break;
	case CORTEN_BRK_SHRINK:
		atomic_long_inc(&corten_nr_brk_shrink);
		break;
	case CORTEN_BRK_NOOP:
		atomic_long_inc(&corten_nr_brk_noop);
		break;
	case CORTEN_BRK_REJECT:
		atomic_long_inc(&corten_nr_brk_reject);
		break;
	default:
		break;
	}
}

/*
 * V-A.3b audit #1: the fault fast hook's window arm.  A MODE mm's
 * window-domain address that reached the FALLBACK verdict has no VMA to
 * find -- post-A.1/A.2 both parked windows and never-declared holes are
 * tree-free (S-1) -- so lock_vma_under_rcu()'s mas_walk there is a
 * guaranteed miss whose only effect is polluting the probe above.  The
 * caller diverts such faults to the slow path, whose mmap_read keeps the
 * legacy race serialization against a concurrent DECLARE/park; this
 * helper never terminates a fault by itself (a lockless MAPERR would
 * SIGSEGV a fault that races a DECLARE which has not published yet --
 * the legacy ordering point is exactly that lock).  Called from the RCU
 * exit of the arch hook, no lock held: the counter is atomic_long like
 * the probe pair.
 */
bool corten_fault_window_fallback(struct mm_struct *mm, unsigned long addr)
{
	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode) ||
	    addr < CORTEN_MODE_WINDOW_START || addr >= CORTEN_MODE_WINDOW_END)
		return false;

	atomic_long_inc(&corten_nr_fault_fallback_window);
	return true;
}

/*
 * V-A.3b audit #2: the fault slow path's window terminus.  Called under
 * the mmap_read just taken by lock_mm_and_find_vma(), before its
 * find_vma(): a window address with no live (non-idle) arena at it is
 * the parked/hole shape whose legacy verdict is SIGSEGV MAPERR (S-1) --
 * answer it without the tree walk.  The registry lookup is the
 * ownership-fallback carve-out: an active arena whose VA a punch
 * implant owns must still run find_vma() so the legacy funnel serves
 * the real VMA (the probe then legally counts the implant hit).  The
 * mmap_read serializes against DECLARE/park's mmap_write, so the
 * idle/live answer cannot change under the lookup (the RCU section is
 * for the xarray walk itself).
 */
bool corten_fault_window_maperr(struct mm_struct *mm, unsigned long addr)
{
	struct corten_arena *ar;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode) ||
	    addr < CORTEN_MODE_WINDOW_START || addr >= CORTEN_MODE_WINDOW_END)
		return false;

	rcu_read_lock();
	ar = corten_arena_lookup(mm, addr);
	rcu_read_unlock();
	if (ar)
		return false;

	/* A hole-window implant (the A.3a P1b idle-eject product, or a
	 * punch into a tree-free window segment) is a registered legacy
	 * VMA the tree must still serve -- the implant registry is the
	 * window domain's occupancy truth for exactly these pieces.
	 * Guest smoke contract caught the missing arm (run_mode_smoke's
	 * punch/implant cases went MAPERR); reads are safe, every
	 * registry writer holds mmap_write and the caller holds
	 * mmap_read.
	 */
	if (corten_implant_covers(mm, addr, 1))
		return false;

	atomic_long_inc(&corten_nr_fault_fallback_window);
	return true;
}

/*
 * V-A.3b audit #29: the uffd mfill/move funnel's window short-circuit.
 * A MODE mm's window domain cannot host an uffd-registered VMA (only a
 * MAP_FIXED punch implant can live there), so uffd_lock_vma()'s
 * lock_vma_under_rcu() and find_vma_and_prepare_anon() would be
 * guaranteed misses -- user-triggerable ones (UFFDIO_COPY with a window
 * destination), which would keep the J1 probe permanently non-zero with
 * a hostile user in the loop.  Answer -ENOENT here, the errno the
 * legacy lookup misses would return (the C12 terminal verdict).  @src_*
 * covers move_pages()'s second lookup leg; mfill passes 0/0 (its source
 * is a userspace buffer read with copy_from_user(), never a tree
 * lookup).  Ranges are pre-sanitized by the callers (page-aligned, no
 * wrap).  Disclosure corner: an implant that somehow got
 * uffd-registered now answers -ENOENT too -- registering a punch
 * implant for uffd is outside every registered shape.
 */
bool corten_uffd_window_reject(struct mm_struct *mm,
			       unsigned long dst_start, unsigned long dst_len,
			       unsigned long src_start, unsigned long src_len)
{
	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode))
		return false;

	if ((dst_start < CORTEN_MODE_WINDOW_END &&
	     dst_start + dst_len > CORTEN_MODE_WINDOW_START) ||
	    (src_start < CORTEN_MODE_WINDOW_END &&
	     src_start + src_len > CORTEN_MODE_WINDOW_START)) {
		atomic_long_inc(&corten_nr_uffd_window_reject);
		return true;
	}

	return false;
}

/*
 * V-A.3b audit #3, observation only (the fix is V-C's corten_gup_probe):
 * gup_vma_lookup()'s miss on a MODE mm's window domain.  GUP-slow
 * (io_uring, 9p, vhost, pin) is structurally blind to arenas; this
 * counts the blind spot until the region-aware branch lands.  The J1
 * probe inside find_vma() has already counted the walk itself.
 */
void corten_gup_note_window_miss(struct mm_struct *mm, unsigned long addr)
{
	if (corten_enabled_static() && READ_ONCE(mm->corten_mode) &&
	    addr >= CORTEN_MODE_WINDOW_START && addr < CORTEN_MODE_WINDOW_END)
		atomic_long_inc(&corten_nr_gup_window_miss);
}

/*
 * V-A.3b audit #7, observation only: __access_remote_vm()'s short
 * answer (zero bytes) on a MODE mm's window domain.  ptrace PEEK/POKE,
 * /proc/pid/mem and process_vm_readv/writev read nothing there until
 * V-C routes remote access through regions.
 */
void corten_remote_note_window_short(struct mm_struct *mm, unsigned long addr)
{
	if (corten_enabled_static() && READ_ONCE(mm->corten_mode) &&
	    addr >= CORTEN_MODE_WINDOW_START && addr < CORTEN_MODE_WINDOW_END)
		atomic_long_inc(&corten_nr_remote_access_window_short);
}

void corten_arena_arenas_report(struct seq_file *m)
{
	struct corten_arena *ar;

	seq_puts(m, "            mm               anchor [start,end)                prot cls  rflg pcs status               rfile     poff\n");

	rcu_read_lock();
	list_for_each_entry_rcu(ar, &corten_arena_list, obs) {
		struct vm_area_struct *carrier = READ_ONCE(ar->carrier);
		struct file *rfile = READ_ONCE(ar->rfile);

		/* V-B.4 (H10): the FILE payload columns -- the region's file
		 * (NULL for anon/reserved, the carrier's own vm_file for a
		 * live FILE region) and the mapping's start pgoff.  Each is
		 * a racing snapshot by design (see above).
		 */
		seq_printf(m,
			   "%016lx %016lx [%lx,%lx)           %02x %-4s %04x %-3u %s %016lx %08lx\n",
			   (unsigned long)READ_ONCE(ar->mm),
			   (unsigned long)(carrier ? carrier :
				READ_ONCE(ar->vma)),
			   ar->start, ar->end, ar->prot,
			   corten_region_class_name(READ_ONCE(ar->rclass)),
			   READ_ONCE(ar->rflags), READ_ONCE(ar->npieces),
			   percpu_ref_is_dying(&ar->active) ?
				"dying" : "active",
			   (unsigned long)rfile,
			   rfile ? (unsigned long)READ_ONCE(ar->rpoff) :
					0UL);
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
/* M6.T3 helpers used by the stats renderer below; defined with the
 * shrinker section (the renderer walks the same registry the shrinker
 * scans).
 */
static int corten_registry_pin(struct mm_struct **mms, int max, long *skip);
static long corten_mm_state_pages(struct mm_struct *mm, long *swapped_out);

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
	/* V-B.2 (H7) / W1.b: the file-event route and the backstops
	 * (the second and third must stay 0).
	 */
	seq_printf(m, "truncate_routes     %ld\n",
		   atomic_long_read(&corten_nr_truncate_routes));
	seq_printf(m, "zap_single_refuses  %ld\n",
		   atomic_long_read(&corten_nr_zap_single_refuses));
	seq_printf(m, "imap_stale_refuses  %ld\n",
		   atomic_long_read(&corten_nr_imap_stale_refuses));
	/* W1.d: the ttu hook's file demotions (the reclaim family). */
	seq_printf(m, "ttu_routes          %ld\n",
		   atomic_long_read(&corten_nr_ttu_routes));
	/* V-B.4 (H10): the FILE ledger. */
	seq_printf(m, "file_mmaps          %ld\n",
		   atomic_long_read(&corten_nr_file_mmaps));
	seq_printf(m, "file_read_faults    %ld\n",
		   atomic_long_read(&corten_nr_file_read_faults));
	seq_printf(m, "file_cow_copies     %ld\n",
		   atomic_long_read(&corten_nr_file_cow_copies));
	seq_printf(m, "file_fork_mirrors   %ld\n",
		   atomic_long_read(&corten_nr_file_fork_mirrors));
	/* M4.T1 magazine observability. */
	seq_printf(m, "seg_claims          %ld\n",
		   atomic_long_read(&corten_nr_seg_claims));
	seq_printf(m, "mag_skips           %ld\n",
		   atomic_long_read(&corten_nr_mag_skips));
	seq_printf(m, "va_recycles         %ld\n",
		   atomic_long_read(&corten_nr_va_recycles));
	/* T1c resident arena pool. */
	seq_printf(m, "pool_parks          %ld\n",
		   atomic_long_read(&corten_nr_pool_parks));
	seq_printf(m, "pool_hits           %ld\n",
		   atomic_long_read(&corten_nr_pool_hits));
	seq_printf(m, "pool_misses         %ld\n",
		   atomic_long_read(&corten_nr_pool_misses));
	seq_printf(m, "pool_over           %ld\n",
		   atomic_long_read(&corten_nr_pool_over));
	seq_printf(m, "pool_ejects         %ld\n",
		   atomic_long_read(&corten_nr_pool_ejects));
	seq_printf(m, "park_unmap_fails    %ld\n",
		   atomic_long_read(&corten_nr_park_unmap_fails));
	/* V-A.2a: the validation-gate refusals and the J1 prelude pair
	 * (probes/hits, sec 1.3; the full J1/J2 audit walker is V-A.3).
	 * V-A.2b: the carrier creation count.
	 */
	seq_printf(m, "auto_vgate          %ld\n",
		   atomic_long_read(&corten_nr_auto_vgate));
	seq_printf(m, "j1_probes           %ld\n",
		   atomic_long_read(&corten_nr_j1_probes));
	seq_printf(m, "j1_hits             %ld\n",
		   atomic_long_read(&corten_nr_j1_hits));
	/* V-A.3b J1-hygiene funnels (audit #1/#2/#3/#7/#29); the two
	 * observation counters stay non-zero until V-C's gup probe.
	 */
	seq_printf(m, "fault_fallback_win  %ld\n",
		   atomic_long_read(&corten_nr_fault_fallback_window));
	seq_printf(m, "uffd_window_reject  %ld\n",
		   atomic_long_read(&corten_nr_uffd_window_reject));
	seq_printf(m, "gup_window_miss     %ld\n",
		   atomic_long_read(&corten_nr_gup_window_miss));
	seq_printf(m, "remote_win_short    %ld\n",
		   atomic_long_read(&corten_nr_remote_access_window_short));
	/* V-A.3c: the INV-MV2 walker ledger.  j2_stale is a classification
	 * (a registry entry whose mmap never installed / was munmapped),
	 * not a failure; j2_violations must stay 0.
	 */
	seq_printf(m, "j2_walks            %ld\n",
		   atomic_long_read(&corten_nr_j2_walks));
	seq_printf(m, "j2_violations       %ld\n",
		   atomic_long_read(&corten_nr_j2_violations));
	seq_printf(m, "j2_stale            %ld\n",
		   atomic_long_read(&corten_nr_j2_stale));
	seq_printf(m, "j2_first_violation  %lx\n",
		   atomic_long_read(&corten_j2_first_violation));
	/* V-A.3d S-5 terminals (j2-audit #13/#20/#23/#28). */
	seq_printf(m, "msync_window_skips  %ld\n",
		   atomic_long_read(&corten_nr_msync_window_skips));
	seq_printf(m, "mincore_routes      %ld\n",
		   atomic_long_read(&corten_nr_mincore_routes));
	seq_printf(m, "madvise_parked      %ld\n",
		   atomic_long_read(&corten_nr_madvise_parked));
	seq_printf(m, "move_pages_window   %ld\n",
		   atomic_long_read(&corten_nr_move_pages_window));
	/* V-C: the dual-source data/observation faces. */
	seq_printf(m, "gup_probes          %ld\n",
		   atomic_long_read(&corten_nr_gup_probes));
	seq_printf(m, "gup_probe_rejects   %ld\n",
		   atomic_long_read(&corten_nr_gup_probe_rejects));
	seq_printf(m, "maps_window_rows    %ld\n",
		   atomic_long_read(&corten_nr_maps_window_rows));
	/* V-E: the brk delegation ledger (spec sec 3.5, OQ-MV-7) -- arm
	 * answers per MODE-mm sys_brk call, and the heap-domain find_vma
	 * lookups (the heap-fault share numerator).
	 */
	seq_printf(m, "brk_grow            %ld\n",
		   atomic_long_read(&corten_nr_brk_grow));
	seq_printf(m, "brk_shrink          %ld\n",
		   atomic_long_read(&corten_nr_brk_shrink));
	seq_printf(m, "brk_noop            %ld\n",
		   atomic_long_read(&corten_nr_brk_noop));
	seq_printf(m, "brk_reject          %ld\n",
		   atomic_long_read(&corten_nr_brk_reject));
	seq_printf(m, "heap_lookups        %ld\n",
		   atomic_long_read(&corten_nr_heap_lookups));
	seq_printf(m, "carriers            %ld\n",
		   atomic_long_read(&corten_nr_carriers));
	seq_printf(m, "zap_pinned          %ld\n",
		   atomic_long_read(&corten_nr_zap_pinned));
	/* M6.T1 reclaim-path guards (M6_RMAP_SPEC.md sec 1.3 V1/V2). */
	seq_printf(m, "reap_skips          %ld\n",
		   atomic_long_read(&corten_nr_reap_skips));
	seq_printf(m, "rmap_rejects        %ld\n",
		   atomic_long_read(&corten_nr_rmap_rejects));
	/* M6.T2 swap transaction (spec sec 2.1 D1/D5). */
	seq_printf(m, "swapped_out         %ld\n",
		   atomic_long_read(&corten_nr_swapped_out));
	seq_printf(m, "swapins             %ld\n",
		   atomic_long_read(&corten_nr_swapins));
	seq_printf(m, "swapin_retries      %ld\n",
		   atomic_long_read(&corten_nr_swapin_retries));
	seq_printf(m, "swapin_heals        %ld\n",
		   atomic_long_read(&corten_nr_swapin_heals));
	seq_printf(m, "zap_swap_frees      %ld\n",
		   atomic_long_read(&corten_nr_zap_swap_frees));
	/* W1.e1 native driver attribution (the evict leg's replacement for
	 * the ttu-based reclaim; the shrinker leg joins in W1.e2).
	 */
	seq_printf(m, "driver_swapped      %ld\n",
		   atomic_long_read(&corten_nr_driver_swapped));
	seq_printf(m, "driver_kept         %ld\n",
		   atomic_long_read(&corten_nr_driver_kept));
	/* V-D (B-2 closure ledger): upper tables the exit walk retired. */
	seq_printf(m, "exit_upper_pmds     %ld\n",
		   atomic_long_read(&corten_nr_exit_upper_pmds));
	seq_printf(m, "exit_upper_puds     %ld\n",
		   atomic_long_read(&corten_nr_exit_upper_puds));
	seq_printf(m, "exit_upper_p4ds     %ld\n",
		   atomic_long_read(&corten_nr_exit_upper_p4ds));
	/* V-D (S-3 disclosure): swapoff unuse visits blind to windows. */
	seq_printf(m, "unuse_blind_mms     %ld\n",
		   atomic_long_read(&corten_nr_unuse_blind_mms));
	/* M6.T3 shrinker pressure channel + M6.T4 observability. */
	seq_printf(m, "shrink_scans        %ld\n",
		   atomic_long_read(&corten_nr_shrink_scans));
	seq_printf(m, "aging_passes        %ld\n",
		   atomic_long_read(&corten_nr_aging_passes));
	seq_printf(m, "shrink_swapped      %ld\n",
		   atomic_long_read(&corten_nr_shrink_swapped));
	seq_printf(m, "shrink_skipped      %ld\n",
		   atomic_long_read(&corten_nr_shrink_skipped));
	seq_printf(m, "evict_busy          %ld\n",
		   atomic_long_read(&corten_nr_evict_busy));
	/* M6.T4: live per-desc slot totals over the registry (resident
	 * content vs recorded swap entries) and the pages/s deltas since
	 * the previous arena_stats read (the rate evidence the guest
	 * run greps).
	 */
	{
		long skip = 0, resident = 0, swapped = 0;

		for (;;) {
			struct mm_struct *mms[16];
			int i, found;

			found = corten_registry_pin(mms, ARRAY_SIZE(mms),
						    &skip);
			if (!found)
				break;
			for (i = 0; i < found; i++) {
				struct mm_struct *mm = mms[i];
				long sw;

				resident += corten_mm_state_pages(mm, &sw);
				swapped += sw;
				mmput(mm);
			}
			skip += found;
		}
		seq_printf(m, "resident_pages      %ld\n", resident);
		seq_printf(m, "swapped_pages       %ld\n", swapped);

		spin_lock(&corten_rate_lock);
		{
			ktime_t now = ktime_get();
			long out = atomic_long_read(&corten_nr_swapped_out);
			long in = atomic_long_read(&corten_nr_swapins);
			long rate_out = 0, rate_in = 0;
			s64 ms = ktime_to_ms(ktime_sub(now, corten_rate_ts));

			if (ms > 0) {
				rate_out = div64_s64((s64)(out -
					atomic_long_read(&corten_rate_last_out)) *
					MSEC_PER_SEC, ms);
				rate_in = div64_s64((s64)(in -
					atomic_long_read(&corten_rate_last_in)) *
					MSEC_PER_SEC, ms);
			}
			seq_printf(m, "swapout_rate        %ld\n", rate_out);
			seq_printf(m, "swapin_rate         %ld\n", rate_in);
			atomic_long_set(&corten_rate_last_out, out);
			atomic_long_set(&corten_rate_last_in, in);
			corten_rate_ts = now;
		}
		spin_unlock(&corten_rate_lock);
	}
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

/* M4.T1 hooks: drive the magazine allocator against an explicit cpu (the
 * segment state is plain per-mm memory; every writer holds the write
 * lock, which the KUnit caller takes) and observe the bookkeeping.
 */
int corten_arena_test_mag_alloc_cpu(struct mm_struct *mm, int cpu,
				    unsigned long len, unsigned long *addr)
{
	struct corten_mm_state *state;
	int ret;

	/* Pairs with the release store in corten_arena_state_create(). */
	state = smp_load_acquire(&mm->corten_state);
	if (!state)
		return -ENOENT;

	/* Magazine writers hold this mm's mmap_lock for writing; the hook
	 * is self-contained so KUnit cannot get the lock contract wrong
	 * (find_vma_intersection asserts it).
	 */
	mmap_write_lock(mm);
	ret = corten_va_mag_alloc_cpu(mm, state, len, addr, cpu);
	mmap_write_unlock(mm);

	return ret;
}

bool corten_arena_test_frame_reserved(struct mm_struct *mm, unsigned long addr)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);

	if (!state)
		return false;
	return xa_load(&state->arenas, addr >> PMD_SHIFT) ==
	       &corten_va_reserve_sentinel;
}

long corten_arena_test_mag_skips(void)
{
	return atomic_long_read(&corten_nr_mag_skips);
}

long corten_arena_test_seg_claims(void)
{
	return atomic_long_read(&corten_nr_seg_claims);
}

long corten_arena_test_va_recycles(void)
{
	return atomic_long_read(&corten_nr_va_recycles);
}

/* T1c pool hooks: named counters, the pool occupancy and the parked
 * probe.  The test owns @mm exclusively, so the plain (unlocked) reads
 * are the established pattern here (cf. test_frame_reserved()).
 */
long corten_arena_test_pool_parks(void)
{
	return atomic_long_read(&corten_nr_pool_parks);
}

long corten_arena_test_pool_hits(void)
{
	return atomic_long_read(&corten_nr_pool_hits);
}

long corten_arena_test_pool_misses(void)
{
	return atomic_long_read(&corten_nr_pool_misses);
}

long corten_arena_test_pool_over(void)
{
	return atomic_long_read(&corten_nr_pool_over);
}

long corten_arena_test_pool_ejects(void)
{
	return atomic_long_read(&corten_nr_pool_ejects);
}

long corten_arena_test_park_unmap_fails(void)
{
	return atomic_long_read(&corten_nr_park_unmap_fails);
}

/* V-A.2a/A.2b hooks: the carrier of the arena at @addr (NULL when the
 * arena anchors on a tree shadow-VMA), the carrier creation counter,
 * and the J1 prelude pair.
 */
struct vm_area_struct *corten_arena_test_carrier_of(struct mm_struct *mm,
						    unsigned long addr)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);
	struct corten_arena *ar;

	if (!state)
		return NULL;

	ar = xa_load(&state->arenas, addr >> PMD_SHIFT);

	return ar ? READ_ONCE(ar->carrier) : NULL;
}

long corten_arena_test_carriers(void)
{
	return atomic_long_read(&corten_nr_carriers);
}

long corten_arena_test_auto_vgate(void)
{
	return atomic_long_read(&corten_nr_auto_vgate);
}

long corten_arena_test_j1_probes(void)
{
	return atomic_long_read(&corten_nr_j1_probes);
}

long corten_arena_test_j1_hits(void)
{
	return atomic_long_read(&corten_nr_j1_hits);
}

/* V-A.3b: the J1-hygiene funnel counters (B-group anchors). */
long corten_arena_test_fault_fallback_window(void)
{
	return atomic_long_read(&corten_nr_fault_fallback_window);
}

long corten_arena_test_uffd_rejects(void)
{
	return atomic_long_read(&corten_nr_uffd_window_reject);
}

/* V-C: the dual-source counters (gup probe matrix + window rows). */
long corten_arena_test_gup_probes(void)
{
	return atomic_long_read(&corten_nr_gup_probes);
}

long corten_arena_test_gup_probe_rejects(void)
{
	return atomic_long_read(&corten_nr_gup_probe_rejects);
}

long corten_arena_test_maps_window_rows(void)
{
	return atomic_long_read(&corten_nr_maps_window_rows);
}

/* V-B.2 (H7): the file-event route counter and the zap backstop. */
long corten_arena_test_truncate_routes(void)
{
	return atomic_long_read(&corten_nr_truncate_routes);
}

/* W1.d: the ttu hook's file demotions. */
long corten_arena_test_ttu_routes(void)
{
	return atomic_long_read(&corten_nr_ttu_routes);
}

long corten_arena_test_zap_single_refuses(void)
{
	return atomic_long_read(&corten_nr_zap_single_refuses);
}

/* W1.b: the stale-node backstop and the per-inode registry occupancy. */
long corten_arena_test_imap_stale_refuses(void)
{
	return atomic_long_read(&corten_nr_imap_stale_refuses);
}

long corten_arena_test_registry_size(struct address_space *mapping)
{
	struct corten_inode_regions *reg;

	reg = xa_load(&corten_inode_regions, (unsigned long)mapping);
	return reg ? READ_ONCE(reg->nr) : 0;
}

bool corten_arena_test_registry_empty(void)
{
	return xa_empty(&corten_inode_regions);
}

long corten_arena_test_pool_nr(struct mm_struct *mm)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);

	if (!state)
		return 0;
	return READ_ONCE(state->nr_pool);
}

bool corten_arena_test_pool_idle(struct mm_struct *mm, unsigned long addr)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);
	struct corten_arena *ar;

	if (!state)
		return false;
	ar = xa_load(&state->arenas, addr >> PMD_SHIFT);

	return ar && ar != &corten_va_reserve_sentinel &&
	       READ_ONCE(ar->idle);
}

/* V-A.1 hook: does @addr's window still carry a tracked PT page?  The
 * park's pure reservation is frames + idle descriptor with the tables
 * retired.
 */
bool corten_arena_test_pt_present(struct mm_struct *mm, unsigned long addr)
{
	pmd_t *pmdp = corten_arena_pmd(mm, addr);

	return pmdp && pmd_present(READ_ONCE(*pmdp)) &&
	       !pmd_leaf(READ_ONCE(*pmdp));
}

/* V-A.1 hook (INV-MV3-adjacent encoding anchor): the shadow-VMA-derived
 * and the pure perm-derived PTE encodings must agree for every perm an
 * arena whitelist can carry (a private non-pkey VMA's protection-map
 * index is the R/W/X bits alone).
 */
bool corten_arena_test_perm_pgprot_pure_eq(u8 perm)
{
	struct vm_area_struct vma = { };
	vm_flags_t flags = VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC |
			   VM_NORESERVE;

	if (perm & CORTEN_PERM_READ)
		flags |= VM_READ;
	if (perm & CORTEN_PERM_WRITE)
		flags |= VM_WRITE;
	if (perm & CORTEN_PERM_EXEC)
		flags |= VM_EXEC;
	vm_flags_init(&vma, flags);

	return pgprot_val(corten_arena_perm_pgprot(&vma, perm)) ==
	       pgprot_val(corten_arena_perm_pgprot_pure(perm));
}

/* V-A.3a placement-surface hooks: the two "normally zero" disclosure
 * counters (a non-zero value in a green run means a defensive layer
 * answered a placement request) and the P1b idle-eject count.
 */
long corten_arena_test_placement_backstop(void)
{
	return atomic_long_read(&corten_nr_placement_backstop);
}

long corten_arena_test_p4_ejects(void)
{
	return atomic_long_read(&corten_nr_p4_ejects);
}

long corten_arena_test_placement_idle_ejects(void)
{
	return atomic_long_read(&corten_nr_placement_idle_ejects);
}

/* V-A.3a implant registry occupancy (the walker's whitelist predicate
 * anchor; read under the caller's mmap_read or better).
 */
long corten_arena_test_implant_nr(struct mm_struct *mm)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);

	if (!state)
		return 0;
	return state->nr_implants;
}

/* V-A.3c INV-MV2 walker anchors (C-group): the ledger reads and the
 * archived first-violation address.
 */
long corten_arena_test_j2_walks(void)
{
	return atomic_long_read(&corten_nr_j2_walks);
}

/* V-D (B-2 ledger): the exit walk's upper-table retirement counts. */
long corten_arena_test_exit_upper_pmds(void)
{
	return atomic_long_read(&corten_nr_exit_upper_pmds);
}

long corten_arena_test_exit_upper_puds(void)
{
	return atomic_long_read(&corten_nr_exit_upper_puds);
}

long corten_arena_test_exit_upper_p4ds(void)
{
	return atomic_long_read(&corten_nr_exit_upper_p4ds);
}

/* V-D (S-3 disclosure): swapoff unuse visits blind to windows. */
long corten_arena_test_unuse_blind_mms(void)
{
	return atomic_long_read(&corten_nr_unuse_blind_mms);
}

long corten_arena_test_j2_violations(void)
{
	return atomic_long_read(&corten_nr_j2_violations);
}

long corten_arena_test_j2_stale(void)
{
	return atomic_long_read(&corten_nr_j2_stale);
}

long corten_arena_test_j2_first_violation(void)
{
	return atomic_long_read(&corten_j2_first_violation);
}

/* V-E: the brk delegation ledger and the whitelist (J2-complete)
 * ledger.  The brk arm read takes the arm index (enum corten_brk_arm,
 * shared header); the whitelist histogram anchor walks @mm once with
 * the self-sufficient entry and fills @counts with this walk's
 * per-class VMA counts (array of CORTEN_WL_NR_CLASSES unsigned long).
 * Forward-declared: the scan body lives with the audit walkers below.
 */
static int corten_whitelist_scan(struct mm_struct *mm,
				 const struct corten_implant_range *implants,
				 unsigned int nr_implants,
				 unsigned long *hist);
long corten_arena_test_brk_arm(int arm)
{
	switch (arm) {
	case CORTEN_BRK_GROW:
		return atomic_long_read(&corten_nr_brk_grow);
	case CORTEN_BRK_SHRINK:
		return atomic_long_read(&corten_nr_brk_shrink);
	case CORTEN_BRK_NOOP:
		return atomic_long_read(&corten_nr_brk_noop);
	case CORTEN_BRK_REJECT:
		return atomic_long_read(&corten_nr_brk_reject);
	default:
		return 0;
	}
}

long corten_arena_test_heap_lookups(void)
{
	return atomic_long_read(&corten_nr_heap_lookups);
}

long corten_arena_test_wl_walks(void)
{
	return atomic_long_read(&corten_nr_wl_walks);
}

long corten_arena_test_wl_violations(void)
{
	return atomic_long_read(&corten_nr_wl_violations);
}

long corten_arena_test_wl_brk_anomalies(void)
{
	return atomic_long_read(&corten_nr_wl_brk_anomalies);
}

long corten_arena_test_wl_brk_vmas(void)
{
	return atomic_long_read(&corten_nr_wl_brk_vmas);
}

void corten_arena_test_wl_histogram(struct mm_struct *mm,
				    unsigned long *counts)
{
	struct corten_mm_state *state;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode))
		return;
	state = READ_ONCE(mm->corten_state);
	if (!state)
		return;

	mutex_lock(&state->ctl_lock);
	corten_whitelist_scan(mm, state->implants, state->nr_implants,
			      counts);
	mutex_unlock(&state->ctl_lock);
}
#endif

/* ------------------------------------------------------------------ *
 * process exit (sec 5.2)
 * ------------------------------------------------------------------
 */

/* V-D helpers: the window-domain tree test and the all-none table
 * scans.  A level's page can only be retired when nothing that the
 * legacy pass will still walk can reach it: every tree VMA in the
 * level's span defers to free_pgtables() (it will descend exactly
 * those spans after the walk returns), and every entry left in the
 * page itself defers the free outright (a non-none entry that is not
 * a window the walk just cleared is an anomaly -- the page stays and
 * the residual pgtables_bytes discloses it, rather than freeing a
 * table something may still be walking).
 */
static bool corten_arena_exit_span_clear(struct mm_struct *mm,
					 unsigned long start,
					 unsigned long end)
{
	VMA_ITERATOR(vmi, mm, start);
	struct vm_area_struct *vma;

	for_each_vma_range(vmi, vma, end)
		return false;

	return true;
}

static bool corten_arena_exit_pmd_page_clear(pmd_t *pmdp)
{
	int i;

	for (i = 0; i < PTRS_PER_PMD; i++)
		if (!pmd_none(READ_ONCE(pmdp[i])))
			return false;

	return true;
}

static bool corten_arena_exit_pud_page_clear(pud_t *pudp)
{
	int i;

	for (i = 0; i < PTRS_PER_PUD; i++)
		if (!pud_none(READ_ONCE(pudp[i])))
			return false;

	return true;
}

static bool corten_arena_exit_p4d_page_clear(p4d_t *p4dp)
{
	int i;

	for (i = 0; i < PTRS_PER_P4D; i++)
		if (!p4d_none(READ_ONCE(p4dp[i])))
			return false;

	return true;
}

/*
 * One walkable run of phase A: the full-clear chunk zap (swap entries
 * released and counted) followed by the PTE-page retirement, against
 * the walk's gather.  A zap failure at mm_users == 0 is a kernel bug
 * (no transaction can be in flight); the run is still retired as far
 * as it got and the residue is left to the leak ledger -- WARN once,
 * never hang the dying process.
 */
static void corten_arena_exit_run(struct mm_struct *mm,
				  struct corten_arena *arena,
				  struct mmu_gather *tlb,
				  unsigned long start, unsigned long end)
{
	if (corten_arena_unmap_chunk_flags(mm, arena, start, end - start,
					   0, tlb))
		WARN_ONCE(1,
			  "corten: exit walk zap failed at [%lx,%lx): PT residue left to the leak ledger\n",
			  start, end);
	corten_arena_free_ptes_span(mm, tlb, start, end);
}

/*
 * V-D (MV_VMA_FREE_SPEC.md sec 3.4): the pure-PT exit walk.  mm_users
 * is 0 and both the tree and the registry are frozen, so the window
 * domain tears its own page tables down instead of waiting for the
 * VMA-bounded free_pgtables() that can never see it:
 *
 *   A) per PMD window still owned by an arena frame slot: full-clear
 *      zap (swap entries released and counted, the M6.T2 arm) + PTE
 *      page retirement through the pte_free_tlb() funnel.  Windows a
 *      tree VMA covers (targeted-DECLARE shadows, punch implants) and
 *      punched-hole frames (slot erased, a legacy VMA lives there)
 *      stay with the legacy unmap_vmas()/free_pgtables() pass that
 *      exit_mmap() runs after this returns -- the walk works at
 *      window granularity, so a punched arena's surviving windows no
 *      longer ride the old arena-level "any VMA in span" skip (their
 *      content and PT pages used to leak at mm death).
 *   B) the upper tables fill_upper() built, retired wherever the
 *      window domain holds a level's page exclusively: PMD pages per
 *      PUD_SIZE span, PUD pages per P4D span, P4D pages per PGDIR
 *      span (runtime-folded levels skip themselves).  pgtables_bytes
 *      returns to zero with them -- the B-2 closure.
 *
 * Everything runs on one fullmm mmu_gather (INV6: every PT page free
 * goes through the pX_free_tlb() batched funnels; the whole mm is
 * dying, so range tracking is moot).  mmap_write is held across the
 * walk -- the strongest fence the legacy pass itself uses
 * (free_pgtables() runs under it in exit_mmap()), and with mm_users 0
 * nobody can contest it: the zap's covering-VMA lookups want a lock to
 * run under, and the tree queries above are its readers.  Runs BEFORE
 * the unpublish (the zap's stats bookkeeping reads mm->corten_state)
 * and BEFORE the drain (the zap anchors rmap on the carrier, which the
 * drain's descriptor teardown frees) -- the j2 audit walk at the
 * mm_exit head already saw the untouched picture.
 */
static void corten_arena_exit_walk(struct mm_struct *mm,
				   struct corten_mm_state *state)
{
	/* Pointer variable for the pX_free_tlb() macros: they substitute
	 * their first argument unparenthesized, so "&tlb" would expand to
	 * "&tlb->freed_tables" (the free_pte_range() shape convention).
	 */
	struct mmu_gather tlb_;
	struct mmu_gather *tlb = &tlb_;
	struct corten_arena *arena;
	unsigned long frame, win, run_start = 0;
	unsigned long seg, walked_until = 0;
	unsigned long done_pmd_seg = 0, done_pud_seg = 0;
	bool have_run = false;

	mmap_write_lock(mm);
	tlb_gather_mmu_fullmm(tlb, mm);

	/* A) zap + PTE-page retirement, window by window. */
	frame = 0;
	xa_for_each(&state->arenas, frame, arena) {
		if (arena == &corten_va_reserve_sentinel)
			continue;

		/* Every frame of an arena holds the same descriptor; walk
		 * each one once, at its first frame.
		 */
		if (frame < walked_until)
			continue;
		walked_until = arena->end >> PMD_SHIFT;

		for (win = arena->start & PMD_MASK; win < arena->end;
		     win += PMD_SIZE) {
			bool walkable;

			/* A punched frame is no longer arena property
			 * (a legacy VMA lives in the hole); the tree
			 * test catches its implant anyway.
			 */
			if (xa_load(&state->arenas,
				    win >> PMD_SHIFT) != arena)
				walkable = false;
			else
				walkable = corten_arena_exit_span_clear(mm,
									win,
									win + PMD_SIZE);

			if (walkable && !have_run) {
				have_run = true;
				run_start = win;
			} else if (!walkable && have_run) {
				have_run = false;
				corten_arena_exit_run(mm, arena, tlb,
						      run_start, win);
			}
		}

		if (have_run) {
			have_run = false;
			corten_arena_exit_run(mm, arena, tlb, run_start,
					      arena->end);
		}
	}

	/* B) upper-table self-teardown, one ascending registry pass per
	 * level.  Segments are visited in ascending arena order; the
	 * done_* cursors keep one level's page processed exactly once
	 * when neighbouring arenas share it.
	 *
	 * The pass separation is load-bearing: a level's page can only be
	 * retired once every page the level below holds in its span is
	 * gone, and that lower-level retirement of a later arena runs
	 * AFTER the first arena reaches the shared upper page.  A single
	 * interleaved pass tested the upper page while a later arena's
	 * lower pages still sat in it, skipped it (the conservative
	 * leave-it rule) and never came back -- the mm's last retiring PMD
	 * page stranded the PUD page above it as a 4096 pgtables_bytes
	 * residue whenever two arenas shared a P4D span from different
	 * PUD segments (the guest metis shape: an 8MiB region at the
	 * window base and a chunk-carved 128MiB arena one segment up).
	 */
	frame = 0;
	walked_until = 0;
	xa_for_each(&state->arenas, frame, arena) {
		if (arena == &corten_va_reserve_sentinel)
			continue;

		/* Same first-frame dedupe as phase A. */
		if (frame < walked_until)
			continue;
		walked_until = arena->end >> PMD_SHIFT;

		/* Pass B1 -- PMD pages: one per PUD_SIZE span. */
		for (seg = arena->start & PUD_MASK; seg < arena->end;
		     seg += PUD_SIZE) {
			pud_t *pudp;
			pmd_t *pmdp;

			if (seg < done_pmd_seg)
				continue;
			done_pmd_seg = seg + PUD_SIZE;

			pudp = corten_arena_pud(mm, seg);
			if (!pudp || !pud_present(READ_ONCE(*pudp)) ||
			    pud_leaf(READ_ONCE(*pudp)))
				continue;
			if (!corten_arena_exit_span_clear(mm, seg,
							  seg + PUD_SIZE))
				continue;
			pmdp = pmd_offset(pudp, seg);
			if (!corten_arena_exit_pmd_page_clear(pmdp))
				continue;

			pud_clear(pudp);
			pmd_free_tlb(tlb, pmdp, seg);
			mm_dec_nr_pmds(mm);
			atomic_long_inc(&corten_nr_exit_upper_pmds);
		}
	}

	/* Pass B2 -- PUD pages: one per P4D span (the pgd slot itself on
	 * a runtime-folded p4d -- p4d_clear() handles both).  Runs after
	 * the B1 pass retired every PMD page, so the all-none scan below
	 * sees the span's whole lower level gone.
	 */
	frame = 0;
	walked_until = 0;
	xa_for_each(&state->arenas, frame, arena) {
		if (arena == &corten_va_reserve_sentinel)
			continue;

		if (frame < walked_until)
			continue;
		walked_until = arena->end >> PMD_SHIFT;

		for (seg = arena->start & P4D_MASK; seg < arena->end;
		     seg += P4D_SIZE) {
			p4d_t *p4dp;
			pud_t *pudp;

			if (seg < done_pud_seg)
				continue;
			done_pud_seg = seg + P4D_SIZE;

			p4dp = p4d_offset(pgd_offset(mm, seg), seg);
			if (!p4d_present(READ_ONCE(*p4dp)) ||
			    p4d_leaf(READ_ONCE(*p4dp)))
				continue;
			if (!corten_arena_exit_span_clear(mm, seg,
							  seg + P4D_SIZE))
				continue;
			pudp = pud_offset(p4dp, seg);
			if (!corten_arena_exit_pud_page_clear(pudp))
				continue;

			p4d_clear(p4dp);
			pud_free_tlb(tlb, pudp, seg);
			mm_dec_nr_puds(mm);
			atomic_long_inc(&corten_nr_exit_upper_puds);
		}
	}

	/* Pass B3 -- P4D pages: real only on a runtime-unfolded p4d; the
	 * p4d_free_tlb() funnel no-ops on the folded shape, so the guard
	 * merely skips dead work.  (p4d pages carry no pgtables_bytes
	 * account -- upstream frees them the same unaccounted way in
	 * free_p4d_range().)  Same pass-separation argument as B2: it
	 * runs after every PUD page of the span is gone.
	 */
	if (!mm_p4d_folded(mm)) {
		frame = 0;
		walked_until = 0;
		xa_for_each(&state->arenas, frame, arena) {
			if (arena == &corten_va_reserve_sentinel)
				continue;

			if (frame < walked_until)
				continue;
			walked_until = arena->end >> PMD_SHIFT;

			for (seg = arena->start & PGDIR_MASK; seg < arena->end;
			     seg += PGDIR_SIZE) {
				pgd_t *pgdp;
				p4d_t *p4dp;

				pgdp = pgd_offset(mm, seg);
				if (!pgd_present(READ_ONCE(*pgdp)))
					continue;
				if (!corten_arena_exit_span_clear(mm, seg,
								  seg + PGDIR_SIZE))
					continue;
				p4dp = p4d_offset(pgdp, seg);
				if (!corten_arena_exit_p4d_page_clear(p4dp))
					continue;

				pgd_clear(pgdp);
				p4d_free_tlb(tlb, p4dp, seg);
				atomic_long_inc(&corten_nr_exit_upper_p4ds);
			}
		}
	}

	tlb_finish_mmu(tlb);
	mmap_write_unlock(mm);
}

/*
 * Called from exit_mmap() before the legacy teardown: walk the window
 * domain's page tables down (V-D), then drain and free every arena and
 * drop the registry.  mm_users is already 0 here, so no fault can be in
 * flight and the drain is purely defensive; the unmap_vmas() that
 * follows retires the tree VMAs' page tables (targeted shadows, punch
 * implants, the delegated domain) through the regular free funnels.
 */
void corten_arena_mm_exit(struct mm_struct *mm)
{
	struct corten_mm_state *state;
	unsigned long frame = 0, drained_until = 0;
	struct corten_arena *arena;
	bool had_arenas;

	/* Pairs with the store in corten_arena_state_create(). */
	state = smp_load_acquire(&mm->corten_state);
	if (!state)
		return;

	/* V-A.3c lifecycle trigger (j2-audit hook list): the INV-MV2 exit
	 * audit runs first, before any drain or zap mutates the picture --
	 * mm_users is 0, so both the tree and the registry are frozen and
	 * the stable-registry walker entry needs no lock.  A violation here
	 * is the process's final report card: every placement guard it ever
	 * ran under had its say.  V-E appends the whitelist pass over the
	 * full tree (the J2-complete form): same frozen picture, and the
	 * delegated composition (heap/stack/special/file/anon buckets) of
	 * every departing MODE mm is tallied for the audit gate.
	 */
	corten_audit_j2_walk_locked(mm);
	corten_audit_whitelist_walk_locked(mm);

	/* V-D: the pure-PT walk (zap + PTE/upper-table retirement) runs
	 * before the unpublish below -- the zap's stats bookkeeping reads
	 * mm->corten_state, which the unpublish clears -- and before the
	 * drain, whose descriptor teardown frees the zap's rmap anchor
	 * (the carrier).  It runs ctl-free deliberately: the lock order
	 * everywhere else is mmap > ctl, and the walk already holds
	 * mmap_write.  Safety needs neither lock: mm_users is 0, so no
	 * fault or transaction can appear, and the descriptor trees' own
	 * locks fence the walk against everything dead.
	 */
	corten_arena_exit_walk(mm, state);

	/* Unpublish; no reader can be racing (mm_users == 0). */
	smp_store_release(&mm->corten_state, NULL);

	/* A5 (G5-fix): did this registry ever hold arena state?  The
	 * pool bookkeeping below is reset before the drain, so sample
	 * it here.  An arena-less registry (the ENTER-only MODE
	 * lifecycle that used to pay one synchronize_rcu() per mm at
	 * exit -- the G5 fork/shell regression) takes the deferred
	 * teardown at the bottom.
	 */
	had_arenas = refcount_read(&state->nr) || READ_ONCE(state->nr_pool);

	/* T1c: parked arenas carry no bookkeeping beyond their pool node;
	 * the walk below drains and frees them like any other descriptor,
	 * so only the list head itself needs resetting here.
	 */
	INIT_LIST_HEAD(&state->arena_pool);
	state->nr_pool = 0;

	/* DEV-13: exit drains with mm_users already 0 and takes only the
	 * lower ctl_lock -- "the lower lock alone" cannot form a cycle.
	 */
	mutex_lock(&state->ctl_lock);

	xa_for_each(&state->arenas, frame, arena) {
		/* M4.T1: reserve markers are not arenas. */
		if (arena == &corten_va_reserve_sentinel)
			continue;

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

	/* M6.T3: leave the shrinker registry first, then wait out the
	 * grace period before the state memory is reused -- a shrinker
	 * reader that found this node under RCU either finished (its mm
	 * reference -- mmget_not_zero -- kept mm->corten_state alive
	 * through its work) or never got past the RCU section.  The
	 * arena-bearing branch below pays the wait synchronously (the
	 * only synchronize_rcu() on the exit path); the A5 arena-less
	 * fast path defers it to the RCU callback instead.  Either way
	 * the registry stays a plain RCU list (no per-node refcount).
	 */
	spin_lock(&corten_mm_registry_lock);
	list_del_rcu(&state->shrink_reg);
	spin_unlock(&corten_mm_registry_lock);

	if (!had_arenas) {
		/* A5 (G5-fix): the grace period is still owed (an
		 * in-flight shrinker reader may hold the node pointer),
		 * but nobody has to stand still for it -- the dying
		 * process hands the teardown to RCU and returns.  This
		 * is the fast path for every MODE lifecycle that never
		 * carried an arena.
		 */
		call_rcu(&state->rcu, corten_arena_state_free_rcu);
		return;
	}

	synchronize_rcu();

	corten_arena_state_free(state);
}

/*
 * V-D (S-3 disclosure, MV_VMA_FREE_SPEC.md sec 4/C18): swapoff's
 * try_to_unuse() reaches this mm's swap entries only through tree
 * VMAs (unuse_mm() is VMA-bounded), and the post-A.2 carrier windows
 * are tree-free -- their swap entries are invisible to the early
 * swap-in optimization and ride until a fault (do_swap_page through
 * the window fault gate) or this mm's exit walk releases them.
 * unuse_mm() calls this under its mmap_read; the counter is pure
 * observation (a disclosure, not a route -- swapoff is not a hot
 * path, and the semantics are the registered S-3 verdict to
 * characterize in the guest retest: bounded spin until the entries
 * drop, never a leak).
 */
void corten_arena_unuse_blind_note(struct mm_struct *mm)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);

	if (state && refcount_read(&state->nr))
		atomic_long_inc(&corten_nr_unuse_blind_mms);
}

/* ------------------------------------------------------------------ *
 * lookup and prctl dispatch
 * ------------------------------------------------------------------
 */

struct corten_arena *corten_arena_lookup(struct mm_struct *mm,
					 unsigned long addr)
{
	struct corten_mm_state *state;
	struct corten_arena *ar;

	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			 "arena lookup without rcu_read_lock() protection");

	/* Two-load fast negation: processes without arenas (and every
	 * non-arena access in arena processes) never reach the xarray.
	 */
	state = smp_load_acquire(&mm->corten_state);
	if (!state || !refcount_read(&state->nr))
		return NULL;

	/* M4.T1: reserve markers of claimed-but-unallocated magazine
	 * frames read as "no arena" -- the frames are not arena property
	 * until handed out and DECLAREd.
	 */
	ar = xa_load(&state->arenas, addr >> PMD_SHIFT);
	if (ar == &corten_va_reserve_sentinel)
		return NULL;

	/* T1c: a parked (pooled) arena is lookup-invisible -- the range
	 * was munmapped and must keep legacy semantics (no arena: faults
	 * fall to the legacy funnel on the reserved PROT_NONE mapping,
	 * munmap/mprotect routes run legacy).  Its window comes back only
	 * through the pool reactivation.
	 */
	if (ar && READ_ONCE(ar->idle))
		return NULL;

	return ar;
}

struct corten_arena *corten_region_lookup(struct mm_struct *mm,
					  unsigned long addr)
{
	/* The registry's point query (sec 2.4) has exactly the existing
	 * lookup's semantics: reserve markers and parked (RESERVED)
	 * windows are not covering regions.
	 */
	return corten_arena_lookup(mm, addr);
}

struct corten_arena *corten_region_next(struct mm_struct *mm,
					struct corten_region_iter *it)
{
	struct corten_mm_state *state;
	struct corten_arena *ar;

	/* Locking contract (sec 2.4): the caller holds mmap_lock for
	 * read or RCU (see the header).  The walk only reads frame slots
	 * -- the same shape the fork/exit registry walks run.
	 */
	state = smp_load_acquire(&mm->corten_state);
	if (!state)
		return NULL;

	for (;;) {
		ar = xa_find(&state->arenas, &it->frame, ULONG_MAX,
			     XA_PRESENT);
		if (!ar)
			return NULL;
		it->frame++;
		/* Magazine reserve markers are not regions. */
		if (ar == &corten_va_reserve_sentinel)
			continue;
		/* Pointer dedup: later frame slots of the region just
		 * produced are skipped, so each region is produced exactly
		 * once (a punched hole's NULL frames are skipped by
		 * xa_find; the pieces table carries the split, sec 2.1).
		 */
		if (ar == it->last)
			continue;
		it->last = ar;
		return ar;
	}
}

/* ------------------------------------------------------------------ *
 * V-C: dual-source consumers (MV_VMA_FREE_SPEC.md sec 3.3) -- the
 * window row stream (/proc rendering), the GUP-slow MODE branch and
 * the remote-access pre-check bypass.  All PTE reads below take the
 * PTE lock (INV6); all region/carrier reads run under mmap_read or
 * better (sec 2.7: mmap_lock is the region record's lock).
 * ------------------------------------------------------------------
 */

/* Implant-registry geometry for the row stream: the sorted, disjoint
 * array is walked inline (nr_implants is small and bounded by the
 * punch/P1b producers).  Both helpers take/return window addresses.
 */
static bool corten_implant_advance(struct mm_struct *mm,
				   unsigned long *addr)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);
	unsigned int i;

	if (!state)
		return false;

	for (i = 0; i < state->nr_implants; i++) {
		if (*addr < state->implants[i].start)
			return false;	/* below the next range: keep */
		if (*addr < state->implants[i].end) {
			*addr = state->implants[i].end;
			return true;	/* was inside: jumped to its end */
		}
	}
	return false;
}

/* Clip *@end down to the start of the first implant inside
 * [@start, *@end) -- the row must stop where a legacy implant begins.
 */
static void corten_implant_clip(struct mm_struct *mm, unsigned long start,
				unsigned long *end)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);
	unsigned int i;

	if (!state)
		return;

	for (i = 0; i < state->nr_implants; i++) {
		if (state->implants[i].start <= start)
			continue;	/* spent (start is past it) */
		if (state->implants[i].start < *end)
			*end = state->implants[i].start;
		return;
	}
}

void corten_row_iter_init(struct corten_row_iter *it)
{
	corten_region_iter_init(&it->rit);
	it->ar = NULL;
	it->next = 0;
}

bool corten_row_next(struct mm_struct *mm, struct corten_row_iter *it,
		     struct corten_region_row *row)
{
	for (;;) {
		struct corten_arena *ar = it->ar;
		unsigned long start, end;

		if (ar) {
			start = it->next;
			/* Punch implants carve legacy-owned sub-ranges out
			 * of the region; a row never spans one.
			 */
			while (start < ar->end &&
			       corten_implant_advance(mm, &start))
				;
			if (start < ar->end) {
				end = ar->end;
				corten_implant_clip(mm, start, &end);
				it->next = end;
				row->ar = ar;
				row->start = start;
				row->end = end;
				atomic_long_inc(&corten_nr_maps_window_rows);
				return true;
			}
			/* Region exhausted: on to the registry stream. */
			it->ar = NULL;
		}

		ar = corten_region_next(mm, &it->rit);
		if (!ar)
			return false;
		/* S-4: a parked window was munmapped -- rendering it
		 * would lie.
		 */
		if (READ_ONCE(ar->idle))
			continue;
		/* Tree-anchored (targeted-DECLARE) arenas are the tree
		 * stream's own rows: producing them here would render
		 * the range twice.
		 */
		if (READ_ONCE(ar->vma))
			continue;
		/* The window stream renders carrier regions only. */
		if (!READ_ONCE(ar->carrier))
			continue;
		it->ar = ar;
		it->next = ar->start;
	}
}

bool corten_row_query(struct mm_struct *mm, unsigned long addr,
		      struct corten_region_row *row)
{
	struct corten_row_iter it;

	/* Covering-or-next: the registry stream starts at @addr's frame,
	 * so regions ending at/below it are never opened, and rows of
	 * the containing region are filtered by the end > @addr test
	 * (the first row that may cover @addr).  A frame erased by a
	 * punch is skipped naturally -- its implant is the tree's row.
	 */
	corten_row_iter_init(&it);
	it.rit.frame = addr >> PMD_SHIFT;
	while (corten_row_next(mm, &it, row)) {
		if (row->end > addr)
			return true;
	}
	return false;
}

bool corten_maps_dual_source(struct mm_struct *mm)
{
	struct corten_mm_state *state;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode))
		return false;
	/* Pairs with the registry publisher's smp_store_release(): the
	 * state pointer and its arena count must read as one snapshot.
	 */
	state = smp_load_acquire(&mm->corten_state);
	return state && refcount_read(&state->nr) != 0;
}

struct vm_area_struct *corten_gup_probe(struct mm_struct *mm,
					unsigned long addr,
					unsigned int gup_flags)
{
	struct vm_area_struct *carrier = NULL;
	enum corten_region_class rclass = CORTEN_REGION_ANON;
	struct corten_arena *ar;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode) ||
	    addr < CORTEN_MODE_WINDOW_START || addr >= CORTEN_MODE_WINDOW_END)
		return NULL;	/* the legacy walk answers */

	/* Implants own real tree VMAs -- find_vma() must see them. */
	if (corten_implant_covers(mm, addr, 1))
		return NULL;

	/* Region resolution under RCU (the registry's read contract);
	 * the class/carrier reads ride the same section, the carrier
	 * pointer itself is what the caller's mmap_read keeps alive.
	 */
	rcu_read_lock();
	ar = corten_region_lookup(mm, addr);
	if (ar) {
		carrier = READ_ONCE(ar->carrier);
		rclass = READ_ONCE(ar->rclass);
	}
	rcu_read_unlock();

	if (ar && !carrier)
		return NULL;	/* tree-anchored arena: find_vma() answers */

	if (!ar) {
		/* Parked (S-1), magazine reserve or window hole: the tree
		 * lookup is a guaranteed miss -- answer its errno here so
		 * the walk (and J1) never happens.
		 */
		atomic_long_inc(&corten_nr_gup_probe_rejects);
		return ERR_PTR(-EFAULT);
	}

	/* check_vma_flags() emulation corner the carrier's anon shape
	 * cannot express: FOLL_ANON must miss a file mapping.
	 */
	if ((gup_flags & FOLL_ANON) && rclass == CORTEN_REGION_FILE) {
		atomic_long_inc(&corten_nr_gup_probe_rejects);
		return ERR_PTR(-EFAULT);
	}

	atomic_long_inc(&corten_nr_gup_probes);
	return carrier;
}

bool corten_remote_vm_window(struct mm_struct *mm, unsigned long addr)
{
	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode) ||
	    addr < CORTEN_MODE_WINDOW_START || addr >= CORTEN_MODE_WINDOW_END)
		return false;
	return !corten_implant_covers(mm, addr, 1);
}

/*
 * smaps PT aggregation of one window row (sec 3.3.1): the
 * mincore-fill skeleton (per-2M-frame PTE walk under ptl, INV6) with
 * the smaps_account() accounting rule reduced to the Rss/Pss/
 * Anonymous/Swap buckets the window truth carries.  PSS uses the
 * folio mapcount divisor (exact for the window's order-0 pages, the
 * same arithmetic smaps_account() applies); the sharing buckets
 * (Shared/Private x Clean/Dirty), Referenced and the hugetlb/KSM
 * families stay zero -- registered disclosure, not parity claims.
 */
/*
 * Per-pte classification for the two /proc walkers below: the page (or
 * NULL for the zero page / none entries) plus the pagemap flag bits
 * and pfn/swap frame encoding (pte_to_pagemap_entry()'s arithmetic).
 */
static struct page *corten_pagemap_pte(struct vm_area_struct *carrier,
				       pte_t pte, unsigned long addr,
				       bool show_pfn, u64 *frame, u64 *flags)
{
	struct page *page = NULL;

	*frame = 0;
	*flags = 0;

	if (pte_present(pte)) {
		*flags |= CORTEN_PM_PRESENT;
		if (show_pfn)
			*frame = pte_pfn(pte);
		page = vm_normal_page(carrier, addr, pte);
	} else if (is_swap_pte(pte)) {
		swp_entry_t entry = pte_to_swp_entry(pte);

		*flags |= CORTEN_PM_SWAP;
		if (show_pfn) {
			pgoff_t offset;

			if (is_pfn_swap_entry(entry))
				offset = swp_offset_pfn(entry);
			else
				offset = swp_offset(entry);
			*frame = swp_type(entry) |
				((u64)offset << MAX_SWAPFILES_SHIFT);
		}
		if (is_pfn_swap_entry(entry))
			page = pfn_swap_entry_to_page(entry);
	}
	/* pte_none: frame/flags stay 0 -- one pme per page, the pagemap
	 * contract.
	 */

	if (page) {
		struct folio *folio = page_folio(page);

		if (!folio_test_anon(folio))
			*flags |= CORTEN_PM_FILE;
		/* PM_MMAP_EXCLUSIVE: the folio mapcount is exact for the
		 * window's order-0 pages (fs/proc/internal.h's precise
		 * page variant inlined).
		 */
		if ((*flags & CORTEN_PM_PRESENT) && folio_mapcount(folio) == 1)
			*flags |= CORTEN_PM_MMAP_EXCLUSIVE;
	}

	return page;
}

/*
 * smaps PT aggregation of one window row (sec 3.3.1): the
 * mincore-fill skeleton (per-2M-frame PTE walk under ptl, INV6) with
 * the smaps_account() accounting rule reduced to the Rss/Pss/
 * Anonymous/Swap buckets the window truth carries.  PSS uses the
 * folio mapcount divisor (exact for the window's order-0 pages, the
 * same arithmetic smaps_account() applies); the sharing buckets
 * (Shared/Private x Clean/Dirty), Referenced and the hugetlb/KSM
 * families stay zero -- registered disclosure, not parity claims.
 */
static void corten_smap_pte(struct vm_area_struct *carrier, pte_t pte,
			    unsigned long addr, struct corten_smap_stats *out)
{
	struct page *page;
	struct folio *folio;
	u64 pss;
	int mapcount;

	if (pte_none_mostly(pte))
		return;
	if (!pte_present(pte)) {
		if (is_swap_pte(pte))
			out->swapped += PAGE_SIZE;
		return;
	}
	page = vm_normal_page(carrier, addr, pte);
	if (!page)
		return;		/* the shared zero page */
	folio = page_folio(page);
	out->resident += PAGE_SIZE;
	if (folio_test_anon(folio))
		out->anon += PAGE_SIZE;
	pss = (u64)PAGE_SIZE << CORTEN_PSS_SHIFT;
	mapcount = folio_mapcount(folio);
	if (mapcount >= 2)
		pss /= mapcount;
	out->pss += pss;
}

void corten_region_smap_stats(struct mm_struct *mm,
			      const struct corten_region_row *row,
			      struct corten_smap_stats *out)
{
	struct vm_area_struct *carrier = READ_ONCE(row->ar->carrier);
	unsigned long a;

	out->resident = 0;
	out->anon = 0;
	out->swapped = 0;
	out->pss = 0;

	for (a = row->start; a < row->end;) {
		unsigned long fe = min(a + PMD_SIZE, row->end);
		pmd_t *pmdp = corten_arena_pmd(mm, a);
		pte_t *ptep;
		spinlock_t *ptl;
		pmd_t pmd;

		if (!pmdp)
			goto hole;	/* no PT page: never faulted */
		pmd = READ_ONCE(*pmdp);
		if (unlikely(pmd_present(pmd) && pmd_leaf(pmd))) {
			/* C20 structurally excludes THP from the window
			 * (the defensive mincore shape): count resident.
			 */
			out->resident += fe - a;
			out->anon += fe - a;
			out->pss += (u64)(fe - a) << CORTEN_PSS_SHIFT;
			a = fe;
			continue;
		}
		if (!pmd_present(pmd))
			goto hole;

		ptep = pte_offset_map_lock(mm, pmdp, a, &ptl);
		if (!ptep)
			goto hole;
		for (; a != fe; a += PAGE_SIZE, ptep++)
			corten_smap_pte(carrier, ptep_get(ptep), a, out);
		pte_unmap_unlock(ptep - 1, ptl);
		cond_resched();
		continue;

hole:
		/* No resident truth in this frame slice. */
		a = fe;
	}
}

/* One locked PT page's slice of the pagemap fill: classify and emit
 * [a, fe), advancing *@ap.  Returns the first non-zero emit() verdict.
 */
static int corten_pagemap_fill_pt(struct vm_area_struct *carrier,
				  pte_t *ptep, unsigned long *ap,
				  unsigned long fe, bool show_pfn,
				  int (*emit)(void *ctx, u64 pme), void *ctx)
{
	unsigned long a = *ap;

	for (; a != fe; a += PAGE_SIZE, ptep++) {
		u64 frame, flags;
		int err;

		corten_pagemap_pte(carrier, ptep_get(ptep), a, show_pfn,
				   &frame, &flags);
		err = emit(ctx,
			   (frame & CORTEN_PM_PFRAME_MASK) | flags);
		if (err) {
			*ap = a;
			return err;
		}
	}
	*ap = a;

	return 0;
}

/*
 * pagemap truth of one window-domain span (j2-audit #11): the generic
 * walk sees a VMA hole there and emits zero entries; this fill
 * derives CORTEN_PM_PRESENT/CORTEN_PM_SWAP (and the pfn under
 * CAP_SYS_ADMIN) from the real PTEs, the mincore skeleton under ptl.
 * The bit layout is byte-compatible with the legacy entries around it
 * (static_assert'ed in fs/proc/task_mmu.c).
 */
int corten_pagemap_fill(struct mm_struct *mm, unsigned long addr,
			unsigned long end, bool show_pfn,
			int (*emit)(void *ctx, u64 pme), void *ctx)
{
	struct vm_area_struct *carrier = NULL;
	unsigned long a;
	int err;

	/* The carrier of the first covering region provides the
	 * vm_normal_page() vma context (no VM_PFNMAP/VM_MIXEDMAP shapes
	 * exist in the window, but the call wants a vma).
	 */
	{
		struct corten_region_row row;

		if (corten_row_query(mm, addr, &row))
			carrier = READ_ONCE(row.ar->carrier);
	}

	/* Walk one PT page's slice [a, fe): classified entries, or the
	 * zero entries of the no-PT-page/defensive-leaf shapes.  A
	 * non-zero emit() verdict aborts the whole fill.
	 */
	for (a = addr; a < end;) {
		unsigned long fe = min(a + PMD_SIZE, end);
		pmd_t *pmdp = corten_arena_pmd(mm, a);
		pte_t *ptep;
		spinlock_t *ptl;
		pmd_t pmd;

		ptep = NULL;
		if (pmdp) {
			pmd = READ_ONCE(*pmdp);
			if (pmd_present(pmd) && !pmd_leaf(pmd))
				ptep = pte_offset_map_lock(mm, pmdp, a, &ptl);
		}
		if (ptep) {
			pte_t *locked = ptep;

			err = corten_pagemap_fill_pt(carrier, ptep, &a, fe,
						     show_pfn, emit, ctx);
			pte_unmap_unlock(locked, ptl);
			cond_resched();
		} else {
			/* No PT page (never faulted) or a defensive leaf:
			 * the window truth there is non-present zero
			 * entries.
			 */
			for (; a != fe; a += PAGE_SIZE) {
				err = emit(ctx, 0);
				if (err)
					return err;
			}
		}
		if (err)
			return err;
	}
	return 0;
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
	/* The type bits must be exactly MAP_PRIVATE: this rejects
	 * MAP_SHARED/MAP_SHARED_VALIDATE for both arms (VM_SHARED never
	 * enters the window, MV_VMA_FREE_SPEC.md sec 3.2.1) and absorbs
	 * the MAP_DROPPABLE alias bit (0x08 lives inside the MAP_TYPE
	 * nibble).
	 */
	if ((flags & MAP_TYPE) != MAP_PRIVATE)
		return CORTEN_MMAP_LEGACY;

	if (file) {
		/* V-B.1: the file mirror of the anon whitelist -- at most
		 * MAP_NORESERVE may accompany the type bits.  MAP_ANONYMOUS
		 * is absent by construction (a file request never carries
		 * it) and also rejects defensively.  Every other bit keeps
		 * the mapping legacy; MAP_FIXED file stays with the D-G''
		 * punch route (the OQ-MV-2 implant exception -- a region
		 * needs full i_mmap write-side semantics to host a forced
		 * file mapping, deliberately out of scope).
		 */
		if (flags & ~(MAP_TYPE | MAP_NORESERVE))
			return CORTEN_MMAP_LEGACY;
		return CORTEN_MMAP_AUTO_FILE;
	}

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

/*
 * The MAY bound of a private file mapping (V-B.1, the one-bit answer of
 * sec 1.3's may calculus): legacy do_mmap() grants
 * VM_MAYREAD|MAYWRITE|MAYEXEC for private mappings regardless of the
 * requested prot (mm/mmap.c stamps the three MAY bits globally), and
 * the open mode can only take EXEC away -- a noexec mount (the
 * path_noexec() arm below drops VM_MAYEXEC in legacy).  Write is kept:
 * a MAP_PRIVATE file write goes COW, which the MAY bound must allow.
 */
static u8 corten_file_may_bound(struct file *file)
{
	u8 may = corten_region_may_full();

	if (path_noexec(&file->f_path))
		may &= ~CORTEN_PERM_EXEC;

	return may;
}

/*
 * The do_mmap() file-validation chain replayed for the MODE window
 * route (V-B.1; mm/mmap.c's file arm, same order, same errnos).  The
 * classify whitelist already rejected every flag-side shape
 * (MAP_SHARED, GROWSDOWN/GROWSUP carriers, DENYWRITE/EXECUTABLE
 * mappings), so what remains is the file itself:
 *
 *   - file_mmap_ok()'s size check: the helper is static in mm/mmap.c,
 *     so its two conditions are re-derived here verbatim (the
 *     driver-size table it reads: MAX_LFS_FILESIZE for REG/BLK/SOCK,
 *     unlimited for "unsigned offset" drivers, 0 -- "unbounded" -- for
 *     the FOP_UNSIGNED_OFFSET shapes);
 *   - the DAX gate: no DAX inode enters the window (the FOLL_LONGTERM
 *     family of refusals applies to a carrier no less than to a VMA;
 *     file_is_dax() covers fsdax and dev-dax alike, both REJECT rows
 *     of sec 2.6);
 *   - V-B.4: the hugetlbfs gate -- an explicitly opened hugetlbfs fd
 *     (no MAP_HUGETLB bit for the classify whitelist to reject) never
 *     enters the window either; legacy's hugetlbfs_mmap() serves it;
 *   - FMODE_READ / noexec x EXEC / can_mmap_file: literally the
 *     mm/mmap.c arms;
 *   - memfd seals: a private mapping always passes check_write_seal()
 *     itself (writability is irrelevant without VM_SHARED); the call
 *     is kept for chain fidelity -- an exotic future seal that bites
 *     private mappings answers here, not silently later.
 *
 * Pure; mm/corten_fault_test.c drives it without an mm.
 * Return: 0 (and *@may_prot = the MAY bound) or -errno; the caller
 * degrades to legacy, which answers the syscall with this same errno
 * through its own chain.
 */
int corten_file_may(struct file *file, unsigned long prot,
		    unsigned long pgoff, unsigned long len, u8 *may_prot)
{
	struct inode *inode = file_inode(file);
	vm_flags_t vm_flags = VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;
	u64 maxsize;
	int err;

	/* file_mmap_size_max() verbatim (mm/mmap.c). */
	if (S_ISREG(inode->i_mode) || S_ISBLK(inode->i_mode) ||
	    S_ISSOCK(inode->i_mode))
		maxsize = MAX_LFS_FILESIZE;
	else if (file->f_op->fop_flags & FOP_UNSIGNED_OFFSET)
		maxsize = 0;
	else
		maxsize = ULONG_MAX;

	/* file_mmap_ok() verbatim (mm/mmap.c): len must fit and the
	 * mapping end must not wrap the max size.
	 */
	if (maxsize && len > maxsize)
		return -EOVERFLOW;
	maxsize -= len;
	if (pgoff > maxsize >> PAGE_SHIFT)
		return -EOVERFLOW;

	if (file_is_dax(file))
		return -EOPNOTSUPP;

	/* V-B.4: a hugetlbfs fd opened explicitly (mmap(NULL, ..., fd)
	 * carries no MAP_HUGETLB bit, so the classify whitelist never
	 * sees it -- the B.3 disclosure item 6).  Legacy serves it
	 * through hugetlbfs_mmap() on VM_HUGETLB carrier semantics the
	 * window machinery does not model (no PTE-level faults, hstate
	 * rounding in ksys_mmap_pgoff); the fetch arm would -EIO->BUS
	 * instead of serving the huge page.  is_file_hugepages() folds
	 * to false without CONFIG_HUGETLBFS, so the gate compiles away.
	 */
	if (is_file_hugepages(file))
		return -EOPNOTSUPP;

	if (!(file->f_mode & FMODE_READ))
		return -EACCES;

	if (path_noexec(&file->f_path)) {
		if (prot & PROT_EXEC)
			return -EPERM;
	}

	if (!can_mmap_file(file))
		return -ENODEV;

	err = memfd_check_seals_mmap(file, &vm_flags);
	if (err)
		return err;

	if (may_prot)
		*may_prot = corten_file_may_bound(file);

	return 0;
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

/* ------------------------------------------------------------------ *
 * M4.T1: per-cpu 2M-frame VA magazine (PS-E1, DESIGN.md sec 1 row E1)
 *
 * The T0 single cursor handed every allocation a PMD-aligned range by
 * bumping one per-mm word under mmap_write, and every allocation paid a
 * VMA-tree obstacle scan (find_vma_intersection) plus a registry probe.
 * The magazine instead claims one exclusive CORTEN_VA_SEG_SIZE segment
 * per cpu -- obstacle-scanned ONCE per CORTEN_VA_SEG_FRAMES allocations,
 * marked frame-by-frame with reserve sentinels in the same registry
 * xarray -- and then serves its allocations by a private bump pointer
 * whose only per-op cost is one marker xa_load (replacing the old maple
 * walk).  Released auto-arena frames return to a per-mm recycle list
 * (markers restored instead of erased, so the xarray subtree stays warm
 * and no node churns) and are served before fresh window.  Everything
 * runs under this mm's mmap_lock for writing: the percpu layout is about
 * address locality and xarray-subtree disjointness, not concurrency.
 * ------------------------------------------------------------------
 */

/* True when @addr's frame lies inside one of this mm's claimed magazine
 * segments (and therefore may carry a restorable reserve marker).
 */
static bool corten_va_in_seg(struct corten_mm_state *state, unsigned long addr)
{
	struct corten_va_segrec *seg;

	list_for_each_entry(seg, &state->seg_list, list) {
		if (addr >= seg->base && addr < seg->end)
			return true;
	}

	return false;
}

/*
 * Claim one fresh segment for the running cpu: scan
 * [state->next_va, CORTEN_MODE_WINDOW_END) for a CORTEN_VA_SEG_FRAMES
 * span with no VMA (explicit legacy mappings in the window are jumped
 * past) and no arena entry (markers included -- another cpu's live
 * segment, jumped frame-wise), store reserve markers over the whole span,
 * record it, and advance the global cursor past it.  The T0 obstacle
 * semantics live here now: nothing inside a claimed segment can be a
 * foreign mapping at claim time, and the per-allocation marker check
 * keeps that true for the segment's whole life (a punch that carves a
 * hole erases markers; those frames are skipped at allocation).
 *
 * Called under this mm's mmap_write; sleeps (xa_store GFP_KERNEL).
 * Return: 0 with *@seg filled, -ENOSPC when the window cannot hold a
 * further segment (the caller degrades, counted).
 */
static int corten_va_seg_claim(struct mm_struct *mm,
			       struct corten_mm_state *state,
			       struct corten_va_seg *seg)
{
	unsigned long start = state->next_va, seg_size;

	if (start < CORTEN_MODE_WINDOW_START)
		start = CORTEN_MODE_WINDOW_START;
	start = round_up(start, PMD_SIZE);
	seg_size = CORTEN_VA_SEG_FRAMES * PMD_SIZE;

	for (;;) {
		unsigned long end = start + seg_size;
		unsigned long frame, last;
		struct corten_va_segrec *rec;
		int ret;

		if (end > CORTEN_MODE_WINDOW_END || end <= start)
			return -ENOSPC;		/* window exhausted */

		/* VMA obstacle: jump past it (round_up can overflow past
		 * the window end, caught by the bounds check above).
		 */
		{
			struct vm_area_struct *v =
				corten_vma_find(mm, start, end);

			if (v) {
				start = round_up(v->vm_end, PMD_SIZE);
				if (start < v->vm_end)
					return -ENOSPC;
				continue;
			}
		}

		/* Arena obstacles: jump past the arena extent; a marker of
		 * a foreign (disjoint by cursor) segment is jumped one
		 * frame -- it should not exist here, but a stale one must
		 * not be handed out either.
		 */
		last = start >> PMD_SHIFT;
		for (frame = last; frame < last + CORTEN_VA_SEG_FRAMES;
		     frame++) {
			struct corten_arena *ar = xa_load(&state->arenas,
							 frame);

			if (!ar)
				continue;
			if (ar == &corten_va_reserve_sentinel) {
				start = (frame + 1) * PMD_SIZE;
			} else {
				start = round_up(ar->end, PMD_SIZE);
				if (start < ar->end)
					return -ENOSPC;
			}
			break;
		}
		if (frame < last + CORTEN_VA_SEG_FRAMES)
			continue;	/* jumped: rescan */

		/* Mark the span, then commit it.  Unwind on allocation
		 * failure so no half-marked segment is left behind.
		 */
		last = start >> PMD_SHIFT;
		for (frame = last; frame < last + CORTEN_VA_SEG_FRAMES;
		     frame++) {
			ret = xa_err(xa_store(&state->arenas, frame,
					      &corten_va_reserve_sentinel,
					      GFP_KERNEL));
			if (ret) {
				while (frame > last)
					xa_erase(&state->arenas, --frame);
				return ret;
			}
		}

		rec = kmalloc(sizeof(*rec), GFP_KERNEL);
		if (!rec) {
			for (frame = last;
			     frame < last + CORTEN_VA_SEG_FRAMES; frame++)
				xa_erase(&state->arenas, frame);
			return -ENOMEM;
		}
		rec->base = start;
		rec->end = start + seg_size;
		list_add_tail(&rec->list, &state->seg_list);

		state->next_va = rec->end;
		seg->base = start;
		seg->end = rec->end;
		seg->next = start;
		atomic_long_inc(&corten_nr_seg_claims);

		return 0;
	}
}

/*
 * Carve @len2 (PMD-rounded) frames from the magazine: recycle list first,
 * then the @cpu segment's private bump, then a fresh segment claim.
 * Reserve markers are verified on every handed-out frame (they are what
 * proves the frame carries no foreign mapping; frames whose marker a
 * punch erased are skipped, counted, and effectively leaked).  Oversized
 * requests go straight to the T0 global path.
 *
 * Called under this mm's mmap_write.  @cpu < 0 selects the running cpu;
 * the KUnit hook passes an explicit id (the percpu state is plain memory
 * owned by @state, and every writer is serialized by the write lock).
 * Return: 0 with *@addr2, -ENOSPC when the magazine cannot serve it
 * (caller falls back to the global path, which keeps the T0 semantics).
 */
static int corten_va_mag_alloc_cpu(struct mm_struct *mm,
				   struct corten_mm_state *state,
				   unsigned long len2, unsigned long *addr2,
				   int cpu)
{
	unsigned long frames = len2 >> PMD_SHIFT;
	struct corten_va_seg want, *seg;

	BUILD_BUG_ON_NOT_POWER_OF_2(CORTEN_VA_SEG_FRAMES);

	if (frames > CORTEN_VA_SEG_FRAMES)
		return -ENOSPC;

	/* 1. Recycle (M4.T1 real VA recycle).  A block is consumed only
	 * whole: a single lost marker anywhere in it (punch legacy
	 * mapping) drops it -- conservative and rare, and it keeps the
	 * steady-state churn (release-restore + re-carve of one frame)
	 * free of any tree walk beyond the marker xa_loads.
	 */
	while (!list_empty(&state->va_free)) {
		struct corten_va_freeblk *blk =
			list_first_entry(&state->va_free,
					 struct corten_va_freeblk, list);
		unsigned long f, last;
		bool ok = true;

		if (blk->frames < frames)
			goto drop;
		last = (blk->base >> PMD_SHIFT) + frames;
		for (f = blk->base >> PMD_SHIFT; f < last; f++) {
			if (xa_load(&state->arenas, f) !=
			    &corten_va_reserve_sentinel) {
				ok = false;
				break;
			}
		}
		if (!ok)
			goto drop;

		/* T0 obstacle semantics on the recycle path too: an
		 * explicit-address legacy mapping can claim a markered
		 * frame's VA in the window between its RELEASE and this
		 * serve (the markers were restored at release).  The bump
		 * path skips such frames frame-wise; here a block with a
		 * foreign VMA over the served range is dropped whole --
		 * conservative, and rare (the window is app-opaque).
		 */
		if (corten_vma_find(mm, blk->base, blk->base + len2))
			goto drop;

		*addr2 = blk->base;
		blk->base += len2;
		blk->frames -= frames;
		state->va_nrfree -= frames;
		if (!blk->frames) {
			list_del(&blk->list);
			kfree(blk);
		}
		atomic_long_add(frames, &corten_nr_va_recycles);
		return 0;
drop:
		list_del(&blk->list);
		state->va_nrfree -= blk->frames;
		kfree(blk);
	}

	/* 2. Segment bump.  Every handed-out span re-verifies the T0
	 * obstacle contract (this is what keeps the magazine safe against
	 * an explicit-address mapping appearing inside a claimed segment
	 * after claim time): reserve markers present -- a punch erased
	 * them means the frames went legacy -- and no VMA covering any of
	 * it.  The claim-side scans only reduce how often the skip fires.
	 * Neither check sleeps, so the cpu-local bump state is read under
	 * a preemption-off section; all writers hold this mm's mmap_write,
	 * so preemption can only migrate the writer, never parallelize.
	 */
	for (;;) {
		unsigned long a2, skip_to = 0;

		if (cpu < 0)
			seg = get_cpu_ptr(state->va_segs);
		else
			seg = per_cpu_ptr(state->va_segs, cpu);

		while (seg->next >= seg->base && seg->next < seg->end &&
		       seg->next + len2 <= seg->end) {
			unsigned long f, last;
			struct vm_area_struct *v;
			bool ok = true;

			a2 = seg->next;
			last = (a2 >> PMD_SHIFT) + frames;
			for (f = a2 >> PMD_SHIFT; f < last; f++) {
				if (xa_load(&state->arenas, f) !=
				    &corten_va_reserve_sentinel) {
					ok = false;
					skip_to = (f + 1) << PMD_SHIFT;
					break;
				}
			}
			if (!ok)
				goto skip;

			/* T0 obstacle semantics: an explicit-address
			 * mapping inside the claimed segment wins -- the
			 * range is skipped past it, never overwritten.
			 */
			v = corten_vma_find(mm, a2, a2 + len2);
			if (v) {
				skip_to = round_up(v->vm_end, PMD_SIZE);
				if (skip_to < v->vm_end)
					break;	/* overflow: segment done */
				goto skip;
			}

			seg->next = a2 + len2;
			if (cpu < 0)
				put_cpu_ptr(state->va_segs);
			*addr2 = a2;
			return 0;
skip:
			atomic_long_inc(&corten_nr_mag_skips);
			seg->next = skip_to;
		}
		want = *seg;
		if (cpu < 0)
			put_cpu_ptr(state->va_segs);

		if (corten_va_seg_claim(mm, state, &want))
			return -ENOSPC;

		/* Publish the fresh segment; if we migrated across the
		 * claim, the receiving cpu merely gains a segment and the
		 * origin keeps its own -- both stay disjoint, which is all
		 * the magazine promises.  An explicit @cpu (KUnit) writes
		 * that cpu directly and retries the bump there.
		 */
		preempt_disable();
		if (cpu < 0)
			*this_cpu_ptr(state->va_segs) = want;
		else
			*per_cpu_ptr(state->va_segs, cpu) = want;
		preempt_enable();
	}
}

static int corten_va_mag_alloc(struct mm_struct *mm,
			       struct corten_mm_state *state,
			       unsigned long len2, unsigned long *addr2)
{
	return corten_va_mag_alloc_cpu(mm, state, len2, addr2, -1);
}

/* Bound on the recycle list: beyond it releases stop restoring blocks
 * (markers are still restored, so correctness is unaffected; the frames
 * fall back to the T0 leak behaviour), keeping the block kmallocs bounded.
 */
#define CORTEN_VA_FREE_MAX_FRAMES	BIT(20)		/* 2 GiB of frames */

/*
 * Return a released auto-arena's frame range to the magazine: restore
 * reserve markers for frames inside claimed segments (instead of leaving
 * the slots empty, which would churn xarray nodes every cycle) and push
 * the range as one recycle block.  Frames outside claimed segments (the
 * T0 global path / targeted arenas) keep the plain erase.  Called under
 * this mm's mmap_write, in place of the release loop's xa_erase.
 */
static void corten_va_release_frame(struct corten_mm_state *state,
				    unsigned long addr)
{
	struct corten_va_freeblk *blk;

	if (corten_va_in_seg(state, addr)) {
		xa_store(&state->arenas, addr >> PMD_SHIFT,
			 &corten_va_reserve_sentinel, GFP_KERNEL);

		/* Extend the tail block when contiguous (the common
		 * multi-frame release), else push a new one.
		 */
		blk = list_empty(&state->va_free) ? NULL :
			list_last_entry(&state->va_free,
					struct corten_va_freeblk, list);
		if (blk && blk->base + blk->frames * PMD_SIZE == addr) {
			blk->frames++;
			state->va_nrfree++;
			return;
		}
		if (state->va_nrfree + 1 <= CORTEN_VA_FREE_MAX_FRAMES) {
			blk = kmalloc(sizeof(*blk), GFP_KERNEL);
			if (blk) {
				blk->base = addr;
				blk->frames = 1;
				state->va_nrfree++;
				list_add_tail(&blk->list, &state->va_free);
				return;
			}
		}
		return;	/* marker restored; frame leaks (T0 behaviour) */
	}

	xa_erase(&state->arenas, addr >> PMD_SHIFT);
}

/*
 * T0 global placement (unchanged semantics): hand out the next
 * PMD-rounded [addr2, addr2+len2) from the global cursor, skipping past
 * any obstacle (a MODE-targeted DECLARE inside the window, or a plain
 * legacy VMA that ended up here -- explicit-address mappings stay legacy
 * by contract) so the caller's MAP_FIXED install can never destroy an
 * existing mapping.  Now the fallback path for oversized magazine
 * requests; the cursor is shared with segment claims (both under this
 * mm's mmap_write).
 *
 * Return: 0 with *@addr2 set, -ENOSPC when the window cannot hold the
 * request (the caller degrades, counted).
 */
static int corten_arena_window_place_global(struct mm_struct *mm,
					    struct corten_mm_state *state,
					    unsigned long len2,
					    unsigned long *addr2)
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
			vma = corten_vma_find(mm, a2, a2 + len2);
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

/*
 * Placement dispatcher (M4.T1): the per-cpu magazine serves everything up
 * to one segment; the T0 global cursor path remains for oversized spans
 * (and, transiently, for a segment-exhausted magazine -- the fallback the
 * spec's "耗尽回退全局" names).  Both paths keep the T0 contract: the
 * handed-out range can never carry a foreign mapping.
 */
static int corten_arena_window_place(struct mm_struct *mm,
				     struct corten_mm_state *state,
				     unsigned long len2, unsigned long *addr2)
{
	if (!corten_va_mag_alloc(mm, state, len2, addr2))
		return 0;

	return corten_arena_window_place_global(mm, state, len2, addr2);
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

/*
 * V-A.2a: the mmap_region() verification checklist the auto takeover
 * used to inherit indirectly (the takeover flowed through
 * mmap_region()), now run up front so the takeover can complete
 * without mmap_region() at all.  All VMA-free:
 *
 *   1. security_mmap_file()/fsnotify_mmap_perm(): NOT re-called here.
 *      Every mmap() syscall enters do_mmap() through vm_mmap_pgoff()
 *      (mm/util.c), which already runs both hooks for the anonymous
 *      shape (file == NULL) before the route sees the request -- the
 *      SPEC's "anon 路径今天也没调" premise does not hold for 6.18
 *      and a second call would double-invoke the LSM hooks.  (MV-4
 *      correction, disclosed in the slice report.)
 *   2. RLIMIT_AS/RLIMIT_DATA: may_expand_vm() -- the identical gate
 *      mmap_region() runs; a refusal degrades to the legacy flow,
 *      which fails with the same errno (the charge has not happened).
 *   3. mlock_future_ok()/def_flags VM_LOCKED: a reused arena cannot
 *      honour mlockall() -- the pool_take refuse, now also for the
 *      fresh window (the legacy flow answers -EAGAIN/-EPERM exactly
 *      as it always did).  MAP_LOCKED itself never reaches here (the
 *      whitelist rejects it).
 *   4. MAP_FIXED_NOREPLACE/explicit hint: unreachable (the whitelist
 *      requires addr == 0; NOREPLACE is classified legacy).
 *   5. pkey/hugetlb/shadow-stack: the only encoding gap is the
 *      execute-only pkey (prot == PROT_EXEC -- the shape do_mmap()
 *      stamps VM_PKEY_* bits for); the arena record has no pkey
 *      encoding (sec 2.6 REJECT row) so the request degrades and the
 *      legacy VMA keeps it, exactly the pre-A.2a attach-fail verdict.
 *   6. OVERCOMMIT_NEVER: handled by the route above this call.
 *
 * Return: 0 = proceed, < 0 = refuse this shape (the caller degrades
 * to the legacy flow; the errno is diagnostic, the verdict comes from
 * the legacy path).
 */
int corten_auto_validate(struct mm_struct *mm, unsigned long len,
			 unsigned long prot, unsigned long flags)
{
	vm_flags_t take =
		corten_take_vm_flags(corten_arena_perm_from_prot(prot));

	/* Unused today: MAP_LOCKED/MAP_POPULATE never reach the route
	 * (whitelist); kept in the signature so the KUnit truth table
	 * pins the full mmap-flags input.
	 */
	(void)flags;

	if (mm->def_flags & VM_LOCKED)
		return -EAGAIN;

	if (prot == PROT_EXEC)
		return -EOPNOTSUPP;

	if (!may_expand_vm(mm, take, len >> PAGE_SHIFT))
		return -ENOMEM;

	return 0;
}

int corten_arena_auto_mmap_route(struct mm_struct *mm, struct file *file,
				 unsigned long pgoff, unsigned long len,
				 unsigned long prot, unsigned long *addr,
				 unsigned long *lenp, unsigned long *flagsp)
{
	struct corten_mm_state *state;
	enum corten_mmap_class class;
	unsigned long addr2, len2;
	int ret;

	if (!corten_enabled_static() || !mm || !READ_ONCE(mm->corten_mode))
		return 0;
	/* PROT_NONE and every prot combination are arena-able
	 * (corten_arena_perm_from_prot() mirrors the prot bits); the
	 * validation gate below decides the shapes the arena cannot
	 * encode.
	 */

	/* OVERCOMMIT_NEVER does not honour MAP_NORESERVE (mm/mmap.c:563),
	 * so the created VMA would carry VM_ACCOUNT and fail the DECLARE
	 * validation chain wholesale -- degrade the auto takeover of this
	 * configuration to legacy up front (DEV-12).  V-B.1 keeps the
	 * conservative posture for the file arm too (a private file
	 * mapping carries no VM_ACCOUNT demand, but the first version
	 * stays uniform; registered for a later micro-tuning).
	 */
	if (sysctl_overcommit_memory == OVERCOMMIT_NEVER) {
		corten_arena_auto_fallback(NULL);
		return 0;
	}

	class = corten_arena_auto_mmap_classify(*flagsp, !!file);
	if (class == CORTEN_MMAP_LEGACY)
		return 0;

	/* V-A.2a: the mmap_region() verification checklist (see
	 * corten_auto_validate()).  A refusal degrades to the legacy
	 * flow -- which delivers the identical verdict through its own
	 * full chain, and no window segment is handed out for a shape
	 * that cannot be honoured.  V-B.1: the file arm adds
	 * corten_file_may() -- the do_mmap() file validation chain the
	 * legacy flow would run later (the EACCES/EPERM/ENODEV/
	 * EOVERFLOW semantics must not be lost to the takeover).
	 */
	len2 = round_up(*lenp, PMD_SIZE);
	ret = corten_auto_validate(mm, len2, prot, *flagsp);
	if (ret) {
		atomic_long_inc(&corten_nr_auto_vgate);
		corten_arena_auto_fallback(NULL);
		return 0;
	}
	/* V-B.3: the dark gate is gone -- a classified FILE shape runs
	 * the takeover for real now (the read/COW/EOF arms live in
	 * corten_arena_fault_once()).  dlopen-shaped addr==0 private
	 * file maps are served from the window; only a corten_file_may()
	 * refusal degrades to the legacy flow below (which answers the
	 * same errno through its own chain).
	 */
	if (class == CORTEN_MMAP_AUTO_FILE &&
	    corten_file_may(file, prot, pgoff, len2, NULL)) {
		corten_arena_auto_fallback(NULL);
		return 0;
	}

	state = corten_arena_get_state(mm);
	if (!state) {
		/* A5: the registry is created lazily at the first
		 * arena-able mmap now; if that allocation fails the
		 * request degrades to the legacy mmap path, counted --
		 * ENTER no longer front-loads it (and no longer fails
		 * with -ENOMEM either).
		 */
		corten_arena_auto_fallback(NULL);
		return 0;
	}

	/* The application's page-rounded length is rounded up to the 2M
	 * arena granularity; the tail is kernel-private padding, invisible
	 * to the application and covered by the munmap release rule
	 * (corten_arena_release_classify()).  V-A.2a: the same rounded
	 * length fed the validation gate above.
	 */
	if (!len2) {
		/* Nothing arena-able left after rounding (len was 0 or the
		 * window-relayed request degenerated): let the normal path
		 * produce its -ENOMEM/-EINVAL.
		 */
		corten_arena_auto_fallback(state);
		return 0;
	}

	/* T1c: serve the request from the resident pool -- the most
	 * recently parked arena of exactly this PMD-rounded size is
	 * re-warmed in place (R/W/X re-encoded to @prot) and its address
	 * returned with ret 2: do_mmap() completes immediately, WITHOUT
	 * the MAP_FIXED flow (whose VMA replacement would free the
	 * window's tracked page tables only for the first touch faults to
	 * rebuild them).  The auto churn re-asks for the size it just
	 * freed, so the pool covers the whole working set; a request no
	 * slot matches keeps the pre-pool placement behaviour.
	 *
	 * V-B.1: the FILE arm skips the pool -- a parked window's
	 * reactivation is the ANON reuse contract, a file mapping always
	 * takes a fresh window.
	 */
	if (class == CORTEN_MMAP_AUTO) {
		ret = corten_arena_pool_take(mm, state, len2, prot, &addr2);
		if (!ret) {
			*addr = addr2;
			*lenp = len2;
			return 2;
		}
		atomic_long_inc(&corten_nr_pool_misses);
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
	 * window.  V-A.2a: the caller completes the mmap WITHOUT
	 * mmap_region() -- corten_arena_auto_attach() declares the arena
	 * (V-A.2b carrier + frames + metadata) and returns; V-B.1's file
	 * arm completes through corten_arena_file_attach() instead; only
	 * a declare failure degrades to the legacy MAP_FIXED flow
	 * (the counted attach-failure degradation).
	 */
	*addr = addr2;
	*lenp = len2;
	*flagsp |= MAP_FIXED | MAP_NORESERVE;

	return 1;
}

/*
 * V-A.2a: the auto takeover completes here -- no mmap_region(), no VMA,
 * no MAP_FIXED flow.  DECLARE's locked body (carrier + frames +
 * metadata, V-A.2b) runs under the write lock the caller (do_mmap)
 * already holds -- exactly the nesting the P0 inversion (DEV-13)
 * enables.  The validation checklist ran in the route before
 * placement; the total_vm charge rides the declare's success path.
 *
 * Failure (memory pressure) degrades to the caller's legacy flow: the
 * mapping becomes a plain anonymous VMA at a window address and every
 * lookup on it resolves no arena (harmless, counted -- the established
 * attach-failure contract).
 */
int corten_arena_auto_attach(struct mm_struct *mm, unsigned long addr,
			     unsigned long len, unsigned long prot)
{
	struct corten_mm_state *state;
	int ret;

	mmap_assert_write_locked(mm);

	/* A5: the route has normally created the registry for this MODE
	 * mm already (it placed the window address); get_state() covers
	 * the direct-attach stragglers so -ENOENT is not a thing here.
	 * Pairs with the store in corten_arena_state_create().
	 */
	state = corten_arena_get_state(mm);
	if (!state)
		return -ENOMEM;

	ret = corten_arena_declare_locked(mm, state, addr, len,
					  corten_arena_perm_from_prot(prot),
					  NULL, 0, true);
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

/*
 * V-B.1: the FILE counterpart of corten_arena_auto_attach() -- the
 * do_mmap() completion body of a CORTEN_MMAP_AUTO_FILE takeover (the
 * dlopen/JVM-libs shape).  DECLARE's locked body runs with the FILE
 * arm: the region record takes the file reference, the carrier is born
 * in FILE shape (vm_file/@pgoff, anon_vma kept for the private COW
 * pages), the region is virtually allocated FILE_MAPPED (faults answer
 * STUB -> SEGV_MAPERR until B.3), and the region joins the mapping's
 * per-inode registry (W1.b).  The total_vm charge rides the declare's
 * success path, the
 * may_expand_vm/RLIMIT gate ran in the route's validate -- exactly the
 * A.2a accounting posture.
 *
 * Same failure contract as the anon attach (memory pressure degrades
 * to the caller's legacy flow, counted); the route's
 * corten_file_may() already validated the file shape.
 */
int corten_arena_file_attach(struct mm_struct *mm, unsigned long addr,
			     unsigned long len, unsigned long prot,
			     struct file *file, unsigned long pgoff)
{
	struct corten_mm_state *state;
	int ret;

	mmap_assert_write_locked(mm);

	state = corten_arena_get_state(mm);
	if (!state)
		return -ENOMEM;

	ret = corten_arena_declare_locked(mm, state, addr, len,
					  corten_arena_perm_from_prot(prot),
					  file, pgoff, true);
	if (ret) {
		corten_arena_auto_fallback(state);
		atomic_long_inc(&corten_nr_auto_attach_fails);
	} else {
		atomic_long_inc(&corten_nr_auto_mmaps);
		atomic_long_inc(&corten_nr_file_mmaps);
	}

	return ret;
}

int corten_arena_mode_enter(struct mm_struct *mm)
{
	if (!mm)
		return -EINVAL;

	mmap_write_lock(mm);
	if (READ_ONCE(mm->corten_mode)) {
		mmap_write_unlock(mm);
		return 0;		/* idempotent */
	}

	/* A5 (G5-fix): no registry here.  The mode bit lives in the
	 * mm_struct (T0a), so ENTER is allocation-free and the registry
	 * is created on demand by the first arena work -- the auto mmap
	 * route, a DECLARE or the fork mirror.  A MODE process that
	 * never arena-maps (the fork/shell churn lat_proc measures) also
	 * never allocates one, and its exit costs nothing (see
	 * corten_arena_mm_exit()).  The hot path keeps its
	 * allocation-free property: get_state() has run by the time the
	 * route places an arena.
	 */
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
	 * can slip in behind our back and re-enter MODE territory.  The
	 * scan is monotonic: xa_find() restarts from the current @frame,
	 * sentinel markers are skipped frame-wise, and a released arena's
	 * frames are either erased or re-stored as sentinels (M4.T1
	 * magazine recycle) -- all of which the scan then walks past, so
	 * the loop drains.  The old rescan-from-0 pattern ("each release
	 * steals one arena, so the loop terminates") would livelock now:
	 * restored markers keep the registry non-empty forever.  Nothing
	 * can contend ctl_lock meanwhile (every other user of it needs
	 * this write lock first).
	 */
	mmap_write_lock(mm);

	if (state) {
		unsigned long frame = 0;

		for (;;) {
			struct corten_arena *ar;
			int r;

			ar = xa_find(&state->arenas, &frame, ULONG_MAX,
				     XA_PRESENT);
			if (!ar)
				break;
			/* M4.T1: reserve markers are not arenas; release
			 * only consumes live arenas, so skip forward.
			 */
			if (ar == &corten_va_reserve_sentinel) {
				frame++;
				continue;
			}

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
			/* Rescan at the released arena's first frame: its
			 * magazine frames (if any) are back to sentinels
			 * and are skipped above; entries below it cannot
			 * reappear (no new registration under this lock).
			 */
			frame = ar->start >> PMD_SHIFT;
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
		/* M4.T1: reserve markers are not arenas. */
		if (arena == &corten_va_reserve_sentinel)
			continue;

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

	/* A5 (G5-fix): the child registry is only created when live
	 * arenas are about to be mirrored into it (fork_commit's
	 * register_child, plus the cursor that must not regress behind
	 * them).  An arena-less MODE child -- the fork/shell churn
	 * lat_proc measures -- inherits just the mode bit and runs
	 * registry-free like its parent; its first arena work builds the
	 * registry then.  Parked arenas do not count: the flush below
	 * releases them, so nothing of them reaches the child either.
	 *
	 * V-A.3c: punch implants DO count.  dup_mmap() copies the parent's
	 * implant VMAs into the child as ordinary legacy VMAs -- without
	 * the registry copy the child's fault terminus
	 * (corten_fault_window_maperr) would MAPERR them and the INV-MV2
	 * walker (whose fork_commit trigger audits the child) would count
	 * every inherited implant as a violation.  The copy runs under
	 * oldmm's mmap_write (held by dup_mmap), so the parent array is
	 * stable; the child registry is single-threaded this early.
	 */
	if (refcount_read(&old_state->nr) ||
	    READ_ONCE(old_state->nr_implants)) {
		struct corten_mm_state *state = corten_arena_get_state(mm);

		if (!state)
			return -ENOMEM;
		state->next_va = READ_ONCE(old_state->next_va);
		if (READ_ONCE(old_state->nr_implants)) {
			state->implants = kmemdup(old_state->implants,
						  old_state->nr_implants *
						  sizeof(*state->implants),
						  GFP_KERNEL_ACCOUNT);
			if (!state->implants)
				return -ENOMEM;
			state->nr_implants = old_state->nr_implants;
			state->nr_implants_alloc = old_state->nr_implants;
		}
	}

	/* DEV-13: dup_mmap holds oldmm's mmap_write; nesting ctl_lock
	 * inside it is the legal order.
	 */
	mutex_lock(&old_state->ctl_lock);

	/* T1c: flush the pool before the freeze window.  A parked arena is
	 * a munmapped-range reservation: its VMA shows as a plain
	 * PROT_NONE anonymous mapping that a no-pool kernel would not
	 * have, and the mirror loop has no idle-awareness.  Releasing the
	 * parked arenas here returns the parent to exactly the pre-pool
	 * layout before dup_mmap() copies any VMA, so parent and child
	 * both end up with the space the application itself observably
	 * munmapped -- and the fork paths need no parked-state handling.
	 * The teardowns are cheap: no content (park zapped it), no
	 * in-flight transactions (the park gating), no PT-page churn
	 * beyond what do_munmap releases.
	 */
	corten_arena_pool_flush_locked(oldmm, old_state);

	/* Freeze window (DEV-15): refuse new transactions, then drain
	 * the in-flight ones.  The frozen write and the drain pair per
	 * arena; a drained arena stays frozen until fork_commit()'s (or
	 * fork_abort()'s) unfreeze closure.  Only the child-state
	 * allocation above can fail, so nothing frozen is ever unwound
	 * inside begin itself.
	 */
	xa_for_each(&old_state->arenas, frame, arena) {
		/* M4.T1: reserve markers are not arenas. */
		if (arena == &corten_va_reserve_sentinel)
			continue;
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
 * for logically writable pages) -- except the pinned-private shape
 * (V-B.4: a GUP-pinned parent page was copied, not shared, and keeps
 * its writable PTE; recording SHARED there is INV7 drift).  Private-anon
 * and dropped-content slots are snapshot unchanged.  The snapshot is
 * consumed by the child
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
		bool shared = true;

		ret = corten_query(&txn, a, &m);
		if (unlikely(ret))
			break;

		if (m.state != CORTEN_MAPPED) {
			snap[pte_index(a)] = m;
			continue;
		}

		/* V-B.4: the pinned-private shape.  copy_page_range() copies
		 * (instead of sharing) a parent page GUP holds pinned, and
		 * skips pieces the child never inherited; both leave this
		 * PTE writable and the folio's exclusive mark in place --
		 * the child maps a different page (or nothing at all), so
		 * the slot is NOT hardware-shared and must not take the
		 * SHARED record (a SHARED slot behind a writable PTE is the
		 * INV7-drift shape).  The genuinely shared shape is
		 * distinguishable here precisely because copy_page_range()
		 * already wrprotected it (a COW mapping's present-writable
		 * PTE never survives the copy).  Unknown shapes (no
		 * translation, a special entry) keep the historical mark.
		 */
		{
			pte_t *ptep, cur;
			spinlock_t *ptl;	/* nests below desc write */

			ptep = pte_offset_map_lock(ar->mm, pmdp, a, &ptl);
			if (!ptep) {
				ret = -EAGAIN;
				break;
			}
			cur = ptep_get(ptep);
			pte_unmap_unlock(ptep, ptl);
			if (pte_present(cur) && !pte_special(cur) &&
			    pte_write(cur) &&
			    PageAnonExclusive(pte_page(cur)))
				shared = false;
		}

		/* The child mirrors the SHARED shape, so the snapshot
		 * records the post-mark state, not the pre-mark one.
		 */
		nm = m;
		nm.flags = m.flags;
		if (shared)
			nm.flags |= CORTEN_PF_SHARED;
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
 * V-A.2b: the child of a carrier arena gets its own detached carrier
 * -- created here (same bounds/flags), its anon_vma forked off the
 * parent's (the exact shape dup_mmap() establishes for tree VMAs, so
 * copy_page_range()'s rmap duplication on the child carrier is the
 * standard one) and published before the registry stores ([FAIL-2]).
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
	struct vm_area_struct *pcarrier, *ccarrier = NULL;
	struct corten_arena *child;
	struct file *rfile = NULL;
	int ret;

	child = kzalloc(sizeof(*child), GFP_KERNEL_ACCOUNT);
	if (!child)
		return -ENOMEM;

	child->start = ar->start;
	child->end = ar->end;
	child->mm = mm;
	child->prot = READ_ONCE(ar->prot);

	/* V-B.1: a FILE parent mirrors its payload into the child -- the
	 * child's carrier is born in FILE shape (the parent's file
	 * pointer, borrowed: register_file below takes the child's own
	 * reference, same object) with the parent's pgoff.
	 */
	if (READ_ONCE(ar->rclass) == CORTEN_REGION_FILE) {
		rfile = READ_ONCE(ar->rfile);
		WARN_ON_ONCE(!rfile || !READ_ONCE(ar->carrier));
	}

	/* V-A.2b/C1: the child carrier of an auto arena, allocated BARE
	 * (no anon_vma_prepare) so anon_vma_fork() starts from a NULL
	 * chain -- the exact dup_mmap() shape.  Both failures
	 * (-ENOMEM from the alloc or the fork) unwind before any
	 * publication.
	 */
	pcarrier = READ_ONCE(ar->carrier);
	if (pcarrier) {
		ccarrier = corten_arena_carrier_alloc(mm, ar->start, ar->end,
						      READ_ONCE(child->prot),
						      rfile,
						      READ_ONCE(ar->rpoff),
						      false);
		if (!ccarrier) {
			ret = -ENOMEM;
			goto out_free_child;
		}
		ret = anon_vma_fork(ccarrier, pcarrier);
		if (ret) {
			corten_arena_carrier_free(ccarrier);
			goto out_free_child;
		}
	}

	/* W1.b: a FILE parent pre-allocates the child's registry head
	 * here -- same contract as the attach arm: the link at the
	 * publication's last step allocates nothing, and the unwind
	 * below returns the head through the disarm.
	 */
	if (rfile) {
		child->rinodes = corten_inode_regions_new();
		if (!child->rinodes) {
			ret = -ENOMEM;
			goto out_free_child;
		}
	}

	/* Region record deep-copy (sec 2.2 lifecycle): the child's region
	 * mirrors the parent's class/MAY bound/flag record (the parent is
	 * always live here: the fork-begin pool flush released every
	 * parked arena).  V-B.1: a FILE parent takes the file-registered
	 * stamp instead -- the child's own reference (get_file) and the
	 * pgoff copy, so INV-MV3 rule (d) holds on both sides of the
	 * fork.  The pieces payload is always empty (no file punch
	 * producer), so single-piece IS the copy.  register()'s INV-MV3
	 * tripwire reads the anchor pairings, so the publishes below must
	 * precede it.
	 */
	WRITE_ONCE(child->carrier, ccarrier);
	WRITE_ONCE(child->vma, cvma);
	if (rfile)
		corten_region_register_file(child, READ_ONCE(ar->may_prot),
					    READ_ONCE(ar->rflags), rfile,
					    READ_ONCE(ar->rpoff));
	else
		corten_region_register(child, READ_ONCE(ar->rclass),
				       READ_ONCE(ar->may_prot),
				       READ_ONCE(ar->rflags));
	mutex_init(&child->fill_lock);
	init_completion(&child->drained);
	WRITE_ONCE(child->frozen, false);
	ret = percpu_ref_init(&child->active, corten_arena_active_release,
			      PERCPU_REF_INIT_ATOMIC, GFP_KERNEL);
	if (ret)
		goto out_put_carrier;

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

	/* Last publishing steps: the observability ledger (S8), then --
	 * V-B.1/W1.b -- the FILE region joins the child side of the
	 * mapping's per-inode registry (past every failure path,
	 * mirroring the attach's publication order; on the unwind below
	 * the arena was never published, so it must not be linked
	 * anywhere -- the ledger only ever contains registered arenas,
	 * the registry armed ones).
	 */
	corten_arena_obs_add(child);
	if (rfile) {
		corten_file_registry_insert(child);
		atomic_long_inc(&corten_nr_file_fork_mirrors);
	}

	mutex_unlock(&state->ctl_lock);

	return 0;

out_unwind:
	while (frame > first)
		xa_erase(&state->arenas, --frame);
	mutex_unlock(&state->ctl_lock);
	percpu_ref_exit(&child->active);
out_put_carrier:
	mutex_destroy(&child->fill_lock);
	/* V-B.1: the half-published FILE payload's reference (and the
	 * carrier's borrowed pointer) -- the registry node was never
	 * linked (the link is past every failure path), so the disarm
	 * spelling, not the teardown one.
	 */
	corten_region_file_disarm(child);
	if (ccarrier) {
		/* The child mm is being discarded wholesale on this path
		 * (fork_abort / MMF_UNSTABLE); dropping the half-forked
		 * carrier here keeps the anon_vma tree clean of a node no
		 * one will ever walk again.
		 */
		corten_arena_carrier_free(ccarrier);
		WRITE_ONCE(child->carrier, NULL);
	}
out_free_child:
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
		case CORTEN_SWAPPED:
			/* The child's swap PTE was installed by
			 * copy_nonpresent_pte() (the entry duplicated
			 * there); mirror the slot payload directly.
			 * mark() cannot express Invalid->SWAPPED (the
			 * state machine routes swap-outs through
			 * corten_swap_out(), a MAPPED-slot operation),
			 * so this is the replay arm -- before M6.T3 the
			 * only swapped-slot producer was the evict
			 * stub, and a fork after a swap-out aborted
			 * here (found by the M6.T3 pressure channel,
			 * which makes swapped slots the common shape).
			 * A child PTE that is not the mirrored entry
			 * (a WIPEONFORK piece: none; anything else:
			 * foreign) leaves the slot unrecorded, the
			 * clean-slate contract.
			 */
			if (pte_none(cur) || pte_present(cur) ||
			    non_swap_entry(pte_to_swp_entry(cur)) ||
			    pte_to_swp_entry(cur).val !=
			    corten_swap_decode(&m).val)
				continue;
			ret = corten_swap_replay(&txn, a, &m);
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
	struct vm_area_struct *vma, *pvma, *cvma, *pcarrier;
	pmd_t *cpmdp;
	unsigned long addr;
	bool any_piece = false;
	int ret;

	/* V-A.2b: the parent carrier (the PTE copy source below). */
	pcarrier = NULL;
	cvma = NULL;

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
	 *
	 * The test reads the TREE shadow only: an auto arena's anchor is
	 * its detached carrier (ar->vma == NULL), which never reaches the
	 * dup_mmap() loop -- the copy below carries it (V-A.2b), so the
	 * carrier shapes must not skip.
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
	if (!any_piece && READ_ONCE(ar->vma)) {
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

	/* V-A.2b: the PTE copy of an auto arena rides the carrier pair --
	 * copy_page_range() over the child/parent carriers is the exact
	 * machinery the dup_mmap() loop runs for tree VMAs (whitelist #1,
	 * DEV-14): COW wrprotect on both sides, folio ref + child rmap
	 * duplication (folio_try_dup_anon_rmap_pte clears the exclusive
	 * mark), swap-entry doubling, the GUP-pinned parent-page copy.
	 * The explicit A.1 arm (hand-rolled present/swap duplication
	 * without any rmap) is gone: restoring the anchor made its
	 * bookkeeping a partial re-derivation of copy_present_ptes().
	 *
	 * Lock shape = dup_mmap()'s: both mms write-locked (asserted in
	 * fork_commit), the parent carrier write-marked (the src_vma
	 * vma_assert_write_locked() inside copy_page_range), the child
	 * carrier write-marked at creation.  Punched holes inside the
	 * arena have no parent PT pages -- copy_page_range() walks by the
	 * parent's tables and costs nothing there.
	 */
	pcarrier = READ_ONCE(ar->carrier);
	if (pcarrier) {
		/* register_child() stored the child at the arena's first
		 * frame; its carrier is the dst_vma of the copy.
		 */
		struct corten_arena *child_ar;
		struct vm_area_struct *ccarrier;

		child_ar = xa_load(&state->arenas, ar->start >> PMD_SHIFT);
		ccarrier = child_ar ? READ_ONCE(child_ar->carrier) : NULL;
		if (WARN_ON_ONCE(!ccarrier))
			return -ENOENT;
		vma_start_write(pcarrier);
		ret = copy_page_range(ccarrier, pcarrier);
		if (ret)
			return -ENOMEM;
	}

	/* ④-2 + ④-4 per window: parent snapshot + SHARED marks, then the
	 * child replay of the same window.
	 */
	for (addr = ar->start; addr < ar->end;
	     addr = min((addr | (PMD_SIZE - 1)) + 1, ar->end)) {
		unsigned long win_end = min((addr | (PMD_SIZE - 1)) + 1,
					    ar->end);

		/* F2 (STATE D18, the over-SHARED corner): a window with no
		 * child PT page has no second mapper -- a VM_DONTCOPY piece
		 * is absent from the child (the dup_mmap loop skips it) and
		 * a VM_WIPEONFORK piece lost its translations (its copy is
		 * skipped).  Marking such pages SHARED would leave residue
		 * whose parent PTE is still writable (copy_page_range never
		 * wrprotected the piece): exactly the INV7-drift shape,
		 * plus a needless first-write COW detour the COW reuse
		 * branch would only have to unwind.  The child replay has
		 * nothing to mirror either (copy_window() gates on the same
		 * test): skip the window.  A window that DID get a child PT
		 * page marks normally -- page-granular piece borders inside
		 * one 2M window are tolerated as benign residue (they
		 * self-heal and stay INV7-clean only per-window).  For a
		 * carrier arena the gate reads the tables the carrier
		 * copy_page_range() just allocated (only where the parent
		 * had content).
		 */
		cpmdp = corten_arena_pmd(mm, addr);
		if (!cpmdp || !pmd_present(READ_ONCE(*cpmdp)) ||
		    pmd_leaf(READ_ONCE(*cpmdp)))
			continue;

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
	/* Pairs with the store in corten_arena_state_create().  NULL also
	 * covers the A5 arena-less child: fork_begin left the registry
	 * out because nothing was mirrored.
	 */
	state = smp_load_acquire(&mm->corten_state);
	if (!state)
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
		/* M4.T1: reserve markers are not arenas. */
		if (arena == &corten_va_reserve_sentinel)
			continue;
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

	/* V-A.3c lifecycle trigger: audit the child.  Its tree is the
	 * dup_mmap() copy (complete), its registry the mirror above plus
	 * the implant copy fork_begin made -- the one moment both sides of
	 * the dominion are provably in sync before the child goes live.
	 * Both mmap_writes are held: the stable-registry form.
	 */
	corten_audit_j2_walk_locked(mm);

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
 * Tier 1b (V-A.1): a *VMA-less* arena (a reactivated pool window, cached
 * pointer NULL, no tree piece anywhere) has no VMA to consult -- its
 * ownership is pure metadata: the descriptor bounds cover @addr and the
 * frame slot still points at this arena.  A punch of such a window erases
 * its frames before the zap (the RELEASE order), so a stale frame claim
 * cannot survive one, and the [F-B seal] re-check under the window lock
 * bounds the race exactly as for the other tiers.
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

	/* V-A.2b: tier 1 reads the anchor -- the carrier (auto arenas)
	 * bounds the arena exactly like the shadow-VMA did (same start/
	 * end by construction), so the zero-walk fast path works for
	 * both shapes.
	 */
	vma = corten_arena_anchor_vma(ar);
	if (vma && corten_arena_vma_spans(vma, addr))
		return true;

	if (!vma) {
		/* Tier 1b: metadata-only ownership (VMA-less arena). */
		struct corten_mm_state *state;

		/* Pairs with the store in corten_arena_state_create(). */
		state = smp_load_acquire(&mm->corten_state);

		if (addr < ar->start || addr >= ar->end || !state)
			return false;

		rcu_read_lock();
		owned = xa_load(&state->arenas, addr >> PMD_SHIFT) == ar;
		rcu_read_unlock();
		return owned;
	}

	/* Tier 2: rare -- only punched or tearing-down arenas get here. */
	rcu_read_lock();
	vma = corten_vma_find(mm, addr, addr + 1);
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
static struct shrinker *corten_shrinker_handle;

/* M6.T3: announce an arena page to the memcg shrinker map (the same
 * post-charge announcement THP's deferred splitter does).  Without the
 * bit, memcg-targeted reclaim skips the corten-arena shrinker for that
 * cgroup entirely -- a cgroup under pressure would OOM with
 * swap-outable arena pages in it.  Cheap when the bit is already set.
 */
static void corten_shrinker_arm_memcg(struct folio *folio)
{
#ifdef CONFIG_MEMCG
	if (corten_shrinker_handle)
		set_shrinker_bit(folio_memcg(folio), folio_nid(folio),
				 corten_shrinker_handle->id);
#endif
}

/*
 * V-A.1: the VMA-less counterpart of vma_alloc_zeroed_movable_folio().
 * An arena window never carries a mempolicy (mbind is route-rejected),
 * so the task's default policy -- exactly what get_vma_policy() falls
 * back to for a policy-less VMA -- is the same allocation answer; only
 * the vm_flags read (VM_DROPPABLE) and the shared-policy lookup are
 * lost, and neither exists for an arena.
 */
static struct folio *corten_arena_folio_alloc_novma(struct mm_struct *mm,
						    unsigned long addr)
{
	struct folio *folio;

	folio = folio_alloc_mpol_noprof(GFP_HIGHUSER_MOVABLE | __GFP_ZERO |
					__GFP_CMA, 0, get_task_policy(current),
					0, numa_node_id());
	if (!folio)
		return NULL;

	if (mem_cgroup_charge(folio, mm, GFP_KERNEL)) {
		folio_put(folio);
		return NULL;
	}
	corten_shrinker_arm_memcg(folio);

	folio_throttle_swaprate(folio, GFP_KERNEL);

	return folio;
}

static struct folio *corten_arena_folio_prealloc(struct mm_struct *mm,
						 struct vm_area_struct *vma,
						 unsigned long addr)
{
	struct folio *folio;

	if (!vma)
		return corten_arena_folio_alloc_novma(mm, addr);

	folio = vma_alloc_zeroed_movable_folio(vma, addr);
	if (!folio)
		return NULL;

	if (mem_cgroup_charge(folio, mm, GFP_KERNEL)) {
		folio_put(folio);
		return NULL;
	}
	corten_shrinker_arm_memcg(folio);

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
	case CORTEN_SWAPPED:
		/* M6.T2 (spec D5): the page lives behind a swap entry --
		 * the fault reads it back.  The handler needs the __resv
		 * payload and its own unlock/relock cycle (swap cache
		 * lookups and I/O sleeps, INV3 forbids both under the
		 * desc lock), so the classifier only routes; see
		 * corten_arena_swap_in().
		 */
		return CORTEN_DISP_SWAPIN;
	case CORTEN_FILE_MAPPED:
		/* V-B.3 (H4): the FILE_MAPPED virtual allocation is live
		 * semantics now.  A read (or instruction) fault takes the
		 * pagecache read arm; a write fault on a writable contract
		 * takes the COW transaction (a MAP_PRIVATE file page is
		 * never written through: the first write copies, exactly
		 * like do_cow_fault()); anything else is a permission
		 * fault.  The SHARED flag distinction the MAPPED arm makes
		 * is moot here: FILE_MAPPED's write answer is COW either
		 * way (shared pagecache folios never reuse), so a read-only
		 * contract answers ACCERR directly (legacy access_error()
		 * on a !VM_WRITE file VMA, both fork-shared and not).
		 */
		if (!corten_arena_perm_ok(m, write, instruction))
			return CORTEN_DISP_ACCERR;
		if (write)
			return CORTEN_DISP_COW_MAYBE;
		return CORTEN_DISP_FILE_READ;
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
 * Walk to the PUD ENTRY of @addr with the same top-down gating (the
 * pud-holding page is one p4d level up; see corten_arena_pmd()).  The
 * V-D exit walk uses it to reach a window span's pud entry for the
 * PMD-page retirement decision.
 */
static pud_t *corten_arena_pud(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgdp = pgd_offset(mm, addr);
	p4d_t *p4dp;

	if (!pgd_present(READ_ONCE(*pgdp)))
		return NULL;
	p4dp = p4d_offset(pgdp, addr);
	if (!p4d_present(READ_ONCE(*p4dp)))
		return NULL;

	return pud_offset(p4dp, addr);
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
	CORTEN_F_BUS,		/* V-B.3: beyond EOF -> SIGBUS BUS_ADRERR
				 * (the filemap_fault() verdict; S-FILE-1
				 * disclosure: legacy-identical semantics,
				 * new delivery path)
				 */
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
	bool			unshare; /* FAULT_FLAG_UNSHARE: GUP read-pin
					  * pre-break (M5.T3)
					  */
	bool			instruction;
	bool			swapin;	/* a swap-in ran in some attempt:
					 * widens the Fig.7 retry budget to
					 * 2+2 (spec D5): the unlocked
					 * re-validation of the swap-in
					 * transaction spends one extra
					 * round when it loses a race
					 */
	bool			fileio;	/* V-B.3: a file read/COW arm ran
					 * (pagecache fetch): the same
					 * widened 2+2 budget -- its
					 * unlocked re-validation spends one
					 * extra round per lost race (the
					 * swapin shape; truncate racing the
					 * fetch)
					 */
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
		ctx->vma = corten_arena_anchor_vma(ctx->ar);

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

	/* S5 assertion: THP/mTHP/khugepaged are excluded by the shadow
	 * VM_NOHUGEPAGE flag (sec 4.7); a set bit would mean the
	 * exclusion is broken and the covering-lock protocol is in
	 * danger.  V-A.1: a VMA-less arena excludes THP structurally
	 * (no VMA for khugepaged to scan).
	 */
	if (WARN_ON_ONCE(vma && (vma->vm_flags & (VM_HUGEPAGE | VM_HUGETLB))))
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
			     vma ? corten_arena_perm_pgprot(vma, m->perm) :
				   corten_arena_perm_pgprot_pure(m->perm));
	entry = pte_sw_mkyoung(entry);
	if (ctx->write)
		entry = corten_pte_mkwrite(pte_mkdirty(entry), vma);

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
		corten_pte_clear_flush(vma, mm, ctx->addr, ptep);
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
	/* V-A.1: a VMA-less arena's pages carry no rmap anchor -- there is
	 * no vma for folio_add_new_anon_rmap() to hang the anon_vma chain
	 * on.  The PTE reference (refcount) is the only binding; every
	 * rmap-walking consumer of arena folios is structurally excluded
	 * (migration/hwpoison reject them, the reclaim guards refuse
	 * them, GUP-slow needs a VMA), and the zap/mprotect paths keep
	 * the mapcount symmetry by skipping the removal for the same
	 * shape.  The V-A.2b carrier restores the anchor.
	 */
	if (vma)
		folio_add_new_anon_rmap(arena_folio, vma, ctx->addr,
					RMAP_EXCLUSIVE);
	/* M3 difference vs do_anonymous_page(): no folio_add_lru_vma() --
	 * arena pages stay off the LRU so reclaim/migration can never
	 * write to them outside a transaction (sec 4.6, matrix 5.17).
	 * M6.T1 no longer leans on this alone: the rmap walkers refuse
	 * shadow-VMAs at their entry (corten_rmap_unmap_one()), so the
	 * claim survives even if some future change anchors arena folios
	 * on an LRU (M6_RMAP_SPEC.md risk R6-3 / violation V2).
	 */

	set_ptes(mm, ctx->addr, ptep, entry, 1);
	/* No TLB invalidate needed (memory.c:5269 argument): the PTE was
	 * pte_none() above, so no CPU can hold a stale translation.
	 */

	if (vma)
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

	entry = pte_mkspecial(pfn_pte(my_zero_pfn(ctx->addr),
				      vma ?
				      corten_arena_perm_pgprot(vma, m->perm) :
				      corten_arena_perm_pgprot_pure(m->perm)));

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
	if (vma)
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

	pmdp = corten_arena_pmd(mm, ctx->addr);
	if (!pmdp)
		return -EAGAIN;
	ptep = pte_offset_map_lock(mm, pmdp, ctx->addr, &ptl);
	if (!ptep)
		return -EAGAIN;

	cur = ptep_get(ptep);
	if (unlikely(!pte_present(cur))) {
		/* CORTEN_MAPPED without a PTE: no M3 path produces this
		 * (all zaps go through transactions).  M6.T1 premise note:
		 * the reclaim side doors are guarded too -- the OOM reaper
		 * skips shadow-VMAs (corten_oom_reap_skip_vma()) and the
		 * rmap walkers refuse them (corten_rmap_unmap_one()) -- so
		 * this WARN firing under memory pressure means the guard
		 * was bypassed, not that reclaim is expected here.
		 */
		pte_unmap_unlock(ptep, ptl);
		WARN_ON_ONCE(1);
		return -EFAULT;
	}

	/* T0b: rebuild with the recorded perm's encoding (the metadata is
	 * the source of truth), not the shadow-VMA flags.
	 */
	entry = mk_pte(pfn_to_page(pte_pfn(cur)),
		       vma ? corten_arena_perm_pgprot(vma, m->perm) :
			     corten_arena_perm_pgprot_pure(m->perm));
	entry = pte_mkyoung(entry);
	if (ctx->write)
		entry = corten_pte_mkwrite(pte_mkdirty(entry), vma);

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
	 * case); a pure no-op rebuild stays flush-free.  The VMA-less
	 * arm always sets and always flushes (bounded page flush).
	 */
	corten_pte_set_access_flags(vma, mm, ctx->addr, ptep, entry,
				    ctx->write);
	if (vma)
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
	bool reuse;
	bool old_is_file;	/* V-B.3 (H6): pagecache folio behind the PTE */
	int ret;

	vma = corten_arena_get_vma(ctx);

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
	old_is_file = !folio_test_anon(old);

	/* V-B.3 (H6): a pagecache folio (the FILE_MAPPED read arm's
	 * install) never takes the reuse answer -- its mapcount is the
	 * whole machine's business (other processes mapping the same
	 * file) and PageAnonExclusive on it would be a lie; the write is
	 * always satisfied by the private copy below (wp_page_copy()'s
	 * file shape, the counter split of corten_zap_release_page()).
	 * Reuse resumes on the copy's own second write, which finds a
	 * CORTEN_MAPPED anon folio again.
	 */

	/* Reuse (paper Fig.8 L28-29: "no need to COW if parent/child has
	 * left") needs the folio exclusively ours under this ptl.  OQ-5
	 * (M5_FORK_SPEC.md sec 8, resolved per upstream): the reuse answer
	 * marks the page exclusive -- do_wp_page() does exactly this in
	 * front of wp_page_reuse(), and a later GUP PIN of the survivor
	 * demands it (gup.c's VM_WARN_ON_ONCE_PAGE(PIN && !exclusive)
	 * would fire on the shared survivor).  A DMA-pinned folio takes
	 * the copy branch instead: wp_can_reuse_anon_folio() refuses
	 * those upstream, and "copy where a reuse would also have been
	 * safe" is the bounded race direction (R-B).
	 */
	reuse = !old_is_file && folio_mapcount(old) == 1;
	if (reuse && !PageAnonExclusive(page)) {
		if (folio_maybe_dma_pinned(old)) {
			reuse = false;
		} else {
			SetPageAnonExclusive(page);
		}
	}

	if (reuse) {
		/* The meta flag clear and the PTE re-arm are one
		 * transaction: every observer is excluded by the covering
		 * desc write lock.
		 */
		struct corten_pte_meta nm = *m;

		nm.flags = m->flags & ~CORTEN_PF_SHARED;
		ret = corten_mark(txn, ctx->addr, PAGE_SIZE, &nm);
		if (unlikely(ret)) {
			pte_unmap_unlock(ptep, ptl);
			return ret == -ENOMEM ? -ENOMEM : -EFAULT;
		}

		entry = mk_pte(page, vma ?
			       corten_arena_perm_pgprot(vma, m->perm) :
			       corten_arena_perm_pgprot_pure(m->perm));
		entry = pte_mkyoung(entry);
		entry = corten_pte_mkwrite(pte_mkdirty(entry), vma);
		/* Flushes only when the old translation could be cached
		 * (the fork's RO shape always qualifies).
		 */
		corten_pte_set_access_flags(vma, mm, ctx->addr, ptep, entry,
					    1);
		if (vma)
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
	corten_pte_clear_flush(vma, mm, ctx->addr, ptep);

	entry = folio_mk_pte(ctx->folio,
			     vma ? corten_arena_perm_pgprot(vma, m->perm) :
				   corten_arena_perm_pgprot_pure(m->perm));
	entry = pte_mkyoung(entry);
	entry = corten_pte_mkwrite(pte_mkdirty(entry), vma);

	/* The speculative single reference becomes the new PTE reference
	 * (order-0); the old folio's PTE reference is released only after
	 * the flush, in the zap ordering.
	 */
	add_mm_counter(mm, MM_ANONPAGES, 1);
	if (vma)
		folio_add_new_anon_rmap(ctx->folio, vma, ctx->addr,
					RMAP_EXCLUSIVE);
	set_ptes(mm, ctx->addr, ptep, entry, 1);
	if (vma)
		update_mmu_cache_range(NULL, vma, ctx->addr, ptep, 1);

	/* V-A.1: the old folio of a VMA-less arena carries no rmap anchor
	 * (see map_anon); the removal stays symmetric with the add.
	 * W1.c (R3): a pagecache old folio's rmap is the file mapping's
	 * own mapcount (the read arm's novma install) -- its removal is
	 * vma-free now, numerically the same -1 mapper the vma-ful call
	 * ran.  V-B.3 (H6): its counter is mm_counter_file()'s family
	 * (V-B.4) -- the same split corten_zap_release_page() makes.
	 */
	if (old_is_file)
		folio_remove_file_rmap_novma(old);
	else if (vma)
		folio_remove_rmap_pte(old, page, vma);
	add_mm_counter(mm, old_is_file ? mm_counter_file(old) :
				    MM_ANONPAGES, -1);
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
	if (old_is_file)
		atomic_long_inc(&corten_nr_file_cow_copies);

	/* The speculative single reference became the new PTE reference
	 * (see the fault_once success epilogue for the other handlers'
	 * ownership).
	 */
	ctx->folio = NULL;

	return 0;
}

/*
 * M6.T2 swap-in (spec sec 2.1 D5): the fault-side reverse of the
 * swap-out transaction.  fault_once() calls it right after releasing the
 * covering desc write lock: reading the page back sleeps (swap cache,
 * readahead, zram I/O, folio lock) and the desc lock is a BH rwlock --
 * no sleeping under it (INV3).  The handler runs its own lock cycles:
 *
 *   lookup/read the entry's folio (swap cache, cluster readahead) ->
 *   folio_lock -> re-lock the range -> re-query -> commit PTE + rmap +
 *   counters + metadata under ONE transaction -> unlock -> swap_free().
 *
 * Every "the world changed while unlocked" outcome (the entry replaced
 * or freed by a concurrent zap, the PTE written by a racing fault, the
 * window re-punched) re-queries to a different metadata shape and
 * answers -EAGAIN: the fault_once retry budget re-dispatches from the
 * query (Fig. 7), so a stale entry is never dereferenced past the
 * re-validation.  Counted (swapin_retries).
 *
 * Why the full transaction instead of falling back to the legacy
 * do_swap_page(): the fallback would need a post-legacy metadata sync
 * glue site INSIDE handle_mm_fault(), and even with it the Swapped
 * metadata would trail the legacy PTE store -- a window (and, if the
 * legacy funnel takes the do_anonymous_page branch, a permanent shape)
 * of Swapped-meta over a none PTE whose next fault answers a zero page
 * over swapped-out content.  The transaction keeps the metadata and the
 * PTE in one critical section, so every lock boundary exposes a
 * consistent pair (PS-B2; the swap PTE stays intact until the commit).
 *
 * Mirrors do_swap_page() (mm/memory.c) for the order-0, non-KSM,
 * uffd-less, through-the-swap-cache shape.  Arena-specific deviations,
 * each deliberate:
 *   - the SWP_SYNCHRONOUS direct shape (alloc + memcg charge +
 *     swapcache_prepare serialization + swap_read_folio) instead of the
 *     swap-cache/readahead flavor: __read_swap_cache_async() batches
 *     its folios onto the LRU (folio_add_lru(), swap_state.c:498) and
 *     the legacy machinery expects swapin folios to be
 *     LRU-reclaimable -- arena pages are never LRU-anchored (DEV-10)
 *     and the reclaim guards refuse them, so a batched cache folio
 *     became a reclaim-invisible zombie the MGLRU still aged (the
 *     lru_gen/freelist poison of the first T2 guest run).  zram is a
 *     synchronous device and the full-reclaim shape leaves exactly the
 *     PTE's reference on the entry, so the direct read covers the
 *     common case without touching page tables (no fabricated
 *     vm_fault, no PT-pinning problem in the lock-free phase).
 *   - the folio is never added to the LRU (the DEV-10 red line, the
 *     same omission as map_anon(); do_swap_page() adds it).
 *   - exclusivity comes from the swap PTE's exclusive bit (recorded by
 *     the swap-out transaction; fork's copy_nonpresent_pte() clears it
 *     on one side, exactly as upstream).  A non-exclusive swap-in
 *     re-arms the metadata SHARED record so every later write fault
 *     takes the M5 COW transaction instead of a bare re-arm -- two
 *     fork sharers of one entry can never both hold the write bit.
 *   - the cache-copy try-free runs even on read faults: an arena folio
 *     is off the LRU, so a swapcache copy left behind would be
 *     uncollectable (upstream leaves the copy for LRU reclaim).
 */
static int corten_arena_swap_in(struct corten_fault_ctx *ctx,
				const struct corten_pte_meta *m)
{
	struct mm_struct *mm = ctx->mm;
	struct vm_area_struct *vma;
	struct folio *folio;
	struct swap_info_struct *si;
	void *shadow;
	struct corten_txn txn;
	struct corten_pte_meta m2, nm;
	struct page *page;
	swp_entry_t entry;
	unsigned long deadline;
	pmd_t *pmdp;
	pte_t *ptep, cur, newpte;
	spinlock_t *ptl;
	rmap_t rmap_flags = RMAP_NONE;
	bool need_clear_cache = false;
	bool exclusive = true;
	int ret;

	vma = ctx->vma ? ctx->vma : corten_arena_anchor_vma(ctx->ar);
	if (!vma)
		return -EFAULT;

	entry = corten_swap_decode(m);
	if (unlikely(!entry.val))
		return -EFAULT;

	/* Prevent swapoff from happening to us (do_swap_page()). */
	si = get_swap_device(entry);
	if (unlikely(!si))
		return -EFAULT;

	/*
	 * Lock-free phase: the DIRECT swapin of do_swap_page()'s
	 * SWP_SYNCHRONOUS_IO branch (zram is a synchronous device and the
	 * full-reclaim shape leaves exactly the PTE's reference on the
	 * entry, __swap_count() == 1).  The swap-cache/readahead flavor
	 * is NOT usable for arena pages: __read_swap_cache_async()
	 * batches its folios onto the LRU (folio_add_lru(),
	 * swap_state.c:498) and the whole legacy machinery expects
	 * swapin folios to be LRU-reclaimable -- arena pages are never
	 * LRU-anchored (DEV-10) and the reclaim guards refuse them, so
	 * a batched cache folio turned into a reclaim-invisible zombie
	 * the MGLRU still aged (the lru_gen/freelist poison of the first
	 * T2 guest run).  The direct folio is charged with the swap
	 * entry's memcg charge (memcg1_swapin()) and never LRU'd.
	 */
	deadline = jiffies + 5 * HZ;
retry:
	/* V-A.1: vma may be NULL (a VMA-less reactivated window); the
	 * arena has no mempolicy, so the task-default-policy allocation
	 * is the same answer (see corten_arena_folio_alloc_novma()).
	 */
	if (vma)
		folio = vma_alloc_folio(GFP_HIGHUSER_MOVABLE | __GFP_CMA, 0,
					vma, ctx->addr);
	else
		folio = folio_alloc_mpol_noprof(GFP_HIGHUSER_MOVABLE |
						__GFP_CMA, 0,
						get_task_policy(current), 0,
						numa_node_id());
	if (!folio) {
		put_swap_device(si);
		return -ENOMEM;
	}
	if (mem_cgroup_swapin_charge_folio(folio, mm, GFP_KERNEL, entry)) {
		folio_put(folio);
		put_swap_device(si);
		return -ENOMEM;
	}
	/* Re-arm the memcg shrinker bit: an all-swapped cgroup had its
	 * bit cleared (SHRINK_EMPTY) and the swap-in is the charge event
	 * that brings resident pages back.
	 */
	corten_shrinker_arm_memcg(folio);
	__folio_set_locked(folio);
	__folio_set_swapbacked(folio);

	/* Serialize against a parallel swapin of the same entry (fork
	 * shares swap entries between mm's): the cache flag doubles as
	 * the in-flight mark, swapcache_clear() releases it below.
	 */
	if (swapcache_prepare(entry, 1)) {
		folio_unlock(folio);
		folio_put(folio);
		if (time_after(jiffies, deadline)) {
			put_swap_device(si);
			return -EAGAIN;
		}
		schedule_timeout_uninterruptible(1);
		goto retry;
	}
	need_clear_cache = true;

	memcg1_swapin(entry, 1);
	shadow = swap_cache_get_shadow(entry);
	if (shadow)
		workingset_refault(folio, shadow);

	/* No folio_add_lru(): arena pages stay off the LRU (DEV-10) --
	 * the deliberate omission upstream's sync branch does make.
	 */
	/* Provide the entry for swap_read_folio(). */
	folio->swap = entry;
	swap_read_folio(folio, NULL);
	folio->private = NULL;

	count_vm_event(PGMAJFAULT);
	count_memcg_event_mm(mm, PGMAJFAULT);

	/* The folio is ours end-to-end (allocated, locked and read in the
	 * direct phase above); only the poisoned-read answer remains.
	 */
	page = folio_page(folio, 0);
	if (unlikely(PageHWPoison(page))) {
		/* Poisoned swap-in pages are kept to kill their owner
		 * (do_swap_page()); answer the loud fault.
		 */
		ret = -EFAULT;
		goto out_clear;
	}

	folio_throttle_swaprate(folio, GFP_KERNEL);

	/* Re-lock: everything past here runs inside one transaction. */
	ret = corten_lock_range(mm, ctx->addr, PAGE_SIZE, &txn);
	if (ret) {
		ret = ret == -EAGAIN ? -EAGAIN : -ENOMEM;
		goto out_put;
	}
	if (corten_query(&txn, ctx->addr, &m2)) {
		ret = -EAGAIN;
		goto out_unlock_txn;
	}
	if (m2.state != CORTEN_SWAPPED ||
	    corten_swap_decode(&m2).val != entry.val) {
		/* The slot moved while unlocked (zap/other fault). */
		atomic_long_inc(&corten_nr_swapin_retries);
		ret = -EAGAIN;
		goto out_unlock_txn;
	}

	if (corten_meta_ensure_locked(txn.covering)) {
		ret = -ENOMEM;
		goto out_unlock_txn;
	}

	pmdp = corten_arena_pmd(mm, ctx->addr);
	if (!pmdp) {
		ret = -EAGAIN;
		goto out_unlock_txn;
	}
	ptep = pte_offset_map_lock(mm, pmdp, ctx->addr, &ptl);
	if (!ptep) {
		ret = -EAGAIN;
		goto out_unlock_txn;
	}

	cur = ptep_get(ptep);
	if (unlikely(pte_none(cur))) {
		/* No producer of this shape: the swap PTE the swap-out
		 * transaction installed cannot vanish without the zap
		 * that also resets the metadata.
		 */
		pte_unmap_unlock(ptep, ptl);
		WARN_ON_ONCE(1);
		ret = -EFAULT;
		goto out_unlock_txn;
	}
	if (pte_present(cur)) {
		if (!pte_special(cur) && pte_pfn(cur) == page_to_pfn(page)) {
			/* A bare writer already installed the resident
			 * page over our swap PTE: the swapoff parity
			 * shape (unuse_pte() raced the unlocked phase).
			 * Heal the metadata only -- the writer moved the
			 * counters and the rmap -- and let the fault
			 * re-dispatch against CORTEN_MAPPED.
			 */
			nm = m2;
			nm.state = CORTEN_MAPPED;
			ret = corten_map(&txn, ctx->addr, page, m2.perm,
					 CORTEN_MAP_FORCE);
			if (!ret)
				ret = corten_mark(&txn, ctx->addr, PAGE_SIZE,
						  &nm);
			pte_unmap_unlock(ptep, ptl);
			if (ret) {
				WARN_ON_ONCE(1);
				ret = -EFAULT;
				goto out_unlock_txn;
			}
			atomic_long_inc(&corten_nr_swapin_heals);
			ret = -EAGAIN;
			goto out_unlock_txn;
		}
		/* Some other translation (zero page / foreign page):
		 * re-dispatch from the query, the pure classifier owns
		 * those shapes.
		 */
		pte_unmap_unlock(ptep, ptl);
		atomic_long_inc(&corten_nr_swapin_retries);
		ret = -EAGAIN;
		goto out_unlock_txn;
	}
	if (pte_to_swp_entry(cur).val != entry.val) {
		/* A different (or re-encoded) swap entry: the slot was
		 * re-swapped under us; re-dispatch.
		 */
		pte_unmap_unlock(ptep, ptl);
		atomic_long_inc(&corten_nr_swapin_retries);
		ret = -EAGAIN;
		goto out_unlock_txn;
	}

	if (unlikely(!folio_test_uptodate(folio))) {
		/* The read failed (do_swap_page() answers SIGBUS).  The
		 * arena fault layer has no SIGBUS action; MAPERR is the
		 * loud counterpart (documented deviation).
		 */
		pte_unmap_unlock(ptep, ptl);
		ret = -EFAULT;
		goto out_unlock_txn;
	}

	/* A fresh folio never exposed to the swapcache -> certainly
	 * exclusive (do_swap_page()'s sync shape).  The write arming and
	 * the SHARED re-arm below follow do_swap_page()'s exclusive
	 * branch.
	 */
	exclusive = true;
	rmap_flags |= RMAP_EXCLUSIVE;

	/* Restore architecture metadata before swap_free() (upstream
	 * ordering); a PTE reference is kept until the metadata commit
	 * below, so the entry cannot be reused under the pair.
	 */
	/* Order-0 only (the swap-out transaction enforces it): @entry is
	 * the folio entry; folio_swap() would be the identity here.
	 */
	arch_swap_restore(entry, folio);

	add_mm_counter(mm, MM_ANONPAGES, 1);
	add_mm_counter(mm, MM_SWAPENTS, -1);

	newpte = mk_pte(page, vma ? corten_arena_perm_pgprot(vma, m2.perm) :
				    corten_arena_perm_pgprot_pure(m2.perm));
	if (pte_swp_soft_dirty(cur))
		newpte = pte_mksoft_dirty(newpte);
	if (exclusive && ctx->write && (m2.perm & CORTEN_PERM_WRITE))
		newpte = corten_pte_mkwrite(pte_mkdirty(newpte), vma);
	/* Refault grace (OQ-M6-8): x86's pte_sw_mkyoung() is a no-op, so a
	 * bare install starts not-young and the T3 two-pass aging would
	 * swap the page right back out before its first reuse under
	 * sustained pressure -- a swap-in/swap-out livelock with no LRU
	 * workingset protection to lean on (arena pages are never LRU'd).
	 * The young bit here is the swap-in's access record: the page gets
	 * one full aging epoch before the shrinker may reclaim it again
	 * (the same grace upstream's folio_mark_accessed() gives a refault
	 * on the swap cache).
	 */
	newpte = pte_mkyoung(newpte);

	/* No folio_add_lru(): arena pages stay off the LRU (DEV-10).
	 * The fresh folio gets the new-anon rmap shape (upstream's
	 * non-swapcache branch); VMA-less arenas skip it (see map_anon).
	 */
	if (vma)
		folio_add_new_anon_rmap(folio, vma, ctx->addr, rmap_flags);
	/* No TLB invalidate: the PTE was a swap entry (non-present), so
	 * no CPU can hold a translation for it.
	 */
	set_ptes(mm, ctx->addr, ptep, newpte, 1);
	if (vma)
		update_mmu_cache_range(NULL, vma, ctx->addr, ptep, 1);
	pte_unmap_unlock(ptep, ptl);

	/* Metadata transition in the same transaction (Swapped -> Mapped
	 * is the paper's legal fault transition).  corten_map() scrubs
	 * the flags, so the non-exclusive shape re-arms SHARED (+ the
	 * WRITABLE record the M5 mark rule demands) through corten_mark().
	 */
	nm = m2;
	nm.state = CORTEN_MAPPED;
	nm.flags = 0;
	if (!exclusive) {
		nm.flags = CORTEN_PF_SHARED;
		if (nm.perm & CORTEN_PERM_WRITE)
			nm.flags |= CORTEN_PF_WRITABLE;
	}
	ret = corten_map(&txn, ctx->addr, page, m2.perm, 0);
	if (!ret && nm.flags)
		ret = corten_mark(&txn, ctx->addr, PAGE_SIZE, &nm);
	if (WARN_ON_ONCE(ret)) {
		/* Impossible (the array was ensured above): the PTE and
		 * counters are committed, the metadata still says
		 * Swapped -- the heal branch of the next fault repairs
		 * the pair.  No folio_put: the lookup reference IS the
		 * PTE reference now (do_swap_page() accounting,
		 * nr_pages - 1 == 0).
		 */
		corten_unlock(&txn);
		folio_unlock(folio);
		if (need_clear_cache)
			swapcache_clear(si, entry, 1);
		put_swap_device(si);
		return -EFAULT;
	}
	corten_arena_fault_stat(READ_ONCE(mm->corten_state),
				CORTEN_ARENA_STAT_MAPPED);
	corten_unlock(&txn);
	atomic_long_inc(&corten_nr_swapins);

	folio_unlock(folio);
	/* The PTE's reference on the entry (upstream frees it under the
	 * ptl before the install; the arena holds it through the
	 * metadata commit instead -- strictly stronger, see above).
	 */
	swap_free(entry);

	/* No folio_put(): the allocation reference IS the PTE reference
	 * now (do_swap_page() accounting, nr_pages - 1 == 0); dropping it
	 * would strip a reference from a still-mapped folio -- the
	 * freelist-poison panic of the first T2 guest run.
	 */
	if (need_clear_cache)
		swapcache_clear(si, entry, 1);
	put_swap_device(si);
	return 0;

out_unlock_txn:
	corten_unlock(&txn);
out_clear:
	if (need_clear_cache)
		swapcache_clear(si, entry, 1);
out_put:
	folio_unlock(folio);
	folio_put(folio);
	put_swap_device(si);
	return ret;
}

/*
 * V-B.3 (H5): fetch the pagecache folio of @pgoff -- the filemap_fault()
 * core without the vm_fault wrapper (single page, no readahead, no
 * fault-around: the B.3 scope).  Runs OUTSIDE every arena lock (the folio
 * lock, the shmem allocator and ->read_folio sleep, INV3) with the arena
 * pinned by ctx->ar's active reference and the region's rfile reference.
 *
 * EOF gate first (filemap_fault()'s entry check): a pgoff at or past the
 * i_size page count answers -ENODATA, the caller's CORTEN_F_BUS (legacy
 * VM_FAULT_SIGBUS; the S-FILE-1 disclosure -- same verdict, new delivery
 * path).
 *
 * Two host families:
 *
 *   shmem/tmpfs (shmem_mapping()): shmem_get_folio(SGP_CACHE) -- the
 *   REAL hole allocator.  A raw FGP_CREAT into a shmem mapping would
 *   bypass shmem_inode_acct_blocks() (tmpfs quota leak, info->alloced
 *   drift, negative used_blocks on the later recalc), would not read
 *   back a swapped-out entry, and would hand out unzeroed memory as
 *   the hole contents.  SGP_CACHE does all three correctly; its own
 *   EOF race answer is -EINVAL (mapped to -ENODATA below).
 *
 *   regular files: the fetch holds the mapping's invalidate lock (the
 *   filemap_create_folio() contract: no folio may be instantiated
 *   into a range a truncate is evicting), takes the folio unlocked
 *   (FGP_FOR_MMAP: the caller of record for doing its own locking
 *   dance) -- a miss creates it, the synchronous read-in, the
 *   PGMAJFAULT shape -- then fills a !uptodate folio through the
 *   filler exactly like filemap_read_folio() (the read unlocks the
 *   folio, the killable wait follows, the lock is re-taken for the
 *   post-read re-checks).
 *
 * Truncation re-check under the folio lock (filemap_fault()'s "Did it
 * get truncated?") and the i_size recheck under the same lock ("We
 * must recheck i_size under page lock"): a folio whose mapping moved,
 * or a pgoff past an i_size that shrank since the gate, drops and
 * answers -EAGAIN / -ENODATA; the bounded retry re-fetches (or the
 * EOF gate answers BUS on the next round).
 *
 * Return: the unlocked, uptodate, referenced folio, or
 * ERR_PTR(-ENODATA) (past EOF), ERR_PTR(-EAGAIN) (truncated/raced:
 * retry), ERR_PTR(-ENOMEM)/ERR_PTR(-EIO).
 */
static struct folio *corten_arena_file_fetch(struct corten_arena *ar,
					     unsigned long pgoff)
{
	struct address_space *mapping = ar->rfile->f_mapping;
	struct folio *folio;
	int ret;

	if (pgoff >= DIV_ROUND_UP(i_size_read(mapping->host), PAGE_SIZE))
		return ERR_PTR(-ENODATA);

	if (IS_ENABLED(CONFIG_SHMEM) && shmem_mapping(mapping)) {
		ret = shmem_get_folio(mapping->host, pgoff, 0, &folio,
				      SGP_CACHE);
		if (unlikely(ret)) {
			if (ret == -EINVAL)
				return ERR_PTR(-ENODATA);
			return ERR_PTR(ret == -ENOMEM ? -ENOMEM : -EIO);
		}
		/* The contract: locked, referenced, zero-filled hole or
		 * swapped-in content, fully accounted.
		 */
		if (unlikely(folio->mapping != mapping)) {
			folio_unlock(folio);
			folio_put(folio);
			return ERR_PTR(-EAGAIN);
		}
		if (unlikely(pgoff >= DIV_ROUND_UP(i_size_read(mapping->host),
						   PAGE_SIZE))) {
			folio_unlock(folio);
			folio_put(folio);
			return ERR_PTR(-ENODATA);
		}
		folio_unlock(folio);
		return folio;
	}

	if (unlikely(!mapping->a_ops->read_folio))
		return ERR_PTR(-EIO);	/* no host to fill this folio from */

	filemap_invalidate_lock_shared(mapping);
	folio = __filemap_get_folio(mapping, pgoff,
				    FGP_CREAT | FGP_FOR_MMAP,
				    mapping_gfp_constraint(mapping,
							   GFP_KERNEL));
	if (IS_ERR(folio)) {
		filemap_invalidate_unlock_shared(mapping);
		return folio;
	}
	count_vm_event(PGMAJFAULT);
	count_memcg_event_mm(ar->mm, PGMAJFAULT);

	folio_lock(folio);
	if (unlikely(folio->mapping != mapping)) {
		folio_unlock(folio);
		folio_put(folio);
		filemap_invalidate_unlock_shared(mapping);
		return ERR_PTR(-EAGAIN);
	}
	if (!folio_test_uptodate(folio)) {
		/* filemap_read_folio()'s body (static in mm/filemap.c,
		 * replayed): the filler read unlocks the folio (I/O
		 * completion owns the unlock), the killable wait
		 * follows; the lock is re-taken for the post-read
		 * re-checks.
		 */
		ret = mapping->a_ops->read_folio(ar->rfile, folio);
		if (!ret)
			ret = folio_wait_locked_killable(folio);
		if (ret || !folio_test_uptodate(folio)) {
			folio_put(folio);
			filemap_invalidate_unlock_shared(mapping);
			return ERR_PTR(ret ? : -EIO);
		}
		folio_lock(folio);
		if (unlikely(folio->mapping != mapping)) {
			folio_unlock(folio);
			folio_put(folio);
			filemap_invalidate_unlock_shared(mapping);
			return ERR_PTR(-EAGAIN);
		}
	}
	/* We must recheck i_size under page lock (filemap_fault()): the
	 * file may have been truncated since the entry gate, and a page
	 * past the new EOF is the BUS verdict, not a stale translation.
	 */
	if (unlikely(pgoff >= DIV_ROUND_UP(i_size_read(mapping->host),
					   PAGE_SIZE))) {
		folio_unlock(folio);
		folio_put(folio);
		filemap_invalidate_unlock_shared(mapping);
		return ERR_PTR(-ENODATA);
	}
	folio_unlock(folio);
	filemap_invalidate_unlock_shared(mapping);

	/* The miss statistics legacy files under VM_FAULT_MAJOR; the
	 * arena's account_fault() only knows the min shape ([P2-8]), so
	 * the major event is counted here and the fault completes without
	 * the marker -- same counters, one less vm_fault_t bit to ferry.
	 */
	return folio;
}

/*
 * V-B.3 (H5): the FILE_MAPPED read arm -- fault_once() calls it right
 * after releasing the covering desc write lock (the pagecache fetch
 * sleeps; the swapin's "owns its lock cycles" shape):
 *
 *   fetch the folio at region.rpoff + offset (lock-free phase) ->
 *   re-lock the range -> re-query -> commit PTE + file rmap +
 *   the mm_counter_file() rss family under ONE transaction.
 *
 * The metadata is NOT written: FILE_MAPPED is both the virtual
 * allocation and the resident form (the page identity is derivable
 * from the region record, so there is no __resv payload and no slot
 * transition to record).  A "the world changed while unlocked"
 * outcome (the slot demoted by the B.2 truncate gate, a racing fault
 * installing the same or a different translation) re-queries to a
 * different shape and answers -EAGAIN: the fault_once retry budget
 * re-dispatches from the query (Fig.7), so a stale fetch is never
 * committed over a moved slot.  The EOF answer (-ENODATA) is the
 * caller's CORTEN_F_BUS.
 *
 * Mirror of do_read_fault() + finish_fault() for the order-0 (or
 * large-folio subpage) clean install; the reference taken by the fetch
 * becomes the PTE's reference (folio_put on every bail-out path).
 */
static int corten_arena_file_read(struct corten_fault_ctx *ctx,
				  const struct corten_pte_meta *m)
{
	struct mm_struct *mm = ctx->mm;
	struct corten_arena *ar = ctx->ar;
	struct vm_area_struct *vma;
	struct folio *folio;
	struct corten_txn txn;
	struct corten_pte_meta m2;
	struct page *page;
	pmd_t *pmdp;
	pte_t *ptep, cur, entry;
	spinlock_t *ptl;	/* nests below the desc write lock (R2) */
	unsigned long pgoff;
	int ret;

	vma = corten_arena_get_vma(ctx);
	if (!vma || !ar->rfile)
		return -EFAULT;

	pgoff = ar->rpoff + ((ctx->addr - ar->start) >> PAGE_SHIFT);

	folio = corten_arena_file_fetch(ar, pgoff);
	if (IS_ERR(folio))
		return PTR_ERR(folio);
	page = folio_file_page(folio, pgoff);

	/* Re-lock: everything past here runs inside one transaction. */
	ret = corten_lock_range(mm, ctx->addr, PAGE_SIZE, &txn);
	if (ret) {
		ret = ret == -ENOMEM ? -ENOMEM : -EAGAIN;
		goto out_put;
	}
	if (corten_query(&txn, ctx->addr, &m2)) {
		ret = -EAGAIN;
		goto out_unlock;
	}
	if (m2.state != CORTEN_FILE_MAPPED || m2.perm != m->perm) {
		/* The slot moved while unlocked (truncate demote, a
		 * concurrent COW, mprotect): re-dispatch from the query.
		 */
		ret = -EAGAIN;
		goto out_unlock;
	}

	pmdp = corten_arena_pmd(mm, ctx->addr);
	if (!pmdp) {
		ret = -EAGAIN;
		goto out_unlock;
	}
	ptep = pte_offset_map_lock(mm, pmdp, ctx->addr, &ptl);
	if (!ptep) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	cur = ptep_get(ptep);
	if (!pte_none(cur)) {
		if (pte_present(cur) && !pte_special(cur) &&
		    pte_pfn(cur) == page_to_pfn(page)) {
			/* A racing fault (this arm re-run by the retry
			 * loop, or a concurrent GUP slow walk) committed
			 * the same translation: ours is the redundant
			 * reference.
			 */
			pte_unmap_unlock(ptep, ptl);
			corten_unlock(&txn);
			folio_put(folio);
			return 0;
		}
		/* Some other translation owns the slot (a COW copy, the
		 * zero page): re-dispatch from the query.
		 */
		pte_unmap_unlock(ptep, ptl);
		ret = -EAGAIN;
		goto out_unlock;
	}
	if (unlikely(check_stable_address_space(mm))) {
		pte_unmap_unlock(ptep, ptl);
		ret = -EAGAIN;
		goto out_unlock;
	}

	/* Clean file translation at the recorded perm (T0b: the metadata
	 * is the source of truth).  No TLB invalidate: the PTE was
	 * pte_none() above, so no CPU can hold a stale translation.
	 */
	entry = mk_pte(page, corten_arena_perm_pgprot(vma, m2.perm));
	entry = pte_sw_mkyoung(entry);
	/* V-B.4: the rss family follows mm_counter_file() -- a shmem
	 * folio is MM_SHMEMPAGES, exactly what legacy's do_read_fault()
	 * and copy_page_range() (the fork PTE copy over the carriers)
	 * account; the B.3-everything-is-FILEPAGES spelling drifted the
	 * child's counters against its own zap at fork time.
	 */
	add_mm_counter(mm, mm_counter_file(folio), 1);
	/* W1.c (R4): the file mapcount is the pagecache folio's own,
	 * carried by the novma wrapper (W1.a) inside this transaction --
	 * numerically the vma-ful call it replaces (order-0: +1 mapper,
	 * first map bumps NR_FILE_MAPPED), minus the vma terms the
	 * wrapper's contract leaves to the caller.  Order-0 only, like
	 * the wrapper: every supported pagecache shape here is order-0
	 * (tmpfs huge defaults to never; the FGP_CREAT fetch above
	 * allocates order 0), and the wrapper's VM_WARN trips loudly on
	 * anything else.
	 */
	folio_add_file_rmap_novma(folio);
	set_ptes(mm, ctx->addr, ptep, entry, 1);
	update_mmu_cache_range(NULL, vma, ctx->addr, ptep, 1);
	pte_unmap_unlock(ptep, ptl);
	corten_unlock(&txn);

	corten_arena_fault_stat(READ_ONCE(mm->corten_state),
				CORTEN_ARENA_STAT_MAPPED);
	atomic_long_inc(&corten_nr_file_read_faults);

	/* No folio_put: the fetch reference IS the PTE reference now
	 * (do_read_fault()'s accounting).
	 */
	return 0;

out_unlock:
	corten_unlock(&txn);
out_put:
	folio_put(folio);
	return ret;
}

/*
 * V-B.3 (H6): the FILE_MAPPED write arm -- a write fault on a private
 * file mapping is always a COW (MAP_PRIVATE never writes through).
 * Two shapes, both ending in the same private copy (the cow_write()
 * copy branch's file deltas: +MM_ANONPAGES/-mm_counter_file(), the
 * file rmap removal, the FILE_MAPPED->MAPPED metadata migration):
 *
 *   PTE present: the read arm's translation is in -- corten_arena_cow_write()
 *   runs under this transaction and takes its file branch.
 *
 *   PTE none: the do_cow_fault() shape -- fetch the file folio outside
 *   the lock, then copy into the fault path's speculative allocation
 *   and commit PTE + rmap + metadata in one re-locked transaction.
 *
 * EOF on the fetch is -ENODATA (the caller's CORTEN_F_BUS): legacy
 * answers a write past i_size with do_cow_fault()'s SIGBUS, not a
 * zeroed private page.
 */
static int corten_arena_file_cow(struct corten_fault_ctx *ctx,
				 const struct corten_pte_meta *m)
{
	struct mm_struct *mm = ctx->mm;
	struct corten_arena *ar = ctx->ar;
	struct vm_area_struct *vma;
	struct folio *folio;
	struct corten_txn txn;
	struct corten_pte_meta m2;
	struct page *page;
	pmd_t *pmdp;
	pte_t *ptep, cur, entry;
	spinlock_t *ptl;	/* nests below the desc write lock (R2) */
	unsigned long pgoff;
	int ret;

	vma = corten_arena_get_vma(ctx);
	if (!vma || !ar->rfile)
		return -EFAULT;

	pgoff = ar->rpoff + ((ctx->addr - ar->start) >> PAGE_SHIFT);

	/* First leg: look at the live slot under one transaction. */
	ret = corten_lock_range(mm, ctx->addr, PAGE_SIZE, &txn);
	if (ret)
		return ret == -ENOMEM ? -ENOMEM : -EAGAIN;
	if (corten_query(&txn, ctx->addr, &m2)) {
		corten_unlock(&txn);
		return -EAGAIN;
	}
	if (m2.state != CORTEN_FILE_MAPPED || m2.perm != m->perm) {
		corten_unlock(&txn);
		return -EAGAIN;
	}

	pmdp = corten_arena_pmd(mm, ctx->addr);
	if (!pmdp) {
		corten_unlock(&txn);
		return -EAGAIN;
	}
	ptep = pte_offset_map_lock(mm, pmdp, ctx->addr, &ptl);
	if (!ptep) {
		corten_unlock(&txn);
		return -EAGAIN;
	}
	cur = ptep_get(ptep);
	if (pte_present(cur) && !pte_special(cur)) {
		/* The read arm's translation: the in-place COW runs
		 * under this same transaction (cow_write() re-takes
		 * ptl itself; the m2 re-query inside its critical
		 * section is the same slot we just read).
		 */
		pte_unmap_unlock(ptep, ptl);
		ret = corten_arena_cow_write(ctx, &txn, &m2);
		corten_unlock(&txn);
		return ret;
	}
	pte_unmap_unlock(ptep, ptl);
	corten_unlock(&txn);
	if (!pte_none(cur))
		return -EAGAIN;		/* swap entry &c.: re-dispatch */

	/* Fetch outside the lock, then commit under a fresh transaction. */
	folio = corten_arena_file_fetch(ar, pgoff);
	if (IS_ERR(folio))
		return PTR_ERR(folio);
	page = folio_file_page(folio, pgoff);

	if (!ctx->folio) {
		/* The prealloc was consumed by a lost race: retry
		 * re-arms it (the fault_once epilogue).
		 */
		folio_put(folio);
		return -EAGAIN;
	}

	ret = corten_lock_range(mm, ctx->addr, PAGE_SIZE, &txn);
	if (ret) {
		ret = ret == -ENOMEM ? -ENOMEM : -EAGAIN;
		goto out_put;
	}
	if (corten_query(&txn, ctx->addr, &m2)) {
		ret = -EAGAIN;
		goto out_unlock;
	}
	if (m2.state != CORTEN_FILE_MAPPED || m2.perm != m->perm) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	pmdp = corten_arena_pmd(mm, ctx->addr);
	if (!pmdp) {
		ret = -EAGAIN;
		goto out_unlock;
	}
	ptep = pte_offset_map_lock(mm, pmdp, ctx->addr, &ptl);
	if (!ptep) {
		ret = -EAGAIN;
		goto out_unlock;
	}
	cur = ptep_get(ptep);
	if (!pte_none(cur)) {
		/* A racing read fault committed the translation: its
		 * pagecache folio is in, re-dispatch takes the
		 * in-place COW above.
		 */
		pte_unmap_unlock(ptep, ptl);
		ret = -EAGAIN;
		goto out_unlock;
	}
	if (unlikely(check_stable_address_space(mm))) {
		pte_unmap_unlock(ptep, ptl);
		ret = -EAGAIN;
		goto out_unlock;
	}

	/* do_cow_fault()'s body: copy the file contents into the private
	 * folio under ptl (the folio stays alive on the fetch reference
	 * until the put below).  No break-before-make flush: the PTE was
	 * pte_none(), so no CPU can hold a cached translation.
	 */
	copy_user_highpage(folio_page(ctx->folio, 0), page, ctx->addr, vma);
	__folio_mark_uptodate(ctx->folio);

	entry = folio_mk_pte(ctx->folio,
			     corten_arena_perm_pgprot(vma, m2.perm));
	entry = pte_mkyoung(entry);
	entry = corten_pte_mkwrite(pte_mkdirty(entry), vma);

	add_mm_counter(mm, MM_ANONPAGES, 1);
	folio_add_new_anon_rmap(ctx->folio, vma, ctx->addr, RMAP_EXCLUSIVE);
	set_ptes(mm, ctx->addr, ptep, entry, 1);
	update_mmu_cache_range(NULL, vma, ctx->addr, ptep, 1);
	pte_unmap_unlock(ptep, ptl);

	/* FILE_MAPPED -> MAPPED ("any -> MAPPED" is the protocol's legal
	 * COW edge); corten_map() scrubs the flags -- SHARED is gone with
	 * the shared folio, which is exactly the private copy's
	 * semantics.
	 */
	ret = corten_map(&txn, ctx->addr, folio_page(ctx->folio, 0),
			 m2.perm, CORTEN_MAP_FORCE);
	if (WARN_ON_ONCE(ret)) {
		/* The PTE is committed and the counters moved; the
		 * metadata still says FILE_MAPPED -- impossible (the
		 * slot carries the FILE pre-mark or the FRESH
		 * synthesis), the next fault's re-query repairs the
		 * pair.  The speculative reference already became the
		 * PTE reference (no put of ctx->folio).
		 */
		corten_unlock(&txn);
		folio_put(folio);
		return -EFAULT;
	}

	corten_arena_fault_stat(READ_ONCE(mm->corten_state),
				CORTEN_ARENA_STAT_COW_COPY);
	atomic_long_inc(&corten_nr_file_cow_copies);
	corten_unlock(&txn);

	folio_put(folio);		/* the fetch reference */
	ctx->folio = NULL;		/* became the PTE reference */
	return 0;

out_unlock:
	corten_unlock(&txn);
out_put:
	folio_put(folio);
	return ret;
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

		/* V-B.3 (H4, the rclass-aware synthesis): inside a FILE
		 * region an Invalid slot is a truncate/invalidation-
		 * demoted FILE_MAPPED page (the B.2 gate's KEEP_PERM
		 * drop).  The synthesis must re-arm FILE_MAPPED, never
		 * PrivateAnon -- the anon shape would present anonymous
		 * zero bytes where the file's (new) content belongs,
		 * the silent corruption the whole FILE pre-mark exists
		 * to prevent.  The dispatch below then takes the read
		 * arm (re-read) or the COW arm (fetch + copy); a slot
		 * past the new EOF answers BUS on the fetch.
		 */
		if (READ_ONCE(ctx->ar->rclass) == CORTEN_REGION_FILE)
			fresh.state = CORTEN_FILE_MAPPED;

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

	/* M5.T2' (sec 4.2, OQ-4): the slow gate's UNSHARE write (the GUP
	 * read-PIN pre-break of a fork-shared page) is ctx->write without
	 * the USER bit, exactly like every other kernel-path writer --
	 * dispatch sends the shared shapes through the COW transaction
	 * below.  [T3] There is deliberately no FOLL_FORCE redirect here
	 * any more: the fault flags cannot tell a FOLL_FORCE poke from a
	 * plain GUP write pin, and a forced copy of a page whose recorded
	 * perm lacks WRITE arms read-only -- a survivor only a FOLL_FORCE
	 * caller can re-follow (can_follow_write_common()).  A plain
	 * writer (pread/O_DIRECT/io_uring zero-copy) would fault, copy
	 * and retry forever, so every external write against a
	 * read-only contract answers ACCERR -- the loud EFAULT the
	 * routed-RO contract ruling (T2' residue 2) chose.
	 */

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
		/* V-B.3 (H6): a FILE_MAPPED write takes the file COW arm,
		 * which owns its lock cycles (the first write may need the
		 * pagecache fetch -- the do_cow_fault() shape -- and the
		 * fetch sleeps, INV3).  The anon shape runs cow_write()
		 * under this transaction as before.
		 */
		if (m.state == CORTEN_FILE_MAPPED) {
			ctx->fileio = true;
			corten_unlock(&txn);
			ret = corten_arena_file_cow(ctx, &m);
			if (ret == -ENODATA || ret == -EIO)
				return CORTEN_F_BUS;
			if (ret == -EFAULT)
				return CORTEN_F_MAPERR;
			break;
		}
		ret = corten_arena_cow_write(ctx, &txn, &m);
		break;
	case CORTEN_DISP_FILE_READ:
		/* V-B.3 (H5): the read arm owns its lock cycles -- the
		 * pagecache fetch sleeps (folio lock, ->read_folio,
		 * INV3), exactly the swapin shape.  -EFAULT is the loud
		 * broken-pair answer (MAPERR); -ENODATA (past EOF) and
		 * -EIO (failed read) are the BUS verdicts
		 * (filemap_fault()'s SIGBUS answers).
		 */
		ctx->fileio = true;
		corten_unlock(&txn);
		ret = corten_arena_file_read(ctx, &m);
		if (ret == -ENODATA || ret == -EIO)
			return CORTEN_F_BUS;
		if (ret == -EFAULT)
			return CORTEN_F_MAPERR;
		break;
	case CORTEN_DISP_SWAPIN:
		/* M6.T2 (spec D5): the swap-in owns its lock cycles --
		 * it releases @txn itself (I/O sleeps) and re-locks
		 * inside.  -EFAULT is the loud broken-pair answer
		 * (MAPERR), not a legacy fallback: falling back on a
		 * Swapped slot would let the legacy funnel install a
		 * zero page over swapped-out content.
		 */
		ctx->swapin = true;
		corten_unlock(&txn);
		ret = corten_arena_swap_in(ctx, &m);
		if (ret == -EFAULT)
			return CORTEN_F_MAPERR;
		break;
	case CORTEN_DISP_COW_COPY:
		/* T1a: a write against a shared page whose contract is
		 * read-only is a genuine permission fault (the page was
		 * RO before the fork -- the wrprotect changed nothing).
		 * The STUB-era WARN is gone: this is a normal, legal
		 * outcome.  It answers SIGSEGV for every writer, kernel
		 * path included -- see the [T3] note above the dispatch.
		 */
		corten_unlock(&txn);
		return CORTEN_F_ACCERR;
	case CORTEN_DISP_ACCERR:
		corten_unlock(&txn);
		return CORTEN_F_ACCERR;
	case CORTEN_DISP_STUB:
		/* M4+ shared-anon: M3 refuses loudly. */
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

		/* [FAIL-2] cached descriptor pointer, no maple walk.
		 * V-A.1: NULL is legal (an A.1-reactivated window
		 * predating its carrier) -- the preallocation has a
		 * VMA-free form.  V-A.2b: an auto arena's carrier takes
		 * the vma arm (policy/pin shapes identical).
		 */
		ctx.vma = corten_arena_anchor_vma(ar);
		folio = corten_arena_folio_prealloc(mm, ctx.vma, ctx.addr);
		if (!folio) {
			st = CORTEN_F_OOM;
			goto out;
		}
		ctx.folio = folio;
	}

	for (;;) {
		st = corten_arena_fault_once(&ctx);
		if (st != CORTEN_F_RETRY ||
		    ++tries >= (ctx.swapin || ctx.fileio ? 4 : 2))
			break;	/* Fig.7 retry cap, x86 fault.c:1408 style;
				 * the swap-in shape gets 2+2 (spec D5):
				 * its unlocked re-validation spends one
				 * extra round per lost race (counted:
				 * swapin_retries); V-B.3's file arms get
				 * the same 2+2 (the fetch's unlocked
				 * re-validation vs a racing truncate)
				 */

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
	case CORTEN_F_BUS:
		/* V-B.3: the beyond-EOF verdict (filemap_fault()'s
		 * SIGBUS shape, delivered through the fast hook).
		 */
		corten_arena_account_fault(mm, regs, ctx.addr, false);
		return CORTEN_FAULT_BUS;
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
	/* OQ-4 (M5_FORK_SPEC.md sec 4.2): a GUP read-PIN of a fork-shared
	 * page (gup_must_unshare() -> -EMLINK) faults in with
	 * FAULT_FLAG_UNSHARE and no write bit.  Upstream answers it as a
	 * plain COW write (do_wp_page()); dispatching it as a read would
	 * re-arm the read-only translation and spin the GUP retry against
	 * the next -EMLINK.  FAULT_FLAG_WRITE|UNSHARE never co-exist
	 * (sanitize_fault_flags() VM_WARNs), so the mapping is total.
	 */
	/* OQ-4 mapping (above) turns the unshare into a write; ctx.unshare
	 * remembers the origin so a denied read-pin unshare can fall back
	 * to the legacy body (see the [T3] note at the returns).
	 */
	if (flags & FAULT_FLAG_UNSHARE) {
		ctx.unshare = true;
		ctx.write = true;
	}
	/* [T3] A non-USER write fault (GUP pin, ptrace, get_user) takes
	 * the same metadata verdicts as the process's own faults: a
	 * read-only contract denies it with ACCERR (see the fault_once
	 * note), a writable contract restores or copies re-followably.
	 * The former FOLL_FORCE forced-write redirect is gone -- its
	 * read-only survivor was only followable by FOLL_FORCE callers
	 * and livelocked the plain ones.
	 */
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
			if (st != CORTEN_F_RETRY ||
			    ++tries >= (ctx.swapin || ctx.fileio ? 4 : 2))
				break;	/* same swap-in budget as above */
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
		/* [T3] A read-pin unshare (gup_must_unshare() -> -EMLINK
		 * -> FAULT_FLAG_UNSHARE) of a page whose recorded perm
		 * lacks WRITE: the COW dispatch denies the synthesized
		 * write, but the operation only asks to break folio
		 * sharing at the recorded perm.  The legacy body owns
		 * exactly that on the shadow-VMA (do_wp_page() unshare,
		 * no mkwrite) -- hand it over instead of denying a read.
		 */
		if (ctx.unshare)
			return CORTEN_FAULT_FALLBACK_BIT | VM_FAULT_FALLBACK;
		return VM_FAULT_SIGSEGV;
	case CORTEN_F_MAPERR:
		return VM_FAULT_SIGSEGV;
	case CORTEN_F_BUS:
		/* V-B.3: beyond EOF -- the legacy VM_FAULT_SIGBUS verdict
		 * (S-FILE-1: same semantics, new delivery path).
		 */
		return VM_FAULT_SIGBUS;
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
 * Driver-owned state of one window's zap ([atomic-sleep fix]).  The lazy
 * gather ([perf1], valid iff @have_tlb) and the walk cursor with its batch
 * overflow flag live here, not in corten_arena_zap_window(), because the
 * driver must drop the covering desc write lock before every flush and
 * before the final finish: tlb_flush_mmu()/tlb_finish_mmu() free batched
 * pages through sleeping paths and may not run inside the write_lock_bh
 * critical section.  All flushes and the finish therefore happen at the
 * driver, outside the lock; see corten_arena_zap_window().
 */
struct corten_zap_win {
	struct mmu_gather tlb;
	bool have_tlb;
	unsigned long addr;	/* in: walk resume point; out: stop on force */
	bool force;		/* out: batch overflowed -- flush, then resume */
};

/*
 * Zap the PTEs of the recorded pages in [start, end) (at most one PMD
 * window; the caller holds the covering desc write lock obtained through
 * @txn) and flip the recorded metadata back to CORTEN_INVALID.  Page
 * references are handed to @tlb, so every folio_put() happens after the
 * TLB flush -- the same ordering guarantee unmap_vmas() relies on
 * (sec 5.5 boundary argument: a re-map of the same VA during the window
 * only produces a harmless extra fault, the old page's reference is
 * dropped after the flush).
 *
 * M4.T2 batching: the walk takes the PTE lock once for the whole window
 * (one PT page) instead of per PTE, and the TLB invalidation is left
 * entirely to the mmu_gather -- tlb_finish_mmu() (or a mid-batch
 * tlb_flush_mmu()) issues exactly one ranged flush over the recorded
 * span.  The previous explicit flush_tlb_range() here duplicated the
 * gather's flush, doubling the shootdown cost of every content-bearing
 * unmap (the r06-m4t12 diagnosis measured the flush/IPI block at more
 * than half of the MODE-arm samples on the unmap shapes).
 *
 * [perf1] The gather is owned by the window itself (opened only when the
 * window carries a translation), so the callers hold no gather across
 * windows and a PTE-less window costs no tlb_flush_pending traffic at all.
 *
 * [atomic-sleep fix] The gather is never finished -- and never flushed --
 * inside this function: tlb_flush_mmu()/tlb_finish_mmu() free the batched
 * pages through paths that sleep (__tlb_batch_free_encoded_pages(),
 * mm/mmu_gather.c), which may not run inside the covering desc write lock
 * (a write_lock_bh critical section).  The walk instead reports a batch
 * overflow through @zw->force with @zw->addr parked on the overflow page,
 * and the driver drops the desc write lock before its tlb_flush_mmu() and
 * finishes the lazy gather after the last round (same placement pattern as
 * the park's post-downgrade finish, [perf1c]).  The lock is only ever
 * dropped at a round boundary, where every cleared PTE is already written
 * and each released folio reference sits in the gather -- flush ordering
 * (after the PTE clear, before the folio put) stays inside the mmu_gather.
 *
 * @zflags goes to corten_unmap() verbatim: the live-arena chunk zap uses
 * CORTEN_UNMAP_KEEP_PERM (the committed mprotect contract survives the
 * content drop), while the T1c park zap passes 0 -- a munmapped arena's
 * permission commitments die with the mapping, and the pooled window must
 * come back pristine (perm-0 Invalid slots) for its next incarnation.
 *
 * R6-2 (M6_RMAP_SPEC.md sec 5, registered not fixed): this zap, unlike
 * the mprotect route (corten_arena_protect_window()), runs without an
 * mmu_notifier invalidation window, so secondary-MMU users (KVM,
 * process-scoped notifiers) are not told about the PTE removals.  M6.T1
 * does not touch that exposure -- the guard refusal arms it adds write
 * nothing and start no notifier traffic of their own -- but the T2
 * swap-out transaction must not copy this shape: its walker-side entry
 * runs inside try_to_unmap_one()'s existing invalidate window, and the
 * shrinker side will have to open one.  Guest exposure today is nil
 * (no secondary-MMU user maps arena ranges), recorded as a Stage-2 item.
 */
/*
 * The zap-side page release: rmap removal plus the anon/file counter
 * split.  V-A.1: @vma is NULL for a VMA-less arena, whose pages carry
 * no rmap anchor (see corten_arena_map_anon()) -- the removal must not
 * run for them or the mapcount underflows, and the folio is not flagged
 * anon, so the window's vma-ness owns the counter split.  W1.c (R7):
 * the *file* family's rmap is the pagecache folio's own mapcount (the
 * read arm's novma install), so its removal is vma-free too and the
 * folio family, not the window's vma-ness, keys the arm; the anon
 * family keeps the carrier-anchored call (its flip is W-2's).
 */
static void corten_zap_release_page(struct mm_struct *mm,
				    struct vm_area_struct *vma,
				    struct page *page, unsigned long addr)
{
	struct folio *folio = page_folio(page);
	bool anon = folio_test_anon(folio);

	/* V-A.1: a VMA-less arena's pages carry no rmap anchor (see
	 * corten_arena_map_anon()) -- the removal must not run for them
	 * or the mapcount underflows, and the folio is not flagged anon,
	 * so the window's vma-ness owns the counter split.  W1.c (R7):
	 * the file family removes through the novma wrapper (symmetric
	 * with the read arm's install); the anon family stays on the
	 * vma-anchored call.
	 */
	if (vma) {
		if (anon)
			folio_remove_rmap_pte(folio, page, vma);
		else
			folio_remove_file_rmap_novma(folio);
	}
	/* V-B.4: the file family is mm_counter_file()'s (shmem-backed
	 * folios are MM_SHMEMPAGES), symmetric with the read arm's
	 * install and upstream's copy_page_range() at fork.
	 */
	if (!vma || anon)
		add_mm_counter(mm, MM_ANONPAGES, -1);
	else
		add_mm_counter(mm, mm_counter_file(folio), -1);
}

/*
 * Untracked-walk arm: release one present non-special PTE's page and
 * queue it on @tlb; returns the batch-overflow flag.
 */
static bool corten_zap_drop_present_page(struct mm_struct *mm,
					 struct vm_area_struct *vma,
					 pte_t oldpte, unsigned long addr,
					 struct mmu_gather *tlb)
{
	struct page *page = pte_page(oldpte);

	corten_zap_release_page(mm, vma, page, addr);
	return __tlb_remove_page_size(tlb, page, false, PAGE_SIZE);
}

static int corten_arena_zap_window(struct mm_struct *mm,
				   struct vm_area_struct *vma,
				   struct corten_txn *txn,
				   struct corten_zap_win *zw,
				   unsigned long end,
				   struct mmu_gather *tlb, u8 zflags)
{
	unsigned long addr = zw->addr;
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);
	struct mmu_gather *g = tlb;
	pmd_t *pmdp;
	int ret = 0;

	pmdp = corten_arena_pmd(mm, addr);
	if (WARN_ON_ONCE(!pmdp))
		return -EAGAIN;

	/*
	 * [perf1] Lazy per-window gather.  The walk now decides under the
	 * PTE lock whether the window holds any translation at all before
	 * opening its mmu_gather.  A window of never-touched pages (the
	 * unmap-virt shape: every refill only recorded metadata) has
	 * nothing to flush, but the old route-level gather was opened for
	 * it anyway -- and tlb_finish_mmu() force-upgrades any gather to a
	 * full-mm shootdown when a second gather on this mm overlaps
	 * (mm_tlb_flush_nested()), which turned the PTE-less chunk churn
	 * into a permanent remote-IPI storm (ftrace tlb_flush: ~2 full-mm
	 * flushes per op vs ~0 legacy, r06/perf1).  Metadata-only drops
	 * now run with no gather at all; windows with real content behave
	 * exactly as before, with the gather finished (and the released
	 * folios flushed) before the covering transaction is dropped.
	 *
	 * @tlb (caller-owned, already gathered) skips the lazy logic: every
	 * non-none PTE is queued into it and the caller owns the finish.
	 * [perf1c] The T1c park passes its route-level gather this way so
	 * its flush can run after the write->read downgrade, outside the
	 * serialized write section -- the same placement
	 * vms_complete_munmap_vmas() uses for the legacy munmap.  The park's
	 * mid-walk batch overflows come back through @zw->force like every
	 * other round: the driver (corten_arena_unmap_chunk_flags()) flushes
	 * the caller-owned gather after dropping the desc write lock, and
	 * the pool keeps its post-downgrade finish.
	 *
	 * Between rounds (@zw->force) the desc write lock is dropped, so a
	 * fault can refill an already-zapped VA before the walk resumes.
	 * That refill is a transactional FRESH fault (PTE + metadata under
	 * the desc write lock), i.e. indistinguishable from a fault landing
	 * one instruction after the munmap/DONTNEED returned -- the same
	 * shape the legacy munmap/DONTNEED of a kept-VA arena admits.  The
	 * driver's flush can therefore also shoot down such a refill's
	 * translation; a spurious flush is always safe (the PTE is live,
	 * the next access re-walks).  The unprocessed tail of the window is
	 * re-scanned by VA on resume, so a refill there is zapped by the
	 * later round as intended.
	 *
	 * tlb_gather_mmu() itself stays under the desc write lock: it is a
	 * plain initializer plus an atomic pending counter, no sleeping
	 * path.
	 *
	 * The scan is race-free against every PTE producer: the covering
	 * desc write lock excludes the fault paths and the other
	 * transactional zaps, and a tracked window cannot be served by the
	 * legacy funnel.
	 *
	 * The walk itself still decides by PTE content, not metadata: a
	 * page can have a PTE with no (or INVALID) metadata behind it --
	 * the legacy fallback body writes PTEs on a shadow-VMA when the
	 * transaction layer hands a fault over -- and trusting the
	 * metadata there leaves the old translation live through the
	 * munmap (r03 defect C: the arena_stress zerocheck read the
	 * previous cycle's magic).  Any !none PTE in the window routes the
	 * walk through the clearing path above; the metadata-only branch
	 * runs only when the scan proved every PTE is none, where a
	 * translation to leave live cannot exist.  Every producer of an
	 * in-arena PTE (arena map/zero page, legacy fault, GUP) attaches a
	 * private anonymous page with rmap, so the present-and-not-special
	 * release below is correct for all of them; the shared zero page
	 * is pte_special()d and owns nothing.
	 */
	for (;;) {
		bool force = false;
		bool any_pte = false;
		pte_t *ptep, *scan;
		unsigned long a;
		spinlock_t *ptl;

		ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
		if (!ptep) {
			ret = -EAGAIN;
			goto out;
		}

		if (!tlb) {
			for (scan = ptep, a = addr; a < end;
			     a += PAGE_SIZE, scan++) {
				if (!pte_none(ptep_get(scan))) {
					any_pte = true;
					break;
				}
			}

			if (any_pte && !zw->have_tlb) {
				/* Atomic-safe initializer (plain stores plus
				 * an atomic pending counter) -- legal inside
				 * the desc write lock, unlike the flushes.
				 */
				tlb_gather_mmu(&zw->tlb, mm);
				zw->have_tlb = true;
			}
			g = zw->have_tlb ? &zw->tlb : NULL;
		}

		for (; addr < end; addr += PAGE_SIZE, ptep++) {
			struct corten_pte_meta *slot;
			struct page *page;
			pte_t oldpte;
			bool recorded;

			/* [perf2a] One pointer read per slot finds the
			 * recorded minority (a NULL array is the pristine
			 * nothing-ever-recorded answer); only those slots
			 * pay the corten_unmap() transition.  The reset is
			 * bounded by the walked range, and the array
			 * survives the park -- a 16KB pool arena stopped
			 * paying the [perf1b] wholesale drop's O(PT page)
			 * count loop plus the kfree/kmalloc+memset churn
			 * per take/park pair.  The pristine contract is
			 * unchanged: every recorded slot in the walk leaves
			 * here Invalid/perm-0, and slots outside an arena's
			 * own range were never recorded (arenas are
			 * PMD-aligned: exclusive owners of their PT pages).
			 */
			slot = corten_txn_slot(txn, addr);
			recorded = !IS_ERR(slot) && slot &&
				   slot->state != CORTEN_INVALID;

			/* V-B.3 (H7 follow-through): the file-event
			 * invalidation family (!even_cows) spares the
			 * private copies -- a MAPPED (COWed) or SWAPPED
			 * slot's content is not the file's business
			 * anymore (should_zap_cows()'s verdict, which
			 * legacy also extends to the swapped COW
			 * entries).  Everything else in a FILE region --
			 * the FILE_MAPPED virtual allocation, PTEs
			 * without a private record -- stays the event's
			 * business.
			 */
			if ((zflags & CORTEN_UNMAP_FILE_EVENT) && slot &&
			    !IS_ERR(slot) &&
			    (slot->state == CORTEN_MAPPED ||
			     slot->state == CORTEN_SWAPPED))
				continue;

			if (!g) {
				/* The scan proved the window carries no
				 * translation: only the recorded-metadata
				 * reset remains.  No PTE store, no TLB
				 * entry, no folio references.
				 */
				if (recorded) {
					ret = corten_unmap(txn, addr,
							   PAGE_SIZE,
							   zflags & CORTEN_UNMAP_ALL);
					if (WARN_ON_ONCE(ret))
						break;

					this_cpu_inc(state->stats[CORTEN_ARENA_STAT_UNMAP_PAGES]);
				}
				continue;
			}

			oldpte = ptep_get_and_clear(mm, addr, ptep);
			if (!pte_none(oldpte))
				tlb_remove_tlb_entry(g, ptep, addr);

			if (!pte_none(oldpte)) {
				/* Zero-page entries are pte_special()d and
				 * not ours to release.
				 */
				if (pte_present(oldpte) &&
				    !pte_special(oldpte)) {
					page = pte_page(oldpte);

					/* [T3] Observability only: a pinned
					 * folio survives the zap on its pin
					 * reference (the batch flush below
					 * drops the PTE reference, the pin
					 * carries the folio to unpin --
					 * M5_FORK_SPEC.md sec 4.3, INV8).
					 */
					if (folio_maybe_dma_pinned(page_folio(page)))
						corten_arena_note_zap_pinned();

					/* [F-B] Every producer of an
					 * in-arena PTE in a *tracked*
					 * window (arena map/zero page,
					 * legacy fault, GUP) attaches a
					 * private anonymous page; the
					 * rmap-less shapes are
					 * corten_zap_release_page()'s
					 * business.
					 */
					corten_zap_release_page(mm, vma, page,
								addr);
					force = __tlb_remove_page_size(g, page,
								       false,
								       PAGE_SIZE);
					if (force)
						break;
				} else if (!pte_present(oldpte)) {
					/* M6.T2 (spec D7): a swap entry
					 * the swap-out transaction
					 * installed.  Release it --
					 * free_swap_and_cache() also
					 * reclaims a no-longer-shared
					 * cache copy (R6-4: the
					 * upstream primitive owns the
					 * count symmetry) -- and drop
					 * the SWAPENTS side; the
					 * metadata reset below scrubs
					 * the __resv payload.  Non-swap
					 * !present entries (migration/
					 * hwpoison markers) cannot be
					 * produced on a shadow-VMA --
					 * the ttu guard refuses those
					 * shapes -- so warn loudly.
					 */
					swp_entry_t sentry;

					sentry = pte_to_swp_entry(oldpte);
					if (!non_swap_entry(sentry)) {
						free_swap_and_cache(sentry);
						add_mm_counter(mm,
							       MM_SWAPENTS,
							       -1);
						atomic_long_inc(&corten_nr_zap_swap_frees);
					} else {
						WARN_ON_ONCE(1);
					}
				}
			}

			if (recorded) {
				/* KEEP_PERM: the content drop must not
				 * dissolve the mprotect contract committed
				 * on the VA.  The slot comes back Invalid
				 * but keeps the recorded perm, so the
				 * FRESH fault gate re-derives the
				 * committed permission
				 * (zero-fill-on-demand semantics, like
				 * the legacy MADV_DONTNEED/munmap of a
				 * committed chunk) instead of the
				 * DECLARE-time arena bound -- a PROT_NONE
				 * reservation with routed RW commits died
				 * with SEGV_ACCERR on the next write here
				 * (the r06 dedup_eq "rogue" ACCERR
				 * family).
				 */
				ret = corten_unmap(txn, addr, PAGE_SIZE,
						   zflags & CORTEN_UNMAP_ALL);
				if (WARN_ON_ONCE(ret))
					break;

				this_cpu_inc(state->stats[CORTEN_ARENA_STAT_UNMAP_PAGES]);
			}
		}

		pte_unmap_unlock(ptep, ptl);

		if (!force)
			break;
		/* Batch overflow (MMU_GATHER_BUNDLE): hand the flush to the
		 * driver.  [atomic-sleep fix] tlb_flush_mmu() frees the
		 * batched pages through sleeping paths and must not run
		 * inside the desc write lock; the driver drops the lock,
		 * flushes, re-locks and resumes at the overflow page (its
		 * PTE is already cleared; the resumed round only re-runs its
		 * metadata reset -- the page reference itself was queued and
		 * is flushed + freed by the driver's flush).  This is the
		 * zap_pte_range() force_flush convention, which the previous
		 * code silently ignored: an allocation failure in
		 * tlb_next_batch() would have overflowed the batch array.
		 */
		zw->addr = addr;
		zw->force = true;
		goto out;
	}

out:
	/* [atomic-sleep fix] No tlb_finish_mmu() here: the lazy gather stays
	 * open across rounds and the driver finishes it -- outside the desc
	 * write lock -- once this function reports the walk done (or failed).
	 */

	/* [perf2a] The per-slot reset inside the walk provides the pristine
	 * contract on every fully-successful pass; there is no wholesale
	 * array drop any more (see the slot loop above).  A walk that bailed
	 * mid-window can leave live PTEs, and a live PTE without metadata
	 * behind it is the r03 defect C shape -- the caller's error path
	 * (real RELEASE for the park) tears the window down completely
	 * instead, exactly as the [perf1b] drop's success-gate did.
	 */

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
 * M4.T2 batching as there: one PTE-lock section, with the gather owned
 * by the window ([perf1] and opened only when a translation exists).
 */
static int corten_arena_zap_untracked_window(struct mm_struct *mm,
					     struct vm_area_struct *vma,
					     unsigned long start,
					     unsigned long end)
{
	unsigned long addr = start;
	struct mmu_gather tlb;
	bool have_tlb = false;
	bool drift = false;
	pmd_t *pmdp;
	int ret = 0;

	pmdp = corten_arena_pmd(mm, start);
	if (!pmdp || !pmd_present(READ_ONCE(*pmdp)))
		return 0;		/* nothing was ever mapped here */
	if (pmd_leaf(READ_ONCE(*pmdp)))
		return -EOPNOTSUPP;	/* THP: not ours in M3 */

	for (;;) {
		bool force = false;
		bool any_pte = false;
		pte_t *ptep, *scan;
		unsigned long a;
		spinlock_t *ptl;

		ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
		if (!ptep) {
			ret = -EAGAIN;
			goto out;
		}

		/* [perf1] Same lazy-gather contract as the tracked zap:
		 * decide by PTE content whether anything needs flushing
		 * before opening the gather (and before clearing).  This
		 * window has no transaction, so the scan does not exclude
		 * concurrent producers by itself -- but a concurrent
		 * producer that appears after the scan said "none" either
		 * writes none PTEs (faults here are diverted to the arena
		 * transaction layer) or takes the same PTE lock.
		 */
		for (scan = ptep, a = addr; a < end; a += PAGE_SIZE, scan++) {
			if (!pte_none(ptep_get(scan))) {
				any_pte = true;
				break;
			}
		}

		if (any_pte && !have_tlb) {
			tlb_gather_mmu(&tlb, mm);
			have_tlb = true;
		}

		if (!have_tlb) {
			pte_unmap_unlock(ptep, ptl);
			break;		/* nothing was ever mapped here */
		}

		for (; addr < end; addr += PAGE_SIZE, ptep++) {
			pte_t oldpte;

			oldpte = ptep_get_and_clear(mm, addr, ptep);
			if (!pte_none(oldpte))
				tlb_remove_tlb_entry(&tlb, ptep, addr);

			if (!pte_none(oldpte)) {
				drift = true;

				if (pte_present(oldpte) &&
				    !pte_special(oldpte)) {
					force = corten_zap_drop_present_page(mm,
									     vma,
									     oldpte,
									     addr,
									     &tlb);
					if (force)
						break;
				}
			}
		}

		pte_unmap_unlock(ptep, ptl);

		if (!force)
			break;
		tlb_flush_mmu(&tlb);
		cond_resched();
	}

out:
	if (have_tlb)
		tlb_finish_mmu(&tlb);

	if (drift)
		corten_legacy_drift_inc();

	return ret;
}

/* The chunk-zap driver; @zflags selects the metadata-reset semantics
 * (see corten_arena_zap_window()).  @tlb: caller-owned gather already
 * initialized with tlb_gather_mmu(), kept open across the whole walk and
 * finished by the caller ([perf1c]: the T1c park flushes after its
 * write->read downgrade); NULL makes every window own a lazy gather.
 * Static: only the chunk routes and the T1c park reach it.
 *
 * [atomic-sleep fix] Every flush and the per-window lazy-gather finish
 * run here, outside the desc write lock: each window is processed in
 * rounds -- zap under the covering write lock, drop it for the
 * tlb_flush_mmu() a batch overflow demands, re-lock, resume -- and the
 * lazy gather is finished only after the last round dropped the lock
 * (the write_lock_bh critical section may not sleep; batched page
 * freeing does).
 */
static int corten_arena_unmap_chunk_flags(struct mm_struct *mm,
					  struct corten_arena *ar,
					  unsigned long start, unsigned long len,
					  u8 zflags, struct mmu_gather *tlb)
{
	unsigned long end = start + len;
	struct vm_area_struct *vma;
	int ret = 0;

	/* V-A.1: the anchor is optional -- a parked window has none live
	 * and the zap arms' NULL arm keeps them on the pure-metadata
	 * path.  V-A.2b: an auto arena's pages are rmap-anchored on its
	 * carrier, so the zap's rmap removal runs against the carrier and
	 * the mapcount symmetry of the shadow-VMA era holds.
	 */
	vma = corten_arena_anchor_vma(ar);

	while (start < end) {
		unsigned long win_end = min((start | (PMD_SIZE - 1)) + 1, end);
		struct corten_zap_win zw = { .addr = start, };
		struct corten_txn txn;
		bool tracked = false;
		int tries;

		for (;;) {
			zw.force = false;
			tries = 0;
			for (;;) {
				ret = corten_lock_range(mm, start,
							win_end - start,
							&txn);
				if (ret != -EAGAIN || ++tries >= 2)
					break;
			}

			switch (ret) {
			case 0:
				tracked = true;
				ret = corten_arena_zap_window(mm, vma, &txn,
							      &zw, win_end,
							      tlb, zflags);
				corten_unlock(&txn);
				/* [atomic-sleep fix] flush after the lock
				 * drop, before the walk resumes.
				 */
				if (zw.force)
					tlb_flush_mmu(tlb ? tlb : &zw.tlb);
				if (!ret && zw.force)
					continue;	/* resume the walk */
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
				 * No desc write lock is held on this path, so
				 * the helper's own flush/finish are already
				 * outside it.
				 */
				ret = corten_arena_zap_untracked_window(mm, vma,
									start, win_end);
				break;
			case -EAGAIN:
				/* Exhausted retries: PT page retiring; nothing may
				 * be zapped through the legacy funnel while the
				 * descriptor is stale.
				 */
				fallthrough;
			default:
				ret = ret == -EOPNOTSUPP ? -EOPNOTSUPP : -EAGAIN;
				break;
			}

			break;
		}

		/* [atomic-sleep fix] The window's lazy gather is finished
		 * here -- every round above dropped the desc write lock
		 * before reaching this point.
		 */
		if (!tlb && zw.have_tlb)
			tlb_finish_mmu(&zw.tlb);

		if (ret)
			return ret;
		if (tracked)
			this_cpu_inc(READ_ONCE(mm->corten_state)->stats[
					CORTEN_ARENA_STAT_MUNMAP_TXNS]);

		start = win_end;
	}

	return ret;
}

/* Non-static: mm/corten_fault_test.c drives the chunk zap directly. */
int corten_arena_unmap_chunk(struct mm_struct *mm, struct corten_arena *ar,
			     unsigned long start, unsigned long len)
{
	/* The live-arena content drop keeps the recorded permission (the
	 * committed mprotect contract survives a chunk munmap / DONTNEED).
	 */
	return corten_arena_unmap_chunk_flags(mm, ar, start, len,
					      CORTEN_UNMAP_KEEP_PERM, NULL);
}

/* ------------------------------------------------------------------ *
 * T1c: the resident arena pool (M4T12_T1C).
 *
 * In a MODE process every mmap(NULL) anonymous mapping is one whole
 * arena lifecycle: DECLARE (~ceremony) + RELEASE (~teardown) -- the
 * r07-m4t12 diagnosis priced that at ~12.6us per 16KB op, all of it
 * under mmap_write.  The pool removes the lifecycle from the churn: a
 * full-coverage munmap of a MODE arena *parks* it (transactional zap
 * with a full metadata reset, then a reserved PROT_NONE anonymous VMA
 * and inert registry slots) and the next auto mmap of the same
 * PMD-rounded size *reactivates* the parked arena in place -- no
 * descriptor allocation, no percpu_ref kill/reinit, warm page tables and
 * xarray nodes.
 *
 * Parked-state invariants (all under the owner mm's mmap_write; flag
 * reads on the hot paths are READ_ONCE against those writers):
 *   - @idle is the single source of truth: set only after the content
 *     zap completed, cleared only after the reactivation republished the
 *     cached shadow-VMA ([FAIL-2] order).  corten_arena_lookup() gates
 *     on it, so every fault/route path treats the range as if unmapped.
 *   - The registry slots keep pointing at the descriptor (zero xarray
 *     churn) while the shadow-VMA cache is NULL; the magazine's obstacle
 *     walks and the global placer see the slots and stay off the range.
 *   - The percpu_ref stays live for the whole park: no drain, no
 *     grace-period accounting, and reactivation is a flag flip.
 *   - state->nr counts only live arenas (park decrements,
 *     reactivation increments), so the nr==0 fast negation covers an
 *     all-parked registry and DECLARES-RELEASES == live holds.
 *
 * The D12 red lines: the reuse hands out a pristine window (the park zap
 * reset every slot to perm-0 Invalid), the pool is bounded and overflows
 * back into the exact pre-pool RELEASE behaviour (counted), and EXIT /
 * mm_exit / fork tear everything down as before.
 * ------------------------------------------------------------------
 */

/*
 * Deregister a parked arena without touching the VMAs in its range: the
 * park left the range a plain anonymous mapping which the reuse flow has
 * usually already replaced, and the fresh declare that follows wants the
 * registry slots back.  With @recycle the frames return to the magazine
 * (markers restored, recycle blocks pushed) exactly like a real release;
 * with !@recycle (the V-A.3a P1b idle-eject arm) they are plain-erased
 * instead -- the caller is about to install a legacy VMA over them, and a
 * restored marker would both read as occupied to the placement backstop
 * and hand the frames back out of the recycle list under that VMA.  The
 * erased frames take the punch precedent's leak semantics (the magazine
 * skips marker-less frames, counted).
 *
 * Called with the owner mm's mmap_write and state->ctl_lock held.
 */
static void corten_arena_pool_eject_locked(struct corten_mm_state *state,
					   struct corten_arena *ar,
					   bool recycle)
{
	unsigned long frame, first, last;

	WRITE_ONCE(ar->idle, false);
	list_del(&ar->pool);
	state->nr_pool--;

	first = ar->start >> PMD_SHIFT;
	last = (ar->end - 1) >> PMD_SHIFT;
	for (frame = first; frame <= last; frame++) {
		struct corten_arena *stale = xa_load(&state->arenas, frame);

		if (WARN_ON_ONCE(stale && stale != ar))
			break;
		if (stale == ar) {
			if (recycle)
				corten_va_release_frame(state,
							frame << PMD_SHIFT);
			else
				xa_erase(&state->arenas, frame);
		}
	}
	atomic_long_inc(&corten_nr_pool_ejects);

	/* The deregistration must unlink the observability ledger BEFORE
	 * the free (obs_remove's contract): the eject path predating V-A.3a
	 * skipped this -- a freed descriptor stayed linked and the arenas
	 * walk dereferenced recycled memory once the slab reused it.  The
	 * placement-surface ejects (P1b/P4) made the shape hot.
	 */
	corten_arena_obs_remove(ar);

	/* No transaction can hold a reference (lookup has gated the park
	 * since the zap; the zap itself fenced the stragglers per window),
	 * so the drain completes synchronously; the timeout branch is the
	 * same counted kernel-bug path as the release body's.
	 */
	if (corten_arena_drain(ar))
		corten_arena_free(ar);
	else
		corten_arena_note_drain_timeout(state);
}

/*
 * Hand a parked arena's window back out: stamp the record back to a live
 * ANON region, refresh the permission bound and republish.  V-A.1: there
 * is no VMA left in the window to decorate -- the reactivation is a pure
 * metadata flip (the sec 2.5 reactivate arm of the idle<=>RESERVED
 * pairing); the first touch fault materializes the PT pages through the
 * FRESH gate, whose perm is the @perm recorded here.
 *
 * The descriptor, its live percpu_ref, the observability ledger entry and
 * the warm registry slots are reused as-is.
 *
 * Called with the owner mm's mmap_write and state->ctl_lock held.
 * Return: 0, -ENOMEM (the accounting or carrier gate refused; the slot
 * stays parked), -EAGAIN (P4: the auto-shape handout found a foreign VMA
 * in the window and ejected the slot).
 */
static int corten_arena_pool_reactivate(struct mm_struct *mm,
					struct corten_mm_state *state,
					struct corten_arena *ar, u8 perm,
					bool novma)
{
	unsigned long npages = (ar->end - ar->start) >> PAGE_SHIFT;
	vm_flags_t flags = corten_take_vm_flags(perm);

	/* P4 (V-A.3a, audit #14-#17): a parked window must be VMA-free at
	 * handout on the AUTO shape (the pool take and the VMA-less
	 * declare-side probe -- nothing legitimately places a tree VMA
	 * there: every mmap route into a parked window ejects it first).
	 * V-A.1 retired the vma_lookup validation arm; this is its
	 * defensive replacement -- a tree VMA inside the range means a
	 * placement guard failed upstream.  Fail safe: eject and answer
	 * -EAGAIN, never hand frames out under someone else's VMA.  The
	 * callers translate: pool_take degrades to its counted miss (the
	 * auto route places fresh window), pool_prepare returns to the
	 * fresh declare path.  The targeted DECLARE (novma == false) is
	 * exempt: its declaring VMA -- which declare_locked adopts and
	 * validates right after -- legitimately covers the range.
	 */
	if (novma && corten_vma_find(mm, ar->start, ar->end)) {
		WARN_ONCE(1, "corten: foreign vma in parked window %lx-%lx\n",
			  ar->start, ar->end);
		atomic_long_inc(&corten_nr_p4_ejects);
		corten_arena_pool_eject_locked(state, ar, true);
		return -EAGAIN;
	}

	/* V-A.1 accounting parity: the fresh path's mmap_region() charges
	 * total_vm (and RLIMIT_AS through may_expand_vm) for the mapping
	 * it installs; the VMA-free reactivation mirrors both by hand
	 * before the flip -- the park's VMA removal unwound the previous
	 * incarnation's charge.  -ENOMEM leaves the slot parked (the
	 * caller degrades), exactly like the take contract.
	 */
	if (!may_expand_vm(mm, flags, npages))
		return -ENOMEM;

	/* The take/declare caller passes the new contract's perm; the
	 * MAY bound is the private-anonymous superset (no VMA carries
	 * the MAY bits any more, sec 2.2).
	 */
	WRITE_ONCE(ar->prot, perm);
	/* V-A.1: no cached shadow-VMA to publish -- [FAIL-2]'s order
	 * argument degenerates to publishing the metadata state.  The
	 * idle flip precedes the register stamp so the INV-MV3 record
	 * check inside register() never observes "live ANON region,
	 * still idle".
	 *
	 * V-A.2b: the carrier persists across the park (descriptor
	 * lifetime) and resumes its anchor duties with the flip -- for a
	 * slot that already has one, the reactivate stays a pure
	 * metadata flip.  A slot whose last incarnation was a targeted
	 * DECLARE (tree shadow, removed at its park) has none: re-arm it
	 * unconditionally (F1: the declare-side route, novma == false,
	 * re-arms too) -- an anchor-less live window would lose its
	 * content at fork (no copy_page_range source) and stay
	 * unpickable, so the A.1 mixed state is not a legal outcome any
	 * more.  -ENOMEM keeps the slot parked -- the same degrade
	 * contract as the accounting gate.
	 */
	WRITE_ONCE(ar->idle, false);
	if (!READ_ONCE(ar->carrier)) {
		struct vm_area_struct *carrier;

		carrier = corten_arena_carrier_alloc(mm, ar->start, ar->end,
						     perm, NULL, 0, true);
		if (!carrier) {
			WRITE_ONCE(ar->idle, true);
			return -ENOMEM;
		}
		WRITE_ONCE(ar->carrier, carrier);
	}
	corten_region_register(ar, CORTEN_REGION_ANON, corten_region_may_full(),
			       0);
	list_del(&ar->pool);
	state->nr_pool--;
	refcount_set(&state->nr, refcount_read(&state->nr) + 1);
	vm_stat_account(mm, flags, npages);
	corten_arena_stat_add(state, CORTEN_ARENA_STAT_DECLARES, 1);
	atomic_long_inc(&corten_nr_pool_hits);

	/* V-A.3c hot-path sample: the window just went live and VMA-free
	 * (reactivation is a pure metadata flip) -- INV-MV2's window half
	 * re-proven at handout.  ctl_lock and mmap_write held.
	 */
	corten_audit_j2_sample_locked(mm);

	return 0;
}

/*
 * The DECLARE-side pool probe: serve the range from the park if it fits,
 * else make the range fresh-path-clean.
 *
 *   - A parked arena of exactly [addr, addr+len) with intact registry
 *     slots is reactivated in place -- this is the pool hit.  The C1
 *     emptiness check runs for parity with the fresh path: content
 *     cannot exist in a parked range through any normal route (the park
 *     zapped it and removed the reservation VMA, so nothing can fault
 *     or GUP into the range at all), but the check is the cheap safety
 *     net that must never serve a reuse over content -- a slot that is
 *     not empty is ejected and the fresh path takes over (which itself
 *     fails [C1] and degrades, the established attach-failure contract).
 *     V-A.1: the fresh path's VMA validation arm is gone -- a parked
 *     window has no VMA to validate; the probe is pure PT + registry.
 *   - Any other parked arena with frames inside the range is deregistered
 *     (ejected) so the fresh path's overlap check and frame stores see a
 *     clean range.
 *
 * Called with the owner mm's mmap_write and state->ctl_lock held.
 * @no_reuse (V-B.1) disables the reactivation half for a FILE declare
 * (a parked window's reuse contract is the ANON metadata flip; a file
 * mapping always takes the fresh path) while keeping the eject half.
 * Return: 0 = reactivated (DECLARE is done), 1 = proceed with the fresh
 * path, -errno = the declare failed.
 */
static int corten_arena_pool_prepare_locked(struct mm_struct *mm,
					    struct corten_mm_state *state,
					    unsigned long addr,
					    unsigned long len,
					    u8 perm, bool novma, bool no_reuse)
{
	unsigned long frame, first, last;
	struct corten_arena *ar;
	bool intact = true;

	first = addr >> PMD_SHIFT;
	last = (addr + len - 1) >> PMD_SHIFT;

	ar = xa_load(&state->arenas, first);
	if (ar && ar != &corten_va_reserve_sentinel &&
	    READ_ONCE(ar->idle) && ar->start == addr && ar->end == addr + len) {
		int ret;

		for (frame = first; frame <= last; frame++) {
			if (xa_load(&state->arenas, frame) != ar) {
				intact = false;
				break;
			}
		}

		if (intact && !no_reuse) {
			/* [C1] parity with the fresh declare. */
			ret = corten_arena_check_empty_locked(mm, addr,
							      addr + len);
			if (!ret) {
				/* The targeted-DECLARE route carries no prot
				 * input (novma == false): the neutral max is
				 * the honest stand-in for the declaring
				 * VMA's prot (which V-A.1 no longer has);
				 * the mprotect route narrows it explicitly.
				 * The V-A.2a auto route passes the mmap's
				 * own perm -- including PROT_NONE's 0.
				 * P4's -EAGAIN (ejected under a foreign VMA)
				 * takes the fresh path like any miss.
				 */
				ret = corten_arena_pool_reactivate(mm, state,
								   ar,
								   novma ? perm :
								   (CORTEN_PERM_READ |
								    CORTEN_PERM_WRITE |
								    CORTEN_PERM_EXEC),
								   novma);
				return ret == -EAGAIN ? 1 : ret;
			}
		}

		/* Holed (punched while parked), or the window is not
		 * empty: the slot cannot serve this reuse.  Return its
		 * frames to the magazine and let the fresh path rebuild.
		 */
		corten_arena_pool_eject_locked(state, ar, true);
		return 1;
	}

	/* Anything else parked inside the range is inert residue for the
	 * fresh path (e.g. an mmap of a different size landing on a
	 * parked arena's slots via the global placer).
	 */
	for (frame = first; frame <= last; frame++) {
		ar = xa_load(&state->arenas, frame);

		if (ar && ar != &corten_va_reserve_sentinel &&
		    READ_ONCE(ar->idle))
			corten_arena_pool_eject_locked(state, ar, true);
	}

	return 1;
}

/*
 * Parkability: the arena must be one intact piece spanning its whole
 * extent and every registry slot must still point at it.  A punched
 * arena (split shadow pieces, NULL hole frames) cannot be parked as a
 * unit -- it takes the real RELEASE, exactly like the pre-pool kernel.
 *
 * V-A.1: the shadow-VMA is optional -- a reactivated pool window is
 * VMA-less by design (sec 3.1.1) and parks as pure metadata.  A cached
 * pointer that exists must still be the exact covering piece.
 *
 * Called with the owner mm's mmap_write and state->ctl_lock held.
 */
static bool corten_arena_pool_parkable(struct mm_struct *mm,
				       struct corten_mm_state *state,
				       struct corten_arena *ar)
{
	struct vm_area_struct *vma = READ_ONCE(ar->vma);
	unsigned long frame, first, last;

	if (vma) {
		if (vma->vm_start != ar->start || vma->vm_end != ar->end)
			return false;
		if (vma_lookup(mm, ar->start) != vma)
			return false;
	}

	first = ar->start >> PMD_SHIFT;
	last = (ar->end - 1) >> PMD_SHIFT;
	for (frame = first; frame <= last; frame++)
		if (xa_load(&state->arenas, frame) != ar)
			return false;

	return true;
}

/*
 * V-A.1 (sec 3.1.1): remove the reservation VMA of a just-parked arena.
 * Runs after the content zap and the idle publish, with the shadow
 * decoration already stripped and the range still holding the (empty)
 * plain anonymous VMA the takeover flow installed: do_munmap() unlinks
 * it and retires the window's (now empty) PT pages through the regular
 * free funnels, leaving the parked window a *pure reservation* -- frame
 * slots with the idle descriptor, no VMA, no PT pages.
 *
 * Precondition: ar is idle, drained and fenced (the zap's window write
 * locks sealed every in-flight transaction), and this mm's mmap_write +
 * state->ctl_lock are held -- the same shape release_arena_locked()'s
 * do_munmap() runs under.
 *
 * Return: 0, or the do_munmap() status (memory pressure; the caller
 * degrades the park to a real RELEASE, counted).
 */
static int corten_arena_park_unmap_vma(struct mm_struct *mm,
				       struct corten_arena *ar)
{
	return do_munmap(mm, ar->start, ar->end - ar->start, NULL);
}

/*
 * The vm_flags shape a pool take's total_vm charge mirrors: the private
 * anonymous NORESERVE mapping the takeover flow would have installed for
 * @perm (V-A.1: the VMA-free reuse keeps the fresh path's accounting
 * parity -- may_expand_vm() sees the same total_vm, and the park/release
 * unwind reverses exactly this charge).
 */
static vm_flags_t corten_take_vm_flags(u8 perm)
{
	vm_flags_t flags = VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC |
			   VM_NORESERVE;

	if (perm & CORTEN_PERM_READ)
		flags |= VM_READ;
	if (perm & CORTEN_PERM_WRITE)
		flags |= VM_WRITE;
	if (perm & CORTEN_PERM_EXEC)
		flags |= VM_EXEC;

	return flags;
}

/*
 * Park @arena (the munmap-route EXACT shape in a MODE process): drop the
 * content with a full metadata reset, then keep the descriptor, its live
 * percpu_ref and the registry slots; V-A.1 removes the reservation VMA
 * from the tree as well -- the parked window is a no-VMA pure metadata
 * domain, so an access to it runs the legacy funnel into bad_area
 * (SEGV_MAPERR, exactly like a real munmap; the registered semantic
 * change S-1 -- D20-a) and /proc/maps no longer shows a reservation
 * segment (S-4).  A full pool (D12 fallback) refuses the park: this
 * munmap takes the real RELEASE -- the exact pre-pool behaviour, counted.
 *
 * Called with the owner mm's mmap_write and state->ctl_lock held.
 * Return: true = parked, false = take the real RELEASE instead.
 */
static bool corten_arena_pool_park_locked(struct mm_struct *mm,
					  struct corten_mm_state *state,
					  struct corten_arena *ar,
					  struct mmu_gather *tlb)
{
	struct vm_area_struct *vma;
	int ret;

	if (state->nr_pool >= CORTEN_ARENA_POOL_MAX) {
		atomic_long_inc(&corten_nr_pool_over);
		return false;
	}
	if (!corten_arena_pool_parkable(mm, state, ar))
		return false;

	/* Content drop, one mmu_gather over the whole arena.  flags=0
	 * resets every slot to perm-0 Invalid: a munmap kills the
	 * mprotect contracts of the range, and the reuse must hand out a
	 * pristine window (the FRESH fault gate derives from ar->prot
	 * after reactivation, which the next mmap re-records -- never
	 * from a previous incarnation's recorded perm).
	 *
	 * [perf1b] The slot reset itself is the wholesale metadata-array
	 * drop inside the zap (a 16KB op on a 2M frame otherwise walks
	 * 512 slots to reset the ~4 that ever carried content).
	 *
	 * [perf1c] @tlb is the caller's route-level gather and stays open
	 * across this function: the caller finishes it only after the
	 * write->read downgrade, so the shootdown wait no longer sits in
	 * the serialized write section (the placement
	 * vms_complete_munmap_vmas() uses for the legacy munmap).  The
	 * downgrade keeps every pool take out until the flush completed --
	 * a take needs mmap_write, so the parked window cannot be
	 * re-warmed while its old translations are still being shot down.
	 */
	ret = corten_arena_unmap_chunk_flags(mm, ar, ar->start,
					     ar->end - ar->start, 0, tlb);
	if (ret)
		return false;

	/* The route-level gather is finished here, under the write lock:
	 * V-A.1's reservation-VMA removal (do_munmap below) opens its own
	 * gather, and a nested finish force-upgrades to a full-mm
	 * shootdown (mm_tlb_flush_nested() -- the [perf1] IPI storm).
	 * This places the zap's flush wait back into the serialized write
	 * section (the pre-[perf1c] placement) -- the bounded price of
	 * the VMA-free park; deferring the removal past the downgrade
	 * would need a re-lock whose window races this pool's own takes.
	 * From here @tlb is spent; the caller must not finish it again.
	 */
	tlb_finish_mmu(tlb);

	/* Publish the parked state before the VMA goes: from here every
	 * lookup misses, so no route can observe the intermediate
	 * "live arena, cache cleared" shape.  In-flight transactions that
	 * entered before the park were fenced window by window by the
	 * zap's descriptor write locks (the [F-B seal] argument), so none
	 * can commit after the zap returned.
	 */
	WRITE_ONCE(ar->idle, true);
	vma = ar->vma;
	if (vma)
		corten_arena_unshadow(ar, vma);

	/* V-B.1 (INV-MV3 rule (c)): park(FILE) drops the file payload
	 * before the RESERVED stamp -- the reactivation is the ANON reuse
	 * contract, a parked window must not pin a live file reference
	 * nor keep a region node in the mapping's registry.  No-op for
	 * ANON regions.
	 */
	corten_region_file_teardown(ar);

	/* idle <=> rclass=RESERVED (sec 2.5): the park is the second
	 * rclass write point.  The record keeps the surviving MAY bound
	 * and flag reflection of the last live incarnation (register()
	 * ORs ar->prot into the bound, preserving may >= prot); the
	 * reactivation stamps the record back to ANON.
	 */
	corten_region_register(ar, CORTEN_REGION_RESERVED,
			       READ_ONCE(ar->may_prot),
			       READ_ONCE(ar->rflags));

	list_add_tail(&ar->pool, &state->arena_pool);
	state->nr_pool++;
	refcount_set(&state->nr, refcount_read(&state->nr) - 1);
	corten_arena_stat_add(state, CORTEN_ARENA_STAT_RELEASES, 1);
	atomic_long_inc(&corten_nr_pool_parks);

	if (vma) {
		/* V-A.1 surgery (sec 3.1.1): the reservation VMA goes.
		 * The empty plain anonymous VMA is unlinked and the
		 * window's (already empty) PT pages retire through the
		 * regular funnels, which also unwinds the total_vm
		 * charge mmap_region() made.  A failure is memory
		 * pressure: the range survives as plain anonymous
		 * memory, which a parked reservation must not be --
		 * degrade to the real RELEASE, counted.
		 */
		ret = corten_arena_park_unmap_vma(mm, ar);
		if (ret) {
			atomic_long_inc(&corten_nr_park_unmap_fails);
			corten_arena_release_arena_locked(mm, state, ar);
			return true;	/* handled: the RELEASE happened */
		}
	} else {
		/* A re-park of a VMA-less (reactivated) window: no VMA
		 * to remove, so the park does the reservation shape by
		 * hand -- the take's total_vm charge is unwound (no
		 * remove_vma() to do it) and the window's PT pages
		 * retire (sec 3.1.1: the pure reservation is frames +
		 * idle descriptor, no VMA, no PT pages).
		 */
		vm_stat_account(mm, corten_take_vm_flags(READ_ONCE(ar->prot)),
				-(long)((ar->end - ar->start) >> PAGE_SHIFT));
		corten_arena_free_ptes_novma(mm, ar->start, ar->end);
	}

	/* V-A.3c hot-path sample (j2-audit hook list): the parked window
	 * is VMA-free and frame-registered again -- the cheapest moment
	 * to prove the dominion holds after the park surgery.  ctl_lock
	 * and mmap_write are held: the stable-registry form.  Default
	 * off (corten_j2_walk_every).
	 */
	corten_audit_j2_sample_locked(mm);

	return true;
}

/*
 * munmap-route entry (EXACT class, MODE process): park, or fall back to
 * the real RELEASE.  Takes the DEV-13 outermost write lock itself, like
 * corten_arena_release() which it stands in for.
 */
static int corten_arena_pool_release(struct mm_struct *mm, unsigned long start,
				     unsigned long len)
{
	struct corten_mm_state *state;
	struct corten_arena *arena;
	struct mmu_gather tlb;
	bool parked;
	int ret;

	/* Pairs with the store in corten_arena_state_create(); the route
	 * verified the arena exists, so NULL cannot happen.
	 */
	state = smp_load_acquire(&mm->corten_state);
	if (!state)
		return -ENOENT;

	mmap_write_lock(mm);
	mutex_lock(&state->ctl_lock);

	arena = xa_load(&state->arenas, start >> PMD_SHIFT);
	if (!arena || arena->start != start || arena->end != start + len) {
		mutex_unlock(&state->ctl_lock);
		mmap_write_unlock(mm);
		return -ENOENT;
	}

	/* [perf1c, V-A.1 revision] The gather spans the park zap.  The
	 * park finishes it itself (its reservation-VMA removal must not
	 * nest a second gather -- the full-mm upgrade storm), so the
	 * parked path has nothing left to finish; on the RELEASE path
	 * the gather is finished after the write->read downgrade, the
	 * flush wait out of the serialized write section (legacy
	 * vms_complete_munmap_vmas() placement).  Either way a partial
	 * zap's queued pages are flushed and freed before this munmap
	 * returns.
	 */
	tlb_gather_mmu(&tlb, mm);
	parked = corten_arena_pool_park_locked(mm, state, arena, &tlb);
	if (parked)
		ret = 0;
	else
		ret = corten_arena_release_arena_locked(mm, state, arena);

	mutex_unlock(&state->ctl_lock);
	mmap_write_downgrade(mm);
	if (!parked)
		tlb_finish_mmu(&tlb);
	mmap_read_unlock(mm);

	return ret;
}

/* Tear every parked arena down (real releases): the fork-begin pool
 * flush.  Called with oldmm's mmap_write and state->ctl_lock held.
 */
static void corten_arena_pool_flush_locked(struct mm_struct *mm,
					   struct corten_mm_state *state)
{
	struct corten_arena *ar, *n;

	list_for_each_entry_safe(ar, n, &state->arena_pool, pool)
		corten_arena_release_arena_locked(mm, state, ar);
}

/*
 * mmap-side pool take: the most recently parked arena of exactly @len2
 * (the tail of the LRU) is reactivated IN PLACE and handed out.  V-A.1:
 * the parked window has no VMA and no PT pages -- the take is a pure
 * metadata flip (rclass ANON, ar->prot re-stamped from @prot, publish)
 * and the mmap completes without the MAP_FIXED flow's VMA replacement,
 * which would have torn the window's page tables down only for the
 * first touch faults to rebuild them.  The do_mmap() cret==2 early
 * return is the contract (mmap.c:433-446).
 *
 * The C1 emptiness check runs before the take: content cannot exist in
 * a parked range through any normal route (the park zapped it AND
 * removed the reservation VMA, so neither faults nor GUP can reach the
 * range), and a slot that is not empty must never serve a reuse -- it
 * is ejected and the caller degrades to fresh window.
 *
 * A reactivated arena's declare bookkeeping (DECLARES stat, nr, ledger
 * entry) is reused as-is; the percpu_ref never died.  The total_vm
 * charge and its RLIMIT_AS gate live in the reactivate body.
 *
 * Called under this mm's mmap_write (do_mmap's contract).
 * Return: 0 with *@addr set, -ENOENT when no slot can serve (the caller
 * counts one pool miss and places fresh window), -ENOMEM when the
 * accounting gate refused (the slot stays parked).
 */
static int corten_arena_pool_take(struct mm_struct *mm,
				  struct corten_mm_state *state,
				  unsigned long len2, unsigned long prot,
				  unsigned long *addr)
{
	unsigned long frame, first, last, perm = CORTEN_PERM_USER;
	struct corten_arena *ar;
	int ret;

	mmap_assert_write_locked(mm);

	/* A reused mapping cannot honour mlockall() (def_flags) -- fall
	 * back to the fresh flow, which runs the full accounting.
	 */
	if (mm->def_flags & VM_LOCKED)
		return -ENOENT;

	mutex_lock(&state->ctl_lock);

	list_for_each_entry_reverse(ar, &state->arena_pool, pool) {
		if (ar->end - ar->start == len2)
			goto found;
	}
	mutex_unlock(&state->ctl_lock);
	return -ENOENT;

found:
	first = ar->start >> PMD_SHIFT;
	last = (ar->end - 1) >> PMD_SHIFT;
	for (frame = first; frame <= last; frame++) {
		if (xa_load(&state->arenas, frame) != ar)
			goto eject;
	}

	/* [C1] parity with the fresh declare (see above). */
	if (corten_arena_check_empty_locked(mm, ar->start, ar->end))
		goto eject;

	if (prot & PROT_READ)
		perm |= CORTEN_PERM_READ;
	if (prot & PROT_WRITE)
		perm |= CORTEN_PERM_WRITE;
	if (prot & PROT_EXEC)
		perm |= CORTEN_PERM_EXEC;

	/* No live PTE can exist (the park zap removed the window's PT
	 * pages with its VMA), so no TLB maintenance is owed; the
	 * FRESH fault gate derives from the perm recorded below.
	 * V-A.2b/F1: the take is an auto shape -- reactivate re-arms the
	 * carrier if the slot predates one (unconditionally: no route
	 * may publish an anchor-less live window).
	 */
	ret = corten_arena_pool_reactivate(mm, state, ar, perm, true);
	if (ret) {
		mutex_unlock(&state->ctl_lock);
		return ret;	/* -ENOMEM: stays parked, degrade */
	}

	*addr = ar->start;
	mutex_unlock(&state->ctl_lock);

	/* V-A.3c hot-path sample: the window handed out.  mmap_write is
	 * still held (the take contract), so the stable-registry form
	 * runs without re-entering ctl_lock.
	 */
	corten_audit_j2_sample_locked(mm);

	return 0;

eject:
	/* Holed (punched while parked), or the window is not empty:
	 * deregister and let the caller place fresh window.
	 */
	corten_arena_pool_eject_locked(state, ar, true);
	mutex_unlock(&state->ctl_lock);

	/* V-A.3c hot-path sample: an ejected slot is the audit-interesting
	 * arm of the take (a punched parked window leaves implant
	 * neighbourhoods behind).
	 */
	corten_audit_j2_sample_locked(mm);

	return -ENOENT;
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
	/* M4.T2 single-lookup fast path: start and end-1 inside one 2M
	 * frame are definitionally the same arena (the registry is
	 * frame-granular and arenas are frame-aligned unions), so the
	 * second RCU lookup + pin -- a contended atomic pair on the
	 * born-atomic ref under churn -- only runs when the range
	 * actually crosses a frame boundary.  Reference accounting is
	 * by @ar_end (NULL when the range never crossed a frame
	 * boundary), NOT by pointer identity: a cross-frame range of
	 * one arena holds TWO references to the same descriptor, and
	 * conflating the two is exactly how the r03 DoD-B leak
	 * regressed under the first cut of this fast path (r06-m4t12:
	 * the JVM's 64MB heap free drained-timed-out 10s later).
	 */
	if (((end - 1) >> PMD_SHIFT) != (start >> PMD_SHIFT))
		ar_end = corten_arena_lookup_get(mm, end - 1);
	else
		ar_end = NULL;	/* same frame: ar_start's reference covers it */

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
			 * zero.  One put per HELD reference (see the
			 * lookup above).  RELEASE (like DECLARE) takes
			 * the write lock itself -- we run lockless
			 * here, so this is the DEV-13 outermost
			 * acquisition, not a re-entry.
			 *
			 * T1c: in a MODE process the arena parks into
			 * the per-mm pool instead (zap + reset, keep
			 * descriptor/VMA/frames); the real RELEASE runs
			 * only when the arena is unparkable or the pool
			 * is full (LRU eviction pays it).
			 */
			if (ar_start)
				percpu_ref_put(&ar_start->active);
			if (ar_end)
				percpu_ref_put(&ar_end->active);
			if (READ_ONCE(mm->corten_mode))
				ret = corten_arena_pool_release(mm, ar->start,
								ar->end -
								ar->start);
			else
				ret = corten_arena_release(mm, ar->start,
							   ar->end -
							   ar->start);
			/* V-A.3c hot-path sample: the park/release just
			 * rewrote the window's tree picture (lockless
			 * here: the self-sufficient walker form).
			 */
			if (ret >= 0)
				corten_audit_j2_sample(mm);
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

	/* One put per held reference (ar_end is non-NULL exactly when the
	 * second lookup pinned one, same arena or not).
	 */
	if (ar_start)
		percpu_ref_put(&ar_start->active);
	if (ar_end)
		percpu_ref_put(&ar_end->active);

	/* V-A.3c hot-path sample: the chunk transaction (ret == 1) just
	 * carved the arena; nothing is held here, so the self-sufficient
	 * walker form takes its own ctl_lock.
	 */
	if (ret == 1)
		corten_audit_j2_sample(mm);

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
		struct vm_area_struct *v = corten_vma_find(mm, a, a + 1);
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
 * V-A.3a implant registry (D24): register [start, start+len) (clipped to
 * the window domain -- outside it an implant is an ordinary legacy VMA
 * among its peers and no walker will ask) as VA the legacy funnel legally
 * owns.  Producers: the punch route's success arms (a file MAP_FIXED over
 * live arena frames) and the P1b idle-eject (a plain MAP_FIXED over a
 * parked window).  Both run under this mm's mmap_write; @ctl_lock is
 * taken here (DEV-13 order, like pool_prepare).  The array is kept sorted
 * and disjoint (merge on insert), so repeated punches of the same hole
 * cost no growth.  A krealloc failure drops the registration, counted --
 * the implant keeps working, only its J2 whitelist entry is missing (the
 * A.3c walker's fallback predicate catches the shape).
 *
 * V-A.3c: a registry-less mm gets one here.  ENTER is allocation-free
 * (A5) and the registry is born at the first arena work -- a MODE mm
 * whose only window occupancy is an implant (the empty-window backstop
 * arm below) would otherwise register into the void: mark would drop
 * the entry, the fault terminus would MAPERR the legal VMA and the J2
 * walker would count it.  get_state() runs under the caller's
 * mmap_write like every producer.
 */
void corten_implant_mark(struct mm_struct *mm, unsigned long start,
			 unsigned long len)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);
	struct corten_implant_range *r;
	unsigned long end;
	unsigned int i;

	if (!state) {
		state = corten_arena_get_state(mm);
		if (!state) {
			atomic_long_inc(&corten_nr_implant_drops);
			return;
		}
	}
	/* V-A.3c: ranges with no window intersection have nothing to
	 * register (the clip below would otherwise fabricate an entry at
	 * the window base for a fully-below-window request -- the A.3a
	 * producers were window-bound by construction, the backstop arm
	 * is not).
	 */
	if (!len || start >= CORTEN_MODE_WINDOW_END ||
	    start + len <= CORTEN_MODE_WINDOW_START)
		return;
	/* The true end before the start clip: a range straddling the
	 * window edge registers only its intersection.
	 */
	end = start + len;
	if (start < CORTEN_MODE_WINDOW_START)
		start = CORTEN_MODE_WINDOW_START;
	if (end > CORTEN_MODE_WINDOW_END)
		end = CORTEN_MODE_WINDOW_END;
	if (end <= start)
		return;

	mutex_lock(&state->ctl_lock);

	/* Locate the first range not entirely below [start, end). */
	for (i = 0; i < state->nr_implants; i++) {
		if (state->implants[i].end >= start)
			break;
	}

	if (i == state->nr_implants ||
	    state->implants[i].start > end) {
		/* Pure insert at @i. */
		if (state->nr_implants == state->nr_implants_alloc) {
			unsigned int n = state->nr_implants_alloc ?
					 state->nr_implants_alloc * 2 : 16;
			struct corten_implant_range *old = state->implants;

			/* V-A.3d: the array is grown by install-and-retire,
			 * not krealloc() -- the J1 exemption below reads the
			 * registry from RCU-only contexts
			 * (lock_vma_under_rcu), where a synchronously freed
			 * old image would be a use-after-free.  Copy,
			 * publish the new image, then let a grace period
			 * release the old one; the publish order (pointer
			 * before the nr bump, wmb below) is what the
			 * lockless reader's read order pairs with.
			 */
			r = kmalloc_array(n, sizeof(*r), GFP_KERNEL);
			if (!r)
				goto drop;
			if (old) {
				memcpy(r, old,
				       state->nr_implants * sizeof(*r));
				kfree_rcu_mightsleep(old);
			}
			WRITE_ONCE(state->implants, r);
			state->nr_implants_alloc = n;
		}
		memmove(&state->implants[i + 1], &state->implants[i],
			(state->nr_implants - i) * sizeof(*r));
		state->implants[i].start = start;
		state->implants[i].end = end;
		smp_wmb();	/* publish the entries before their count */
		WRITE_ONCE(state->nr_implants, state->nr_implants + 1);
	} else {
		/* Overlap or adjacency with implants[i..j): merge into one
		 * entry covering the union.
		 */
		unsigned int j = i;

		if (start < state->implants[i].start)
			state->implants[i].start = start;
		while (j + 1 < state->nr_implants &&
		       state->implants[j + 1].start <= end)
			j++;
		if (end > state->implants[j].end)
			state->implants[j].end = end;
		state->implants[i].end = state->implants[j].end;
		if (j > i) {
			memmove(&state->implants[i + 1], &state->implants[j + 1],
				(state->nr_implants - j - 1) * sizeof(*r));
			state->nr_implants -= j - i;
		}
	}

	mutex_unlock(&state->ctl_lock);
	return;

drop:
	atomic_long_inc(&corten_nr_implant_drops);
	mutex_unlock(&state->ctl_lock);
}

/*
 * The registry's query form (A.3a: KUnit anchors; the A.3c INV-MV2 walker
 * is the production consumer): is [start, start+len) fully inside the
 * union of registered ranges?  Sorted + disjoint lets a single forward
 * scan answer.  Read under mmap_read or better (every writer holds
 * mmap_write, so no reader can race a mutation).
 */
bool corten_implant_covers(struct mm_struct *mm, unsigned long start,
			   unsigned long len)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);
	unsigned long cursor = start, end = start + len;
	unsigned int i;

	if (!state || !len || end <= start)
		return false;

	for (i = 0; i < state->nr_implants && cursor < end; i++) {
		if (state->implants[i].end <= cursor)
			continue;	/* spent */
		if (state->implants[i].start > cursor)
			return false;	/* hole before the next range */
		cursor = state->implants[i].end;
	}

	return cursor >= end;
}

/*
 * The RCU-safe query form (V-A.3d): same predicate as
 * corten_implant_covers(), for readers that hold an RCU read-side
 * section instead of the mmap lock -- the J1 exemption inside
 * corten_j1_slow(), reachable from lock_vma_under_rcu().  Safety has
 * two halves: (a) the array image read here is snapshot once (count
 * first, then pointer; the pair ordering with mark's publish -- pointer
 * before count, smp_wmb between -- guarantees the snapshot's count
 * never exceeds the image's allocation, on every architecture), and
 * (b) retired images are freed by kfree_rcu(), so the snapshot stays
 * valid through the caller's RCU section (mmap-lock callers are
 * excluded from every writer anyway).  In-place mutations (insert
 * memmove, merge) may be observed torn: the predicate then misanswers
 * one advisory counter, it never walks out of bounds.
 */
bool corten_implant_covers_lockless(struct mm_struct *mm,
				    unsigned long start, unsigned long len)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);
	const struct corten_implant_range *implants;
	unsigned long cursor = start, end = start + len;
	unsigned int nr, i;

	if (!state || !len || end <= start)
		return false;

	nr = READ_ONCE(state->nr_implants);
	smp_rmb();	/* pair with mark's publish order above */
	implants = READ_ONCE(state->implants);
	if (!implants)
		return false;

	for (i = 0; i < nr && cursor < end; i++) {
		if (implants[i].end <= cursor)
			continue;	/* spent */
		if (implants[i].start > cursor)
			return false;	/* hole before the next range */
		cursor = implants[i].end;
	}

	return cursor >= end;
}

/* ------------------------------------------------------------------ *
 * V-A.3c: the INV-MV2 audit walker (j2-audit hook list, MV_VMA_FREE_SPEC
 * sec 1.3 J2).  Read-only by construction (INV6): maple tree + registry
 * reads, no PTE is walked or written.
 * ------------------------------------------------------------------
 */

/*
 * The whitelist predicate over a caller-supplied (stable or locked)
 * registry image: is [start, end) fully inside the union of the sorted,
 * disjoint @implants?  The array form of corten_implant_covers() -- the
 * walker needs it against both the live array (locked callers) and no
 * other form exists, so it is factored once.
 */
static bool corten_audit_j2_covered(const struct corten_implant_range *implants,
				    unsigned int nr, unsigned long start,
				    unsigned long end)
{
	unsigned long cursor = start;
	unsigned int i;

	for (i = 0; i < nr && cursor < end; i++) {
		if (implants[i].end <= cursor)
			continue;	/* spent */
		if (implants[i].start > cursor)
			return false;	/* hole before the next range */
		cursor = implants[i].end;
	}

	return cursor >= end;
}

/*
 * One audit pass.  INV-MV2: every tree VMA that intersects the window
 * domain must be either the arena's own (VM_CORTEN -- a targeted
 * DECLARE's shadow piece or a punch-split survivor; the MODE auto
 * windows themselves are VMA-free post-A.2) or fully inside the implant
 * registry (a punch implant, a P1b idle-eject admission or the placement
 * backstop's empty-window arm).  A VMA that is neither is a placement
 * guard failure: WARN once, count, and archive the first address.
 *
 * The stale pass classifies registry entries carrying no tree VMA at
 * all (mark-then-fail installs, munmapped implants) -- benign, counted
 * separately, never WARNed: the A.3b handoff predicate.
 *
 * Locking: the maple walk runs under the walker's own RCU section
 * (mas_find/mt_find on the USE_RCU-mode mm tree; VMA lifetime is
 * RCU-fenced, so the pass is safe in every caller context -- mmap
 * write, mmap read or lockless).  The @implants image must be stable
 * for the duration: the locked entry passes the live array under its
 * stability contract, the self-sufficient entry holds ctl_lock.
 */
static int corten_audit_j2_scan(struct mm_struct *mm,
				const struct corten_implant_range *implants,
				unsigned int nr_implants)
{
	struct vm_area_struct *vma;
	unsigned long first_viol = 0;
	unsigned int stale = 0, i;
	int violations = 0;

	MA_STATE(mas, &mm->mm_mt, CORTEN_MODE_WINDOW_START,
		 CORTEN_MODE_WINDOW_END - 1);

	rcu_read_lock();
	mas_for_each(&mas, vma, CORTEN_MODE_WINDOW_END - 1) {
		unsigned long s, e;

		if (vma->vm_flags & VM_CORTEN)
			continue;
		s = max(vma->vm_start, CORTEN_MODE_WINDOW_START);
		e = min(vma->vm_end, CORTEN_MODE_WINDOW_END);
		if (corten_audit_j2_covered(implants, nr_implants, s, e))
			continue;
		violations++;
		if (!first_viol)
			first_viol = s;
	}
	for (i = 0; i < nr_implants; i++) {
		unsigned long index = implants[i].start;

		vma = mt_find(&mm->mm_mt, &index, implants[i].end - 1);
		if (!vma || vma->vm_start >= implants[i].end)
			stale++;
	}
	rcu_read_unlock();

	atomic_long_inc(&corten_nr_j2_walks);
	if (violations) {
		atomic_long_add(violations, &corten_nr_j2_violations);
		/* First writer wins; the address is diagnostic, not state. */
		atomic_long_cmpxchg(&corten_j2_first_violation, 0, first_viol);
		WARN_ONCE(1,
			  "corten: INV-MV2 violated: %d window vma(s) outside the implant registry (first at %lx)\n",
			  violations, first_viol);
	}
	if (stale)
		atomic_long_add(stale, &corten_nr_j2_stale);

	return violations;
}

/*
 * The self-sufficient walker entry: for callers holding no lock of this
 * mm (the three syscall-route tails, the debugfs manual trigger,
 * KUnit).  Takes the registry ctl_lock for the scan -- legal under a
 * held mmap_read/write as well (DEV-13: mmap outermost, ctl inner), so
 * the madvise route's variable lock modes all work; only a caller
 * already inside ctl_lock must use the _locked entry instead.
 * Return: violations found in this walk (0 = INV-MV2 holds).
 */
int corten_audit_j2_walk(struct mm_struct *mm)
{
	struct corten_mm_state *state;
	int violations;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode))
		return 0;
	state = READ_ONCE(mm->corten_state);
	if (!state)
		return 0;

	mutex_lock(&state->ctl_lock);
	violations = corten_audit_j2_scan(mm, state->implants,
					  state->nr_implants);
	mutex_unlock(&state->ctl_lock);

	return violations;
}

/*
 * The stable-registry entry: for callers whose context already fences
 * every implant_mark() writer -- this mm's mmap_write held (park, take,
 * reactivate, fork_commit; mark producers all run under mmap_write),
 * the registry ctl_lock held, or mm_users == 0 (mm_exit head).  No lock
 * is taken.  Return: as corten_audit_j2_walk().
 */
int corten_audit_j2_walk_locked(struct mm_struct *mm)
{
	struct corten_mm_state *state;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode))
		return 0;
	state = READ_ONCE(mm->corten_state);
	if (!state)
		return 0;

	return corten_audit_j2_scan(mm, state->implants, state->nr_implants);
}

/*
 * The hot-path sample gate (audit hook list; default off).  The park /
 * take / reactivate / route-tail triggers sit on churn-heavy paths, so
 * they walk only when the debugfs switch enables this static key; the
 * lifecycle points (mm_exit, fork_commit) always walk.  Off, the cost
 * is one patched-out branch.  Two forms for the two lock shapes of the
 * call sites (see the walkers above).
 */
static DEFINE_STATIC_KEY_FALSE(corten_j2_sample_key);

static void corten_audit_j2_sample(struct mm_struct *mm)
{
	if (static_branch_unlikely(&corten_j2_sample_key) &&
	    READ_ONCE(mm->corten_mode))
		corten_audit_j2_walk(mm);
}

static void corten_audit_j2_sample_locked(struct mm_struct *mm)
{
	if (static_branch_unlikely(&corten_j2_sample_key) &&
	    READ_ONCE(mm->corten_mode))
		corten_audit_j2_walk_locked(mm);
}

/*
 * debugfs "j2_walk_every" switch backend and the "j2_walk <pid>" manual
 * trigger backend (mm/corten.c owns the files).  The static key is
 * enabled/disabled from process context, exactly like the boot corten=on
 * key flip.
 */
void corten_arena_j2_sample_set(bool on)
{
	if (on)
		static_branch_enable(&corten_j2_sample_key);
	else
		static_branch_disable(&corten_j2_sample_key);
}

int corten_arena_j2_walk_pid(pid_t pid)
{
	struct task_struct *task;
	struct mm_struct *mm;
	int ret;

	if (!corten_enabled_static())
		return -EOPNOTSUPP;

	rcu_read_lock();
	task = find_get_task_by_vpid(pid);
	rcu_read_unlock();
	if (!task)
		return -ESRCH;

	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -EINVAL;

	ret = corten_audit_j2_walk(mm);
	mmput(mm);

	return ret;
}

/* ------------------------------------------------------------------
 * V-E whitelist classification (MV_VMA_FREE_SPEC.md sec 1.3 J2, the
 * complete form; sec 3.5): one read-only pass over the *whole* maple
 * tree of a MODE mm, classifying every VMA.  Read-only by construction
 * (INV6): tree + registry reads only, no PTE is walked or written.
 * ------------------------------------------------------------------
 */

/*
 * The pure predicate (KUnit-covered through the scan histogram):
 * window-intersecting VMAs must be the arena's own or an implant
 * (VIOLATION otherwise -- the same invariant corten_audit_j2_scan
 * asserts over the window segment; the whitelist form re-derives it
 * from the full-tree walk so the delegated composition and the window
 * closure come from one pass), and the delegated domain splits into
 * the whitelist categories (see enum corten_wl_class).  The heap
 * predicate accepts exactly the [start_brk, PAGE_ALIGN(brk)) span --
 * the VMA sys_brk/do_brk_flags maintains; the grows flags carry the
 * main stack; arch_vma_name() the vdso/vvar/vsyscall trio; vm_file
 * any file mapping (exec-time and MAP_SHARED alike).
 */
static enum corten_wl_class
corten_whitelist_classify(const struct corten_implant_range *implants,
			  unsigned int nr, struct mm_struct *mm,
			  struct vm_area_struct *vma)
{
	unsigned long top = PAGE_ALIGN(mm->brk);

	if (vma->vm_end > CORTEN_MODE_WINDOW_START &&
	    vma->vm_start < CORTEN_MODE_WINDOW_END) {
		unsigned long s, e;

		if (vma->vm_flags & VM_CORTEN)
			return CORTEN_WL_SHADOW;
		s = max(vma->vm_start, CORTEN_MODE_WINDOW_START);
		e = min(vma->vm_end, CORTEN_MODE_WINDOW_END);
		if (corten_audit_j2_covered(implants, nr, s, e))
			return CORTEN_WL_IMPLANT;
		return CORTEN_WL_VIOLATION;
	}
	if (vma->vm_start >= mm->start_brk && vma->vm_start < top &&
	    vma->vm_end <= top && !vma->vm_file)
		return CORTEN_WL_BRK;
	if (vma->vm_flags & (VM_GROWSDOWN | VM_GROWSUP))
		return CORTEN_WL_STACK;
	if (arch_vma_name(vma))
		return CORTEN_WL_SPECIAL;
	if (vma->vm_file)
		return CORTEN_WL_FILE;
	if (vma_is_anonymous(vma))
		return CORTEN_WL_ANON;
	return CORTEN_WL_UNCLASSIFIED;
}

/*
 * One audit pass (see the j2 scan above for the locking contract: RCU
 * tree walk, @implants image stable for the duration).  @hist, when
 * non-NULL, receives the per-class VMA count of this walk (KUnit).
 * Return: window-domain violations found (0 = J2-complete holds).
 */
static int corten_whitelist_scan(struct mm_struct *mm,
				 const struct corten_implant_range *implants,
				 unsigned int nr_implants,
				 unsigned long *hist)
{
	struct vm_area_struct *vma;
	unsigned int brk_vmas = 0;
	int violations = 0;

	MA_STATE(mas, &mm->mm_mt, 0, ULONG_MAX);

	if (hist)
		memset(hist, 0, CORTEN_WL_NR_CLASSES * sizeof(*hist));

	rcu_read_lock();
	mas_for_each(&mas, vma, ULONG_MAX) {
		enum corten_wl_class c;

		c = corten_whitelist_classify(implants, nr_implants,
					      mm, vma);

		if (hist)
			hist[c]++;
		if (c == CORTEN_WL_VIOLATION) {
			violations++;
		} else if (c == CORTEN_WL_BRK) {
			brk_vmas++;
			atomic_long_inc(&corten_nr_wl_brk_vmas);
		} else if (c >= CORTEN_WL_BRK) {
			/* delegated whitelist (BRK handled above) */
			atomic_long_inc(&corten_nr_wl_delegated_vmas);
			if (c == CORTEN_WL_UNCLASSIFIED)
				atomic_long_inc(&corten_nr_wl_unclassified);
		}
	}
	rcu_read_unlock();

	atomic_long_inc(&corten_nr_wl_walks);
	if (violations) {
		atomic_long_add(violations, &corten_nr_wl_violations);
		WARN_ONCE(1,
			  "corten: whitelist violated: %d window vma(s) unclassified as shadow/implant\n",
			  violations);
	}
	if (brk_vmas > 1)
		atomic_long_inc(&corten_nr_wl_brk_anomalies);

	return violations;
}

/* The self-sufficient entry: see corten_audit_j2_walk() above. */
int corten_audit_whitelist_walk(struct mm_struct *mm)
{
	struct corten_mm_state *state;
	int violations;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode))
		return 0;
	state = READ_ONCE(mm->corten_state);
	if (!state)
		return 0;

	mutex_lock(&state->ctl_lock);
	violations = corten_whitelist_scan(mm, state->implants,
					   state->nr_implants, NULL);
	mutex_unlock(&state->ctl_lock);

	return violations;
}

/* The stable-registry entry: see corten_audit_j2_walk_locked() above. */
int corten_audit_whitelist_walk_locked(struct mm_struct *mm)
{
	struct corten_mm_state *state;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode))
		return 0;
	state = READ_ONCE(mm->corten_state);
	if (!state)
		return 0;

	return corten_whitelist_scan(mm, state->implants,
				     state->nr_implants, NULL);
}

/* debugfs "whitelist <pid>" backend (mm/corten.c owns the file). */
int corten_arena_wl_audit_pid(pid_t pid)
{
	struct task_struct *task;
	struct mm_struct *mm;
	int ret;

	if (!corten_enabled_static())
		return -EOPNOTSUPP;

	rcu_read_lock();
	task = find_get_task_by_vpid(pid);
	rcu_read_unlock();
	if (!task)
		return -ESRCH;

	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -EINVAL;

	ret = corten_audit_whitelist_walk(mm);
	mmput(mm);

	return ret;
}

/*
 * The A-series exit-gate one-stop read (debugfs "audit_gate", kselftest
 * output style): the J1 pair and the J2 walker ledger in one place so
 * the guest acceptance run greps a single file.  gate_pass is the two
 * hard invariants -- no J1 window *hit* (a tree VMA the legacy lookup
 * found in the window domain; implant accesses are exempted from the
 * pair by V-A.3d, so a compliant workload has no legal hit source left)
 * and no INV-MV2 violation; j1_probes and j2_stale are printed raw
 * because their zero-ness is workload-bound (post-A.3d the S-5
 * terminals answer the msync/madvise/mincore window legs, so probes on
 * a pure-MODE workload should read zero too -- any residual belongs to
 * the V-C families (#3/#7) or a disclosed N-low row; stale is the
 * benign classification by design).  V-E extends gate_pass with the
 * whitelist ledger's two zeros (wl_violations, wl_brk_anomalies) and
 * prints the composition raw: wl_brk_vmas/wl_delegated_vmas are
 * workload-bound disclosure, wl_unclassified is the same shape as
 * j2_stale (a bucket, not a verdict).
 */
void corten_arena_audit_gate_report(struct seq_file *m)
{
	long probes = atomic_long_read(&corten_nr_j1_probes);
	long hits = atomic_long_read(&corten_nr_j1_hits);
	long viol = atomic_long_read(&corten_nr_j2_violations);
	long wl_viol = atomic_long_read(&corten_nr_wl_violations);
	long wl_anom = atomic_long_read(&corten_nr_wl_brk_anomalies);

	seq_printf(m, "j1_probes          %ld\n", probes);
	seq_printf(m, "j1_hits            %ld\n", hits);
	seq_printf(m, "j2_walks           %ld\n",
		   atomic_long_read(&corten_nr_j2_walks));
	seq_printf(m, "j2_violations      %ld\n", viol);
	seq_printf(m, "j2_stale           %ld\n",
		   atomic_long_read(&corten_nr_j2_stale));
	seq_printf(m, "j2_first_violation 0x%lx\n",
		   atomic_long_read(&corten_j2_first_violation));
	/* V-E: the whitelist (J2-complete) ledger. */
	seq_printf(m, "wl_walks           %ld\n",
		   atomic_long_read(&corten_nr_wl_walks));
	seq_printf(m, "wl_violations      %ld\n", wl_viol);
	seq_printf(m, "wl_brk_vmas        %ld\n",
		   atomic_long_read(&corten_nr_wl_brk_vmas));
	seq_printf(m, "wl_delegated_vmas  %ld\n",
		   atomic_long_read(&corten_nr_wl_delegated_vmas));
	seq_printf(m, "wl_unclassified    %ld\n",
		   atomic_long_read(&corten_nr_wl_unclassified));
	seq_printf(m, "wl_brk_anomalies   %ld\n", wl_anom);
	seq_printf(m, "gate_pass          %d\n",
		   !hits && !viol && !wl_viol && !wl_anom);
}

/*
 * V-A.3a P1b (audit #14, D24): clear [addr, addr+len) of parked (idle)
 * arenas before a plain MAP_FIXED replaces them.  Both lookup_get()
 * probes below skip idle arenas, so without this arm a parked window is
 * "no arena involved" to the punch route AND tree-free to the MARK
 * route's early exit -- the legacy funnel would install a foreign VMA on
 * frames the registry still owns, and a later pool handout (reactivate,
 * fresh declare) would collide with it.  Legacy plain MAP_FIXED is
 * *replace*, so the answer is eject-and-admit, not reject: every idle
 * arena with a frame inside the range is deregistered whole (the parkable
 * invariant wants intact pieces; a boundary-cutting range ejects just the
 * same) and the incoming VMA becomes a registered implant on freed VA.
 *
 * Magazine reserve sentinels inside the range are left untouched: a
 * sentinel under an explicit-address legacy mapping is a legal shape the
 * magazine already owns (every serve re-verifies marker AND tree, the T0
 * obstacle contract in corten_va_mag_alloc_cpu()), and rejecting here
 * would surface an error class A.1 never had -- the D24 ruling.  The
 * eject uses the erase variant (no marker restore), mirroring the punch
 * precedent: the frames are about to be foreign-owned.
 *
 * MODE-agnostic (the registry is the truth source, not the mode bit) and
 * called under this mm's mmap_write (do_mmap's contract); ctl_lock is
 * taken inside (DEV-13 order: mmap_write -> ctl_lock, like
 * pool_prepare).
 * Return: 0 = the range is clear of idle state (or held none) -- proceed
 * with the punch/legacy flow; no failure mode exists (an eject cannot
 * fail).
 */
static int corten_arena_placement_punch_idle(struct mm_struct *mm,
					     unsigned long addr,
					     unsigned long len)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);
	struct corten_arena *ar;
	unsigned long frame, first, last;
	bool ejected = false;

	if (!state || !READ_ONCE(state->nr_pool))
		return 0;

	first = addr >> PMD_SHIFT;
	last = (addr + len - 1) >> PMD_SHIFT;

	mutex_lock(&state->ctl_lock);
	xa_for_each_range(&state->arenas, frame, ar, first, last) {
		if (ar != &corten_va_reserve_sentinel && READ_ONCE(ar->idle)) {
			corten_arena_pool_eject_locked(state, ar, false);
			ejected = true;
		}
	}
	mutex_unlock(&state->ctl_lock);

	if (ejected) {
		atomic_long_inc(&corten_nr_placement_idle_ejects);
		/* The freed VA is legal legacy terrain now: register the
		 * implant the gather below is about to create (D24; the J2
		 * whitelist's producer).
		 */
		corten_implant_mark(mm, addr, len);
	}

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
	 * on the incl-idle occupancy probe at the do_mmap gate (V-A.3a) --
	 * so only a real MAP_FIXED overwrite gets here, with the parked
	 * windows already cleared by the P1b arm in corten_arena_mmap_route().
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
		unsigned long rel_start = ar->start;
		unsigned long rel_len = ar->end - ar->start;

		if (ar_start)
			percpu_ref_put(&ar_start->active);
		if (ar_end)
			percpu_ref_put(&ar_end->active);
		ret = corten_arena_release_locked(mm, state, rel_start,
						  rel_len);
		if (!ret) {
			atomic_long_inc(&corten_nr_mmap_punches);
			/* V-A.3a: the legacy funnel is about to install a
			 * foreign VMA over the freed arena range -- the
			 * implant registry's second producer.  (@ar may be
			 * freed by now; the range was captured above.)
			 */
			corten_implant_mark(mm, rel_start, rel_len);
		}
		return ret < 0 ? ret : 0;
	}

	/* CHUNK: punch the hole, keep the rest of the arena. */
	ret = corten_arena_mmap_punch(mm, state, ar, addr, end);
	if (ar_start)
		percpu_ref_put(&ar_start->active);
	if (ar_end)
		percpu_ref_put(&ar_end->active);
	if (!ret) {
		/* V-A.3a: the punched hole receives the foreign VMA (the
		 * D-G'' file-MAP_FIXED shape) -- register the implant.
		 */
		corten_implant_mark(mm, addr, len);
	}

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
	struct vm_area_struct *vma;
	unsigned long a, end = addr + len, start = addr;
	u8 perm = CORTEN_PERM_USER;
	int ret = 0;

	if (!corten_enabled_static() || !READ_ONCE(mm->corten_state))
		return 0;

	/* V-A.3a P1b (audit #14): parked windows are invisible to both
	 * dispatch legs below (lookup_get() skips idle; the MARK leg's
	 * lookup miss exits legacy), so a plain MAP_FIXED over one would
	 * reach the funnel with the frames still registered.  Clear them
	 * first; NOREPLACE never gets here (do_mmap's gate answered
	 * -EEXIST on the incl-idle occupancy probe).
	 */
	if ((flags & MAP_FIXED) && !(flags & MAP_FIXED_NOREPLACE)) {
		ret = corten_arena_placement_punch_idle(mm, addr, len);
		if (ret)
			return ret;
	}

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

	/* V-A.1: the mark is pure metadata; the anchor pointer (may be
	 * NULL for an A.1-reactivated window) only feeds the zap's rmap
	 * bookkeeping, which is NULL-tolerant.  V-A.2b: the carrier
	 * keeps that bookkeeping exact for auto arenas.
	 */
	vma = corten_arena_anchor_vma(ar);

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

	/* [perf1] No route-level gather here either: the zap below owns its
	 * mmu_gather per window and skips it entirely for windows without
	 * translations, so the MAP_FIXED refill of a never-touched chunk
	 * (the unmap-virt recycle shape) pays no tlb_flush_pending traffic
	 * and cannot trigger tlb_finish_mmu()'s mm_tlb_flush_nested()
	 * full-mm upgrade against a concurrent chunk zap.
	 *
	 * [atomic-sleep fix] The flush rounds and the per-window finish run
	 * outside the desc write lock, same as the chunk-zap driver.
	 */
	while (start < end) {
		unsigned long win_end = min((start | (PMD_SIZE - 1)) + 1, end);
		struct corten_zap_win zw = { .addr = start, };
		struct corten_txn txn;
		int tries;

		for (;;) {
			zw.force = false;
			tries = 0;
			for (;;) {
				ret = corten_lock_range(mm, start,
							win_end - start,
							&txn);
				if (ret != -EAGAIN || ++tries >= 2)
					break;
			}

			switch (ret) {
			case 0:
				/* Discard whatever the range held (legacy MAP_FIXED
				 * semantics); the fresh allocation is marked
				 * below, under the lock of the final round.
				 */
				ret = corten_arena_zap_window(mm, vma, &txn,
							      &zw, win_end,
							      NULL,
							      CORTEN_UNMAP_KEEP_PERM);
				if (!ret && !zw.force) {
					/* Window fully zapped under this
					 * lock: mark it now (the round's
					 * transaction covers the window).
					 */
					int mret;

					mret = corten_mark(&txn, start,
							   win_end - start,
							   &meta);
					corten_unlock(&txn);
					if (mret) {
						ret = mret;
						goto out_win;
					}
					this_cpu_inc(READ_ONCE(mm->corten_state)->stats[
							CORTEN_ARENA_STAT_MMAP_MARK_TXNS]);
					break;
				}
				corten_unlock(&txn);
				/* [atomic-sleep fix] flush after the lock
				 * drop, before the walk resumes.
				 */
				if (zw.force)
					tlb_flush_mmu(&zw.tlb);
				if (ret)
					goto out_win;
				continue;	/* resume the walk */
			case -ENOENT:
				/* Cannot happen after the fill pass above. */
				WARN_ON_ONCE(1);
				ret = -EOPNOTSUPP;
				fallthrough;
			default:
				goto out_win;
			}

			break;
		}

out_win:
		/* [atomic-sleep fix] The window's lazy gather is finished
		 * here -- outside the desc write lock (the last round
		 * dropped it before reaching this point), on every exit
		 * path.
		 */
		if (zw.have_tlb)
			tlb_finish_mmu(&zw.tlb);

		if (ret)
			goto out;

		start = win_end;
	}

out:
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
		 * CORTEN_SWAPPED joins the recorded set in M6.T2 (spec
		 * D7): the perm rewrite is pure metadata (a swap PTE
		 * carries no hardware permission bits), and the pending
		 * perm takes effect when the swap-in transaction re-arms
		 * the PTE from m2.perm.  STUB-family states (no M3
		 * producer) are left alone.
		 */
		recorded = corten_query(txn, addr, &m) == 0;
		if (recorded && (m.state == CORTEN_INVALID ||
				 m.state == CORTEN_PRIVATE_ANON ||
				 m.state == CORTEN_MAPPED ||
				 m.state == CORTEN_SWAPPED)) {
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
				/* The shared zero page must never
				 * become writable: drop the
				 * translation and let
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
			/* V-A.1: vma may be NULL (a VMA-less
			 * reactivated window) -- the modify
			 * wrappers only pass through vma->vm_mm.
			 */
			if (vma) {
				newpte = ptep_modify_prot_start(vma, addr, ptep);
				newpte = pte_modify(newpte, new_pgprot);
				/* Preserve write on
				 * already-writable pages (no
				 * spurious refault); a downgrade
				 * clears the write bit through the
				 * pgprot, an upgrade on a
				 * read-only page self-heals through
				 * the fault path's
				 * CORTEN_DISP_RESTORE (mkwrite+dirty).
				 */
				if ((perm & CORTEN_PERM_WRITE) &&
				    pte_write(cur))
					newpte = pte_mkwrite(newpte,
							     vma);
				ptep_modify_prot_commit(vma, addr,
							ptep, cur,
							newpte);
			} else {
				newpte = ptep_get_and_clear(mm, addr,
							    ptep);
				newpte = pte_modify(newpte, new_pgprot);
				if ((perm & CORTEN_PERM_WRITE) &&
				    pte_write(cur))
					newpte = pte_mkwrite_novma(newpte);
				set_pte_at(mm, addr, ptep, newpte);
			}
flush_this:
			if (!*flushed) {
				*flush_start = addr;
				*flushed = true;
			}
			*flush_end = addr + PAGE_SIZE;
		}
		/* !pte_present: M6.T2 -- a swap PTE has no permission
		 * bits to rewrite; the pending perm is already recorded
		 * above and takes effect at swap-in.  Nothing to
		 * re-protect here.
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

	/* V-A.1: @vma may be NULL (a VMA-less reactivated window); every
	 * derived value below has its pure-metadata form.
	 */
	new_pgprot = vma ? corten_arena_perm_pgprot(vma, perm) :
			   corten_arena_perm_pgprot_pure(perm);

	/* Secondary-MMU parities: the legacy funnel wraps
	 * change_protection() in notifier invalidation; the arena walk
	 * writes the same PTEs and owes the same courtesy.
	 */
	mmu_notifier_range_init(&range, MMU_NOTIFY_PROTECTION_VMA, 0,
				mm, start, end);
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
	if (flushed) {
		if (vma)
			flush_tlb_range(vma, flush_start, flush_end);
		else
			flush_tlb_mm_range(mm, flush_start, flush_end,
					   PAGE_SHIFT, false);
	}
	mmu_notifier_invalidate_range_end(&range);

	if (ret)
		return ret;

	if (whole) {
		/* The whole arena changed protection: move the upper
		 * bound the FRESH fault gate reads (lockless, under the
		 * active-ref barrier).  V-A.1: the shadow-VMA flag
		 * re-encoding only applies when the arena still has one
		 * (fresh-declared windows); a VMA-less window's
		 * non-PTE consumers own the metadata alone.
		 */
		WRITE_ONCE(ar->prot, perm);

		if (vma) {
			vm_flags_t flags = vma->vm_flags &
					   ~(VM_READ | VM_WRITE | VM_EXEC);

			if (perm & CORTEN_PERM_READ)
				flags |= VM_READ;
			if (perm & CORTEN_PERM_WRITE)
				flags |= VM_WRITE;
			if (perm & CORTEN_PERM_EXEC)
				flags |= VM_EXEC;
			/* vm_flags_set() only ORs bits in: a downshift must
			 * clear the R/W/X bits the old encoding carried
			 * before the re-assignment, or stale permissions
			 * outlive the call.
			 */
			vm_flags_clear(vma, VM_READ | VM_WRITE | VM_EXEC);
			vm_flags_set(vma, flags | VM_SOFTDIRTY);
			WRITE_ONCE(vma->vm_page_prot, new_pgprot);
		}
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

	/* V-A.1: the anchor is optional -- a VMA-less (reactivated)
	 * arena routes mprotect as a pure metadata perm rewrite (the PTE
	 * re-encodings and the flush have VMA-free forms).  V-A.2b: the
	 * carrier restores the precise ranged flush/notifier shapes for
	 * auto arenas.
	 */
	vma = corten_arena_anchor_vma(ar);

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
 * V-A.2a follow-up: materialize one page of the mremap-move copy
 * through the arena's own fault entry.  The move's two ranges are
 * VMA-less (the target is a direct declare, an auto source carries no
 * tree VMA), and a kernel uaccess fault never reaches the arena hook
 * -- the x86 entry gates it on user_mode(regs), so an unmaterialized
 * page would take the exception-table fixup and copy_to_user() would
 * answer -EFAULT for a perfectly legal grow.  Faulting in here is
 * exactly the work the copy's page faults would have done: the
 * destination at write intent (fresh exclusive pages through FRESH),
 * the source at read intent (the copy only reads it, so the
 * SHARED/COW state and the parent's marks are preserved).  A page
 * already present is left alone (the walk is a pure skip check, no
 * COW side effects).  Swapped source pages go through the swap-in leg
 * of the same entry.
 * Return: 0 = the page is resident, -EFAULT = the arena refused it
 * (permission contract, untracked window).
 */
static int corten_arena_mremap_prefault(struct mm_struct *mm,
					unsigned long start,
					unsigned long len, bool write)
{
	unsigned long addr;

	for (addr = start; addr < start + len; addr += PAGE_SIZE) {
		unsigned int fflags = write ? FAULT_FLAG_WRITE : 0;
		pmd_t *pmdp;

		pmdp = corten_arena_pmd(mm, addr);
		if (pmdp && pmd_present(READ_ONCE(*pmdp)) &&
		    !pmd_leaf(READ_ONCE(*pmdp))) {
			pte_t *ptep;
			spinlock_t *ptl;

			ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
			if (ptep) {
				bool present = pte_present(ptep_get(ptep));

				pte_unmap_unlock(ptep, ptl);
				if (present)
					continue;
			}
		}

		if (corten_arena_user_fault(mm, addr, 0, NULL, &fflags) !=
		    CORTEN_FAULT_HANDLED)
			return -EFAULT;
	}

	return 0;
}

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
	unsigned long len2, addr2, ncopy;
	long ret;
	u8 perm;

	/* OVERCOMMIT_NEVER does not honour MAP_NORESERVE, so the new VMA
	 * would carry VM_ACCOUNT and fail the DECLARE validation
	 * wholesale (DEV-12, same posture as the auto-mmap route).
	 */
	if (sysctl_overcommit_memory == OVERCOMMIT_NEVER)
		return -EOPNOTSUPP;

	perm = READ_ONCE(ar->prot);
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

	/* C2 (review): the pre-A.2a route installed the target through
	 * do_mmap(NULL, ..., MAP_FIXED), whose mmap_region() arm forces
	 * may_expand_vm() on the full new length ("Check against address
	 * space limit") with the old mapping still charged; the direct
	 * declare below carries the charge but not the gate.  Run it here,
	 * same lock, same full-len2 shape -- RLIMIT_AS pressure now
	 * answers -ENOMEM exactly like the legacy flow did (and like the
	 * auto route's corten_auto_validate() does before placement).
	 */
	if (!may_expand_vm(mm, corten_take_vm_flags(perm),
			   len2 >> PAGE_SHIFT)) {
		mmap_write_unlock(mm);
		return -ENOMEM;
	}

	/* V-A.2a: the move target is a direct VMA-less declare (the
	 * carrier + frames + metadata, the old arena's recorded perm as
	 * the contract) -- the do_mmap()+declare detour that installed a
	 * plain VMA at the fresh slot only to have DECLARE shadowize it
	 * is gone, along with the VMA itself.  The copy below faults
	 * through the new arena's FRESH path exactly as before.
	 */
	ret = corten_arena_declare_locked(mm, state, addr2, len2, perm, NULL,
					  0, true);
	if (ret) {
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
	if (corten_arena_mremap_prefault(mm, addr2, ncopy, true) ||
	    corten_arena_mremap_prefault(mm, addr, ncopy, false) ||
	    copy_to_user((void __user *)addr2, (void __user *)addr, ncopy)) {
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
		/* V-A.3c hot-path sample: the shrink's chunk zap ran; the
		 * route is lockless here (self-sufficient walker form).
		 */
		corten_audit_j2_sample(mm);
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

	/* V-A.3c hot-path sample: the move retired the old window and
	 * declared the new one; lockless here (self-sufficient form).
	 */
	corten_audit_j2_sample(mm);

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
	if (!corten_enabled_static() || !state)
		return false;
	/* T1c: parked arenas do not count as arena state (their range is
	 * a plain anonymous reservation); with the whole population
	 * parked the fast negation must not fire either.
	 */
	if (!refcount_read(&state->nr) && !READ_ONCE(state->nr_pool))
		return false;
	if (!len)
		return false;

	rcu_read_lock();
	xa_for_each_range(&state->arenas, frame, ar, start >> PMD_SHIFT,
			  (start + len - 1) >> PMD_SHIFT) {
		if (ar == &corten_va_reserve_sentinel || READ_ONCE(ar->idle))
			continue;
		if (start < ar->end && start + len > ar->start) {
			ret = true;
			break;
		}
	}
	rcu_read_unlock();

	return ret;
}

/*
 * V-A.3a placement truth (MV_VMA_FREE_SPEC.md sec 1.2, audit #14-#17):
 * same walk as corten_arena_range_overlaps(), but EVERY registered arena
 * frame counts -- live and parked (idle) ones alike, plus (for the
 * NOREPLACE gate) the magazine reserve sentinels.  The parked case is why
 * this exists: A.1 removed the reservation VMA, so after a park the range
 * is tree-free yet still owned (the registry is the only occupancy truth
 * for the window domain), and a placement decision that trusts the tree
 * alone installs a foreign VMA on frames a later reactivation would hand
 * out again.  The sentinel is slot-keyed (its own bounds are meaningless),
 * so an index hit IS the overlap.  Fast negation keeps the two-condition
 * shape: with the whole population parked the registry is still full.
 *
 * The callers are the placement funnels (do_mmap's NOREPLACE gate,
 * __mmap_prepare's backstop), all under mmap_write -- serialized against
 * seg_claim/park/eject writers, so the lockless RCU walk sees a stable
 * registry.  No reference is taken: only @start/@end/@idle are read, and
 * the descriptor is freed by kfree_rcu().
 */
bool corten_arena_range_occupied_incl_idle(struct mm_struct *mm,
					   unsigned long start,
					   unsigned long len)
{
	struct corten_mm_state *state;
	struct corten_arena *ar;
	unsigned long frame;
	bool ret = false;

	state = READ_ONCE(mm->corten_state);
	if (!corten_enabled_static() || !state)
		return false;
	/* Placement semantics: parked frames are spoken-for VA exactly like
	 * live ones -- the whole-population-parked case must not fast-negate
	 * (mirrors the T1c fix in range_overlaps() above).
	 */
	if (!refcount_read(&state->nr) && !READ_ONCE(state->nr_pool))
		return false;
	if (!len)
		return false;

	rcu_read_lock();
	xa_for_each_range(&state->arenas, frame, ar, start >> PMD_SHIFT,
			  (start + len - 1) >> PMD_SHIFT) {
		if (ar == &corten_va_reserve_sentinel) {
			/* Slot-keyed marker: the iteration index is the frame
			 * it claims -- that IS the overlap.
			 */
			ret = true;
			break;
		}
		if (start < ar->end && start + len > ar->start) {
			ret = true;
			break;
		}
	}
	rcu_read_unlock();

	return ret;
}

/*
 * V-A.3a P3: the zero-VMA arm of the __mmap_prepare() backstop
 * (mm/vma.c).  The placement guards upstream (do_mmap's NOREPLACE
 * -EEXIST, the punch route's idle-eject) make a hit unreachable; this
 * "rings, never answers" shape is deliberate -- returning -EOPNOTSUPP
 * with a disclosed counter is strictly better than letting the gather
 * install a mapping over registered frames.  Unlike the NOREPLACE gate
 * this walk does NOT count magazine reserve sentinels: a sentinel under
 * a legacy VMA is a legal, tolerated shape (the magazine re-verifies
 * marker + tree at every serve, the T0 obstacle contract), and counting
 * them here would veto exactly the plain-MAP_FIXED-over-claimed-frames
 * mappings that contract exists for.  mmap_write context.
 *
 * V-A.3c: the empty-window mark arm below now runs regardless of the
 * registry's arena population (and of the registry existing at all).
 * A MODE mm with an empty or absent registry -- every arena punched
 * out, or an ENTER-only process -- still owns the window domain, and
 * its MAP_FIXED hole installs were escaping registration through the
 * early returns (mark itself creates the registry when absent).  The
 * A.3b gate fix covered only the populated shape.
 */
bool corten_arena_placement_backstop(struct mm_struct *mm,
				     unsigned long start, unsigned long len)
{
	struct corten_mm_state *state;
	struct corten_arena *ar;
	unsigned long frame;

	if (!corten_enabled_static())
		return false;

	state = READ_ONCE(mm->corten_state);
	if (state && len &&
	    (refcount_read(&state->nr) || READ_ONCE(state->nr_pool))) {
		rcu_read_lock();
		xa_for_each_range(&state->arenas, frame, ar,
				  start >> PMD_SHIFT,
				  (start + len - 1) >> PMD_SHIFT) {
			if (ar == &corten_va_reserve_sentinel)
				continue;
			if (start < ar->end && start + len > ar->start) {
				rcu_read_unlock();
				atomic_long_inc(&corten_nr_placement_backstop);
				return true;
			}
		}
		rcu_read_unlock();
	}

	/* The empty-window legal arm: no arena frames anywhere in range,
	 * so the legacy funnel below will place an ordinary VMA inside
	 * the window domain (the smoke contract's mmap-fixed-at-window
	 * shape).  Register it as an implant right here -- under this
	 * mm's mmap_write like every producer -- so the fault terminus
	 * (corten_fault_window_maperr) and the A.3c INV-MV2 walker both
	 * see the registry as the window domain's complete occupancy
	 * truth.  A range that later fails to install leaves a stale
	 * entry whose only effect is a tree lookup that answers the
	 * same legacy MAPERR (harmless; the walker's fallback predicate
	 * collects it).  implant_mark clips to the window itself.
	 */
	if (READ_ONCE(mm->corten_mode) && len &&
	    start < CORTEN_MODE_WINDOW_END &&
	    start + len > CORTEN_MODE_WINDOW_START)
		corten_implant_mark(mm, start, len);

	return false;
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
 * V-A.3d S-5③ predicate (audit #20, D24): is [start, start+len) a
 * fully-parked window span -- entirely inside the window domain, every
 * 2M frame registered, and every registered frame idle (or a magazine
 * reserve sentinel, the claim-window marker that carries no content
 * either)?  This is the exact VA population the A.1 reservation VMA
 * used to describe: PROT_NONE, anonymous, content dropped at park.
 * Anything else -- an active frame (the existing route arms own it), a
 * hole frame, an implant frame -- returns false and the caller keeps
 * the legacy verdict.  RCU xarray walk like range_overlaps(); the
 * per-frame completeness check is a count comparison, so a sentinel
 * occupying an otherwise idle span still classifies as parked.
 */
static bool corten_arena_window_parked_span(struct mm_struct *mm,
					    unsigned long start,
					    unsigned long len)
{
	struct corten_mm_state *state;
	struct corten_arena *ar;
	unsigned long frame, expect, found = 0;

	state = READ_ONCE(mm->corten_state);
	if (!state)
		return false;
	if (len & ~PAGE_MASK)
		return false;
	if (start < CORTEN_MODE_WINDOW_START ||
	    start + len > CORTEN_MODE_WINDOW_END ||
	    start + len <= start)
		return false;

	expect = ((start + len - 1) >> PMD_SHIFT) - (start >> PMD_SHIFT) + 1;

	rcu_read_lock();
	xa_for_each_range(&state->arenas, frame, ar, start >> PMD_SHIFT,
			  (start + len - 1) >> PMD_SHIFT) {
		if (ar != &corten_va_reserve_sentinel &&
		    !READ_ONCE(ar->idle)) {
			/* An active frame: the route arms below own it,
			 * this is not S-5 land.
			 */
			found = 0;
			break;
		}
		found++;
	}
	rcu_read_unlock();

	return found == expect;
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

	/* V-A.3d S-5③ (audit #20, D24): the parked-window terminal.  A
	 * fully-parked span is the exact VA the A.1 reservation VMA used
	 * to describe, and madvise() on that shape answered 0 -- the
	 * legacy walk after A.1 answers -ENOMEM instead (an unregistered
	 * regression) and burns a window find_vma_prev() plus a
	 * lock_vma_under_rcu() per call (the J1 pollution #20).  Answer
	 * the contract behaviours here: the content-drops are semantic
	 * no-ops (park dropped the content), the hints were already
	 * no-op successes on the live arm.  Every other behaviour keeps
	 * the legacy verdict (-ENOMEM on the walk) -- the disclosed S-5
	 * residual row.
	 */
	if (READ_ONCE(mm->corten_mode) && len &&
	    corten_arena_window_parked_span(mm, start, len)) {
		switch (behavior) {
		case MADV_DONTNEED:
		case MADV_DONTNEED_LOCKED:
		case MADV_FREE:
		case MADV_NORMAL:
		case MADV_SEQUENTIAL:
		case MADV_RANDOM:
		case MADV_COLD:
			atomic_long_inc(&corten_nr_madvise_parked);
			return 1;
		default:
			break;
		}
	}

	switch (behavior) {
	case MADV_DONTNEED:
	case MADV_DONTNEED_LOCKED:
		ret = corten_arena_dontneed_route(mm, start, len);
		/* V-A.3c hot-path sample: the transactional zap rewrote
		 * the window's content picture (self-sufficient form --
		 * whatever madvise_lock() holds is DEV-13-legal).
		 */
		if (ret == 1)
			corten_audit_j2_sample(mm);
		return ret;

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
		if (ret == 1) {
			atomic_long_inc(&corten_nr_madvise_free_txns);
			corten_audit_j2_sample(mm);
		}
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
		/* V-A.3c hot-path sample (no tree mutation on this arm --
		 * the walk is the periodic re-proof the sampling knob
		 * exists for).
		 */
		corten_audit_j2_sample(mm);
		return 1;

	default:
		break;
	}

	return corten_arena_range_overlaps(mm, start, len) ? -EOPNOTSUPP : 0;
}

/* ------------------------------------------------------------------ *
 * V-A.3d S-5 (j2-audit #13/#20/#23/#28, the D24 verdicts): the query
 * syscalls whose window-domain legs A.1 turned from "anonymous no-op"
 * into -ENOMEM.  Each terminal below restores the pre-A.1 baseline for
 * the *occupied* window (active or parked frames); holes and implants
 * keep the legacy verdict -- a hole was -ENOMEM before A.1 too, and an
 * implant is a real tree VMA the legacy funnel must serve.
 * ------------------------------------------------------------------
 */

/*
 * S-5① (audit #23): advance @start past the leading registered-window
 * segment, if any.  sys_msync() calls this before each find_vma(): a
 * registered frame span is the reservation-VMA population of old, and
 * msync() was a 0-returning no-op on it (anonymous, no file, nothing
 * to invalidate).  The scan walks the registry frame by frame, so a
 * punched-out (implant) or never-declared (hole) frame stops it and
 * the loop's legacy machinery takes over from there -- mixed ranges
 * keep legacy's "process everything, -ENOMEM for the gaps" shape with
 * the occupied segments contributing neither work nor error.  Caller
 * holds mmap_read (serialised against every registry writer); pure
 * function of (mm, start, end), returns the possibly advanced start.
 */
unsigned long corten_arena_msync_skip(struct mm_struct *mm,
				      unsigned long start,
				      unsigned long end)
{
	struct corten_mm_state *state;
	unsigned long fend, f;

	state = READ_ONCE(mm->corten_state);
	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode) || !state)
		return start;
	if (start < CORTEN_MODE_WINDOW_START ||
	    start >= CORTEN_MODE_WINDOW_END || start >= end)
		return start;

	fend = min(end, CORTEN_MODE_WINDOW_END);

	rcu_read_lock();
	for (f = start >> PMD_SHIFT; (f << PMD_SHIFT) < fend; f++) {
		struct corten_arena *ar = xa_load(&state->arenas, f);

		if (!ar)
			break;
	}
	rcu_read_unlock();

	f <<= PMD_SHIFT;
	if (f <= start)
		return start;

	atomic_long_inc(&corten_nr_msync_window_skips);
	return min(f, fend);
}

/*
 * S-5② (audit #13, D24: the OQ-MV-11 truth walk, delivered early): the
 * mincore(2) window terminal.  For a chunk whose every window frame is
 * registered: parked frames answer 0 (the reservation-VMA vector --
 * content is gone, no page would fault in), active frames answer the
 * REAL residency vector read straight off the page tables (the window
 * has no VMA for walk_page_range() to anchor on post-A.2, so the route
 * walks the PMD-gated path itself; the arena model guarantees one PMD
 * entry per 2M frame and no THP leaves inside the window, C20).  Any
 * unregistered frame (hole or implant) returns -EAGAIN and do_mincore()
 * walks the legacy tree -- the implant's own pages keep their tree
 * truth, the hole keeps its legacy -ENOMEM.  Read-only in the INV6
 * sense: PTEs are read under the ptl, nothing is written.
 * Return: the number of vector bytes filled, or -EAGAIN for legacy.
 */
static unsigned char corten_mincore_swap_truth(swp_entry_t entry)
{
	struct swap_info_struct *si;
	struct folio *folio;

	/* The anon arm of mincore_swap(): non-swap entries (migration,
	 * hwpoison) are uptodate by definition; a real entry is resident
	 * iff the swap cache holds an uptodate folio for it.  The extra
	 * device gate is the shmem arm's toll: production PTEs imply a
	 * live device, but the walk answers (never crashes) on any entry
	 * shape the window can carry -- a type with no device reads 0.
	 */
	if (non_swap_entry(entry))
		return 1;
	if (!IS_ENABLED(CONFIG_SWAP))
		return 0;
	si = get_swap_device(entry);
	if (!si)
		return 0;
	put_swap_device(si);
	folio = swap_cache_get_folio(entry);
	if (folio && !xa_is_value(folio)) {
		unsigned char p = folio_test_uptodate(folio);

		folio_put(folio);
		return p;
	}
	return 0;
}

static void corten_mincore_fill_frame(struct mm_struct *mm,
				      unsigned char *vec,
				      unsigned long start,
				      unsigned long end)
{
	unsigned long nr = (end - start) >> PAGE_SHIFT;
	pmd_t *pmdp;
	pte_t *ptep;
	spinlock_t *ptl;	/* the PTE lock pairing the read walk */
	pmd_t pmd;

	pmdp = corten_arena_pmd(mm, start);
	if (!pmdp)
		goto zero;
	pmd = READ_ONCE(*pmdp);
	if (!pmd_present(pmd))
		goto zero;
	if (unlikely(pmd_leaf(pmd))) {
		/* C20 structurally excludes THP from the window; a leaf
		 * here can only be a foreign shape -- mirror
		 * mincore_pte_range()'s THP answer (all resident).
		 */
		memset(vec, 1, nr);
		return;
	}

	ptep = pte_offset_map_lock(mm, pmdp, start, &ptl);
	if (!ptep)
		goto zero;
	for (; start != end; start += PAGE_SIZE, vec++, ptep++) {
		pte_t pte = ptep_get(ptep);

		if (pte_none_mostly(pte))
			*vec = 0;	/* never faulted: the anon 0 */
		else if (pte_present(pte))
			*vec = 1;
		else
			*vec = corten_mincore_swap_truth(pte_to_swp_entry(pte));
	}
	pte_unmap_unlock(ptep - 1, ptl);
	cond_resched();
	return;

zero:
	memset(vec, 0, nr);
}

long corten_arena_mincore_route(struct mm_struct *mm, unsigned long addr,
				unsigned long pages, unsigned char *vec)
{
	struct corten_mm_state *state;
	unsigned long end, f, fend, off = 0;
	int active = 0;

	state = READ_ONCE(mm->corten_state);
	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode) || !state)
		return -EAGAIN;
	/* Chunks start inside the window only (do_mincore() re-enters per
	 * PAGE_SIZE chunk, so a range crossing the window edge is served
	 * as [window part routed][rest legacy]).
	 */
	if (!pages || addr < CORTEN_MODE_WINDOW_START ||
	    addr >= CORTEN_MODE_WINDOW_END)
		return -EAGAIN;
	end = addr + (pages << PAGE_SHIFT);
	fend = min(end, CORTEN_MODE_WINDOW_END);

	/* Every window frame of the chunk must be registered; classify
	 * each frame on the way (parked -> zero vector, active -> truth
	 * walk) so one call can carry a mixed active/parked span.
	 */
	rcu_read_lock();
	for (f = addr >> PMD_SHIFT; (f << PMD_SHIFT) < fend; f++) {
		struct corten_arena *ar = xa_load(&state->arenas, f);

		if (!ar) {
			rcu_read_unlock();
			return -EAGAIN;	/* hole or implant: legacy tree */
		}
		if (ar != &corten_va_reserve_sentinel && !READ_ONCE(ar->idle))
			active++;
	}
	rcu_read_unlock();

	atomic_long_inc(&corten_nr_mincore_routes);
	if (!active) {
		memset(vec, 0, pages);
		return pages;
	}

	for (f = addr >> PMD_SHIFT; (f << PMD_SHIFT) < fend; f++) {
		unsigned long fs = f << PMD_SHIFT;
		unsigned long fe = min(fs + PMD_SIZE, fend);
		struct corten_arena *ar;
		bool idle;

		rcu_read_lock();
		ar = xa_load(&state->arenas, f);
		idle = !ar || ar == &corten_va_reserve_sentinel ||
		       READ_ONCE(ar->idle);
		rcu_read_unlock();

		fs = max(fs, addr);
		if (idle)
			memset(vec + off, 0, (fe - fs) >> PAGE_SHIFT);
		else
			corten_mincore_fill_frame(mm, vec + off, fs, fe);
		off += (fe - fs) >> PAGE_SHIFT;
	}

	return pages;
}

/*
 * S-5 #28 (audit #28): the move_pages(2) query leg's cheap window
 * short-circuit.  do_pages_stat_array() vma_lookup()s every page; on a
 * MODE mm every window address except an implant is a guaranteed miss
 * (-EFAULT, the errno the audit recorded for both sides of A.1), so
 * answer it without the tree walk.  Implant-covered addresses return
 * false and take the lookup: their VMA is real and the nid is the
 * truth.  Caller holds the target mm's mmap_read (covers() contract).
 */
bool corten_arena_move_pages_window(struct mm_struct *mm, unsigned long addr)
{
	if (!corten_enabled_static() || !READ_ONCE(mm->corten_mode) ||
	    addr < CORTEN_MODE_WINDOW_START || addr >= CORTEN_MODE_WINDOW_END)
		return false;
	if (corten_implant_covers(mm, addr, 1))
		return false;

	atomic_long_inc(&corten_nr_move_pages_window);
	return true;
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

/* ------------------------------------------------------------------ *
 * M6.T1: reclaim-path guards (M6_RMAP_SPEC.md sec 1.3 V1/V2, sec 2.1
 * D1/D3).  The arena page is "full swap-out shape" (swapbacked, anon
 * rmap, memcg-charged) but deliberately off the LRU, so reclaim only
 * reaches its PTEs through two side doors: the OOM reaper's
 * unmap_page_range() (V1, reachable today) and the rmap walkers
 * try_to_unmap_one()/try_to_migrate_one() via the shadow-VMA's
 * anon_vma (V2, latent until something puts arena folios on the LRU).
 * Both write arena PTEs bare -- no descriptor write lock, no metadata
 * update -- which leaves meta=CORTEN_MAPPED behind a none PTE
 * (INV6/INV7 broken, a false restore WARN on the next fault).  These
 * guards close both doors without moving arena pages: every arena
 * content transition still happens inside a transaction.
 */

/**
 * corten_oom_reap_skip_vma - the OOM reaper's shadow-VMA predicate (V1).
 * @vma: the VMA the reaper is about to unmap_page_range().
 *
 * D3 of the spec sketched an MMF_UNSTABLE fault gate so that the reaper
 * could keep unmapping arena pages while new transactions refuse to
 * start.  That shape still lets the in-flight unmap race a transaction
 * that predates the flag (ptl-serialised, but the survivor shape is a
 * half-updated window).  The reaper is best-effort by contract and its
 * victim is SIGKILL'd anyway: skipping the VMA outright keeps every
 * arena PTE transaction-owned (zero tearing, by construction rather
 * than by arbitration), at the cost of freeing the arena's memory a few
 * moments later in exit_mmap(), whose teardown is already the accepted,
 * transaction-ordered carve-out (corten_arena_mm_exit() drain + legacy
 * unmap_vmas).  The reaper's actual job -- freeing legacy anonymous
 * memory of the victim so the OOM can make progress -- is untouched for
 * every non-corten VMA of the same mm, including the plain mmap()s of a
 * MODE process; a whole-mm skip was considered and rejected precisely
 * because it would withhold that relief.
 *
 * Counted (reap_skips) so the reclaim-latency report can quantify how
 * much memory waits for exit under OOM.
 *
 * Return: true if @vma is a shadow-VMA and must be skipped.
 */
bool corten_oom_reap_skip_vma(struct vm_area_struct *vma)
{
	if (!(vma->vm_flags & VM_CORTEN))
		return false;

	atomic_long_inc(&corten_nr_reap_skips);
	return true;
}

/*
 * The W1.e2 shape gate for the ttu walkers (the M6.T2 prefilter, V2,
 * spec sec 2.1 D1, flipped by W1_NATIVE_RMAP_SPEC.md sec 3.2): refuse
 * every shape the driver cannot or must not take, before any lock or
 * notifier traffic (the refusal writes nothing, so secondary-MMU users
 * see nothing -- the T1 posture for the declined shapes is unchanged).
 * What changed with the flip: the shape is no longer served by the M6
 * completion arm (retired) but routed to the native swap-out driver,
 * which owns the entry leg itself -- so the old "no swap entry"
 * refusal is retired with it and a swapcache-less folio is now a
 * routable shape.
 *
 * @migrate distinguishes the try_to_migrate_one() caller: the ttu_flags
 * alone cannot (migrate.c passes plain TTU_BATCH_FLUSH/0, the same
 * encoding an unmap uses).  Migration semantics (remap the PTE to a
 * migration entry) is Stage-3 scope (OQ-M6-3) and is refused here; the
 * isolation side of kernel migration never sees arena folios anyway
 * (no LRU, compaction skips !LRU, migrate_pages is routed off).
 *
 * Return: true = the shape is the plain exclusive-unmap swap-out and
 * the walker hands the folio to corten_swap_out_driver_ttu() (the
 * transfer contract documented there); false = declined, the walker
 * aborts this VMA without writing anything.  Counted (rmap_rejects)
 * on every refusal -- after the flip this counter is the unreachable
 * backstop's ledger: production has no ttu path to a window's
 * anonymous page any more, and the KUnit anchors hold it at zero.
 */
bool corten_rmap_unmap_one(struct folio *folio, struct vm_area_struct *vma,
			   unsigned long address, enum ttu_flags flags,
			   bool migrate)
{
	if (!(vma->vm_flags & VM_CORTEN))
		return false;

	/* The VMA-flag test is the gate: VM_CORTEN is only ever set by
	 * shadowize on a corten=on kernel, so no separate static-branch
	 * read is needed in here.
	 */
	if (migrate || (flags & TTU_HWPOISON) ||
	    (!(flags & TTU_IGNORE_MLOCK) && (vma->vm_flags & VM_LOCKED)) ||
	    folio_maybe_dma_pinned(folio) ||
	    folio_nr_pages(folio) != 1) {
		atomic_long_inc(&corten_nr_rmap_rejects);
		return false;
	}

	return true;
}

/*
 * The M6.T2 completion arm (spec sec 2.1 D1 steps 4-6): swap one arena
 * page out transactionally.  Called from try_to_unmap_one() inside its
 * mmu_notifier invalidate window (R6-2), still under the walker's folio
 * lock and reference; the swap entry and its cache membership were
 * created by the caller before try_to_unmap() (shrink_folio_list's
 * folio_alloc_swap(), or the eviction driver's), so the transaction
 * itself only takes desc->lock(W, BH) > ptl -- DEV-13 direction, zero
 * new lock classes inside the critical section (D4), no allocation
 * except the GFP_NOWAIT-proof metadata (already ensured by the
 * producing fault).
 *
 * The PTE work mirrors try_to_unmap_one()'s swap-entry branch
 * (rmap.c:2120-2201) bit for bit -- swap_duplicate(), arch_unmap_one(),
 * the anon-exclusive try-share, the mmlist registration, the
 * ANONPAGES/SWAPENTS counter move, the exclusive/soft-dirty/uffd-wp
 * encoding -- with one deliberate ordering difference: the metadata
 * transition (corten_swap_out()) commits under the same ptl, before the
 * entry's PTE reference is shared with anybody.  The set_pte_at() below
 * is the corten_glue_pte_write whitelist entry #2 (docs/DESIGN.md sec 3):
 * a legacy-path writer, accepted because every write it makes is
 * mirrored into the metadata by the transaction that contains it.
 *
 * @defer/@old_pte carry the walker's should_defer_flush() verdict out:
 * when batching, the flush_tlb_range() is skipped and the walker does
 * the upstream set_tlb_ubc_flush_pending() bookkeeping with the cleared
 * PTE.  Registering after the transaction (instead of between the clear
 * and the swap-PTE store as upstream does) is safe: both operations
 * flush by address, the ordering requirement is
 * register-happens-after-clear, and the swap PTE installed in between is
 * read only through the ptl this function holds (R6-1).
 *
 * Every abort path restores the original PTE under the same ptl and
 * reports false -- the folio stays resident, exactly the T1 posture.
 *
 * Return: true = swapped out (PTE holds @entry, metadata =
 * CORTEN_SWAPPED with the entry encoded in __resv).
 */
bool corten_rmap_swap_out(struct folio *folio, struct vm_area_struct *vma,
			  unsigned long address, bool defer, pte_t *old_pte)
{
	struct mm_struct *mm = vma->vm_mm;
	struct page *page = folio_page(folio, 0);
	struct corten_pte_meta m, sm;
	struct corten_txn txn;
	bool anon_exclusive;
	pmd_t *pmdp;
	pte_t *ptep, pteval, swp_pte;
	spinlock_t *ptl;
	swp_entry_t entry = folio->swap;
	int ret;

	ret = corten_lock_range(mm, address, PAGE_SIZE, &txn);
	if (unlikely(ret)) {
		/* -EAGAIN/-ENOMEM/-ENOENT: the descriptor tree is in
		 * transition (or the window was never tracked).  Not
		 * ours to fix under reclaim; leave the folio mapped.
		 */
		atomic_long_inc(&corten_nr_rmap_rejects);
		return false;
	}
	if (unlikely(corten_query(&txn, address, &m))) {
		corten_unlock(&txn);
		atomic_long_inc(&corten_nr_rmap_rejects);
		return false;
	}

	/* The fork-shared shape is OQ-M6-2: both sides' entries would
	 * need coordinated swap-out with COW-fault convergence.  A
	 * shared slot keeps its page resident until the COW
	 * transactions resolve it (or exit zaps it).
	 */
	if (m.state != CORTEN_MAPPED || (m.flags & CORTEN_PF_SHARED)) {
		corten_unlock(&txn);
		atomic_long_inc(&corten_nr_rmap_rejects);
		return false;
	}

	pmdp = corten_arena_pmd(mm, address);
	if (unlikely(!pmdp)) {
		corten_unlock(&txn);
		atomic_long_inc(&corten_nr_rmap_rejects);
		return false;
	}
	ptep = pte_offset_map_lock(mm, pmdp, address, &ptl);
	if (unlikely(!ptep)) {
		corten_unlock(&txn);
		atomic_long_inc(&corten_nr_rmap_rejects);
		return false;
	}

	pteval = ptep_get(ptep);
	if (unlikely(!pte_present(pteval) || pte_special(pteval) ||
		     pte_pfn(pteval) != folio_pfn(folio))) {
		/* Race loser: the page moved under the walker's feet
		 * between the rmap walk and the transaction (fault
		 * retry, zap, COW copy).  The metadata and whatever PTE
		 * is there now form their own consistent pair -- just
		 * get out.
		 */
		pte_unmap_unlock(ptep, ptl);
		corten_unlock(&txn);
		atomic_long_inc(&corten_nr_rmap_rejects);
		return false;
	}

	/* --- committed to the swap-out from here (mirrors rmap.c) --- */
	anon_exclusive = PageAnonExclusive(page);

	flush_cache_range(vma, address, address + PAGE_SIZE);
	pteval = ptep_get_and_clear(mm, address, ptep);
	if (!defer)
		flush_tlb_range(vma, address, address + PAGE_SIZE);
	/* @defer: the walker registers the deferred flush after this
	 * transaction returns (set_tlb_ubc_flush_pending() is an
	 * rmap.c static; see the R6-1 note in the header comment).
	 */
	if (pte_dirty(pteval))
		folio_mark_dirty(folio);

	if (unlikely(swap_duplicate(entry) < 0)) {
		set_pte_at(mm, address, ptep, pteval);
		pte_unmap_unlock(ptep, ptl);
		corten_unlock(&txn);
		atomic_long_inc(&corten_nr_rmap_rejects);
		return false;
	}
	if (unlikely(arch_unmap_one(mm, vma, address, pteval) < 0)) {
		swap_free(entry);
		set_pte_at(mm, address, ptep, pteval);
		pte_unmap_unlock(ptep, ptl);
		corten_unlock(&txn);
		atomic_long_inc(&corten_nr_rmap_rejects);
		return false;
	}
	/* See folio_try_share_anon_rmap(): clear PTE first (done). */
	if (unlikely(anon_exclusive &&
		     folio_try_share_anon_rmap_pte(folio, page))) {
		swap_free(entry);
		set_pte_at(mm, address, ptep, pteval);
		pte_unmap_unlock(ptep, ptl);
		corten_unlock(&txn);
		atomic_long_inc(&corten_nr_rmap_rejects);
		return false;
	}
	if (unlikely(list_empty(&mm->mmlist))) {
		spin_lock(&mmlist_lock);
		if (list_empty(&mm->mmlist))
			list_add(&mm->mmlist, &init_mm.mmlist);
		spin_unlock(&mmlist_lock);
	}
	update_hiwater_rss(mm);
	dec_mm_counter(mm, MM_ANONPAGES);
	inc_mm_counter(mm, MM_SWAPENTS);

	swp_pte = swp_entry_to_pte(entry);
	if (anon_exclusive)
		swp_pte = pte_swp_mkexclusive(swp_pte);
	if (likely(pte_present(pteval))) {
		if (pte_soft_dirty(pteval))
			swp_pte = pte_swp_mksoft_dirty(swp_pte);
		if (pte_uffd_wp(pteval))
			swp_pte = pte_swp_mkuffd_wp(swp_pte);
	}
	/* Glue whitelist entry #2 (docs/DESIGN.md sec 3, DEV-11(b)): the
	 * reclaim walker's swap-PTE install, contained and mirrored by
	 * this transaction.
	 */
	set_pte_at(mm, address, ptep, swp_pte);
	/* No pte_install_uffd_wp_if_needed(): userfaultfd is excluded at
	 * shadowize (VM_UFFD_* rejected, arena.c:535), so no marker can
	 * be pending for a shadow-VMA.
	 */

	folio_remove_rmap_ptes(folio, page, 1, vma);
	folio_put_refs(folio, 1);

	/* The metadata transition, same critical section: state
	 * MAPPED -> SWAPPED, perm kept (the contract survives), COW
	 * flags scrubbed (no shared page reaches this arm), the entry
	 * encoded into __resv (spec D6).  corten_swap_out() cannot fail
	 * here (the slot is MAPPED and the array was ensured by the
	 * fault that mapped the page).
	 */
	sm.state = CORTEN_SWAPPED;
	sm.perm = m.perm;
	sm.flags = 0;
	corten_swap_encode(&sm, entry);
	if (WARN_ON_ONCE(corten_swap_out(&txn, address, &sm))) {
		/* Unreachable; the PTE/counters are committed and the
		 * metadata still says MAPPED -- the next fault of the
		 * swap-in path heals the pair (counted), the reclaim
		 * side still reports success.
		 */
		pte_unmap_unlock(ptep, ptl);
		corten_unlock(&txn);
		*old_pte = pteval;
		return true;
	}
	pte_unmap_unlock(ptep, ptl);
	corten_unlock(&txn);

	*old_pte = pteval;
	atomic_long_inc(&corten_nr_swapped_out);
	return true;
}

/*
 * W1.d (W1_NATIVE_RMAP_SPEC.md sec 3.2): the single-page file demotion
 * -- the ttu hook's worker.  The slot address was derived from
 * folio->index and the region record by corten_rmap_ttu(); this
 * transaction verifies the slot really maps @folio and removes exactly
 * that mapping, the reclaim-family twin of the B.2 truncate gate's
 * chunk transaction.
 *
 * Lock shape (DEV-13 direction, the corten_rmap_swap_out() one): the
 * caller opened the mmu_notifier invalidate window before the desc lock
 * (R6-2 -- the removal is invisible to secondary-MMU users otherwise;
 * the window may sleep, so it never nests the desc write lock), then
 * desc->lock(W, BH) > ptl inside.  No allocation, no I/O.
 *
 * Slot admission mirrors the FILE_EVENT spare of the B.2 zap (V-B.3):
 * only a CORTEN_FILE_MAPPED slot whose PTE still maps this pagecache
 * folio (pfn identity) is the reclaim's business.  A MAPPED/SWAPPED
 * slot is a private COW copy -- its content is not the file's anymore
 * and reclaim of the pagecache folio must not touch it (its own anon
 * folio is the anchored ttu anon family's); anything else (INVALID
 * from a truncate demote, a refilled generation, a hole) simply does
 * not map this folio.
 *
 * The commit is the read arm's (corten_arena_file_read(), R4) exact
 * inverse inside the transaction: PTE clear + TLB flush (the M6
 * arm's flush_cache/flush_tlb shape, no deferral -- OQ-W1-1's
 * non-defer first version), the dirty bit propagated to the folio
 * (upstream's file branch), mm_counter_file() -1, the novma mapcount
 * return and the PTE's folio reference.  The metadata is deliberately
 * NOT written: CORTEN_FILE_MAPPED is both the virtual allocation and
 * the resident form (the page identity is derivable from the region
 * record), so the demoted slot IS the FILE_MAPPED record and the next
 * fault dispatches FILE_READ -- it re-reads the pagecache (old content
 * or a refilled generation, never anonymous zeros).
 *
 * Return: 1 = demoted one window mapping; 0 = this slot is not a
 * mapping of @folio (declined inside the transaction, nothing written).
 */
static int corten_rmap_ttu_file_one(struct corten_arena *ar,
				    unsigned long addr, struct folio *folio,
				    enum ttu_flags flags)
{
	struct mm_struct *mm = ar->mm;
	struct vm_area_struct *vma;
	struct corten_txn txn;
	struct corten_pte_meta m;
	struct mmu_notifier_range range;
	pmd_t *pmdp;
	pte_t *ptep, pteval;
	spinlock_t *ptl;
	int demoted = 0;

	/* The mlock contract, the M6.T1 prefilter's shape: a locked
	 * window keeps its mappings until the reclaim passes
	 * TTU_IGNORE_MLOCK (unmap_poisoned_folio's shape).  The carrier
	 * carries the VM_LOCKED verdict the same way it carries the
	 * committed perm.
	 */
	vma = READ_ONCE(ar->carrier);
	if (!(flags & TTU_IGNORE_MLOCK) && vma &&
	    (vma->vm_flags & VM_LOCKED)) {
		atomic_long_inc(&corten_nr_rmap_rejects);
		return 0;
	}

	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, mm,
				addr, addr + PAGE_SIZE);
	mmu_notifier_invalidate_range_start(&range);

	if (corten_lock_range(mm, addr, PAGE_SIZE, &txn))
		goto out_range;
	if (corten_query(&txn, addr, &m))
		goto out_unlock;
	if (m.state != CORTEN_FILE_MAPPED)
		goto out_unlock;

	/* The demotion is a PTE removal: it needs the carrier for the
	 * cache/tlb flush shapes.  A live FILE region has one (the
	 * parked/frozen windows were skipped by the enumeration).
	 */
	vma = corten_arena_anchor_vma(ar);
	if (unlikely(!vma)) {
		WARN_ONCE(1, "corten: live FILE region without a carrier\n");
		goto out_unlock;
	}

	pmdp = corten_arena_pmd(mm, addr);
	if (unlikely(!pmdp))
		goto out_unlock;
	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	if (unlikely(!ptep))
		goto out_unlock;

	pteval = ptep_get(ptep);
	if (unlikely(!pte_present(pteval) || pte_special(pteval) ||
		     pte_pfn(pteval) != folio_pfn(folio))) {
		/* Not this folio's mapper: a COW copy (skipped by the
		 * state check above only while the metadata is honest),
		 * a racing fault's new generation, or a demoted slot
		 * with a legacy-fallback PTE.  Leave the slot alone --
		 * the folio's own mapcount verdict stays exact.
		 */
		pte_unmap_unlock(ptep, ptl);
		corten_unlock(&txn);
		mmu_notifier_invalidate_range_end(&range);
		return 0;
	}

	/* Committed (mirrors try_to_unmap_one()'s file branch): clear
	 * + flush, dirty propagation, counter, rmap, PTE reference.
	 */
	flush_cache_range(vma, addr, addr + PAGE_SIZE);
	pteval = ptep_get_and_clear(mm, addr, ptep);
	flush_tlb_range(vma, addr, addr + PAGE_SIZE);
	if (pte_dirty(pteval))
		folio_mark_dirty(folio);

	update_hiwater_rss(mm);
	add_mm_counter(mm, mm_counter_file(folio), -1);
	/* W1.d (R4 inverse): the file mapcount is the pagecache folio's
	 * own, returned through the novma wrapper inside this
	 * transaction -- exactly what the read arm's install borrowed.
	 */
	folio_remove_file_rmap_novma(folio);
	folio_put(folio);
	/* The metadata stays CORTEN_FILE_MAPPED: no __resv payload, no
	 * transition to record (the read arm never wrote one either).
	 */

	pte_unmap_unlock(ptep, ptl);
	demoted = 1;
out_unlock:
	corten_unlock(&txn);
out_range:
	mmu_notifier_invalidate_range_end(&range);
	if (demoted)
		this_cpu_inc(READ_ONCE(mm->corten_state)->stats[
				CORTEN_ARENA_STAT_UNMAP_PAGES]);
	return demoted;
}

/*
 * W1.d (W1_NATIVE_RMAP_SPEC.md sec 3.2): the try_to_unmap() layer's
 * vma-free hook -- the file side's true routing.  A pagecache folio
 * mapped by an arena window has no i_mmap member behind the window
 * (carriers stopped joining the tree in W1.b), so the rmap walk that
 * follows in try_to_unmap() sees only the legacy mappers and the
 * window's borrowed mapcount would keep the folio resident forever --
 * the structural exclusion W1.d replaces.  This hook enumerates the
 * folio's (mapping, index) through the per-inode registry -- the same
 * source and the same derivation the B.2 truncate gate uses (three
 * consumers, one registry) -- and hands every region that covers
 * folio->index to corten_rmap_ttu_file_one().
 *
 * The caller's contract is try_to_unmap()'s: folio locked and
 * referenced, so mapping/index are stable.  After the hook, the
 * ordinary rmap walk handles the legacy mappers and folio_not_mapped()
 * delivers the upstream verdict over the shared mapcount: the
 * demotions here participate in it through the novma wrapper, there is
 * no corten-specific return channel.
 *
 * Declines (the M6 exclusion postures, each deliberately silent unless
 * counted):
 *   - anon folios: W1.e2 routed them at the _one guard instead --
 *     try_to_unmap_one() hands them to the native driver inside the
 *     walk (the walk is what discovers their address); this hook's
 *     registry derivation is file-shaped and stays out of the way,
 *     file-COW copies included -- they are anon folios.
 *   - large folios: every window install is order-0 (the novma
 *     wrapper's contract); this also keeps the TTU_RMAP_LOCKED
 *     callers (split/collapse hold i_mmap_rwsem themselves) out of
 *     the read lock below.
 *   - TTU_HWPOISON: the poison path needs its own entry semantics
 *     (kill markers, M6's loud refusal), not a silent demotion --
 *     refused and counted, the folio stays mapped and the hwpoison
 *     caller keeps its -EBUSY terminal state.
 *
 * Lock order: i_mmap_rwsem(read) > notifier window > desc->lock(W) >
 * ptl -- the B.2/W1.b declared edge (F8) with the M6 window shape
 * nested one step deeper; nothing climbs back (a transaction never
 * acquires i_mmap or a notifier), so the composition is acyclic and
 * zero new lock classes.
 */
void corten_rmap_ttu(struct folio *folio, enum ttu_flags flags)
{
	struct corten_inode_regions *reg;
	struct address_space *mapping;

	if (!corten_enabled_static() || folio_test_anon(folio) ||
	    folio_test_large(folio))
		return;

	mapping = folio->mapping;
	if (!mapping)
		return;

	/* The fast gate: a mapping without corten regions -- the whole
	 * legacy world and every corten=off process -- costs one xarray
	 * probe and returns.  The probe is a peek: the authoritative
	 * load happens under the registry's lock (the erase side holds
	 * it write), so a concurrently dying registry head is never
	 * dereferenced.
	 */
	if (!xa_load(&corten_inode_regions, (unsigned long)mapping))
		return;

	if (flags & TTU_HWPOISON) {
		atomic_long_inc(&corten_nr_rmap_rejects);
		return;
	}

	i_mmap_lock_read(mapping);
	reg = xa_load(&corten_inode_regions, (unsigned long)mapping);
	if (!reg)
		goto out_unlock;

	{
		struct corten_arena *ar;
		unsigned long routes = 0;
		pgoff_t index = folio->index;

		list_for_each_entry(ar, &reg->regions, rfile_node) {
			unsigned long npages =
				(ar->end - ar->start) >> PAGE_SHIFT;
			unsigned long addr;

			if (index < ar->rpoff ||
			    index >= ar->rpoff + npages)
				continue;	/* not this region's page */

			addr = ar->start + ((index - ar->rpoff) <<
					    PAGE_SHIFT);
			/* The enumeration's lifetime rules (W1.b): a
			 * parked window owns no mapping and a frozen one
			 * belongs to the fork snapshot; the active ref
			 * extends the read hold's pin past the section.
			 */
			if (READ_ONCE(ar->idle) || READ_ONCE(ar->frozen))
				continue;
			if (!percpu_ref_tryget_live(&ar->active))
				continue;
			routes += corten_rmap_ttu_file_one(ar, addr, folio,
							   flags);
			percpu_ref_put(&ar->active);
		}
		atomic_long_add(routes, &corten_nr_ttu_routes);
	}
out_unlock:
	i_mmap_unlock_read(mapping);
}

/*
 * M6.T2 swapoff parity (spec sec 1.2 P12): unuse_pte() (swapfile.c) is a
 * legacy PTE writer on shadow-VMAs -- the glue whitelist entry #3.  It
 * reads a swap PTE, reads the page back from the device and re-installs
 * a present PTE; without a metadata mirror the slot would stay
 * CORTEN_SWAPPED behind a resident page (the swap-in path would then
 * re-read an entry the PTE no longer references).  This helper commits
 * the mirror BEFORE unuse_pte() takes its ptl, so the DEV-13 direction
 * (desc write lock > ptl) holds and the swap-in self-heal below covers
 * only the tiny race window between the two.
 */
int corten_swapin_sync_meta(struct mm_struct *mm, unsigned long addr,
			    swp_entry_t entry, struct folio *folio)
{
	struct corten_txn txn;
	struct corten_pte_meta m, nm;
	int ret;

	ret = corten_lock_range(mm, addr, PAGE_SIZE, &txn);
	if (ret)
		return ret == -EAGAIN ? -EAGAIN : -ENOMEM;

	if (corten_query(&txn, addr, &m) ||
	    m.state != CORTEN_SWAPPED ||
	    corten_swap_decode(&m).val != entry.val) {
		/* Not a swapped arena slot (or already healed): the PTE
		 * below belongs to a shape the legacy code fully owns.
		 */
		corten_unlock(&txn);
		return 0;
	}

	nm = m;
	nm.state = CORTEN_MAPPED;
	nm.flags = 0;
	ret = corten_map(&txn, addr, folio_page(folio, 0), m.perm,
			 CORTEN_MAP_FORCE);
	corten_unlock(&txn);
	return ret;
}

/*
 * W1.e1 (W1_NATIVE_RMAP_SPEC.md sec 3.4): the native anonymous swap-out
 * driver -- the shrinker pick's replacement for the
 * __reclaim_pages()-through-ttu detour.  The upstream chain was
 * "pick(addr) -> shrink_folio_list -> try_to_unmap re-discovers addr via
 * the rmap walk -> the M6.T2 completion arm"; the only thing rmap
 * contributed was handing the driver's own address back to the driver.
 * This function deletes the detour: it owns the whole lifecycle, entry
 * allocation to release, with the pick (address + folio) as its input.
 *
 * R-W1-1 mitigation: every leg is a bit-for-bit mirror of the
 * shrink_folio_list() anonymous arm it replaces (mm/vmscan.c, line
 * numbers re-measured at W1.e1 -- the spec's 1360-1420 citation drifts
 * only inside the arm, not at its edges):
 *
 *   vmscan.c     upstream leg                     -> driver leg
 *   1174         folio_trylock                    -> folio_trylock (moved in;
 *                                                   the caller contract is the
 *                                                   pick reference only)
 *   1360-1365    anon+swapbacked, __GFP_IO, pin   -> shape gates (the IO gate
 *                                                   is caller-enforced: the
 *                                                   shrinker bails without
 *                                                   __GFP_IO at scan_objects,
 *                                                   evict is admin-driven)
 *   1366-1378    large-folio split                -> unreachable: the pick
 *                                                   gate admits order-0 only
 *   1379         folio_alloc_swap(__GFP_HIGH|     -> same call (one step in
 *                __GFP_NOWARN), add to swapcache     this tree: entry + memcg
 *                                                   charge + swap_cache_add_
 *                                                   folio = folio->swap)
 *   1402-1413    MADV_FREE mark_dirty             -> folio_mark_dirty
 *   1442-1470    try_to_unmap + folio_mapped      -> THE M6.T2 TRANSACTION,
 *                verdict                             corten_rmap_swap_out()
 *                                                   UNCHANGED (INV6: the PTE/
 *                                                   metadata writes stay in
 *                                                   the transaction body; the
 *                                                   driver only opens the
 *                                                   notifier window around it,
 *                                                   the walker's R6-2 duty)
 *   1479-1481    post-unmap pin recheck           -> same
 *   1483         mapping = folio_mapping()        -> same (swapcache space)
 *   1484-1567    pageout/writeout                 -> folio_clear_dirty_for_io
 *                (dirty -> clear-for-io ->            + swap_writeout(folio,
 *                swap_writepage, sync writes          NULL) (the aops form of
 *                settle inline)                       the same leg, mm/swap.h);
 *                                                     zram's sync write settles
 *                                                     inline, so PAGE_SUCCESS
 *                                                     falls to the relock
 *   1593-1613    buffers/private release          -> unreachable: arena anon
 *                                                     folios carry no private
 *   1615-1631    lazyfree arm, __remove_mapping   -> lazyfree unreachable
 *                (refcount freeze 2, decache,        (swapbacked enforced);
 *                put_swap_folio)                     __remove_mapping() itself
 *   1635-1647    free_it: unqueue_deferred_split, -> same four calls, batch
 *                uncharge, flush, free_unref         of one
 *   1659-1677    activate/keep + folio_free_swap  -> keep_locked arm; the
 *                on keep                             activate bookkeeping is
 *                                                    LRU-family (DEV-10:
 *                                                    excluded), the pick
 *                                                    reference is dropped
 *                                                    instead of putback
 *
 * Reference ledger (the safety net ttu used to be): the pick carries one
 * reference; folio_alloc_swap adds the swapcache one (2 total = upstream's
 * isolation + cache).  The transaction drops the PTE reference
 * (folio_put_refs inside corten_rmap_swap_out).  __remove_mapping() then
 * freezes exactly 2 (cache + pick = upstream's cache + isolation) and the
 * free batch releases the pick.  On any keep the driver drops the pick
 * itself -- no folio_putback_lru(), ever (DEV-10).
 *
 * Lock order (D4, zero new edges): folio trylock > [swap cluster lock] >
 * notifier window > desc->lock(W, BH) > ptl.  The entry allocation and
 * the swapcache insertion complete BEFORE the descriptor lock, the
 * notifier window never nests the (sleepable) window under the desc lock,
 * and the transaction body is untouched.
 *
 * Return: true = the folio was freed (its content lives behind the swap
 * PTE the transaction installed); false = kept resident, pick reference
 * dropped, metadata and PTE consistent for the next pass.
 */
bool corten_swap_out_driver(struct mm_struct *mm, unsigned long addr,
			    struct folio *folio)
{
	struct mmu_notifier_range range;
	struct corten_arena *ar;
	struct vm_area_struct *vma;
	struct address_space *mapping;
	struct folio_batch fbatch;
	pte_t corten_pte;
	bool swapped;

	/* vmscan.c:1174.  The trylock failure is the upstream "keep" (a
	 * concurrent actor -- the fault path -- owns the folio right now).
	 */
	if (!folio_trylock(folio))
		goto keep;

	/* vmscan.c:1360-1365.  The order-0 gate mirrors both the pick
	 * budget and the ttu prefilter's folio_nr_pages() check; the
	 * large-folio arm (1366-1378) is structurally unreachable.
	 */
	if (!folio_test_anon(folio) || !folio_test_swapbacked(folio) ||
	    folio_nr_pages(folio) != 1 || folio_maybe_dma_pinned(folio))
		goto keep_locked;

	/* The carrier, for the flush shapes and the mlock verdict (the
	 * corten_rmap_ttu_file_one() resolve shape): the active reference
	 * pins the arena, and the carrier cannot be freed or flag-changed
	 * under it.  A lookup loss here is a torn window the pick raced
	 * with -- keep, the state is somebody else's transaction now.
	 */
	ar = corten_arena_lookup_get(mm, addr);
	if (!ar)
		goto keep_locked;
	vma = corten_arena_anchor_vma(ar);
	if (unlikely(!vma)) {
		percpu_ref_put(&ar->active);
		goto keep_locked;
	}
	/* The M6.T1 prefilter's mlock shape (upstream reclaim respects
	 * VM_LOCKED at ttu -- no TTU_IGNORE_MLOCK in shrink_folio_list's
	 * flags): a locked window keeps its page.  Counted in the same
	 * ledger the walker refusal used.
	 */
	if (vma->vm_flags & VM_LOCKED) {
		percpu_ref_put(&ar->active);
		atomic_long_inc(&corten_nr_rmap_rejects);
		goto keep_locked;
	}

	/* --- vmscan.c:1360-1414, the entry + swapcache leg.  One call in
	 * this tree: folio_alloc_swap() allocates the entry, charges the
	 * memcg and adds the folio to the swapcache (folio->swap set, one
	 * reference added).  On failure the folio stays mapped and clean
	 * of swap state -- the 1379 abort, minus the THP split retry that
	 * cannot apply to an order-0 pick.
	 */
	if (!folio_test_swapcache(folio)) {
		if (folio_alloc_swap(folio, __GFP_HIGH | __GFP_NOWARN)) {
			percpu_ref_put(&ar->active);
			goto keep_locked;
		}
		/* vmscan.c:1402-1413: the PTE will be dirty by the time
		 * its content matters; the unconditional mark makes the
		 * MADV_FREE corner (dirty bit lost without a lock) a
		 * non-issue exactly like upstream.
		 */
		folio_mark_dirty(folio);
	}

	/* The walker's notifier window (R6-2), driver-owned: the removal
	 * must be invisible to secondary-MMU users.  Opened before the
	 * transaction, closed after -- it never nests the desc lock (the
	 * window may sleep).
	 */
	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, mm,
				addr, addr + PAGE_SIZE);
	mmu_notifier_invalidate_range_start(&range);

	/* --- vmscan.c:1442-1470 replaced by THE transaction (INV6).  No
	 * defer: the driver is its own batch boundary and flushes inline
	 * (the OQ-W1-1 non-defer first version, same resolution W1.d
	 * took; @corten_pte is the defer bookkeeping's scratch and stays
	 * unwritten on the flush-now shape).
	 */
	swapped = corten_rmap_swap_out(folio, vma, addr, false, &corten_pte);

	mmu_notifier_invalidate_range_end(&range);
	percpu_ref_put(&ar->active);

	if (!swapped)
		goto keep_locked;
	/* Committed: the PTE holds the swap entry, the metadata says
	 * CORTEN_SWAPPED, the folio is unmapped (mapcount returned) and
	 * carries the cache + pick references (2).
	 */

	/* vmscan.c:1479-1481. */
	if (folio_maybe_dma_pinned(folio))
		goto keep_locked;

	/* vmscan.c:1483.  For a swapcache folio this is the swap space's
	 * address_space.
	 */
	mapping = folio_mapping(folio);

	/* --- vmscan.c:1484-1567, the pageout/writeout leg in the
	 * swapcache-sync shape.  swap_writeout() is the aops->writepage
	 * this branch would reach: it takes the folio_free_swap()
	 * entry-reuse shortcut, arch_prepare_to_swap(), the zeromap and
	 * zswap arms, and unlocks the folio on every path that submitted
	 * I/O (folio_start_writeback + unlock inside, exactly upstream).
	 */
	if (folio_test_dirty(folio)) {
		int res;

		if (!folio_clear_dirty_for_io(folio))
			goto remove;	/* PAGE_CLEAN: nothing to write */
		folio_set_reclaim(folio);
		res = swap_writeout(folio, NULL);
		if (res == AOP_WRITEPAGE_ACTIVATE) {
			/* Still locked and dirty (the zswap-disabled arm
			 * marked it): upstream clears the reclaim hint and
			 * activates -- the LRU half is DEV-10-excluded.
			 */
			folio_clear_reclaim(folio);
			goto keep_locked;
		}
		if (res < 0) {
			/* The handle_write_error() shape (folio locked,
			 * error recorded, unlocked); the error arm marked
			 * the folio dirty, so the keep below preserves the
			 * data exactly like the upstream dirty re-check.
			 */
			folio_lock(folio);
			if (folio_mapping(folio) == mapping)
				mapping_set_error(mapping, res);
			folio_unlock(folio);
			goto keep;
		}
		/* PAGE_SUCCESS (folio unlocked).  The writeback requeue
		 * arm only exists for async devices -- the driver's sync
		 * shape (zram settles inline, zero/zswap arms never set
		 * writeback) leaves it mirroring-only -- but the checks
		 * are upstream's, so they stay.
		 */
		if (!folio_test_writeback(folio))
			folio_clear_reclaim(folio);
		if (folio_test_writeback(folio))
			goto keep;
		if (folio_test_dirty(folio))
			goto keep;
		/* The synchronous-write relock (vmscan.c:1558-1563). */
		if (!folio_trylock(folio))
			goto keep;
		if (folio_test_dirty(folio) || folio_test_writeback(folio))
			goto keep_locked;
		mapping = folio_mapping(folio);
	}

remove:
	/* --- vmscan.c:1629, the removal.  The lazyfree arm (1615) is
	 * unreachable -- swapbacked was enforced at the gates -- so the
	 * freeze protocol is __remove_mapping()'s own: refcount 2 (cache
	 * + pick) frozen, decache, put_swap_folio, memcg swapout
	 * bookkeeping, workingset shadow.  Failure (a racing reference --
	 * a concurrent swap-in of the fresh entry is the only taker) is
	 * the upstream keep: the folio stays in the swapcache, the entry
	 * stays shared, and the next pass converges (the healed MAPPED
	 * slot re-picks; the swapcache member short-circuits the entry
	 * leg at the folio_test_swapcache() check above).
	 */
	if (!mapping || !__remove_mapping(mapping, folio, true, NULL))
		goto keep_locked;

	/* vmscan.c:1633-1647, free_it, batch of one.  The frozen folio's
	 * last reference is the pick; free_unref_folios() releases it.
	 * try_to_unmap_flush() mirrors the batch drain -- nothing is
	 * pending (this driver flushes inline), the call is the same
	 * cheap static check upstream runs.
	 */
	folio_unlock(folio);
	folio_unqueue_deferred_split(folio);
	folio_batch_init(&fbatch);
	folio_batch_add(&fbatch, folio);
	mem_cgroup_uncharge_folios(&fbatch);
	try_to_unmap_flush();
	free_unref_folios(&fbatch);

	atomic_long_inc(&corten_nr_driver_swapped);
	return true;

keep_locked:
	/* vmscan.c:1659-1663: reclaim the swap space of a kept swapcache
	 * folio when the cgroup is swap-tight (or the page mlocked --
	 * excluded at the gates, kept for the mirror).  On a pre-transaction
	 * keep this restores the plain mapped-anon shape; post-transaction
	 * the entry is PTE-referenced and folio_free_swap() is a no-op for
	 * it (the swap count is 2, not cache-only), so the arm cannot free
	 * a live entry here.
	 */
	if (folio_test_swapcache(folio) &&
	    (mem_cgroup_swap_full(folio) || folio_test_mlocked(folio)))
		folio_free_swap(folio);
	folio_unlock(folio);
keep:
	/* No folio_putback_lru() -- DEV-10.  The kept folio's pick
	 * reference is dropped here.
	 */
	folio_put(folio);
	atomic_long_inc(&corten_nr_driver_kept);
	return false;
}

/*
 * W1.e2 (W1_NATIVE_RMAP_SPEC.md sec 3.2): the from-ttu entry arm -- the
 * flipped guard in try_to_unmap_one() hands the window's context (mm,
 * address, folio) over to the W1.e1 driver here.  The shape mapping is
 * by flags:
 *
 *   - TTU_HWPOISON and the migration caller never reach this arm: the
 *     guard's backstop refuses them first (WARN + rmap_rejects), the
 *     M6 exclusion postures unchanged.
 *   - The mlock verdict is flags-blind and stays with the driver: a
 *     TTU_IGNORE_MLOCK caller asking for a locked window's page gets
 *     the safe keep (counted in rmap_rejects by the driver) rather
 *     than the ignore it asked for -- reclaim must never widen what a
 *     ttu shape may take.
 *   - The defer-flush family (TTU_BATCH_FLUSH and friends) maps to the
 *     driver's inline flush: this arm is its own batch boundary (the
 *     OQ-W1-1 non-defer first version, the W1.d resolution), so no
 *     rmap.c tlb_ubc bookkeeping happens on this route.
 *
 * The caller contract is a transfer: ttu hands the folio over locked
 * and referenced, and this arm consumes both -- the lock is released
 * for the driver's trylock leg, and the reference becomes the pick
 * reference the driver's ledger consumes on every arm (success frees
 * the folio, every keep drops it).  A ttu caller that expects its lock
 * and reference back would be broken by design; none exists -- after
 * the W1.e2 shrinker rewire no production path feeds a window's
 * anonymous page to ttu any more (off-LRU, migration and hwpoison
 * isolation-gated), the arm is the walk's route for whatever arrives
 * anyway, and the double-entry arbitration with the shrinker's own
 * driver runs is the folio trylock: whoever wins swaps, the loser
 * keeps, both converge (KUnit-anchored, guest stress gate).
 *
 * Return: the driver's verdict, true = swapped out (folio freed).
 */
bool corten_swap_out_driver_ttu(struct mm_struct *mm, unsigned long addr,
				struct folio *folio, enum ttu_flags flags)
{
	/* The transfer's input contract (the debug face of it): a folio
	 * handed over unlocked is a caller bug -- keep it intact (WARN
	 * plus recovery: nothing is consumed on this arm) rather than
	 * trip folio_unlock's VM_BUG_ON one call later.
	 */
	if (WARN_ON_ONCE(!folio_test_locked(folio)))
		return false;

	/* The lock is ttu's contribution to the handover; the reference
	 * stays and plays the pick's role in the driver's ledger.
	 */
	folio_unlock(folio);

	return corten_swap_out_driver(mm, addr, folio);
}

/*
 * M6.T2 eviction driver (spec slice table: the debugfs "evict N pages"
 * entry -- T2's minimal shrink stub; the pressure channel, mm registry
 * and victim aging are M6.T3 per spec D2).  Candidate selection walks
 * the arenas' windows: metadata CORTEN_MAPPED (content, not the zero
 * page), COW-unshared (OQ-M6-2), not DMA-pinned, present PTE.
 *
 * W1.e1: the picked folios go to the native swap-out driver
 * (corten_swap_out_driver() above) as (addr, folio) pairs -- the shrinker
 * pick always knew the address, and the __reclaim_pages() detour through
 * try_to_unmap existed only to hand it back through the rmap walk.  The
 * transaction, writeout and removal stay the upstream-mirrored legs
 * inside the driver; the ttu machinery is no longer reachable from the
 * evict path.  W1.e2 rewired the shrinker pressure leg the same way:
 * both channels -- shrinker pick and evict -- drain through the driver,
 * and the guard's flipped anon arm routes whatever ttu arrivals exist
 * to the same driver (corten_swap_out_driver_ttu()), so every swap-out
 * of a window's anonymous page converges on one transaction; the
 * folio trylock arbitrates concurrent entries and the M6 guard's
 * reject arm remains only as the unreachable WARN backstop.
 *
 * The old private-cookie kept-folio contract (folio_putback_lru() is
 * forbidden, DEV-10) moved into the driver's keep arms, which drop the
 * pick reference themselves and count it in driver_kept.
 *
 * Lock order: lookup_get (active ref, honors the fork freeze) >
 * corten_lock_range > ptl for the pick, all released before the driver
 * runs (folio_trylock inside; a pick that lost its mapping by then is
 * kept by the driver, not corrupted -- the transaction re-checks
 * everything under the covering lock).
 */
/* ------------------------------------------------------------------ *
 * M6.T3 shrinker pressure channel (M6_RMAP_SPEC.md sec 2.1 D2)
 *
 * Arena pages are reclaim-invisible by design (DEV-10: never anchored
 * on any LRU), so the pressure-side consumer of the M6.T2 swap
 * transaction is a shrinker -- the kernel's native reclaim channel for
 * non-LRU memory.  kswapd and direct reclaim both drive it through
 * shrink_slab(), which brings memcg scoping and priority for free;
 * what this code feeds it is the arena population of the registered
 * mms (the corten_mm_registry above).
 *
 * Victim selection is the two-pass young-bit aging transaction (spec
 * T3): pass 1 clears the accessed bits of one victim-window set -- the
 * PTE write mirrored from vmscan.c:3717's ptep_clear_young_notify(),
 * but under the covering desc write lock, because every arena PTE
 * write is a transaction (spec red line 1); pass 2, a later slice or
 * scan, swaps out the windows whose bits did NOT come back -- pages
 * nobody touched in between.  The per-window pass-1 flags live in
 * state->shrink_aged, so aging spans scan invocations: a window aged
 * by scan N is an evaluation candidate for scan N+1 (a linger-list
 * equivalent for a non-LRU population).  MGLRU keeps its structural
 * skip (spec sec 1.2 P2/P3): arena folios stay invisible to walk_mm
 * and eviction because folio_lru_gen() < 0, and nothing here adds any
 * folio_add_lru -- the shrinker is the only reclaim entry, and the
 * lru_gen walker has no arena-side hook to trip over.
 *
 * Batch shape (the T2 evict lesson, spec sec 2.2): victim selection
 * runs under the per-mm shrink trylock -- one picker per mm; two
 * pickers could isolate the same folio twice -- in bounded slices so
 * no spinlock hold is long, and every pick of a scan feeds one
 * (addr, folio) list.  Everything after the pick is the native
 * driver's (W1.e2: entry, the unchanged transaction, writeout, the
 * final unmap/free), drained with the lock dropped like kswapd's
 * batch.  The T2 stub was per-page slow because it re-scanned from
 * frame 0 on every debugfs call and paid a full window transaction per
 * pick; the rotation cursor and the shared walker amortize that to one
 * lock per window and one reclaim per scan.
 */

/* Victim-walk bounds: a shrink_lock hold spans one slice, capped by
 * both windows and candidate pages (each page costs one short ptl
 * section); the reclaim itself runs with the lock dropped.  The slice
 * count bounds one scan_objects() invocation.
 */
#define CORTEN_SHRINK_SLICE_WINDOWS	16
#define CORTEN_SHRINK_SLICE_PAGES	512
#define CORTEN_SHRINK_MAX_SLICES	2

/* One (address, folio) pick of the native swap-out driver (W1.e1 added
 * the struct for the evict leg, W1.e2 made it the shrinker leg's shape
 * too): the driver owns the address (the ttu detour existed to hand it
 * back) and consumes the pick's folio reference at drain time.
 */
struct corten_swap_pick {
	unsigned long addr;
	struct folio *folio;
	struct list_head link;
};

/* One shrinker victim walk over a window set.  @budget counts candidate
 * pages (MAPPED slots with a present PTE), not raw slots, so sparse
 * windows do not burn the scan allowance.  @picks collects the
 * isolated victims as (addr, folio) pairs for the native swap-out
 * driver (W1.e2: the shrinker leg's shape -- the ttu/`__reclaim_pages`
 * detour that used to hand the address back is gone, and the pick
 * itself owns the reference the driver consumes at drain time).
 */
struct corten_shrink_walk {
	struct list_head *picks;
	int budget;
	int nr_scanned;
	int nr_young;
	int nr_picked;
	/* pass 2: pick the pages whose young bit did not come back. */
	bool eval;
	/* evict driver: pick regardless of age; no epoch clearing. */
	bool force;
};

/* The pinned PT-page descriptor of @mm's 2M window at @addr, or NULL.
 * Caller holds rcu_read_lock() (the pin protocol); count-time use only
 * -- the counters are read as a READ_ONCE() snapshot.
 */
static struct corten_ptdesc *corten_arena_window_desc(struct mm_struct *mm,
						      unsigned long addr)
{
	struct corten_ptdesc *desc;
	pmd_t *pmdp, pmd;

	pmdp = corten_arena_pmd(mm, addr);
	if (!pmdp)
		return NULL;
	pmd = READ_ONCE(*pmdp);
	if (!pmd_present(pmd) || pmd_leaf(pmd))
		return NULL;
	desc = corten_ptdesc_get(page_to_pfn(pmd_page(pmd)));
	if (desc && (READ_ONCE(desc->stale) || desc->mm != mm)) {
		corten_ptdesc_put(desc);
		return NULL;
	}
	return desc;
}

/* Live resident/swapped totals of one registered mm (the M6.T3
 * count_objects basis and the M6.T4 swapped-page accounting).  One
 * short RCU section per window -- the T2 lesson: no long preempt-off
 * spans over a whole arena.
 */
static long corten_mm_state_pages(struct mm_struct *mm, long *swapped_out)
{
	struct corten_mm_state *state;
	unsigned long idx = 0;
	long resident = 0, swapped = 0;

	/* Pairs with the smp_store_release() publisher in
	 * corten_arena_state_create() (the same acquire every reader of
	 * the published registry uses).
	 */
	state = smp_load_acquire(&mm->corten_state);
	if (!state)
		goto out;

	for (;;) {
		struct corten_arena *arena;
		unsigned long wstart;
		struct corten_ptdesc *desc;

		rcu_read_lock();
		arena = xa_find(&state->arenas, &idx, ULONG_MAX, XA_PRESENT);
		if (!arena || arena == &corten_va_reserve_sentinel) {
			rcu_read_unlock();
			if (!arena)
				break;
			idx++;
			continue;
		}
		wstart = max(idx << PMD_SHIFT, READ_ONCE(arena->start));
		desc = corten_arena_window_desc(mm, wstart);
		if (desc) {
			resident += READ_ONCE(desc->nr_mapped);
			swapped += READ_ONCE(desc->nr_swapped);
			corten_ptdesc_put(desc);
		}
		rcu_read_unlock();
		idx++;
		cond_resched();
	}

out:
	if (swapped_out)
		*swapped_out = swapped;
	return resident;
}

/* The aging/pick transaction over one 2M window slice: candidate pages
 * get their young bit cleared (pass 1 and pass 2 -- vmscan.c:3717
 * mirrored, ptl nested under the covering desc write lock, same order
 * the MGLRU walker establishes for its ptep_clear_young_notify()) and,
 * in the pass-2/evict shapes, the cold (or any) ones are isolated as
 * (addr, folio) pairs onto @w->picks.  Lock failures skip the window:
 * a -EAGAIN is the Fig.7 descriptor transition, retried on a later
 * pass.
 */
static void corten_arena_shrink_walk(struct mm_struct *mm,
				     struct corten_arena *ar,
				     unsigned long fstart, unsigned long fend,
				     struct corten_shrink_walk *w)
{
	/* V-A.2b: the anchor -- a carrier arena's pages are rmap-anchored
	 * and the driver's verdict shapes resolve the carrier for them
	 * (the flush/mlock legs); a punched DECLARE arena whose piece
	 * cache went NULL stays unpickable (counted -- the conservative
	 * residue; F1 removed the A.1 anchor-less window this arm also
	 * used to carry).  The driver no longer needs rmap to find the
	 * address (W1.e2: no ttu leg), but an anchorless pick would
	 * deterministically keep at the carrier lookup -- not picking it
	 * saves the doomed trip; W-2 revisits when the driver
	 * de-carriers.
	 */
	struct vm_area_struct *vma = corten_arena_anchor_vma(ar);
	struct corten_txn txn;
	pmd_t *pmdp;
	unsigned long a;
	int ret;

	/* V-A.1: vma may be NULL (a VMA-less reactivated window) -- the
	 * walk is PT+metadata driven and only the aging notifier wants
	 * the vma; the no-vma arm notifies through the mm directly (no
	 * secondary-MMU user maps arena ranges, R6-2).
	 */
	ret = corten_lock_range(mm, fstart, fend - fstart, &txn);
	if (ret)
		return;	/* untracked / transitioning / -ENOMEM: next pass */

	pmdp = corten_arena_pmd(mm, fstart);
	if (pmdp) {
		for (a = fstart; a < fend && w->budget > 0; a += PAGE_SIZE) {
			struct corten_pte_meta m;
			pte_t *ptep, cur;
			spinlock_t *ptl;
			bool young;

			if (corten_query(&txn, a, &m) ||
			    m.state != CORTEN_MAPPED ||
			    (m.flags & CORTEN_PF_SHARED))
				continue;

			ptep = pte_offset_map_lock(mm, pmdp, a, &ptl);
			if (!ptep)
				continue;
			cur = ptep_get(ptep);
			if (!pte_present(cur) || pte_special(cur)) {
				pte_unmap_unlock(ptep, ptl);
				continue;
			}
			/* The aging PTE write inside the transaction.  In
			 * the evict shape (@force) the young bit is left
			 * alone: eviction does not consume epochs.
			 */
			young = false;
			if (!w->force) {
				if (vma) {
					young = ptep_clear_young_notify(vma,
									a,
									ptep);
				} else {
					young = pte_young(cur);
					if (young) {
						pte_t old = pte_mkold(cur);

						set_pte_at(mm, a, ptep, old);
						mmu_notifier_clear_young(mm,
									 a,
									 a +
									 PAGE_SIZE);
					}
				}
			}
			pte_unmap_unlock(ptep, ptl);

			w->nr_scanned++;
			w->budget--;
			if (young) {
				w->nr_young++;
				continue;
			}
			if (!w->eval && !w->force)
				continue;

			/* Isolation criteria (spec sec 3 red line 3,
			 * OQ-M6-2): unshared (checked above), unpinned,
			 * order-0, no writeback in flight.  A pick that
			 * loses its mapping before the drain is kept by
			 * the driver, not corrupted -- the transaction
			 * re-checks everything under the covering lock.
			 *
			 * V-A.1 boundary: a VMA-less arena's pages carry
			 * no rmap anchor and no carrier for the driver's
			 * verdict shapes -- such a pick would
			 * deterministically keep at the driver's carrier
			 * lookup, so the window stays unpickable (the
			 * pages stay resident until their munmap/exit,
			 * counted); the V-A.2b carrier restores
			 * reclaimability.
			 */
			if (!vma) {
				atomic_long_inc(&corten_nr_shrink_skipped);
				continue;
			}
			{
				/* W1.e2: every leg picks (addr, folio)
				 * pairs for the native driver -- the
				 * driver holds the address, so no LRU
				 * link is borrowed and no ttu walk is
				 * owed a mapping.  A NOWAIT entry
				 * failure just skips this pick (counted,
				 * reference returned).
				 */
				struct corten_swap_pick *p;
				struct folio *folio = page_folio(pte_page(cur));

				if (folio_nr_pages(folio) != 1 ||
				    folio_maybe_dma_pinned(folio) ||
				    folio_test_writeback(folio)) {
					atomic_long_inc(&corten_nr_shrink_skipped);
					continue;
				}
				folio_get(folio);
				p = kmalloc(sizeof(*p), GFP_NOWAIT);
				if (!p) {
					folio_put(folio);
					atomic_long_inc(&corten_nr_shrink_skipped);
					continue;
				}
				p->addr = a;
				p->folio = folio;
				list_add_tail(&p->link, w->picks);
				w->nr_picked++;
			}
		}
	}
	corten_unlock(&txn);
}

/* Pass 2 slice: evaluate up to @max_windows windows whose pass-1 flag
 * is set -- pick the pages that stayed cold, erase the flag (a hot
 * window is re-aged by a later pass 1; a swapped-out window has
 * nothing left).  Caller holds state->shrink_lock.
 */
static void corten_shrink_eval_slice(struct mm_struct *mm,
				     struct corten_mm_state *state,
				     struct corten_shrink_walk *w,
				     int max_windows)
{
	unsigned long idx = 0;
	int windows = 0;

	while (windows < max_windows && w->budget > 0) {
		struct corten_arena *arena;
		unsigned long key, fstart, fend;
		void *v;

		rcu_read_lock();
		v = xa_find(&state->shrink_aged, &idx, ULONG_MAX, XA_PRESENT);
		if (!v) {
			rcu_read_unlock();
			break;
		}
		key = idx;
		arena = xa_load(&state->arenas, key);
		if (!arena || arena == &corten_va_reserve_sentinel ||
		    !percpu_ref_tryget_live(&arena->active)) {
			rcu_read_unlock();
			xa_erase(&state->shrink_aged, key);
			idx = key + 1;
			windows++;
			continue;
		}
		rcu_read_unlock();

		xa_erase(&state->shrink_aged, key);
		w->eval = true;
		if (!READ_ONCE(arena->frozen) && !READ_ONCE(arena->idle)) {
			fstart = max(key << PMD_SHIFT, READ_ONCE(arena->start));
			fend = min((key << PMD_SHIFT) + PMD_SIZE,
				   READ_ONCE(arena->end));
			if (fstart < fend)
				corten_arena_shrink_walk(mm, arena, fstart,
							 fend, w);
		}
		percpu_ref_put(&arena->active);
		idx = key + 1;
		windows++;
	}
	w->eval = false;
}

/* Pass-1 rotation slice: age up to @max_windows fresh windows from the
 * per-mm rotation cursor, clear their young bits and flag them for a
 * later pass-2 evaluation.  The cursor wraps ACROSS scans only: when
 * the rotation is exhausted it restarts from frame 0 on a later slice
 * or scan, so one scan never ages a window twice (a window evaluated
 * in this scan is not re-flagged by the scan's own tail).  Caller holds
 * state->shrink_lock.
 */
static void corten_shrink_age_slice(struct mm_struct *mm,
				    struct corten_mm_state *state,
				    struct corten_shrink_walk *w,
				    int max_windows)
{
	unsigned long idx = state->shrink_cursor;
	int windows = 0;

	while (windows < max_windows && w->budget > 0) {
		struct corten_arena *arena;
		unsigned long key, fstart, fend;

		rcu_read_lock();
		arena = xa_find(&state->arenas, &idx, ULONG_MAX, XA_PRESENT);
		if (!arena) {
			/* Rotation exhausted: restart from frame 0 on the
			 * next slice/scan (no same-scan wrap).
			 */
			rcu_read_unlock();
			state->shrink_cursor = 0;
			return;
		}
		key = idx;
		if (arena == &corten_va_reserve_sentinel ||
		    !percpu_ref_tryget_live(&arena->active)) {
			rcu_read_unlock();
			idx = key + 1;
			continue;
		}
		rcu_read_unlock();

		if (!READ_ONCE(arena->frozen) && !READ_ONCE(arena->idle)) {
			fstart = max(key << PMD_SHIFT, READ_ONCE(arena->start));
			fend = min((key << PMD_SHIFT) + PMD_SIZE,
				   READ_ONCE(arena->end));
			if (fstart < fend) {
				corten_arena_shrink_walk(mm, arena, fstart,
							 fend, w);
				/* The flag is the pass-2 handoff; a
				 * GFP_NOWAIT store failure just means the
				 * window is aged again next rotation.
				 * The evict shape never consumes epochs
				 * and never feeds the aging bookkeeping.
				 */
				if (!w->force) {
					if (!xa_store(&state->shrink_aged, key,
						      xa_mk_value(1),
						      GFP_NOWAIT))
						atomic_long_inc(&corten_nr_aging_passes);
				}
				windows++;
			}
		}
		percpu_ref_put(&arena->active);
		idx = key + 1;
	}

	state->shrink_cursor = idx;
}

/* The per-mm scan: alternating pass-2/pass-1 slices under the per-mm
 * shrink trylock, one reclaim per slice with the lock dropped.  Returns
 * the pages freed; *@scanned_out reports the candidate-page budget used.
 *
 * W1.e2: the picks drain through the native swap-out driver (the evict
 * leg's shape) -- the __reclaim_pages()-through-ttu detour is gone, and
 * with it the M6.T2 ttu completion arm's last feed.  The lock is held
 * across a slice's aging only: the drain sleeps (folio trylock,
 * synchronous zram writeout), so each slice drops the lock before
 * driving its picks, exactly like the evict loop.
 */
static unsigned long corten_shrink_mm(struct mm_struct *mm,
				      struct corten_mm_state *state,
				      int budget, int *scanned_out)
{
	unsigned long freed = 0;
	int picked = 0, kept = 0;
	int scanned = 0, rounds = 0;

	while (budget > 0 && rounds++ < CORTEN_SHRINK_MAX_SLICES) {
		struct corten_shrink_walk w;
		LIST_HEAD(picks);

		w.picks = &picks;
		w.budget = min(budget, CORTEN_SHRINK_SLICE_PAGES);
		w.nr_scanned = w.nr_young = w.nr_picked = 0;
		w.eval = false;
		w.force = false;

		if (!spin_trylock(&state->shrink_lock))
			break;

		corten_shrink_eval_slice(mm, state, &w,
					 CORTEN_SHRINK_SLICE_WINDOWS);
		if (w.budget > 0)
			corten_shrink_age_slice(mm, state, &w,
						CORTEN_SHRINK_SLICE_WINDOWS);

		spin_unlock(&state->shrink_lock);

		scanned += w.nr_scanned;
		budget -= w.nr_scanned;
		picked += w.nr_picked;

		/* W1.e2: the native driver drains the picks itself -- no
		 * __reclaim_pages()/ttu detour.  Each pick's folio
		 * reference is consumed inside (freed on success, dropped
		 * on every keep arm); the keep verdict feeds the
		 * picked-minus-kept swap-out attribution below.
		 */
		while (!list_empty(&picks)) {
			struct corten_swap_pick *p;

			p = list_first_entry(&picks, struct corten_swap_pick,
					     link);
			list_del(&p->link);
			if (corten_swap_out_driver(mm, p->addr, p->folio))
				freed++;
			else
				kept++;
			kfree(p);
		}
		if (!w.nr_scanned)
			break;	/* nothing left to age or evaluate */
		cond_resched();
	}

	/* Exact shrinker swap-out attribution: every freed pick of an
	 * anonymous arena page reached swap through the native driver's
	 * transaction (picked minus kept), race-free by construction.
	 */
	if (picked > kept)
		atomic_long_add(picked - kept, &corten_nr_shrink_swapped);
	*scanned_out = scanned;
	return freed;
}

/* Pin up to @max registered mms in one RCU section: mmget inside the
 * section makes them safe to use after it (mm_users > 0 keeps
 * exit_mmap -- and with it the registry unlink and the state free --
 * from running).  *@skip resumes the walk; entries appearing or
 * vanishing between rounds only skew the rotation, never the
 * correctness (every scanned mm is a pinned, consistent registry
 * member).  An exiting mm (mm_users == 0) is skipped: its teardown
 * drains the arenas itself.
 */
static int corten_registry_pin(struct mm_struct **mms, int max, long *skip)
{
	struct corten_mm_state *state;
	int found = 0;

	rcu_read_lock();
	list_for_each_entry_rcu(state, &corten_mm_registry, shrink_reg) {
		struct mm_struct *mm = READ_ONCE(state->owner_mm);

		if (*skip > 0) {
			(*skip)--;
			continue;
		}
		if (!mm || !mmget_not_zero(mm))
			continue;
		mms[found++] = mm;
		if (found == max)
			break;
	}
	rcu_read_unlock();

	return found;
}

/* Does @mm belong to @target's cgroup subtree?  Coarse scan-time
 * filter (spec D2): the live per-mm memcg, not a stale recorded
 * pointer, so cgroup moves are honored.
 */
static bool corten_mm_in_cgroup(struct mm_struct *mm,
				struct mem_cgroup *target)
{
	struct mem_cgroup *memcg;
	bool match;

	if (mem_cgroup_disabled())
		return true;
	memcg = get_mem_cgroup_from_mm(mm);
	if (!memcg)
		return true;
#ifdef CONFIG_MEMCG
	match = mem_cgroup_is_descendant(memcg, target);
#else
	/* !MEMCG: the cgroup-subtree filter degenerates to match-all, the
	 * same answer the mem_cgroup_disabled() bail above gives.
	 */
	match = true;
#endif
	mem_cgroup_put(memcg);
	return match;
}

static unsigned long corten_shrink_mms(int budget, struct mem_cgroup *target)
{
	unsigned long freed = 0;
	long skip = 0;

	while (budget > 0) {
		struct mm_struct *mms[16];
		int i, found;

		found = corten_registry_pin(mms, ARRAY_SIZE(mms), &skip);
		if (!found)
			break;

		for (i = 0; i < found && budget > 0; i++) {
			struct mm_struct *mm = mms[i];
			struct corten_mm_state *state;
			int scanned = 0;

			/* Pairs with the smp_store_release() publisher
			 * in corten_arena_state_create(); NULL means the
			 * owner's exit teardown already unpublished it.
			 */
			state = smp_load_acquire(&mm->corten_state);
			if (state && (!target || corten_mm_in_cgroup(mm, target))) {
				freed += corten_shrink_mm(mm, state, budget,
							  &scanned);
				budget -= min(scanned, budget);
			}
			mmput(mm);
			cond_resched();
		}
		skip += found;
	}

	return freed;
}

static unsigned long corten_count_mms(struct mem_cgroup *target)
{
	long skip = 0;
	unsigned long total = 0;

	for (;;) {
		struct mm_struct *mms[16];
		int i, found;

		found = corten_registry_pin(mms, ARRAY_SIZE(mms), &skip);
		if (!found)
			break;
		for (i = 0; i < found; i++) {
			struct mm_struct *mm = mms[i];

			if (!target || corten_mm_in_cgroup(mm, target))
				total += corten_mm_state_pages(mm, NULL);
			mmput(mm);
			cond_resched();
		}
		skip += found;
	}

	return total;
}

static unsigned long corten_shrink_count_objects(struct shrinker *shrinker,
						 struct shrink_control *sc)
{
	if (!corten_enabled_static())
		return 0;
	return corten_count_mms(sc->memcg);
}

static unsigned long corten_shrink_scan_objects(struct shrinker *shrinker,
						struct shrink_control *sc)
{
	if (!corten_enabled_static() || !sc->nr_to_scan)
		return 0;

	/* The scan's whole point is swap-out I/O (a synchronous zram
	 * write per page), so a caller without __GFP_IO has nothing
	 * useful for us to do -- bail without counting a scan, the
	 * way the superblock shrinker bails on missing __GFP_FS.
	 */
	if (!(sc->gfp_mask & __GFP_IO))
		return 0;

	atomic_long_inc(&corten_nr_shrink_scans);

	return corten_shrink_mms((int)min(sc->nr_to_scan, INT_MAX), sc->memcg);
}

/* Registered only on a corten=on boot (spec red line 6): with the
 * static key off the shrinker must not exist at all -- an empty count
 * would still put scan pressure bookkeeping on every reclaim cycle.
 */
static int __init corten_shrinker_init(void)
{
	if (!corten_enabled_static())
		return 0;

	corten_shrinker_handle = shrinker_alloc(SHRINKER_MEMCG_AWARE,
						"corten-arena");
	if (!corten_shrinker_handle)
		return -ENOMEM;

	corten_shrinker_handle->count_objects = corten_shrink_count_objects;
	corten_shrinker_handle->scan_objects = corten_shrink_scan_objects;
	corten_shrinker_handle->seeks = DEFAULT_SEEKS;

	shrinker_register(corten_shrinker_handle);
	return 0;
}
late_initcall(corten_shrinker_init);

/*
 * M6.T2 eviction driver, M6.T3 batch shape: the debugfs "evict N pages"
 * entry rides the same victim machinery as the shrinker -- the shared
 * walker, the per-mm shrink trylock (mutual exclusion with a concurrent
 * scan; a busy mm means the shrinker is already doing this batch's work)
 * and the rotation cursor (no from-frame-0 rescan per call).  Candidates
 * are metadata CORTEN_MAPPED (content, not the zero page), COW-unshared
 * (OQ-M6-2), not DMA-pinned, not in writeback, present PTE.  W1.e1: the
 * picks are (addr, folio) pairs drained by the native swap-out driver
 * (the ttu walk no longer rejoins this path; W1.e2 put the shrinker leg
 * on the same shape).  zram is a synchronous-write device, so the batch
 * settles inline.
 *
 * Lock order: shrink trylock > [lookup_get active ref implicitly via
 * the arenas walk] > corten_lock_range > ptl for the pick.  All of
 * those are released before the driver runs -- and so is the shrink
 * lock itself: the driver sleeps (folio trylock, zram's synchronous
 * submit_bio_wait), so each slice drops the lock before driving the
 * picks and spin_trylock()s it again for the next slice, exactly
 * like corten_shrink_mm(); a busy reacquire ends this batch early
 * (counted in evict_busy -- the concurrent scan is doing this work
 * anyway).  A pick that lost its mapping by drive time is kept by
 * the driver, not corrupted -- the transaction re-checks everything
 * under the covering lock.
 */
static int corten_arena_evict_mm(struct mm_struct *mm, int nr)
{
	struct corten_mm_state *state;
	unsigned long swapped_before;
	int scanned = 0, reclaimed;

	/* Pairs with the smp_store_release() publisher in
	 * corten_arena_state_create() (the other two readers use the
	 * same acquire in the fault and exit paths).
	 */
	state = smp_load_acquire(&mm->corten_state);
	if (!state || nr <= 0)
		return -EINVAL;

	swapped_before = atomic_long_read(&corten_nr_swapped_out);

	if (!spin_trylock(&state->shrink_lock)) {
		atomic_long_inc(&corten_nr_evict_busy);
		return -EBUSY;
	}

	/* The lock is held across a slice's aging only.  Every exit
	 * from this loop happens with it already dropped: the break
	 * paths run after the unlock, and the reacquire below is the
	 * last statement before the loop re-tests.
	 */
	for ( ; ; ) {
		struct corten_shrink_walk w;
		LIST_HEAD(picks);

		w.picks = &picks;
		w.budget = min(nr - scanned, CORTEN_SHRINK_SLICE_PAGES);
		w.nr_scanned = w.nr_young = w.nr_picked = 0;
		w.eval = false;
		w.force = true;

		corten_shrink_age_slice(mm, state, &w,
					CORTEN_SHRINK_SLICE_WINDOWS);
		/* Drop the lock before the reclaim: it sleeps (folio
		 * trylock, synchronous zram writeout), so holding the
		 * spinlock across it is an atomic-sleep bug, not an
		 * optimisation.
		 */
		spin_unlock(&state->shrink_lock);

		scanned += w.nr_scanned;
		/* W1.e1: the native driver drains the picks itself -- no
		 * __reclaim_pages()/ttu detour.  Each pick's folio
		 * reference is consumed inside (freed on success, dropped
		 * on every keep arm); zram's synchronous writeout settles
		 * each page before the next slice re-acquires the lock.
		 */
		while (!list_empty(&picks)) {
			struct corten_swap_pick *p;

			p = list_first_entry(&picks, struct corten_swap_pick,
					     link);
			list_del(&p->link);
			corten_swap_out_driver(mm, p->addr, p->folio);
			kfree(p);
		}
		if (!w.nr_scanned || scanned >= nr)
			break;
		cond_resched();

		/* Re-acquire for the next slice, same shape as
		 * corten_shrink_mm(): a busy reacquire means a
		 * concurrent scan owns this mm and is doing this
		 * batch's work -- end ours here; the swap delta below
		 * still reports the slices reclaimed so far.
		 */
		if (!spin_trylock(&state->shrink_lock)) {
			atomic_long_inc(&corten_nr_evict_busy);
			break;
		}
	}

	/* Report what actually reached swap (the reclaim count also
	 * covers non-swap frees); the arena_stats swapped_out delta is
	 * the exact transaction number.
	 */
	if (atomic_long_read(&corten_nr_swapped_out) == swapped_before)
		return 0;
	reclaimed = (int)(atomic_long_read(&corten_nr_swapped_out) -
			  swapped_before);
	return reclaimed;
}

int corten_arena_evict_pid(pid_t pid, int nr)
{
	struct task_struct *task;
	struct mm_struct *mm;
	int ret;

	if (!corten_enabled_static())
		return -EOPNOTSUPP;

	rcu_read_lock();
	task = find_get_task_by_vpid(pid);
	rcu_read_unlock();
	if (!task)
		return -ESRCH;

	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -EINVAL;

	ret = corten_arena_evict_mm(mm, nr);
	mmput(mm);

	return ret;
}

#ifdef CONFIG_CORTEN_MM_ARENA_KUNIT_TEST
/*
 * M6.T3/T4 hooks (the shrinker bodies are defined above; the shrinker
 * itself is only registered on a corten=on boot, so the suite drives
 * the same functions with a synthetic target instead).
 */
unsigned long corten_arena_test_shrink_count(void)
{
	return corten_count_mms(NULL);
}

unsigned long corten_arena_test_shrink_scan(int nr)
{
	atomic_long_inc(&corten_nr_shrink_scans);

	return corten_shrink_mms(nr, NULL);
}

long corten_arena_test_registry_nr(void)
{
	struct corten_mm_state *state;
	long nr = 0;

	rcu_read_lock();
	list_for_each_entry_rcu(state, &corten_mm_registry, shrink_reg)
		nr++;
	rcu_read_unlock();

	return nr;
}

long corten_arena_test_shrink_scans(void)
{
	return atomic_long_read(&corten_nr_shrink_scans);
}

long corten_arena_test_aging_passes(void)
{
	return atomic_long_read(&corten_nr_aging_passes);
}

long corten_arena_test_shrink_swapped(void)
{
	return atomic_long_read(&corten_nr_shrink_swapped);
}

long corten_arena_test_shrink_skipped(void)
{
	return atomic_long_read(&corten_nr_shrink_skipped);
}

bool corten_arena_test_window_aged(struct mm_struct *mm, unsigned long addr)
{
	struct corten_mm_state *state = READ_ONCE(mm->corten_state);

	if (!state)
		return false;

	return xa_load(&state->shrink_aged, addr >> PMD_SHIFT) != NULL;
}

long corten_arena_test_resident_pages(void)
{
	long skip = 0, total = 0;

	for (;;) {
		struct mm_struct *mms[16];
		int i, found;

		found = corten_registry_pin(mms, ARRAY_SIZE(mms), &skip);
		if (!found)
			break;
		for (i = 0; i < found; i++) {
			total += corten_mm_state_pages(mms[i], NULL);
			mmput(mms[i]);
		}
		skip += found;
	}

	return total;
}

long corten_arena_test_swapped_pages(void)
{
	long skip = 0, total = 0;

	for (;;) {
		struct mm_struct *mms[16];
		int i, found;

		found = corten_registry_pin(mms, ARRAY_SIZE(mms), &skip);
		if (!found)
			break;
		for (i = 0; i < found; i++) {
			long sw = 0;

			corten_mm_state_pages(mms[i], &sw);
			total += sw;
			mmput(mms[i]);
		}
		skip += found;
	}

	return total;
}
#endif
