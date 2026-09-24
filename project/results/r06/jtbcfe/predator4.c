// predator4.c - interpose pread64; on the failing class resource, open an
// in-process perf_event hardware watchpoint (write) on the buffer, drain the
// ring for ~8ms and log every write RIP mapped to its module.
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
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

#include <linux/perf_event.h>

static FILE *logf;

static uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static long pe_open(struct perf_event_attr *a, pid_t pid) {
	return syscall(SYS_perf_event_open, a, pid, -1, -1, 0);
}

/* map rip -> "path+off" using /proc/self/maps */
static void resolve(uint64_t rip, char *out, size_t n) {
	FILE *f = fopen("/proc/self/maps", "r");
	char line[1024];
	out[0] = 0;
	if (!f) return;
	while (fgets(line, sizeof(line), f)) {
		uint64_t s, e;
		char rest[512];
		if (sscanf(line, "%lx-%lx %*s %*s %*s %*s %511[^\n]", &s, &e, rest) < 2)
			continue;
		if (rip >= s && rip < e) {
			char path[512];
			path[0] = 0;
			/* rest starts with perms; path is last field */
			char *p = strrchr(line, ' ');
			while (p && *p == ' ') p--;
			char *q = p;
			while (q > line && *(q-1) != ' ') q--;
			snprintf(path, 512, "%s", q);
			snprintf(out, n, "%s+0x%llx", path, (unsigned long long)(rip - s));
			fclose(f);
			return;
		}
	}
	fclose(f);
	snprintf(out, n, "anon?+0x%llx", (unsigned long long)rip);
}

static int watch_once2(uint64_t addr, uint64_t len_ns, int bp_type, const char *tag) {
	struct perf_event_attr a;
	memset(&a, 0, sizeof(a));
	a.type = PERF_TYPE_BREAKPOINT;
	a.size = sizeof(a);
	a.bp_addr = addr;
	a.bp_len = 1;                 /* HW_BREAKPOINT_LEN_1 */
	a.bp_type = bp_type;
	a.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
	a.sample_period = 1;          /* trap every match */
	a.wakeup_events = 1;
	a.disabled = 1;
	a.exclude_kernel = 1;         /* user-space writes only */
	int fd = (int)pe_open(&a, syscall(SYS_gettid));
	if (fd < 0) { fprintf(logf, "pe_open(%s) fail %s\n", tag, strerror(errno)); fflush(logf); return -1; }
	size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
	size_t ringsz = (1 + 64) * pagesz; /* must be 1+2^n pages */
	void *ring = mmap(NULL, ringsz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (ring == MAP_FAILED) { fprintf(logf, "mmap fail\n"); fflush(logf); close(fd); return -1; }
	struct perf_event_mmap_page *mp = ring;
	ioctl(fd, PERF_EVENT_IOC_RESET, 0);
	ioctl(fd, PERF_EVENT_IOC_REFRESH, 1);

	uint64_t t0 = now_ns();
	int n = 0;
	while (now_ns() - t0 < len_ns) {
		uint64_t head = __atomic_load_n(&mp->data_head, __ATOMIC_ACQUIRE);
		uint64_t tail = mp->data_tail;
		if (head == tail) { sched_yield(); continue; }
		char *dbase = (char *)ring + pagesz;
		uint64_t i = tail;
		while (i < head) {
			struct { struct perf_event_header h; uint64_t ip; uint32_t tid; } *e =
				(void *)(dbase + (i % (ringsz - pagesz)));
			if (e->h.type == PERF_RECORD_SAMPLE) {
				char where[512];
				resolve(e->ip, where, 512);
				fprintf(logf, "WRITE rip=%llx %s\n",
					(unsigned long long)e->ip, where);
				n++;
			}
			i += e->h.size;
		}
		mp->data_tail = head;
	}
	ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
	munmap(ring, ringsz);
	close(fd);
	fprintf(logf, "WATCH[%s] done hits=%d\n", tag, n);
	fflush(logf);
	return n;
}

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static void log_pread(int fd, void *buf, size_t count, off_t off, ssize_t ret) {
	if ((uint64_t)buf >> 40 >= 16 && (uint64_t)buf >> 40 < 64 &&
	    count >= 256 && count <= 8192 && ret > 0) {
		pthread_mutex_lock(&lk);
		fprintf(logf, "PREAD fd=%d buf=%p count=%zu off=%ld ret=%ld\n",
			fd, buf, count, (long)off, (long)ret);
		fflush(logf);
		if (count == 2352 && off == 9760807) {
			/* find the modules mapping and read-watch base+offset */
			uint64_t mbase = 0;
			FILE *mf = fopen("/proc/self/maps", "r");
			char ln[1024];
			while (mf && fgets(ln, sizeof(ln), mf)) {
				uint64_t s, e;
				if (strstr(ln, "lib/modules") &&
				    sscanf(ln, "%lx-%lx", &s, &e) == 2) {
					mbase = s; break;
				}
			}
			if (mf) fclose(mf);
			if (mbase) {
				fprintf(logf, "MODULES base=%lx watch=%lx\n", mbase, mbase + off);
				fflush(logf);
				watch_once2(mbase + off, 12000000ULL, 1, "MR");
			}
		}
		pthread_mutex_unlock(&lk);
	}
}

static ssize_t do_pread(int fd, void *buf, size_t count, off_t off) {
	ssize_t ret = syscall(SYS_pread64, fd, buf, count, off);
	log_pread(fd, buf, count, off, ret);
	return ret;
}
ssize_t pread(int fd, void *buf, size_t count, off_t off) { return do_pread(fd, buf, count, off); }
ssize_t pread64(int fd, void *buf, size_t count, off64_t off) { return do_pread(fd, buf, count, (off_t)off); }
ssize_t __pread64_chk(int fd, void *buf, size_t count, off64_t off, size_t bs) { (void)bs; return do_pread(fd, buf, count, (off_t)off); }
ssize_t __pread_chk(int fd, void *buf, size_t count, off_t off, size_t bs) { (void)bs; return do_pread(fd, buf, count, off); }

__attribute__((constructor)) static void init(void) {
	char nm[64];
	snprintf(nm, sizeof(nm), "/tmp/pred4_%d.log", getpid());
	logf = fopen(nm, "a");
	if (logf) setvbuf(logf, NULL, _IOLBF, 0);
}
