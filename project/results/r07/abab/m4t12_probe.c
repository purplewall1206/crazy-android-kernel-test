// m4t12_probe.c - M4.T1/T2 diagnosis probe (r06 m4t12 shift)
// Modes:
//   uv  : unmap-virt shape  - 160MB arena, munmap 16KB chunk + refill (never touched)
//   u   : unmap shape       - same, refill re-touched
//   mpl : mmap+munmap pair shape (16KB, addr=0)
// Args: m4t12_probe <mode> <iters>
// Prints wall time and the guest TLB IPI counter delta (/proc/interrupts TLB row).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#ifndef PR_CORTEN_MODE
#define PR_CORTEN_MODE 80
#define CORTEN_MODE_ENTER 1
#endif

#define MB (1024UL * 1024)
#define OPP (16UL * 1024)

static unsigned long long now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static long tlb_ipis(void)
{
	FILE *f = fopen("/proc/interrupts", "r");
	char line[4096];
	long total = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "TL:", 2)) {
			total = 0;
			char *p = line + 3;
			char *tok;
			for (tok = strtok(p, " \t"); tok; tok = strtok(NULL, " \t")) {
				char *end;
				long v = strtol(tok, &end, 10);
				if (end && *end == '\0')
					total += v;
			}
		}
	fclose(f);
	return total;
}

int base_main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "uv";
	long iters = argc > 2 ? atol(argv[2]) : 20000;
	size_t arena_sz = 160 * MB;
	char *base;
	long i, ipi0, ipi1;
	unsigned long long t0, t1;
	int enter = strcmp(mode, "BASE") != 0;

	if (enter) {
		if (prctl(PR_CORTEN_MODE, CORTEN_MODE_ENTER, 0, 0, 0)) {
			perror("prctl ENTER");
			return 126;
		}
	}

	ipi0 = tlb_ipis();
	t0 = now_ns();

	if (!strcmp(mode, "mpl")) {
		for (i = 0; i < iters; i++) {
			void *p = mmap(NULL, OPP, PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
				       -1, 0);
			if (p == MAP_FAILED) { perror("mmap"); return 3; }
			if (munmap(p, OPP)) { perror("munmap"); return 3; }
		}
	} else {
		base = mmap(NULL, arena_sz, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (base == MAP_FAILED) { perror("arena mmap"); return 3; }
		if (!strcmp(mode, "u"))
			memset(base, 1, arena_sz);
		for (i = 0; i < iters; i++) {
			size_t off = (size_t)(i % (arena_sz / OPP)) * OPP;
			char *chunk = base + off;

			if (munmap(chunk, OPP)) { perror("munmap"); return 3; }
			void *r = mmap(chunk, OPP, PROT_READ | PROT_WRITE,
				       MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS |
				       MAP_NORESERVE, -1, 0);
			if (r == MAP_FAILED) { perror("refill"); return 3; }
			if (!strcmp(mode, "u"))
				memset(chunk, 1, OPP);
		}
	}

	t1 = now_ns();
	ipi1 = tlb_ipis();

	printf("%s enter=%d iters=%ld wall_ms=%.1f per_op_us=%.3f tlb_ipi_delta=%ld (%.2f/op)\n",
	       mode, enter, iters, (t1 - t0) / 1e6,
	       (double)(t1 - t0) / 1000.0 / iters,
	       ipi1 - ipi0, (double)(ipi1 - ipi0) / iters);
	return 0;
}
// --- threaded variant appended: m4t12_probe_mt <mode> <iters> <threads>
#include <pthread.h>
static void *mt_worker(void *arg);
struct mt_arg { int mode; long iters; unsigned long long ns; };
static int mt_enter;
static void *mt_worker(void *arg)
{
	struct mt_arg *a = arg;
	size_t arena_sz = 160 * MB;
	long i;
	char *base = mmap(NULL, arena_sz, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (base == MAP_FAILED) return (void *)3L;
	if (a->mode == 'u') memset(base, 1, arena_sz);
	unsigned long long t0 = now_ns();
	for (i = 0; i < a->iters; i++) {
		size_t off = (size_t)(i % (arena_sz / OPP)) * OPP;
		char *chunk = base + off;
		if (munmap(chunk, OPP)) return (void *)3L;
		void *r = mmap(chunk, OPP, PROT_READ | PROT_WRITE,
			       MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS |
			       MAP_NORESERVE, -1, 0);
		if (r == MAP_FAILED) return (void *)3L;
		if (a->mode == 'u') memset(chunk, 1, OPP);
	}
	a->ns = now_ns() - t0;
	return NULL;
}
int mt_main(int argc, char **argv)
{
	int mode = argc > 1 && argv[1][0] == 'u' && argv[1][1] == 't' ? 'u' :
		   argc > 1 && argv[1][0] == 'v' ? 'v' : 'v';
	int nt = argc > 3 ? atoi(argv[3]) : 4;
	long iters = argc > 2 ? atol(argv[2]) : 10000;
	// argv[1]: "vt" = unmap-virt threaded, "ut" = unmap threaded
	if (argc > 1 && argv[1][0] == 'u' && argv[1][1] == 't') mode = 'u';
	if (argc > 1 && argv[1][0] == 'v') mode = 'v';
	if (argc > 1 && !strcmp(argv[1], "BASE")) { mt_enter = 0; mode = 'v'; }
	if (argc > 1 && !strcmp(argv[1], "BASE-u")) { mt_enter = 0; mode = 'u'; }
	if (mt_enter && prctl(PR_CORTEN_MODE, CORTEN_MODE_ENTER, 0, 0, 0)) {
		perror("prctl"); return 126;
	}
	pthread_t th[64];
	struct mt_arg args[64];
	long ipi0 = tlb_ipis();
	unsigned long long t0 = now_ns();
	for (int i = 0; i < nt; i++) {
		args[i].mode = mode; args[i].iters = iters; args[i].ns = 0;
		pthread_create(&th[i], NULL, mt_worker, &args[i]);
	}
	for (int i = 0; i < nt; i++) { void *r; pthread_join(th[i], &r); }
	unsigned long long t1 = now_ns();
	long ipi1 = tlb_ipis();
	printf("%s enter=%d threads=%d iters=%ld wall_ms=%.1f per_op_us=%.3f tlb_ipi_delta=%ld (%.2f/op)\n",
	       argc > 1 ? argv[1] : "?", mt_enter, nt, iters, (t1 - t0) / 1e6,
	       (double)(t1 - t0) / 1000.0 / (iters * nt),
	       ipi1 - ipi0, (double)(ipi1 - ipi0) / (iters * nt));
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 4 || (argc > 1 && (strstr(argv[1], "t") == argv[1] + 1)))
		return mt_main(argc, argv);
	return base_main(argc, argv);
}
