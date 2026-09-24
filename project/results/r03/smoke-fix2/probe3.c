#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#ifndef PR_CORTEN_ARENA
#define PR_CORTEN_ARENA 79
#endif
#define DECLARE 0
#define RELEASE_OP 1
#define ARENA_LEN (64UL << 20)
#define CHUNK_LEN (2UL << 20)
#define PAGES (CHUNK_LEN / 4096UL)
static long apr(unsigned long op, unsigned long a, unsigned long l){
	return syscall(SYS_prctl, PR_CORTEN_ARENA, op, a, l, 0UL); }
static uint64_t magic(int tid, uint64_t pg){ return 0x5a5a000000000000ULL ^ ((uint64_t)tid<<32) ^ pg; }
int main(void){
	unsigned long base = 0x10000000UL;
	char *arena = mmap((void*)base, ARENA_LEN, PROT_READ|PROT_WRITE,
		MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE, -1, 0);
	if (arena==MAP_FAILED){perror("mmap");return 3;}
	if (apr(DECLARE,(unsigned long)arena,ARENA_LEN)){perror("DECLARE");return 3;}
	int fails=0, zfails=0; unsigned long seed=12345;
	for (int c=0;c<300;c++){
		seed = seed*6364136223846793005ULL + 1442695040888963407ULL;
		size_t len = 16384UL << ((seed>>33) & 7);
		unsigned long off = ((seed>>20) % ((ARENA_LEN-len)/16384+1)) * 16384UL;
		unsigned long n = len/4096;
		char *p = mmap(arena+off, len, PROT_READ|PROT_WRITE,
			MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
		if (p==MAP_FAILED){perror("mmap");return 3;}
		for (unsigned long i=0;i<n;i++)
			*(volatile uint64_t*)(p+i*4096) = magic(1, off/4096+i);
		for (unsigned long i=0;i<n;i++){
			uint64_t got=*(volatile uint64_t*)(p+i*4096);
			if (got != magic(1, off/4096+i) && fails<5)
				fprintf(stderr,"INPLACE c=%d off=%lx i=%lx got=%016lx\n",c,off,i,got);
			fails += got != magic(1, off/4096+i);
		}
		if (munmap(arena+off, len)){perror("munmap");return 3;}
		/* zerocheck: remap, read, expect zero */
		p = mmap(arena+off, len, PROT_READ|PROT_WRITE,
			MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
		if (p==MAP_FAILED){perror("remap");return 3;}
		for (unsigned long i=0;i<n;i++){
			uint64_t got=*(volatile uint64_t*)(p+i*4096);
			if (got != 0 && zfails<5)
				fprintf(stderr,"ZFAIL c=%d off=%lx i=%lx got=%016lx\n",c,off,i,got);
			zfails += got != 0;
		}
		munmap(arena+off, len);
	}
	fprintf(stderr,"done fails=%d zfails=%d\n", fails, zfails);
	if (apr(RELEASE_OP,(unsigned long)arena,ARENA_LEN)) fprintf(stderr,"RELEASE hang/fail\n");
	fprintf(stderr,"RELEASE ok\n");
	return 0;
}
