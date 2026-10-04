// SPDX-License-Identifier: GPL-2.0
/*
 * mvc_j3_workload.c - J3 oracle workload (M-V V-C, MV_VMA_FREE_SPEC.md
 * sec 3.3.4): a deterministic MODE-process shape whose /proc face the
 * oracle snapshots for the cross-boot byte diff (the A.1-era shadow
 * baseline vs the V-C dual-source renderer), plus the live external
 * data face (process_vm_readv) the GUP-slow probe restores.
 *
 * Shape (fully deterministic under setarch -R, no ASLR):
 *   - 3 anonymous window regions: RW 8MiB (touched), RW 2MiB (touched),
 *     PROT_NONE 2MiB reserve (the glibc new_heap shape, untounched);
 *   - 1 window region parked by a full-coverage munmap (S-4: must NOT
 *     render);
 *   - 1 private file mapping (MAP_PRIVATE of this binary, 2MiB window
 *     region after takeover, read once -- the V-B FILE face);
 *   - delegated-domain legacy: brk growth + one low MAP_PRIVATE file
 *     mapping of /proc/self/exe... deliberately none -- exec/stack/
 *     vdso/brk are the delegated rows both kernels render identically
 *     from the tree.
 *
 * At the checkpoint it writes its own /proc/self/{maps,smaps,numa_maps}
 * and the PROCMAP_QUERY answers into $SNAP, then parks until SIGTERM.
 *
 * Build (host or guest, static):
 *   gcc -static -O2 -Wall -Wextra -o mvc_j3_workload mvc_j3_workload.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PR_CORTEN_MODE
#define PR_CORTEN_MODE		80
#define CORTEN_MODE_ENTER	1
#endif

/* PROCMAP_QUERY (include/uapi/linux/fs.h) -- minimal local copy,
 * byte-for-byte the kernel's truth: the cmd is _IOWR over magic 'f'
 * nr 17 with the 104-byte struct (the local spelling that used
 * _IOC_NONE/'P'/nr 1 and a u32 vma_name_addr got ENOTTY).
 */
struct procmap_query {
	__u64 size;			/* in */
	__u64 query_flags;		/* in */
	__u64 query_addr;		/* in */
	__u64 vma_start;		/* out */
	__u64 vma_end;			/* out */
	__u64 vma_flags;		/* out */
	__u64 vma_page_size;		/* out */
	__u64 vma_offset;		/* out */
	__u64 inode;			/* out */
	__u32 dev_major;		/* out */
	__u32 dev_minor;		/* out */
	__u32 vma_name_size;		/* in/out */
	__u32 build_id_size;		/* in/out */
	__u64 vma_name_addr;		/* in */
	__u64 build_id_addr;		/* in */
};
#define PROCFS_IOCTL_MAGIC	'f'
#define PROCMAP_QUERY		_IOWR(PROCFS_IOCTL_MAGIC, 17, struct procmap_query)

#define WIN_LABEL	"[anon:corten_arena]"
#define MAGIC		0x5a3c6b9d1f2e4d7cULL

static volatile sig_atomic_t stop;

static void on_term(int sig)
{
	(void)sig;
	stop = 1;
}

static void snapshot_procfs(const char *dir, unsigned long win_rw)
{
	char path[256], buf[4096];
	const char *files[] = { "maps", "smaps", "numa_maps" };
	int i;

	for (i = 0; i < 3; i++) {
		int in, out;
		ssize_t n;

		snprintf(path, sizeof(path), "/proc/self/%s", files[i]);
		in = open(path, O_RDONLY);
		if (in < 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
		out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) {
			close(in);
			continue;
		}
		while ((n = read(in, buf, sizeof(buf))) > 0)
			if (write(out, buf, n) != n)
				break;
		close(in);
		close(out);
	}

	/* The pagemap truth of the first RW window page (bit 63 present).
	 */
	{
		int fd = open("/proc/self/pagemap", O_RDONLY);
		__u64 pme = 0;
		off_t off = (off_t)(win_rw >> 12) * 8;

		if (fd >= 0 && pread(fd, &pme, 8, off) == 8) {
			snprintf(path, sizeof(path), "%s/pagemap-first", dir);
			{
				int out = open(path, O_WRONLY | O_CREAT |
						   O_TRUNC, 0644);
				if (out >= 0) {
					dprintf(out, "0x%016llx\n",
						(unsigned long long)pme);
					close(out);
				}
			}
			close(fd);
		} else if (fd >= 0) {
			close(fd);
		}
	}

	/* PROCMAP_QUERY on the first RW window page: the covering row. */
	{
		int fd = open("/proc/self/maps", O_RDONLY);
		struct procmap_query q = { .size = sizeof(q),
					   .query_addr = win_rw, };
		int out;

		snprintf(path, sizeof(path), "%s/procmap-first", dir);
		out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0 && out >= 0 &&
		    !ioctl(fd, PROCMAP_QUERY, &q)) {
			dprintf(out, "start=0x%llx end=0x%llx flags=0x%llx\n",
				(unsigned long long)q.vma_start,
				(unsigned long long)q.vma_end,
				(unsigned long long)q.vma_flags);
		} else {
			dprintf(out, "ioctl rc=-1 errno=%d\n", errno);
		}
		if (fd >= 0)
			close(fd);
		if (out >= 0)
			close(out);
	}
}

int main(int argc, char **argv)
{
	const char *snapdir = argc > 1 ? argv[1] : "/tmp/j3";
	unsigned long win_rw = 0, magic_addr = 0;
	void *p, *park, *fp;
	char b;
	pid_t child;
	int fd, status;

	if (prctl(PR_CORTEN_MODE, CORTEN_MODE_ENTER, 0, 0, 0)) {
		fprintf(stderr, "[j3] MODE ENTER failed: %s (corten=on? "
			"CAP_SYS_ADMIN?)\n", strerror(errno));
		return 2;
	}

	/* Region 1: RW 8MiB, first page touched. */
	p = mmap(NULL, 8 << 20, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap rw8");
		return 2;
	}
	*(volatile unsigned long *)p = 0x11;
	*(volatile unsigned long *)((char *)p + 0x1500) = 0x22;
	win_rw = (unsigned long)p;

	/* Region 2: RW 2MiB touched. */
	p = mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap rw2");
		return 2;
	}
	*(volatile char *)p = 1;

	/* Region 3: PROT_NONE reserve (untouched). */
	p = mmap(NULL, 2 << 20, PROT_NONE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap none");
		return 2;
	}

	/* Region 4: private file mapping of this binary (V-B face). */
	fd = open("/proc/self/exe", O_RDONLY);
	if (fd >= 0) {
		fp = mmap(NULL, 2 << 20, PROT_READ,
			  MAP_PRIVATE, fd, 0);
		if (fp != MAP_FAILED)
			b = *(volatile char *)fp;
		(void)b;
		close(fd);
	}

	/* Region 5: created then parked (full-coverage munmap, S-4 --
	 * must disappear from maps on both kernels).
	 */
	park = mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (park != MAP_FAILED) {
		*(volatile char *)park = 1;
		if (munmap(park, 2 << 20))
			perror("munmap park");
	}

	/* The magic page the external reader must find through the
	 * window (GUP-slow probe face).
	 */
	p = mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap magic");
		return 2;
	}
	*(volatile unsigned long *)p = MAGIC;
	magic_addr = (unsigned long)p;

	/* The external data face: a child reads the magic through
	 * process_vm_readv (the A.3b #7 short-answer regression).
	 *
	 * W-6 A.1-compat variant: J3_SKIP_VMREADV=1 skips this leg.
	 * process_vm_readv through the window is the V-C gup-probe route
	 * (post-A.1); on the A.1 baseline it answers short and the
	 * registered workload exits rc=3 before snapshotting.  The maps
	 * face is untouched (the child's mm is never snapshotted).
	 */
	if (getenv("J3_SKIP_VMREADV")) {
		snapshot_procfs(snapdir, win_rw);
		signal(SIGTERM, on_term);
		printf("[j3] ready win_rw=0x%lx magic=0x%lx "
		       "(vmreadv leg skipped)\n", win_rw, magic_addr);
		fflush(stdout);
		while (!stop)
			pause();
		return 0;
	}
	child = fork();
	if (child == 0) {
		unsigned long v = 0;
		struct iovec l = { &v, sizeof(v) };
		struct iovec r = { (void *)magic_addr, sizeof(v) };
		ssize_t n;

		n = process_vm_readv(getppid(), &l, 1, &r, 1, 0);
		if (n == sizeof(v) && v == MAGIC)
			_exit(0);
		_exit(1);
	}
	waitpid(child, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr, "[j3] FAIL: process_vm_readv through the "
			"window lost the magic\n");
		return 3;
	}

	snapshot_procfs(snapdir, win_rw);

	signal(SIGTERM, on_term);
	printf("[j3] ready win_rw=0x%lx magic=0x%lx\n", win_rw, magic_addr);
	fflush(stdout);
	while (!stop)
		pause();

	return 0;
}
