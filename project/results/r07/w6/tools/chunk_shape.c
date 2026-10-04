// SPDX-License-Identifier: GPL-2.0
/* chunk_shape.c - W-6 probe-failure triage: reproduce the mva1_probe
 * CHUNK shape deterministically and park, so maps/arenas can be read
 * externally at each state.
 *   state A: 8M window region, first half touched
 *   state B: second half munmapped (dropped), first half still live
 *   state C: second half re-touched (re-materialized)
 * The state file records progress; SIGTERM exits 0.
 * Build: gcc -static -O2 -Wall -Wextra -o chunk_shape chunk_shape.c
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_CORTEN_MODE
#define PR_CORTEN_MODE		80
#define CORTEN_MODE_ENTER	1
#endif

static volatile sig_atomic_t stop;

static void on_term(int s)
{
	(void)s;
	stop = 1;
}

static void wait_state(const char *dir, const char *state)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/state", dir);
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n", state);
		fclose(f);
	}
	while (!stop) {
		int now = 0;
		f = fopen(path, "r");
		if (f) {
			char buf[64] = { 0 };
			fgets(buf, sizeof(buf), f);
			fclose(f);
			now = strncmp(buf, "next", 4) == 0;
		}
		if (now)
			break;
		usleep(20000);
	}
	unlink(path);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "/tmp/chunk";
	char *p;

	if (prctl(PR_CORTEN_MODE, CORTEN_MODE_ENTER, 0, 0, 0)) {
		perror("enter");
		return 2;
	}
	p = mmap(NULL, 8 << 20, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	*(volatile char *)p = 1;
	*(volatile char *)(p + (4 << 20)) = 1;
	signal(SIGTERM, on_term);

	wait_state(dir, "A-full");		/* full 8M, both halves touched */
	if (munmap(p + (4 << 20), 4 << 20))
		perror("munmap half");
	wait_state(dir, "B-dropped");		/* second half dropped */
	*(volatile char *)(p + (4 << 20)) = 2;	/* re-materialize */
	wait_state(dir, "C-remat");		/* second half re-touched */
	printf("[chunk] done\n");
	return 0;
}
