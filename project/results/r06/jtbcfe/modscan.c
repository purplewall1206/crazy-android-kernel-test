// modscan <pid> <start> <end>: per-page present/PFN/content scan of a range.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>

int main(int argc, char **argv) {
	if (argc < 4) return 2;
	int pid = atoi(argv[1]);
	uint64_t s = strtoull(argv[2], NULL, 0), e = strtoull(argv[3], NULL, 0);
	char pm[64], mm[64];
	snprintf(pm, 64, "/proc/%d/pagemap", pid);
	snprintf(mm, 64, "/proc/%d/mem", pid);
	int pfd = open(pm, O_RDONLY), mfd = open(mm, O_RDONLY);
	if (pfd < 0 || mfd < 0) { perror("open"); return 1; }
	unsigned char buf[4096];
	uint64_t zeropfn = ~0ULL;
	int nzero = 0;
	for (uint64_t a = s; a < e; a += 4096) {
		uint64_t ent = 0;
		if (pread(pfd, &ent, 8, (a / 4096) * 8) != 8) continue;
		if (!((ent >> 63) & 1)) continue;
		uint64_t pfn = ent & ((1ULL << 55) - 1);
		int allz = -1;
		if (pread(mfd, buf, 4096, a) == 4096) {
			allz = 1;
			for (int i = 0; i < 4096; i += 8)
				if (*(uint64_t *)(buf + i)) { allz = 0; break; }
		}
		if (allz == 1) {
			nzero++;
			if (zeropfn == ~0ULL) zeropfn = pfn;
			printf("ZERO %#" PRIx64 " pfn=%#" PRIx64 "%s\n",
			       a - s, pfn, pfn == zeropfn ? " (same-as-first)" : "");
		}
	}
	printf("TICK done zeropages=%d firstzeropfn=%#" PRIx64 "\n", nzero, zeropfn);
	return 0;
}
