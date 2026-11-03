/*
** checkpoint_dump.c - write the running state as a logical graph (format v1)
**
** Called at a safe point (see patches/0001-vm-safepoint-hook.patch): every
** frame's pc and registers are in memory, no exception is in flight and no
** C function is between the base frame and the current one.
**
** Roots are the VM (every execution context: the root, each Task's, each
** Fiber's, with their stacks and call frames), the scheduler (the Tasks in
** their queues, the clock) and the changes made to permanent objects since
** the baseline (methods, ivars and constants of permanent classes, ivars of
** top_self, globals).
*/

#include <stdio.h>
#include <stdarg.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/array.h>
#include <mruby/hash.h>
#include <mruby/range.h>
#include <mruby/error.h>
#include <mruby/variable.h>
#include <mruby/irep.h>
#include <mruby/data.h>
#include <mruby/internal.h>
#include <mruby/version.h>
#include <mruby/debug.h>
#include <mruby/numeric.h>
#include "checkpoint_internal.h"
#include "task.h"

typedef struct {
  mrb_state *mrb;
  checkpoint_state *G;
  checkpoint_perm *P;
  gbuf syms, ireps, objs, patches, vm;
  uint32_t nsym, nirep, nobj, npatch;
  pmap symid, irepid, objid;
  pmap body;        /* permanent Procs a frame runs: their irep is carried too */
  struct RBasic **oq; char **olabel; size_t oq_cap;
  const mrb_irep **iq; size_t iq_cap;
  /* execution contexts (v1): numbered as they are reached, the root first */
  gbuf ctxs, sched;
  pmap ctxid;
  struct mrb_context **cq; size_t cq_cap; uint32_t nctx;
  /* the Tasks: the queues in order, then the main Task (Task.current at the root) */
  mrb_task **tq; uint32_t ntask;
  pmap taskid;
  pmap queues;      /* Task::Queue struct -> index + 1 in qobj (only when a Task waits on one) */
  struct RBasic **qobj; uint32_t nqobj, qobj_cap;
  /* the file names the line numbers (DBG0) name, in their order (for SRC0) */
  mrb_sym *dfiles; uint32_t ndfiles;
  int failed;
  char *err; size_t errlen;
} dumper;

static void
fail(dumper *d, const char *fmt, ...)
{
  if (d->failed) return;
  d->failed = 1;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(d->err, d->errlen, fmt, ap);
  va_end(ap);
}

static const char*
class_name(mrb_state *mrb, struct RClass *c)
{
  if (!c) return "(none)";
  c = mrb_class_real(c);
  if (!c) return "(none)";
  const char *s = mrb_class_name(mrb, c);
  return s ? s : "(anonymous)";
}

static const char*
type_name(enum mrb_vtype tt)
{
  switch (tt) {
#define TN(t, type, name) case t: return name;
  MRB_VTYPE_FOREACH(TN)
#undef TN
  default: return "?";
  }
}

/* ---- references ---- */

static uint32_t
sym_ref(dumper *d, mrb_sym sym)
{
  uint32_t id;
  if (sym == 0) return 0;
  if (pm_get(&d->symid, (const void*)(uintptr_t)sym, &id)) return id;
  mrb_int len;
  const char *s = mrb_sym_name_len(d->mrb, sym, &len);
  gb_str(d->mrb, &d->syms, s, (size_t)len);
  id = ++d->nsym;
  pm_put(d->mrb, &d->symid, (const void*)(uintptr_t)sym, id);
  return id;
}

static uint32_t
irep_ref(dumper *d, const mrb_irep *irep)
{
  uint32_t id;
  if (!irep) return 0;
  if (pm_get(&d->irepid, irep, &id)) return id;
  id = ++d->nirep;
  if (id > d->iq_cap) {
    d->iq_cap = d->iq_cap ? d->iq_cap * 2 : 64;
    d->iq = (const mrb_irep**)mrb_realloc(d->mrb, d->iq, sizeof(void*) * d->iq_cap);
  }
  d->iq[id - 1] = irep;
  pm_put(d->mrb, &d->irepid, irep, id);
  return id;
}

static uint32_t
obj_ref(dumper *d, const void *p, const char *label)
{
  uint32_t id;
  if (!p) return 0;
  if (pm_get(&d->objid, p, &id)) return id;
  id = ++d->nobj;
  if (id > d->oq_cap) {
    d->oq_cap = d->oq_cap ? d->oq_cap * 2 : 256;
    d->oq = (struct RBasic**)mrb_realloc(d->mrb, d->oq, sizeof(void*) * d->oq_cap);
    d->olabel = (char**)mrb_realloc(d->mrb, d->olabel, sizeof(char*) * d->oq_cap);
  }
  d->oq[id - 1] = (struct RBasic*)p;
  size_t n = strlen(label);
  d->olabel[id - 1] = (char*)mrb_malloc(d->mrb, n + 1);
  memcpy(d->olabel[id - 1], label, n + 1);
  pm_put(d->mrb, &d->objid, p, id);
  return id;
}

static uint32_t
ctx_ref(dumper *d, struct mrb_context *c)
{
  uint32_t id;
  if (!c) return 0;
  if (pm_get(&d->ctxid, c, &id)) return id;
  id = ++d->nctx;
  if (id > d->cq_cap) {
    d->cq_cap = d->cq_cap ? d->cq_cap * 2 : 16;
    d->cq = (struct mrb_context**)mrb_realloc(d->mrb, d->cq, sizeof(void*) * d->cq_cap);
  }
  d->cq[id - 1] = c;
  pm_put(d->mrb, &d->ctxid, c, id);
  return id;
}

static void
w_value(dumper *d, gbuf *b, mrb_value v, const char *label)
{
  mrb_state *mrb = d->mrb;
  switch (mrb_type(v)) {
  case MRB_TT_FALSE:
    gb_u8(mrb, b, mrb_nil_p(v) ? GV_NIL : GV_FALSE);
    break;
  case MRB_TT_TRUE:
    gb_u8(mrb, b, GV_TRUE);
    break;
  case MRB_TT_INTEGER:
    gb_u8(mrb, b, GV_INT);
    gb_u64(mrb, b, (uint64_t)mrb_integer(v));
    break;
  case MRB_TT_FLOAT: {
    double f = (double)mrb_float(v);
    uint64_t u;
    memcpy(&u, &f, 8);
    gb_u8(mrb, b, GV_FLOAT);
    gb_u64(mrb, b, u);
    break;
  }
  case MRB_TT_SYMBOL:
    gb_u8(mrb, b, GV_SYM);
    gb_u32(mrb, b, sym_ref(d, mrb_symbol(v)));
    break;
  case MRB_TT_UNDEF:
    gb_u8(mrb, b, GV_UNDEF);
    break;
  case MRB_TT_CPTR:
  case MRB_TT_FREE:
    fail(d, "cannot carry a %s value (%s)", type_name(mrb_type(v)), label);
    gb_u8(mrb, b, GV_NIL);
    break;
  default:
    gb_u8(mrb, b, GV_OBJ);
    gb_u32(mrb, b, obj_ref(d, mrb_ptr(v), label));
    break;
  }
}

static void
w_ivars(dumper *d, gbuf *b, struct RBasic *o, uint32_t id)
{
  giv_entry *iv; int n;
  char label[160];
  checkpoint_collect_iv(d->mrb, mrb_obj_value(o), &iv, &n);
  gb_u32(d->mrb, b, (uint32_t)n);
  for (int i = 0; i < n; i++) {
    gb_u32(d->mrb, b, sym_ref(d, iv[i].sym));
    snprintf(label, sizeof(label), "ivar %s of #%u", mrb_sym_name(d->mrb, iv[i].sym), id);
    w_value(d, b, iv[i].val, label);
  }
  mrb_free(d->mrb, iv);
}

static uint8_t
vis_code(uint32_t flags)
{
  return (uint8_t)((flags >> 25) & 3);  /* 0 public, 1 private, 2 protected, 3 default */
}

static void
w_method(dumper *d, gbuf *b, struct RClass *c, mrb_sym sym, uint32_t flags, const void *ptr,
         const char *where)
{
  mrb_state *mrb = d->mrb;
  char label[200];
  gb_u32(mrb, b, sym_ref(d, sym));
  gb_u8(mrb, b, vis_code(flags));
  if (ptr == NULL) {
    gb_u8(mrb, b, GM_UNDEF);
  }
  else if (flags & MRB_METHOD_FUNC_FL) {
    const char *fname = checkpoint_perm_func_name(d->P, (mrb_func_t)ptr);
    if (!fname) {
      fail(d, "method %s of %s is a C function that has no name in the baseline",
           mrb_sym_name(mrb, sym), where);
      fname = "";
    }
    gb_u8(mrb, b, GM_FUNC);
    gb_str(mrb, b, fname, strlen(fname));
    gb_u32(mrb, b, flags & 0xffffff);
  }
  else {
    gb_u8(mrb, b, GM_PROC);
    snprintf(label, sizeof(label), "method %s of %s", mrb_sym_name(mrb, sym), where);
    gb_u32(mrb, b, obj_ref(d, ptr, label));
  }
}

static uint8_t
inst_type(enum mrb_vtype tt)
{
  switch (tt) {
  case MRB_TT_FALSE: return GI_NONE;
  case MRB_TT_OBJECT: return GI_OBJECT;
  case MRB_TT_EXCEPTION: return GI_EXCEPTION;
  case MRB_TT_STRING: return GI_STRING;
  case MRB_TT_ARRAY: return GI_ARRAY;
  case MRB_TT_HASH: return GI_HASH;
  case MRB_TT_RANGE: return GI_RANGE;
  case MRB_TT_PROC: return GI_PROC;
  case MRB_TT_CDATA: return GI_DATA;
  case MRB_TT_STRUCT: return GI_STRUCT;
  case MRB_TT_SET: return GI_SET;
  case MRB_TT_COMPLEX: return GI_COMPLEX;
  case MRB_TT_RATIONAL: return GI_RATIONAL;
  case MRB_TT_BIGINT: return GI_BIGINT;
  case MRB_TT_ISTRUCT: return GI_RANDOM;
  default: return GI_OTHER;
  }
}

static checkpoint_data_ent*
data_ent(checkpoint_state *G, const mrb_data_type *t)
{
  for (int i = 0; i < G->ndata; i++) {
    if (G->data[i].type == t) return &G->data[i];
  }
  return NULL;
}

static mrb_callinfo*
frame_with_env(struct mrb_context *c, struct REnv *e)
{
  if (!c->cibase || !c->ci) return NULL;
  for (mrb_callinfo *ci = c->cibase; ci <= c->ci; ci++) {
    if (mrb_vm_ci_env(ci) == e) return ci;
  }
  return NULL;
}

typedef struct { dumper *d; gbuf *b; uint32_t id; int i; } hash_w;

static int
hash_w_cb(mrb_state *mrb, mrb_value k, mrb_value v, void *p)
{
  hash_w *hw = (hash_w*)p;
  char label[96];
  snprintf(label, sizeof(label), "key %d of #%u", hw->i, hw->id);
  w_value(hw->d, hw->b, k, label);
  snprintf(label, sizeof(label), "value %d of #%u", hw->i, hw->id);
  w_value(hw->d, hw->b, v, label);
  hw->i++;
  return 0;
}

/* a Random (mruby-random), or an instance of a subclass */
static int
is_random(mrb_state *mrb, struct RBasic *o)
{
  if (o->tt != MRB_TT_ISTRUCT || !mrb_class_defined(mrb, "Random")) return 0;
  return mrb_obj_is_kind_of(mrb, mrb_obj_value(o), mrb_class_get(mrb, "Random"));
}

static mrb_value
set_to_a(mrb_state *mrb, void *o)
{
  return mrb_funcall_argv(mrb, mrb_obj_value(o), mrb_intern_lit(mrb, "to_a"), 0, NULL);
}

static void
w_object(dumper *d, uint32_t id)
{
  mrb_state *mrb = d->mrb;
  gbuf *b = &d->objs;
  struct RBasic *o = d->oq[id - 1];
  const char *reach = d->olabel[id - 1];
  char label[200];

  const char *pname = checkpoint_perm_name_of(d->P, o);
  if (pname) {
    if (pm_get(&d->body, o, NULL)) {
      /* a frame runs this mrblib method: its irep goes along (GR_PERM_CARRIED),
         so a reader whose mrblib differs can still run the frame */
      gb_u8(mrb, b, GK_PERM_BODY);
      gb_str(mrb, b, pname, strlen(pname));
      gb_u32(mrb, b, irep_ref(d, ((struct RProc*)o)->body.irep));
      return;
    }
    gb_u8(mrb, b, GK_PERM);
    gb_str(mrb, b, pname, strlen(pname));
    return;
  }
  if (pm_get(&d->P->heap, o, NULL)) {
    if (o->tt == MRB_TT_CLASS || o->tt == MRB_TT_MODULE) {
      fail(d, "object made by mrb_open has no name: %s %s (%s)",
           type_name(o->tt), class_name(mrb, (struct RClass*)o), reach);
    }
    else {
      fail(d, "object made by mrb_open has no name: %s of class %s (%s)",
           type_name(o->tt), class_name(mrb, o->c), reach);
    }
    gb_u8(mrb, b, GK_PERM);
    gb_str(mrb, b, "", 0);
    return;
  }

#define HEADER(kind) do { \
    gb_u8(mrb, b, (kind)); \
    snprintf(label, sizeof(label), "class of #%u", id); \
    gb_u32(mrb, b, obj_ref(d, o->c, label)); \
    gb_u8(mrb, b, (uint8_t)o->frozen); \
  } while (0)

  switch (o->tt) {
  case MRB_TT_OBJECT:
    HEADER(GK_OBJECT);
    w_ivars(d, b, o, id);
    break;

  case MRB_TT_STRING: {
    mrb_value s = mrb_obj_value(o);
    HEADER(RSTR_BINARY_P((struct RString*)o) ? GK_BSTRING : GK_STRING);
    gb_str(mrb, b, RSTRING_PTR(s), (size_t)RSTRING_LEN(s));
    break;
  }

  case MRB_TT_ARRAY: {
    mrb_value a = mrb_obj_value(o);
    mrb_int n = RARRAY_LEN(a);
    HEADER(GK_ARRAY);
    gb_u32(mrb, b, (uint32_t)n);
    for (mrb_int i = 0; i < n; i++) {
      snprintf(label, sizeof(label), "element %d of #%u", (int)i, id);
      w_value(d, b, RARRAY_PTR(a)[i], label);
    }
    break;
  }

  case MRB_TT_HASH: {
    mrb_value h = mrb_obj_value(o);
    uint8_t fl = 0;
    if (MRB_RHASH_DEFAULT_P(h)) fl |= 1;
    if (MRB_RHASH_PROCDEFAULT_P(h)) fl |= 2;
    HEADER(GK_HASH);
    gb_u8(mrb, b, fl);
    gb_u32(mrb, b, (uint32_t)mrb_hash_size(mrb, h));
    hash_w hw = { d, b, id, 0 };
    mrb_hash_foreach(mrb, mrb_hash_ptr(h), hash_w_cb, &hw);  /* no Ruby method is called */
    w_ivars(d, b, o, id);
    break;
  }

  case MRB_TT_RANGE: {
    struct RRange *r = (struct RRange*)o;
    HEADER(GK_RANGE);
    if (!(o->flags & 1)) {  /* RANGE_INITIALIZED_FLAG (range.c): Range.allocate, no edges yet */
      gb_u8(mrb, b, GV_NIL);
      gb_u8(mrb, b, GV_NIL);
      gb_u8(mrb, b, 2);
      break;
    }
    snprintf(label, sizeof(label), "begin of #%u", id);
    w_value(d, b, RANGE_BEG(r), label);
    snprintf(label, sizeof(label), "end of #%u", id);
    w_value(d, b, RANGE_END(r), label);
    gb_u8(mrb, b, (uint8_t)RANGE_EXCL(r));
    break;
  }

  case MRB_TT_EXCEPTION: {
    struct RException *e = (struct RException*)o;
    mrb_value bt = mrb_nil_value();
    if (e->backtrace) bt = mrb_exc_backtrace(mrb, mrb_obj_value(o)); /* packed -> Array */
    HEADER(GK_EXCEPTION);
    snprintf(label, sizeof(label), "message of #%u", id);
    w_value(d, b, e->mesg ? mrb_obj_value(e->mesg) : mrb_nil_value(), label);
    snprintf(label, sizeof(label), "backtrace of #%u", id);
    w_value(d, b, bt, label);
    w_ivars(d, b, o, id);
    break;
  }

  case MRB_TT_CLASS:
  case MRB_TT_MODULE:
  case MRB_TT_SCLASS: {
    struct RClass *c = (struct RClass*)o;
    HEADER(o->tt == MRB_TT_CLASS ? GK_CLASS : o->tt == MRB_TT_MODULE ? GK_MODULE : GK_SCLASS);
    snprintf(label, sizeof(label), "superclass of #%u", id);
    gb_u32(mrb, b, obj_ref(d, c->super, label));
    uint8_t it = inst_type(MRB_INSTANCE_TT(c));
    if (it == GI_OTHER) {
      fail(d, "class %s makes instances of type %s, not in format v1 (%s)",
           class_name(mrb, c), type_name(MRB_INSTANCE_TT(c)), reach);
    }
    gb_u8(mrb, b, it);
    gb_u8(mrb, b, (uint8_t)((MRB_UNDEF_ALLOCATOR_P(c) ? 1 : 0) |
                            ((c->flags & MRB_FL_CLASS_IS_INHERITED) ? 2 : 0) |
                            ((c->flags & MRB_FL_CLASS_IS_PREPENDED) ? 4 : 0) |
                            (c->mt ? 8 : 0)));
    w_ivars(d, b, o, id);
    gmt_entry *mt; int n;
    checkpoint_collect_mt(mrb, c, &mt, &n);
    gb_u32(mrb, b, (uint32_t)n);
    snprintf(label, sizeof(label), "#%u", id);
    for (int i = 0; i < n; i++) w_method(d, b, c, mt[i].sym, mt[i].flags, mt[i].ptr, label);
    mrb_free(mrb, mt);
    break;
  }

  case MRB_TT_ICLASS: {
    struct RClass *c = (struct RClass*)o;
    if (c->flags & MRB_FL_CLASS_IS_ORIGIN) {
      /* the origin of a class with prepended modules: holds the class's
         own methods (the class field is the class) */
      HEADER(GK_ORIGIN);
      snprintf(label, sizeof(label), "superclass of #%u", id);
      gb_u32(mrb, b, obj_ref(d, c->super, label));
      gb_u8(mrb, b, c->mt ? 1 : 0);
      gmt_entry *mt; int n;
      checkpoint_collect_mt(mrb, c, &mt, &n);
      gb_u32(mrb, b, (uint32_t)n);
      snprintf(label, sizeof(label), "#%u", id);
      for (int i = 0; i < n; i++) w_method(d, b, c, mt[i].sym, mt[i].flags, mt[i].ptr, label);
      mrb_free(mrb, mt);
      break;
    }
    HEADER(GK_ICLASS);  /* the class field is the included module */
    snprintf(label, sizeof(label), "superclass of #%u", id);
    gb_u32(mrb, b, obj_ref(d, c->super, label));
    break;
  }

  case MRB_TT_PROC: {
    struct RProc *p = (struct RProc*)o;
    uint8_t fl = 0;
    if (MRB_PROC_STRICT_P(p)) fl |= 1;
    if (MRB_PROC_ORPHAN_P(p)) fl |= 2;
    if (MRB_PROC_SCOPE_P(p)) fl |= 4;
    HEADER(GK_PROC);
    gb_u8(mrb, b, fl);
    if (MRB_PROC_ALIAS_P(p)) {
      /* alias_method of a Ruby method: the original's name, and the
         original Proc as upper */
      gb_u8(mrb, b, 2);
      gb_u32(mrb, b, sym_ref(d, p->body.mid));
    }
    else if (MRB_PROC_CFUNC_P(p)) {
      const char *fname = checkpoint_perm_func_name(d->P, p->body.func);
      if (!fname) {
        fail(d, "Proc made from a C function that has no name in the baseline (%s)", reach);
        fname = "";
      }
      gb_u8(mrb, b, 1);
      gb_str(mrb, b, fname, strlen(fname));
      gb_u32(mrb, b, MRB_PROC_NOARG_P(p) ? 0 : (mrb_proc_decompress_caspec(p->flags) | 0x80000000u));
    }
    else {
      gb_u8(mrb, b, 0);
      gb_u32(mrb, b, irep_ref(d, p->body.irep));
    }
    snprintf(label, sizeof(label), "upper of #%u", id);
    gb_u32(mrb, b, obj_ref(d, p->upper, label));
    if (MRB_PROC_ENV_P(p)) {
      gb_u8(mrb, b, 1);
      snprintf(label, sizeof(label), "env of #%u", id);
      gb_u32(mrb, b, obj_ref(d, p->e.env, label));
    }
    else if (p->e.target_class) {
      gb_u8(mrb, b, 2);
      snprintf(label, sizeof(label), "target class of #%u", id);
      gb_u32(mrb, b, obj_ref(d, p->e.target_class, label));
    }
    else {
      gb_u8(mrb, b, 0);
      gb_u32(mrb, b, 0);
    }
    break;
  }

  case MRB_TT_ENV: {
    struct REnv *e = (struct REnv*)o;
    HEADER(GK_ENV);  /* the class field is the target class */
    gb_u32(mrb, b, sym_ref(d, e->mid));
    gb_u8(mrb, b, (uint8_t)MRB_ENV_LEN(e));
    gb_u8(mrb, b, (uint8_t)MRB_ENV_BIDX(e));
    gb_u8(mrb, b, (uint8_t)((e->flags >> 16) & 0xf));
    if (MRB_ENV_ONSTACK_P(e)) {
      /* the registers of a frame still running in some context: the
         context and the position in its stack */
      struct mrb_context *c = e->cxt;
      if (!c || !c->stbase || e->stack < c->stbase || e->stack >= c->stend) {
        fail(d, "env on the stack of no live context (%s)", reach);
      }
      else if (!frame_with_env(c, e)) {
        fail(d, "env on the stack without a frame (%s)", reach);
      }
      gb_u8(mrb, b, 1);
      gb_u32(mrb, b, ctx_ref(d, c));
      gb_u32(mrb, b, c && c->stbase ? (uint32_t)(e->stack - c->stbase) : 0);
    }
    else {
      gb_u8(mrb, b, 0);
      for (mrb_int i = 0; i < MRB_ENV_LEN(e); i++) {
        snprintf(label, sizeof(label), "slot %d of env #%u", (int)i, id);
        w_value(d, b, e->stack[i], label);
      }
    }
    break;
  }

  case MRB_TT_BIGINT: {
    /* sign + hex digits of the absolute value */
    mrb_value str = mrb_integer_to_str(mrb, mrb_obj_value(o), 16);
    const char *p = RSTRING_PTR(str); mrb_int n = RSTRING_LEN(str);
    HEADER(GK_BIGINT);
    uint8_t neg = (n > 0 && p[0] == '-');
    gb_u8(mrb, b, neg);
    gb_str(mrb, b, p + neg, (size_t)(n - neg));
    break;
  }

  case MRB_TT_ISTRUCT: {
    if (!is_random(mrb, o)) {
      fail(d, "type istruct (class %s) is not in format v1 (%s)", class_name(mrb, o->c), reach);
      gb_u8(mrb, b, GK_PERM); gb_str(mrb, b, "", 0);
      break;
    }
    uint64_t st; uint32_t sd;
    checkpoint_random_get(mrb_obj_value(o), &st, &sd);
    HEADER(GK_RANDOM);
    gb_u64(mrb, b, st);
    gb_u32(mrb, b, sd);
    w_ivars(d, b, o, id);
    break;
  }

  case MRB_TT_STRUCT: {
    /* a Struct's values are an Array's body (mruby-struct); the member
       names are the class's */
    mrb_value a = mrb_obj_value(o);
    mrb_int n = RARRAY_LEN(a);
    HEADER(GK_STRUCT);
    gb_u32(mrb, b, (uint32_t)n);
    for (mrb_int i = 0; i < n; i++) {
      snprintf(label, sizeof(label), "member %d of #%u", (int)i, id);
      w_value(d, b, RARRAY_PTR(a)[i], label);
    }
    w_ivars(d, b, o, id);
    break;
  }

  case MRB_TT_SET: {
    /* the elements in order (Set#to_a, a C method); the reader adds them
       again after everything is built, like a Hash's keys */
    /* RSet embeds a khash whose first field is its table (mruby-set/src/set.c,
       mruby/khash.h): NULL until Set#initialize ran (Set.allocate) */
    struct { MRB_OBJECT_HEADER; void *data; } *rs = (void*)o;
    HEADER(GK_SET);
    if (rs->data == NULL) {
      gb_u32(mrb, b, UINT32_MAX);
      w_ivars(d, b, o, id);
      break;
    }
    mrb_value ary = set_to_a(mrb, o);
    if (!mrb_array_p(ary)) { fail(d, "Set#to_a did not give an Array (%s)", reach); break; }
    gb_u32(mrb, b, (uint32_t)RARRAY_LEN(ary));
    for (mrb_int i = 0; i < RARRAY_LEN(ary); i++) {
      snprintf(label, sizeof(label), "element %d of #%u", (int)i, id);
      w_value(d, b, RARRAY_PTR(ary)[i], label);
    }
    w_ivars(d, b, o, id);
    break;
  }

  case MRB_TT_RATIONAL: {
    /* numerator and denominator as decimal text (either may be a bigint) */
    mrb_value nu = mrb_funcall_argv(mrb, mrb_obj_value(o), mrb_intern_lit(mrb, "numerator"), 0, NULL);
    mrb_value de = mrb_funcall_argv(mrb, mrb_obj_value(o), mrb_intern_lit(mrb, "denominator"), 0, NULL);
    HEADER(GK_RATIONAL);
    mrb_value ns = mrb_integer_to_str(mrb, nu, 10), ds = mrb_integer_to_str(mrb, de, 10);
    gb_str(mrb, b, RSTRING_PTR(ns), (size_t)RSTRING_LEN(ns));
    gb_str(mrb, b, RSTRING_PTR(ds), (size_t)RSTRING_LEN(ds));
    break;
  }

  case MRB_TT_COMPLEX: {
    mrb_value re = mrb_funcall_argv(mrb, mrb_obj_value(o), mrb_intern_lit(mrb, "real"), 0, NULL);
    mrb_value im = mrb_funcall_argv(mrb, mrb_obj_value(o), mrb_intern_lit(mrb, "imaginary"), 0, NULL);
    HEADER(GK_COMPLEX);
    double fr = mrb_float_p(re) ? mrb_float(re) : (double)mrb_as_int(mrb, re);
    double fi = mrb_float_p(im) ? mrb_float(im) : (double)mrb_as_int(mrb, im);
    uint64_t u;
    memcpy(&u, &fr, 8); gb_u64(mrb, b, u);
    memcpy(&u, &fi, 8); gb_u64(mrb, b, u);
    break;
  }

  case MRB_TT_BREAK: {
    /* a break / jump / stop going through an ensure clause: its kind (the
       VM's tag: 0 break, 1 jump, 2 stop), the frame it ends at, its value */
    struct RBreak *brk = (struct RBreak*)o;
    HEADER(GK_BREAK);
    gb_u8(mrb, b, (uint8_t)((o->flags >> 8) & 7));
    gb_u32(mrb, b, (uint32_t)brk->ci_break_index);
    snprintf(label, sizeof(label), "value of break #%u", id);
    w_value(d, b, mrb_break_value_get(brk), label);
    break;
  }

  case MRB_TT_FIBER: {
    /* its execution context, in the context table */
    struct RFiber *f = (struct RFiber*)o;
    HEADER(GK_FIBER);
    gb_u32(mrb, b, ctx_ref(d, f->cxt));
    break;
  }

  case MRB_TT_CDATA: {
    struct RData *rd = (struct RData*)o;
    if (rd->type && strcmp(rd->type->struct_name, "Task") == 0 &&
        mrb_obj_is_kind_of(mrb, mrb_obj_value(o), mrb_class_get(mrb, "Task"))) {
      /* a Task: its record in the scheduler section */
      uint32_t ti = 0;
      if (!rd->data || !pm_get(&d->taskid, rd->data, &ti)) {
        fail(d, "a Task that is in no queue of the scheduler (%s)", reach);
      }
      HEADER(GK_TASK);
      gb_u32(mrb, b, ti);
      w_ivars(d, b, o, id);
      break;
    }
    if (!rd->type && !rd->data) {
      /* allocated, not initialized yet (Regexp.allocate ...): no type, no
         pointer; carried as such (type name "") */
      HEADER(GK_DATA);
      gb_str(mrb, b, "", 0);
      gb_str(mrb, b, "", 0);
      w_ivars(d, b, o, id);
      break;
    }
    checkpoint_data_ent *de = rd->type ? data_ent(d->G, rd->type) : NULL;
    if (!de) {
      fail(d, "C data of class %s (type '%s') has no entry in the checkpoint data table (%s)",
           class_name(mrb, o->c), rd->type ? rd->type->struct_name : "untyped", reach);
      gb_u8(mrb, b, GK_PERM); gb_str(mrb, b, "", 0);
      break;
    }
    HEADER(GK_DATA);
    gb_str(mrb, b, de->name, strlen(de->name));
    gbuf blob; gb_init(&blob);
    if (de->dump(mrb, mrb_obj_value(o), &blob) != 0) {
      fail(d, "C data of class %s refused to be dumped (%s)", class_name(mrb, o->c), reach);
    }
    gb_str(mrb, b, (const char*)blob.p, blob.len);
    gb_free(mrb, &blob);
    w_ivars(d, b, o, id);
    break;
  }

  default:
    fail(d, "type %s (class %s) is not in format v1 (%s)",
         type_name(o->tt), class_name(mrb, o->c), reach);
    gb_u8(mrb, b, GK_PERM); gb_str(mrb, b, "", 0);
    break;
  }
#undef HEADER
}

static void
w_irep(dumper *d, uint32_t id)
{
  mrb_state *mrb = d->mrb;
  gbuf *b = &d->ireps;
  const mrb_irep *irep = d->iq[id - 1];
  uint64_t h;

  /* A permanent irep (mrblib) is in the table only because live code needs it:
     a frame runs it or a Proc made at run time holds it. Its hash binds it on a
     reader with the same mrblib; its content goes along for a reader whose
     mrblib differs (another build, another VM). Its children are reached
     through the content and are carried the same way. */
  if (checkpoint_perm_irep(d->P, irep, &h)) {
    gb_u8(mrb, b, GR_PERM_CARRIED);
    gb_u64(mrb, b, h);
  }
  else {
    gb_u8(mrb, b, GR_CARRIED);
    gb_u64(mrb, b, checkpoint_irep_hash(mrb, irep));
  }
  gb_u16(mrb, b, irep->nlocals);
  gb_u16(mrb, b, irep->nregs);
  gb_u16(mrb, b, irep->clen);
  gb_u32(mrb, b, irep->ilen);
  gb_bytes(mrb, b, irep->iseq, irep->ilen + sizeof(struct mrb_irep_catch_handler) * irep->clen);
  gb_u16(mrb, b, irep->plen);
  for (int i = 0; i < irep->plen; i++) {
    const mrb_irep_pool *p = &irep->pool[i];
    switch (p->tt) {
    case IREP_TT_INT32:
      gb_u8(mrb, b, 1); gb_u64(mrb, b, (uint64_t)(int64_t)p->u.i32); break;
    case IREP_TT_INT64:
      gb_u8(mrb, b, 1); gb_u64(mrb, b, (uint64_t)p->u.i64); break;
    case IREP_TT_FLOAT: {
      double f = p->u.f; uint64_t u; memcpy(&u, &f, 8);
      gb_u8(mrb, b, 2); gb_u64(mrb, b, u); break;
    }
    case IREP_TT_BIGINT: {
      size_t n = (uint8_t)p->u.str[0] + 2;
      gb_u8(mrb, b, 3); gb_str(mrb, b, p->u.str, n); break;
    }
    default:
      gb_u8(mrb, b, 0); gb_str(mrb, b, p->u.str, p->tt >> 2); break;
    }
  }
  gb_u16(mrb, b, irep->slen);
  for (int i = 0; i < irep->slen; i++) gb_u32(mrb, b, irep->syms ? sym_ref(d, irep->syms[i]) : 0);
  gb_u16(mrb, b, irep->rlen);
  for (int i = 0; i < irep->rlen; i++) gb_u32(mrb, b, irep->reps ? irep_ref(d, irep->reps[i]) : 0);
  if (irep->lv) {
    gb_u16(mrb, b, (uint16_t)(irep->nlocals > 0 ? irep->nlocals - 1 : 0));
    for (int i = 0; i + 1 < irep->nlocals; i++) gb_u32(mrb, b, sym_ref(d, irep->lv[i]));
  }
  else {
    gb_u16(mrb, b, 0);
  }
}

/* ---- changes to permanent objects ---- */

static int
same_value(mrb_value a, mrb_value b)
{
  if (mrb_type(a) != mrb_type(b)) return 0;
  switch (mrb_type(a)) {
  case MRB_TT_FALSE: case MRB_TT_TRUE: return mrb_nil_p(a) == mrb_nil_p(b);
  case MRB_TT_INTEGER: return mrb_integer(a) == mrb_integer(b);
  case MRB_TT_FLOAT: {
    mrb_float x = mrb_float(a), y = mrb_float(b);
    return memcmp(&x, &y, sizeof(mrb_float)) == 0;
  }
  case MRB_TT_SYMBOL: return mrb_symbol(a) == mrb_symbol(b);
  case MRB_TT_UNDEF: return 1;
  default: return mrb_ptr(a) == mrb_ptr(b);
  }
}

static void
diff_iv(dumper *d, struct RBasic *o, giv_entry *old, int nold, giv_entry *cur, int ncur, const char *name)
{
  mrb_state *mrb = d->mrb;
  char label[200];
  for (int i = 0; i < ncur; i++) {
    int j;
    for (j = 0; j < nold; j++) if (old[j].sym == cur[i].sym) break;
    if (j < nold && same_value(old[j].val, cur[i].val)) continue;
    gb_u8(mrb, &d->patches, GP_IV_SET);
    snprintf(label, sizeof(label), "permanent %s", name);
    gb_u32(mrb, &d->patches, obj_ref(d, o, label));
    gb_u32(mrb, &d->patches, sym_ref(d, cur[i].sym));
    snprintf(label, sizeof(label), "%s of %s", mrb_sym_name(mrb, cur[i].sym), name);
    w_value(d, &d->patches, cur[i].val, label);
    d->npatch++;
  }
  for (int j = 0; j < nold; j++) {
    int i;
    for (i = 0; i < ncur; i++) if (old[j].sym == cur[i].sym) break;
    if (i < ncur) continue;
    gb_u8(mrb, &d->patches, GP_IV_REMOVE);
    snprintf(label, sizeof(label), "permanent %s", name);
    gb_u32(mrb, &d->patches, obj_ref(d, o, label));
    gb_u32(mrb, &d->patches, sym_ref(d, old[j].sym));
    d->npatch++;
  }
}

static void
w_patches(dumper *d)
{
  mrb_state *mrb = d->mrb;
  checkpoint_perm *P = d->P;
  char label[200];

  /* the C side's GC roots (mrb_gc_register: the Tasks, a host's own values)
     are not the program's: the reader's scheduler and host register theirs */
  mrb_sym gc_root = mrb_intern_lit(mrb, "_gc_root_");
  mrb_value roots = mrb_gv_get(mrb, gc_root);
  for (uint32_t k = 0; k < P->nsnaps && !d->failed; k++) {
    gsnap *s = &P->snaps[k];
    struct RBasic *o = s->obj;
    if (mrb_array_p(roots) && o == mrb_basic_ptr(roots)) continue;
    const char *name = checkpoint_perm_name_of(P, o);
    snprintf(label, sizeof(label), "permanent %s", name);

    switch (o->tt) {
    case MRB_TT_CLASS: case MRB_TT_MODULE: case MRB_TT_SCLASS: {
      struct RClass *c = (struct RClass*)o;
      gmt_entry *mt; int n;
      checkpoint_collect_mt(mrb, c, &mt, &n);
      for (int i = 0; i < n; i++) {
        int j;
        for (j = 0; j < s->nmt; j++) if (s->mt[j].sym == mt[i].sym) break;
        if (j < s->nmt && s->mt[j].flags == mt[i].flags && s->mt[j].ptr == mt[i].ptr) continue;
        gb_u8(mrb, &d->patches, GP_METHOD_SET);
        gb_u32(mrb, &d->patches, obj_ref(d, o, label));
        w_method(d, &d->patches, c, mt[i].sym, mt[i].flags, mt[i].ptr, name);
        d->npatch++;
      }
      for (int j = 0; j < s->nmt; j++) {
        int i;
        for (i = 0; i < n; i++) if (s->mt[j].sym == mt[i].sym) break;
        if (i < n) continue;
        gb_u8(mrb, &d->patches, GP_METHOD_REMOVE);
        gb_u32(mrb, &d->patches, obj_ref(d, o, label));
        gb_u32(mrb, &d->patches, sym_ref(d, s->mt[j].sym));
        d->npatch++;
      }
      mrb_free(mrb, mt);
    }
      /* fall through */
    case MRB_TT_ICLASS: {
      struct RClass *c = (struct RClass*)o;
      if (c->super != s->super) {
        gb_u8(mrb, &d->patches, GP_SUPER_SET);
        gb_u32(mrb, &d->patches, obj_ref(d, o, label));
        gb_u32(mrb, &d->patches, obj_ref(d, c->super, label));
        d->npatch++;
      }
      if (o->tt != MRB_TT_ICLASS && (c->flags & MRB_FL_CLASS_IS_PREPENDED) != s->cflags) {
        gb_u8(mrb, &d->patches, GP_CLASS_FLAGS);
        gb_u32(mrb, &d->patches, obj_ref(d, o, label));
        gb_u8(mrb, &d->patches, (c->flags & MRB_FL_CLASS_IS_PREPENDED) ? 1 : 0);
        d->npatch++;
      }
      if (o->tt == MRB_TT_ICLASS) break;
    }
      /* fall through */
    case MRB_TT_OBJECT: case MRB_TT_EXCEPTION: case MRB_TT_CDATA: {
      giv_entry *iv; int n;
      checkpoint_collect_iv(mrb, mrb_obj_value(o), &iv, &n);
      diff_iv(d, o, s->iv, s->niv, iv, n, name);  /* refers to o only if something changed */
      mrb_free(mrb, iv);
      break;
    }
    case MRB_TT_ISTRUCT:
      if (checkpoint_content_digest(mrb, o) != s->digest) {
        if (!is_random(mrb, o)) {
          fail(d, "permanent %s (inline C struct of class %s) was modified", name, class_name(mrb, o->c));
          break;
        }
        uint64_t st; uint32_t sd;
        checkpoint_random_get(mrb_obj_value(o), &st, &sd);
        gb_u8(mrb, &d->patches, GP_RANDOM_SET);
        gb_u32(mrb, &d->patches, obj_ref(d, o, label));
        gb_u64(mrb, &d->patches, st);
        gb_u32(mrb, &d->patches, sd);
        d->npatch++;
      }
      break;
    case MRB_TT_STRING: case MRB_TT_ARRAY: case MRB_TT_HASH:
      if (checkpoint_content_digest(mrb, o) != s->digest) {
        /* the whole contents again, as in the object record (and frozen) */
        mrb_value v = mrb_obj_value(o);
        gb_u8(mrb, &d->patches, GP_CONTENT_SET);
        gb_u32(mrb, &d->patches, obj_ref(d, o, label));
        gb_u8(mrb, &d->patches, (uint8_t)o->frozen);
        gb_u8(mrb, &d->patches, o->tt == MRB_TT_STRING ? GK_STRING : o->tt == MRB_TT_ARRAY ? GK_ARRAY : GK_HASH);
        if (o->tt == MRB_TT_STRING) {
          gb_str(mrb, &d->patches, RSTRING_PTR(v), (size_t)RSTRING_LEN(v));
        }
        else if (o->tt == MRB_TT_ARRAY) {
          gb_u32(mrb, &d->patches, (uint32_t)RARRAY_LEN(v));
          for (mrb_int i = 0; i < RARRAY_LEN(v); i++) {
            snprintf(label, sizeof(label), "element %d of permanent %s", (int)i, name);
            w_value(d, &d->patches, RARRAY_PTR(v)[i], label);
          }
        }
        else {
          gb_u32(mrb, &d->patches, (uint32_t)mrb_hash_size(mrb, v));
          hash_w hw = { d, &d->patches, 0, 0 };
          mrb_hash_foreach(mrb, mrb_hash_ptr(v), hash_w_cb, &hw);
        }
        d->npatch++;
      }
      if (o->tt != MRB_TT_HASH) break;
      {
        /* a Hash's ivars (its default value) */
        giv_entry *iv; int n;
        checkpoint_collect_iv(mrb, mrb_obj_value(o), &iv, &n);
        diff_iv(d, o, s->iv, s->niv, iv, n, name);
        mrb_free(mrb, iv);
      }
      break;
    default:
      break;
    }
  }

  /* globals */
  giv_entry *gv; int ngv;
  checkpoint_collect_gv(mrb, &gv, &ngv);
  for (int i = 0; i < ngv; i++) {
    int j;
    if (gv[i].sym == gc_root) continue;
    for (j = 0; j < P->ngv; j++) if (P->gv[j].sym == gv[i].sym) break;
    if (j < P->ngv && same_value(P->gv[j].val, gv[i].val)) continue;
    gb_u8(mrb, &d->patches, GP_GV_SET);
    gb_u32(mrb, &d->patches, sym_ref(d, gv[i].sym));
    snprintf(label, sizeof(label), "global %s", mrb_sym_name(mrb, gv[i].sym));
    w_value(d, &d->patches, gv[i].val, label);
    d->npatch++;
  }
  for (int j = 0; j < P->ngv; j++) {
    int i;
    if (P->gv[j].sym == gc_root) continue;
    for (i = 0; i < ngv; i++) if (P->gv[j].sym == gv[i].sym) break;
    if (i < ngv) continue;
    gb_u8(mrb, &d->patches, GP_GV_REMOVE);
    gb_u32(mrb, &d->patches, sym_ref(d, P->gv[j].sym));
    d->npatch++;
  }
  mrb_free(mrb, gv);
}

/* ---- the VM: execution contexts ---- */

/* The C functions a frame may wait in: they only switch the context, and
   the value they return is written by whoever switches back (Fiber#resume,
   Fiber#transfer, Fiber.yield), or they are the scheduler's loop, which the
   reader starts again from the state of the queues (Task.run, Task.pass). */
static const char *const switch_funcs[] = {
  "F:Fiber#resume", "F:Fiber#transfer", "F:S:Fiber#yield",
  "F:S:Task#run", "F:S:Task#pass",
};

/* the name of the C function a frame runs (NULL when it has none) */
static const char*
cframe_func_name(dumper *d, struct mrb_context *c, mrb_callinfo *ci)
{
  mrb_state *mrb = d->mrb;
  const struct RProc *p = ci->proc;
  if (p && MRB_PROC_CFUNC_P(p)) return checkpoint_perm_func_name(d->P, p->body.func);
  if (p || !ci->mid) return NULL;
  /* a method stored as a function: found again from the receiver */
  struct RClass *k = ci->stack < c->stend ? mrb_class(mrb, ci->stack[0]) : NULL;
  if (!k) return NULL;
  mrb_method_t m = mrb_method_search_vm(mrb, &k, ci->mid);
  if (MRB_METHOD_UNDEF_P(m)) return NULL;
  if (MRB_METHOD_FUNC_P(m)) return checkpoint_perm_func_name(d->P, MRB_METHOD_FUNC(m));
  const struct RProc *mp = MRB_METHOD_PROC(m);
  if (mp && MRB_PROC_CFUNC_P(mp)) return checkpoint_perm_func_name(d->P, mp->body.func);
  return NULL;
}

static int
switch_func_p(const char *name)
{
  if (!name) return 0;
  for (size_t i = 0; i < sizeof(switch_funcs) / sizeof(switch_funcs[0]); i++) {
    if (strcmp(name, switch_funcs[i]) == 0) return 1;
  }
  return 0;
}

/* a context with no frame: the root of a host that runs the scheduler
   from C (mrb_task_run: the root is idle, its first frame is what the last
   run at the top left there), or a finished Fiber */
static int
no_frames_p(mrb_state *mrb, struct mrb_context *c)
{
  if (!c->cibase || !c->ci) return 1;
  if (c == mrb->root_c) return c != mrb->c && c->ci == c->cibase;
  return 0;
}

static void
w_context(dumper *d, uint32_t id)
{
  mrb_state *mrb = d->mrb;
  struct mrb_context *c = d->cq[id - 1];
  gbuf *b = &d->ctxs;
  char label[200];

  gb_u8(mrb, b, (uint8_t)c->status);
  gb_u8(mrb, b, c->vmexec ? 1 : 0);
  gb_u32(mrb, b, ctx_ref(d, c->prev));
  snprintf(label, sizeof(label), "fiber of context %u", id);
  gb_u32(mrb, b, obj_ref(d, c->fib, label));

  if (no_frames_p(mrb, c)) {
    gb_u32(mrb, b, 0);   /* stack */
    gb_u32(mrb, b, 0);   /* frames */
    return;
  }

  mrb_callinfo *top = c->ci;
  size_t live = (size_t)(top->stack - c->stbase) + (size_t)mrb_ci_nregs(top);
  if (c->stbase + live > c->stend) live = (size_t)(c->stend - c->stbase);
  /* dead registers (never read again from where the frames stand) go as nil */
  uint8_t *keep = (uint8_t*)mrb_malloc(mrb, live + 1);
  if (d->G->keep_dead) memset(keep, 1, live);
  else checkpoint_live_slots(mrb, c, live, keep);
  gb_u32(mrb, b, (uint32_t)live);
  for (size_t i = 0; i < live; i++) {
    if (!keep[i]) {
      if (!mrb_nil_p(c->stbase[i])) d->G->last_dead++;
      gb_u8(mrb, b, GV_NIL);
      continue;
    }
    snprintf(label, sizeof(label), "stack slot %d of context %u", (int)i, id);
    w_value(d, b, c->stbase[i], label);
  }
  mrb_free(mrb, keep);

  uint32_t nframes = (uint32_t)(top - c->cibase) + 1;
  gb_u32(mrb, b, nframes);
  for (uint32_t k = 0; k < nframes; k++) {
    mrb_callinfo *ci = c->cibase + k;
    const struct RProc *p = ci->proc;
    const char *mname = ci->mid ? mrb_sym_name(mrb, ci->mid) : "?";
    if (ci->cci != 0) {
      fail(d, "frame %u (%s) of context %u was entered from C: the C frame cannot be carried",
           k, mname, id);
      return;
    }
    if (!p || MRB_PROC_CFUNC_P(p)) {
      const char *fname = cframe_func_name(d, c, ci);
      if (ci != top || c == mrb->c) {
        fail(d, "frame %u (%s) of context %u is a C function; not a safe point", k, mname, id);
        return;
      }
      if (!switch_func_p(fname)) {
        fail(d, "context %u waits in the C function %s: its C frame cannot be carried",
             id, fname ? fname : mname);
        return;
      }
      gb_u8(mrb, b, GF_CFUNC);
      gb_u32(mrb, b, sym_ref(d, ci->mid));
      snprintf(label, sizeof(label), "proc of frame %u of context %u", k, id);
      gb_u32(mrb, b, obj_ref(d, p, label));
      gb_str(mrb, b, fname, strlen(fname));
    }
    else {
      if (ci->pc && (ci->pc < p->body.irep->iseq || ci->pc > p->body.irep->iseq + p->body.irep->ilen)) {
        fail(d, "frame %u (%s) of context %u stands outside its code (the VM ran past it)", k, mname, id);
        return;
      }
      if (!MRB_PROC_ALIAS_P(p) && checkpoint_perm_name_of(d->P, p) &&
          checkpoint_perm_irep(d->P, p->body.irep, NULL)) {
        pm_put(mrb, &d->body, p, 1);
      }
      gb_u8(mrb, b, GF_RUBY);
      gb_u32(mrb, b, sym_ref(d, ci->mid));
      snprintf(label, sizeof(label), "proc of frame %u of context %u", k, id);
      gb_u32(mrb, b, obj_ref(d, p, label));
    }
    gb_u32(mrb, b, (uint32_t)(ci->stack - c->stbase));
    gb_u32(mrb, b, (ci->pc && p && !MRB_PROC_CFUNC_P(p)) ? (uint32_t)(ci->pc - p->body.irep->iseq) : UINT32_MAX);
    gb_u8(mrb, b, ci->n);
    gb_u8(mrb, b, ci->nk);
    gb_u8(mrb, b, ci->cci);
    gb_u8(mrb, b, ci->vis);
    snprintf(label, sizeof(label), "block of frame %u of context %u", k, id);
    gb_u32(mrb, b, obj_ref(d, ci->blk, label));
    struct REnv *e = mrb_vm_ci_env(ci);
    if (e) {
      gb_u8(mrb, b, 1);
      snprintf(label, sizeof(label), "env of frame %u of context %u", k, id);
      gb_u32(mrb, b, obj_ref(d, e, label));
    }
    else if (ci->u.target_class) {
      gb_u8(mrb, b, 2);
      snprintf(label, sizeof(label), "target class of frame %u of context %u", k, id);
      gb_u32(mrb, b, obj_ref(d, ci->u.target_class, label));
    }
    else {
      gb_u8(mrb, b, 0);
      gb_u32(mrb, b, 0);
    }
  }
}

/* ---- the scheduler (mruby-task) ---- */

static int
queue_obj_cb(mrb_state *mrb, struct RBasic *obj, void *ud)
{
  dumper *d = (dumper*)ud;
  if (obj->tt == MRB_TT_CDATA) {
    struct RData *rd = (struct RData*)obj;
    if (rd->type && rd->data && strcmp(rd->type->struct_name, "Task::Queue") == 0 &&
        !pm_get(&d->queues, rd->data, NULL)) {
      if (d->nqobj == d->qobj_cap) {
        d->qobj_cap = d->qobj_cap ? d->qobj_cap * 2 : 8;
        d->qobj = (struct RBasic**)mrb_realloc(mrb, d->qobj, sizeof(void*) * d->qobj_cap);
      }
      d->qobj[d->nqobj++] = obj;
      pm_put(mrb, &d->queues, rd->data, d->nqobj);
    }
  }
  return MRB_EACH_OBJ_OK;
}

static void
w_remaining(dumper *d, gbuf *b, uint32_t deadline)
{
  gb_u32(d->mrb, b, (uint32_t)(int32_t)(deadline - d->mrb->task.tick));
}

static void
w_sched(dumper *d)
{
  mrb_state *mrb = d->mrb;
  gbuf *b = &d->sched;
  char label[200];

  uint8_t fl = 0;
  if (d->G->dump_switch) fl |= 1;
  if (mrb->task.loop_running) fl |= 2;
  gb_u8(mrb, b, fl);
  gb_u16(mrb, b, MRB_TICK_UNIT);
  gb_u32(mrb, b, mrb->task.tick);
  if (mrb->task.wakeup_tick == UINT32_MAX) {
    gb_u8(mrb, b, 0);
    gb_u32(mrb, b, 0);
  }
  else {
    gb_u8(mrb, b, 1);
    w_remaining(d, b, mrb->task.wakeup_tick);
  }

  /* the Task::Queue objects the waiting Tasks name by their struct */
  int need_queues = 0;
  for (uint32_t i = 0; i < d->ntask; i++) {
    if (d->tq[i]->reason == MRB_TASK_REASON_QUEUE) need_queues = 1;
  }
  if (need_queues) mrb_objspace_each_objects(mrb, queue_obj_cb, d);

  uint32_t running = 0;
  mrb_task *run = checkpoint_running_task(mrb);
  if (run && !pm_get(&d->taskid, run, &running)) running = 0;
  gb_u32(mrb, b, d->ntask);
  for (uint32_t i = 0; i < d->ntask && !d->failed; i++) {
    mrb_task *t = d->tq[i];
    int is_main = (t == mrb->task.main_task);
    snprintf(label, sizeof(label), "Task #%u", i + 1);
    gb_u32(mrb, b, obj_ref(d, mrb_obj_ptr(t->self), label));
    gb_u32(mrb, b, is_main ? 0 : ctx_ref(d, &t->c));
    gb_u8(mrb, b, t->status);
    uint8_t q = 255;
    if (!is_main) {
      switch (t->status) {
      case MRB_TASK_STATUS_READY: case MRB_TASK_STATUS_RUNNING: q = MRB_TASK_QUEUE_READY; break;
      case MRB_TASK_STATUS_WAITING: q = MRB_TASK_QUEUE_WAITING; break;
      case MRB_TASK_STATUS_SUSPENDED: q = MRB_TASK_QUEUE_SUSPENDED; break;
      default: q = MRB_TASK_QUEUE_DORMANT; break;
      }
      if (t->status == MRB_TASK_STATUS_RUNNING && running != i + 1) {
        fail(d, "two Tasks are running");
      }
    }
    gb_u8(mrb, b, q);
    gb_u8(mrb, b, t->priority);
    gb_u8(mrb, b, t->reason);
    gb_u8(mrb, b, t->timeslice);
    if (is_main) {
      /* mruby-task keeps the main Task's name and value alive through
         nothing (it is in no queue; its object holds them by a pointer the
         GC does not follow), so they may be gone: not read. The reader
         names it "main", as Task.current does. */
      gb_u8(mrb, b, GV_NIL);
      gb_u8(mrb, b, GV_NIL);
    }
    else {
      snprintf(label, sizeof(label), "name of Task #%u", i + 1);
      w_value(d, b, t->name, label);
      snprintf(label, sizeof(label), "value of Task #%u", i + 1);
      w_value(d, b, t->result, label);
    }
    switch (t->reason) {
    case MRB_TASK_REASON_NONE:
      break;
    case MRB_TASK_REASON_SLEEP:
      w_remaining(d, b, t->wait.wakeup_tick);
      break;
    case MRB_TASK_REASON_JOIN: {
      uint32_t ji = 0;
      if (t->wait.join && !pm_get(&d->taskid, t->wait.join, &ji) &&
          t->status != MRB_TASK_STATUS_DORMANT) {
        fail(d, "Task #%u joins a Task that is in no queue", i + 1);
      }
      gb_u32(mrb, b, ji);
      break;
    }
    case MRB_TASK_REASON_QUEUE: {
      uint32_t qi = 0;
      /* (a finished Task keeps the reason it last waited for: its queue may be gone) */
      if (t->wait.queue.target && !pm_get(&d->queues, t->wait.queue.target, &qi) &&
          t->status != MRB_TASK_STATUS_DORMANT) {
        fail(d, "Task #%u waits on a Task::Queue that is not alive", i + 1);
      }
      snprintf(label, sizeof(label), "the Task::Queue Task #%u waits on", i + 1);
      gb_u32(mrb, b, qi ? obj_ref(d, d->qobj[qi - 1], label) : 0);
      if (t->wait.queue.wakeup_tick == UINT32_MAX) {
        gb_u8(mrb, b, 0);
        gb_u32(mrb, b, 0);
      }
      else {
        gb_u8(mrb, b, 1);
        w_remaining(d, b, t->wait.queue.wakeup_tick);
      }
      break;
    }
    default:
      fail(d, "Task #%u waits for reason %u, not in format v1", i + 1, t->reason);
      break;
    }
  }

  /* the running Task: the context it was entered from runs the scheduler's
     loop (Task.run), one step of it (Task.pass) or nothing (a host that
     called mrb_task_run from C) */
  if (running && !d->failed) {
    mrb_task *t = d->tq[running - 1];
    struct mrb_context *e = t->c.prev;
    if (!e) {
      fail(d, "the running Task has no context it was entered from");
    }
    else if (no_frames_p(mrb, e)) {
      if (e != mrb->root_c) fail(d, "the running Task was entered from a context with no frame");
    }
    else {
      const char *fname = (!e->ci->proc || MRB_PROC_CFUNC_P(e->ci->proc)) ? cframe_func_name(d, e, e->ci) : NULL;
      if (!fname || (strcmp(fname, "F:S:Task#run") != 0 && strcmp(fname, "F:S:Task#pass") != 0)) {
        fail(d, "the scheduler runs from the C function %s: its C frame cannot be carried",
             fname ? fname : "(unknown)");
      }
    }
  }
  gb_u32(mrb, b, running);
  uint32_t host = 0;
  if (d->G->host_task) pm_get(&d->taskid, d->G->host_task, &host);
  gb_u32(mrb, b, host);
}

/* ---- debug info (line numbers), the optional last section ---- */

/* the files and (pc, line) changes of a carried irep's debug info: the
   program's ireps and the permanent ones (mrblib) alike, so that a reader
   with another mrblib, which builds them from their contents, has their
   lines too */
static int
w_debug_irep(dumper *d, gbuf *b, uint32_t id)
{
  mrb_state *mrb = d->mrb;
  const mrb_irep *irep = d->iq[id - 1];
  const mrb_irep_debug_info *di = irep->debug_info;
  if (!di || di->flen == 0) return 0;
  for (int i = 0; i < di->flen; i++) {
    if (di->files[i]->line_type != mrb_debug_line_packed_map) return 0;
  }
  gb_u32(mrb, b, id);
  gb_u16(mrb, b, di->flen);
  for (int i = 0; i < di->flen; i++) {
    const mrb_irep_debug_info_file *f = di->files[i];
    uint32_t end = i + 1 < di->flen ? di->files[i + 1]->start_pos : di->pc_count;
    mrb_int flen;
    const char *fn = mrb_sym_name_len(mrb, f->filename_sym, &flen);
    gb_str(mrb, b, fn, (size_t)flen);
    uint32_t k = 0;
    while (k < d->ndfiles && d->dfiles[k] != f->filename_sym) k++;
    if (k == d->ndfiles) {
      d->dfiles = (mrb_sym*)mrb_realloc(mrb, d->dfiles, sizeof(mrb_sym) * (d->ndfiles + 1));
      d->dfiles[d->ndfiles++] = f->filename_sym;
    }
    gb_u32(mrb, b, f->start_pos);
    gb_u32(mrb, b, end);
    /* count, then the entries */
    const uint8_t *p = f->lines.packed_map, *pend = p + f->line_entry_count;
    uint32_t n = 0;
    while (p < pend) { mrb_packed_int_decode(p, &p); mrb_packed_int_decode(p, &p); n++; }
    gb_u32(mrb, b, n);
    p = f->lines.packed_map;
    uint32_t pos = 0, line = 0;
    while (p < pend) {
      pos += mrb_packed_int_decode(p, &p);
      line += mrb_packed_int_decode(p, &p);
      gb_u32(mrb, b, pos);
      gb_u32(mrb, b, line);
    }
  }
  return 1;
}

int
checkpoint_dump(mrb_state *mrb, gbuf *out, char *err, size_t errlen)
{
  dumper d;
  memset(&d, 0, sizeof(d));
  d.mrb = mrb;
  d.G = checkpoint_get(mrb);
  d.P = d.G->perm;
  d.err = err; d.errlen = errlen;
  err[0] = 0;

  if (!d.P) {
    snprintf(err, errlen, "no baseline (mrb_checkpoint_baseline was not called)");
    return -1;
  }
  if (mrb->task.scheduler_lock > 0) {
    snprintf(err, errlen, "inside a synchronous Task (mrb_execute_proc_synchronously): its C frame cannot be carried");
    return -1;
  }

  gc_settings gcs;
  checkpoint_gc_settings(mrb, &gcs);   /* before the dump turns the GC off */
  mrb_bool gc_was = mrb->gc.disabled;
  mrb->gc.disabled = TRUE;
  int ai = mrb_gc_arena_save(mrb);
  d.G->last_dead = 0;

  /* the Tasks, queue by queue in order, then the main Task (Task.current
     at the root: no context, in no queue) */
  for (int q = 0; q < MRB_NUM_TASK_QUEUE; q++) {
    for (mrb_task *t = mrb->task.queues[q]; t; t = t->next) {
      d.tq = (mrb_task**)mrb_realloc(mrb, d.tq, sizeof(void*) * (d.ntask + 1));
      d.tq[d.ntask++] = t;
      pm_put(mrb, &d.taskid, t, d.ntask);
    }
  }
  if (mrb->task.main_task) {
    d.tq = (mrb_task**)mrb_realloc(mrb, d.tq, sizeof(void*) * (d.ntask + 1));
    d.tq[d.ntask++] = mrb->task.main_task;
    pm_put(mrb, &d.taskid, mrb->task.main_task, d.ntask);
  }

  /* the contexts: the root first, the Tasks' in their order, the current one */
  ctx_ref(&d, mrb->root_c);
  for (uint32_t i = 0; i < d.ntask; i++) {
    if (d.tq[i] != mrb->task.main_task) ctx_ref(&d, &d.tq[i]->c);
  }
  uint32_t current = ctx_ref(&d, mrb->c);

  w_sched(&d);
  if (!d.failed) w_patches(&d);
  /* contexts and objects reach each other: go on until both are written */
  uint32_t nc = 1, no = 1;
  while (!d.failed && (nc <= d.nctx || no <= d.nobj)) {
    if (nc <= d.nctx) w_context(&d, nc++);
    else w_object(&d, no++);
  }
  for (uint32_t i = 1; i <= d.nirep && !d.failed; i++) w_irep(&d, i);

  if (!d.failed) {
    char tmp[64];
    gb_bytes(mrb, out, CHECKPOINT_MAGIC, 4);
    gb_u16(mrb, out, CHECKPOINT_FORMAT_VERSION);
    snprintf(tmp, sizeof(tmp), "mruby %s", MRUBY_VERSION);
    gb_str(mrb, out, tmp, strlen(tmp));
    gb_str(mrb, out, "RITE0400", 8);
    gb_u8(mrb, out, 64);                        /* Integer bits */
    gb_u8(mrb, out, 64);                        /* Float bits */
    gb_u8(mrb, out, 1);                         /* String is UTF-8 */
    gb_u8(mrb, out, (uint8_t)(sizeof(void*) * 8)); /* writer's pointer width (information only) */
    gb_u32(mrb, out, d.nsym);   gb_bytes(mrb, out, d.syms.p, d.syms.len);
    gb_u32(mrb, out, d.nirep);  gb_bytes(mrb, out, d.ireps.p, d.ireps.len);
    gb_u32(mrb, out, d.nobj);   gb_bytes(mrb, out, d.objs.p, d.objs.len);
    gb_u32(mrb, out, d.npatch); gb_bytes(mrb, out, d.patches.p, d.patches.len);
    /* the VM: the contexts, the current one, the scheduler */
    gb_u32(mrb, out, d.nctx);   gb_bytes(mrb, out, d.ctxs.p, d.ctxs.len);
    gb_u32(mrb, out, current);
    gb_bytes(mrb, out, d.sched.p, d.sched.len);
    /* line numbers of the carried ireps (optional: left out when no carried
       irep has them) */
    gbuf dbg; gb_init(&dbg);
    uint32_t ndbg = 0;
    for (uint32_t i = 1; i <= d.nirep; i++) ndbg += (uint32_t)w_debug_irep(&d, &dbg, i);
    if (ndbg > 0) {
      /* every optional section: tag, length of the contents, contents */
      gb_bytes(mrb, out, "DBG0", 4);
      gb_u32(mrb, out, (uint32_t)(4 + dbg.len));
      gb_u32(mrb, out, ndbg);
      gb_bytes(mrb, out, dbg.p, dbg.len);
    }
    gb_free(mrb, &dbg);
    /* the program's source, for display (optional, asked for by source: or
       mrb_checkpoint_set_include_source): the registered files that the
       line numbers above name */
    if (d.G->dump_source) {
      gbuf src; gb_init(&src);
      uint32_t nsrc = 0;
      for (uint32_t i = 0; i < d.ndfiles; i++) {
        mrb_int flen;
        const char *fn = mrb_sym_name_len(mrb, d.dfiles[i], &flen);
        const struct checkpoint_source *cs = checkpoint_source_of(d.G, fn, (size_t)flen);
        if (!cs) continue;
        gb_str(mrb, &src, fn, (size_t)flen);
        gb_str(mrb, &src, cs->text, cs->len);
        nsrc++;
      }
      if (nsrc > 0) {
        gb_bytes(mrb, out, "SRC0", 4);
        gb_u32(mrb, out, (uint32_t)(4 + src.len));
        gb_u32(mrb, out, nsrc);
        gb_bytes(mrb, out, src.p, src.len);
      }
      gb_free(mrb, &src);
    }
    /* the GC settings, when the program changed them */
    if (memcmp(&gcs, &d.P->gc, sizeof(gcs)) != 0) {
      gb_bytes(mrb, out, "GCS0", 4);
      gb_u32(mrb, out, 1 + 4 + 4 + 8 + 8 + 8);
      gb_u8(mrb, out, gcs.flags);
      gb_u32(mrb, out, gcs.interval_ratio);
      gb_u32(mrb, out, gcs.step_ratio);
      gb_u64(mrb, out, gcs.step_limit);
      gb_u64(mrb, out, gcs.malloc_threshold);
      gb_u64(mrb, out, (uint64_t)gcs.debt_limit);
    }
    /* the end: a cut image is refused, also one cut between sections */
    gb_bytes(mrb, out, "END0", 4);
  }

  mrb_gc_arena_restore(mrb, ai);
  mrb->gc.disabled = gc_was;

  gb_free(mrb, &d.syms); gb_free(mrb, &d.ireps); gb_free(mrb, &d.objs);
  gb_free(mrb, &d.patches); gb_free(mrb, &d.ctxs); gb_free(mrb, &d.sched);
  pm_free(mrb, &d.symid); pm_free(mrb, &d.irepid); pm_free(mrb, &d.objid);
  pm_free(mrb, &d.body); pm_free(mrb, &d.ctxid); pm_free(mrb, &d.taskid); pm_free(mrb, &d.queues);
  for (uint32_t i = 0; i < d.nobj; i++) mrb_free(mrb, d.olabel[i]);
  mrb_free(mrb, d.oq); mrb_free(mrb, d.olabel); mrb_free(mrb, d.iq);
  mrb_free(mrb, d.cq); mrb_free(mrb, d.tq); mrb_free(mrb, d.qobj);
  mrb_free(mrb, d.dfiles);
  return d.failed ? -1 : 0;
}
