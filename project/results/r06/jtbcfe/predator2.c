// predator2.c - interpose pread64; for class-size preads into the arena
// window, arm a HARDWARE WATCHPOINT (DR0) on the destination buffer via a
// ptrace child attached to the preading thread.  Any write to the buffer
// between pread-return and parse completion is trapped and its RIP logged.
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
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <errno.h>

static FILE *logf;
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

struct wparg { pid_t tid; uint64_t addr; uint64_t len; };

/* child: attach to the tid, set DR0 watchpoint, report writes for ~3s */
static void *watcher_thread(void *a) {
	struct wparg *w = a;
	pid_t pid = w->tid;
	if (ptrace(PTRACE_SEIZE, pid, 0, 0) < 0) {
		fprintf(logf, "WP seize fail %s\n", strerror(errno));
		fflush(logf); return NULL;
	}
	/* interrupt to poke debug regs */
	kill(pid, SIGSTOP); /* hint; actual stop via interrupt */
	if (ptrace(PTRACE_INTERRUPT, pid, 0, 0) < 0) { /* seize+interrupt */
	}
	int st;
	waitpid(pid, &st, __WALL);

	size_t droff = (size_t)&((struct user *)0)->u_debugreg[0];
	ptrace(PTRACE_POKEUSER, pid, droff + 0*8, w->addr);           /* DR0 */
	ptrace(PTRACE_POKEUSER, pid, droff + 7*8, (1UL << 0));        /* DR7: L0 */
	unsigned long dr7 = (1UL << 0);                                /* local enable 0 */
	dr7 |= (0x1UL << 16);   /* RW0 = 01 write break */
	dr7 |= (0x0UL << 18);   /* LEN0 = 00 (1 byte, x86_64) */
	ptrace(PTRACE_POKEUSER, pid, droff + 7*8, dr7);
	ptrace(PTRACE_CONT, pid, 0, 0);

	uint64_t t0 = now_ns();
	while (now_ns() - t0 < 3000000000ULL) {
		pid_t r = waitpid(pid, &st, __WALL | WNOHANG);
		if (r < 0) break;
		if (r == pid) {
			if (WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP) {
				struct user_regs_struct regs;
				ptrace(PTRACE_GETREGS, pid, 0, &regs);
				pthread_mutex_lock(&lk);
				fprintf(logf, "WPWRITE rip=%llx addr=%llx\n",
					(unsigned long long)regs.rip,
					(unsigned long long)regs.rax);
				fflush(logf);
				pthread_mutex_unlock(&lk);
				/* clear DR6 status and continue */
				ptrace(PTRACE_POKEUSER, pid, droff + 6*8, 0);
				ptrace(PTRACE_CONT, pid, 0, 0);
				continue;
			}
			if (WIFEXITED(st) || WIFSIGNALED(st)) break;
			/* forward other stops */
			ptrace(PTRACE_CONT, pid, 0, WSTOPSIG(st) == SIGSTOP ? 0 : WSTOPSIG(st));
		}
		usleep(200);
	}
	/* disarm + detach */
	ptrace(PTRACE_INTERRUPT, pid, 0, 0);
	waitpid(pid, &st, __WALL);
	ptrace(PTRACE_POKEUSER, pid, droff + 7*8, 0);
	ptrace(PTRACE_DETACH, pid, 0, 0);
	fprintf(logf, "WP done\n"); fflush(logf);
	return NULL;
}

static ssize_t do_pread(int fd, void *buf, size_t count, off_t off) {
	ssize_t ret = syscall(SYS_pread64, fd, buf, count, off);
	{ uint64_t w = (uint64_t)buf >> 40;
	  if (w >= 16 && w < 64 && count >= 256 && count <= 8192 && ret > 0) {
		const unsigned char *p = (const unsigned char *)buf;
		ssize_t i, nz = 0;
		for (i = 0; i < ret; i++) if (p[i] == 0) nz++;
		if (nz) {
			pthread_mutex_lock(&lk);
			fprintf(logf, "CLASSAUDIT buf=%p off=%ld ret=%ld ZEROS=%ld\n",
				buf, (long)off, (long)ret, (long)nz);
			fflush(logf);
			pthread_mutex_unlock(&lk);
		}
		{
			/* arm the watchpoint on the failing class's buffer */
			static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
			static int armed = 0;
			pthread_mutex_lock(&once);
			if (!armed && count == 2352 && off == 9760807) {
				armed = 1;
				pthread_t t;
				struct wparg *w2 = malloc(sizeof(*w2));
				w2->tid = syscall(SYS_gettid);
				w2->addr = (uint64_t)buf;
				w2->len = count;
				pthread_mutex_unlock(&once);
				fprintf(logf, "ARM tid=%d buf=%p count=%zu off=%ld\n",
					(int)w2->tid, buf, count, (long)off);
				fflush(logf);
				pthread_create(&t, NULL, watcher_thread, w2);
				goto out;
			}
			pthread_mutex_unlock(&once);
		}
	  } }
out:
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
	snprintf(nm, sizeof(nm), "/tmp/pred2_%d.log", getpid());
	logf = fopen(nm, "a");
	if (logf) setvbuf(logf, NULL, _IOLBF, 0);
}
