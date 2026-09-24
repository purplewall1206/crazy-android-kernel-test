/* probe2.c — minimal churn-content forensics for the CortenMM r03 fix2 night.
 * In a declared arena: mmap MAP_FIXED 2M, write per-page magic, read back,
 * on mismatch dump pagemap + first qwords; then munmap.  Static, no deps. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef PR_CORTEN_ARENA
#define PR_CORTEN_ARENA 79
#endif
#define CORTEN_ARENA_DECLARE 0
#define CORTEN_ARENA_RELEASE 1

#define ARENA_LEN (4UL << 20)	/* 2 windows of 2M */
#define CHUNK_LEN (2UL << 20)
#define PAGES (CHUNK_LEN / 4096UL)

static long arena_prctl(unsigned long op, unsigned long addr, unsigned long len)
{
	return syscall(SYS_prctl, PR_CORTEN_ARENA, op, addr, len, 0UL);
}

static uint64_t magic(int tid, uint64_t pg) { return 0x5a5a000000000000ULL ^ ((uint64_t)tid << 32) ^ pg; }

static int pmap_fd = -1;
static uint64_t pagemap(unsigned long va)
{
	uint64_t e = 0;
	if (pmap_fd < 0)
		pmap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pmap_fd < 0) return ~0ULL;
	if (pread(pmap_fd, &e, 8, (va / 4096) * 8) != 8) return ~0ULL;
	return e;
}

int main(int argc, char **argv)
{
	unsigned long base = 0x10000000UL;
	char *arena;
	int cycles = argc > 1 ? atoi(argv[1]) : 200;
	int fails = 0;

	arena = mmap((void *)base, ARENA_LEN, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
		     MAP_FIXED_NOREPLACE, -1, 0);
	if (arena == MAP_FAILED) { perror("mmap"); return 3; }
	if (arena_prctl(CORTEN_ARENA_DECLARE, (unsigned long)arena, ARENA_LEN)) {
		perror("DECLARE"); return 3;
	}
	fprintf(stderr, "arena %p-%p live\n", arena, arena + ARENA_LEN);

	for (int c = 0; c < cycles; c++) {
		unsigned long off = (c & 1) ? 0 : (2UL << 20); /* alternate window */
		char *chunk = arena + off;
		char *p;

		p = mmap(chunk, CHUNK_LEN, PROT_READ | PROT_WRITE,
			 MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS |
			 MAP_NORESERVE, -1, 0);
		if (p == MAP_FAILED) { perror("chunk mmap"); return 3; }
		for (unsigned long i = 0; i < PAGES; i++)
			*(volatile uint64_t *)(p + i * 4096) = magic(1, off / 4096 + i);
		for (unsigned long i = 0; i < PAGES; i++) {
			uint64_t got = *(volatile uint64_t *)(p + i * 4096);
			uint64_t exp = magic(1, off / 4096 + i);

			if (got != exp && fails < 5) {
				uint64_t pm = pagemap((unsigned long)p + i * 4096);
				fprintf(stderr,
					"FAIL c=%d off=%lx pg=%lx got=%016lx exp=%016lx pagemap=%016lx present=%lld pfn=%llx\n",
					c, off, i, got, exp, pm,
					(long long)!!(pm & 0x4000000000000000ULL),
					(unsigned long long)(pm & 0x7fffffffffffffULL) * 4096ULL);
				fails++;
			} else if (got != exp) {
				fails++;
			}
		}
		if (munmap(chunk, CHUNK_LEN)) { perror("munmap"); return 3; }
	}
	fprintf(stderr, "done cycles=%d fail_pages=%d\n", cycles, fails);
	if (arena_prctl(CORTEN_ARENA_RELEASE, (unsigned long)arena, ARENA_LEN))
		fprintf(stderr, "RELEASE failed\n");
	fprintf(stderr, "RELEASE ok\n");
	return fails ? 3 : 0;
}
/* zerocheck variant: after munmap, remap and read — expect all zero. */
