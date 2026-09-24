// SPDX-License-Identifier: GPL-2.0
/*
 * corten_mode_hook.c - zero-modification MODE-process injection for the
 * T0 DoD runs (M4T0_SPEC.md sec 6/7, STATE D12 zero-modification gate).
 *
 * Form (the spec's LD_PRELOAD shape): this library's constructor runs
 * inside the libc init of the *target* process -- i.e. on the mm the
 * target will actually keep -- so one prctl(PR_CORTEN_MODE, ENTER)
 * takes the whole process over without touching its source.  This is
 * the only correct runner shape:
 *
 *   - "wrapper + exec" cannot work: execve() builds a fresh mm and the
 *     ENTER done in the wrapper's mm is lost with it.  run_t0_dod.sh
 *     demonstrates this and asserts the constructor form instead.
 *
 * The hook also plants an atexit() fork probe: by the time a normal
 * exit() runs, the target's worker threads are alive and the arena
 * registry is full -- fork()+waitpid() there is exactly the
 * M4T0_SPEC.md sec 5 multi-thread-with-arenas case (DEV-11 demotion
 * path; DoD item 3 "fork 行为").
 *
 * CORTEN_MODE_HOOK_STRICT=1 makes a failed ENTER/GET abort the process
 * before main(), so the runner's marker check catches a silently
 * no-op preload (corten=off kernel, missing capability, ...).
 *
 * Build (guest side or a libc-matching host):
 *   gcc -shared -fPIC -O2 -Wall -Wextra -o corten_mode_hook.so \
 *       corten_mode_hook.c
 * Use:
 *   LD_PRELOAD=/path/corten_mode_hook.so <target ...>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PR_CORTEN_MODE
#define PR_CORTEN_MODE		80	/* matches include/uapi/linux/prctl.h */
#define CORTEN_MODE_ENTER	1
#define CORTEN_MODE_GET		3
#endif

/*
 * Runs from exit(): worker threads are up, arenas are live -- fork()
 * must survive via the DEV-11 arena demotion (child inherits the MODE
 * bit, both sides keep their COW content) and waitpid must reap it.
 */
static void corten_mode_hook_fork_probe(void)
{
	pid_t pid = fork();

	if (pid < 0) {
		fprintf(stderr,
			"corten_mode_hook: fork-probe FAILED (fork: %m)\n");
		return;
	}
	if (pid == 0)
		_exit(42);

	{
		int st = -1;
		pid_t w = waitpid(pid, &st, 0);

		if (w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 42)
			fprintf(stderr,
				"corten_mode_hook: fork-probe OK (threads+arenas alive, child exited 42)\n");
		else
			fprintf(stderr,
				"corten_mode_hook: fork-probe FAILED (w=%d st=%d)\n",
				(int)w, st);
	}
}

__attribute__((constructor))
static void corten_mode_hook_enter(void)
{
	int strict = getenv("CORTEN_MODE_HOOK_STRICT") != NULL;

	if (prctl(PR_CORTEN_MODE, CORTEN_MODE_ENTER, 0, 0, 0)) {
		fprintf(stderr, "corten_mode_hook: ENTER failed: %m\n");
		if (strict)
			_exit(126);
		return;
	}

	if (prctl(PR_CORTEN_MODE, CORTEN_MODE_GET, 0, 0, 0) != 1) {
		fprintf(stderr, "corten_mode_hook: GET != 1 after ENTER\n");
		if (strict)
			_exit(126);
		return;
	}

	fprintf(stderr,
		"corten_mode_hook: MODE on (pid %d)\n", (int)getpid());
#ifndef CORTEN_MODE_HOOK_NOPROBE
	atexit(corten_mode_hook_fork_probe);
#endif
}
