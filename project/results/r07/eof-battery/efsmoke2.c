/* V4.3 EOF closure probe, round 2: the TRUNCATION verdict leg.
 * Map a 2-page region over a 2-page file, ftruncate to 0, then read:
 * the mmap contract says SIGBUS (the page WAS file-backed at declare,
 * so the reof gate must NOT serve zero -- the fetch's live-i_size gate
 * owns the verdict).
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>

static sigjmp_buf jb;
static volatile sig_atomic_t got;

static void bus(int s, siginfo_t *si, void *uc)
{
	(void)si; (void)uc;
	got = s;
	siglongjmp(jb, 1);
}

int main(void)
{
	unsigned char *p, buf[64];
	int fd;
	volatile int rc = 0;
	struct sigaction sa = { .sa_sigaction = bus, .sa_flags = SA_SIGINFO };

	fd = open("/tmp/ef2.bin", O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) { perror("open"); return 1; }
	memset(buf, 0xCD, sizeof(buf));
	if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
		perror("write"); return 1;
	}
	if (ftruncate(fd, 8192)) { perror("ftruncate-ext"); return 1; }

	p = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) { perror("mmap"); return 1; }
	if (p[0] != 0xCD) { printf("FAIL pre content\n"); rc = 1; }
	else printf("PASS pre-trunc content\n");

	sigaction(SIGBUS, &sa, NULL);
	if (ftruncate(fd, 0)) { perror("ftruncate"); return 1; }

	got = 0;
	if (sigsetjmp(jb, 1) == 0) {
		volatile unsigned char x = p[0];
		(void)x;
		printf("FAIL no SIGBUS after truncate (read ok)\n");
		rc = 1;
	} else if (got == SIGBUS) {
		printf("PASS SIGBUS after truncate\n");
	} else {
		printf("FAIL wrong signal %d\n", got);
		rc = 1;
	}

	/* The past-EOF zero leg is unaffected by another file: a fresh
	 * 1-page file mapped as 2 pages reads zero on page 1.
	 */
	close(fd);
	unlink("/tmp/ef2.bin");
	fd = open("/tmp/ef3.bin", O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) { perror("open3"); return 1; }
	if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
		perror("write3"); return 1;
	}
	p = mmap(NULL, 2 * 4096, PROT_READ, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) { perror("mmap3"); return 1; }
	if (p[4096] != 0) { printf("FAIL tail not zero\n"); rc = 1; }
	else printf("PASS tail zero (fresh file)\n");

	munmap(p, 2 * 4096);
	close(fd);
	unlink("/tmp/ef3.bin");
	printf(rc ? "EFSMOKE2 FAIL rc=%d\n" : "EFSMOKE2 PASS\n", rc);
	return rc;
}
