// LD_PRELOAD: on-demand stack dump of every thread of a MANA rank (no ptrace needed).
// No thread of its own: a thread made in a lower-half constructor can be mapped over when MANA places the upper half.
// The constructor (lower half only: MANA gives the upper half UH_PRELOAD instead) installs two handlers.
// "kill -50 <pid>" from outside: the receiving thread signals every thread with 51 in turn, and each writes rip, rsp,
// fs and the stack words that point into executable maps to $HANGTRACE_DIR/hang.<host>.<pid>.txt.
// Handlers use only inline syscalls, so they run with whatever TLS (upper or lower half) the thread had.
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define SIG_REQ 50
#define SIG_DUMP 51

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
  return r;
}
static long sys4(long n, long a, long b, long c, long d) {
  long r;
  register long r10 __asm__("r10") = d;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
  return r;
}

static char maps[1 << 20];
static char line[512];
static int len;
static int out_fd = 2;
static volatile int done, busy;
static char path[700];
static unsigned long peek_addr[16], peek_words[16];  // $HANGTRACE_PEEK: "0xADDR:WORDS,..." (int words)
static int npeek;

static void put(const char *s) { while (*s && len < (int)sizeof line - 1) line[len++] = *s++; }
static void hex(unsigned long v) {
  char b[19]; int i = 18; b[i] = 0;
  do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v && i > 2);
  b[--i] = 'x'; b[--i] = '0';
  put(b + i);
}
static void dec(long v) {
  char b[24]; int i = 23; b[i] = 0;
  int neg = v < 0; if (neg) v = -v;
  do { b[--i] = '0' + v % 10; v /= 10; } while (v);
  if (neg) b[--i] = '-';
  put(b + i);
}
static void flush(void) { line[len++] = '\n'; sys3(SYS_write, out_fd, (long)line, len); len = 0; }

static unsigned long parsehex(const char **p) {
  unsigned long v = 0;
  for (;; (*p)++) {
    char c = **p;
    if (c >= '0' && c <= '9') v = v * 16 + c - '0';
    else if (c >= 'a' && c <= 'f') v = v * 16 + c - 'a' + 10;
    else return v;
  }
}

static long slurp(const char *f, char *buf, long cap) {
  long fd = sys3(SYS_open, (long)f, O_RDONLY, 0), n = 0;
  if (fd < 0) { buf[0] = 0; return 0; }
  for (long k; n < cap - 1 && (k = sys3(SYS_read, fd, (long)buf + n, cap - 1 - n)) > 0;) n += k;
  buf[n] = 0;
  sys3(SYS_close, fd, 0, 0);
  return n;
}

static const char *find(unsigned long a, int *exec, unsigned long *rel) {
  for (const char *l = maps; *l;) {
    const char *p = l;
    unsigned long lo = parsehex(&p); p++;
    unsigned long hi = parsehex(&p); p++;
    int x = p[2] == 'x';
    p += 5;
    unsigned long off = parsehex(&p);
    if (a >= lo && a < hi) { *exec = x; *rel = a - lo + off; return l; }
    while (*l && *l != '\n') l++;
    if (*l) l++;
  }
  return NULL;
}

// Prints "what addr = +fileoffset in <path>".
static void where(const char *what, unsigned long a) {
  int x; unsigned long rel;
  const char *l = find(a, &x, &rel);
  put("  "); put(what); put(" "); hex(a);
  if (!l) { put(" unmapped"); flush(); return; }
  put(" = +"); hex(rel); put(" in ");
  const char *e = l; while (*e && *e != '\n') e++;
  const char *s = e; while (s > l && s[-1] != ' ') s--;
  if (s == e) put("[anon]");
  while (s < e && len < (int)sizeof line - 2) line[len++] = *s++;
  flush();
}

static void dump_self(ucontext_t *uc) {
  greg_t *g = uc->uc_mcontext.gregs;
  unsigned long fs = 0;
  sys3(SYS_arch_prctl, 0x1003 /* ARCH_GET_FS */, (long)&fs, 0);
  put("tid "); dec(sys3(SYS_gettid, 0, 0, 0)); flush();
  where("rip", g[REG_RIP]);
  where("rsp", g[REG_RSP]);
  where("fs", fs);
  unsigned long *sp = (unsigned long *)g[REG_RSP];
  int x, hits = 0; unsigned long rel;
  for (int i = 0; i < 2048 && hits < 64; i++) {
    if (!find((unsigned long)(sp + i), &x, &rel)) break;
    if (find(sp[i], &x, &rel) && x) {
      char what[16] = "s[";
      int k = 2;
      if (i >= 1000) what[k++] = '0' + i / 1000;
      if (i >= 100) what[k++] = '0' + i / 100 % 10;
      if (i >= 10) what[k++] = '0' + i / 10 % 10;
      what[k++] = '0' + i % 10;
      what[k++] = ']'; what[k] = 0;
      where(what, sp[i]);
      hits++;
    }
  }
}

static void on_dump(int sig, siginfo_t *si, void *ctx) {
  (void)sig; (void)si;
  dump_self(ctx);
  done = 1;
}

struct ksigaction { void *handler; unsigned long flags; void *restorer; unsigned long mask; };
void hangtrace_restorer(void);
__asm__(".text\n.globl hangtrace_restorer\nhangtrace_restorer:\n  mov $15, %rax\n  syscall\n");

static void install(int sig, void *h) {
  struct ksigaction k = {h, SA_SIGINFO | SA_ONSTACK | SA_RESTART | 0x04000000 /* SA_RESTORER */,
                         (void *)hangtrace_restorer, 0};
  sys4(SYS_rt_sigaction, sig, (long)&k, 0, 8);
}

static void nap(long ns) { struct timespec ts = {0, ns}; sys3(SYS_nanosleep, (long)&ts, 0, 0); }

struct dirent64k { unsigned long ino; long off; unsigned short reclen; unsigned char type; char name[]; };

static void on_req(int sig, siginfo_t *si, void *ctx) {
  (void)sig; (void)si;
  if (__sync_lock_test_and_set(&busy, 1)) return;
  install(SIG_DUMP, (void *)on_dump);
  out_fd = sys3(SYS_open, (long)path, O_CREAT | O_APPEND | O_WRONLY, 0644);
  if (out_fd < 0) { out_fd = 2; busy = 0; return; }
  slurp("/proc/self/maps", maps, sizeof maps);
  long pid = sys3(SYS_getpid, 0, 0, 0), self = sys3(SYS_gettid, 0, 0, 0);
  struct timespec now; sys3(SYS_clock_gettime, CLOCK_REALTIME, (long)&now, 0);
  put("=== pid "); dec(pid); put(" now "); dec(now.tv_sec); put(" by tid "); dec(self); flush();
  for (int k = 0; k < npeek; k++) {
    int x; unsigned long rel;
    put("peek "); hex(peek_addr[k]); put(":");
    if (!find(peek_addr[k], &x, &rel)) { put(" unmapped"); flush(); continue; }
    for (unsigned long w = 0; w < peek_words[k]; w++) { put(" "); dec(((int *)peek_addr[k])[w]); }
    flush();
  }
  static char dents[8192];
  long dfd = sys3(SYS_open, (long)"/proc/self/task", O_RDONLY | O_DIRECTORY, 0);
  long n = dfd < 0 ? 0 : sys3(SYS_getdents64, dfd, (long)dents, sizeof dents);
  for (long o = 0; o < n;) {
    struct dirent64k *e = (struct dirent64k *)(dents + o);
    o += e->reclen;
    long tid = 0;
    for (const char *c = e->name; *c >= '0' && *c <= '9'; c++) tid = tid * 10 + *c - '0';
    if (tid <= 0) continue;
    char f[64], st[512], wc[64];
    len = 0; put("/proc/self/task/"); dec(tid); put("/stat"); line[len] = 0; memcpy(f, line, len + 1);
    len = 0; slurp(f, st, sizeof st);
    len = 0; put("/proc/self/task/"); dec(tid); put("/wchan"); line[len] = 0; memcpy(f, line, len + 1);
    len = 0; slurp(f, wc, sizeof wc);
    for (char *c = st; *c; c++) if (*c == '\n') *c = 0;
    put("thread "); dec(tid); put(" wchan "); put(wc); put(" stat "); put(st); flush();
    if (tid == self) { dump_self(ctx); continue; }
    done = 0;
    sys3(SYS_tgkill, pid, tid, SIG_DUMP);
    for (int i = 0; i < 100 && !done; i++) nap(10000000);
    if (!done) { put("  no response (signal blocked?)"); flush(); }
  }
  if (dfd >= 0) sys3(SYS_close, dfd, 0, 0);
  sys3(SYS_close, out_fd, 0, 0);
  out_fd = 2;
  busy = 0;
}

__attribute__((constructor)) static void start(void) {
  const char *d = getenv("HANGTRACE_DIR");
  if (!d) return;
  char exe[256];
  long m = readlink("/proc/self/exe", exe, sizeof exe - 1);
  exe[m > 0 ? m : 0] = 0;
  if (!strstr(exe, "lower-half")) return;  // MANA ranks only
  struct utsname u; uname(&u);
  snprintf(path, sizeof path, "%s/hang.%s.%d.txt", d, u.nodename, (int)getpid());
  for (const char *q = getenv("HANGTRACE_PEEK"); q && *q && npeek < 16;) {
    char *e;
    peek_addr[npeek] = strtoul(q, &e, 0);
    peek_words[npeek] = *e == ':' ? strtoul(e + 1, &e, 0) : 2;
    if (peek_words[npeek] > 64) peek_words[npeek] = 64;
    npeek++;
    q = *e == ',' ? e + 1 : e + strlen(e);
  }
  install(SIG_DUMP, (void *)on_dump);
  install(SIG_REQ, (void *)on_req);
  if (getenv("HANGTRACE_VERBOSE")) fprintf(stderr, "hangtrace: armed pid %d %s\n", (int)getpid(), exe);
}
