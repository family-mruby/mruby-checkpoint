/*
** checkpoint_dump.c - write the running state as a logical graph (format v0)
**
** Called at a safe point (see patches/0001-vm-safepoint-hook.patch): every
** frame's pc and registers are in memory, no exception is in flight and no
** C function is between the root frame and the current one.
**
** Roots are the VM (the root context's stack and call frames) and the
** changes made to permanent objects since the baseline (methods, ivars and
** constants of permanent classes, ivars of top_self, globals).
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
#include "checkpoint_internal.h"

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
frame_with_env(mrb_state *mrb, struct REnv *e)
{
  for (mrb_callinfo *ci = mrb->c->cibase; ci <= mrb->c->ci; ci++) {
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
    fail(d, "object made by mrb_open has no name: %s of class %s (%s)",
         type_name(o->tt), class_name(mrb, o->c), reach);
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
    HEADER(GK_STRING);
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
    if (c->flags & (MRB_FL_CLASS_IS_PREPENDED | MRB_FL_CLASS_IS_ORIGIN)) {
      fail(d, "class %s uses prepend, which format v0 does not carry (%s)", class_name(mrb, c), reach);
    }
    HEADER(o->tt == MRB_TT_CLASS ? GK_CLASS : o->tt == MRB_TT_MODULE ? GK_MODULE : GK_SCLASS);
    snprintf(label, sizeof(label), "superclass of #%u", id);
    gb_u32(mrb, b, obj_ref(d, c->super, label));
    uint8_t it = inst_type(MRB_INSTANCE_TT(c));
    if (it == GI_OTHER) {
      fail(d, "class %s makes instances of type %s, not in format v0 (%s)",
           class_name(mrb, c), type_name(MRB_INSTANCE_TT(c)), reach);
    }
    gb_u8(mrb, b, it);
    gb_u8(mrb, b, (uint8_t)((MRB_UNDEF_ALLOCATOR_P(c) ? 1 : 0) |
                            ((c->flags & MRB_FL_CLASS_IS_INHERITED) ? 2 : 0)));
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
    if (MRB_PROC_ALIAS_P(p)) {
      fail(d, "alias proc is not in format v0 (%s)", reach);
    }
    HEADER(GK_PROC);
    gb_u8(mrb, b, fl);
    if (MRB_PROC_CFUNC_P(p)) {
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
      struct mrb_context *c = mrb->c;
      if (e->cxt != c || e->stack < c->stbase || e->stack >= c->stend) {
        fail(d, "env shared with another context (%s)", reach);
      }
      else if (!frame_with_env(mrb, e)) {
        fail(d, "env on the stack without a frame (%s)", reach);
      }
      gb_u8(mrb, b, 1);
      gb_u32(mrb, b, (uint32_t)(e->stack - c->stbase));
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

  case MRB_TT_CDATA: {
    struct RData *rd = (struct RData*)o;
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
    fail(d, "type %s (class %s) is not in format v0 (%s)",
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
  for (int i = 0; i < irep->slen; i++) gb_u32(mrb, b, sym_ref(d, irep->syms[i]));
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

  for (uint32_t k = 0; k < P->nsnaps && !d->failed; k++) {
    gsnap *s = &P->snaps[k];
    struct RBasic *o = s->obj;
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
    case MRB_TT_STRING: case MRB_TT_ARRAY: case MRB_TT_HASH:
      if (checkpoint_content_digest(mrb, o) != s->digest) {
        fail(d, "permanent %s (%s) was modified; format v0 carries only changes to "
             "classes, ivars and globals", name, type_name(o->tt));
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
    for (i = 0; i < ngv; i++) if (P->gv[j].sym == gv[i].sym) break;
    if (i < ngv) continue;
    gb_u8(mrb, &d->patches, GP_GV_REMOVE);
    gb_u32(mrb, &d->patches, sym_ref(d, P->gv[j].sym));
    d->npatch++;
  }
  mrb_free(mrb, gv);
}

/* ---- the VM ---- */

static void
w_vm(dumper *d)
{
  mrb_state *mrb = d->mrb;
  struct mrb_context *c = mrb->c;
  gbuf *b = &d->vm;
  char label[200];

  mrb_callinfo *top = c->ci;
  size_t live = (size_t)(top->stack - c->stbase) + (size_t)mrb_ci_nregs(top);
  if (c->stbase + live > c->stend) live = (size_t)(c->stend - c->stbase);
  gb_u32(mrb, b, (uint32_t)live);
  for (size_t i = 0; i < live; i++) {
    snprintf(label, sizeof(label), "stack slot %d", (int)i);
    w_value(d, b, c->stbase[i], label);
  }

  uint32_t nframes = (uint32_t)(top - c->cibase) + 1;
  gb_u32(mrb, b, nframes);
  for (uint32_t k = 0; k < nframes; k++) {
    mrb_callinfo *ci = c->cibase + k;
    const struct RProc *p = ci->proc;
    if (!p || MRB_PROC_CFUNC_P(p)) {
      fail(d, "frame %u (%s) is a C function; not a safe point", k,
           ci->mid ? mrb_sym_name(mrb, ci->mid) : "?");
      return;
    }
    if (!MRB_PROC_ALIAS_P(p) && checkpoint_perm_name_of(d->P, p) &&
        checkpoint_perm_irep(d->P, p->body.irep, NULL)) {
      pm_put(mrb, &d->body, p, 1);
    }
    gb_u32(mrb, b, sym_ref(d, ci->mid));
    snprintf(label, sizeof(label), "proc of frame %u", k);
    gb_u32(mrb, b, obj_ref(d, p, label));
    gb_u32(mrb, b, (uint32_t)(ci->stack - c->stbase));
    gb_u32(mrb, b, ci->pc ? (uint32_t)(ci->pc - p->body.irep->iseq) : UINT32_MAX);
    gb_u8(mrb, b, ci->n);
    gb_u8(mrb, b, ci->nk);
    gb_u8(mrb, b, ci->cci);
    gb_u8(mrb, b, ci->vis);
    snprintf(label, sizeof(label), "block of frame %u", k);
    gb_u32(mrb, b, obj_ref(d, ci->blk, label));
    struct REnv *e = mrb_vm_ci_env(ci);
    if (e) {
      gb_u8(mrb, b, 1);
      snprintf(label, sizeof(label), "env of frame %u", k);
      gb_u32(mrb, b, obj_ref(d, e, label));
    }
    else if (ci->u.target_class) {
      gb_u8(mrb, b, 2);
      snprintf(label, sizeof(label), "target class of frame %u", k);
      gb_u32(mrb, b, obj_ref(d, ci->u.target_class, label));
    }
    else {
      gb_u8(mrb, b, 0);
      gb_u32(mrb, b, 0);
    }
  }
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
  if (mrb->c != mrb->root_c) {
    snprintf(err, errlen, "running in a Fiber or Task; format v0 carries only the root context");
    return -1;
  }
  for (int i = 0; i < 4; i++) {
    if (mrb->task.queues[i]) {
      snprintf(err, errlen, "Tasks exist; format v0 does not carry Tasks");
      return -1;
    }
  }

  mrb_bool gc_was = mrb->gc.disabled;
  mrb->gc.disabled = TRUE;
  int ai = mrb_gc_arena_save(mrb);

  w_vm(&d);
  if (!d.failed) w_patches(&d);
  for (uint32_t i = 1; i <= d.nobj; i++) w_object(&d, i);  /* nobj grows while we go */
  for (uint32_t i = 1; i <= d.nirep; i++) w_irep(&d, i);

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
    gb_bytes(mrb, out, d.vm.p, d.vm.len);
  }

  mrb_gc_arena_restore(mrb, ai);
  mrb->gc.disabled = gc_was;

  gb_free(mrb, &d.syms); gb_free(mrb, &d.ireps); gb_free(mrb, &d.objs);
  gb_free(mrb, &d.patches); gb_free(mrb, &d.vm);
  pm_free(mrb, &d.symid); pm_free(mrb, &d.irepid); pm_free(mrb, &d.objid);
  pm_free(mrb, &d.body);
  for (uint32_t i = 0; i < d.nobj; i++) mrb_free(mrb, d.olabel[i]);
  mrb_free(mrb, d.oq); mrb_free(mrb, d.olabel); mrb_free(mrb, d.iq);
  return d.failed ? -1 : 0;
}
