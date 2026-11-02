/*
** checkpoint_task.c - Tasks (mruby-task) and Fibers in a checkpoint (format v1)
**
** The writer and the reader of the contexts and the scheduler are in
** checkpoint_dump.c and checkpoint_restore.c. Here:
**   - the probes for mruby-task's data types (static there) and the entry of
**     Task::Queue in the C data table;
**   - mrb_checkpoint_continue: what was running goes on.
**
** Going on. The image says which context was running and, if a Task was
** running, which one; the scheduler's own C frames (mrb_task_run ->
** execute_task -> mrb_vm_exec) are not in the image. They hold nothing the
** queues do not say, so the reader makes them again:
**   - no Task was running: the current context (the root, or a Fiber the
**     root resumed) goes on in mrb_vm_exec, as the writer's did;
**   - a Task was running: its slice goes on from the current context (the
**     Task's own or a Fiber it resumed) inside the same steps as mruby-task's
**     execute_task (task_slice below, with the remaining timeslice reset).
**     If the scheduler had asked for a switch at that safe point, the slice
**     ends right there, as it did on the writer. Then the caller of the
**     scheduler takes over as it would have: the next round of the loop of
**     Task.run (the call is made again: its frame is dropped and the
**     caller's pc set back to the call), the return of Task.pass (nil), or
**     the host's mrb_task_run (a host that ran the scheduler from C).
** task_slice, wake_join and rotate follow execute_task, wake_up_join_waiters
** and task_run_body of mruby-task/src/task.c (mruby f56d44ecc), which are
** static there. The scheduler itself (its queues, the order of priorities)
** is not changed.
*/

#include <string.h>
#include <mruby.h>
#include <mruby/data.h>
#include <mruby/class.h>
#include <mruby/proc.h>
#include <mruby/irep.h>
#include <mruby/opcode.h>
#include <mruby/error.h>
#include <mruby/variable.h>
#include <mruby/internal.h>
#include "checkpoint_internal.h"
#include "task.h"

/* ---- the C functions a frame may wait in ---- */

static const char *const switch_names[] = {
  "F:Fiber#resume", "F:Fiber#transfer", "F:S:Fiber#yield",
  "F:S:Task#run", "F:S:Task#pass",
};

int
checkpoint_switch_func_name(const char *name, size_t len)
{
  for (size_t i = 0; i < sizeof(switch_names) / sizeof(switch_names[0]); i++) {
    if (strlen(switch_names[i]) == len && memcmp(switch_names[i], name, len) == 0) return 1;
  }
  return 0;
}

/* ---- probes ---- */

/* a Proc for mrb_create_task to start from: one OP_STOP */
static struct RProc*
stop_proc(mrb_state *mrb)
{
  mrb_irep *irep = mrb_add_irep(mrb);
  mrb_code *iseq = (mrb_code*)mrb_malloc(mrb, 1);
  iseq[0] = OP_STOP;
  irep->iseq = iseq;
  irep->ilen = 1;
  irep->nlocals = 1;
  irep->nregs = 2;
  struct RProc *p = mrb_proc_new(mrb, irep);
  mrb_irep_decref(mrb, irep);
  return p;
}

/* mruby-task's data type of Task: from a Task made for the purpose and
   taken out of the scheduler again at once */
const mrb_data_type*
checkpoint_task_type(mrb_state *mrb)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (g && g->task_type) return g->task_type;
  if (!mrb_class_defined(mrb, "Task")) return NULL;
  int ai = mrb_gc_arena_save(mrb);
  mrb_bool sw = mrb->task.switching;
  mrb_value v = mrb_create_task(mrb, stop_proc(mrb), mrb_nil_value(), mrb_nil_value(), mrb_nil_value());
  const mrb_data_type *type = DATA_TYPE(v);
  mrb_task *t = (mrb_task*)DATA_PTR(v);
  mrb_task_excl_enter(mrb);
  mrb_task_q_delete(mrb, t);
  mrb_task_excl_exit(mrb);
  mrb_gc_unregister(mrb, v);
  DATA_PTR(v) = NULL;
  DATA_TYPE(v) = NULL;
  mrb_free(mrb, t->c.stbase);
  mrb_free(mrb, t->c.cibase);
  mrb_free(mrb, t);
  mrb->task.switching = sw;
  mrb_gc_arena_restore(mrb, ai);
  if (g) g->task_type = type;
  return type;
}

/* Task::Queue (mruby-task/src/task_queue.c, struct mrb_task_queue): the
   closed flag; the items are its ivar @items, the Tasks that wait on it
   name it in the scheduler section */
struct ck_task_queue { uint8_t closed; };

static int
queue_dump(mrb_state *mrb, mrb_value obj, mrb_checkpoint_buf *out)
{
  struct ck_task_queue *q = (struct ck_task_queue*)DATA_PTR(obj);
  uint8_t c = q->closed ? 1 : 0;
  mrb_checkpoint_buf_bytes(mrb, out, &c, 1);
  return 0;
}

static void*
queue_restore(mrb_state *mrb, const uint8_t *bytes, size_t len)
{
  struct ck_task_queue *q = (struct ck_task_queue*)mrb_malloc(mrb, sizeof(struct ck_task_queue));
  q->closed = (len > 0 && bytes[0]) ? 1 : 0;
  return q;
}

static mrb_value
probe_queue(mrb_state *mrb, void *ud)
{
  const mrb_data_type **out = (const mrb_data_type**)ud;
  struct RClass *task = mrb_class_get(mrb, "Task");
  mrb_value q = mrb_obj_new(mrb, mrb_class_get_under(mrb, task, "Queue"), 0, NULL);
  *out = DATA_TYPE(q);
  return mrb_nil_value();
}

/* at the baseline, on both sides (made objects are garbage then) */
void
checkpoint_task_register(mrb_state *mrb)
{
  if (!mrb_class_defined(mrb, "Task")) return;
  checkpoint_task_type(mrb);
  const mrb_data_type *qt = NULL;
  int ai = mrb_gc_arena_save(mrb);
  mrb_bool err;
  mrb_protect_error(mrb, probe_queue, &qt, &err);
  mrb->exc = NULL;
  mrb_gc_arena_restore(mrb, ai);
  if (qt) mrb_checkpoint_register_data(mrb, qt, "Task::Queue", queue_dump, queue_restore);
}

/* ---- which Task a context is ---- */

mrb_task*
checkpoint_task_of_context(mrb_state *mrb, struct mrb_context *c)
{
  if (!c || c == mrb->root_c) return NULL;
  for (int q = 0; q < MRB_NUM_TASK_QUEUE; q++) {
    for (mrb_task *t = mrb->task.queues[q]; t; t = t->next) {
      if (&t->c == c) return t;
    }
  }
  return NULL;
}

/* the Task whose slice is running (the one RUNNING in the ready queue) */
mrb_task*
checkpoint_running_task(mrb_state *mrb)
{
  for (mrb_task *t = mrb->task.queues[MRB_TASK_QUEUE_READY]; t; t = t->next) {
    if (t->status == MRB_TASK_STATUS_RUNNING) return t;
  }
  return NULL;
}

/* ---- one slice of a Task (execute_task) ---- */

/* wake_up_join_waiters */
static void
wake_join(mrb_state *mrb, mrb_task *done)
{
  mrb_task_excl_enter(mrb);
  mrb_task *curr = mrb->task.queues[MRB_TASK_QUEUE_WAITING];
  while (curr != NULL) {
    mrb_task *next = curr->next;
    if (curr->reason == MRB_TASK_REASON_JOIN && curr->wait.join == done) {
      mrb_task_q_delete(mrb, curr);
      curr->status = MRB_TASK_STATUS_READY;
      curr->reason = MRB_TASK_REASON_NONE;
      curr->wait.join = NULL;
      mrb_task_q_insert(mrb, curr);
      if (mrb->c != mrb->root_c && !mrb->task.switching) {
        mrb_task *running = checkpoint_task_of_context(mrb, mrb->c);
        if (running && curr->priority < running->priority) {
          mrb->task.switching = TRUE;
        }
      }
    }
    curr = next;
  }
  mrb_task_excl_exit(mrb);
}

typedef struct { mrb_task *t; const struct RProc *proc; const mrb_code *pc; } slice_args;

/* execute_task_vm */
static mrb_value
slice_vm(mrb_state *mrb, void *ud)
{
  slice_args *a = (slice_args*)ud;
  mrb->task.exception_as_result = TRUE;
  a->t->result = mrb_vm_exec(mrb, a->proc, a->pc);
  if (mrb->exc) {
    a->t->result = mrb_obj_value(mrb->exc);
    mrb->exc = NULL;
  }
  mrb->task.exception_as_result = FALSE;
  return a->t->result;
}

/* execute_task, going on from the context `start` (the Task's own, or a
   Fiber it resumed). ended: the slice had already come to its end at the
   safe point (the scheduler had asked for a switch): only what
   execute_task does after mrb_vm_exec, which returned nil there. */
static void
task_slice(mrb_state *mrb, mrb_task *t, struct mrb_context *start, int ended, int keep_timeslice)
{
  struct mrb_context *prev_c = mrb->c;
  mrb_callinfo *prev_ci = prev_c->ci;
  uint8_t prev_cci = prev_c->ci->cci;
  mrb_bool error = FALSE;

  if (!ended) {
    /* the timeslice starts full (doc/format_v1.md: the reader's choice);
       mrb_checkpoint_keep_timeslice: what was left of it on the writer */
    if (!keep_timeslice || t->timeslice == 0 || t->timeslice > MRB_TIMESLICE_TICK_COUNT) {
      t->timeslice = MRB_TIMESLICE_TICK_COUNT;
    }
    t->status = MRB_TASK_STATUS_RUNNING;
    t->c.prev = mrb->c;
    mrb->c = start;
    mrb->task.switching = FALSE;
    slice_args a = { t, start->ci->proc, start->ci->pc };
    t->c.vmexec = TRUE;
    int ai = mrb_gc_arena_save(mrb);
    t->result = mrb_protect_error(mrb, slice_vm, &a, &error);
    mrb_gc_arena_restore(mrb, ai);
    mrb->task.exception_as_result = FALSE;
  }
  else {
    t->result = mrb_nil_value();
  }

  t->c.vmexec = FALSE;
  mrb->task.switching = FALSE;
  mrb->c = prev_c;
  t->c.prev = NULL;
  prev_c->ci = prev_ci;
  prev_ci->cci = prev_cci;
  if (error) {
    t->c.status = MRB_TASK_STOPPED;
  }
  if (t->c.status == MRB_TASK_STOPPED) {
    mrb->task.switching = FALSE;
    mrb_task_excl_enter(mrb);
    mrb_task_q_delete(mrb, t);
    t->status = MRB_TASK_STATUS_DORMANT;
    mrb_task_q_insert(mrb, t);
    mrb_task_excl_exit(mrb);
    wake_join(mrb, t);
  }
  else if (t->status == MRB_TASK_STATUS_RUNNING) {
    t->status = MRB_TASK_STATUS_READY;
  }
}

/* what task_run_body does after a slice: round robin, a step of the GC */
static void
loop_after_slice(mrb_state *mrb, mrb_task *t)
{
  if (t->status == MRB_TASK_STATUS_READY) {
    mrb_task_excl_enter(mrb);
    mrb_task_q_delete(mrb, t);
    mrb_task_q_insert(mrb, t);
    mrb_task_excl_exit(mrb);
  }
  if (mrb->gc.state != MRB_GC_STATE_ROOT) {
    mrb_incremental_gc(mrb);
  }
}

/* ---- going on ---- */

static void
continue_error(mrb_state *mrb, const char *msg)
{
  mrb_raise(mrb, mrb_class_get_under(mrb, mrb_module_get(mrb, "Checkpoint"), "Error"), msg);
}

static int
no_frames_p(struct mrb_context *c)
{
  return !c->cibase || !c->ci || (c->ci == c->cibase && !c->cibase->proc);
}

/* the name of the C function the top frame of c runs */
static const char*
top_func_name(mrb_state *mrb, checkpoint_state *g, struct mrb_context *c)
{
  mrb_callinfo *ci = c->ci;
  const struct RProc *p = ci->proc;
  if (p && MRB_PROC_CFUNC_P(p)) return checkpoint_perm_func_name(g->perm, p->body.func);
  if (p) return NULL;
  struct RClass *k = mrb_class(mrb, ci->stack[0]);
  mrb_method_t m = mrb_method_search_vm(mrb, &k, ci->mid);
  if (MRB_METHOD_UNDEF_P(m)) return NULL;
  if (MRB_METHOD_FUNC_P(m)) return checkpoint_perm_func_name(g->perm, MRB_METHOD_FUNC(m));
  p = MRB_METHOD_PROC(m);
  return (p && MRB_PROC_CFUNC_P(p)) ? checkpoint_perm_func_name(g->perm, p->body.func) : NULL;
}

/* drop the top frame of c (a call to a C function) and set the caller back
   to the instruction that made the call: it is made again */
static void
call_again(mrb_state *mrb, struct mrb_context *c)
{
  mrb_callinfo *ci = c->ci;
  mrb_sym mid = ci->mid;
  if (ci == c->cibase) continue_error(mrb, "the scheduler's caller has no frame");
  mrb_callinfo *caller = ci - 1;
  const struct RProc *p = caller->proc;
  if (!p || MRB_PROC_CFUNC_P(p) || !caller->pc) continue_error(mrb, "the scheduler's caller is not Ruby code");
  const mrb_irep *irep = p->body.irep;
  uint32_t start, a, b, cc;
  uint8_t op;
  if (!checkpoint_insn_ending_at(irep, (uint32_t)(caller->pc - irep->iseq), &start, &op, &a, &b, &cc)) {
    continue_error(mrb, "the call to the scheduler is not found");
  }
  int send = (op == OP_SEND || op == OP_SENDB || op == OP_SSEND || op == OP_SSENDB);
  int send0 = (op == OP_SEND0 || op == OP_SSEND0);
  if (!(send || send0) || b >= irep->slen || irep->syms[b] != mid ||
      (send && ((cc >> 4) & 0xf) != 0 && ((cc >> 4) & 0xf) != 15) ||
      ci->stack != caller->stack + a) {
    continue_error(mrb, "the call to the scheduler cannot be made again");
  }
  c->ci = caller;
  caller->pc = irep->iseq + start;
}

/* the call in the top frame of c returns v */
static void
call_returns(mrb_state *mrb, struct mrb_context *c, mrb_value v)
{
  mrb_callinfo *ci = c->ci;
  if (ci == c->cibase) continue_error(mrb, "the scheduler's caller has no frame");
  ci->stack[0] = v;
  struct RProc *b = ci->blk;
  if (b && !MRB_PROC_STRICT_P(b) && MRB_PROC_ENV(b) == mrb_vm_ci_env(ci - 1)) {
    b->flags |= MRB_PROC_ORPHAN;
  }
  c->ci--;
}

MRB_API mrb_value
mrb_checkpoint_continue(mrb_state *mrb)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (!g || !g->plan.set) continue_error(mrb, "nothing restored");
  struct checkpoint_resume_plan plan = g->plan;
  memset(&g->plan, 0, sizeof(g->plan));
  mrb_task *t = (mrb_task*)plan.task;

  if (!t) {
    /* the root, or a Fiber the root resumed: as it stood */
    struct mrb_context *x = plan.current;
    if (no_frames_p(x)) continue_error(mrb, "nothing restored");
    mrb->c = x;
    mrb->task.switching = plan.switch_pending ? TRUE : FALSE;
    return mrb_vm_exec(mrb, x->ci->proc, x->ci->pc);
  }

  /* the context the scheduler ran in */
  struct mrb_context *e = t->c.prev;
  if (!e) continue_error(mrb, "the running Task has no context it was entered from");
  const char *entry = no_frames_p(e) ? NULL : top_func_name(mrb, g, e);
  if (!no_frames_p(e) && (!entry || (strcmp(entry, "F:S:Task#run") != 0 && strcmp(entry, "F:S:Task#pass") != 0))) {
    continue_error(mrb, "the scheduler was not entered from Task.run, Task.pass or the host");
  }
  mrb->c = e;
  mrb->task.loop_running = plan.loop_running ? TRUE : FALSE;
  task_slice(mrb, t, plan.current, plan.switch_pending, g->keep_timeslice);

  if (!entry) {
    /* the host's mrb_task_run: the rest of its loop */
    loop_after_slice(mrb, t);
    mrb->task.loop_running = FALSE;
    return mrb_task_run(mrb);
  }
  if (strcmp(entry, "F:S:Task#run") == 0) {
    loop_after_slice(mrb, t);
    mrb->task.loop_running = FALSE;
    call_again(mrb, e);
  }
  else {
    call_returns(mrb, e, mrb_nil_value());
  }
  mrb->c = e;
  return mrb_vm_exec(mrb, e->ci->proc, e->ci->pc);
}

MRB_API void
mrb_checkpoint_keep_timeslice(mrb_state *mrb, mrb_bool keep)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (g) g->keep_timeslice = keep ? 1 : 0;
}

/* ---- the embedder's Task ---- */

MRB_API void
mrb_checkpoint_set_host_task(mrb_state *mrb, mrb_value task)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (!g) return;
  const mrb_data_type *type = checkpoint_task_type(mrb);
  g->host_task = (mrb_type(task) == MRB_TT_CDATA && DATA_TYPE(task) == type) ? DATA_PTR(task) : NULL;
}

MRB_API mrb_value
mrb_checkpoint_host_task(mrb_state *mrb)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (!g || !g->host_task) return mrb_nil_value();
  return ((mrb_task*)g->host_task)->self;
}
