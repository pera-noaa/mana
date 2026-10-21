// mmap() from several threads at once.  Under MANA the upper half's mmap()
// and munmap() go to the lower half's wrapper, which picks addresses from its
// own lists.  Each of 4 threads maps 4 KiB to 512 KiB, records the range in a
// table of live ranges (and fails if it overlaps one), fills it with its own
// pattern, checks the pattern, and unmaps it.  The main thread reports
// progress, and checkpoints come while the threads run.  It calls
// MPI_Barrier in each iteration, so that MPI runs in the lower half while the
// threads are in the wrapper; with -q (mmap_threads_quiet) it does not.

#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "mana_test.h"

#define NTHREADS 4
#define MAXLIVE 64

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t live_lo[MAXLIVE], live_hi[MAXLIVE];
static volatile int stop;
static volatile long rounds[NTHREADS];
static volatile pid_t tids[NTHREADS];
static int idle;  // DEBUG -n: no mmap

static void
add_live(int slot, char *p, size_t len)
{
  pthread_mutex_lock(&lock);
  for (int i = 0; i < MAXLIVE; i++) {
    MT_CHECK(live_hi[i] <= (uintptr_t)p || live_lo[i] >= (uintptr_t)p + len,
             "mmap(NULL, %#zx) = %p overlaps a live mapping %#lx-%#lx", len,
             (void *)p, (unsigned long)live_lo[i], (unsigned long)live_hi[i]);
  }
  live_lo[slot] = (uintptr_t)p;
  live_hi[slot] = (uintptr_t)p + len;
  pthread_mutex_unlock(&lock);
}

static void
remove_live(int slot)
{
  pthread_mutex_lock(&lock);
  live_lo[slot] = live_hi[slot] = 0;
  pthread_mutex_unlock(&lock);
}

static void *
worker(void *arg)
{
  int t = (int)(intptr_t)arg;
  tids[t] = (pid_t)syscall(SYS_gettid);
  if (idle) {
    for (long r = 0; !stop; r++) {
      usleep(1000);
      rounds[t] = r;
    }
    printf("DBG543 thread %d exits\n", t);
    fflush(stdout);
    return NULL;
  }
  uint64_t seed = 0x9e3779b97f4a7c15ULL * (t + 1);
  for (long r = 0; !stop; r++) {
    // Up to MAXLIVE / NTHREADS live mappings per thread.
    char *p[MAXLIVE / NTHREADS];
    size_t len[MAXLIVE / NTHREADS];
    int n = 1 + (int)(r % (MAXLIVE / NTHREADS));
    for (int i = 0; i < n; i++) {
      seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
      len[i] = (size_t)4096 << (seed >> 61);  // 4 KiB to 512 KiB
      p[i] = mmap(NULL, len[i], PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      MT_CHECK(p[i] != MAP_FAILED, "thread %d: mmap of %zu bytes failed", t,
               len[i]);
      add_live(t * (MAXLIVE / NTHREADS) + i, p[i], len[i]);
      for (size_t k = 0; k < len[i]; k += 4096) {
        p[i][k] = (char)(t * 31 + i + k / 4096);
      }
    }
    for (int i = 0; i < n; i++) {
      for (size_t k = 0; k < len[i]; k += 4096) {
        MT_CHECK(p[i][k] == (char)(t * 31 + i + k / 4096),
                 "thread %d: page %zu of %p (%#zx bytes) changed", t,
                 k / 4096, (void *)p[i], len[i]);
      }
      remove_live(t * (MAXLIVE / NTHREADS) + i);
      MT_CHECK(munmap(p[i], len[i]) == 0, "thread %d: munmap failed", t);
    }
    rounds[t] = r;
  }
  printf("DBG543 thread %d exits\n", t);
  fflush(stdout);
  return NULL;
}

int
main(int argc, char **argv)
{
  int quiet = 0;
  for (int a = 1; a < argc; a++) {
    quiet |= strcmp(argv[a], "-q") == 0;
    idle |= strcmp(argv[a], "-n") == 0;
  }
  mt_init(&argc, &argv, idle ? "threads_idle" : quiet ? "mmap_threads_quiet" : "mmap_threads");
  pthread_t th[NTHREADS];
  for (int t = 0; t < NTHREADS; t++) {
    MT_CHECK(pthread_create(&th[t], NULL, worker, (void *)(intptr_t)t) == 0,
             "pthread_create");
  }
  long it;
  for (it = 0; mt_continue(it); it++) {
    usleep(10000);
    if (!quiet) {
      MT_MPI(MPI_Barrier(MPI_COMM_WORLD));
    }
  }
  stop = 1;
  printf("DBG543 rank %d loop done, my tid %ld\n", mt_rank, (long)syscall(SYS_gettid));
  for (int t = 0; t < NTHREADS; t++) {
    printf("DBG543 rank %d joining %d: tid %d, *(th+720)=%d rounds %ld\n", mt_rank, t, tids[t],
           *(pid_t *)((char *)th[t] + 720), rounds[t]);
    fflush(stdout);
    pthread_join(th[t], NULL);
    printf("DBG543 rank %d joined %d\n", mt_rank, t);
    fflush(stdout);
  }
  printf("DBG543 rank %d before mt_finish\n", mt_rank);
  fflush(stdout);
  mt_finish(it);
  return 0;
}
