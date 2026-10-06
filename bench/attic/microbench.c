/*
 * microbench.c — CortenMM (SOSP'25) Table 3 microbenchmarks, Linux port.
 *
 * Five microbenchmarks over 16KB slots (4 x 4KB pages), per paper Table 3:
 *   mmap       : timed = mmap(16KB, MAP_PRIVATE|MAP_ANONYMOUS) at an unmapped slot;
 *                untimed = munmap sweep after each batch (cleanup).
 *   mmap-PF    : timed = mmap + write-touch all 4 pages + munmap (whole op body,
 *                per paper definition "mmap()s a region and then accesses it").
 *   unmap-virt : timed = munmap of a slot never touched; untimed = mmap in prep.
 *   unmap      : timed = munmap of a populated slot; untimed = mmap + touch in prep.
 *   PF         : timed = write-touch 4 pages of an untouched mapping
 *                (mmap in untimed prep, munmap in untimed cleanup) => pure fault cost.
 *
 * Timing discipline: clock_gettime(CLOCK_MONOTONIC) brackets each batch of
 * measured ops; prep/cleanup phases live strictly outside the bracket. See
 * README.md for the full list of operationalization assumptions (paper does
 * not publish its harness details).
 *
 * C99; depends only on pthread + POSIX/Linux syscalls.
 * Build: gcc -O2 -Wall -Wextra -pthread -o microbench microbench.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE (1 << 20)   /* Linux 4.17+; value from linux/uapi */
#endif
#ifndef PR_SET_THP_DISABLE
#define PR_SET_THP_DISABLE 41
#endif

#define PAGE_BYTES       4096UL
#define SLOT_BYTES       (16UL * 1024)          /* paper: 16KB region = 4 x 4KB pages */
#define SLOT_PAGES       4
#define LOW_ARENA_BYTES  (256UL << 20)          /* per-thread private grid: 16384 slots (cap, cyclic reuse) */
#define HIGH_ARENA_BYTES (512UL << 20)          /* shared grid: 32768 slots */
#define NLOW_SLOTS       ((uint32_t)(LOW_ARENA_BYTES / SLOT_BYTES))
#define NHIGH_SLOTS      ((uint32_t)(HIGH_ARENA_BYTES / SLOT_BYTES))
#define MAX_THREADS      64
#define MAX_THREAD_LIST  16
/* Batch = ops per timed window (one clock pair per batch => negligible clock
 * overhead). mmap bench holds one mapping per op until batch cleanup, so its
 * in-flight set would collide with the shared grid at batch=1024 (up to 8192
 * live slots vs 32768 => ~20% same-slot retries); batch=256 keeps the retry
 * rate low. Claim benches (unmap family, PF) do their claiming in untimed
 * prep, so the timed phase never retries and can afford the bigger batch. */
#define BATCH_MMAP       256
#define BATCH_CLAIM      1024

typedef enum { B_MMAP, B_MMAP_PF, B_UNMAP_VIRT, B_UNMAP, B_PF } bench_t;
typedef enum { C_LOW, C_HIGH } contention_t;

static const char *bench_name[] = { "mmap", "mmap-PF", "unmap-virt", "unmap", "PF" };
static const char *cont_name[]  = { "low", "high" };

typedef struct {
    int tid;
    bench_t bench;
    contention_t cont;
    double duration;        /* target accumulated *timed* seconds */
    char *base;             /* slot grid base */
    uint32_t nslots;        /* power of two */
    uint32_t *slots;        /* per-batch claimed slot indices */
    uint64_t rng;           /* per-thread xorshift64* state (no rand(): libc lock would pollute) */
    uint64_t ops;
    double time_s;          /* accumulated timed-window seconds only */
} worker_t;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "microbench: ");
    vfprintf(stderr, fmt, ap);
    if (errno)
        fprintf(stderr, ": %s", strerror(errno));
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* ---- xorshift64* (Marsaglia/Vigna), per-thread, lock-free ---- */
static uint64_t rnd64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 2685821657736338717ULL;
}

/* ---- VA grid allocator ----
 * Grids are plain address ranges, not reservations: slots are individually
 * mmap'd/munmap'd inside them. We carve them from one big PROT_NONE anchor
 * mapping that is released immediately (single-threaded init), so each grid
 * has a unique, race-free address range. Kernel-placed stray mappings cannot
 * collide with our slots because slot placement uses MAP_FIXED_NOREPLACE
 * (EEXIST => redraw); if the kernel lacks that flag we fall back to MAP_FIXED
 * (safe here: slots are free by construction except for rare cross-thread
 * in-flight overlaps, and this is only used on kernels we control). */
static char *g_va_next;
static int g_have_noreplace;

static void va_init(void)
{
    size_t total = (size_t)MAX_THREADS * LOW_ARENA_BYTES + HIGH_ARENA_BYTES + (256UL << 20);
    char *p = mmap(NULL, total, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        die("cannot reserve %zu bytes of VA", total);

    /* probe MAP_FIXED_NOREPLACE one page past our (freed later) reservation */
    void *q = mmap(p + total, PAGE_BYTES, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (q != MAP_FAILED) {
        g_have_noreplace = 1;
        munmap(q, PAGE_BYTES);
    } else if (errno == EEXIST) {
        g_have_noreplace = 1;           /* flag understood, address just busy */
    } else if (errno == EINVAL || errno == EOPNOTSUPP) {
        g_have_noreplace = 0;
    } else {
        die("MAP_FIXED_NOREPLACE probe failed");
    }
    munmap(p, total);
    g_va_next = p;
}

static char *va_take(size_t len)
{
    /* round to 2MB so every grid base is >= slot aligned */
    len = (len + ((1UL << 21) - 1)) & ~((1UL << 21) - 1);
    char *p = g_va_next;
    g_va_next += len;
    return p;
}

/* claim a slot: install a 16KB anon mapping exactly at slot address */
static int slot_claim(char *base, uint32_t idx)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS |
                (g_have_noreplace ? MAP_FIXED_NOREPLACE : MAP_FIXED);
    void *p = mmap(base + (size_t)idx * SLOT_BYTES, SLOT_BYTES,
                   PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p != MAP_FAILED)
        return 0;
    if (errno == EEXIST)
        return -1;                      /* slot in flight by another thread */
    die("slot mmap failed");
    return -1;
}

static void slot_release(char *base, uint32_t idx)
{
    if (munmap(base + (size_t)idx * SLOT_BYTES, SLOT_BYTES))
        die("slot munmap failed");
}

/* write-fault all 4 pages: one volatile store per 4KB page is enough to
 * install the page (fault + zero-page COW), matching "accesses it" in Table 3 */
static void slot_touch(char *base, uint32_t idx)
{
    volatile int *p = (volatile int *)(base + (size_t)idx * SLOT_BYTES);
    for (unsigned i = 0; i < SLOT_PAGES; i++)
        p[i * (PAGE_BYTES / sizeof(int))] = 1;
}

static void run_batch(worker_t *w)
{
    uint32_t batch = (w->bench == B_MMAP) ? BATCH_MMAP : BATCH_CLAIM;
    uint32_t mask = w->nslots - 1;      /* nslots is a power of two */
    /* Slot sequence per batch: low contention walks the private grid
     * sequentially (best case, matches "each thread works on a private memory
     * region", Fig 14); high contention picks a random start plus an odd
     * stride => a permutation of the shared grid, unique within the thread,
     * per-op randomness across threads. Threads never share an RNG state, so
     * two threads picking the same index is a true collision, not aliasing. */
    uint32_t start = (uint32_t)(rnd64(&w->rng) >> 16) & mask;
    uint32_t stride = (w->cont == C_HIGH) ? (uint32_t)(rnd64(&w->rng) | 1) : 1;
    uint32_t pos = start;
    struct timespec t0, t1;

    /* ---- untimed prep: claim (and for `unmap`, populate) the slots ---- */
    if (w->bench == B_UNMAP_VIRT || w->bench == B_UNMAP || w->bench == B_PF) {
        for (uint32_t got = 0; got < batch; pos = (pos + stride) & mask) {
            if (slot_claim(w->base, pos) == 0) {
                /* `unmap` needs physical pages so munmap has real work;
                 * `unmap-virt`/`PF` deliberately leave the slot untouched */
                if (w->bench == B_UNMAP)
                    slot_touch(w->base, pos);
                w->slots[got++] = pos;
            }
            /* on EEXIST just advance: another thread holds this slot */
        }
    }

    /* ---- timed window: only the measured ops ---- */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    switch (w->bench) {
    case B_MMAP:
        /* mmap at an unmapped slot; mapping held until untimed cleanup.
         * Rare EEXIST retries (concurrent same-slot pick) stay inside the
         * timed window by design: they are part of the contention protocol. */
        for (uint32_t got = 0; got < batch; pos = (pos + stride) & mask) {
            if (slot_claim(w->base, pos) == 0)
                w->slots[got++] = pos;
        }
        break;
    case B_MMAP_PF:
        /* whole op body timed: mmap + touch 4 pages + munmap */
        for (uint32_t got = 0; got < batch; pos = (pos + stride) & mask) {
            if (slot_claim(w->base, pos) == 0) {
                slot_touch(w->base, pos);
                slot_release(w->base, pos);
                got++;
            }
        }
        break;
    case B_UNMAP_VIRT:
    case B_UNMAP:
        for (uint32_t i = 0; i < batch; i++)
            slot_release(w->base, w->slots[i]);
        break;
    case B_PF:
        for (uint32_t i = 0; i < batch; i++)
            slot_touch(w->base, w->slots[i]);
        break;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    w->time_s += (double)(t1.tv_sec - t0.tv_sec) +
                 (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    w->ops += batch;

    /* ---- untimed cleanup ---- */
    if (w->bench == B_MMAP || w->bench == B_PF) {
        for (uint32_t i = 0; i < batch; i++)
            slot_release(w->base, w->slots[i]);
    }
}

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    while (w->time_s < w->duration)
        run_batch(w);
    return NULL;
}

static void run_config(bench_t b, contention_t c, int nthreads, double duration)
{
    worker_t *ws = calloc((size_t)nthreads, sizeof(*ws));
    pthread_t *th = calloc((size_t)nthreads, sizeof(*th));
    char *shbase = NULL;
    uint64_t total_ops = 0;
    double total_s = 0.0;
    struct timespec ts;

    if (!ws || !th)
        die("out of memory");
    if (c == C_HIGH)
        shbase = va_take(HIGH_ARENA_BYTES);

    clock_gettime(CLOCK_MONOTONIC, &ts);
    for (int t = 0; t < nthreads; t++) {
        ws[t].tid = t;
        ws[t].bench = b;
        ws[t].cont = c;
        ws[t].duration = duration;
        ws[t].base = (c == C_HIGH) ? shbase : va_take(LOW_ARENA_BYTES);
        ws[t].nslots = (c == C_HIGH) ? NHIGH_SLOTS : NLOW_SLOTS;
        ws[t].slots = malloc(BATCH_CLAIM * sizeof(uint32_t));
        if (!ws[t].slots)
            die("out of memory");
        /* independent seed per thread: xorshift state must be nonzero */
        uint64_t seed = 0x9E3779B97F4A7C15ULL ^
                        (uint64_t)(uintptr_t)&ts ^
                        ((uint64_t)ts.tv_nsec << 8) ^
                        ((uint64_t)t * 0xDA3E39CB3B0437B3ULL);
        ws[t].rng = seed ? seed : 1;
    }

    for (int t = 0; t < nthreads; t++) {
        if (pthread_create(&th[t], NULL, worker_main, &ws[t]))
            die("pthread_create");
    }
    for (int t = 0; t < nthreads; t++)
        pthread_join(th[t], NULL);

    for (int t = 0; t < nthreads; t++) {
        total_ops += ws[t].ops;         /* per-thread counters aggregated at end */
        total_s += ws[t].time_s;
        free(ws[t].slots);
    }

    /* release grid VA (unmaps any mapping we failed to clean; holes are fine) */
    if (shbase)
        munmap(shbase, HIGH_ARENA_BYTES);
    for (int t = 0; t < nthreads; t++)
        if (c == C_LOW)
            munmap(ws[t].base, LOW_ARENA_BYTES);

    /* duration_s = sum of *timed* windows across threads (prep/cleanup wall
     * time excluded), so ops_per_sec is a pure measured-op throughput */
    printf("{\"bench\":\"%s\",\"contention\":\"%s\",\"threads\":%d,"
           "\"duration_s\":%.4f,\"ops\":%llu,\"ops_per_sec\":%.1f}\n",
           bench_name[b], cont_name[c], nthreads, total_s,
           (unsigned long long)total_ops,
           total_s > 0.0 ? (double)total_ops / total_s : 0.0);
    fflush(stdout);
    free(ws);
    free(th);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: microbench [--bench mmap|mmap-PF|unmap-virt|unmap|PF|all]\n"
        "                  [--contention low|high|both]\n"
        "                  [--threads \"1,2,4,8\"] [--duration seconds]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    bench_t benches[8];
    contention_t conts[2];
    int nb = 0, nc = 0, ntl = 0;
    int thread_list[MAX_THREAD_LIST];
    double duration = 5.0;
    const char *bs = "all", *cs = "both", *ts = "1";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bench") && i + 1 < argc) {
            bs = argv[++i];
        } else if (!strcmp(argv[i], "--contention") && i + 1 < argc) {
            cs = argv[++i];
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            ts = argv[++i];
        } else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
            duration = atof(argv[++i]);
        } else {
            usage();
        }
    }
    if (duration <= 0.0)
        usage();

    if (!strcmp(bs, "all")) {
        for (int i = 0; i < 5; i++)
            benches[nb++] = (bench_t)i;
    } else {
        for (int i = 0; i < 5; i++)
            if (!strcmp(bs, bench_name[i]))
                benches[nb++] = (bench_t)i;
        if (nb != 1)
            usage();
    }
    if (!strcmp(cs, "both")) {
        conts[nc++] = C_LOW;
        conts[nc++] = C_HIGH;
    } else if (!strcmp(cs, "low")) {
        conts[nc++] = C_LOW;
    } else if (!strcmp(cs, "high")) {
        conts[nc++] = C_HIGH;
    } else {
        usage();
    }

    const char *p = ts;
    while (*p && ntl < MAX_THREAD_LIST) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || v < 1 || v > MAX_THREADS)
            usage();
        thread_list[ntl++] = (int)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (ntl == 0 || *p != '\0')
        usage();

    /* keep THP out of the measurement: 16KB slots must behave as 4KB pages,
     * otherwise khugepaged could collapse fully-touched batch regions into
     * 2MB pages and distort munmap/PF costs. Best effort; ignore failures. */
    prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);

    va_init();

    for (int ci = 0; ci < nc; ci++)
        for (int bi = 0; bi < nb; bi++)
            for (int ti = 0; ti < ntl; ti++)
                run_config(benches[bi], conts[ci], thread_list[ti], duration);
    return 0;
}
