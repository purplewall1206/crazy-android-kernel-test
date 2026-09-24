/* dg_probe3.c - D-G'' fix guest judgment probe (r06 night, verify shift).
 * Reproduces the JVM CDS file-MAP_FIXED-into-arena shape and the E1/E2/E4/E5
 * experiments from results/r05/dg2-analysis.md sec 4, on the D-G'' fixed
 * kernel (corten=on boot).  Static-ish, no deps.
 *
 * Modes (argv[1]):
 *   e1    JVM-faithful: MODE ENTER -> PROT_NONE reserve 128M anon ->
 *         trim head 10M + tail 6M (munmap) -> two file MAP_FIXED maps
 *         at the trimmed base (JVM lengths/offsets) -> read file bytes
 *         back (expect 0x7f 'E' 'L' 'F'), no SIGSEGV.  Also prints the
 *         /proc/self/maps sandwich around the file mapping (head piece
 *         [anon:corten_arena] + rw-p file + tail piece [anon:corten_arena]).
 *   e1b   probe2 lineage: DECLARE 4M arena, commit window 0 via anon
 *         MAP_FIXED (MARK) + write, file MAP_FIXED at q = p + PMD (the
 *         punched hole sits in window 1; head piece keeps window 0 data) ->
 *         read 0x7f; head magic must survive.
 *   e2    E1 control: anonymous MAP_FIXED at the same q (MARK route):
 *         write+read roundtrip must pass (pre-fix mmap-fixed-rw already
 *         passed; this stays green if the fix did not regress MARK).
 *   e4    e1b + whole-arena mprotect(RW) before the file mmap (EXACT ->
 *         ar->prot lift).  Post-fix the file byte must be 0x7f, NOT 0x00
 *         (the pre-fix silent-fresh-anon-page shape must be gone).
 *   e5    NOREPLACE/FIXED asymmetry on a live arena window: r1
 *         MAP_FIXED_NOREPLACE -> MAP_FAILED/EEXIST; r2 MAP_FIXED file ->
 *         success and content readable (0x7f).
 *   fork  multi-piece arena fork smoke: DECLARE arena, commit window 0,
 *         punch a file hole in window 1, then fork; child + parent touch
 *         head / file / tail; waitpid must reap 0.  Walks fork_demote
 *         across a 3-piece arena.
 *
 * A MODE-sourced SIGSEGV (si_code SEGV_ACCERR) is the D-G'' fingerprint;
 * the handler prints it and exits 42 so a regression is loud.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PR_CORTEN_ARENA
#define PR_CORTEN_ARENA 79
#endif
#define CORTEN_ARENA_DECLARE 0
#define CORTEN_ARENA_RELEASE 1
#ifndef PR_CORTEN_MODE
#define PR_CORTEN_MODE 80
#define CORTEN_MODE_ENTER 1
#endif

#define PMD (2UL << 20)
#define PAGE 4096UL

static void on_segv(int sig, siginfo_t *si, void *uc __attribute__((unused)))
{
	fprintf(stderr,
		"PROBE-SIGSEGV sig=%d si_code=%d si_addr=%p (D-G'' fingerprint: ACCERR=2)\n",
		sig, si->si_code, si->si_addr);
	_exit(42);
}

static void install_segv_handler(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
}

static long arena_prctl(unsigned long op, unsigned long addr, unsigned long len)
{
	return syscall(SYS_prctl, PR_CORTEN_ARENA, op, addr, len, 0UL);
}

static int mode_enter(void)
{
	return prctl(PR_CORTEN_MODE, CORTEN_MODE_ENTER, 0, 0, 0);
}

/* Print /proc/self/maps lines overlapping [lo, hi). */
static void dump_maps(unsigned long lo, unsigned long hi)
{
	char buf[512];
	FILE *f = fopen("/proc/self/maps", "r");

	if (!f)
		return;
	fprintf(stderr, "--- maps sandwich [%lx,%lx) ---\n", lo, hi);
	while (fgets(buf, sizeof(buf), f)) {
		unsigned long s, e;

		if (sscanf(buf, "%lx-%lx", &s, &e) == 2 && s < hi && e > lo)
			fputs(buf, stderr);
	}
	fclose(f);
	fprintf(stderr, "--- end sandwich ---\n");
}

static int read_stats(const char *key, long *out)
{
	char line[128];
	FILE *f = fopen("/sys/kernel/debug/corten/arena_stats", "r");

	if (!f)
		return -1;
	*out = -1;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%127s %ld", line, out) == 2 &&
		    !strcmp(line, key))
			break;
	fclose(f);
	return 0;
}

static long stats_delta(const char *key)
{
	long before, after;

	if (read_stats(key, &before) || read_stats(key, &after))
		return -999;
	return after - before;
}

/* Compare the mapped bytes against a pread() of the SAME file offsets:
 * no magic-number assumptions (the file offset at the mapping base is
 * whatever the caller passed to mmap).  4 probe points.
 */
static int expect_file_bytes(const char *tag, volatile unsigned char *p,
			     int fd, off_t base_off, size_t map_len)
{
	size_t offs[4] = { 0, 0x10, 0x1000, map_len / 2 };
	int i, ok = 1;
	struct stat st;

	if (fstat(fd, &st) == 0)
		for (i = 0; i < 4; i++)
			if ((long long)base_off + (long long)offs[i] >= (long long)st.st_size)
				offs[i] = 0;	/* beyond EOF: skip (SIGBUS) */

	for (i = 0; i < 4; i++) {
		unsigned char want, got = p[offs[i]];

		if (offs[i] == 0 && i == 3)
			continue;	/* clamped skip */
		if (pread(fd, &want, 1, (off_t)base_off + offs[i]) != 1)
			want = 0xee;
		if (got != want) {
			fprintf(stderr,
				"[%s] MISMATCH at +%#lx: mapped=%02x pread=%02x\n",
				tag, offs[i], got, want);
			ok = 0;
		}
	}
	fprintf(stderr, "[%s] mapped==pread over 4 probe points -> %s\n",
		tag, ok ? "PASS (legacy filemap serves)" : "FAIL");
	return ok ? 0 : 1;
}

/* The JVM CDS shape (dg2-analysis sec 2 fact chain). */
static int run_e1(void)
{
	const size_t RESERVE = 128UL << 20;
	const size_t HEAD_TRIM = 10UL << 20;
	const size_t TAIL_TRIM = 6UL << 20;
	volatile unsigned char *v;
	unsigned long base;
	int fd, rc = 0;

	install_segv_handler();
	if (mode_enter()) {
		perror("MODE ENTER");
		return 3;
	}
	v = mmap(NULL, RESERVE, PROT_NONE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (v == MAP_FAILED) {
		perror("reserve");
		return 3;
	}
	base = (unsigned long)v;
	fprintf(stderr, "[e1] reserve %lx (%s window)\n", base,
		base >= 0x100000000000UL && base < 0x400000000000UL ?
			"MODE" : "LEGACY");
	/* Step 2+3: trim head and tail (munmap CHUNKs). */
	if (munmap((void *)base, HEAD_TRIM) ||
	    munmap((void *)(base + RESERVE - TAIL_TRIM), TAIL_TRIM)) {
		perror("trim");
		return 3;
	}
	fd = open("/bin/true", O_RDONLY);
	if (fd < 0) {
		perror("/bin/true");
		return 3;
	}
	/* Step 4+5: the two JVM map_archive MAP_FIXED maps. */
	if (mmap((void *)(base + HEAD_TRIM), 4722688UL,
		 PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd,
		 0x1000) == MAP_FAILED) {
		perror("file mmap 1");
		return 3;
	}
	if (mmap((void *)(base + HEAD_TRIM + 0x481000UL), 8499200UL,
		 PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd,
		 0x482000) == MAP_FAILED) {
		perror("file mmap 2");
		return 3;
	}
	fprintf(stderr, "[e1] four-step sequence done (reserve, 2 trims, 2 file FIXED)\n");
	/* The relocation read that died pre-fix at +0x10. */
	fprintf(stderr, "[e1] read @base+0x10: %02x\n",
		*(volatile unsigned char *)(base + HEAD_TRIM + 0x10));
	rc |= expect_file_bytes("e1", (volatile unsigned char *)(base + HEAD_TRIM),
				fd, 0x1000, 4722688UL);
	dump_maps(base + HEAD_TRIM - 4 * PAGE,
		  base + HEAD_TRIM + 0x481000UL + 8499200UL + 4 * PAGE);
	close(fd);
	printf("E1-RC %d\n", rc);
	return rc;
}

/* probe2 lineage: explicit DECLARE arena, hole at q = p + PMD. */
static int run_e1b(int do_mprotect_first)
{
	const unsigned long BASE = 0x10000000UL;
	const size_t LEN = 4 * PMD;
	const unsigned long p = BASE, q = BASE + PMD;
	volatile unsigned char *w, *f;
	int fd, rc = 0;

	install_segv_handler();
	w = mmap((void *)BASE, LEN, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
		 MAP_FIXED_NOREPLACE, -1, 0);
	if (w == MAP_FAILED) {
		perror("arena mmap");
		return 3;
	}
	if (arena_prctl(CORTEN_ARENA_DECLARE, BASE, LEN)) {
		perror("DECLARE");
		return 3;
	}
	fprintf(stderr, "[e] arena %lx-%lx declared\n", BASE, BASE + LEN);
	/* Commit window 0 with a magic (MARK route). */
	w = mmap((void *)p, PMD, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
		 -1, 0);
	if (w == MAP_FAILED) {
		perror("chunk mmap");
		return 3;
	}
	*(volatile unsigned long *)p = 0x5a5ac0deUL;
	if (do_mprotect_first) {
		if (mprotect((void *)BASE, LEN, PROT_READ | PROT_WRITE)) {
			perror("mprotect arena");
			return 3;
		}
		fprintf(stderr, "[e4] whole-arena mprotect done (EXACT route)\n");
	}
	fd = open("/bin/true", O_RDONLY);
	if (fd < 0) {
		perror("/bin/true");
		return 3;
	}
	f = mmap((void *)q, PAGE, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_FIXED, fd, 0);
	if (f == MAP_FAILED) {
		perror("file mmap @q");
		return 3;
	}
	fprintf(stderr, "[e] file MAP_FIXED at q=%lx (p+PMD) ok\n", q);
	rc |= expect_file_bytes(do_mprotect_first ? "e4" : "e1b", f, fd, 0,
				PAGE);
	if (*(volatile unsigned long *)p != 0x5a5ac0deUL) {
		fprintf(stderr, "[%s] head magic clobbered -> FAIL\n",
			do_mprotect_first ? "e4" : "e1b");
		rc |= 2;
	} else {
		fprintf(stderr, "[%s] head window-0 magic intact\n",
			do_mprotect_first ? "e4" : "e1b");
	}
	dump_maps(q - 4 * PAGE, q + 8 * PAGE);
	close(fd);
	printf("E%s-RC %d\n", do_mprotect_first ? "4" : "1B", rc);
	return rc;
}

/* E2: anonymous MAP_FIXED at the same spot still passes (MARK). */
static int run_e2(void)
{
	const unsigned long BASE = 0x20000000UL;
	const size_t LEN = 4 * PMD;
	const unsigned long q = BASE + PMD;
	volatile unsigned char *w;
	long punch_before;
	int rc = 0;

	install_segv_handler();
	read_stats("mmap_punches", &punch_before);
	w = mmap((void *)BASE, LEN, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
		 MAP_FIXED_NOREPLACE, -1, 0);
	if (w == MAP_FAILED) {
		perror("arena mmap");
		return 3;
	}
	if (arena_prctl(CORTEN_ARENA_DECLARE, BASE, LEN)) {
		perror("DECLARE");
		return 3;
	}
	w = mmap((void *)q, PAGE, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (w == MAP_FAILED) {
		perror("anon MAP_FIXED @q");
		return 3;
	}
	*(volatile unsigned long *)q = 0xfeedface5a5aUL;
	if (*(volatile unsigned long *)q != 0xfeedface5a5aUL) {
		fprintf(stderr, "[e2] roundtrip mismatch -> FAIL\n");
		rc |= 1;
	} else {
		fprintf(stderr, "[e2] anon MAP_FIXED write+read roundtrip PASS\n");
	}
	fprintf(stderr, "[e2] mmap_punches delta over E2 = %ld (mark route must not punch)\n",
		stats_delta("mmap_punches") - punch_before);
	printf("E2-RC %d\n", rc);
	return rc;
}

/* E5: NOREPLACE/FIXED asymmetry (F-B backstop must keep rejecting
 * NOREPLACE; the FIXED leg must punch and serve real file content). */
static int run_e5(void)
{
	const unsigned long BASE = 0x30000000UL;
	const size_t LEN = 4 * PMD;
	const unsigned long q = BASE + PMD;
	void *r1, *r2;
	int fd, rc = 0;

	install_segv_handler();
	r1 = mmap((void *)BASE, LEN, PROT_READ | PROT_WRITE,
		  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
		  MAP_FIXED_NOREPLACE, -1, 0);
	if (r1 == MAP_FAILED) {
		perror("arena mmap");
		return 3;
	}
	if (arena_prctl(CORTEN_ARENA_DECLARE, BASE, LEN)) {
		perror("DECLARE");
		return 3;
	}
	fd = open("/bin/true", O_RDONLY);
	if (fd < 0) {
		perror("/bin/true");
		return 3;
	}
	r1 = mmap((void *)q, PAGE, PROT_READ,
		  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (r1 == MAP_FAILED && errno == EEXIST) {
		fprintf(stderr, "[e5] r1 NOREPLACE -> MAP_FAILED EEXIST PASS\n");
	} else {
		fprintf(stderr, "[e5] r1 NOREPLACE -> %p errno=%d FAIL (want EEXIST)\n",
			r1, errno);
		rc |= 1;
	}
	r2 = mmap((void *)q, PAGE, PROT_READ | PROT_WRITE,
		  MAP_PRIVATE | MAP_FIXED, fd, 0);
	if (r2 == MAP_FAILED) {
		fprintf(stderr, "[e5] r2 MAP_FIXED -> MAP_FAILED errno=%d FAIL\n",
			errno);
		rc |= 2;
		return rc | 4;
	}
	fprintf(stderr, "[e5] r2 MAP_FIXED -> %lx ok\n", (unsigned long)r2);
	rc |= expect_file_bytes("e5", (volatile unsigned char *)r2, fd, 0,
				PAGE);
	close(fd);
	printf("E5-RC %d\n", rc);
	return rc;
}

/* Multi-piece arena fork smoke (fork_demote across 3 pieces). */
static int run_fork(void)
{
	const unsigned long BASE = 0x40000000UL;
	const size_t LEN = 4 * PMD;
	const unsigned long q = BASE + PMD;
	volatile unsigned char *f;
	int fd, status = 0;
	pid_t pid;

	install_segv_handler();
	if (mmap((void *)BASE, LEN, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
		 MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED) {
		perror("arena mmap");
		return 3;
	}
	if (arena_prctl(CORTEN_ARENA_DECLARE, BASE, LEN)) {
		perror("DECLARE");
		return 3;
	}
	if (mode_enter()) {	/* MODE bit: fork demote path needs it */
		perror("MODE ENTER");
		return 3;
	}
	/* Window 0: committed magic. */
	if (mmap((void *)BASE, PMD, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
		 -1, 0) == MAP_FAILED) {
		perror("w0 mmap");
		return 3;
	}
	*(volatile unsigned long *)BASE = 0xc0ffeeUL;
	fd = open("/bin/true", O_RDONLY);
	if (fd < 0) {
		perror("/bin/true");
		return 3;
	}
	/* Window 1: file hole -> 3-piece arena (head/hole/tail). */
	f = mmap((void *)q, PAGE, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_FIXED, fd, 0);
	if (f == MAP_FAILED) {
		perror("hole mmap");
		return 3;
	}
	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 3;
	}
	if (pid == 0) {
		/* Child: demoted (metadata-only); touch all three pieces. */
		unsigned long magic = *(volatile unsigned long *)BASE;
		int crc = (f[0] == 0x7f);
		volatile unsigned char *tail =
			(volatile unsigned char *)(BASE + 2 * PMD + PAGE);
		int trc;

		*tail = 0xaa;
		trc = (*tail == 0xaa);
		fprintf(stderr, "[fork-child] head=%lx file=%d tailrw=%d\n",
			magic, crc, trc);
		_exit((magic == 0xc0ffeeUL && crc && trc) ? 0 : 9);
	}
	if (waitpid(pid, &status, 0) != pid) {
		perror("waitpid");
		return 3;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "[fork-parent] child exit status=%d FAIL\n",
			status);
		printf("FORK-RC 1\n");
		return 1;
	}
	/* Parent side: same three pieces still serve. */
	{
		volatile unsigned char *tail =
			(volatile unsigned char *)(BASE + 2 * PMD + PAGE);
		int ok = *(volatile unsigned long *)BASE == 0xc0ffeeUL &&
			 f[0] == 0x7f;
		*tail = 0x55;
		ok &= (*tail == 0x55);
		fprintf(stderr, "[fork-parent] head+file+tail %s\n",
			ok ? "PASS" : "FAIL");
		close(fd);
		printf("FORK-RC %d\n", ok ? 0 : 1);
		return ok ? 0 : 1;
	}
}

int main(int argc, char **argv)
{
	const char *m = argc > 1 ? argv[1] : "";

	if (!strcmp(m, "e1"))
		return run_e1();
	if (!strcmp(m, "e1b"))
		return run_e1b(0);
	if (!strcmp(m, "e2"))
		return run_e2();
	if (!strcmp(m, "e4"))
		return run_e1b(1);
	if (!strcmp(m, "e5"))
		return run_e5();
	if (!strcmp(m, "fork"))
		return run_fork();
	fprintf(stderr, "usage: %s {e1|e1b|e2|e4|e5|fork}\n", argv[0]);
	return 64;
}
