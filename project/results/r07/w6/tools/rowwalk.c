// SPDX-License-Identifier: GPL-2.0
/* rowwalk.c - W-6 J2/J4 diagnosis probe: enumerate every covering row of
 * /proc/<pid>/maps via the PROCMAP_QUERY ioctl (the one /proc face that
 * survived the W-2 row-arm regression -- maps/smaps/numa are empty for
 * MODE mms).  Prints start-end flags name per row, address-ordered, so
 * the tree rows and the region rows can be told apart and counted.
 *
 * Build (host): gcc -static -O2 -Wall -Wextra -o rowwalk rowwalk.c
 * Usage (guest): ./rowwalk <pid>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/types.h>

struct procmap_query {
	__u64 size;
	__u64 query_flags;
	__u64 query_addr;
	__u64 vma_start;
	__u64 vma_end;
	__u64 vma_flags;
	__u64 vma_page_size;
	__u64 vma_offset;
	__u64 inode;
	__u32 dev_major;
	__u32 dev_minor;
	__u32 vma_name_size;
	__u32 build_id_size;
	__u64 vma_name_addr;
	__u64 build_id_addr;
};
#define PROCFS_IOCTL_MAGIC	'f'
#define PROCMAP_QUERY		_IOWR(PROCFS_IOCTL_MAGIC, 17, struct procmap_query)
#define PROCMAP_QUERY_COVERING_OR_NEXT_VMA	0x10

int main(int argc, char **argv)
{
	char name[256];
	int fd, n = 0;
	unsigned long addr = 0x10000;
	unsigned long long qflags = 0x10;
	char path[64];

	if (argc < 2)
		return 2;
	if (argc > 2)
		addr = strtoul(argv[2], NULL, 16);
	if (argc > 3)
		qflags = strtoull(argv[3], NULL, 16);
	snprintf(path, sizeof(path), "/proc/%s/maps", argv[1]);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 2;
	}
	while (addr < 0xfffffffffffff000UL) {
		struct procmap_query q;

		memset(&q, 0, sizeof(q));
		q.size = sizeof(q);
		q.query_flags = qflags;
		q.query_addr = addr;
		q.vma_name_addr = (unsigned long)name;
		q.vma_name_size = sizeof(name);
		if (ioctl(fd, PROCMAP_QUERY, &q)) {
			if (errno == ENOENT || errno == ESRCH)
				break;		/* past the last row */
			fprintf(stderr, "ioctl@%lx errno=%d\n", addr, errno);
			fprintf(stderr, "ioctl at %lx: %s\n",
				addr, strerror(errno));
			break;
		}
		name[sizeof(name) - 1] = '\0';
		printf("%012lx-%012lx flags=0x%llx pgoff=%llu ino=%llu %s\n",
		       (unsigned long)q.vma_start,
		       (unsigned long)q.vma_end,
		       (unsigned long long)q.vma_flags,
		       (unsigned long long)q.vma_offset,
		       (unsigned long long)q.inode,
		       q.vma_name_size ? name : "");
		n++;
		if (q.vma_end <= addr) {
			fprintf(stderr, "non-advancing row at %lx\n", addr);
			break;
		}
		addr = q.vma_end;
	}
	fprintf(stderr, "rows=%d\n", n);
	close(fd);
	return 0;
}
