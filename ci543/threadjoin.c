// A thread runs until the file "stop" exists; main then joins it, giving up
// after 20 s.  Checkpoint and restart it in between.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static volatile int stop;

static void *
worker(void *arg)
{
  while (!stop) {
    usleep(1000);
  }
  return arg;
}

int
main(void)
{
  pthread_t th;
  pthread_create(&th, NULL, worker, NULL);
  while (access("stop", F_OK) != 0) {
    usleep(10000);
  }
  stop = 1;
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  t.tv_sec += 20;
  int rc = pthread_timedjoin_np(th, NULL, &t);
  printf(rc == 0 ? "threadjoin: PASS\n" : "threadjoin: JOIN TIMED OUT\n");
  return rc != 0;
}
