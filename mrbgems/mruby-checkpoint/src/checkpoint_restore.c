/*
** checkpoint_restore.c - rebuild a checkpoint image (format v0) in a fresh mrb_state
**
** Order:
**   symbols -> ireps -> objects (pass 1: make every object, pass 2: fill
**   the fields) -> changes to permanent objects -> included-module classes
**   -> Hash entries (they may call #hash / #eql? of restored objects) ->
**   frozen bits -> the VM (stack and frames) -> full GC -> continue.
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
#include "checkpoint_internal.h"

typedef struct { struct RBasic *h; mrb_value k, v; } hent;
typedef struct { struct REnv *e; uint32_t off; } senv;

typedef struct {
  mrb_state *mrb;
  checkpoint_state *G;
  checkpoint_perm *P;
  const uint8_t *p, *end;
  int failed;
  char err[512];
  int fill;                 /* object pass: 0 = make, 1 = fill */

  mrb_sym *syms; uint32_t nsym;
  mrb_irep **ireps; uint8_t *irep_perm; uint32_t *irep_parents; uint32_t nirep;
  struct RBasic **objs; uint8_t *okind; uint8_t *ofrozen; uint32_t nobj;

  hent *hents; size_t nhent, caphent;
  senv *senvs; size_t nsenv, capsenv;
  struct RClass **icl; size_t nicl, capicl;
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
  mrb_define_method_raw(r->mrb, c, sym, m);
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
  default: return MRB_TT_FALSE;
  }
}

static struct RBasic*
alloc_obj(reader *r, enum mrb_vtype tt)
{
  return (struct RBasic*)mrb_obj_alloc(r->mrb, tt, NULL);
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

  case GK_STRING: {
    uint32_t len; const uint8_t *s = r_str(r, &len);
    if (!r->fill) {
      mrb_value str = mrb_str_new(mrb, (const char*)s, len);
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
      r->objs[id - 1] = mrb_basic_ptr(mrb_range_new(mrb, mrb_nil_value(), mrb_nil_value(), excl));
    }
    else {
      struct RRange *rg = (struct RRange*)o;
      RANGE_BEG(rg) = b;
      RANGE_END(rg) = e;
      RANGE_EXCL(rg) = excl;
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
    }
    r_ivars(r, o);
    uint32_t n = r_u32(r);
    for (uint32_t i = 0; i < n && !r->failed; i++) r_method(r, c);
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
        if (aspec & 0x80000000u) mrb_proc_set_cfunc_aspec(p, aspec & 0x7fffffffu);
        else p->flags |= MRB_PROC_NOARG;
      }
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
      uint32_t off = r_u32(r);
      if (r->fill) {
        e->cxt = mrb->root_c;
        e->stack = NULL;
        if (r->nsenv == r->capsenv) {
          r->capsenv = r->capsenv ? r->capsenv * 2 : 16;
          r->senvs = (senv*)mrb_realloc(mrb, r->senvs, sizeof(senv) * r->capsenv);
        }
        r->senvs[r->nsenv].e = e; r->senvs[r->nsenv].off = off; r->nsenv++;
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

  case GK_DATA: {
    uint32_t nlen; const uint8_t *name = r_str(r, &nlen);
    uint32_t blen; const uint8_t *blob = r_str(r, &blen);
    if (!r->fill) {
      checkpoint_data_ent *de = data_ent_by_name(r->G, name, nlen);
      if (!de) { rfail(r, "C data type '%.*s' is not in this build's checkpoint data table", (int)nlen, name); break; }
      void *ptr = de->restore(mrb, blob, blen);
      r->objs[id - 1] = (struct RBasic*)mrb_data_object_alloc(mrb, NULL, ptr, de->type);
    }
    r_ivars(r, o);
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

static void
r_vm(reader *r)
{
  mrb_state *mrb = r->mrb;
  struct mrb_context *c = mrb->root_c;

  uint32_t live = r_u32(r);
  if (r->failed) return;
  size_t size = (size_t)(c->stend - c->stbase);
  if (size < (size_t)live + 128) {
    size_t nsize = (size_t)live + 128;
    c->stbase = (mrb_value*)mrb_realloc(mrb, c->stbase, sizeof(mrb_value) * nsize);
    c->stend = c->stbase + nsize;
    size = nsize;
  }
  for (size_t i = 0; i < size; i++) c->stbase[i] = mrb_nil_value();
  for (uint32_t i = 0; i < live && !r->failed; i++) c->stbase[i] = r_value(r);

  uint32_t nframes = r_u32(r);
  if (r->failed) return;
  if (nframes == 0) { rfail(r, "no frames"); return; }
  size_t cisize = (size_t)(c->ciend - c->cibase);
  if (cisize < (size_t)nframes + 16) {
    size_t nsize = (size_t)nframes + 16;
    c->cibase = (mrb_callinfo*)mrb_realloc(mrb, c->cibase, sizeof(mrb_callinfo) * nsize);
    c->ciend = c->cibase + nsize;
    cisize = nsize;
  }
  memset(c->cibase, 0, sizeof(mrb_callinfo) * cisize);
  for (uint32_t k = 0; k < nframes && !r->failed; k++) {
    mrb_callinfo *ci = c->cibase + k;
    ci->mid = r_sym(r);
    ci->proc = (const struct RProc*)r_obj(r);
    uint32_t off = r_u32(r);
    uint32_t pc = r_u32(r);
    ci->n = r_u8(r) & 0xf;
    ci->nk = r_u8(r) & 0xf;
    ci->cci = r_u8(r);
    ci->vis = r_u8(r);
    ci->blk = (struct RProc*)r_obj(r);
    uint8_t uk = r_u8(r);
    struct RBasic *u = r_obj(r);
    if (r->failed) break;
    if (!ci->proc || MRB_PROC_CFUNC_P(ci->proc)) { rfail(r, "frame %u has no Ruby proc", k); break; }
    if (off >= size) { rfail(r, "frame %u stack offset out of range", k); break; }
    ci->stack = c->stbase + off;
    if (pc != UINT32_MAX) {
      if (pc > ci->proc->body.irep->ilen) { rfail(r, "frame %u pc out of range", k); break; }
      ci->pc = ci->proc->body.irep->iseq + pc;
    }
    if (uk == 1) ci->u.env = (struct REnv*)u;
    else if (uk == 2) ci->u.target_class = (struct RClass*)u;
  }
  c->ci = c->cibase + nframes - 1;

  for (size_t i = 0; i < r->nsenv; i++) {
    if (r->senvs[i].off >= size) { rfail(r, "env stack offset out of range"); break; }
    r->senvs[i].e->stack = c->stbase + r->senvs[i].off;
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
}

MRB_API mrb_value
mrb_checkpoint_resume(mrb_state *mrb, const uint8_t *buf, size_t len)
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
       a root irep (no parent here) the Procs now hold it, so drop that one */
    for (uint32_t i = 0; i < r.nirep; i++) {
      if (!r.irep_perm[i] && r.irep_parents[i] == 0 && r.ireps[i]->refcnt > 1) {
        mrb_irep_decref(mrb, r.ireps[i]);
      }
    }
  }
  r.fill = 1;
  if (!r.failed) r_patches(&r);
  if (!r.failed) {
    /* included-module classes share the module's method table */
    for (size_t i = 0; i < r.nicl; i++) {
      struct RClass *ic = r.icl[i];
      struct RClass *m = ic->c;
      MRB_CLASS_ORIGIN(m);
      ic->mt = m->mt;
    }
    /* Hash entries last: #hash and #eql? may be user methods */
    for (size_t i = 0; i < r.nhent; i++) {
      mrb_hash_set(mrb, mrb_obj_value(r.hents[i].h), r.hents[i].k, r.hents[i].v);
    }
    for (uint32_t i = 0; i < r.nobj; i++) {
      if (r.okind[i] != GK_PERM && r.okind[i] != GK_PERM_BODY && r.ofrozen[i]) r.objs[i]->frozen = 1;
    }
  }
  if (!r.failed) r_vm(&r);
  if (!r.failed && r.p != r.end) rfail(&r, "%d bytes left after the image", (int)(r.end - r.p));

#ifndef MRB_NO_METHOD_CACHE
  memset(mrb->cache, 0, sizeof(mrb->cache));
#endif
#ifndef MRB_NO_CONST_CACHE
  memset(mrb->const_cache, 0, sizeof(mrb->const_cache));
#endif

  mrb_gc_arena_restore(mrb, ai);
  mrb->gc.disabled = gc_was;

  if (r.failed) {
    /* leave the root context idle again */
    mrb->root_c->ci = mrb->root_c->cibase;
    char msg[600];
    snprintf(msg, sizeof(msg), "restore failed: %s", r.err);
    reader_free(&r);
    mrb_raise(mrb, eCheckpoint, msg);
  }
  reader_free(&r);

  mrb_full_gc(mrb);
  r.G->resumed = 1;

  mrb_callinfo *top = mrb->root_c->ci;
  return mrb_vm_exec(mrb, top->proc, top->pc);
}
