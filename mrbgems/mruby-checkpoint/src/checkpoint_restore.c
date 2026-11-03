/*
** checkpoint_restore.c - rebuild a checkpoint image (format v1) in a fresh mrb_state
**
** Order:
**   symbols -> ireps -> objects (pass 1: make every object, pass 2: fill
**   the fields) -> changes to permanent objects -> included-module classes
**   -> frozen bits (not of Hashes) -> the VM (the contexts with their
**   stacks and frames, the Tasks in their queues, the clock) -> line
**   numbers, GC settings -> Hash entries (they may call #hash / #eql? of
**   restored objects, which may run closures over the restored stacks) ->
**   frozen bits of Hashes -> full GC -> GC settings -> continue
**   (mrb_checkpoint_continue, checkpoint_task.c: the context or the Task
**   that was running goes on).
** The GC is off until the whole graph hangs from the roots.
*/

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
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
#include <mruby/debug.h>
#include <mruby/presym.h>
#include <mruby/throw.h>
#include <mruby/numeric.h>
#include "checkpoint_internal.h"
#include "task.h"

typedef struct { struct RBasic *h; mrb_value k, v; uint8_t set; } hent;
typedef struct { struct REnv *e; uint32_t ctx, off; } senv;
typedef struct { struct RFiber *f; uint32_t ctx; } sfib;

/* a context as read, before it is put in place */
typedef struct {
  uint8_t status, flags;
  uint32_t prev;
  struct RBasic *fib;
  uint32_t nvals; mrb_value *vals;
  uint32_t nframes; mrb_callinfo *frames; uint32_t *offs;
  struct mrb_context *c;
} rctx;

/* a Task as read */
typedef struct {
  struct RBasic *self;
  uint32_t ctx;
  uint8_t status, queue, priority, reason, timeslice;
  mrb_value name, result;
  int32_t remaining; uint8_t has_deadline;
  uint32_t join;
  struct RBasic *queue_obj;
  mrb_task *t;
} rtask;
typedef struct { struct RBasic *o; checkpoint_data_ent *de; const uint8_t *blob; size_t len; } dfix;

typedef struct {
  mrb_state *mrb;
  checkpoint_state *G;
  checkpoint_perm *P;
  const uint8_t *p, *end;
  int failed;
  char err[512];
  int fill;                 /* object pass: 0 = make, 1 = fill */
  int roots_done;           /* the root ireps' first reference is dropped */

  mrb_sym *syms; uint32_t nsym;
  mrb_irep **ireps; uint8_t *irep_perm; uint32_t *irep_parents; uint32_t nirep;
  struct RBasic **objs; uint8_t *okind; uint8_t *ofrozen; uint32_t nobj;

  hent *hents; size_t nhent, caphent;
  senv *senvs; size_t nsenv, capsenv;
  struct RClass **icl; size_t nicl, capicl;
  dfix *dfix; size_t ndfix, capdfix;   /* C data built from the filled object */
  struct RClass **pre; size_t npre, cappre;   /* classes with prepended modules */
  struct RBasic **fl; size_t nfl, capfl;      /* permanent Hashes frozen after their entries */
  /* v1 */
  sfib *sfibs; size_t nsfib, capsfib;         /* Fiber objects and their contexts */
  uint32_t *task_of_obj;                      /* object -> its Task record (Task objects) */
  const mrb_data_type *task_type;             /* mruby-task's (static there; probed) */
  rctx *ctxs; uint32_t nctx;
  rtask *tasks; uint32_t ntask;
} reader;

static void
rfail(reader *r, const char *fmt, ...)
{
  if (r->failed) return;
  r->failed = 1;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r->err, sizeof(r->err), fmt, ap);
  va_end(ap);
  r->p = r->end;
}

static int
need(reader *r, size_t n)
{
  if (r->failed) return 0;
  if ((size_t)(r->end - r->p) < n) { rfail(r, "truncated image"); return 0; }
  return 1;
}

static uint8_t
r_u8(reader *r)
{
  if (!need(r, 1)) return 0;
  return *r->p++;
}

static uint16_t
r_u16(reader *r)
{
  if (!need(r, 2)) return 0;
  uint16_t v = (uint16_t)((r->p[0] << 8) | r->p[1]);
  r->p += 2;
  return v;
}

static uint32_t
r_u32(reader *r)
{
  if (!need(r, 4)) return 0;
  uint32_t v = ((uint32_t)r->p[0] << 24) | ((uint32_t)r->p[1] << 16) | ((uint32_t)r->p[2] << 8) | r->p[3];
  r->p += 4;
  return v;
}

static uint64_t
r_u64(reader *r)
{
  uint64_t hi = r_u32(r);
  return (hi << 32) | r_u32(r);
}

static const uint8_t*
r_str(reader *r, uint32_t *len)
{
  *len = r_u32(r);
  if (!need(r, *len)) { *len = 0; return (const uint8_t*)""; }
  const uint8_t *s = r->p;
  r->p += *len;
  return s;
}

static mrb_sym
r_sym(reader *r)
{
  uint32_t i = r_u32(r);
  if (i == 0) return 0;
  if (i > r->nsym) { rfail(r, "symbol #%u out of range", i); return 0; }
  return r->syms[i - 1];
}

static struct RBasic*
r_obj(reader *r)
{
  uint32_t i = r_u32(r);
  if (i == 0 || !r->fill) return NULL;
  if (i > r->nobj) { rfail(r, "object #%u out of range", i); return NULL; }
  return r->objs[i - 1];
}

static mrb_value
r_value(reader *r)
{
  uint8_t tag = r_u8(r);
  switch (tag) {
  case GV_NIL: return mrb_nil_value();
  case GV_FALSE: return mrb_false_value();
  case GV_TRUE: return mrb_true_value();
  case GV_INT: return mrb_int_value(r->mrb, (mrb_int)(int64_t)r_u64(r));
  case GV_FLOAT: {
    uint64_t u = r_u64(r);
    double d; memcpy(&d, &u, 8);
    return mrb_float_value(r->mrb, (mrb_float)d);
  }
  case GV_SYM: {
    mrb_sym s = r_sym(r);
    return mrb_symbol_value(s);
  }
  case GV_OBJ: {
    struct RBasic *o = r_obj(r);
    if (!r->fill) return mrb_nil_value();
    if (!o) { rfail(r, "null object reference in a value"); return mrb_nil_value(); }
    return mrb_obj_value(o);
  }
  case GV_UNDEF: return mrb_undef_value();
  default:
    rfail(r, "bad value tag %u", tag);
    return mrb_nil_value();
  }
}

/* ---- sections ---- */

static void
r_syms(reader *r)
{
  r->nsym = r_u32(r);
  r->syms = (mrb_sym*)mrb_calloc(r->mrb, r->nsym + 1, sizeof(mrb_sym));
  for (uint32_t i = 0; i < r->nsym && !r->failed; i++) {
    uint32_t len;
    const uint8_t *s = r_str(r, &len);
    r->syms[i] = mrb_intern(r->mrb, (const char*)s, len);
  }
}

/* the content of a GR_PERM_CARRIED record whose hash this build has */
static void
skip_irep_content(reader *r)
{
  uint32_t len;
  r_u16(r); r_u16(r);                      /* nlocals, nregs */
  uint16_t clen = r_u16(r);
  uint32_t ilen = r_u32(r);
  size_t n = (size_t)ilen + sizeof(struct mrb_irep_catch_handler) * clen;
  if (!need(r, n)) return;
  r->p += n;
  uint16_t plen = r_u16(r);
  for (int k = 0; k < plen && !r->failed; k++) {
    uint8_t t = r_u8(r);
    if (t == 1 || t == 2) r_u64(r);
    else if (t == 0 || t == 3) r_str(r, &len);
    else rfail(r, "bad pool entry type %u", t);
  }
  uint16_t slen = r_u16(r);
  for (int k = 0; k < slen; k++) r_u32(r);
  uint16_t rlen = r_u16(r);
  for (int k = 0; k < rlen; k++) r_u32(r);
  uint16_t nlv = r_u16(r);
  for (int k = 0; k < nlv; k++) r_u32(r);
}

static void
r_ireps(reader *r)
{
  mrb_state *mrb = r->mrb;
  r->nirep = r_u32(r);
  r->ireps = (mrb_irep**)mrb_calloc(mrb, r->nirep + 1, sizeof(mrb_irep*));
  r->irep_perm = (uint8_t*)mrb_calloc(mrb, r->nirep + 1, 1);
  r->irep_parents = (uint32_t*)mrb_calloc(mrb, r->nirep + 1, sizeof(uint32_t));
  uint32_t **children = (uint32_t**)mrb_calloc(mrb, r->nirep + 1, sizeof(uint32_t*));
  uint64_t *hashes = (uint64_t*)mrb_calloc(mrb, r->nirep + 1, sizeof(uint64_t));

  for (uint32_t i = 0; i < r->nirep && !r->failed; i++) {
    uint8_t kind = r_u8(r);
    uint64_t hash = r_u64(r);
    hashes[i] = hash;
    if (kind == GR_PERM || kind == GR_PERM_CARRIED) {
      /* this build's own mrblib irep when the hash matches (the same build);
         otherwise the carried content is built like a program's irep */
      const mrb_irep *pi = checkpoint_perm_irep_by_hash(r->P, hash);
      if (pi) {
        if (kind == GR_PERM_CARRIED) skip_irep_content(r);
        r->ireps[i] = (mrb_irep*)pi;
        r->irep_perm[i] = 1;
        continue;
      }
      if (kind == GR_PERM) {
        rfail(r, "permanent irep %016llx not found in this build (a different mrblib?)",
              (unsigned long long)hash);
        break;
      }
    }
    else if (kind != GR_CARRIED) {
      rfail(r, "bad irep kind %u (@%u)", kind, i + 1);
      break;
    }
    mrb_irep *irep = mrb_add_irep(mrb);
    r->ireps[i] = irep;
    irep->nlocals = r_u16(r);
    irep->nregs = r_u16(r);
    irep->clen = r_u16(r);
    irep->ilen = r_u32(r);
    size_t ilen = irep->ilen + sizeof(struct mrb_irep_catch_handler) * irep->clen;
    if (!need(r, ilen)) break;
    mrb_code *iseq = (mrb_code*)mrb_malloc(mrb, ilen ? ilen : 1);
    memcpy(iseq, r->p, ilen);
    r->p += ilen;
    irep->iseq = iseq;

    uint16_t plen = r_u16(r);
    mrb_irep_pool *pool = (mrb_irep_pool*)mrb_calloc(mrb, plen ? plen : 1, sizeof(mrb_irep_pool));
    irep->pool = pool;
    irep->plen = plen;
    for (int k = 0; k < plen && !r->failed; k++) {
      uint8_t t = r_u8(r);
      switch (t) {
      case 0: { /* string */
        uint32_t len; const uint8_t *s = r_str(r, &len);
        char *c = (char*)mrb_malloc(mrb, len + 1);
        memcpy(c, s, len); c[len] = 0;
        pool[k].tt = (len << 2) | IREP_TT_STR;
        pool[k].u.str = c;
        break;
      }
      case 1: { /* integer */
        int64_t v = (int64_t)r_u64(r);
#ifdef MRB_64BIT
        pool[k].tt = IREP_TT_INT64; pool[k].u.i64 = v;
#else
        if (v >= INT32_MIN && v <= INT32_MAX) { pool[k].tt = IREP_TT_INT32; pool[k].u.i32 = (int32_t)v; }
        else { pool[k].tt = IREP_TT_INT64; pool[k].u.i64 = v; }
#endif
        break;
      }
      case 2: { /* float */
        uint64_t u = r_u64(r); double d; memcpy(&d, &u, 8);
        pool[k].tt = IREP_TT_FLOAT; pool[k].u.f = (mrb_float)d;
        break;
      }
      case 3: { /* bigint, mruby's own packing */
        uint32_t len; const uint8_t *s = r_str(r, &len);
        char *c = (char*)mrb_malloc(mrb, len ? len : 1);
        memcpy(c, s, len);
        pool[k].tt = IREP_TT_BIGINT; pool[k].u.str = c;
        break;
      }
      default:
        rfail(r, "bad pool entry type %u", t);
      }
    }

    uint16_t slen = r_u16(r);
    mrb_sym *syms = (mrb_sym*)mrb_calloc(mrb, slen ? slen : 1, sizeof(mrb_sym));
    for (int k = 0; k < slen; k++) syms[k] = r_sym(r);
    irep->syms = syms;
    irep->slen = slen;

    uint16_t rlen = r_u16(r);
    irep->rlen = rlen;
    children[i] = (uint32_t*)mrb_calloc(mrb, rlen ? rlen : 1, sizeof(uint32_t));
    for (int k = 0; k < rlen; k++) children[i][k] = r_u32(r);

    uint16_t nlv = r_u16(r);
    if (nlv > 0) {
      mrb_sym *lv = (mrb_sym*)mrb_calloc(mrb, nlv, sizeof(mrb_sym));
      for (int k = 0; k < nlv; k++) lv[k] = r_sym(r);
      irep->lv = lv;
    }
  }

  /* link children */
  for (uint32_t i = 0; i < r->nirep && !r->failed; i++) {
    if (r->irep_perm[i]) continue;
    mrb_irep *irep = r->ireps[i];
    if (irep->rlen == 0) continue;
    const mrb_irep **reps = (const mrb_irep**)mrb_calloc(mrb, irep->rlen, sizeof(mrb_irep*));
    for (int k = 0; k < irep->rlen; k++) {
      uint32_t c = children[i][k];
      if (c == 0) continue;
      if (c > r->nirep) { rfail(r, "irep #%u out of range", c); break; }
      reps[k] = r->ireps[c - 1];
      /* a new irep starts with one reference: its first parent's */
      if (r->irep_perm[c - 1] || r->irep_parents[c - 1]++ > 0) {
        mrb_irep_incref(mrb, r->ireps[c - 1]);
      }
    }
    irep->reps = reps;
  }

  /* the content hash must match what the writer computed */
  for (uint32_t i = 0; i < r->nirep && !r->failed; i++) {
    if (r->irep_perm[i]) continue;
    uint64_t h = checkpoint_irep_hash(mrb, r->ireps[i]);
    if (h != hashes[i]) {
      rfail(r, "irep #%u content hash mismatch", i + 1);
    }
  }

  for (uint32_t i = 0; i < r->nirep; i++) mrb_free(mrb, children[i]);
  mrb_free(mrb, children);
  mrb_free(mrb, hashes);
}

static checkpoint_data_ent*
data_ent_by_name(checkpoint_state *G, const uint8_t *name, uint32_t len)
{
  for (int i = 0; i < G->ndata; i++) {
    if (strlen(G->data[i].name) == len && memcmp(G->data[i].name, name, len) == 0) return &G->data[i];
  }
  return NULL;
}

static void
r_ivars(reader *r, struct RBasic *o)
{
  uint32_t n = r_u32(r);
  for (uint32_t i = 0; i < n && !r->failed; i++) {
    mrb_sym s = r_sym(r);
    mrb_value v = r_value(r);
    if (r->fill && !r->failed) mrb_obj_iv_set(r->mrb, (struct RObject*)o, s, v);
  }
}

static void
define_method(reader *r, struct RClass *c, mrb_sym sym, uint8_t vis, uint8_t kind,
              const void *ptr, uint32_t aspec)
{
  mrb_method_t m;
  uint32_t vflags = vis == 1 ? MRB_METHOD_PRIVATE_FL : vis == 2 ? MRB_METHOD_PROTECTED_FL : MRB_METHOD_PUBLIC_FL;
  if (kind == GM_FUNC) {
    MRB_METHOD_FROM_FUNC(m, (mrb_func_t)ptr);
    m.flags |= (aspec & 0xffffff);
  }
  else {
    MRB_METHOD_FROM_PROC(m, (const struct RProc*)ptr);
  }
  MRB_SET_VISIBILITY_FLAGS(m.flags, vflags);
  /* mrb_define_method_raw makes the class the Proc's target class; the
     image has the Proc's own (which differs for methods an origin holds) */
  struct RProc *p = (kind == GM_PROC) ? (struct RProc*)ptr : NULL;
  struct RClass *tc = (p && !MRB_PROC_ENV_P(p)) ? p->e.target_class : NULL;
  mrb_define_method_raw(r->mrb, c, sym, m);
  if (tc && p->gc_color != MRB_GC_RED && !MRB_PROC_ENV_P(p)) p->e.target_class = tc;
}

static void
r_method(reader *r, struct RClass *c)
{
  mrb_sym sym = r_sym(r);
  uint8_t vis = r_u8(r);
  uint8_t kind = r_u8(r);
  const void *ptr = NULL;
  uint32_t aspec = 0;
  if (kind == GM_PROC) {
    ptr = r_obj(r);
  }
  else if (kind == GM_FUNC) {
    uint32_t len; const uint8_t *name = r_str(r, &len);
    aspec = r_u32(r);
    if (r->fill) {
      int is_func = 0;
      ptr = checkpoint_perm_lookup(r->P, (const char*)name, len, &is_func);
      if (!ptr || !is_func) { rfail(r, "C function '%.*s' not found in this build", (int)len, name); return; }
    }
  }
  else if (kind != GM_UNDEF) {
    rfail(r, "bad method kind %u", kind);
    return;
  }
  if (r->fill && !r->failed) define_method(r, c, sym, vis, kind, ptr, aspec);
}

static enum mrb_vtype
vtype_of_inst(uint8_t it)
{
  switch (it) {
  case GI_OBJECT: return MRB_TT_OBJECT;
  case GI_EXCEPTION: return MRB_TT_EXCEPTION;
  case GI_STRING: return MRB_TT_STRING;
  case GI_ARRAY: return MRB_TT_ARRAY;
  case GI_HASH: return MRB_TT_HASH;
  case GI_RANGE: return MRB_TT_RANGE;
  case GI_PROC: return MRB_TT_PROC;
  case GI_DATA: return MRB_TT_CDATA;
  case GI_STRUCT: return MRB_TT_STRUCT;
  case GI_SET: return MRB_TT_SET;
  case GI_COMPLEX: return MRB_TT_COMPLEX;
  case GI_RATIONAL: return MRB_TT_RATIONAL;
  case GI_BIGINT: return MRB_TT_BIGINT;
  case GI_RANDOM: return MRB_TT_ISTRUCT;
  default: return MRB_TT_FALSE;
  }
}

static struct RBasic*
alloc_obj(reader *r, enum mrb_vtype tt)
{
  return (struct RBasic*)mrb_obj_alloc(r->mrb, tt, NULL);
}

/* give c a method table of its own, as mruby does at class creation
   (mt_new in class.c; the struct is public in mruby/class.h) */
static void
ensure_mt(reader *r, struct RClass *c)
{
  if (c->mt) return;
  mrb_mt_tbl *t = (mrb_mt_tbl*)mrb_malloc(r->mrb, sizeof(mrb_mt_tbl));
  t->size = 0;
  t->alloc = 0;
  t->ptr = NULL;
  t->next = NULL;
  c->mt = t;
}

static void
add_frozen_later(reader *r, struct RBasic *o)
{
  if (r->nfl == r->capfl) {
    r->capfl = r->capfl ? r->capfl * 2 : 8;
    r->fl = (struct RBasic**)mrb_realloc(r->mrb, r->fl, sizeof(void*) * r->capfl);
  }
  r->fl[r->nfl++] = o;
}

static void
add_prepended(reader *r, struct RClass *c)
{
  if (r->npre == r->cappre) {
    r->cappre = r->cappre ? r->cappre * 2 : 8;
    r->pre = (struct RClass**)mrb_realloc(r->mrb, r->pre, sizeof(void*) * r->cappre);
  }
  r->pre[r->npre++] = c;
}

static void
r_object(reader *r, uint32_t id)
{
  mrb_state *mrb = r->mrb;
  uint8_t kind = r_u8(r);
  struct RBasic *o = r->fill ? r->objs[id - 1] : NULL;

  if (kind == GK_PERM || kind == GK_PERM_BODY) {
    uint32_t len; const uint8_t *name = r_str(r, &len);
    /* GK_PERM_BODY also names the irep the frames run; this reader binds the
       Proc by name and its frames use that Proc's own irep (the same build) */
    if (kind == GK_PERM_BODY) r_u32(r);
    if (!r->fill) {
      int is_func = 0;
      const void *p = checkpoint_perm_lookup(r->P, (const char*)name, len, &is_func);
      if (!p || is_func) { rfail(r, "permanent object '%.*s' not found in this build", (int)len, name); return; }
      r->objs[id - 1] = (struct RBasic*)p;
    }
    r->okind[id - 1] = kind;
    return;
  }
  r->okind[id - 1] = kind;

  struct RBasic *klass = r_obj(r);
  uint8_t frozen = r_u8(r);
  r->ofrozen[id - 1] = frozen;
  if (r->fill && o) o->c = (struct RClass*)klass;

  switch (kind) {
  case GK_OBJECT:
    if (!r->fill) r->objs[id - 1] = alloc_obj(r, MRB_TT_OBJECT);
    r_ivars(r, o);
    break;

  case GK_STRING:
  case GK_BSTRING: {
    uint32_t len; const uint8_t *s = r_str(r, &len);
    if (!r->fill) {
      mrb_value str = mrb_str_new(mrb, (const char*)s, len);
      if (kind == GK_BSTRING) mrb_str_ptr(str)->flags |= MRB_STR_BINARY;
      r->objs[id - 1] = mrb_basic_ptr(str);
    }
    break;
  }

  case GK_ARRAY: {
    uint32_t n = r_u32(r);
    if (!r->fill) r->objs[id - 1] = mrb_basic_ptr(mrb_ary_new_capa(mrb, n));
    for (uint32_t i = 0; i < n && !r->failed; i++) {
      mrb_value v = r_value(r);
      if (r->fill) mrb_ary_push(mrb, mrb_obj_value(o), v);
    }
    break;
  }

  case GK_HASH: {
    uint8_t fl = r_u8(r);
    uint32_t n = r_u32(r);
    if (!r->fill) r->objs[id - 1] = mrb_basic_ptr(mrb_hash_new(mrb));
    for (uint32_t i = 0; i < n && !r->failed; i++) {
      mrb_value k = r_value(r);
      mrb_value v = r_value(r);
      if (r->fill) {
        if (r->nhent == r->caphent) {
          r->caphent = r->caphent ? r->caphent * 2 : 64;
          r->hents = (hent*)mrb_realloc(mrb, r->hents, sizeof(hent) * r->caphent);
        }
        r->hents[r->nhent].h = o; r->hents[r->nhent].k = k; r->hents[r->nhent].v = v;
        r->hents[r->nhent].set = 0;
        r->nhent++;
      }
    }
    r_ivars(r, o);
    if (r->fill) {
      if (fl & 1) o->flags |= MRB_HASH_DEFAULT;
      if (fl & 2) o->flags |= MRB_HASH_PROC_DEFAULT;
    }
    break;
  }

  case GK_RANGE: {
    mrb_value b = r_value(r);
    mrb_value e = r_value(r);
    uint8_t excl = r_u8(r);
    if (!r->fill) {
      /* bit 1: not initialized yet (Range.allocate) */
      r->objs[id - 1] = (excl & 2) ? alloc_obj(r, MRB_TT_RANGE) :
        mrb_basic_ptr(mrb_range_new(mrb, mrb_nil_value(), mrb_nil_value(), excl & 1));
    }
    else if (!(excl & 2)) {
      struct RRange *rg = (struct RRange*)o;
      RANGE_BEG(rg) = b;
      RANGE_END(rg) = e;
      RANGE_EXCL(rg) = excl & 1;
    }
    break;
  }

  case GK_EXCEPTION: {
    if (!r->fill) r->objs[id - 1] = alloc_obj(r, MRB_TT_EXCEPTION);
    mrb_value mesg = r_value(r);
    mrb_value bt = r_value(r);
    if (r->fill) {
      struct RException *ex = (struct RException*)o;
      ex->mesg = mrb_nil_p(mesg) ? NULL : mrb_basic_ptr(mesg);
      ex->backtrace = mrb_nil_p(bt) ? NULL : mrb_basic_ptr(bt);
    }
    r_ivars(r, o);
    break;
  }

  case GK_CLASS:
  case GK_MODULE:
  case GK_SCLASS: {
    enum mrb_vtype tt = kind == GK_CLASS ? MRB_TT_CLASS : kind == GK_MODULE ? MRB_TT_MODULE : MRB_TT_SCLASS;
    if (!r->fill) r->objs[id - 1] = alloc_obj(r, tt);
    struct RClass *c = (struct RClass*)o;
    struct RBasic *super = r_obj(r);
    uint8_t it = r_u8(r);
    uint8_t cfl = r_u8(r);
    if (r->fill) {
      c->super = (struct RClass*)super;
      MRB_SET_INSTANCE_TT(c, vtype_of_inst(it));
      if (cfl & 1) c->flags |= MRB_FL_UNDEF_ALLOCATE;
      if (cfl & 2) c->flags |= MRB_FL_CLASS_IS_INHERITED;
      if (cfl & 4) add_prepended(r, c);   /* the flag once the origin is built */
    }
    r_ivars(r, o);
    uint32_t n = r_u32(r);
    for (uint32_t i = 0; i < n && !r->failed; i++) r_method(r, c);
    /* a class's (empty) method table: modules are told apart by it
       (Module#prepend's cycle check compares tables) */
    if (r->fill && (cfl & 8)) ensure_mt(r, c);
    break;
  }

  case GK_ORIGIN: {
    if (!r->fill) r->objs[id - 1] = alloc_obj(r, MRB_TT_ICLASS);
    struct RClass *c = (struct RClass*)o;
    struct RBasic *super = r_obj(r);
    if (r->fill) {
      c->super = (struct RClass*)super;
      o->flags |= MRB_FL_CLASS_IS_ORIGIN | MRB_FL_CLASS_IS_INHERITED;
    }
    uint8_t has_mt = r_u8(r);
    uint32_t n = r_u32(r);
    for (uint32_t i = 0; i < n && !r->failed; i++) r_method(r, c);
    if (r->fill && has_mt) ensure_mt(r, c);
    break;
  }

  case GK_ICLASS: {
    if (!r->fill) r->objs[id - 1] = alloc_obj(r, MRB_TT_ICLASS);
    struct RBasic *super = r_obj(r);
    if (r->fill) {
      ((struct RClass*)o)->super = (struct RClass*)super;
      if (r->nicl == r->capicl) {
        r->capicl = r->capicl ? r->capicl * 2 : 16;
        r->icl = (struct RClass**)mrb_realloc(mrb, r->icl, sizeof(void*) * r->capicl);
      }
      r->icl[r->nicl++] = (struct RClass*)o;
    }
    break;
  }

  case GK_PROC: {
    if (!r->fill) r->objs[id - 1] = alloc_obj(r, MRB_TT_PROC);
    struct RProc *p = (struct RProc*)o;
    uint8_t fl = r_u8(r);
    uint8_t body = r_u8(r);
    if (body == 1) {
      uint32_t len; const uint8_t *name = r_str(r, &len);
      uint32_t aspec = r_u32(r);
      if (r->fill) {
        int is_func = 0;
        const void *f = checkpoint_perm_lookup(r->P, (const char*)name, len, &is_func);
        if (!f || !is_func) { rfail(r, "C function '%.*s' not found in this build", (int)len, name); break; }
        p->body.func = (mrb_func_t)f;
        p->flags |= MRB_PROC_CFUNC_FL;
        /* high bit: the compressed aspec follows; 0 there means the
           arguments are not checked (attr_writer's Proc), unlike NOARG */
        if (aspec & 0x80000000u) {
          p->flags &= ~(MRB_PROC_NOARG | MRB_PROC_CASPEC_MASK);
          if (aspec & 0x7fffffffu) p->flags |= mrb_proc_compress_aspec(aspec & 0x7fffffffu);
        }
        else p->flags |= MRB_PROC_NOARG;
      }
    }
    else if (body == 2) {
      mrb_sym mid = r_sym(r);
      if (r->fill) {
        p->body.mid = mid;
        p->flags |= MRB_PROC_ALIAS;
      }
    }
    else if (body != 0) {
      rfail(r, "bad proc body kind %u", body);
      break;
    }
    else {
      uint32_t ir = r_u32(r);
      if (r->fill) {
        if (ir == 0 || ir > r->nirep) { rfail(r, "irep #%u out of range", ir); break; }
        p->body.irep = r->ireps[ir - 1];
        mrb_irep_incref(mrb, (mrb_irep*)p->body.irep);
      }
    }
    struct RBasic *upper = r_obj(r);
    uint8_t ek = r_u8(r);
    struct RBasic *e = r_obj(r);
    if (r->fill) {
      if (fl & 1) p->flags |= MRB_PROC_STRICT;
      if (fl & 2) p->flags |= MRB_PROC_ORPHAN;
      if (fl & 4) p->flags |= MRB_PROC_SCOPE;
      p->upper = (const struct RProc*)upper;
      if (ek == 1) { p->e.env = (struct REnv*)e; p->flags |= MRB_PROC_ENVSET; }
      else if (ek == 2) p->e.target_class = (struct RClass*)e;
    }
    break;
  }

  case GK_ENV: {
    if (!r->fill) r->objs[id - 1] = alloc_obj(r, MRB_TT_ENV);
    struct REnv *e = (struct REnv*)o;
    mrb_sym mid = r_sym(r);
    uint8_t len = r_u8(r);
    uint8_t bidx = r_u8(r);
    uint8_t vis = r_u8(r);
    uint8_t onstack = r_u8(r);
    if (r->fill) {
      e->mid = mid;
      MRB_ENV_SET_LEN(e, len);
      MRB_ENV_SET_BIDX(e, bidx);
      e->flags = (e->flags & ~(0xfu << 16)) | ((uint32_t)vis << 16);
    }
    if (onstack) {
      uint32_t cx = r_u32(r);
      uint32_t off = r_u32(r);
      if (r->fill) {
        e->cxt = NULL;     /* the context and the stack once the contexts are built */
        e->stack = NULL;
        if (r->nsenv == r->capsenv) {
          r->capsenv = r->capsenv ? r->capsenv * 2 : 16;
          r->senvs = (senv*)mrb_realloc(mrb, r->senvs, sizeof(senv) * r->capsenv);
        }
        r->senvs[r->nsenv].e = e; r->senvs[r->nsenv].ctx = cx; r->senvs[r->nsenv].off = off; r->nsenv++;
      }
    }
    else {
      mrb_value *st = NULL;
      if (r->fill) {
        st = (mrb_value*)mrb_malloc(mrb, sizeof(mrb_value) * (len ? len : 1));
        e->stack = st;
        e->cxt = NULL;
      }
      for (int i = 0; i < len; i++) {
        mrb_value v = r_value(r);
        if (r->fill) st[i] = v;
      }
    }
    break;
  }

  case GK_BIGINT: {
    uint8_t neg = r_u8(r);
    uint32_t len; const uint8_t *digits = r_str(r, &len);
    if (!r->fill && !r->failed) {
#ifdef MRB_USE_BIGINT
      mrb_value v = mrb_bint_new_str(mrb, (const char*)digits, len, neg ? -16 : 16);
      /* the writer had a bigint object even for a value that fits (mruby does
         not always normalize); keep it one */
      if (!mrb_bigint_p(v)) v = mrb_as_bint(mrb, v);
      if (!mrb_bigint_p(v)) { rfail(r, "bigint #%u could not be made", id); break; }
      r->objs[id - 1] = mrb_basic_ptr(v);
#else
      (void)neg; (void)digits;
      rfail(r, "this build has no bigint");
#endif
    }
    break;
  }

  case GK_RANDOM: {
    uint64_t st = r_u64(r);
    uint32_t sd = r_u32(r);
    if (!r->fill) r->objs[id - 1] = alloc_obj(r, MRB_TT_ISTRUCT);
    else checkpoint_random_set(mrb_obj_value(o), st, sd);
    r_ivars(r, o);
    break;
  }

  case GK_STRUCT: {
    uint32_t n = r_u32(r);
    if (!r->fill) r->objs[id - 1] = alloc_obj(r, MRB_TT_STRUCT);
    for (uint32_t i = 0; i < n && !r->failed; i++) {
      mrb_value v = r_value(r);
      if (r->fill) mrb_ary_push(mrb, mrb_obj_value(o), v);
    }
    r_ivars(r, o);
    break;
  }

  case GK_SET: {
    uint32_t n = r_u32(r);
    if (n == UINT32_MAX) {   /* not initialized (Set.allocate) */
      if (!r->fill) r->objs[id - 1] = alloc_obj(r, MRB_TT_SET);
      r_ivars(r, o);
      break;
    }
    if (!r->fill) {
      /* an initialized, empty Set; its class is set in the fill pass */
      r->objs[id - 1] = mrb_basic_ptr(mrb_funcall_argv(mrb, mrb_obj_value(mrb_class_get(mrb, "Set")),
                                                     mrb_intern_lit(mrb, "new"), 0, NULL));
    }
    for (uint32_t i = 0; i < n && !r->failed; i++) {
      mrb_value v = r_value(r);
      if (r->fill) {
        if (r->nhent == r->caphent) {
          r->caphent = r->caphent ? r->caphent * 2 : 64;
          r->hents = (hent*)mrb_realloc(mrb, r->hents, sizeof(hent) * r->caphent);
        }
        r->hents[r->nhent].h = o; r->hents[r->nhent].k = v; r->hents[r->nhent].v = mrb_nil_value();
        r->hents[r->nhent].set = 1;
        r->nhent++;
      }
    }
    r_ivars(r, o);
    break;
  }

  case GK_RATIONAL: {
    uint32_t ln, ld;
    const uint8_t *ns = r_str(r, &ln);
    const uint8_t *ds = r_str(r, &ld);
    if (!r->fill && !r->failed) {
      mrb_value a[2];
      a[0] = mrb_str_to_integer(mrb, mrb_str_new(mrb, (const char*)ns, ln), 10, TRUE);
      a[1] = mrb_str_to_integer(mrb, mrb_str_new(mrb, (const char*)ds, ld), 10, TRUE);
      mrb_value v = mrb_funcall_argv(mrb, mrb_top_self(mrb), mrb_intern_lit(mrb, "Rational"), 2, a);
      r->objs[id - 1] = mrb_basic_ptr(v);
    }
    break;
  }

  case GK_COMPLEX: {
    uint64_t ur = r_u64(r), ui = r_u64(r);
    if (!r->fill && !r->failed) {
      double fr, fi;
      memcpy(&fr, &ur, 8); memcpy(&fi, &ui, 8);
#ifdef MRB_USE_COMPLEX
      r->objs[id - 1] = mrb_basic_ptr(mrb_complex_new(mrb, (mrb_float)fr, (mrb_float)fi));
#else
      (void)fr; (void)fi;
      rfail(r, "this build has no Complex");
#endif
    }
    break;
  }

  case GK_BREAK: {
    uint8_t tag = r_u8(r);
    uint32_t ci = r_u32(r);
    mrb_value v = r_value(r);
    if (!r->fill) r->objs[id - 1] = alloc_obj(r, MRB_TT_BREAK);
    else {
      struct RBreak *brk = (struct RBreak*)o;
      o->c = NULL;
      brk->ci_break_index = ci;
      mrb_break_value_set(brk, v);
      o->flags = (o->flags & ~(7u << 8)) | ((uint32_t)(tag & 7) << 8);
    }
    break;
  }

  case GK_FIBER: {
    uint32_t cx = r_u32(r);
    if (!r->fill) r->objs[id - 1] = alloc_obj(r, MRB_TT_FIBER);
    else if (cx) {
      if (r->nsfib == r->capsfib) {
        r->capsfib = r->capsfib ? r->capsfib * 2 : 8;
        r->sfibs = (sfib*)mrb_realloc(mrb, r->sfibs, sizeof(sfib) * r->capsfib);
      }
      r->sfibs[r->nsfib].f = (struct RFiber*)o; r->sfibs[r->nsfib].ctx = cx; r->nsfib++;
    }
    break;
  }

  case GK_TASK: {
    uint32_t ti = r_u32(r);
    if (!r->fill) {
      /* the object now, its mrb_task with the scheduler section */
      if (!r->task_type) r->task_type = checkpoint_task_type(mrb);
      if (!r->task_type) { rfail(r, "this build has no Task"); break; }
      r->objs[id - 1] = (struct RBasic*)mrb_data_object_alloc(mrb, NULL, NULL, r->task_type);
      r->task_of_obj[id - 1] = ti;
    }
    r_ivars(r, o);
    break;
  }

  case GK_DATA: {
    uint32_t nlen; const uint8_t *name = r_str(r, &nlen);
    uint32_t blen; const uint8_t *blob = r_str(r, &blen);
    if (nlen == 0) {   /* not initialized: no type, no pointer */
      if (!r->fill) r->objs[id - 1] = (struct RBasic*)mrb_data_object_alloc(mrb, NULL, NULL, NULL);
      r_ivars(r, o);
      break;
    }
    checkpoint_data_ent *de = data_ent_by_name(r->G, name, nlen);
    if (!de) { rfail(r, "C data type '%.*s' is not in this build's checkpoint data table", (int)nlen, name); break; }
    if (!r->fill) {
      /* an entry that builds from the object (its ivars) runs once every
         object is filled */
      void *ptr = de->restore ? de->restore(mrb, blob, blen) : NULL;
      r->objs[id - 1] = (struct RBasic*)mrb_data_object_alloc(mrb, NULL, ptr, de->type);
    }
    r_ivars(r, o);
    if (r->fill && de->restore_obj) {
      if (r->ndfix == r->capdfix) {
        r->capdfix = r->capdfix ? r->capdfix * 2 : 16;
        r->dfix = (dfix*)mrb_realloc(mrb, r->dfix, sizeof(dfix) * r->capdfix);
      }
      r->dfix[r->ndfix].o = o; r->dfix[r->ndfix].de = de;
      r->dfix[r->ndfix].blob = blob; r->dfix[r->ndfix].len = blen;
      r->ndfix++;
    }
    break;
  }

  default:
    rfail(r, "bad object kind %u (#%u)", kind, id);
    break;
  }
}

static void
r_objects(reader *r)
{
  r->nobj = r_u32(r);
  r->objs = (struct RBasic**)mrb_calloc(r->mrb, r->nobj + 1, sizeof(void*));
  r->okind = (uint8_t*)mrb_calloc(r->mrb, r->nobj + 1, 1);
  r->ofrozen = (uint8_t*)mrb_calloc(r->mrb, r->nobj + 1, 1);
  r->task_of_obj = (uint32_t*)mrb_calloc(r->mrb, r->nobj + 1, sizeof(uint32_t));
  const uint8_t *start = r->p;
  r->fill = 0;
  for (uint32_t i = 1; i <= r->nobj && !r->failed; i++) r_object(r, i);
  if (r->failed) return;
  r->p = start;
  r->fill = 1;
  for (uint32_t i = 1; i <= r->nobj && !r->failed; i++) r_object(r, i);
}

static void
r_patches(reader *r)
{
  mrb_state *mrb = r->mrb;
  uint32_t n = r_u32(r);
  for (uint32_t i = 0; i < n && !r->failed; i++) {
    uint8_t kind = r_u8(r);
    switch (kind) {
    case GP_METHOD_SET: {
      struct RClass *c = (struct RClass*)r_obj(r);
      r_method(r, c);
      break;
    }
    case GP_METHOD_REMOVE: {
      struct RClass *c = (struct RClass*)r_obj(r);
      mrb_sym s = r_sym(r);
      if (!r->failed) mrb_remove_method(mrb, c, s);
      break;
    }
    case GP_IV_SET: {
      struct RBasic *o = r_obj(r);
      mrb_sym s = r_sym(r);
      mrb_value v = r_value(r);
      if (!r->failed) mrb_obj_iv_set(mrb, (struct RObject*)o, s, v);
      break;
    }
    case GP_IV_REMOVE: {
      struct RBasic *o = r_obj(r);
      mrb_sym s = r_sym(r);
      if (!r->failed) mrb_iv_remove(mrb, mrb_obj_value(o), s);
      break;
    }
    case GP_SUPER_SET: {
      struct RClass *c = (struct RClass*)r_obj(r);
      struct RClass *s = (struct RClass*)r_obj(r);
      if (!r->failed) c->super = s;
      break;
    }
    case GP_GV_SET: {
      mrb_sym s = r_sym(r);
      mrb_value v = r_value(r);
      if (!r->failed) mrb_gv_set(mrb, s, v);
      break;
    }
    case GP_CONTENT_SET: {
      struct RBasic *o = r_obj(r);
      uint8_t frozen = r_u8(r);
      uint8_t kind = r_u8(r);   /* 3 String, 4 Array, 5 Hash: the object's own type */
      if (r->failed) break;
      if ((kind == GK_STRING && o->tt != MRB_TT_STRING) || (kind == GK_ARRAY && o->tt != MRB_TT_ARRAY) ||
          (kind == GK_HASH && o->tt != MRB_TT_HASH)) { rfail(r, "contents of the wrong type for a permanent object"); break; }
      mrb_value v = mrb_obj_value(o);
      o->frozen = 0;
      if (o->tt == MRB_TT_STRING) {
        uint32_t len; const uint8_t *b = r_str(r, &len);
        if (!r->failed) { mrb_str_resize(mrb, v, 0); mrb_str_cat(mrb, v, (const char*)b, len); }
        o->frozen = frozen;
      }
      else if (o->tt == MRB_TT_ARRAY) {
        uint32_t n = r_u32(r);
        mrb_ary_clear(mrb, v);
        for (uint32_t i = 0; i < n && !r->failed; i++) mrb_ary_push(mrb, v, r_value(r));
        o->frozen = frozen;
      }
      else if (o->tt == MRB_TT_HASH) {
        /* emptied now, filled with the other Hashes (keys may be user objects) */
        uint32_t n = r_u32(r);
        mrb_hash_clear(mrb, v);
        for (uint32_t i = 0; i < n && !r->failed; i++) {
          mrb_value k = r_value(r);
          mrb_value val = r_value(r);
          if (r->nhent == r->caphent) {
            r->caphent = r->caphent ? r->caphent * 2 : 64;
            r->hents = (hent*)mrb_realloc(mrb, r->hents, sizeof(hent) * r->caphent);
          }
          r->hents[r->nhent].h = o; r->hents[r->nhent].k = k; r->hents[r->nhent].v = val;
          r->hents[r->nhent].set = 0;
          r->nhent++;
        }
        if (frozen) add_frozen_later(r, o);
      }
      else rfail(r, "contents for a permanent object that is not a String, Array or Hash");
      break;
    }
    case GP_CLASS_FLAGS: {
      struct RClass *c = (struct RClass*)r_obj(r);
      uint8_t f = r_u8(r);
      if (!r->failed) {
        if (f & 1) c->flags |= MRB_FL_CLASS_IS_PREPENDED;
        else c->flags &= ~MRB_FL_CLASS_IS_PREPENDED;
      }
      break;
    }
    case GP_RANDOM_SET: {
      struct RBasic *o = r_obj(r);
      uint64_t st = r_u64(r);
      uint32_t sd = r_u32(r);
      if (!r->failed) {
        if (!o || o->tt != MRB_TT_ISTRUCT) { rfail(r, "Random state for an object that is not one"); break; }
        checkpoint_random_set(mrb_obj_value(o), st, sd);
      }
      break;
    }
    case GP_GV_REMOVE: {
      mrb_sym s = r_sym(r);
      if (!r->failed) mrb_gv_remove(mrb, s);
      break;
    }
    default:
      rfail(r, "bad patch kind %u", kind);
    }
  }
}

/* one frame of a context (format v1 section 10) */
static void
r_frame(reader *r, rctx *x, uint32_t cid, uint32_t k)
{
  mrb_callinfo *ci = &x->frames[k];
  uint8_t kind = r_u8(r);
  ci->mid = r_sym(r);
  ci->proc = (const struct RProc*)r_obj(r);
  if (kind == GF_CFUNC) {
    uint32_t len; const uint8_t *name = r_str(r, &len);
    if (r->failed) return;
    /* only the functions that wait for a context switch (see the writer) */
    if (!checkpoint_switch_func_name((const char*)name, len)) {
      rfail(r, "context %u waits in the C function %.*s, which this reader cannot start again", cid, (int)len, name);
      return;
    }
    if (k + 1 != x->nframes) { rfail(r, "a C function frame below the top of context %u", cid); return; }
    if (ci->proc && !MRB_PROC_CFUNC_P(ci->proc)) { rfail(r, "C function frame %u with a Ruby proc", k); return; }
  }
  else if (kind != GF_RUBY) {
    rfail(r, "bad frame kind %u", kind);
    return;
  }
  uint32_t off = r_u32(r);
  uint32_t pc = r_u32(r);
  ci->n = r_u8(r) & 0xf;
  ci->nk = r_u8(r) & 0xf;
  ci->cci = r_u8(r);
  ci->vis = r_u8(r);
  ci->blk = (struct RProc*)r_obj(r);
  uint8_t uk = r_u8(r);
  struct RBasic *u = r_obj(r);
  if (r->failed) return;
  if (kind == GF_RUBY && (!ci->proc || MRB_PROC_CFUNC_P(ci->proc))) { rfail(r, "frame %u of context %u has no Ruby proc", k, cid); return; }
  if (off > x->nvals) { rfail(r, "frame %u stack offset out of range", k); return; }
  x->offs[k] = off;
  ci->pc = NULL;
  if (kind == GF_RUBY && pc != UINT32_MAX) {
    if (pc > ci->proc->body.irep->ilen) { rfail(r, "frame %u pc out of range", k); return; }
    ci->pc = ci->proc->body.irep->iseq + pc;
  }
  if (uk == 1) ci->u.env = (struct REnv*)u;
  else if (uk == 2) ci->u.target_class = (struct RClass*)u;
}

static void
r_contexts(reader *r)
{
  mrb_state *mrb = r->mrb;
  r->nctx = r_u32(r);
  if (r->failed) return;
  if (r->nctx == 0) { rfail(r, "no contexts"); return; }
  r->ctxs = (rctx*)mrb_calloc(mrb, r->nctx, sizeof(rctx));
  for (uint32_t i = 0; i < r->nctx && !r->failed; i++) {
    rctx *x = &r->ctxs[i];
    x->status = r_u8(r);
    x->flags = r_u8(r);
    x->prev = r_u32(r);
    x->fib = r_obj(r);
    if (x->prev > r->nctx) { rfail(r, "context #%u out of range", x->prev); return; }
    if (x->status > MRB_FIBER_TERMINATED) { rfail(r, "bad context status %u", x->status); return; }
    x->nvals = r_u32(r);
    if (r->failed) return;
    if (x->nvals > (size_t)(r->end - r->p)) { rfail(r, "truncated image"); return; }
    x->vals = (mrb_value*)mrb_calloc(mrb, x->nvals + 1, sizeof(mrb_value));
    for (uint32_t k = 0; k < x->nvals && !r->failed; k++) x->vals[k] = r_value(r);
    x->nframes = r_u32(r);
    if (r->failed) return;
    if (x->nframes > (size_t)(r->end - r->p)) { rfail(r, "truncated image"); return; }
    if (x->nframes == 0 && x->nvals > 0) { rfail(r, "a stack without frames in context %u", i + 1); return; }
    x->frames = (mrb_callinfo*)mrb_calloc(mrb, x->nframes + 1, sizeof(mrb_callinfo));
    x->offs = (uint32_t*)mrb_calloc(mrb, x->nframes + 1, sizeof(uint32_t));
    for (uint32_t k = 0; k < x->nframes && !r->failed; k++) r_frame(r, x, i + 1, k);
  }
  if (!r->failed && r->ctxs[0].nframes == 0 && r->ctxs[0].status != MRB_FIBER_CREATED &&
      r->ctxs[0].status != MRB_FIBER_RUNNING && r->ctxs[0].status != MRB_FIBER_RESUMED) {
    rfail(r, "bad state of the root context");
  }
}

static int32_t
r_i32(reader *r)
{
  return (int32_t)r_u32(r);
}

static void
r_sched(reader *r, int *switch_pending, int *loop_running, uint16_t *unit, uint32_t *tick,
        uint8_t *has_wakeup, int32_t *wakeup, uint32_t *running, uint32_t *host)
{
  mrb_state *mrb = r->mrb;
  uint8_t fl = r_u8(r);
  *switch_pending = fl & 1;
  *loop_running = (fl & 2) != 0;
  *unit = r_u16(r);
  *tick = r_u32(r);
  *has_wakeup = r_u8(r);
  *wakeup = r_i32(r);
  r->ntask = r_u32(r);
  if (r->failed) return;
  if (r->ntask > (size_t)(r->end - r->p)) { rfail(r, "truncated image"); return; }
  r->tasks = (rtask*)mrb_calloc(mrb, r->ntask + 1, sizeof(rtask));
  int nmain = 0;
  for (uint32_t i = 0; i < r->ntask && !r->failed; i++) {
    rtask *t = &r->tasks[i];
    t->self = r_obj(r);
    t->ctx = r_u32(r);
    t->status = r_u8(r);
    t->queue = r_u8(r);
    t->priority = r_u8(r);
    t->reason = r_u8(r);
    t->timeslice = r_u8(r);
    t->name = r_value(r);
    t->result = r_value(r);
    switch (t->reason) {
    case MRB_TASK_REASON_NONE: break;
    case MRB_TASK_REASON_SLEEP: t->has_deadline = 1; t->remaining = r_i32(r); break;
    case MRB_TASK_REASON_JOIN: t->join = r_u32(r); break;
    case MRB_TASK_REASON_QUEUE:
      t->queue_obj = r_obj(r);
      t->has_deadline = r_u8(r);
      t->remaining = r_i32(r);
      break;
    default: rfail(r, "Task #%u waits for reason %u", i + 1, t->reason); break;
    }
    if (r->failed) break;
    if (!t->self || t->self->tt != MRB_TT_CDATA || DATA_TYPE(mrb_obj_value(t->self)) != r->task_type) {
      rfail(r, "Task #%u has no Task object", i + 1); break;
    }
    if (t->queue == 255) {
      if (t->ctx != 0 || ++nmain > 1) { rfail(r, "bad main Task #%u", i + 1); break; }
    }
    else {
      if (t->queue >= MRB_NUM_TASK_QUEUE || t->ctx == 0 || t->ctx > r->nctx || t->ctx == 1) {
        rfail(r, "bad Task #%u", i + 1); break;
      }
      if (i > 0 && r->tasks[i - 1].queue != 255 && r->tasks[i - 1].queue > t->queue) {
        rfail(r, "Tasks out of the order of their queues"); break;
      }
    }
    if (t->join > r->ntask) { rfail(r, "Task #%u joins Task #%u out of range", i + 1, t->join); break; }
    if (t->reason == MRB_TASK_REASON_QUEUE && t->queue_obj &&
        (t->queue_obj->tt != MRB_TT_CDATA || !DATA_TYPE(mrb_obj_value(t->queue_obj)) ||
         strcmp(DATA_TYPE(mrb_obj_value(t->queue_obj))->struct_name, "Task::Queue") != 0)) {
      rfail(r, "Task #%u waits on something that is not a Task::Queue", i + 1); break;
    }
  }
  *running = r_u32(r);
  *host = r_u32(r);
  if (!r->failed && (*running > r->ntask || *host > r->ntask)) rfail(r, "Task out of range");
  if (!r->failed && *running && r->tasks[*running - 1].queue == 255) {
    rfail(r, "the main Task cannot be the running one");
  }
}

/* the ticks of the writer's clock in this one's */
static uint32_t
conv_ticks(uint32_t t, uint16_t unit)
{
  if (unit == MRB_TICK_UNIT || unit == 0) return t;
  return (uint32_t)(((uint64_t)t * unit + MRB_TICK_UNIT - 1) / MRB_TICK_UNIT);
}

static int32_t
conv_remaining(int32_t t, uint16_t unit)
{
  if (unit == MRB_TICK_UNIT || unit == 0) return t;
  int64_t ms = (int64_t)t * unit;
  return (int32_t)(ms >= 0 ? (ms + MRB_TICK_UNIT - 1) / MRB_TICK_UNIT : ms / MRB_TICK_UNIT);
}

static void
r_vm(reader *r)
{
  mrb_state *mrb = r->mrb;
  r_contexts(r);
  uint32_t current = r_u32(r);
  if (!r->failed && (current == 0 || current > r->nctx)) rfail(r, "current context out of range");
  int switch_pending = 0, loop_running = 0;
  uint16_t unit = 0; uint32_t tick = 0, running = 0, host = 0;
  uint8_t has_wakeup = 0; int32_t wakeup = 0;
  if (!r->failed) r_sched(r, &switch_pending, &loop_running, &unit, &tick, &has_wakeup, &wakeup, &running, &host);
  if (r->failed) return;

  /* which context is whose: the root, the Tasks', the others (Fibers') */
  uint8_t *owner = (uint8_t*)mrb_calloc(mrb, r->nctx + 1, 1);
  owner[0] = 1;
  for (uint32_t i = 0; i < r->ntask && !r->failed; i++) {
    if (r->tasks[i].queue == 255) continue;
    if (owner[r->tasks[i].ctx - 1]) rfail(r, "context %u belongs to two owners", r->tasks[i].ctx);
    owner[r->tasks[i].ctx - 1] = 2;
  }
  for (size_t i = 0; i < r->nsfib && !r->failed; i++) {
    uint32_t cx = r->sfibs[i].ctx;
    if (cx == 0 || cx > r->nctx) { rfail(r, "Fiber's context out of range"); break; }
    /* Fiber.current at the root or in a Task names that context; a Fiber's
       own context belongs to the Fiber */
    if (owner[cx - 1] == 0) owner[cx - 1] = 3;
  }
  for (uint32_t i = 0; i < r->nctx && !r->failed; i++) {
    if (!owner[i]) rfail(r, "context %u belongs to no Fiber or Task", i + 1);
  }
  for (size_t i = 0; i < r->nsenv && !r->failed; i++) {
    uint32_t cx = r->senvs[i].ctx;
    if (cx == 0 || cx > r->nctx || r->senvs[i].off >= r->ctxs[cx - 1].nvals) rfail(r, "env stack position out of range");
  }
  if (r->failed) { mrb_free(mrb, owner); return; }

  /* the Tasks' structs (mruby-task's mrb_task) */
  for (uint32_t i = 0; i < r->ntask; i++) {
    rtask *rt = &r->tasks[i];
    mrb_task *t = (mrb_task*)mrb_calloc(mrb, 1, sizeof(mrb_task));
    rt->t = t;
    t->priority = rt->priority;
    t->status = rt->status;
    t->reason = rt->reason;
    t->timeslice = rt->timeslice;
    t->name = rt->name;
    t->result = rt->result;
    t->self = mrb_obj_value(rt->self);
    DATA_PTR(t->self) = t;
    if (rt->queue != 255) r->ctxs[rt->ctx - 1].c = &t->c;
  }

  /* the contexts in place */
  for (uint32_t i = 0; i < r->nctx; i++) {
    rctx *x = &r->ctxs[i];
    struct mrb_context *c;
    if (i == 0) c = mrb->root_c;
    else if (x->c) c = x->c;
    else c = (struct mrb_context*)mrb_calloc(mrb, 1, sizeof(struct mrb_context));
    x->c = c;
    if (x->nframes == 0) {
      if (i == 0) {
        /* the root of a host that runs the scheduler from C: idle */
        for (mrb_value *v = c->stbase; v < c->stend; v++) *v = mrb_nil_value();
        if (c->stbase < c->stend) c->stbase[0] = mrb_top_self(mrb);
        memset(c->cibase, 0, sizeof(mrb_callinfo));
        c->ci = c->cibase;
        c->ci->stack = c->stbase;
        c->ci->u.target_class = mrb->object_class;
      }
      else {
        c->stbase = c->stend = NULL;
        c->cibase = c->ci = c->ciend = NULL;
      }
      continue;
    }
    size_t size = (size_t)x->nvals + 128;
    size_t cisize = (size_t)x->nframes + 16;
    if (i == 0) {
      if ((size_t)(c->stend - c->stbase) < size) {
        c->stbase = (mrb_value*)mrb_realloc(mrb, c->stbase, sizeof(mrb_value) * size);
        c->stend = c->stbase + size;
      }
      if ((size_t)(c->ciend - c->cibase) < cisize) {
        c->cibase = (mrb_callinfo*)mrb_realloc(mrb, c->cibase, sizeof(mrb_callinfo) * cisize);
        c->ciend = c->cibase + cisize;
      }
      size = (size_t)(c->stend - c->stbase);
      cisize = (size_t)(c->ciend - c->cibase);
    }
    else {
      c->stbase = (mrb_value*)mrb_malloc(mrb, sizeof(mrb_value) * size);
      c->stend = c->stbase + size;
      c->cibase = (mrb_callinfo*)mrb_malloc(mrb, sizeof(mrb_callinfo) * cisize);
      c->ciend = c->cibase + cisize;
    }
    for (size_t k = 0; k < size; k++) c->stbase[k] = k < x->nvals ? x->vals[k] : mrb_nil_value();
    memset(c->cibase, 0, sizeof(mrb_callinfo) * cisize);
    for (uint32_t k = 0; k < x->nframes; k++) {
      c->cibase[k] = x->frames[k];
      c->cibase[k].stack = c->stbase + x->offs[k];
    }
    c->ci = c->cibase + x->nframes - 1;
  }
  for (uint32_t i = 0; i < r->nctx; i++) {
    rctx *x = &r->ctxs[i];
    x->c->status = (enum mrb_fiber_state)x->status;
    x->c->vmexec = (x->flags & 1) ? TRUE : FALSE;
    x->c->prev = x->prev ? r->ctxs[x->prev - 1].c : NULL;
    x->c->fib = (struct RFiber*)x->fib;
  }
  for (size_t i = 0; i < r->nsfib; i++) r->sfibs[i].f->cxt = r->ctxs[r->sfibs[i].ctx - 1].c;
  for (size_t i = 0; i < r->nsenv; i++) {
    struct mrb_context *c = r->ctxs[r->senvs[i].ctx - 1].c;
    r->senvs[i].e->cxt = c;
    r->senvs[i].e->stack = c->stbase + r->senvs[i].off;
  }
  mrb_free(mrb, owner);

  /* the clock goes on from the writer's: the program may hold ticks (a
     Task::Queue#pop deadline, Task.tick); what a wait has left is the same */
  uint32_t now = conv_ticks(tick, unit);
  mrb->task.tick = now;
  mrb->task.wakeup_tick = has_wakeup ? mrb_task_normalize_wakeup(now + (uint32_t)conv_remaining(wakeup, unit)) : UINT32_MAX;

  /* the queues, in their order */
  mrb_task *last[MRB_NUM_TASK_QUEUE] = { NULL, NULL, NULL, NULL };
  for (int q = 0; q < MRB_NUM_TASK_QUEUE; q++) mrb->task.queues[q] = NULL;
  for (uint32_t i = 0; i < r->ntask; i++) {
    rtask *rt = &r->tasks[i];
    mrb_task *t = rt->t;
    if (rt->reason == MRB_TASK_REASON_SLEEP) {
      t->wait.wakeup_tick = mrb_task_normalize_wakeup(now + (uint32_t)conv_remaining(rt->remaining, unit));
    }
    else if (rt->reason == MRB_TASK_REASON_JOIN) {
      t->wait.join = rt->join ? r->tasks[rt->join - 1].t : NULL;
    }
    else if (rt->reason == MRB_TASK_REASON_QUEUE) {
      t->wait.queue.target = rt->queue_obj ? (struct mrb_task_queue*)DATA_PTR(mrb_obj_value(rt->queue_obj)) : NULL;
      t->wait.queue.wakeup_tick = rt->has_deadline ?
        mrb_task_normalize_wakeup(now + (uint32_t)conv_remaining(rt->remaining, unit)) : UINT32_MAX;
    }
    if (rt->queue == 255) {
      mrb->task.main_task = t;
      if (mrb_nil_p(t->name)) t->name = mrb_str_new_cstr(mrb, "main");
    }
    else {
      t->next = NULL;
      if (last[rt->queue]) last[rt->queue]->next = t;
      else mrb->task.queues[rt->queue] = t;
      last[rt->queue] = t;
    }
    mrb_gc_register(mrb, t->self);
  }
  mrb->task.switching = FALSE;
  mrb->task.loop_running = FALSE;

  /* what mrb_checkpoint_continue starts again */
  r->G->plan.set = 1;
  r->G->plan.current = r->ctxs[current - 1].c;
  r->G->plan.task = running ? r->tasks[running - 1].t : NULL;
  r->G->plan.switch_pending = switch_pending;
  r->G->plan.loop_running = loop_running;
  r->G->host_task = host ? r->tasks[host - 1].t : NULL;
}

/* line numbers of carried ireps (section 13) */
static void
r_debug(reader *r)
{
  mrb_state *mrb = r->mrb;
  uint32_t n = r_u32(r);
  for (uint32_t i = 0; i < n && !r->failed; i++) {
    uint32_t id = r_u32(r);
    uint16_t nf = r_u16(r);
    if (id == 0 || id > r->nirep) { rfail(r, "debug info for irep #%u out of range", id); return; }
    mrb_irep *irep = r->ireps[id - 1];
    int use = !r->irep_perm[id - 1] && !irep->debug_info;
    uint16_t *lines = NULL;
    mrb_irep_debug_info *di = NULL;
    if (use) {
      lines = (uint16_t*)mrb_calloc(mrb, irep->ilen + 1, sizeof(uint16_t));
      di = mrb_debug_info_alloc(mrb, irep);
    }
    for (uint16_t k = 0; k < nf && !r->failed; k++) {
      uint32_t flen; const uint8_t *fn = r_str(r, &flen);
      uint32_t start = r_u32(r), end = r_u32(r), ne = r_u32(r);
      char name[512];
      snprintf(name, sizeof(name), "%.*s", (int)flen, (const char*)fn);
      uint32_t pos = start, line = 0;
      for (uint32_t j = 0; j < ne && !r->failed; j++) {
        uint32_t p = r_u32(r), l = r_u32(r);
        if (use) {
          for (; pos < p && pos < end && pos <= irep->ilen; pos++) lines[pos] = (uint16_t)line;
        }
        line = l;
      }
      if (use && !r->failed) {
        for (; pos < end && pos <= irep->ilen; pos++) lines[pos] = (uint16_t)line;
        if (end > irep->ilen + 1) end = irep->ilen + 1;
        if (start < end) mrb_debug_info_append_file(mrb, di, name, lines, start, end);
      }
    }
    mrb_free(mrb, lines);
  }
}

/* the program's source (section 13), for display only: registered again,
   as the host registers what it compiles, so that a later image of this
   state can carry it on */
static void
r_sources(reader *r)
{
  uint32_t n = r_u32(r);
  for (uint32_t i = 0; i < n && !r->failed; i++) {
    uint32_t flen, slen;
    const uint8_t *fn = r_str(r, &flen);
    const uint8_t *src = r_str(r, &slen);
    if (r->failed) return;
    char *name = (char*)mrb_malloc(r->mrb, flen + 1);
    memcpy(name, fn, flen);
    name[flen] = 0;
    mrb_checkpoint_add_source(r->mrb, name, (const char*)src, slen);
    mrb_free(r->mrb, name);
  }
}

static void
reader_free(reader *r)
{
  mrb_state *mrb = r->mrb;
  mrb_free(mrb, r->syms);
  mrb_free(mrb, r->ireps); mrb_free(mrb, r->irep_perm); mrb_free(mrb, r->irep_parents);
  mrb_free(mrb, r->objs); mrb_free(mrb, r->okind); mrb_free(mrb, r->ofrozen);
  mrb_free(mrb, r->hents); mrb_free(mrb, r->senvs); mrb_free(mrb, r->icl);
  mrb_free(mrb, r->dfix);
  mrb_free(mrb, r->pre);
  mrb_free(mrb, r->fl);
  mrb_free(mrb, r->sfibs);
  mrb_free(mrb, r->task_of_obj);
  for (uint32_t i = 0; r->ctxs && i < r->nctx; i++) {
    mrb_free(mrb, r->ctxs[i].vals);
    mrb_free(mrb, r->ctxs[i].frames);
    mrb_free(mrb, r->ctxs[i].offs);
  }
  mrb_free(mrb, r->ctxs);
  mrb_free(mrb, r->tasks);
}

MRB_API void
mrb_checkpoint_restore(mrb_state *mrb, const uint8_t *buf, size_t len)
{
  reader r;
  memset(&r, 0, sizeof(r));
  r.mrb = mrb;
  r.G = checkpoint_get(mrb);
  r.P = r.G->perm;
  r.p = buf; r.end = buf + len;

  struct RClass *eCheckpoint = mrb_class_get_under(mrb, mrb_module_get(mrb, "Checkpoint"), "Error");
  if (!r.P) mrb_raise(mrb, eCheckpoint, "no baseline (mrb_checkpoint_baseline was not called)");
  if (mrb->c != mrb->root_c || mrb->c->ci != mrb->c->cibase) {
    mrb_raise(mrb, eCheckpoint, "restore needs an idle root context");
  }
  for (int q = 0; q < MRB_NUM_TASK_QUEUE; q++) {
    if (mrb->task.queues[q]) mrb_raise(mrb, eCheckpoint, "restore needs a state without Tasks");
  }
  memset(&r.G->plan, 0, sizeof(r.G->plan));

  /* header */
  if (!need(&r, 6) || memcmp(r.p, CHECKPOINT_MAGIC, 4) != 0) mrb_raise(mrb, eCheckpoint, "not a checkpoint image");
  r.p += 4;
  uint16_t ver = r_u16(&r);
  if (ver != CHECKPOINT_FORMAT_VERSION) mrb_raisef(mrb, eCheckpoint, "checkpoint format %d is not supported", (int)ver);
  uint32_t l1; const uint8_t *mv = r_str(&r, &l1);
  uint32_t l2; r_str(&r, &l2);
  uint8_t ibits = r_u8(&r), fbits = r_u8(&r), utf8 = r_u8(&r);
  r_u8(&r); /* writer pointer width */
  if (r.failed) mrb_raise(mrb, eCheckpoint, r.err);
  if (ibits != 64 || fbits != 64 || utf8 != 1) {
    mrb_raise(mrb, eCheckpoint, "value representation differs (needs 64-bit Integer, double Float, UTF-8)");
  }
  (void)mv;

  mrb_full_gc(mrb);
  mrb_bool gc_was = mrb->gc.disabled;
  mrb->gc.disabled = TRUE;
  int ai = mrb_gc_arena_save(mrb);

  r_syms(&r);
  if (!r.failed) r_ireps(&r);
  if (!r.failed) r_objects(&r);
  if (!r.failed) {
    /* a new irep starts with one reference, owned by its first parent; for
       a root irep (no parent here) the Procs now hold it, so drop that one
       (a root no Proc holds is not used: it goes) */
    for (uint32_t i = 0; i < r.nirep; i++) {
      if (!r.irep_perm[i] && r.irep_parents[i] == 0) mrb_irep_decref(mrb, r.ireps[i]);
    }
    r.roots_done = 1;
  }
  r.fill = 1;
  for (size_t i = 0; i < r.npre; i++) r.pre[i]->flags |= MRB_FL_CLASS_IS_PREPENDED;
  for (size_t i = 0; i < r.ndfix && !r.failed; i++) {
    struct RData *rd = (struct RData*)r.dfix[i].o;
    char why[200] = "";
    void *ptr = r.dfix[i].de->restore_obj(mrb, mrb_obj_value(rd), r.dfix[i].blob, r.dfix[i].len, why, sizeof(why));
    if (!ptr) { rfail(&r, "C data %s could not be rebuilt: %s", r.dfix[i].de->name, why); break; }
    rd->data = ptr;
  }
  if (!r.failed) r_patches(&r);
  if (!r.failed) {
    /* included-module classes share the module's method table */
    for (size_t i = 0; i < r.nicl; i++) {
      struct RClass *ic = r.icl[i];
      struct RClass *m = ic->c;
      MRB_CLASS_ORIGIN(m);
      ic->mt = m->mt;
    }
    /* frozen bits, except a Hash's (its entries are not in yet) */
    for (uint32_t i = 0; i < r.nobj; i++) {
      if (r.okind[i] != GK_PERM && r.okind[i] != GK_PERM_BODY && r.okind[i] != GK_HASH &&
          r.okind[i] != GK_SET && r.ofrozen[i]) r.objs[i]->frozen = 1;
    }
  }
  /* the VM before the Hash entries: a key's #hash may run a closure whose
     variables live on the restored stack */
  if (!r.failed) r_vm(&r);
  /* optional sections: a tag, the length of the contents, the contents.
     A section this reader does not know is skipped by its length; a known
     one must fill its length exactly. The end mark END0 has no length. */
  int has_gcs = 0, has_end = 0;
  gc_settings gcs;
  while (!r.failed && r.end - r.p >= 4) {
    if (memcmp(r.p, "END0", 4) == 0) {
      r.p += 4;
      has_end = 1;
      break;
    }
    const uint8_t *tag = r.p;
    r.p += 4;
    uint32_t len = r_u32(&r);
    if (r.failed) break;
    if ((size_t)(r.end - r.p) < len) {
      rfail(&r, "section %.4s: %u bytes of contents, only %d left", (const char*)tag, len, (int)(r.end - r.p));
      break;
    }
    const uint8_t *all_end = r.end, *body_end = r.p + len;
    r.end = body_end;
    if (memcmp(tag, "DBG0", 4) == 0) {
      r_debug(&r);
    }
    else if (memcmp(tag, "SRC0", 4) == 0) {
      r_sources(&r);
    }
    else if (memcmp(tag, "GCS0", 4) == 0) {
      has_gcs = 1;
      gcs.flags = r_u8(&r);
      gcs.interval_ratio = r_u32(&r);
      gcs.step_ratio = r_u32(&r);
      gcs.step_limit = r_u64(&r);
      gcs.malloc_threshold = r_u64(&r);
      gcs.debt_limit = (int64_t)r_u64(&r);
    }
    else {
      r.p = body_end;          /* not ours: skipped */
    }
    if (!r.failed && r.p != body_end) {
      rfail(&r, "section %.4s: %d bytes left inside its length", (const char*)tag, (int)(body_end - r.p));
    }
    if (r.failed) break;
    r.end = all_end;
  }
  if (!r.failed && !has_end) rfail(&r, "truncated image (no end mark)");
  if (!r.failed && r.p != r.end) rfail(&r, "%d bytes left after the image", (int)(r.end - r.p));

#ifndef MRB_NO_METHOD_CACHE
  memset(mrb->cache, 0, sizeof(mrb->cache));
#endif
#ifndef MRB_NO_CONST_CACHE
  memset(mrb->const_cache, 0, sizeof(mrb->const_cache));
#endif

  if (!r.failed) {
    /* Hash entries last: #hash and #eql? may be user methods. They run
       here (as in Marshal.load); one that raises, or that changes a Hash
       while the Hashes are rebuilt, makes the restore fail */
    struct mrb_jmpbuf *prev_jmp = mrb->jmp;
    struct mrb_jmpbuf c_jmp;
    MRB_TRY(&c_jmp) {
      mrb->jmp = &c_jmp;
      /* containers last-written first: an object reached from another has a
         later number, so a Hash or Set used as a key or element is filled
         before the one that hashes it */
      mrb_sym add = mrb_intern_lit(mrb, "add");
      struct RClass *setc = NULL;
      size_t end = r.nhent;
      while (end > 0) {
        size_t start = end;
        while (start > 0 && r.hents[start - 1].h == r.hents[end - 1].h) start--;
        for (size_t i = start; i < end; i++) {
          hent *e = &r.hents[i];
          if (!e->set) {
            mrb_hash_set(mrb, mrb_obj_value(e->h), e->k, e->v);
          }
          else {
            /* Set#add of the built-in class, whatever a subclass did */
            if (!setc) setc = mrb_class_get(mrb, "Set");
            struct RClass *keep = e->h->c;
            e->h->c = setc;
            mrb_funcall_argv(mrb, mrb_obj_value(e->h), add, 1, &e->k);
            e->h->c = keep;
          }
        }
        end = start;
      }
      mrb->jmp = prev_jmp;
    }
    MRB_CATCH(&c_jmp) {
      mrb->jmp = prev_jmp;
      mrb_value e = mrb->exc ? mrb_obj_value(mrb->exc) : mrb_nil_value();
      mrb->exc = NULL;
      mrb_value m = mrb_nil_p(e) ? mrb_nil_value() : mrb_funcall_id(mrb, e, MRB_SYM(inspect), 0);
      rfail(&r, "a Hash key's #hash or #eql? raised while the Hash was rebuilt: %s",
            mrb_string_p(m) ? RSTRING_PTR(m) : "?");
    }
    MRB_END_EXC(&c_jmp);
    for (size_t i = 0, j; i < r.nhent && !r.failed; i = j) {
      for (j = i; j < r.nhent && r.hents[j].h == r.hents[i].h; j++) {}
      if (!r.hents[i].set && (size_t)mrb_hash_size(mrb, mrb_obj_value(r.hents[i].h)) != j - i) {
        rfail(&r, "a Hash came out with %d entries instead of %d: its keys' #hash or #eql? "
              "changed it while it was rebuilt",
              (int)mrb_hash_size(mrb, mrb_obj_value(r.hents[i].h)), (int)(j - i));
      }
    }
    for (uint32_t i = 0; i < r.nobj && !r.failed; i++) {
      if ((r.okind[i] == GK_HASH || r.okind[i] == GK_SET) && r.ofrozen[i]) r.objs[i]->frozen = 1;
    }
    for (size_t i = 0; i < r.nfl && !r.failed; i++) r.fl[i]->frozen = 1;
  }

  mrb_gc_arena_restore(mrb, ai);
  mrb->gc.disabled = gc_was;

  if (r.failed) {
    /* the root ireps' own reference (their Procs, now garbage, hold the rest) */
    for (uint32_t i = 0; !r.roots_done && r.ireps && i < r.nirep; i++) {
      if (!r.ireps[i] || r.irep_perm[i] || r.irep_parents[i] != 0) continue;
      mrb_irep_decref(mrb, r.ireps[i]);
    }
    /* leave the root context idle again */
    mrb->root_c->ci = mrb->root_c->cibase;
    memset(&r.G->plan, 0, sizeof(r.G->plan));
    r.G->host_task = NULL;
    char msg[600];
    snprintf(msg, sizeof(msg), "restore failed: %s", r.err);
    reader_free(&r);
    mrb_raise(mrb, eCheckpoint, msg);
  }
  reader_free(&r);

  mrb_full_gc(mrb);
  r.G->resumed = 1;

  if (has_gcs) {
    /* the program's GC settings; generational mode goes through GC.generational_mode=
       (it changes the collector's state), while the GC is on and not scheduler-driven */
    mrb_bool gen = (gcs.flags & 2) != 0;
    if (mrb->gc.generational != gen) {
      mrb->gc.disabled = FALSE;
      mrb->gc.sched_driven = FALSE;
      mrb_funcall_id(mrb, mrb_obj_value(mrb_module_get(mrb, "GC")),
                     mrb_intern_lit(mrb, "generational_mode="), 1, mrb_bool_value(gen));
    }
    mrb->gc.interval_ratio = (int)gcs.interval_ratio;
    mrb->gc.step_ratio = (int)gcs.step_ratio;
    mrb->gc.step_limit = (size_t)gcs.step_limit;
    mrb->gc.malloc_threshold = (size_t)gcs.malloc_threshold;
    mrb->gc.debt_limit = (mrb_int)gcs.debt_limit;
    mrb->gc.auto_step = (gcs.flags & 4) != 0;
    mrb->gc.sched_driven = (gcs.flags & 8) != 0;
    mrb->gc.disabled = (gcs.flags & 1) != 0;
  }
}

MRB_API mrb_value
mrb_checkpoint_resume(mrb_state *mrb, const uint8_t *buf, size_t len)
{
  mrb_checkpoint_restore(mrb, buf, len);
  return mrb_checkpoint_continue(mrb);
}
