// predator.c - interpose pread64 (all symbol variants); after each class-size
// pread into the arena window, poll the destination page (content fingerprint
// + pagemap PFN) for ~5ms; log the landing state and any transition.
#define _GNU_SOURCE
#define pread64 host_pread64_inline
#include <stdio.h>
#include <unistd.h>
#undef pread64
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <syscall.h>
#include <pthread.h>
#include <time.h>

static FILE *logf;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static struct req { uint64_t buf; size_t count; } cur;
static int go;

static uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static uint64_t pfn_of(uint64_t addr) {
	uint64_t ent = 0;
	static int fd = -1;
	if (fd < 0) fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) return ~0ULL;
	if (pread(fd, &ent, 8, (addr / 4096) * 8) != 8) return ~0ULL;
	if (!((ent >> 63) & 1)) return ~0ULL;
	return ent & ((1ULL << 55) - 1);
}

static void *watcher(void *arg) {
	(void)arg;
	for (;;) {
		while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) sched_yield();
		__atomic_store_n(&go, 0, __ATOMIC_RELEASE);
		uint64_t page = cur.buf & ~4095ULL;
		if (!page) continue;
		volatile uint32_t *p = (volatile uint32_t *)page;
		uint64_t t0 = now_ns();
		uint64_t last_pfn = pfn_of(page);
		uint32_t last_w = p[0] ^ p[1] ^ p[100] ^ p[500];
		uint64_t n = 0;
		while (now_ns() - t0 < 5000000ULL && n < 500) {
			uint64_t pfn = pfn_of(page);
			uint32_t w = p[0] ^ p[1] ^ p[100] ^ p[500];
			if (pfn != last_pfn || w != last_w) {
				pthread_mutex_lock(&lk);
				fprintf(logf, "TRANS ns=%lu page=%lx pfn %lx->%lx w %08x->%08x w0=%08x\n",
					(unsigned long)(now_ns() - t0), page,
					last_pfn, pfn, last_w, w, p[0]);
				fflush(logf);
				pthread_mutex_unlock(&lk);
				last_pfn = pfn; last_w = w;
			}
			n++;
			sched_yield();
		}
	}
	return NULL;
}

static void log_pread(int fd, void *buf, size_t count, off_t off, ssize_t ret) {
	if (0)
	{ /* audit disabled for noise; class-size only below */
		const unsigned char *p = (const unsigned char *)buf;
		ssize_t i, nz = 0, firstz = -1, lastz = -1;
		for (i = 0; i < ret; i++) {
			if (p[i] == 0) { nz++; if (firstz < 0) firstz = i; lastz = i; }
		}
		if (ret > 0 && nz) {
			pthread_mutex_lock(&lk);
			fprintf(logf, "AUDIT buf=%p ret=%ld ZEROS=%ld first=%ld last=%ld "
				"magic=%02x%02x%02x%02x tag10=%02x\n", buf, (long)ret,
				(long)nz, (long)firstz, (long)lastz,
				p[0], p[1], p[2], p[3],
				ret > 10 ? p[10] : 0);
			fflush(logf);
			pthread_mutex_unlock(&lk);
		}
	}
	{ uint64_t w = (uint64_t)buf >> 40; if (w >= 16 && w < 64 && count >= 256 && count <= 8192 && ret > 0) {
		cur.buf = (uint64_t)buf; cur.count = count;
		pthread_mutex_lock(&lk);
		fprintf(logf, "PREAD fd=%d buf=%p count=%zu off=%ld ret=%ld w0=%08x\n",
			fd, buf, count, (long)off, (long)ret,
			*(volatile uint32_t *)buf);
		fflush(logf);
		pthread_mutex_unlock(&lk);
		__atomic_store_n(&go, 1, __ATOMIC_RELEASE);
	}}
}

static ssize_t do_pread(int fd, void *buf, size_t count, off_t off) {
	ssize_t ret = syscall(SYS_pread64, fd, buf, count, off);
	log_pread(fd, buf, count, off, ret);
	return ret;
}

ssize_t pread(int fd, void *buf, size_t count, off_t off)
	{ return do_pread(fd, buf, count, off); }
ssize_t pread64(int fd, void *buf, size_t count, off64_t off)
	{ return do_pread(fd, buf, count, (off_t)off); }
ssize_t __pread64_chk(int fd, void *buf, size_t count, off64_t off, size_t bs)
	{ (void)bs; return do_pread(fd, buf, count, (off_t)off); }
ssize_t __pread_chk(int fd, void *buf, size_t count, off_t off, size_t bs)
	{ (void)bs; return do_pread(fd, buf, count, off); }

__attribute__((constructor)) static void init(void) {
	char nm[64];
	snprintf(nm, sizeof(nm), "/tmp/predator_%d.log", getpid());
	logf = fopen(nm, "a");
	if (logf) setvbuf(logf, NULL, _IOLBF, 0);
	pthread_t t;
	pthread_create(&t, NULL, watcher, NULL);
}
