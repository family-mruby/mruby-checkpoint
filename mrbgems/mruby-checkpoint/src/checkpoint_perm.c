/*
** checkpoint_perm.c - the baseline: what mrb_open() made, and its names
**
** Both sides (the writer before running the program, the reader before
** restoring) walk the fresh state the same way and give every reachable
** permanent object a name built from how it is reached:
**
**   T                  top_self
**   Object, Math::DomainError ...   classes and modules by constant path
**   S:<name>           the singleton class of <name>
**   I:<name>/<k>       the k-th included-module class right above <name>
**   M:<name>#<meth>    the Proc of a method (Ruby-defined methods in mrblib)
**   E:<name>#<meth>    the env of that Proc
**   F:<name>#<meth>    a C function in a method table
**   F:attr_reader ...  C functions the core uses for closures
**   V:<name>@<ivar>    an object held in a class's ivar or constant
**   G:$<name>          an object held in a global variable
**   X:nomem_err ...    preallocated objects of mrb_state
**
** Names are given in a fixed order (sorted by symbol name), so the same
** build gives the same names in every process and on every pointer width.
*/

#include <stdlib.h>
#include <stdio.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/array.h>
#include <mruby/hash.h>
#include <mruby/variable.h>
#include <mruby/irep.h>
#include <mruby/gc.h>
#include <mruby/data.h>
#include <mruby/error.h>
#include <mruby/class.h>
#include <mruby/internal.h>
#include "checkpoint_internal.h"

mrb_value mrb_attr_reader(mrb_state *mrb, mrb_value obj);
mrb_value mrb_attr_writer(mrb_state *mrb, mrb_value obj);

/* ---- collecting tables ----

   The arrays these give are work areas while a dump or the baseline runs
   (ck_realloc): the caller frees them with checkpoint_collect_free. */

static void*
coll_realloc(mrb_state *mrb, void *p, size_t n)
{
  checkpoint_state *g = checkpoint_get(mrb);
  return g ? ck_realloc(g, p, n) : mrb_realloc(mrb, p, n);
}

void
checkpoint_collect_free(mrb_state *mrb, void *p)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (g) ck_free(g, p);
  else mrb_free(mrb, p);
}

typedef struct { gmt_entry *a; int n, cap; mrb_state *mrb; } mt_acc;

static int
mt_cb(mrb_state *mrb, mrb_sym sym, mrb_method_t m, void *p)
{
  mt_acc *acc = (mt_acc*)p;
  if (acc->n == acc->cap) {
    acc->cap = acc->cap ? acc->cap * 2 : 16;
    acc->a = (gmt_entry*)coll_realloc(mrb, acc->a, sizeof(gmt_entry) * acc->cap);
  }
  acc->a[acc->n].sym = sym;
  acc->a[acc->n].flags = m.flags;
  acc->a[acc->n].ptr = MRB_METHOD_FUNC_P(m) ? (const void*)MRB_METHOD_FUNC(m) : (const void*)MRB_METHOD_PROC(m);
  acc->n++;
  return 0;
}

void
checkpoint_collect_mt(mrb_state *mrb, struct RClass *c, gmt_entry **out, int *n)
{
  mt_acc acc = { NULL, 0, 0, mrb };
  mrb_mt_foreach(mrb, c, mt_cb, &acc);
  *out = acc.a; *n = acc.n;
}

typedef struct { giv_entry *a; int n, cap; } iv_acc;

static int
iv_cb(mrb_state *mrb, mrb_sym sym, mrb_value v, void *p)
{
  iv_acc *acc = (iv_acc*)p;
  if (acc->n == acc->cap) {
    acc->cap = acc->cap ? acc->cap * 2 : 16;
    acc->a = (giv_entry*)coll_realloc(mrb, acc->a, sizeof(giv_entry) * acc->cap);
  }
  acc->a[acc->n].sym = sym;
  acc->a[acc->n].val = v;
  acc->n++;
  return 0;
}

void
checkpoint_collect_iv(mrb_state *mrb, mrb_value obj, giv_entry **out, int *n)
{
  iv_acc acc = { NULL, 0, 0 };
  mrb_iv_foreach(mrb, obj, iv_cb, &acc);
  *out = acc.a; *n = acc.n;
}

void
checkpoint_collect_gv(mrb_state *mrb, giv_entry **out, int *n)
{
  /* the global table has the same layout as an object's ivar table; walk
     it through a stand-in object (no API exposes the table itself) */
  struct RObject tmp;
  memset(&tmp, 0, sizeof(tmp));
  tmp.tt = MRB_TT_OBJECT;
  tmp.iv = mrb->globals;
  checkpoint_collect_iv(mrb, mrb_obj_value(&tmp), out, n);
}

/* ---- sorting by symbol name ----

   qsort() has no argument for the comparison's context, and the names belong
   to one mrb_state: a merge sort that carries it (no static, so that several
   states can build their baselines at once, on several threads). Stable. */

typedef int (*sym_cmp_fn)(mrb_state *mrb, const void *a, const void *b);

/* mrb_sym_name() may return a shared buffer for short symbols, so copy */
static int
sym_cmp(mrb_state *mrb, mrb_sym a, mrb_sym b)
{
  char x[256];
  mrb_int la, lb;
  const char *sa = mrb_sym_name_len(mrb, a, &la);
  if (la > 255) la = 255;
  memcpy(x, sa, la); x[la] = 0;
  const char *sb = mrb_sym_name_len(mrb, b, &lb);
  int r = strncmp(x, sb, la < lb ? la : lb);
  if (r) return r;
  return la < lb ? -1 : la > lb ? 1 : 0;
}

static void
sort_by_name(mrb_state *mrb, void *base, size_t n, size_t size, sym_cmp_fn cmp)
{
  if (n < 2) return;
  uint8_t *a = (uint8_t*)base;
  uint8_t *tmp = (uint8_t*)coll_realloc(mrb, NULL, n * size);
  for (size_t w = 1; w < n; w *= 2) {
    for (size_t lo = 0; lo < n - w; lo += 2 * w) {
      size_t mid = lo + w, hi = lo + 2 * w < n ? lo + 2 * w : n;
      size_t i = lo, j = mid, k = 0;
      while (i < mid && j < hi) {
        if (cmp(mrb, a + j * size, a + i * size) < 0) { memcpy(tmp + k++ * size, a + j++ * size, size); }
        else { memcpy(tmp + k++ * size, a + i++ * size, size); }
      }
      while (i < mid) memcpy(tmp + k++ * size, a + i++ * size, size);
      while (j < hi) memcpy(tmp + k++ * size, a + j++ * size, size);
      memcpy(a + lo * size, tmp, k * size);
    }
  }
  checkpoint_collect_free(mrb, tmp);
}

static int
cmp_mt(mrb_state *mrb, const void *a, const void *b)
{
  return sym_cmp(mrb, ((const gmt_entry*)a)->sym, ((const gmt_entry*)b)->sym);
}

static int
cmp_iv(mrb_state *mrb, const void *a, const void *b)
{
  return sym_cmp(mrb, ((const giv_entry*)a)->sym, ((const giv_entry*)b)->sym);
}

/* ---- digests ---- */

static uint64_t
val_bits(uint64_t h, mrb_value v)
{
  h = fnv_u64(h, (uint64_t)mrb_type(v));
  switch (mrb_type(v)) {
  case MRB_TT_FALSE: case MRB_TT_TRUE:
    return fnv_u64(h, mrb_nil_p(v) ? 0 : 1);
  case MRB_TT_INTEGER:
    return fnv_u64(h, (uint64_t)mrb_integer(v));
  case MRB_TT_FLOAT: {
    double d = mrb_float(v); uint64_t u; memcpy(&u, &d, 8);
    return fnv_u64(h, u);
  }
  case MRB_TT_SYMBOL:
    return fnv_u64(h, (uint64_t)mrb_symbol(v));
  default:
    return fnv_u64(h, (uint64_t)(uintptr_t)mrb_ptr(v));
  }
}

static int
hash_digest_cb(mrb_state *mrb, mrb_value k, mrb_value v, void *p)
{
  uint64_t *h = (uint64_t*)p;
  *h = val_bits(*h, k);
  *h = val_bits(*h, v);
  return 0;
}

uint64_t
checkpoint_content_digest(mrb_state *mrb, struct RBasic *obj)
{
  uint64_t h = FNV_INIT;
  mrb_value v = mrb_obj_value(obj);
  switch (obj->tt) {
  case MRB_TT_STRING:
    h = fnv_bytes(h, RSTRING_PTR(v), RSTRING_LEN(v));
    h = fnv_u64(h, RSTRING_LEN(v));
    break;
  case MRB_TT_ARRAY:
    h = fnv_u64(h, RARRAY_LEN(v));
    for (mrb_int i = 0; i < RARRAY_LEN(v); i++) h = val_bits(h, RARRAY_PTR(v)[i]);
    break;
  case MRB_TT_HASH:
    h = fnv_u64(h, mrb_hash_size(mrb, v));
    mrb_hash_foreach(mrb, mrb_hash_ptr(v), hash_digest_cb, &h);
    break;
  case MRB_TT_ISTRUCT:
    h = fnv_bytes(h, mrb_istruct_ptr(v), ISTRUCT_DATA_SIZE);
    break;
  default:
    break;
  }
  h = fnv_u64(h, obj->frozen);
  return h;
}

/* ---- irep content hash ---- */

uint64_t
checkpoint_irep_hash(mrb_state *mrb, const mrb_irep *irep)
{
  uint64_t h = FNV_INIT;
  h = fnv_u64(h, irep->nlocals);
  h = fnv_u64(h, irep->nregs);
  h = fnv_u64(h, irep->clen);
  h = fnv_u64(h, irep->ilen);
  h = fnv_bytes(h, irep->iseq, irep->ilen + sizeof(struct mrb_irep_catch_handler) * irep->clen);
  h = fnv_u64(h, irep->plen);
  for (int i = 0; i < irep->plen; i++) {
    const mrb_irep_pool *p = &irep->pool[i];
    switch (p->tt) {
    case IREP_TT_INT32:
      h = fnv_u64(h, 'i'); h = fnv_u64(h, (uint64_t)(int64_t)p->u.i32); break;
    case IREP_TT_INT64:
      h = fnv_u64(h, 'i'); h = fnv_u64(h, (uint64_t)p->u.i64); break;
    case IREP_TT_FLOAT: {
      double d = p->u.f; uint64_t u; memcpy(&u, &d, 8);
      h = fnv_u64(h, 'f'); h = fnv_u64(h, u); break;
    }
    case IREP_TT_BIGINT: {
      size_t n = (uint8_t)p->u.str[0] + 2;
      h = fnv_u64(h, 'b'); h = fnv_u64(h, n); h = fnv_bytes(h, p->u.str, n); break;
    }
    default: { /* string */
      size_t n = p->tt >> 2;
      h = fnv_u64(h, 's'); h = fnv_u64(h, n); h = fnv_bytes(h, p->u.str, n); break;
    }
    }
  }
  h = fnv_u64(h, irep->slen);
  for (int i = 0; i < irep->slen; i++) {
    /* (a ROM irep may have slen > 0 and no table: BasicObject#!= in class.c) */
    if (!irep->syms || irep->syms[i] == 0) { h = fnv_u64(h, UINT64_MAX); continue; }
    mrb_int len;
    const char *s = mrb_sym_name_len(mrb, irep->syms[i], &len);
    h = fnv_u64(h, (uint64_t)len); h = fnv_bytes(h, s, len);
  }
  h = fnv_u64(h, irep->rlen);
  for (int i = 0; i < irep->rlen; i++) {
    h = fnv_u64(h, (irep->reps && irep->reps[i]) ? checkpoint_irep_hash(mrb, irep->reps[i]) : 0);
  }
  return h;
}

/* ---- building ----

   The baseline is built in tables that grow, with hash tables for finding
   what has a name already. At the end the hash tables are given back and
   the tables are cut to the size they need (finish_perm): what stays is a
   few flat arrays, with no hash table and no spare room.

   What stays (checkpoint_internal.h, struct checkpoint_perm):
     names     every name, NUL-terminated, one after another in the order
               given, and name_off[i] where name i starts (name_off[n] is
               the end). Nothing in them depends on the state: the same
               build gives the same block, so it can later be a table fixed
               in the firmware.
     ptrs      the object or C function of name i (is_func: a bit each)
     obj_idx, func_idx
               the names of objects / of C functions, as indices sorted by
               their pointer (the name of a pointer: a binary search)
     keep      the hidden Array that keeps every object alive at the
               baseline, sorted by address: it is also the set of those
               objects (a binary search)
     ireps, irep_idx
               the mrblib ireps with their content hashes, in the order
               found, and the same sorted by address
     snaps, mt, iv
               what each permanent object looked like; a snapshot's methods
               and ivars are ranges of the shared arrays mt and iv. A method
               refers to its Proc or C function by name index, not address. */

typedef struct {
  mrb_state *mrb;
  /* names */
  char *blob; uint32_t blen, bcap;
  uint32_t *off; const void **ptrs; uint8_t *isf; uint32_t n, cap;
  pmap obj2name, func2name;   /* pointer -> name index + 1 */
  /* ireps */
  girep_perm *ireps; uint32_t nireps, capireps;
  pmap irep2idx;              /* irep pointer -> index + 1 */
  /* snapshots */
  gsnap *snaps; uint32_t nsnaps, capsnaps;
  gsnap_mt *mt; uint32_t nmt, capmt;
  giv_entry *iv; uint32_t niv, capiv;
} pbuild;

/* room for `need` elements of `size` bytes in a table that grows */
static void*
grow(pbuild *B, void *p, uint32_t *cap, uint32_t need, size_t size, uint32_t first)
{
  if (need <= *cap) return p;
  uint32_t c = *cap ? *cap : first;
  while (c < need) c *= 2;
  p = mrb_realloc(B->mrb, p, (size_t)c * size);
  *cap = c;
  return p;
}

static void
add_name(pbuild *B, const char *name, const void *ptr, int is_func)
{
  uint32_t len = (uint32_t)strlen(name) + 1;
  if (B->blen + len > B->bcap) {
    uint32_t c = B->bcap ? B->bcap : 16384;
    while (c < B->blen + len) c *= 2;
    B->blob = (char*)mrb_realloc(B->mrb, B->blob, c);
    B->bcap = c;
  }
  if (B->n == B->cap) {
    uint32_t c = B->cap ? B->cap * 2 : 1024;
    B->off = (uint32_t*)mrb_realloc(B->mrb, B->off, sizeof(uint32_t) * c);
    B->ptrs = (const void**)mrb_realloc(B->mrb, B->ptrs, sizeof(void*) * c);
    B->isf = (uint8_t*)mrb_realloc(B->mrb, B->isf, c);
    B->cap = c;
  }
  memcpy(B->blob + B->blen, name, len);
  B->off[B->n] = B->blen;
  B->blen += len;
  B->ptrs[B->n] = ptr;
  B->isf[B->n] = (uint8_t)is_func;
  B->n++;
  pm_put(B->mrb, is_func ? &B->func2name : &B->obj2name, ptr, B->n);
}

static int
named_obj(pbuild *B, const void *p)
{
  return pm_get(&B->obj2name, p, NULL);
}

static uint32_t
new_snap(pbuild *B, struct RBasic *obj)
{
  B->snaps = (gsnap*)grow(B, B->snaps, &B->capsnaps, B->nsnaps + 1, sizeof(gsnap), 256);
  gsnap *s = &B->snaps[B->nsnaps];
  memset(s, 0, sizeof(*s));
  s->obj = obj;
  return B->nsnaps++;
}

static void
snap_count(pbuild *B, uint32_t n, const char *what)
{
  mrb_state *mrb = B->mrb;
  if (n > UINT16_MAX) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "checkpoint baseline: too many %s in one object (%d)", what, (int)n);
  }
}

/* the ivars of o, into the shared array (the snapshot's range) */
static void
snap_iv(pbuild *B, uint32_t sidx, struct RBasic *o)
{
  giv_entry *iv; int n;
  checkpoint_collect_iv(B->mrb, mrb_obj_value(o), &iv, &n);
  snap_count(B, (uint32_t)n, "instance variables");
  B->iv = (giv_entry*)grow(B, B->iv, &B->capiv, B->niv + (uint32_t)n, sizeof(giv_entry), 1024);
  if (n > 0) memcpy(B->iv + B->niv, iv, sizeof(giv_entry) * n);
  B->snaps[sidx].iv = B->niv;
  B->snaps[sidx].niv = (uint16_t)n;
  B->niv += (uint32_t)n;
  checkpoint_collect_free(B->mrb, iv);
}

static void
add_irep(pbuild *B, const mrb_irep *irep)
{
  if (!irep || pm_get(&B->irep2idx, irep, NULL)) return;
  B->ireps = (girep_perm*)grow(B, B->ireps, &B->capireps, B->nireps + 1, sizeof(girep_perm), 256);
  B->ireps[B->nireps].irep = irep;
  B->ireps[B->nireps].hash = checkpoint_irep_hash(B->mrb, irep);
  B->nireps++;
  pm_put(B->mrb, &B->irep2idx, irep, B->nireps);
  for (int i = 0; irep->reps && i < irep->rlen; i++) add_irep(B, irep->reps[i]);
}

static void snap_object(pbuild *B, struct RBasic *o);
static void visit_class(pbuild *B, struct RClass *c, const char *path);

static void
name_value(pbuild *B, mrb_value v, const char *name)
{
  if (mrb_immediate_p(v)) return;
  struct RBasic *o = mrb_basic_ptr(v);
  if (named_obj(B, o)) return;
  add_name(B, name, o, 0);
  snap_object(B, o);
  /* an object's own singleton class (ENV's methods live there) */
  if (o->tt != MRB_TT_CLASS && o->tt != MRB_TT_MODULE && o->tt != MRB_TT_SCLASS &&
      o->c && o->c->tt == MRB_TT_SCLASS) {
    char buf[512];
    snprintf(buf, sizeof(buf), "S:%s", name);
    visit_class(B, o->c, buf);
  }
}

static void
snap_object(pbuild *B, struct RBasic *o)
{
  uint32_t s;
  switch (o->tt) {
  case MRB_TT_OBJECT: case MRB_TT_EXCEPTION: case MRB_TT_CDATA:
    s = new_snap(B, o);
    snap_iv(B, s, o);
    break;
  case MRB_TT_STRING: case MRB_TT_ARRAY: case MRB_TT_ISTRUCT:
    s = new_snap(B, o);
    B->snaps[s].u.digest = checkpoint_content_digest(B->mrb, o);
    break;
  case MRB_TT_HASH:
    s = new_snap(B, o);
    B->snaps[s].u.digest = checkpoint_content_digest(B->mrb, o);
    snap_iv(B, s, o);
    break;
  default:
    break;
  }
}

static int
const_name_p(mrb_state *mrb, mrb_sym sym)
{
  const char *s = mrb_sym_name(mrb, sym);
  return s && s[0] >= 'A' && s[0] <= 'Z';
}

static void
visit_class(pbuild *B, struct RClass *c, const char *path)
{
  mrb_state *mrb = B->mrb;
  char buf[512];

  if (named_obj(B, c)) return;
  add_name(B, path, c, 0);

  uint32_t sidx = new_snap(B, (struct RBasic*)c);
  B->snaps[sidx].u.super = c->super;
  B->snaps[sidx].cflags = (c->flags & MRB_FL_CLASS_IS_PREPENDED) ? 1 : 0;
  gmt_entry *mt0; int nmt;
  checkpoint_collect_mt(mrb, c, &mt0, &nmt);
  snap_count(B, (uint32_t)nmt, "methods");
  snap_iv(B, sidx, (struct RBasic*)c);

  /* methods, in name order */
  if (nmt > 0) {
    gmt_entry *mt = (gmt_entry*)mrb_malloc(B->mrb, sizeof(gmt_entry) * nmt);
    memcpy(mt, mt0, sizeof(gmt_entry) * nmt);
    sort_by_name(mrb, mt, nmt, sizeof(gmt_entry), cmp_mt);
    for (int i = 0; i < nmt; i++) {
      const char *mname = mrb_sym_name(mrb, mt[i].sym);
      if (mt[i].ptr == NULL) continue; /* undef */
      if (mt[i].flags & MRB_METHOD_FUNC_FL) {
        if (!pm_get(&B->func2name, mt[i].ptr, NULL)) {
          snprintf(buf, sizeof(buf), "F:%s#%s", path, mname);
          add_name(B, buf, mt[i].ptr, 1);
        }
        continue;
      }
      const struct RProc *p = (const struct RProc*)mt[i].ptr;
      if (!named_obj(B, p)) {
        snprintf(buf, sizeof(buf), "M:%s#%s", path, mname);
        add_name(B, buf, p, 0);
      }
      if (MRB_PROC_CFUNC_P(p)) {
        if (!pm_get(&B->func2name, (const void*)p->body.func, NULL)) {
          snprintf(buf, sizeof(buf), "F:%s#%s", path, mname);
          add_name(B, buf, (const void*)p->body.func, 1);
        }
      }
      else if (!MRB_PROC_ALIAS_P(p)) {  /* an alias proc holds a symbol, not an irep */
        add_irep(B, p->body.irep);
      }
      if (MRB_PROC_ENV_P(p) && !named_obj(B, p->e.env)) {
        snprintf(buf, sizeof(buf), "E:%s#%s", path, mname);
        add_name(B, buf, p->e.env, 0);
      }
    }
    mrb_free(B->mrb, mt);
  }
  /* the snapshot of the methods, in the table's own order, each referring
     to its Proc or C function by name (every one has a name by now) */
  B->mt = (gsnap_mt*)grow(B, B->mt, &B->capmt, B->nmt + (uint32_t)nmt, sizeof(gsnap_mt), 1024);
  B->snaps[sidx].mt = B->nmt;
  B->snaps[sidx].nmt = (uint16_t)nmt;
  for (int i = 0; i < nmt; i++) {
    gsnap_mt *e = &B->mt[B->nmt++];
    uint32_t ref = 0;
    if (mt0[i].ptr) {
      pm_get((mt0[i].flags & MRB_METHOD_FUNC_FL) ? &B->func2name : &B->obj2name, mt0[i].ptr, &ref);
    }
    e->sym = mt0[i].sym;
    e->flags = mt0[i].flags;
    e->ref = ref;
  }
  checkpoint_collect_free(mrb, mt0);

  /* included-module classes directly above */
  int k = 0;
  for (struct RClass *ic = c->super; ic && ic->tt == MRB_TT_ICLASS; ic = ic->super, k++) {
    if (named_obj(B, ic)) continue;
    snprintf(buf, sizeof(buf), "I:%s/%d", path, k);
    add_name(B, buf, ic, 0);
    uint32_t is = new_snap(B, (struct RBasic*)ic);
    B->snaps[is].u.super = ic->super;
  }

  /* singleton class */
  if (c->c && c->c->tt == MRB_TT_SCLASS) {
    snprintf(buf, sizeof(buf), "S:%s", path);
    visit_class(B, c->c, buf);
  }

  /* the superclass, when no constant leads to it (BasicObject: its constant
     is in BasicObject itself, not in Object) */
  {
    struct RClass *sup = c->super;
    while (sup && sup->tt == MRB_TT_ICLASS) sup = sup->super;
    if (sup && sup->tt == MRB_TT_CLASS && !named_obj(B, sup)) {
      mrb_value cp = mrb_class_path(mrb, sup);
      if (mrb_string_p(cp)) {
        snprintf(buf, sizeof(buf), "%.*s", (int)RSTRING_LEN(cp), RSTRING_PTR(cp));
        visit_class(B, sup, buf);
      }
    }
  }

  /* ivars and constants, in name order */
  int niv = B->snaps[sidx].niv;
  if (niv > 0) {
    giv_entry *iv = (giv_entry*)mrb_malloc(B->mrb, sizeof(giv_entry) * niv);
    memcpy(iv, B->iv + B->snaps[sidx].iv, sizeof(giv_entry) * niv);
    sort_by_name(mrb, iv, niv, sizeof(giv_entry), cmp_iv);
    /* plain values first, so a class reached by two constants is named
       by the first one in a stable order */
    for (int i = 0; i < niv; i++) {
      mrb_value v = iv[i].val;
      if (mrb_immediate_p(v)) continue;
      enum mrb_vtype t = mrb_type(v);
      const char *ivname = mrb_sym_name(mrb, iv[i].sym);
      if ((t == MRB_TT_CLASS || t == MRB_TT_MODULE) && const_name_p(mrb, iv[i].sym)) {
        /* prefer the class's own name (Integer, not its alias Fixnum) */
        mrb_value cp = mrb_class_path(mrb, mrb_class_ptr(v));
        if (mrb_string_p(cp)) snprintf(buf, sizeof(buf), "%.*s", (int)RSTRING_LEN(cp), RSTRING_PTR(cp));
        else if (strcmp(path, "Object") == 0) snprintf(buf, sizeof(buf), "%s", ivname);
        else snprintf(buf, sizeof(buf), "%s::%s", path, ivname);
        visit_class(B, mrb_class_ptr(v), buf);
      }
      else if (t == MRB_TT_CLASS || t == MRB_TT_MODULE || t == MRB_TT_SCLASS) {
        /* e.g. __attached__: named elsewhere */
      }
      else {
        snprintf(buf, sizeof(buf), "V:%s@%s", path, ivname);
        name_value(B, v, buf);
      }
    }
    mrb_free(B->mrb, iv);
  }
}

/* ---- the flat tables that stay ---- */

typedef struct { uintptr_t key; uint32_t idx; } keyed;

static int
cmp_keyed(const void *a, const void *b)
{
  uintptr_t x = ((const keyed*)a)->key, y = ((const keyed*)b)->key;
  return x < y ? -1 : x > y ? 1 : 0;
}

static int
cmp_uintptr(const void *a, const void *b)
{
  uintptr_t x = *(const uintptr_t*)a, y = *(const uintptr_t*)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

/* the indices 0..n-1 that pick takes (all without it), sorted by key (NULL
   when none) */
static uint32_t*
sorted_index(pbuild *B, uint32_t n, uintptr_t (*key)(pbuild*, uint32_t), int (*pick)(pbuild*, uint32_t),
             uint32_t *count)
{
  uint32_t m = 0;
  for (uint32_t i = 0; i < n; i++) if (!pick || pick(B, i)) m++;
  *count = m;
  if (m == 0) return NULL;
  keyed *k = (keyed*)mrb_malloc(B->mrb, sizeof(keyed) * m);
  m = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (pick && !pick(B, i)) continue;
    k[m].key = key(B, i);
    k[m].idx = i;
    m++;
  }
  qsort(k, m, sizeof(keyed), cmp_keyed);
  uint32_t *idx = (uint32_t*)mrb_malloc(B->mrb, sizeof(uint32_t) * m);
  for (uint32_t i = 0; i < m; i++) idx[i] = k[i].idx;
  mrb_free(B->mrb, k);
  return idx;
}

static uintptr_t name_key(pbuild *B, uint32_t i) { return (uintptr_t)B->ptrs[i]; }
static int pick_obj(pbuild *B, uint32_t i) { return !B->isf[i]; }
static int pick_func(pbuild *B, uint32_t i) { return B->isf[i]; }
static uintptr_t irep_key(pbuild *B, uint32_t i) { return (uintptr_t)B->ireps[i].irep; }

/* a copy of n bytes (NULL for none) */
static void*
keep_copy(mrb_state *mrb, const void *p, size_t n)
{
  if (n == 0) return NULL;
  void *q = mrb_malloc(mrb, n);
  memcpy(q, p, n);
  return q;
}

/* a table that grew, cut to n bytes (in place: the allocator gives the
   rest back) */
static void*
shrink(mrb_state *mrb, void *p, size_t n)
{
  if (n == 0) {
    mrb_free(mrb, p);
    return NULL;
  }
  return mrb_realloc(mrb, p, n);
}

static void
finish_perm(pbuild *B, checkpoint_perm *P)
{
  mrb_state *mrb = B->mrb;

  /* the hash tables first, so that the indices below can take their room */
  pm_free(mrb, &B->obj2name); pm_free(mrb, &B->func2name); pm_free(mrb, &B->irep2idx);

  P->n = B->n;
  P->names = (char*)shrink(mrb, B->blob, B->blen);
  P->name_off = (uint32_t*)mrb_realloc(mrb, B->off, sizeof(uint32_t) * (B->n + 1));
  P->name_off[B->n] = B->blen;
  P->ptrs = (const void**)shrink(mrb, B->ptrs, sizeof(void*) * B->n);
  B->ptrs = P->ptrs;   /* (name_key reads it) */
  P->is_func = (uint8_t*)mrb_calloc(mrb, (B->n + 7) / 8 + 1, 1);
  for (uint32_t i = 0; i < B->n; i++) {
    if (B->isf[i]) P->is_func[i / 8] |= (uint8_t)(1 << (i % 8));
  }
  P->obj_idx = sorted_index(B, B->n, name_key, pick_obj, &P->nobj);
  P->func_idx = sorted_index(B, B->n, name_key, pick_func, &P->nfunc);
  mrb_free(mrb, B->isf);

  P->nireps = B->nireps;
  P->ireps = (girep_perm*)shrink(mrb, B->ireps, sizeof(girep_perm) * B->nireps);
  B->ireps = P->ireps;   /* (irep_key reads it) */
  P->irep_idx = sorted_index(B, B->nireps, irep_key, NULL, &P->nireps);

  P->nsnaps = B->nsnaps;
  P->snaps = (gsnap*)shrink(mrb, B->snaps, sizeof(gsnap) * B->nsnaps);
  P->nmt = B->nmt;
  P->mt = (gsnap_mt*)shrink(mrb, B->mt, sizeof(gsnap_mt) * B->nmt);
  P->niv = B->niv;
  P->iv = (giv_entry*)shrink(mrb, B->iv, sizeof(giv_entry) * B->niv);
}


void
checkpoint_gc_settings(mrb_state *mrb, gc_settings *g)
{
  memset(g, 0, sizeof(*g));
  g->flags = (uint8_t)((mrb->gc.disabled ? 1 : 0) | (mrb->gc.generational ? 2 : 0) |
                       (mrb->gc.auto_step ? 4 : 0) | (mrb->gc.sched_driven ? 8 : 0));
  g->interval_ratio = (uint32_t)mrb->gc.interval_ratio;
  g->step_ratio = (uint32_t)mrb->gc.step_ratio;
  g->step_limit = (uint64_t)mrb->gc.step_limit;
  g->malloc_threshold = (uint64_t)mrb->gc.malloc_threshold;
  g->debt_limit = (int64_t)mrb->gc.debt_limit;
}

/* C functions the bundled gems put into classes they make at run time
   (Struct.new, Data.define): static there, so a class is made for the
   purpose before the baseline and the C functions of its methods and of
   its singleton class's methods are named after the maker. The list is the
   state's (checkpoint_state.made), found at its first baseline. */
#define MAX_MADE_FUNCS 64

typedef struct {
  checkpoint_state *g;
  const char *prefix;
} made_ctx;

static int
made_cb(mrb_state *mrb, mrb_sym sym, mrb_method_t m, void *ud)
{
  made_ctx *x = (made_ctx*)ud;
  checkpoint_state *g = x->g;
  mrb_func_t f = NULL;
  if (MRB_METHOD_FUNC_P(m)) f = MRB_METHOD_FUNC(m);
  else if (MRB_METHOD_PROC(m) && MRB_PROC_CFUNC_P(MRB_METHOD_PROC(m))) f = MRB_METHOD_PROC(m)->body.func;
  if (!f || g->nmade >= MAX_MADE_FUNCS) return 0;
  for (int i = 0; i < g->nmade; i++) if (g->made[i].f == f) return 0;
  char name[128];
  snprintf(name, sizeof(name), "F:%s#%s", x->prefix, mrb_sym_name(mrb, sym));
  g->made = (struct checkpoint_made_func*)mrb_realloc(mrb, g->made, sizeof(g->made[0]) * (g->nmade + 1));
  g->made[g->nmade].name = checkpoint_strdup(mrb, name);
  g->made[g->nmade].f = f;
  g->nmade++;
  return 0;
}

static void
made_class(mrb_state *mrb, checkpoint_state *g, const char *maker, const char *meth, const char *label)
{
  if (!mrb_class_defined(mrb, maker)) return;
  /* enough members to see every kind of accessor (mruby-data has direct
     functions for the first four, a closure after that) */
  mrb_value a[6];
  for (int i = 0; i < 6; i++) {
    char n[2] = { (char)('a' + i), 0 };
    a[i] = mrb_symbol_value(mrb_intern_cstr(mrb, n));
  }
  mrb_value k = mrb_funcall_argv(mrb, mrb_obj_value(mrb_class_get(mrb, maker)), mrb_intern_cstr(mrb, meth), 6, a);
  if (!mrb_class_p(k)) return;
  char buf[64];
  made_ctx x = { g, label };
  mrb_mt_foreach(mrb, mrb_class_ptr(k), made_cb, &x);
  snprintf(buf, sizeof(buf), "S:%s", label);
  struct RClass *sc = mrb_class_ptr(k)->c;
  x.prefix = buf;
  if (sc && sc->tt == MRB_TT_SCLASS) mrb_mt_foreach(mrb, sc, made_cb, &x);
}

static mrb_value
find_closures(mrb_state *mrb, void *ud)
{
  checkpoint_state *g = (checkpoint_state*)ud;
  made_class(mrb, g, "Struct", "new", "Struct.new");
  made_class(mrb, g, "Data", "define", "Data.define");
  return mrb_nil_value();
}
typedef struct { pbuild *B; uintptr_t *a; uint32_t n, cap; } heap_acc;

static int
heap_cb(mrb_state *mrb, struct RBasic *obj, void *data)
{
  heap_acc *h = (heap_acc*)data;
  (void)mrb;
  if (obj->tt == MRB_TT_FREE) return MRB_EACH_OBJ_OK;
  h->a = (uintptr_t*)grow(h->B, h->a, &h->cap, h->n + 1, sizeof(uintptr_t), 1024);
  h->a[h->n++] = (uintptr_t)obj;
  return MRB_EACH_OBJ_OK;
}

static checkpoint_perm*
build_perm(mrb_state *mrb, checkpoint_state *g, pbuild *B)
{
  char buf[512];
  checkpoint_perm *P = (checkpoint_perm*)mrb_calloc(mrb, 1, sizeof(checkpoint_perm));

  /* every heap object alive now is permanent, named or not. None of them
     may be freed later (the program can drop the last reference, e.g.
     remove_const, and a new object would then take the slot and the name):
     a hidden Array keeps them all alive, in an ivar of Checkpoint whose
     name Ruby code cannot spell. Its elements are sorted by address, so
     the same Array is the set of these objects (checkpoint_perm_heap_p). */
  {
    heap_acc h = { B, NULL, 0, 0 };
    mrb_objspace_each_objects(mrb, heap_cb, &h);
    if (h.n > 1) qsort(h.a, h.n, sizeof(uintptr_t), cmp_uintptr);
    int ai = mrb_gc_arena_save(mrb);
    mrb_value keep = mrb_ary_new_capa(mrb, (mrb_int)h.n);
    for (uint32_t i = 0; i < h.n; i++) mrb_ary_push(mrb, keep, mrb_obj_value((void*)h.a[i]));
    mrb_free(mrb, h.a);
    mrb_obj_ptr(keep)->c = NULL;   /* hidden from ObjectSpace, like the GC roots */
    P->keep = mrb_ary_ptr(keep);
    struct RClass *ck = mrb_module_get(mrb, "Checkpoint");
    mrb_obj_iv_set(mrb, (struct RObject*)ck, mrb_intern_lit(mrb, "__checkpoint_keep__"), keep);
    mrb_gc_arena_restore(mrb, ai);
  }

  /* C functions the core puts in closures */
  add_name(B, "F:attr_reader", (const void*)mrb_attr_reader, 1);
  add_name(B, "F:attr_writer", (const void*)mrb_attr_writer, 1);
  /* ... and the bundled gems: the accessors Struct.new and Data.define make
     (static functions there; found through a class made for the purpose) */
  /* (the functions of the classes gems make at run time come last) */

  add_name(B, "T", mrb->top_self, 0);
  snap_object(B, (struct RBasic*)mrb->top_self);
  visit_class(B, mrb->object_class, "Object");
  if (mrb->top_self->c && mrb->top_self->c->tt == MRB_TT_SCLASS) {
    visit_class(B, mrb->top_self->c, "S:T");
  }
  if (mrb->nomem_err) add_name(B, "X:nomem_err", mrb->nomem_err, 0);
  if (mrb->stack_err) add_name(B, "X:stack_err", mrb->stack_err, 0);

  /* globals */
  giv_entry *gv; int ngv;
  checkpoint_collect_gv(mrb, &gv, &ngv);
  P->ngv = ngv;
  P->gv = (giv_entry*)keep_copy(mrb, gv, sizeof(giv_entry) * ngv);
  if (ngv > 0) {
    sort_by_name(mrb, gv, ngv, sizeof(giv_entry), cmp_iv);
    for (int i = 0; i < ngv; i++) {
      mrb_value v = gv[i].val;
      if (mrb_immediate_p(v)) continue;
      enum mrb_vtype t = mrb_type(v);
      if (t == MRB_TT_CLASS || t == MRB_TT_MODULE) {
        snprintf(buf, sizeof(buf), "G:%s", mrb_sym_name(mrb, gv[i].sym));
        visit_class(B, mrb_class_ptr(v), buf);
      }
      else {
        snprintf(buf, sizeof(buf), "G:%s", mrb_sym_name(mrb, gv[i].sym));
        name_value(B, v, buf);
      }
    }
  }
  checkpoint_collect_free(mrb, gv);

  for (int i = 0; i < g->nmade; i++) {
    if (!pm_get(&B->func2name, (const void*)g->made[i].f, NULL)) {
      add_name(B, g->made[i].name, (const void*)g->made[i].f, 1);
    }
  }
  /* the embedder's (mrb_checkpoint_register_func) */
  for (int i = 0; i < g->nfuncs; i++) {
    if (!pm_get(&B->func2name, (const void*)g->funcs[i], NULL)) {
      add_name(B, g->fnames[i], (const void*)g->funcs[i], 1);
    }
  }

  checkpoint_gc_settings(mrb, &P->gc);
  finish_perm(B, P);
  return P;
}

checkpoint_perm*
checkpoint_perm_build(mrb_state *mrb, checkpoint_state *g)
{
  if (!g->made_found) {
    /* once per state: the made classes are garbage before the heap is
       taken below */
    mrb_bool err;
    g->made_found = 1;
    int ai = mrb_gc_arena_save(mrb);
    mrb_protect_error(mrb, find_closures, g, &err);
    mrb_gc_arena_restore(mrb, ai);
    mrb->exc = NULL;
    mrb_full_gc(mrb);
  }
  pbuild B;
  memset(&B, 0, sizeof(B));
  B.mrb = mrb;
  pm_init(&B.obj2name); pm_init(&B.func2name); pm_init(&B.irep2idx);
  return build_perm(mrb, g, &B);
}

void
checkpoint_perm_free(mrb_state *mrb, checkpoint_perm *P)
{
  if (!P) return;
  mrb_free(mrb, P->names); mrb_free(mrb, P->name_off);
  mrb_free(mrb, P->ptrs); mrb_free(mrb, P->is_func);
  mrb_free(mrb, P->obj_idx); mrb_free(mrb, P->func_idx);
  mrb_free(mrb, P->ireps); mrb_free(mrb, P->irep_idx);
  mrb_free(mrb, P->snaps); mrb_free(mrb, P->mt); mrb_free(mrb, P->iv);
  mrb_free(mrb, P->gv);
  mrb_free(mrb, P);
}

/* ---- looking things up ---- */

/* the entry of idx[0..n) whose key (keys of an index) is k, or -1 */
static int64_t
idx_find(const uint32_t *idx, uint32_t n, const void *const *keys, uintptr_t k)
{
  uint32_t lo = 0, hi = n;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    uintptr_t m = (uintptr_t)keys[idx[mid]];
    if (m == k) return idx[mid];
    if (m < k) lo = mid + 1;
    else hi = mid;
  }
  return -1;
}

const char*
checkpoint_perm_name_of(checkpoint_perm *P, const void *obj)
{
  if (!obj) return NULL;
  int64_t i = idx_find(P->obj_idx, P->nobj, P->ptrs, (uintptr_t)obj);
  return i < 0 ? NULL : P->names + P->name_off[i];
}

const char*
checkpoint_perm_func_name(checkpoint_perm *P, mrb_func_t f)
{
  if (!f) return NULL;
  int64_t i = idx_find(P->func_idx, P->nfunc, P->ptrs, (uintptr_t)f);
  return i < 0 ? NULL : P->names + P->name_off[i];
}

int
checkpoint_perm_heap_p(checkpoint_perm *P, const void *obj)
{
  if (!obj || !P->keep) return 0;
  if (obj == (const void*)P->keep) return 1;
  const mrb_value *a = ARY_PTR(P->keep);
  mrb_int lo = 0, hi = ARY_LEN(P->keep);
  uintptr_t k = (uintptr_t)obj;
  while (lo < hi) {
    mrb_int mid = lo + (hi - lo) / 2;
    uintptr_t m = (uintptr_t)mrb_ptr(a[mid]);
    if (m == k) return 1;
    if (m < k) lo = mid + 1;
    else hi = mid;
  }
  return 0;
}

const void*
checkpoint_perm_lookup(checkpoint_perm *P, const char *name, size_t len, int *is_func)
{
  for (uint32_t i = 0; i < P->n; i++) {
    if (P->name_off[i + 1] - P->name_off[i] - 1 == len &&
        memcmp(P->names + P->name_off[i], name, len) == 0) {
      if (is_func) *is_func = (P->is_func[i / 8] >> (i % 8)) & 1;
      return P->ptrs[i];
    }
  }
  return NULL;
}

uint32_t
checkpoint_perm_count(checkpoint_perm *P)
{
  return P->n;
}

const char*
checkpoint_perm_name_at(checkpoint_perm *P, uint32_t i, int *is_func)
{
  if (i >= P->n) return NULL;
  if (is_func) *is_func = (P->is_func[i / 8] >> (i % 8)) & 1;
  return P->names + P->name_off[i];
}

const void*
checkpoint_perm_ref(checkpoint_perm *P, uint32_t ref)
{
  return (ref == 0 || ref > P->n) ? NULL : P->ptrs[ref - 1];
}

int
checkpoint_perm_irep(checkpoint_perm *P, const mrb_irep *irep, uint64_t *hash)
{
  uint32_t lo = 0, hi = P->nireps;
  uintptr_t k = (uintptr_t)irep;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    const girep_perm *e = &P->ireps[P->irep_idx[mid]];
    uintptr_t m = (uintptr_t)e->irep;
    if (m == k) {
      if (hash) *hash = e->hash;
      return 1;
    }
    if (m < k) lo = mid + 1;
    else hi = mid;
  }
  return 0;
}

const mrb_irep*
checkpoint_perm_irep_by_hash(checkpoint_perm *P, uint64_t hash)
{
  for (uint32_t i = 0; i < P->nireps; i++) {
    if (P->ireps[i].hash == hash) return P->ireps[i].irep;
  }
  return NULL;
}

gsnap*
checkpoint_perm_snap(checkpoint_perm *P, const void *obj)
{
  for (uint32_t i = 0; i < P->nsnaps; i++) {
    if (P->snaps[i].obj == obj) return &P->snaps[i];
  }
  return NULL;
}
