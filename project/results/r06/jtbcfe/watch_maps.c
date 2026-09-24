// watch_maps.c - non-mutating poller: for a pid's /modules and classes.jsa
// file mappings, record per-tick page PFNs from pagemap only (no mem reads,
// so no fault-in contamination).  A FILE mapping must map distinct physical
// pages per offset; a repeated PFN across offsets = zero page / anon leak.
// usage: ./watch_maps <pid> <outfile>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <inttypes.h>

struct seg { uint64_t s, e; char path[256]; char perms[8]; };

static int load_segs(pid_t pid, struct seg *segs, int max) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	FILE *f = fopen(path, "r");
	int n = 0;
	char line[1024];
	if (!f) return -1;
	while (fgets(line, sizeof(line), f) && n < max) {
		uint64_t s, e;
		char perms[8], rest[512];
		if (sscanf(line, "%" SCNx64 "-%" SCNx64 " %7s %*s %*s %*s %511[^\n]",
			   &s, &e, perms, rest) < 3) continue;
		if (!strstr(rest, "/modules") && !strstr(rest, "classes.jsa"))
			continue;
		segs[n].s = s; segs[n].e = e;
		snprintf(segs[n].path, 256, "%s", rest);
		snprintf(segs[n].perms, 8, "%s", perms);
		n++;
	}
	fclose(f);
	return n;
}

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: %s pid out\n", argv[0]); return 2; }
	pid_t pid = atoi(argv[1]);
	FILE *out = fopen(argv[2], "w");
	char pm[64];
	snprintf(pm, sizeof(pm), "/proc/%d/pagemap", pid);
	struct seg segs[64];
	fprintf(out, "# t seg start npages present dup_pfn dup_count first_dup_off\n");
	double t0 = now_s();
	while (1) {
		int n = load_segs(pid, segs, 64);
		if (n <= 0) break;
		int pmf = open(pm, O_RDONLY);
		if (pmf < 0) break;
		for (int i = 0; i < n; i++) {
			uint64_t a, npresent = 0, first_dup = 0, dup_pfn = 0;
			int dup_count = 0, ntot = 0;
			/* small PFN seen-set: sample at most 8192 pages per tick */
			static uint64_t seen[8192];
			int nseen = 0;
			for (a = segs[i].s; a < segs[i].e && ntot < 8192; a += 4096, ntot++) {
				uint64_t ent = 0, pfn;
				if (pread(pmf, &ent, 8, (a / 4096) * 8) != 8) continue;
				if (!((ent >> 63) & 1)) continue;
				pfn = ent & ((1ULL << 55) - 1);
				npresent++;
				for (int k = 0; k < nseen; k++) {
					if (seen[k] == pfn) {
						dup_count++;
						dup_pfn = pfn;
						if (!first_dup) first_dup = a - segs[i].s;
						break;
					}
				}
				if (nseen < 8192) seen[nseen++] = pfn;
			}
			if (1)
				fprintf(out, "%.3f %d %016" PRIx64 " %d %" PRIu64 " %d %" PRIx64 " %016" PRIx64 "\n",
					now_s() - t0, i, segs[i].s, ntot, npresent,
					dup_count, dup_pfn, first_dup);
			fflush(out);
		}
		close(pmf);
		usleep(20000);
	}
	fprintf(out, "# end %.3f\n", now_s() - t0);
	fclose(out);
	return 0;
}
