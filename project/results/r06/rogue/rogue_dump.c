/* rogue_dump.c — SIGSEGV forensic dumper for the r06 present-RO family.
 * Preload with the MODE hook semantics: constructor enters MODE (prctl 80),
 * SIGSEGV handler dumps maps + smaps + pagemap of si_addr, then dies.
 * gcc -shared -fPIC -O2 -o rogue_dump.so rogue_dump.c
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef PR_CORTEN_MODE
#define PR_CORTEN_MODE 80
#define CORTEN_MODE_ENTER 1
#endif

static unsigned long g_addr;

static void dump(int sig, siginfo_t *si, void *uc)
{
	char buf[16384];
	int fd, n, mf;
	unsigned long page, off, entry = 0;
	char path[128];

	(void)sig; (void)uc;
	fd = open("/tmp/rogue_dump.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		_exit(97);

	n = snprintf(buf, sizeof(buf),
		     "SIGSEGV si_addr=%p si_code=%d pid=%d\n",
		     si->si_addr, si->si_code, getpid());
	write(fd, buf, n);

	/* /proc/self/maps (whole thing, small enough) */
	mf = open("/proc/self/maps", O_RDONLY);
	n = snprintf(buf, sizeof(buf), "==== maps ====\n");
	write(fd, buf, n);
	while ((n = read(mf, buf, sizeof(buf))) > 0)
		write(fd, buf, n);
	close(mf);

	/* pagemap entry of si_addr */
	page = (unsigned long)si->si_addr & ~0xfffUL;
	off = (page / 4096) * sizeof(entry);
	mf = open("/proc/self/pagemap", O_RDONLY);
	if (mf >= 0) {
		if (pread(mf, &entry, sizeof(entry), off) == sizeof(entry)) {
			n = snprintf(buf, sizeof(buf),
				     "==== pagemap %lx ====\nentry=%lx "
				     "present=%d swapped=%d pfn=%lx "
				     "exclusive=%d softdirty=%d\n",
				     page, entry, (int)(entry >> 63 & 1),
				     (int)(entry >> 62 & 1),
				     entry & ((1UL << 55) - 1),
				     (int)(entry >> 56 & 1),
				     (int)(entry >> 55 & 1));
			write(fd, buf, n);
		}
		close(mf);
	}

	/* the smaps block covering si_addr */
	snprintf(path, sizeof(path), "/proc/self/smaps");
	mf = open(path, O_RDONLY);
	if (mf >= 0) {
		char line[512], keep[512] = "";
		int in = 0;
		FILE *f = fdopen(mf, "r");
		unsigned long a = (unsigned long)si->si_addr;
		unsigned long s, e;
		n = snprintf(buf, sizeof(buf), "==== smaps block ====\n");
		write(fd, buf, n);
		while (fgets(line, sizeof(line), f)) {
			if (sscanf(line, "%lx-%lx", &s, &e) == 2) {
				in = (a >= s && a < e);
				strncpy(keep, line, sizeof(keep) - 1);
			}
			if (in)
				write(fd, line, strlen(line));
		}
		(void)keep;
		fclose(f);
	}
	n = snprintf(buf, sizeof(buf), "==== end ====\n");
	write(fd, buf, n);
	close(fd);
	_exit(98);
}

static void __attribute__((constructor)) rogue_init(void)
{
	struct sigaction sa;

	prctl(PR_CORTEN_MODE, CORTEN_MODE_ENTER, 0, 0, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = dump;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	g_addr = 0;
	(void)g_addr;
}
