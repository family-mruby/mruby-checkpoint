/*
** checkpoint.c - mruby-checkpoint gem: Checkpoint.save / Checkpoint.suspend
** and the safe point hook
**
** save(path) and suspend(path) only raise a request: they mark the state
** and set mrb->task.switching, the flag the VM already tests at every
** instruction boundary for task switches. The VM then calls
** mrb_vm_safepoint_hook (patches/0001-vm-safepoint-hook.patch) at the next
** boundary where a task switch would be allowed (no C function between the
** root frame and the current one, no exception in flight). The request is
** only accepted when there is no C function between the root frame and the
** caller, so that boundary is always the very next instruction: the
** checkpoint lands right after the call, whose value is already in its
** register. Inside a block called from C (Array#sort!, Array.new { }, ...)
** the request is refused on the spot with Checkpoint::Error.
**
** The call's value tells the two sides apart: the writing side gets :saved,
** the image holds :resumed in the same register.
**
** task.switching is the scheduler's flag too (a Task's timeslice is over,
** it sleeps, a Task it woke goes first), and a plain bool: a request that
** sets it cannot tell later whether the scheduler set it as well. So the
** request notes how the scheduler stood (mark_switch), and at the safe point
** the change says whether the scheduler asked for a switch meanwhile
** (sched_switch): the running Task no longer runs, its timeslice is over,
** a waiting Task woke, a Task of higher priority became ready. Then the flag
** is left set (the VM returns to the scheduler right after the hook) and the
** image says the slice ends there; otherwise it is cleared. The one switch
** that leaves no trace is Task.pass called by the very instruction between
** an external request and its safe point: that pass is lost (the Task runs
** on until its timeslice is over). The test trigger asks at the boundary
** where it stands (mrb_checkpoint_request_here), which has no such window.
*/

#include <stdio.h>
#include <stdlib.h>
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/string.h>
#include <mruby/data.h>
#include <mruby/variable.h>
#include <mruby/proc.h>
#include <mruby/throw.h>
#include <mruby/presym.h>
#include "checkpoint_internal.h"
#include "task.h"

extern void (*mrb_vm_safepoint_hook)(mrb_state *mrb);


/* ---- per-state side table ---- */

#define CHECKPOINT_MAX_STATES 8
static checkpoint_state *states[CHECKPOINT_MAX_STATES];

checkpoint_state*
checkpoint_get(mrb_state *mrb)
{
  for (int i = 0; i < CHECKPOINT_MAX_STATES; i++) {
    if (states[i] && states[i]->mrb == mrb) return states[i];
  }
  return NULL;
}

static checkpoint_state*
checkpoint_new(mrb_state *mrb)
{
  for (int i = 0; i < CHECKPOINT_MAX_STATES; i++) {
    if (!states[i]) {
      checkpoint_state *g = (checkpoint_state*)calloc(1, sizeof(checkpoint_state));
      g->mrb = mrb;
      states[i] = g;
      return g;
    }
  }
  return NULL;
}

static void
checkpoint_delete(mrb_state *mrb)
{
  for (int i = 0; i < CHECKPOINT_MAX_STATES; i++) {
    if (states[i] && states[i]->mrb == mrb) {
      checkpoint_state *g = states[i];
      checkpoint_perm_free(mrb, g->perm);
      free(g->pending_path);
      free(g->output);
      for (int k = 0; k < g->ndata; k++) free(g->data[k].name);
      free(g->data);
      for (int k = 0; k < g->nfuncs; k++) free(g->fnames[k]);
      free(g->fnames); free(g->funcs);
      free(g->snap_wait); free(g->snap_reason); free(g->snap_ready);
      free(g);
      states[i] = NULL;
    }
  }
}

static char*
dupstr(const char *s)
{
  if (!s) return NULL;
  size_t n = strlen(s);
  char *d = (char*)malloc(n + 1);
  memcpy(d, s, n + 1);
  return d;
}

/* ---- C API ---- */

MRB_API void
mrb_checkpoint_baseline(mrb_state *mrb)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (!g) return;
  checkpoint_perm_free(mrb, g->perm);
  g->perm = NULL;
  checkpoint_bundled_register(mrb);
  checkpoint_task_register(mrb);
  g->perm = checkpoint_perm_build(mrb, g);
}

MRB_API void
mrb_checkpoint_each_name(mrb_state *mrb, mrb_checkpoint_name_fn fn, void *ud)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (!g || !g->perm) return;
  uint32_t n = checkpoint_perm_count(g->perm);
  for (uint32_t i = 0; i < n; i++) {
    int is_func = 0;
    const char *name = checkpoint_perm_name_at(g->perm, i, &is_func);
    fn(name, is_func, ud);
  }
}

MRB_API void
mrb_checkpoint_set_mode(mrb_state *mrb, enum mrb_checkpoint_mode mode)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (g) g->mode = mode;
}

MRB_API void
mrb_checkpoint_set_output(mrb_state *mrb, const char *path)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (!g) return;
  free(g->output);
  g->output = dupstr(path);
}

MRB_API void
mrb_checkpoint_set_handler(mrb_state *mrb, mrb_checkpoint_handler fn, void *ud)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (!g) return;
  g->handler = fn;
  g->handler_ud = ud;
}

MRB_API void
mrb_checkpoint_register_data(mrb_state *mrb, const mrb_data_type *type, const char *name,
                        mrb_checkpoint_data_dump dump, mrb_checkpoint_data_restore restore)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (!g) return;
  for (int i = 0; i < g->ndata; i++) if (g->data[i].type == type) return;
  g->data = (checkpoint_data_ent*)realloc(g->data, sizeof(checkpoint_data_ent) * (g->ndata + 1));
  g->data[g->ndata].type = type;
  g->data[g->ndata].name = dupstr(name);
  g->data[g->ndata].dump = dump;
  g->data[g->ndata].restore = restore;
  g->data[g->ndata].restore_obj = NULL;
  g->ndata++;
}

MRB_API void
mrb_checkpoint_register_func(mrb_state *mrb, const char *name, mrb_func_t func)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (!g || !func) return;
  g->fnames = (char**)realloc(g->fnames, sizeof(char*) * (g->nfuncs + 1));
  g->funcs = (mrb_func_t*)realloc(g->funcs, sizeof(mrb_func_t) * (g->nfuncs + 1));
  size_t n = strlen(name);
  g->fnames[g->nfuncs] = (char*)malloc(n + 3);
  memcpy(g->fnames[g->nfuncs], "F:", 2);
  memcpy(g->fnames[g->nfuncs] + 2, name, n + 1);
  g->funcs[g->nfuncs] = func;
  g->nfuncs++;
}

MRB_API void
mrb_checkpoint_register_data_obj(mrb_state *mrb, const mrb_data_type *type, const char *name,
                                 mrb_checkpoint_data_dump dump, mrb_checkpoint_data_restore_obj restore)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (!g) return;
  mrb_checkpoint_register_data(mrb, type, name, dump, NULL);
  g->data[g->ndata - 1].restore_obj = restore;
}

/* ---- save / suspend ---- */

static struct RClass*
error_class(mrb_state *mrb)
{
  return mrb_class_get_under(mrb, mrb_module_get(mrb, "Checkpoint"), "Error");
}

static void
default_handler(mrb_state *mrb, const uint8_t *buf, size_t len, const char *path,
                enum mrb_checkpoint_kind kind, void *ud)
{
  FILE *fp = fopen(path, "wb");
  if (!fp || fwrite(buf, 1, len, fp) != len || fclose(fp) != 0) {
    if (kind == MRB_CHECKPOINT_SAVE) {
      mrb_raisef(mrb, error_class(mrb), "cannot write %s", path);
    }
    fprintf(stderr, "checkpoint: cannot write %s\n", path);
    exit(2);
  }
  if (kind == MRB_CHECKPOINT_SUSPEND) {
    fflush(stdout);
    exit(0);
  }
}

/* ---- the scheduler's flag ---- */

static void
snap_free(checkpoint_state *g)
{
  free(g->snap_wait); free(g->snap_reason); free(g->snap_ready);
  g->snap_wait = NULL; g->snap_reason = NULL; g->snap_ready = NULL;
  g->nsnap_wait = g->nsnap_ready = 0;
  g->snap_task = NULL;
}

/* raise the request: the VM tests task.switching at every boundary */
static void
mark_switch(mrb_state *mrb, checkpoint_state *g)
{
  snap_free(g);
  if (mrb->task.switching) {   /* a switch is asked for already */
    g->own_switch = 0;
    return;
  }
  g->own_switch = 1;
  mrb->task.switching = TRUE;
  mrb_task *run = checkpoint_running_task(mrb);
  if (!run) return;
  g->snap_task = run;
  int nw = 0, nr = 0;
  for (mrb_task *t = mrb->task.queues[MRB_TASK_QUEUE_WAITING]; t; t = t->next) nw++;
  for (mrb_task *t = mrb->task.queues[MRB_TASK_QUEUE_READY]; t; t = t->next) nr++;
  g->snap_wait = (void**)malloc(sizeof(void*) * (nw + 1));
  g->snap_reason = (uint8_t*)malloc(nw + 1);
  g->snap_ready = (void**)malloc(sizeof(void*) * (nr + 1));
  for (mrb_task *t = mrb->task.queues[MRB_TASK_QUEUE_WAITING]; t; t = t->next) {
    g->snap_reason[g->nsnap_wait] = t->reason;
    g->snap_wait[g->nsnap_wait++] = t;
  }
  for (mrb_task *t = mrb->task.queues[MRB_TASK_QUEUE_READY]; t; t = t->next) g->snap_ready[g->nsnap_ready++] = t;
}

static int
in_queue(mrb_state *mrb, int q, void *t)
{
  for (mrb_task *x = mrb->task.queues[q]; x; x = x->next) if ((void*)x == t) return 1;
  return 0;
}

/* at the safe point: did the scheduler ask for a switch (before the request,
   or since)? */
static int
sched_switch(mrb_state *mrb, checkpoint_state *g)
{
  int sw;
  if (!mrb->task.switching) sw = 0;    /* the scheduler cleared it (not at a safe point here) */
  else if (!g->own_switch) sw = 1;
  else if (!g->snap_task) sw = 0;      /* no Task was running: nothing to switch from */
  else {
    mrb_task *run = (mrb_task*)g->snap_task;
    sw = run->status != MRB_TASK_STATUS_RUNNING || run->c.status == MRB_TASK_STOPPED ||
         run->timeslice == 0;
    /* a waiting Task woke (a tick, a push, a close; a join only for a Task
       of higher priority, as wake_up_join_waiters does) */
    for (int i = 0; !sw && i < g->nsnap_wait; i++) {
      mrb_task *t = (mrb_task*)g->snap_wait[i];
      if (!in_queue(mrb, MRB_TASK_QUEUE_READY, t)) continue;
      if (g->snap_reason[i] != MRB_TASK_REASON_JOIN || t->priority < run->priority) sw = 1;
    }
    /* a Task of higher priority than the running one became ready (made,
       resumed) */
    for (mrb_task *t = mrb->task.queues[MRB_TASK_QUEUE_READY]; !sw && t; t = t->next) {
      if (t == run || t->priority >= run->priority) continue;
      int was = 0;
      for (int i = 0; i < g->nsnap_ready; i++) if (g->snap_ready[i] == (void*)t) { was = 1; break; }
      for (int i = 0; !was && i < g->nsnap_wait; i++) if (g->snap_wait[i] == (void*)t) was = 1;
      if (!was) sw = 1;
    }
  }
  snap_free(g);
  g->own_switch = 0;
  return sw;
}

/* leave the flag as the scheduler would have it */
static void
settle_switch(mrb_state *mrb, int sw)
{
  mrb->task.switching = sw ? TRUE : FALSE;
}

/* write the image of an external request here; sw: the scheduler asked for
   a switch at this point (the slice ends here). The flag is left to the
   caller. */
static void
external_write(mrb_state *mrb, checkpoint_state *g, int sw)
{
  g->pending = 0;
  g->dump_switch = sw;

  gbuf out;
  gb_init(&out);
  char err[512];
  if (checkpoint_dump(mrb, &out, err, sizeof(err))) {
    gb_free(mrb, &out);
    if (g->refusal) g->refusal(mrb, err, g->refusal_ud);
    return;
  }
  struct mrb_jmpbuf *prev_jmp = mrb->jmp;
  struct mrb_jmpbuf c_jmp;
  MRB_TRY(&c_jmp) {
    mrb->jmp = &c_jmp;
    if (g->handler) g->handler(mrb, out.p, out.len, g->output, MRB_CHECKPOINT_EXTERNAL, g->handler_ud);
    mrb->jmp = prev_jmp;
  }
  MRB_CATCH(&c_jmp) {
    mrb->jmp = prev_jmp;
    gb_free(mrb, &out);
    MRB_THROW(prev_jmp);
  }
  MRB_END_EXC(&c_jmp);
  gb_free(mrb, &out);
}

static void
external_safepoint(mrb_state *mrb, checkpoint_state *g)
{
  int sw = sched_switch(mrb, g);
  settle_switch(mrb, sw);
  external_write(mrb, g, sw);
}

static void
checkpoint_safepoint(mrb_state *mrb)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (!g || !g->pending) return;
  if (g->pending_kind == MRB_CHECKPOINT_EXTERNAL) {
    external_safepoint(mrb, g);
    return;
  }

  int sw = sched_switch(mrb, g);
  g->pending = 0;
  g->dump_switch = sw;
  char *path = g->pending_path;
  g->pending_path = NULL;
  enum mrb_checkpoint_kind kind = g->pending_kind;

  /* The call has returned: the caller is the current frame and the call's
     value (:saved) is in its register. The image gets :resumed there. */
  struct mrb_context *c = mrb->c;
  if (c != g->ret_ctx || c->ci - c->cibase != g->ret_ci || c->stbase + g->ret_slot >= c->stend ||
      !mrb_symbol_p(c->stbase[g->ret_slot]) ||
      mrb_symbol(c->stbase[g->ret_slot]) != MRB_SYM(saved)) {
    free(path);
    settle_switch(mrb, sw);
    mrb_raise(mrb, error_class(mrb), "the call's value is not where the checkpoint expects it");
  }
  mrb_value *slot = &c->stbase[g->ret_slot];
  *slot = mrb_symbol_value(MRB_SYM(resumed));

  gbuf out;
  gb_init(&out);
  char err[512];
  int failed = checkpoint_dump(mrb, &out, err, sizeof(err));
  *slot = mrb_symbol_value(MRB_SYM(saved));
  settle_switch(mrb, sw);
  if (failed) {
    gb_free(mrb, &out);
    free(path);
    mrb_raise(mrb, error_class(mrb), err);
  }
  struct mrb_jmpbuf *prev_jmp = mrb->jmp;
  struct mrb_jmpbuf c_jmp;
  MRB_TRY(&c_jmp) {
    mrb->jmp = &c_jmp;
    if (g->handler) g->handler(mrb, out.p, out.len, path, kind, g->handler_ud);
    else default_handler(mrb, out.p, out.len, path, kind, NULL);
    mrb->jmp = prev_jmp;
  }
  MRB_CATCH(&c_jmp) {
    mrb->jmp = prev_jmp;
    gb_free(mrb, &out);
    free(path);
    MRB_THROW(prev_jmp);
  }
  MRB_END_EXC(&c_jmp);
  /* SAVE, or a handler that returns from SUSPEND: the program goes on here */
  gb_free(mrb, &out);
  free(path);
}

/* Name the C function between the base frame and the caller, if any: a
   frame entered from C (cci > 0) sits on top of the C function's frame. The
   contexts the current one was resumed from count too (their frames wait
   below it), up to the root or a Task's (whose scheduler the reader starts
   again); a Fiber resumed by C (mrb_fiber_resume) waits in that C call. */
static int
c_boundary(mrb_state *mrb, char *buf, size_t len)
{
  for (struct mrb_context *c = mrb->c; c; c = c->prev) {
    mrb_task *task = checkpoint_task_of_context(mrb, c);
    if (c != mrb->root_c && !task && c->vmexec) {
      snprintf(buf, len, "a C function that resumed a Fiber (mrb_fiber_resume)");
      return 1;
    }
    if (c->cibase && c->ci) {
      /* in the current context c->ci is the frame of save / suspend itself */
      for (mrb_callinfo *ci = c->ci; ci > c->cibase; ci--) {
        if (ci->cci == 0) continue;
        mrb_callinfo *cf = ci - 1;
        if (cf > c->cibase && cf->mid) {
          struct RClass *k = mrb_vm_ci_target_class(cf);
          snprintf(buf, len, "%s#%s", k ? mrb_class_name(mrb, k) : "?",
                   mrb_sym_name(mrb, cf->mid));
        }
        else {
          snprintf(buf, len, "a C function");
        }
        return 1;
      }
    }
    if (c == mrb->root_c) break;
  }
  return 0;
}

static mrb_value
request(mrb_state *mrb, enum mrb_checkpoint_kind kind)
{
  const char *what = kind == MRB_CHECKPOINT_SAVE ? "Checkpoint.save" : "Checkpoint.suspend";
  const char *path = NULL;
  mrb_get_args(mrb, "|z!", &path);
  checkpoint_state *g = checkpoint_get(mrb);
  struct RClass *e = error_class(mrb);

  if (!g) mrb_raise(mrb, e, "checkpoint is not initialized");
  /* refusals that depend only on where the call is made come first, so that
     a reference run (IGNORE) behaves the same */
  char cfn[256];
  if (c_boundary(mrb, cfn, sizeof(cfn))) {
    mrb_raisef(mrb, e, "%s inside a block called from C (%s): the C frame cannot be carried", what, cfn);
  }
  if (g->mode == MRB_CHECKPOINT_MODE_IGNORE) {
    /* what the side that goes on sees in a real run */
    return mrb_symbol_value(kind == MRB_CHECKPOINT_SAVE ? MRB_SYM(saved) : MRB_SYM(resumed));
  }
  if (!g->perm) mrb_raisef(mrb, e, "%s: no baseline: run the program through the checkpoint driver", what);
  if (g->output) path = g->output;
  if (!path) mrb_raisef(mrb, e, "%s needs a path", what);
  if (g->pending) mrb_raisef(mrb, e, "%s: a checkpoint is already pending", what);

  /* where the value of this call lands: the caller's register R[a], which
     is the base of this cfunc's frame */
  struct mrb_context *c = mrb->c;
  free(g->pending_path);
  g->pending_path = dupstr(path);
  g->pending_kind = kind;
  g->ret_ctx = c;
  g->ret_ci = (c->ci - 1) - c->cibase;
  g->ret_slot = c->ci->stack - c->stbase;
  g->pending = 1;
  mark_switch(mrb, g);
  return mrb_symbol_value(MRB_SYM(saved));
}

MRB_API enum mrb_checkpoint_req
mrb_checkpoint_request(mrb_state *mrb)
{
  checkpoint_state *g = checkpoint_get(mrb);
  char cfn[256];
  if (!g || !g->perm) return MRB_CHECKPOINT_REQ_NOT_READY;
  if (g->pending) {
    /* the scheduler cleared the flag before the safe point came (it starts
       or ends a slice, the root sleeps): raise it again */
    if (!mrb->task.switching) mark_switch(mrb, g);
    return MRB_CHECKPOINT_REQ_PENDING;
  }
  /* an exception (or a break through an ensure clause) in flight is not
     refused: the VM calls the hook only once it is caught */
  if (c_boundary(mrb, cfn, sizeof(cfn))) return MRB_CHECKPOINT_REQ_C_BOUNDARY;
  g->pending = 1;
  g->pending_kind = MRB_CHECKPOINT_EXTERNAL;
  mark_switch(mrb, g);
  return MRB_CHECKPOINT_REQ_OK;
}

MRB_API enum mrb_checkpoint_req
mrb_checkpoint_request_here(mrb_state *mrb)
{
  checkpoint_state *g = checkpoint_get(mrb);
  char cfn[256];
  if (!g || !g->perm) return MRB_CHECKPOINT_REQ_NOT_READY;
  if (g->pending) {
    if (!mrb->task.switching) mark_switch(mrb, g);
    return MRB_CHECKPOINT_REQ_PENDING;
  }
  if (c_boundary(mrb, cfn, sizeof(cfn))) return MRB_CHECKPOINT_REQ_C_BOUNDARY;
  g->pending = 1;
  g->pending_kind = MRB_CHECKPOINT_EXTERNAL;
  if (mrb->exc || mrb->gc.iterating) {
    /* an exception (or a break through an ensure clause) in flight, or a C
       walk of the heap: the VM calls the hook at the first safe point */
    mark_switch(mrb, g);
    return MRB_CHECKPOINT_REQ_OK;
  }
  /* a safe point: the image is of this boundary, the flag stays as it is */
  external_write(mrb, g, mrb->task.switching ? 1 : 0);
  return MRB_CHECKPOINT_REQ_OK;
}

MRB_API void
mrb_checkpoint_set_refusal_handler(mrb_state *mrb, mrb_checkpoint_refusal_handler fn, void *ud)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (!g) return;
  g->refusal = fn;
  g->refusal_ud = ud;
}

MRB_API void
mrb_checkpoint_keep_dead_registers(mrb_state *mrb, mrb_bool keep)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (g) g->keep_dead = keep ? 1 : 0;
}

MRB_API uint32_t
mrb_checkpoint_last_dead(mrb_state *mrb)
{
  checkpoint_state *g = checkpoint_get(mrb);
  return g ? g->last_dead : 0;
}

static mrb_value
checkpoint_save(mrb_state *mrb, mrb_value self)
{
  return request(mrb, MRB_CHECKPOINT_SAVE);
}

static mrb_value
checkpoint_suspend(mrb_state *mrb, mrb_value self)
{
  return request(mrb, MRB_CHECKPOINT_SUSPEND);
}

#ifdef CHECKPOINT_TEST_HELPERS
/* test builds only: Task.pass and Checkpoint.save in one call, so that the
   image is taken where the scheduler has asked for a switch (the slice ends
   at that safe point, on both sides) */
static mrb_value
checkpoint_pass_and_save(mrb_state *mrb, mrb_value self)
{
  if (mrb->c != mrb->root_c) mrb->task.switching = TRUE;
  return request(mrb, MRB_CHECKPOINT_SAVE);
}
#endif

static mrb_value
checkpoint_resumed_p(mrb_state *mrb, mrb_value self)
{
  checkpoint_state *g = checkpoint_get(mrb);
  return mrb_bool_value(g && g->resumed);
}

/* ---- Checkpoint::Box: a C data type carried through the side table ---- */

typedef struct { int64_t v; } checkpoint_box;

static void
box_free(mrb_state *mrb, void *p)
{
  mrb_free(mrb, p);
}

static const struct mrb_data_type box_type = { "Checkpoint::Box", box_free };

static mrb_value
box_init(mrb_state *mrb, mrb_value self)
{
  mrb_int v = 0;
  mrb_get_args(mrb, "|i", &v);
  checkpoint_box *b = (checkpoint_box*)DATA_PTR(self);
  if (b) mrb_free(mrb, b);
  b = (checkpoint_box*)mrb_malloc(mrb, sizeof(checkpoint_box));
  b->v = v;
  mrb_data_init(self, b, &box_type);
  return self;
}

static mrb_value
box_value(mrb_state *mrb, mrb_value self)
{
  checkpoint_box *b = (checkpoint_box*)mrb_data_get_ptr(mrb, self, &box_type);
  return mrb_int_value(mrb, b->v);
}

static mrb_value
box_set_value(mrb_state *mrb, mrb_value self)
{
  mrb_int v;
  mrb_get_args(mrb, "i", &v);
  checkpoint_box *b = (checkpoint_box*)mrb_data_get_ptr(mrb, self, &box_type);
  b->v = v;
  return mrb_int_value(mrb, v);
}

static int
box_dump(mrb_state *mrb, mrb_value obj, mrb_checkpoint_buf *out)
{
  checkpoint_box *b = (checkpoint_box*)DATA_PTR(obj);
  uint8_t x[8];
  for (int i = 0; i < 8; i++) x[i] = (uint8_t)((uint64_t)b->v >> (56 - 8*i));
  mrb_checkpoint_buf_bytes(mrb, out, x, 8);
  return 0;
}

static void*
box_restore(mrb_state *mrb, const uint8_t *bytes, size_t len)
{
  checkpoint_box *b = (checkpoint_box*)mrb_malloc(mrb, sizeof(checkpoint_box));
  uint64_t u = 0;
  for (size_t i = 0; i < 8 && i < len; i++) u = (u << 8) | bytes[i];
  b->v = (int64_t)u;
  return b;
}

/* ---- gem ---- */

void
mrb_mruby_checkpoint_gem_init(mrb_state *mrb)
{
  checkpoint_new(mrb);
  mrb_vm_safepoint_hook = checkpoint_safepoint;

  struct RClass *m = mrb_define_module(mrb, "Checkpoint");
  mrb_define_class_under(mrb, m, "Error", mrb->eStandardError_class);
  mrb_define_module_function(mrb, m, "save", checkpoint_save, MRB_ARGS_OPT(1));
  mrb_define_module_function(mrb, m, "suspend", checkpoint_suspend, MRB_ARGS_OPT(1));
  mrb_define_module_function(mrb, m, "resumed?", checkpoint_resumed_p, MRB_ARGS_NONE());
#ifdef CHECKPOINT_TEST_HELPERS
  mrb_define_module_function(mrb, m, "__pass_and_save", checkpoint_pass_and_save, MRB_ARGS_OPT(1));
#endif

  struct RClass *box = mrb_define_class_under(mrb, m, "Box", mrb->object_class);
  MRB_SET_INSTANCE_TT(box, MRB_TT_CDATA);
  mrb_define_method(mrb, box, "initialize", box_init, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, box, "value", box_value, MRB_ARGS_NONE());
  mrb_define_method(mrb, box, "value=", box_set_value, MRB_ARGS_REQ(1));
  mrb_checkpoint_register_data(mrb, &box_type, "Checkpoint::Box", box_dump, box_restore);
}

void
mrb_mruby_checkpoint_gem_final(mrb_state *mrb)
{
  checkpoint_delete(mrb);
}
