/*
** checkpoint.c - mruby-checkpoint gem: Checkpoint.save / Checkpoint.suspend
** and the safe point hook
**
** save(path) and suspend(path) only raise a request: they mark the state
** and set mrb->task.switching, the flag the VM already tests at every
** instruction boundary for task switches. The VM then calls the state's
** safepoint_hook (patches/0001-vm-safepoint-hook.patch) at the next
** boundary where a task switch would be allowed (no C function between the
** root frame and the current one, no exception in flight). The request is
** only accepted when there is no C function between the root frame and the
** caller, so that boundary is always the very next instruction: the
** checkpoint lands right after the call, whose value is already in its
** register. Inside a block called from C (Array#sort!, Array.new { }, ...)
** the request is refused on the spot with Checkpoint::Error.
**
** save(path, source: true) also puts the program's source into the image
** (section SRC0, for display only; mrb_checkpoint_add_source registers it,
** mrb_checkpoint_set_include_source sets the default for calls without
** source:).
**
** The call's value tells the two sides apart: the writing side gets :saved,
** the image holds :resumed in the same register.
**
** task.switching is the scheduler's flag too (a Task's timeslice is over,
** it sleeps or passes, a Task it woke goes first): a request that sets it
** would hide whether the scheduler set it as well. mruby-task counts its own
** requests (task_switch_requests(), patches/0002-task-switch-requests.patch;
** every place that sets the flag for the scheduler moves the count). The
** request notes the count and whether the flag was set already (mark_switch);
** at the safe point the scheduler has asked for a switch exactly when the
** flag was set already or the count has moved (sched_switch). Then the flag
** is left set (the VM returns to the scheduler right after the hook) and the
** image says the slice ends there; otherwise it is cleared, as it would be
** without the request, unless the count moves meanwhile (a timeslice that
** ends on another thread, settle_switch).
**
** The state of each mrb_state is its own (checkpoint_get) and comes from its
** allocator: several VMs can carry the gem in one process.
*/

#include <stdio.h>
#include <stdlib.h>
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/string.h>
#include <mruby/array.h>
#include <mruby/data.h>
#include <mruby/variable.h>
#include <mruby/proc.h>
#include <mruby/throw.h>
#include <mruby/presym.h>
#include "checkpoint_internal.h"
#include "task.h"


/* ---- per-state side table ----

   Each mrb_state has its own checkpoint_state, allocated from that state's
   allocator (mrb_malloc), so that any number of VMs can coexist in one
   process, on several threads (Family mruby runs every application in its
   own VM, each with its own memory pool). Nothing of it is static. The
   pointer lives in a global variable of the state, under a name Ruby code
   cannot spell; it holds a C pointer, which is an immediate value: it is
   neither a permanent object nor something an image carries. */

#define STATE_GV "_checkpoint_state_"

static void checkpoint_safepoint(mrb_state *mrb);
static void checkpoint_gc_root(mrb_state *mrb);

/* The VM calls the state's safe point hook (mrb->safepoint_hook) at a task
   switch; looking the state up there (a symbol and a global variable) costs
   hundreds of instructions, so the hook is set only while this state has a
   request pending. A state without one does not call anything. The pointer
   is a field of the state (patches/0001), so the gem keeps nothing static
   and on a device it lives in the VM's own memory. A request is made and
   taken on the state's own thread. */
static void
set_pending(checkpoint_state *g, int on)
{
  on = on ? 1 : 0;
  if (g->pending == on) return;
  g->pending = on;
  g->mrb->safepoint_hook = on ? checkpoint_safepoint : NULL;
}

checkpoint_state*
checkpoint_get(mrb_state *mrb)
{
  mrb_value v = mrb_gv_get(mrb, mrb_intern_lit(mrb, STATE_GV));
  return mrb_cptr_p(v) ? (checkpoint_state*)mrb_cptr(v) : NULL;
}

static checkpoint_state*
checkpoint_new(mrb_state *mrb)
{
  checkpoint_state *g = (checkpoint_state*)mrb_calloc(mrb, 1, sizeof(checkpoint_state));
  g->mrb = mrb;
  mrb_gv_set(mrb, mrb_intern_lit(mrb, STATE_GV), mrb_cptr_value(mrb, g));
  /* the state's GC root hook (patches/0004): only the states that use the
     gem have one, the others call nothing at a root scan */
  mrb->gc_root_hook = checkpoint_gc_root;
  return g;
}

static void
checkpoint_delete(mrb_state *mrb)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (!g) return;
  set_pending(g, 0);
  mrb->gc_root_hook = NULL;
  mrb_gv_remove(mrb, mrb_intern_lit(mrb, STATE_GV));
  checkpoint_perm_free(mrb, g->perm);
  mrb_free(mrb, g->pending_path);
  mrb_free(mrb, g->output);
  for (int k = 0; k < g->ndata; k++) mrb_free(mrb, g->data[k].name);
  mrb_free(mrb, g->data);
  for (int k = 0; k < g->nfuncs; k++) mrb_free(mrb, g->fnames[k]);
  mrb_free(mrb, g->fnames); mrb_free(mrb, g->funcs);
  for (int k = 0; k < g->nmade; k++) mrb_free(mrb, g->made[k].name);
  mrb_free(mrb, g->made);
  for (int k = 0; k < g->nsrc; k++) { mrb_free(mrb, g->src[k].name); mrb_free(mrb, g->src[k].text); }
  mrb_free(mrb, g->src);
  mrb_free(mrb, g->missing);
  mrb_free(mrb, g);
}

char*
checkpoint_strdup(mrb_state *mrb, const char *s)
{
  if (!s) return NULL;
  size_t n = strlen(s);
  char *d = (char*)mrb_malloc(mrb, n + 1);
  memcpy(d, s, n + 1);
  return d;
}

/* ---- C API ---- */

MRB_API void
mrb_checkpoint_baseline(mrb_state *mrb)
{
  checkpoint_state *g = checkpoint_need(mrb);
  if (!g) return;
  checkpoint_perm_free(mrb, g->perm);
  g->perm = NULL;
  checkpoint_bundled_register(mrb);
  checkpoint_task_register(mrb);
  g->perm = checkpoint_perm_build(mrb, g);
}

MRB_API void
mrb_checkpoint_set_table(mrb_state *mrb, const mrb_checkpoint_table *table)
{
  checkpoint_state *g = checkpoint_need(mrb);
  if (g) g->table = table;
}

MRB_API void
mrb_checkpoint_prepare_table(mrb_state *mrb)
{
  checkpoint_state *g = checkpoint_need(mrb);
  if (g) g->for_table = 1;
}

MRB_API const char*
mrb_checkpoint_table_status(mrb_state *mrb)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (!g || !g->perm) return "not used: no baseline";
  return g->table_why[0] ? g->table_why : NULL;
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
mrb_checkpoint_add_source(mrb_state *mrb, const char *filename, const char *src, size_t len)
{
  checkpoint_state *g = checkpoint_need(mrb);
  if (!g || !filename) return;
  char *text = (char*)mrb_malloc(mrb, len + 1);
  if (len) memcpy(text, src, len);
  text[len] = 0;
  for (int i = 0; i < g->nsrc; i++) {
    if (strcmp(g->src[i].name, filename) == 0) {
      mrb_free(mrb, g->src[i].text);
      g->src[i].text = text;
      g->src[i].len = len;
      return;
    }
  }
  struct checkpoint_source *a = (struct checkpoint_source*)mrb_realloc_simple(mrb, g->src, sizeof(*a) * (g->nsrc + 1));
  if (!a) { mrb_free(mrb, text); return; }
  g->src = a;
  char *name = (char*)mrb_malloc_simple(mrb, strlen(filename) + 1);
  if (!name) { mrb_free(mrb, text); return; }
  memcpy(name, filename, strlen(filename) + 1);
  g->src[g->nsrc].name = name;
  g->src[g->nsrc].text = text;
  g->src[g->nsrc].len = len;
  g->nsrc++;
}

const struct checkpoint_source*
checkpoint_source_of(checkpoint_state *g, const char *name, size_t len)
{
  for (int i = 0; i < g->nsrc; i++) {
    if (strlen(g->src[i].name) == len && memcmp(g->src[i].name, name, len) == 0) return &g->src[i];
  }
  return NULL;
}

MRB_API void
mrb_checkpoint_set_include_source(mrb_state *mrb, mrb_bool on)
{
  checkpoint_state *g = checkpoint_need(mrb);
  if (g) g->source_default = on ? 1 : 0;
}

MRB_API void
mrb_checkpoint_set_mode(mrb_state *mrb, enum mrb_checkpoint_mode mode)
{
  checkpoint_state *g = checkpoint_need(mrb);
  if (g) g->mode = mode;
}

MRB_API void
mrb_checkpoint_set_output(mrb_state *mrb, const char *path)
{
  checkpoint_state *g = checkpoint_need(mrb);
  if (!g) return;
  mrb_free(mrb, g->output);
  g->output = checkpoint_strdup(mrb, path);
}

MRB_API void
mrb_checkpoint_set_handler(mrb_state *mrb, mrb_checkpoint_handler fn, void *ud)
{
  checkpoint_state *g = checkpoint_need(mrb);
  if (!g) return;
  g->handler = fn;
  g->handler_ud = ud;
}

MRB_API void
mrb_checkpoint_set_allocator(mrb_state *mrb, mrb_checkpoint_realloc_fn fn, void *ud)
{
  checkpoint_state *g = checkpoint_need(mrb);
  if (!g) return;
  g->alloc = fn;
  g->alloc_ud = ud;
}

MRB_API void
mrb_checkpoint_set_embedder(mrb_state *mrb, mrb_checkpoint_embedder_dump dump,
                            mrb_checkpoint_embedder_restore restore, void *ud)
{
  checkpoint_state *g = checkpoint_need(mrb);
  if (!g) return;
  g->emb_dump = dump;
  g->emb_restore = restore;
  g->emb_ud = ud;
}

MRB_API void
mrb_checkpoint_register_data(mrb_state *mrb, const mrb_data_type *type, const char *name,
                        mrb_checkpoint_data_dump dump, mrb_checkpoint_data_restore restore)
{
  checkpoint_state *g = checkpoint_need(mrb);
  if (!g) return;
  for (int i = 0; i < g->ndata; i++) if (g->data[i].type == type) return;
  g->data = (checkpoint_data_ent*)mrb_realloc(mrb, g->data, sizeof(checkpoint_data_ent) * (g->ndata + 1));
  g->data[g->ndata].type = type;
  g->data[g->ndata].name = checkpoint_strdup(mrb, name);
  g->data[g->ndata].dump = dump;
  g->data[g->ndata].restore = restore;
  g->data[g->ndata].restore_obj = NULL;
  g->ndata++;
}

MRB_API void
mrb_checkpoint_register_func(mrb_state *mrb, const char *name, mrb_func_t func)
{
  checkpoint_state *g = checkpoint_need(mrb);
  if (!g || !func) return;
  g->fnames = (char**)mrb_realloc(mrb, g->fnames, sizeof(char*) * (g->nfuncs + 1));
  g->funcs = (mrb_func_t*)mrb_realloc(mrb, g->funcs, sizeof(mrb_func_t) * (g->nfuncs + 1));
  size_t n = strlen(name);
  g->fnames[g->nfuncs] = (char*)mrb_malloc(mrb, n + 3);
  memcpy(g->fnames[g->nfuncs], "F:", 2);
  memcpy(g->fnames[g->nfuncs] + 2, name, n + 1);
  g->funcs[g->nfuncs] = func;
  g->nfuncs++;
}

MRB_API void
mrb_checkpoint_register_data_obj(mrb_state *mrb, const mrb_data_type *type, const char *name,
                                 mrb_checkpoint_data_dump dump, mrb_checkpoint_data_restore_obj restore)
{
  checkpoint_state *g = checkpoint_need(mrb);
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

/* raise the request: the VM tests task.switching at every boundary. The
   scheduler's record as it stands now is what the safe point compares with. */
static void
mark_switch(mrb_state *mrb, checkpoint_state *g)
{
  g->sched_asked = mrb->task.switching ? 1 : 0;
  g->sched_mark = task_switch_requests(mrb);
  mrb->task.switching = TRUE;
}

/* at the safe point: has the scheduler asked for a switch (before the
   request, or since)? */
static int
sched_switch(mrb_state *mrb, checkpoint_state *g)
{
  int sw;
  g->sched_requests = task_switch_requests(mrb);   /* what settle_switch compares with */
  if (!mrb->task.switching) sw = 0;    /* the scheduler cleared it (not at a safe point here) */
  else sw = g->sched_asked || g->sched_requests != g->sched_mark;
  g->sched_asked = 0;
  return sw;
}

/* leave the flag as the scheduler would have it. The scheduler's timeslice
   may end on another thread meanwhile (a tick task that raises the flag
   without the VM, as Family mruby's FreeRTOS port does; it moves
   switch_requests_async first): a count that moved since sched_switch is a
   request the cleared flag must not hide. */
static void
settle_switch(mrb_state *mrb, checkpoint_state *g, int sw)
{
  if (sw) {
    mrb->task.switching = TRUE;
    return;
  }
  mrb->task.switching = FALSE;
#if defined(__GNUC__) || defined(__clang__)
  __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the store above before the load below */
#endif
  if (task_switch_requests(mrb) != g->sched_requests) mrb->task.switching = TRUE;
}

/* write the image of an external request here; sw: the scheduler asked for
   a switch at this point (the slice ends here). The flag is left to the
   caller. */
static void
external_write(mrb_state *mrb, checkpoint_state *g, int sw)
{
  set_pending(g, 0);
  g->dump_switch = sw;
  g->dump_source = g->source_default;

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
  settle_switch(mrb, g, sw);
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
  set_pending(g, 0);
  g->dump_switch = sw;
  g->dump_source = g->pending_source;
  char *path = g->pending_path;
  g->pending_path = NULL;
  enum mrb_checkpoint_kind kind = g->pending_kind;

  /* The call has returned: the caller is the current frame and the call's
     value (:saved) is in its register. The image gets :resumed there. */
  struct mrb_context *c = mrb->c;
  if (c != g->ret_ctx || c->ci - c->cibase != g->ret_ci || c->stbase + g->ret_slot >= c->stend ||
      !mrb_symbol_p(c->stbase[g->ret_slot]) ||
      mrb_symbol(c->stbase[g->ret_slot]) != MRB_SYM(saved)) {
    mrb_free(mrb, path);
    settle_switch(mrb, g, sw);
    mrb_raise(mrb, error_class(mrb), "the call's value is not where the checkpoint expects it");
  }
  mrb_value *slot = &c->stbase[g->ret_slot];
  *slot = mrb_symbol_value(MRB_SYM(resumed));

  gbuf out;
  gb_init(&out);
  char err[512];
  int failed = checkpoint_dump(mrb, &out, err, sizeof(err));
  *slot = mrb_symbol_value(MRB_SYM(saved));
  settle_switch(mrb, g, sw);
  if (failed) {
    gb_free(mrb, &out);
    mrb_free(mrb, path);
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
    mrb_free(mrb, path);
    MRB_THROW(prev_jmp);
  }
  MRB_END_EXC(&c_jmp);
  /* SAVE, or a handler that returns from SUSPEND: the program goes on here */
  gb_free(mrb, &out);
  mrb_free(mrb, path);
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
  /* source: true / false: the image carries the program's source (SRC0) */
  mrb_sym kw_names[] = { MRB_SYM(source) };
  mrb_value kw_values[1];
  mrb_kwargs kw = { 1, 0, kw_names, kw_values, NULL };
  mrb_get_args(mrb, "|z!:", &path, &kw);
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
  mrb_free(mrb, g->pending_path);
  g->pending_path = NULL;
  g->pending_path = checkpoint_strdup(mrb, path);
  g->pending_kind = kind;
  g->pending_source = mrb_undef_p(kw_values[0]) ? g->source_default : mrb_test(kw_values[0]);
  g->ret_ctx = c;
  g->ret_ci = (c->ci - 1) - c->cibase;
  g->ret_slot = c->ci->stack - c->stbase;
  set_pending(g, 1);
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
  set_pending(g, 1);
  g->pending_kind = MRB_CHECKPOINT_EXTERNAL;
  mark_switch(mrb, g);
  return MRB_CHECKPOINT_REQ_OK;
}

MRB_API void
mrb_checkpoint_set_refusal_handler(mrb_state *mrb, mrb_checkpoint_refusal_handler fn, void *ud)
{
  checkpoint_state *g = checkpoint_need(mrb);
  if (!g) return;
  g->refusal = fn;
  g->refusal_ud = ud;
}

MRB_API void
mrb_checkpoint_keep_dead_registers(mrb_state *mrb, mrb_bool keep)
{
  checkpoint_state *g = checkpoint_need(mrb);
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
/* test builds only: the names an image would carry in SND0 (mrb_checkpoint_sent_names) */
static void
sent_name_cb(mrb_state *mrb, const char *name, size_t len, void *ud)
{
  mrb_ary_push(mrb, *(mrb_value*)ud, mrb_str_new(mrb, name, (mrb_int)len));
}

static mrb_value
checkpoint_sent_names_m(mrb_state *mrb, mrb_value self)
{
  mrb_value a = mrb_ary_new(mrb);
  mrb_checkpoint_sent_names(mrb, sent_name_cb, &a);
  return a;
}

/* test builds only: Task.pass and Checkpoint.save (or suspend) in one call,
   so that the image is taken where the scheduler has asked for a switch (the
   slice ends at that safe point, on both sides). The pass is the one of
   mruby-task (task_request_switch) and is counted as the scheduler's. */
static void
pass_like_task(mrb_state *mrb)
{
  if (mrb->c != mrb->root_c) {
    mrb->task.switch_requests++;
    mrb->task.switching = TRUE;
  }
}

static mrb_value
checkpoint_pass_and_save(mrb_state *mrb, mrb_value self)
{
  pass_like_task(mrb);
  return request(mrb, MRB_CHECKPOINT_SAVE);
}

static mrb_value
checkpoint_pass_and_suspend(mrb_state *mrb, mrb_value self)
{
  pass_like_task(mrb);
  return request(mrb, MRB_CHECKPOINT_SUSPEND);
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

/* Sets the gem up for one mrb_state: its state, the Ruby API (Checkpoint,
   Checkpoint::Error, Checkpoint::Box) and the gem's own C data type. Nothing
   of it exists in a state until the embedder asks: mrb_checkpoint_open(), or
   any call that configures the gem (the baseline, a handler, a registration).
   A VM that never uses the gem pays nothing for it (its hooks stay NULL). */
checkpoint_state*
checkpoint_need(mrb_state *mrb)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (g) return g;
  int ai = mrb_gc_arena_save(mrb);
  g = checkpoint_new(mrb);

  struct RClass *m = mrb_define_module(mrb, "Checkpoint");
  mrb_define_class_under(mrb, m, "Error", mrb->eStandardError_class);
  mrb_define_module_function(mrb, m, "save", checkpoint_save, MRB_ARGS_OPT(1) | MRB_ARGS_KEY(1, 0));
  mrb_define_module_function(mrb, m, "suspend", checkpoint_suspend, MRB_ARGS_OPT(1) | MRB_ARGS_KEY(1, 0));
  mrb_define_module_function(mrb, m, "resumed?", checkpoint_resumed_p, MRB_ARGS_NONE());
#ifdef CHECKPOINT_TEST_HELPERS
  mrb_define_module_function(mrb, m, "__pass_and_save", checkpoint_pass_and_save, MRB_ARGS_OPT(1));
  mrb_define_module_function(mrb, m, "__pass_and_suspend", checkpoint_pass_and_suspend, MRB_ARGS_OPT(1));
  mrb_define_module_function(mrb, m, "__sent_names", checkpoint_sent_names_m, MRB_ARGS_NONE());
#endif

  struct RClass *box = mrb_define_class_under(mrb, m, "Box", mrb->object_class);
  MRB_SET_INSTANCE_TT(box, MRB_TT_CDATA);
  mrb_define_method(mrb, box, "initialize", box_init, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, box, "value", box_value, MRB_ARGS_NONE());
  mrb_define_method(mrb, box, "value=", box_set_value, MRB_ARGS_REQ(1));
  mrb_checkpoint_register_data(mrb, &box_type, "Checkpoint::Box", box_dump, box_restore);
  mrb_gc_arena_restore(mrb, ai);
  return g;
}

MRB_API void
mrb_checkpoint_open(mrb_state *mrb)
{
  checkpoint_need(mrb);
}

/* ---- gem ---- */

/* the GC's root hook (patches/0004): the objects of a baseline kept as
   addresses (with a table). Set in the states that use the gem
   (checkpoint_new): a lookup only, nothing is allocated. */
static void
checkpoint_gc_root(mrb_state *mrb)
{
  mrb_sym sym = mrb_intern_check(mrb, STATE_GV, sizeof(STATE_GV) - 1);
  if (sym == 0) return;
  mrb_value v = mrb_gv_get(mrb, sym);
  if (!mrb_cptr_p(v)) return;
  checkpoint_state *g = (checkpoint_state*)mrb_cptr(v);
  if (g->perm) checkpoint_perm_mark(mrb, g->perm);
}

void
mrb_mruby_checkpoint_gem_init(mrb_state *mrb)
{
  /* nothing until the embedder asks (checkpoint_need): the state's hooks
     stay NULL */
  (void)mrb;
}

void
mrb_mruby_checkpoint_gem_final(mrb_state *mrb)
{
  checkpoint_delete(mrb);
}
