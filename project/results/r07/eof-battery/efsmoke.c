/* V4.3 EOF three-point closure probe: a 1-page file mapped as a
 * 3-page private file region (the loader's memsz > filesz shape).
 * Page 0 = in-file content; pages 1-2 = in-declaration past-EOF
 * (BSS tail): reads must see ZERO, writes must get private pages.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

int main(void)
{
	unsigned char *p, buf[64];
	int fd, rc = 0, pid, st;
	ssize_t wr;

	fd = open("/tmp/ef.bin", O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) { perror("open"); return 1; }
	memset(buf, 0xAB, sizeof(buf));
	wr = write(fd, buf, sizeof(buf));
	if (wr != sizeof(buf)) { perror("write"); return 1; }

	p = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) { perror("mmap"); return 1; }
	printf("map=%p\n", p);

	/* 1: in-file content readable */
	if (p[0] != 0xAB || p[63] != 0xAB) {
		printf("FAIL in-file content p0=%02x p63=%02x\n", p[0], p[63]);
		rc = 1;
	} else
		printf("PASS in-file content\n");

	/* 2: past-EOF reads are ZERO-FILLED */
	if (p[4096] != 0 || p[4096 + 4095] != 0 || p[8191] != 0) {
		printf("FAIL past-EOF read not zero: %02x %02x %02x\n",
		       p[4096], p[4096 + 4095], p[8191]);
		rc = 1;
	} else
		printf("PASS past-EOF read zero\n");

	/* 3: past-EOF write is private (readback + file untouched) */
	p[4096] = 0x5A;
	p[8191] = 0x5C;
	if (p[4096] != 0x5A || p[8191] != 0x5C) {
		printf("FAIL past-EOF write readback\n");
		rc = 1;
	} else
		printf("PASS past-EOF write readback\n");
	/* the file must still read its own content (private COW) */
	if (lseek(fd, 0, SEEK_SET) < 0 || read(fd, buf, sizeof(buf)) < 0 ||
	    buf[0] != 0xAB) {
		printf("FAIL file clobbered by past-EOF write\n");
		rc = 1;
	} else
		printf("PASS file not clobbered\n");

	/* 4: fork faithfulness over the past-EOF private page */
	pid = fork();
	if (pid == 0) {
		if (p[4096] == 0x5A)
			_exit(0);
		_exit(7);
	}
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		printf("FAIL fork past-EOF private page st=%d\n", st);
		rc = 1;
	} else
		printf("PASS fork past-EOF private page\n");

	/* 5: the region landed in the window (auto file arm) */
	if ((unsigned long)p >= 0x100000000000UL &&
	    (unsigned long)p < 0x400000000000UL)
		printf("PASS window placement %px\n", (void *)p);
	else
		printf("NOTE legacy placement %px (classify dependent)\n",
		       (void *)p);

	munmap(p, 3 * 4096);
	close(fd);
	unlink("/tmp/ef.bin");
	printf(rc ? "EFSMOKE FAIL rc=%d\n" : "EFSMOKE PASS\n", rc);
	return rc;
}
