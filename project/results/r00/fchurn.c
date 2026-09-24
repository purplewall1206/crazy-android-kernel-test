/* fchurn.c — multithreaded mmap/touch/munmap churn (perfetto pipeline smoke) */
#include <pthread.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define SZ (4UL<<20)
static void *w(void *arg){
  long iters=(long)arg;
  for(long i=0;i<iters;i++){
    char *p=mmap(0,SZ,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED) continue;
    memset(p,1,SZ);
    munmap(p,SZ);
  }
  return 0;
}
int main(int argc,char**argv){
  int n=argc>1?atoi(argv[1]):4;
  long iters=argc>2?atol(argv[2]):2000;
  pthread_t t[16];
  fprintf(stderr,"fchurn: %d threads x %ld iters x 4MB\n",n,iters);
  for(int i=0;i<n;i++) pthread_create(&t[i],0,w,(void*)iters);
  for(int i=0;i<n;i++) pthread_join(t[i],0);
  return 0;
}
