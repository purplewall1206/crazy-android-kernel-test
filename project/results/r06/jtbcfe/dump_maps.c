// jtbcfe_dump.c - MODE ClassFormatError forensics dumper.
// Destructor fires on JVM exit (incl. the ClassFormatError exit path --
// atexit destructors demonstrably run there, cf. corten_mode_hook probe).
// Dumps: full maps+smaps, page-granularity zero-scan + pagemap decode of
// every file mapping of interest (modules / classes.jsa), and the corten
// debugfs counters, to /tmp/jtbcfe_dump_<pid>.txt
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>

static FILE *out;

static void cat_file(const char *path) {
	FILE *f = fopen(path, "r");
	char buf[8192];
	size_t n;
	if (!f) { fprintf(out, "!! cannot open %s\n", path); return; }
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		fwrite(buf, 1, n, out);
	fclose(f);
}

/* read a range of this process's memory without SIGBUS/SIGSEGV risk */
static int safe_read(uint64_t addr, unsigned char *buf, size_t len) {
	FILE *f = fopen("/proc/self/mem", "rb");
	int ret;
	if (!f) return -1;
	if (fseek(f, (long)addr, SEEK_SET)) { fclose(f); return -1; }
	ret = fread(buf, 1, len, f) == (size_t)len ? 0 : -1;
	fclose(f);
	return ret;
}

static uint64_t pagemap_pfn(uint64_t addr, int *present, int *soft) {
	uint64_t ent = 0, off = (addr / 4096) * 8;
	static FILE *f = NULL;
	*present = 0; *soft = 0;
	if (!f) f = fopen("/proc/self/pagemap", "rb");
	if (!f) return ~0ULL;
	if (fseek(f, (long)off, SEEK_SET)) return ~0ULL;
	if (fread(&ent, 1, 8, f) != 8) return ~0ULL;
	*present = (ent >> 63) & 1;
	*soft = (ent >> 55) & 1;
	if (*present) return ent & ((1ULL << 55) - 1);
	return ~0ULL;
}

struct seg { uint64_t s, e; char path[512]; char perms[8]; };
static struct seg segs[256];
static int nsegs;

__attribute__((destructor)) static void jtbcfe_dump(void)
{
	char path[256];
	FILE *f;
	char line[1024];
	int i;

	snprintf(path, sizeof(path), "/tmp/jtbcfe_dump_%d.txt", getpid());
	out = fopen(path, "w");
	if (!out) return;
	setvbuf(out, NULL, _IOLBF, 0);

	fprintf(out, "== jtbcfe dump pid=%d\n", getpid());
	/* MODE state */
	{
		FILE *pr = fopen("/proc/self/status", "r");
		(void)pr;
	}
	fprintf(out, "-- maps:\n");
	cat_file("/proc/self/maps");
	fprintf(out, "-- smaps (abbrev): \n");
	f = fopen("/proc/self/smaps", "r");
	if (f) {
		int keep = 0;
		while (fgets(line, sizeof(line), f)) {
			if (strchr(line, '-') && strchr(line, ' ')) {
				keep = strstr(line, "modules") ||
				       strstr(line, "classes.jsa") ||
				       strstr(line, "corten_arena");
				if (keep) fprintf(out, "%s", line);
				continue;
			}
			if (keep && (strncmp(line, "Size:", 5) == 0 ||
				     strncmp(line, "Rss:", 4) == 0 ||
				     strncmp(line, "Anonymous:", 10) == 0 ||
				     strncmp(line, "Shared_Clean:", 13) == 0 ||
				     strncmp(line, "Shared_Dirty:", 13) == 0 ||
				     strncmp(line, "Private_Clean:", 14) == 0 ||
				     strncmp(line, "Private_Dirty:", 14) == 0))
				fprintf(out, "%s", line);
		}
		fclose(f);
	}

	/* collect interesting file segs */
	f = fopen("/proc/self/maps", "r");
	if (!f) goto done;
	while (fgets(line, sizeof(line), f) && nsegs < 256) {
		uint64_t s, e;
		char perms[8];
		char rest[512];
		if (sscanf(line, "%" SCNx64 "-%" SCNx64 " %7s %*s %*s %*s %511[^\n]",
			   &s, &e, perms, rest) < 3) continue;
		if (!strstr(rest, "/modules") && !strstr(rest, "classes.jsa"))
			continue;
		segs[nsegs].s = s; segs[nsegs].e = e;
		snprintf(segs[nsegs].path, 512, "%s", rest);
		snprintf(segs[nsegs].perms, 8, "%s", perms);
		nsegs++;
	}
	fclose(f);

	for (i = 0; i < nsegs; i++) {
		uint64_t a, zeropfn = ~0ULL, npresent = 0, nzero = 0;
		int nzpages = 0, ntot = 0, nzeropages = 0, zrun = 0, maxzrun = 0;
		unsigned char *buf = malloc(4096);
		uint64_t first_zero = 0;
		fprintf(out, "-- scan %d: %016" PRIx64 "-%016" PRIx64 " %s %s\n",
			i, segs[i].s, segs[i].e, segs[i].perms, segs[i].path);
		for (a = segs[i].s; a < segs[i].e; a += 4096) {
			int present, soft;
			uint64_t pfn;
			ntot++;
			pfn = pagemap_pfn(a, &present, &soft);
			if (present) { npresent++; if (pfn != ~0ULL && pfn == zeropfn) { nzeropages++; zrun++; if (zrun > maxzrun) maxzrun = zrun; } else zrun = 0; }
			if (!safe_read(a, buf, 4096)) {
				int j, allz = 1;
				for (j = 0; j < 4096; j += 64) {
					uint64_t *p = (uint64_t *)(buf + j);
					if (p[0] | p[1] | p[2] | p[3] | p[4] | p[5] | p[6] | p[7]) { allz = 0; break; }
				}
				if (allz) {
					nzero++;
					if (!first_zero) first_zero = a;
					if (zrun == 0 && !present) { }
				}
			}
		}
		(void)nzpages;
		fprintf(out, "   pages=%d present=%" PRIu64 " allzero=%d first_zero=%016" PRIx64 "\n",
			ntot, npresent, nzero, first_zero);
		free(buf);
	}
	/* sample: dump first 4096 bytes around first_zero of seg 0 */
	if (nsegs && segs[0].e - segs[0].s > 0) {
		unsigned char *buf = malloc(65536);
		if (buf && !safe_read(segs[0].s, buf, 65536)) {
			fprintf(out, "-- head 64B of seg0: ");
			for (i = 0; i < 64; i++) fprintf(out, "%02x", buf[i]);
			fprintf(out, "\n");
		}
		free(buf);
	}
	fprintf(out, "-- corten arena_stats:\n");
	cat_file("/sys/kernel/debug/corten/arena_stats");
	fprintf(out, "-- arenas:\n");
	cat_file("/sys/kernel/debug/corten/arenas");
done:
	fclose(out);
}
