// SPDX-License-Identifier: GPL-2.0
/*
 * corten_mode_smoke.c - T0a MODE-process transparent takeover smoke test
 * (guest side; run as root on a corten=on kernel, M4T0_SPEC.md sec 3).
 *
 * Shape of the check (no kernel modifications of the app needed -- the
 * "first line calls prctl" runner form from the spec):
 *
 *   1. ENTER (PR_CORTEN_MODE) -> GET reports 1
 *   2. mmap(NULL, anon private) lands in the arena window
 *      [0x1000_0000_0000, 0x4000_0000_0000); a page of it is writable
 *      through the arena fault path (the shell driver additionally
 *      checks the debugfs arenas ledger before/after).
 *   3. Out-of-whitelist mmaps (MAP_SHARED anon, MAP_POPULATE, MAP_STACK)
 *      stay OUTSIDE the window.
 *   4. munmap of the page-rounded length (the glibc free() shape)
 *      releases the whole arena: the whole 2M region leaves the map.
 *   5. An explicit-address MAP_FIXED mapping at the freed window address
 *      still works (legacy -- the window is ordinary VA).
 *   6. fork(): child sees the parent's pre-fork mapping content (COW),
 *      the child's own mmap(NULL) lands in the window again (MODE bit
 *      inherited), waitpid succeeds.
 *   7. EXIT (PR_CORTEN_MODE) -> GET reports 0, everything still
 *      munmaps cleanly.
 *
 * Exit code 0 = all checks passed.  Diagnostics on stderr.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PR_CORTEN_MODE
#define PR_CORTEN_MODE		80
#define CORTEN_MODE_ENTER	1
#define CORTEN_MODE_EXIT	2
#define CORTEN_MODE_GET		3
#endif

#define WIN_START	0x100000000000UL
#define WIN_END		0x400000000000UL

static int failures;

/* The swept-stock witness: written before ENTER, read back after the
 * refused EXIT.  The .bss page is the process's own TLS/heap stock the
 * W-4 entry sweep adopts -- the same family the pre-W-4 exit shape
 * destroyed (the guest smoke's exit-leg SIGSEGV at the canary read).
 */
static unsigned long guard[64];

#define CHECK(cond, name) do {						\
	if (cond) {							\
		fprintf(stderr, "PASS %s\n", name);			\
	} else {							\
		fprintf(stderr, "FAIL %s (line %d errno=%d)\n",		\
			name, __LINE__, errno);				\
		failures++;						\
	}								\
} while (0)

static int in_window(unsigned long addr)
{
	return addr >= WIN_START && addr < WIN_END;
}

/* S-5 (W1.f/D24): a parked or released window answers mincore() with
 * success and an all-zero vector (the region truth-walk), no longer
 * ENOMEM.  "Mapped" therefore means "at least one resident page";
 * "gone" means success with every vector byte zero, or ENOMEM (the
 * pre-S-5 shape, still legal for legacy neighbors).
 */
static int mapped(void *addr)
{
	unsigned char vec;

	return mincore(addr, 4096, &vec) == 0 && (vec & 1);
}

static int fully_unmapped(void *addr, unsigned long len)
{
	unsigned long i, n = (len + 4095) / 4096;
	unsigned char vec[4096];

	if (len > 4096UL * 4096)
		return 0;	/* beyond the vec: treat as mapped */
	if (mincore(addr, len, vec) < 0)
		return errno == ENOMEM;
	for (i = 0; i < n; i++)
		if (vec[i] & 1)
			return 0;	/* some page resident */
	return 1;			/* success, all-zero vector */
}

int main(void)
{
	unsigned long *p, *big, *shared, *pop, *stackish, *fixed;
	long l;
	pid_t pid;
	int status;

	/* 1. ENTER + GET */
	errno = 0;
	/* The stock witness: write before ENTER so the page is resident
	 * stock when the sweep adopts it.
	 */
	guard[0] = 0x5a5ac0deUL;
	guard[63] = 0xc0ffeeUL;
	CHECK(prctl(PR_CORTEN_MODE, CORTEN_MODE_ENTER, 0, 0, 0) == 0,
	      "mode-enter");
	CHECK(prctl(PR_CORTEN_MODE, CORTEN_MODE_GET, 0, 0, 0) == 1,
	      "mode-get-1");

	/* 2. plain anonymous private mmap(NULL) -> auto arena in window */
	p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(p != MAP_FAILED, "mmap-anon");
	CHECK(in_window((unsigned long)p), "mmap-anon-in-window");
	p[0] = 0x5a5ac0deUL;

	/* a bigger one: 3 pages -> own 2M-rounded arena further up */
	big = mmap(NULL, 3 * 4096UL, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(big != MAP_FAILED, "mmap-big");
	CHECK(in_window((unsigned long)big), "mmap-big-in-window");
	CHECK((unsigned long)big >= (unsigned long)p + (2UL << 20),
	      "mmap-big-next-arena");
	big[0] = 0x1234abcdUL;
	l = (3 * 4096UL / sizeof(long)) - 1;
	big[l] = 0xfeedfaceUL;

	/* 3. out-of-whitelist mappings must stay legacy */
	shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(shared != MAP_FAILED && !in_window((unsigned long)shared),
	      "mmap-shared-stays-legacy");

	pop = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	CHECK(pop != MAP_FAILED && !in_window((unsigned long)pop),
	      "mmap-populate-stays-legacy");

	/* W-3 flipped the MAP_STACK whitelist: thread stacks now route
	 * into the window (OQ-B's T0b precondition closed it).  The
	 * contract is success + window placement; the legacy-stays
	 * reading is the pre-W-3 shape.
	 */
	stackish = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	CHECK(stackish != MAP_FAILED && in_window((unsigned long)stackish),
	      "mmap-stack-routes-to-window");

	/* 4. release-on-full-coverage: munmap the request length (one
	 * page) of the first arena -> the WHOLE 2M region must go.
	 */
	CHECK(munmap(p, 4096) == 0, "munmap-release");
	CHECK(!mapped(p), "released-arena-unmapped");
	CHECK(fully_unmapped(p, 2UL << 20), "released-arena-gone");

	/* 5. explicit-address MAP_FIXED at the freed window address:
	 * legacy mapping, no arena (debugfs ledger checked by driver).
	 */
	fixed = mmap((void *)p, 4096, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	CHECK(fixed == (void *)p, "mmap-fixed-at-window-addr");
	if (fixed == (void *)p) {
		fixed[0] = 0x0ddba11UL;
		CHECK(fixed[0] == 0x0ddba11UL, "mmap-fixed-rw");
	}

	/* 6. fork: MODE inheritance + full parent arena demotion */
	pid = fork();
	if (pid == 0) {
		/* child: pre-fork mapping readable with the same value */
		int ok = big[0] == 0x1234abcdUL;
		unsigned long *c;

		/* child's own mmap(NULL) re-enters the arena */
		c = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		ok = ok && c != MAP_FAILED && in_window((unsigned long)c);
		if (ok)
			c[0] = 0xc0ffeeUL;
		_exit(ok ? 0 : 1);
	}
	CHECK(pid > 0, "fork");
	CHECK(pid > 0 && waitpid(pid, &status, 0) == pid, "waitpid");
	CHECK(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "fork-child-checks");
	/* parent still sees its data (fork demoted the arenas) */
	CHECK(big[0] == 0x1234abcdUL, "parent-content-after-fork");

	/* 7. EXIT: mappings still work, mode off, clean teardown */
	CHECK(munmap(big, 3 * 4096UL) == 0, "munmap-big");
	CHECK(munmap(shared, 4096) == 0, "munmap-shared");
	CHECK(munmap(pop, 4096) == 0, "munmap-pop");
	CHECK(munmap(stackish, 1 << 20) == 0, "munmap-stack");
	CHECK(munmap(fixed, 4096) == 0, "munmap-fixed");

	/* 7b. EXIT under the W-4 sticky-MODE contract (B5/D32): a hooked
	 * process carries swept stock (RF_ADOPTED regions) -- the
	 * explicit EXIT refuses (-EBUSY, MODE stays on, mappings keep
	 * working behind the refusal; process death does the teardown).
	 * A self-entered process (no stock) still exits cleanly and GET
	 * reports 0.  Both contracts encoded.
	 */
	{
		int rc = prctl(PR_CORTEN_MODE, CORTEN_MODE_EXIT, 0, 0, 0);
		int m;

		if (rc == 0) {
			m = prctl(PR_CORTEN_MODE, CORTEN_MODE_GET, 0, 0, 0);
			CHECK(m == 0, "mode-get-0");
		} else {
			CHECK(errno == EBUSY, "mode-exit-refused");
			m = prctl(PR_CORTEN_MODE, CORTEN_MODE_GET, 0, 0, 0);
			/* The GET==1 (sticky MODE) rides with the stock
			 * witness: the guard was written before ENTER,
			 * and big is munmapped by now, so the readable
			 * survivor is the tool's own bss -- exactly the
			 * swept pre-existing stock the refusal protects.
			 */
			CHECK(m == 1 &&
			      guard[0] == 0x5a5ac0deUL &&
			      guard[63] == 0xc0ffeeUL,
			      "mode-sticky-stock-intact");
		}
	}

	fprintf(stderr, failures ? "SMOKE FAIL (%d)\n" : "SMOKE PASS\n",
		failures);
	return failures ? 1 : 0;
}
