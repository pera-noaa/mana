#ifndef SEQ_NUM_H
#define SEQ_NUM_H

#include <mpi.h>
#include <pthread.h>
#include <unordered_map>

typedef enum _phase_t {
  IN_CS,
  IS_READY
} phase_t;

// Global communicator for MANA internal use
extern MPI_Comm g_world_comm;

// The main functions of the sequence number algorithm for MPI collectives
void commit_begin(MPI_Comm comm);
void commit_finish(MPI_Comm comm);

void drain_mpi_collective();
void complete_pending_nonblocking_collectives();
int test_pending_nonblocking_collectives();
void share_seq_nums();
int check_seq_nums();
void print_seq_nums();
void seq_num_init();
void seq_num_reset();
void seq_num_destroy();

extern std::unordered_map<unsigned int, unsigned long> seq_num;
extern std::unordered_map<unsigned int, unsigned long> target;
extern std::unordered_map<MPI_Comm, unsigned int> ggid_table;
typedef std::pair<unsigned int, unsigned long> comm_seq_pair_t;

// DEBUG (CI only): checkpoint progress to stderr with MANA_CKPTDBG set.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
extern int g_world_rank;
#define CKPTDBG(...)                                                       \
  do {                                                                     \
    if (getenv("MANA_CKPTDBG")) {                                          \
      struct timespec ts_;                                                 \
      clock_gettime(CLOCK_REALTIME, &ts_);                                 \
      char b_[512];                                                        \
      int n_ = snprintf(b_, sizeof(b_), "CKPTDBG %ld.%03ld r%d ",          \
                        (long)ts_.tv_sec, ts_.tv_nsec / 1000000,           \
                        g_world_rank);                                     \
      snprintf(b_ + n_, sizeof(b_) - n_, __VA_ARGS__);                     \
      fprintf(stderr, "%s\n", b_);                                        \
    }                                                                      \
  } while (0)

#endif // ifndef SEQ_NUM_H
