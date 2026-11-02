/*
** checkpoint-fuzz - run a test file of the mruby test suite and stop it from
** outside at pseudo-random instruction boundaries (test builds only)
**
**   checkpoint-fuzz [options] --assert assert.rb test.rb
**   checkpoint-fuzz [options] --resume IMAGE --state STATE   (internal)
**
** The program is mrbtest's assert.rb, the test file and a call to report,
** compiled as one, in an mrb_state prepared like mrbtest's (t_print,
** _str_match?, Mrbtest, GEMNAME, the gems' C test helpers).
**
** The trigger: the VM's per-instruction hook (code_fetch_hook, from
** MRB_USE_DEBUG_HOOK) counts the fetched instructions. At pseudo-random
** counts (gaps uniform in 1..2*mean from a seed) it calls
** mrb_checkpoint_request_here() before the instruction runs: the program is
** not involved and calls no Checkpoint method. The image is of that very
** boundary when it is a safe point (else of the first safe point after: an
** exception in flight). The image is then restored into a fresh mrb_state,
** the old state is closed, and the program goes on in the new one, with the
** instruction of that boundary; every --xproc-th image instead goes on in
** another process (the --peer binaries in turn, e.g. the 32-bit build),
** which carries on the same schedule.
**
** The clock of mruby-task is the virtual one of the test builds
** (hal-task-virtual): it moves when the scheduler is idle or the root
** sleeps, and with --quantum N also once every N instructions (counted like
** the trigger points, across states and processes), which ends timeslices.
**
** Options:
**   --seed N        seed of the schedule (default 1)
**   --mean N        mean gap between trigger points, in instructions (1000)
**   --at A,B,...    trigger exactly at these instruction counts (replaces the
**                   random schedule; to reproduce one point)
**   --limit K       at most K trigger points
**   --ref           no trigger: the reference run (counts the instructions)
**   --xproc N       every N-th written image goes on in another process
**   --peer BIN      a binary for --xproc (may be given more than once)
**   --gem NAME      GEMNAME of the test ("mruby-test" for the core tests)
**   --log FILE      append one line per trigger point (and the end) to FILE
**   --images DIR    keep every written image as DIR/<pid>-<k>.ckpt
**   --keep-dead     write dead registers as they are (no liveness pass)
**   --tmp DIR       where images for another process go (default /tmp)
**   --suspend FILE  write the first image to FILE and end (for another VM)
**   --as-task       run the program as a Task (mrb_create_task + mrb_task_run),
**                   the way Family mruby starts an application
**   --quantum N     one tick of mruby-task's clock every N instructions
**   --keep-timeslice  the restored running Task keeps what was left of its
**                   timeslice (mrb_checkpoint_keep_timeslice)
**
** Log lines:  P <k> <icount> <outcome> [detail]   one per trigger point
**             (icount: where the request was made; --at reproduces it)
**             outcome: ok <bytes> <dead> <inproc|xproc:BIN> written-at <icount>,
**                      req <fiber|c_boundary|tasks|pending|exception|not_ready>,
**                      refused <reason>, restore_failed <reason>
**             E <icount> <points> <status>          the end of the program
**
** Exit status: 0 normal end, 1 uncaught exception, 2 usage, 3 restore failed.
*/

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <mruby.h>
#include <mruby/compile.h>
#include <mruby/error.h>
#include <mruby/array.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/irep.h>
#include <mruby/checkpoint.h>
#include <mruby/proc.h>

void checkpoint_fuzz_test_env(mrb_state *mrb, const char *gemname);
/* mruby-task (include/task.h) */
MRB_API mrb_value mrb_create_task(mrb_state *mrb, struct RProc *proc, mrb_value name, mrb_value priority, mrb_value top_self);
MRB_API mrb_value mrb_task_run(mrb_state *mrb);
MRB_API void mrb_tick(mrb_state *mrb);
#include "task.h"
#include <mruby/gc.h>
static struct mrb_context *task_context(struct mrb_task *t) { return &t->c; }

/* ---- options and the schedule ---- */

static uint64_t opt_seed = 1;
static uint64_t opt_mean = 1000;
static uint64_t *opt_at = NULL; static size_t opt_nat = 0; static const char *opt_at_str = NULL;
static uint64_t opt_limit = UINT64_MAX;
static int opt_ref = 0;
static uint64_t opt_xproc = 0;
static const char *opt_peers[8]; static int opt_npeers = 0;
static const char *opt_gem = "mruby-test";
static const char *opt_log = NULL;
static const char *opt_images = NULL;
static int opt_keep_dead = 0;
static const char *opt_suspend = NULL;  /* write the first image there and end the process */
static const char *opt_tmp = "/tmp";
static int opt_as_task = 0;
static uint64_t opt_quantum = 0;
static int opt_keep_timeslice = 0;
static char **saved_argv;

static uint64_t rng;          /* xorshift64* state */
static uint64_t icount;       /* instructions fetched so far (all states, all processes) */
static uint64_t next_at;      /* the next trigger point */
static uint64_t npoints;      /* trigger points so far */
static uint64_t nwritten;     /* images written so far */
static size_t at_idx;         /* next entry of opt_at */
static int peer_turn;         /* which peer gets the next --xproc image */

static uint64_t
rng_next(void)
{
  rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
  return rng * 2685821657736338717ULL;
}

static void
schedule_next(void)
{
  if (opt_ref || npoints >= opt_limit) { next_at = UINT64_MAX; return; }
  if (opt_at) {
    while (at_idx < opt_nat && opt_at[at_idx] <= icount) at_idx++;
    next_at = at_idx < opt_nat ? opt_at[at_idx++] : UINT64_MAX;
    return;
  }
  next_at = icount + 1 + rng_next() % (2 * opt_mean);
}

static FILE *log_fp;

static void
logline(const char *fmt, ...)
{
  if (!log_fp) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(log_fp, fmt, ap);
  va_end(ap);
  fflush(log_fp);
}

static void
sanitize(char *s)
{
  for (; *s; s++) if (*s == '\n' || *s == '\r') *s = ' ';
}

/* ---- the hop ---- */

static jmp_buf hop_jmp;
static int hop_ready = 0;       /* an image is waiting: leave the old state */
static uint8_t *hop_buf = NULL;
static size_t hop_len = 0;
static uint64_t hop_point;      /* the point that wrote it */
static uint64_t hop_req_at;     /* the instruction count of its request (what --at takes) */

static void
fetch_hook(mrb_state *mrb, const struct mrb_irep *irep, const mrb_code *pc, mrb_value *regs)
{
  (void)irep; (void)pc; (void)regs;
  if (hop_ready) {
    /* the image is of this boundary (or was written at the safe point just
       passed): nothing of this instruction has run; it runs first, and is
       counted, in the new state */
    longjmp(hop_jmp, 1);
  }
  if (icount + 1 == next_at) {
    /* point k is the boundary before instruction k */
    npoints++;
    hop_point = npoints;
    hop_req_at = icount + 1;
    enum mrb_checkpoint_req r = mrb_checkpoint_request_here(mrb);
    if (r != MRB_CHECKPOINT_REQ_OK) {
      static const char *why[] = { "ok", "not_ready", "fiber", "c_boundary", "tasks", "pending", "exception" };
      logline("P %llu %llu req %s\n", (unsigned long long)npoints, (unsigned long long)(icount + 1),
              (unsigned)r < sizeof(why) / sizeof(why[0]) ? why[r] : "?");
    }
    icount++;
    schedule_next();
    icount--;
    if (hop_ready) longjmp(hop_jmp, 1);
  }
  icount++;
  if (opt_quantum && icount % opt_quantum == 0) mrb_tick(mrb);
}

static void
refusal_handler(mrb_state *mrb, const char *reason, void *ud)
{
  (void)mrb; (void)ud;
  char buf[600];
  snprintf(buf, sizeof(buf), "%s", reason);
  sanitize(buf);
  logline("P %llu %llu refused %s\n", (unsigned long long)hop_point, (unsigned long long)hop_req_at, buf);
}

static mrb_state *restore_state(const uint8_t *buf, size_t len);
static void abandon_state(mrb_state *mrb);
static mrb_state *hop_new = NULL;   /* the restored state the program goes on in */
static int hop_is_xproc = 0;

/* an image was written: restore it now, while the old state can still go
   on if the image cannot be restored (that is counted as a refusal); the
   switch itself happens at the next instruction fetch */
static void
image_handler(mrb_state *mrb, const uint8_t *buf, size_t len, const char *path,
              enum mrb_checkpoint_kind kind, void *ud)
{
  (void)path; (void)ud;
  if (kind != MRB_CHECKPOINT_EXTERNAL) return;
  nwritten++;
  uint32_t dead = mrb_checkpoint_last_dead(mrb);
  if (opt_images) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/%d-%llu.ckpt", opt_images, (int)getpid(), (unsigned long long)hop_point);
    FILE *fp = fopen(p, "wb");
    if (fp) { fwrite(buf, 1, len, fp); fclose(fp); }
  }
  if (opt_suspend) {
    /* like Checkpoint.suspend: the image goes to another VM (SabiRuby), this
       process ends here, its output so far is what came before the point */
    FILE *fp = fopen(opt_suspend, "wb");
    if (!fp || fwrite(buf, 1, len, fp) != len || fclose(fp) != 0) { fprintf(stderr, "checkpoint-fuzz: cannot write %s\n", opt_suspend); exit(2); }
    logline("P %llu %llu ok %zu %u suspend written-at %llu\n", (unsigned long long)hop_point,
            (unsigned long long)hop_req_at, len, mrb_checkpoint_last_dead(mrb), (unsigned long long)icount + 1);
    fflush(stdout);
    _exit(0);
  }
  int xproc = opt_xproc && opt_npeers > 0 && nwritten % opt_xproc == 0;
  /* restore it here first; for another process too, where a failure could
     no longer fall back to the old state */
  hop_new = restore_state(buf, len);
  if (!hop_new) return;    /* logged as restore_failed; the old state goes on */
  if (xproc) {
    abandon_state(hop_new);
    hop_new = NULL;
    free(hop_buf);
    hop_buf = (uint8_t*)malloc(len ? len : 1);
    memcpy(hop_buf, buf, len);
    hop_len = len;
  }
  hop_is_xproc = xproc;
  hop_ready = 1;
  logline("P %llu %llu ok %zu %u %s%s written-at %llu\n", (unsigned long long)hop_point,
          (unsigned long long)hop_req_at, len, dead, xproc ? "xproc:" : "inproc",
          xproc ? opt_peers[peer_turn % opt_npeers] : "", (unsigned long long)icount + 1);
}

/* ---- states ---- */

static uint16_t open_atexit;   /* atexit entries mrb_open made (the gems' finals) */

/* close a state the program has left: its exit handlers registered after
   mrb_open (the test helpers', the program's) belong to the program, which
   goes on elsewhere, so they do not run here */
static mrb_ccontext *prog_cxt;   /* the compile context of the program, while it runs */
static mrb_state *prog_cxt_mrb;

/* mruby-task with mruby-fiber (f56d44ecc): Fiber.current inside a Task makes
   a Fiber whose context is the one embedded in the Task's struct, and
   mrb_close frees a Fiber's context: it frees an address inside the Task
   (seen with Enumerator#next in a Task, at the end of any such run). Such a
   Fiber is let go of its context before the state is closed. This tool
   closes states in the middle of the program (each hop), where the run
   without checkpoints closes only at the end. */
static int
detach_cb(mrb_state *mrb, struct RBasic *obj, void *ud)
{
  (void)ud;
  if (obj->tt != MRB_TT_FIBER) return MRB_EACH_OBJ_OK;
  struct RFiber *f = (struct RFiber*)obj;
  for (int q = 0; f->cxt && q < 4; q++) {
    for (struct mrb_task *t = mrb->task.queues[q]; t; t = t->next) {
      if (f->cxt == task_context(t)) { f->cxt = NULL; break; }
    }
  }
  return MRB_EACH_OBJ_OK;
}

static void
close_state(mrb_state *mrb)
{
  mrb_objspace_each_objects(mrb, detach_cb, NULL);
  mrb_close(mrb);
}

static void
abandon_state(mrb_state *mrb)
{
  if (prog_cxt && prog_cxt_mrb == mrb) {   /* left by the longjmp out of the program */
    mrb_ccontext_free(mrb, prog_cxt);
    prog_cxt = NULL;
  }
  if (mrb->atexit_stack_len > open_atexit) mrb->atexit_stack_len = open_atexit;
  close_state(mrb);
}

static mrb_state*
open_state(void)
{
  mrb_state *mrb = mrb_open();
  if (!mrb) { fprintf(stderr, "checkpoint-fuzz: mrb_open failed\n"); exit(2); }
  open_atexit = mrb->atexit_stack_len;
  mrb_define_global_const(mrb, "ARGV", mrb_ary_new(mrb));
  checkpoint_fuzz_test_env(mrb, opt_gem);
  mrb_full_gc(mrb);
  mrb_checkpoint_baseline(mrb);
  mrb_checkpoint_set_handler(mrb, image_handler, NULL);
  mrb_checkpoint_set_refusal_handler(mrb, refusal_handler, NULL);
  mrb_checkpoint_keep_dead_registers(mrb, opt_keep_dead);
  mrb_checkpoint_keep_timeslice(mrb, opt_keep_timeslice);
  return mrb;
}

static int
finish(mrb_state *mrb, mrb_value r, mrb_bool error)
{
  int status = 0;
  if (error) {
    mrb->exc = mrb_obj_ptr(r);
    mrb_print_error(mrb);
    status = 1;
  }
  else if (mrb->exc) {
    mrb_print_error(mrb);
    status = 1;
  }
  else {
    /* --as-task: the program's Task ended with an exception */
    mrb_value t = mrb_checkpoint_host_task(mrb);
    if (!mrb_nil_p(t)) {
      mrb_value v = mrb_funcall(mrb, t, "value", 0);
      if (mrb_obj_is_kind_of(mrb, v, mrb->eException_class)) {
        mrb->exc = mrb_obj_ptr(v);
        mrb_print_error(mrb);
        mrb->exc = NULL;
        status = 1;
      }
    }
  }
  fflush(stdout);
  return status;
}

typedef struct { const char *src; size_t len; const char *name; } prog;

static mrb_value
run_program(mrb_state *mrb, void *ud)
{
  prog *p = (prog*)ud;
  mrb_ccontext *c = mrb_ccontext_new(mrb);
  prog_cxt = c; prog_cxt_mrb = mrb;
  mrb_ccontext_filename(mrb, c, p->name);
  if (opt_as_task) c->no_exec = TRUE;
  mrb_value v = mrb_load_nstring_cxt(mrb, p->src, p->len, c);
  mrb_ccontext_free(mrb, c);
  prog_cxt = NULL;
  if (opt_as_task && !mrb->exc) {
    /* compiled only: the scheduler runs it as a Task */
    mrb_value task = mrb_create_task(mrb, mrb_proc_ptr(v), mrb_str_new_cstr(mrb, "test"),
                                     mrb_nil_value(), mrb_nil_value());
    mrb_checkpoint_set_host_task(mrb, task);
    v = mrb_task_run(mrb);
  }
  if (mrb->exc) {
    mrb_value e = mrb_obj_value(mrb->exc);
    mrb->exc = NULL;
    mrb_exc_raise(mrb, e);
  }
  return v;
}

static mrb_value
continue_program(mrb_state *mrb, void *ud)
{
  (void)ud;
  mrb_value v = mrb_checkpoint_continue(mrb);
  if (mrb->exc) {
    mrb_value e = mrb_obj_value(mrb->exc);
    mrb->exc = NULL;
    mrb_exc_raise(mrb, e);
  }
  return v;
}

typedef struct { const uint8_t *buf; size_t len; } image;

static mrb_value
restore_body(mrb_state *mrb, void *ud)
{
  image *im = (image*)ud;
  mrb_checkpoint_restore(mrb, im->buf, im->len);
  return mrb_nil_value();
}

/* a fresh state from the image; NULL (logged) when it cannot be restored */
static mrb_state*
restore_state(const uint8_t *buf, size_t len)
{
  mrb_state *mrb = open_state();
  image im = { buf, len };
  mrb_bool error;
  mrb_value r = mrb_protect_error(mrb, restore_body, &im, &error);
  if (error) {
    mrb_value s = mrb_inspect(mrb, r);
    char msg[600];
    snprintf(msg, sizeof(msg), "%.*s", (int)RSTRING_LEN(s), RSTRING_PTR(s));
    sanitize(msg);
    logline("P %llu %llu restore_failed %s\n", (unsigned long long)hop_point,
            (unsigned long long)hop_req_at, msg);
    fprintf(stderr, "checkpoint-fuzz: restore failed at point %llu: %s\n",
            (unsigned long long)hop_point, msg);
    close_state(mrb);
    return NULL;
  }
  return mrb;
}

/* hand the image to another process, which goes on; this one waits and
   exits with its status */
static void
hop_xproc(void)
{
  const char *peer = opt_peers[peer_turn++ % opt_npeers];
  char img[1024], state[256];
  snprintf(img, sizeof(img), "%s/checkpoint-fuzz-%d-%llu.ckpt", opt_tmp, (int)getpid(),
           (unsigned long long)hop_point);
  FILE *fp = fopen(img, "wb");
  if (!fp || fwrite(hop_buf, 1, hop_len, fp) != hop_len || fclose(fp) != 0) {
    fprintf(stderr, "checkpoint-fuzz: cannot write %s\n", img);
    exit(2);
  }
  snprintf(state, sizeof(state), "%llu,%llu,%llu,%llu,%llu,%llu,%d",
           (unsigned long long)rng, (unsigned long long)icount, (unsigned long long)next_at,
           (unsigned long long)npoints, (unsigned long long)nwritten,
           (unsigned long long)at_idx, peer_turn);
  /* the same options, then the image and the schedule */
  const char *args[64]; int n = 0;
  char num[6][32];
  args[n++] = (char*)peer;
  snprintf(num[0], 32, "%llu", (unsigned long long)opt_seed); args[n++] = "--seed"; args[n++] = num[0];
  snprintf(num[1], 32, "%llu", (unsigned long long)opt_mean); args[n++] = "--mean"; args[n++] = num[1];
  if (opt_limit != UINT64_MAX) {
    snprintf(num[2], 32, "%llu", (unsigned long long)opt_limit); args[n++] = "--limit"; args[n++] = num[2];
  }
  if (opt_at_str) { args[n++] = "--at"; args[n++] = (char*)opt_at_str; }
  snprintf(num[3], 32, "%llu", (unsigned long long)opt_xproc); args[n++] = "--xproc"; args[n++] = num[3];
  for (int i = 0; i < opt_npeers; i++) { args[n++] = "--peer"; args[n++] = (char*)opt_peers[i]; }
  args[n++] = "--gem"; args[n++] = (char*)opt_gem;
  if (opt_log) { args[n++] = "--log"; args[n++] = (char*)opt_log; }
  if (opt_images) { args[n++] = "--images"; args[n++] = (char*)opt_images; }
  if (opt_keep_dead) args[n++] = "--keep-dead";
  if (opt_as_task) args[n++] = "--as-task";
  if (opt_keep_timeslice) args[n++] = "--keep-timeslice";
  if (opt_quantum) {
    snprintf(num[4], 32, "%llu", (unsigned long long)opt_quantum); args[n++] = "--quantum"; args[n++] = num[4];
  }
  args[n++] = "--tmp"; args[n++] = (char*)opt_tmp;
  args[n++] = "--resume"; args[n++] = img;
  args[n++] = "--state"; args[n++] = state;
  args[n] = NULL;
  fflush(stdout); fflush(stderr);
  if (log_fp) fflush(log_fp);
  pid_t pid = fork();
  if (pid < 0) { perror("fork"); exit(2); }
  if (pid == 0) {
    execv(peer, (char *const *)args);
    perror(peer);
    _exit(2);
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0) {}
  unlink(img);
  /* the same status as if this process had gone on (a signal: 128 + it) */
  _exit(WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 2);
}

/* run the program (or the restored state) to the end, hopping as the
   images come */
static int
drive(mrb_state *mrb_in, prog *p)
{
  mrb_state *volatile mrb = mrb_in;
  volatile int first = p != NULL;
  for (;;) {
    mrb->code_fetch_hook = fetch_hook;   /* the reference run counts too */
    if (setjmp(hop_jmp) == 0) {
      mrb_bool error;
      mrb_value r = first ? mrb_protect_error(mrb, run_program, p, &error)
                          : mrb_protect_error(mrb, continue_program, NULL, &error);
      int status = finish(mrb, r, error);
      logline("E %llu %llu %d\n", (unsigned long long)icount, (unsigned long long)npoints, status);
      close_state(mrb);
      return status;
    }
    /* back from the old state, which is dropped */
    hop_ready = 0;
    mrb->code_fetch_hook = NULL;
    abandon_state(mrb);
    first = 0;
    if (hop_is_xproc) hop_xproc();   /* does not return */
    mrb = hop_new;
    hop_new = NULL;
  }
}

static char*
read_file(const char *path, size_t *len)
{
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  fseek(fp, 0, SEEK_END);
  long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  char *buf = (char*)malloc((size_t)n + 1);
  if (fread(buf, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(buf); return NULL; }
  fclose(fp);
  buf[n] = 0;
  *len = (size_t)n;
  return buf;
}

static int
usage(void)
{
  fprintf(stderr, "usage: checkpoint-fuzz [options] --assert assert.rb test.rb\n"
                  "       checkpoint-fuzz [options] --resume IMAGE --state STATE\n");
  return 2;
}

static void
parse_at(const char *s)
{
  size_t cap = 16;
  opt_at = (uint64_t*)malloc(sizeof(uint64_t) * cap);
  while (*s) {
    char *e;
    uint64_t v = strtoull(s, &e, 10);
    if (e == s) break;
    if (opt_nat == cap) { cap *= 2; opt_at = (uint64_t*)realloc(opt_at, sizeof(uint64_t) * cap); }
    opt_at[opt_nat++] = v;
    s = *e == ',' ? e + 1 : e;
  }
}

int
main(int argc, char **argv)
{
  const char *assert_rb = NULL, *file = NULL, *resume = NULL, *state = NULL;
  saved_argv = argv;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
#define ARG() (i + 1 < argc ? argv[++i] : (exit(usage()), (char*)NULL))
    if (strcmp(a, "--seed") == 0) opt_seed = strtoull(ARG(), NULL, 10);
    else if (strcmp(a, "--mean") == 0) opt_mean = strtoull(ARG(), NULL, 10);
    else if (strcmp(a, "--at") == 0) { opt_at_str = ARG(); parse_at(opt_at_str); }
    else if (strcmp(a, "--limit") == 0) opt_limit = strtoull(ARG(), NULL, 10);
    else if (strcmp(a, "--ref") == 0) opt_ref = 1;
    else if (strcmp(a, "--xproc") == 0) opt_xproc = strtoull(ARG(), NULL, 10);
    else if (strcmp(a, "--peer") == 0) { if (opt_npeers < 8) opt_peers[opt_npeers++] = ARG(); else ARG(); }
    else if (strcmp(a, "--gem") == 0) opt_gem = ARG();
    else if (strcmp(a, "--log") == 0) opt_log = ARG();
    else if (strcmp(a, "--images") == 0) opt_images = ARG();
    else if (strcmp(a, "--keep-dead") == 0) opt_keep_dead = 1;
    else if (strcmp(a, "--suspend") == 0) opt_suspend = ARG();
    else if (strcmp(a, "--tmp") == 0) opt_tmp = ARG();
    else if (strcmp(a, "--as-task") == 0) opt_as_task = 1;
    else if (strcmp(a, "--keep-timeslice") == 0) opt_keep_timeslice = 1;
    else if (strcmp(a, "--quantum") == 0) opt_quantum = strtoull(ARG(), NULL, 10);
    else if (strcmp(a, "--assert") == 0) assert_rb = ARG();
    else if (strcmp(a, "--resume") == 0) resume = ARG();
    else if (strcmp(a, "--state") == 0) state = ARG();
    else if (a[0] == '-') return usage();
    else if (!file) file = a;
    else return usage();
#undef ARG
  }
  if (opt_mean == 0) opt_mean = 1;
  if (opt_log) {
    log_fp = fopen(opt_log, "a");
    if (!log_fp) { fprintf(stderr, "checkpoint-fuzz: cannot open %s\n", opt_log); return 2; }
    /* the tool's own descriptor stays out of the processes it starts (the
       program there may name descriptors by number) */
    fcntl(fileno(log_fp), F_SETFD, FD_CLOEXEC);
  }

  if (resume) {
    unsigned long long s[6]; int turn = 0;
    if (!state || sscanf(state, "%llu,%llu,%llu,%llu,%llu,%llu,%d",
                         &s[0], &s[1], &s[2], &s[3], &s[4], &s[5], &turn) != 7) return usage();
    rng = s[0]; icount = s[1]; next_at = s[2]; npoints = s[3]; nwritten = s[4];
    at_idx = (size_t)s[5]; peer_turn = turn;
    hop_point = npoints;
    size_t len;
    char *buf = read_file(resume, &len);
    if (!buf) { fprintf(stderr, "checkpoint-fuzz: cannot read %s\n", resume); return 2; }
    mrb_state *mrb = restore_state((const uint8_t*)buf, len);
    free(buf);
    if (!mrb) return 3;
    return drive(mrb, NULL);
  }

  if (!assert_rb || !file) return usage();
  rng = opt_seed * 0x9e3779b97f4a7c15ULL + 0x2545f4914f6cdd1dULL;
  if (rng == 0) rng = 1;
  icount = 0;
  schedule_next();

  size_t la, lt;
  char *sa = read_file(assert_rb, &la);
  char *st = read_file(file, &lt);
  if (!sa || !st) { fprintf(stderr, "checkpoint-fuzz: cannot read the program\n"); return 2; }
  static const char trailer[] = "\nreport\n";
  size_t len = la + 1 + lt + sizeof(trailer) - 1;
  char *src = (char*)malloc(len + 1);
  memcpy(src, sa, la);
  src[la] = '\n';
  memcpy(src + la + 1, st, lt);
  memcpy(src + la + 1 + lt, trailer, sizeof(trailer));
  free(sa); free(st);

  prog p = { src, len, file };
  mrb_state *mrb = open_state();
  int status = drive(mrb, &p);
  free(src);
  return status;
}
