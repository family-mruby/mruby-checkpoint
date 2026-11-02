/*
** ghost_perm.c - the baseline: what mrb_open() made, and its names
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
#include "ghost_internal.h"

mrb_value mrb_attr_reader(mrb_state *mrb, mrb_value obj);
mrb_value mrb_attr_writer(mrb_state *mrb, mrb_value obj);

/* ---- collecting tables ---- */

typedef struct { gmt_entry *a; int n, cap; mrb_state *mrb; } mt_acc;

static int
mt_cb(mrb_state *mrb, mrb_sym sym, mrb_method_t m, void *p)
{
  mt_acc *acc = (mt_acc*)p;
  if (acc->n == acc->cap) {
    acc->cap = acc->cap ? acc->cap * 2 : 16;
    acc->a = (gmt_entry*)mrb_realloc(mrb, acc->a, sizeof(gmt_entry) * acc->cap);
  }
  acc->a[acc->n].sym = sym;
  acc->a[acc->n].flags = m.flags;
  acc->a[acc->n].ptr = MRB_METHOD_FUNC_P(m) ? (const void*)MRB_METHOD_FUNC(m) : (const void*)MRB_METHOD_PROC(m);
  acc->n++;
  return 0;
}

void
ghost_collect_mt(mrb_state *mrb, struct RClass *c, gmt_entry **out, int *n)
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
    acc->a = (giv_entry*)mrb_realloc(mrb, acc->a, sizeof(giv_entry) * acc->cap);
  }
  acc->a[acc->n].sym = sym;
  acc->a[acc->n].val = v;
  acc->n++;
  return 0;
}

void
ghost_collect_iv(mrb_state *mrb, mrb_value obj, giv_entry **out, int *n)
{
  iv_acc acc = { NULL, 0, 0 };
  mrb_iv_foreach(mrb, obj, iv_cb, &acc);
  *out = acc.a; *n = acc.n;
}

void
ghost_collect_gv(mrb_state *mrb, giv_entry **out, int *n)
{
  /* the global table has the same layout as an object's ivar table; walk
     it through a stand-in object (no API exposes the table itself) */
  struct RObject tmp;
  memset(&tmp, 0, sizeof(tmp));
  tmp.tt = MRB_TT_OBJECT;
  tmp.iv = mrb->globals;
  ghost_collect_iv(mrb, mrb_obj_value(&tmp), out, n);
}

/* ---- sorting by symbol name ---- */

static mrb_state *sort_mrb;

/* mrb_sym_name() may return a shared buffer for short symbols, so copy */
static int
sym_cmp(mrb_sym a, mrb_sym b)
{
  char x[256];
  mrb_int la, lb;
  const char *sa = mrb_sym_name_len(sort_mrb, a, &la);
  if (la > 255) la = 255;
  memcpy(x, sa, la); x[la] = 0;
  const char *sb = mrb_sym_name_len(sort_mrb, b, &lb);
  int r = strncmp(x, sb, la < lb ? la : lb);
  if (r) return r;
  return la < lb ? -1 : la > lb ? 1 : 0;
}

static int
cmp_mt(const void *a, const void *b)
{
  return sym_cmp(((const gmt_entry*)a)->sym, ((const gmt_entry*)b)->sym);
}

static int
cmp_iv(const void *a, const void *b)
{
  return sym_cmp(((const giv_entry*)a)->sym, ((const giv_entry*)b)->sym);
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
ghost_content_digest(mrb_state *mrb, struct RBasic *obj)
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
  default:
    break;
  }
  h = fnv_u64(h, obj->frozen);
  return h;
}

/* ---- irep content hash ---- */

uint64_t
ghost_irep_hash(mrb_state *mrb, const mrb_irep *irep)
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
    if (irep->syms[i] == 0) { h = fnv_u64(h, UINT64_MAX); continue; }
    mrb_int len;
    const char *s = mrb_sym_name_len(mrb, irep->syms[i], &len);
    h = fnv_u64(h, (uint64_t)len); h = fnv_bytes(h, s, len);
  }
  h = fnv_u64(h, irep->rlen);
  for (int i = 0; i < irep->rlen; i++) {
    h = fnv_u64(h, (irep->reps && irep->reps[i]) ? ghost_irep_hash(mrb, irep->reps[i]) : 0);
  }
  return h;
}

/* ---- building ---- */

static void
add_name(mrb_state *mrb, ghost_perm *P, const char *name, const void *ptr, int is_func)
{
  if (P->n == P->cap) {
    P->cap = P->cap ? P->cap * 2 : 1024;
    P->names = (char**)mrb_realloc(mrb, P->names, sizeof(char*) * P->cap);
    P->ptrs = (const void**)mrb_realloc(mrb, P->ptrs, sizeof(void*) * P->cap);
    P->is_func = (uint8_t*)mrb_realloc(mrb, P->is_func, P->cap);
  }
  size_t len = strlen(name);
  char *s = (char*)mrb_malloc(mrb, len + 1);
  memcpy(s, name, len + 1);
  P->names[P->n] = s;
  P->ptrs[P->n] = ptr;
  P->is_func[P->n] = (uint8_t)is_func;
  P->n++;
  pm_put(mrb, is_func ? &P->func2name : &P->obj2name, ptr, P->n);
}

static int
named_obj(ghost_perm *P, const void *p)
{
  return pm_get(&P->obj2name, p, NULL);
}

static gsnap*
new_snap(mrb_state *mrb, ghost_perm *P, struct RBasic *obj)
{
  if (P->nsnaps == P->capsnaps) {
    P->capsnaps = P->capsnaps ? P->capsnaps * 2 : 256;
    P->snaps = (gsnap*)mrb_realloc(mrb, P->snaps, sizeof(gsnap) * P->capsnaps);
  }
  gsnap *s = &P->snaps[P->nsnaps++];
  memset(s, 0, sizeof(*s));
  s->obj = obj;
  return s;
}

static void
add_irep(mrb_state *mrb, ghost_perm *P, const mrb_irep *irep)
{
  if (!irep || pm_get(&P->irep2idx, irep, NULL)) return;
  if (P->nireps == P->capireps) {
    P->capireps = P->capireps ? P->capireps * 2 : 256;
    P->ireps = (girep_perm*)mrb_realloc(mrb, P->ireps, sizeof(girep_perm) * P->capireps);
  }
  P->ireps[P->nireps].irep = irep;
  P->ireps[P->nireps].hash = ghost_irep_hash(mrb, irep);
  P->nireps++;
  pm_put(mrb, &P->irep2idx, irep, P->nireps);
  for (int i = 0; irep->reps && i < irep->rlen; i++) add_irep(mrb, P, irep->reps[i]);
}

static void snap_object(mrb_state *mrb, ghost_perm *P, struct RBasic *o);

static void
name_value(mrb_state *mrb, ghost_perm *P, mrb_value v, const char *name)
{
  if (mrb_immediate_p(v)) return;
  struct RBasic *o = mrb_basic_ptr(v);
  if (named_obj(P, o)) return;
  add_name(mrb, P, name, o, 0);
  snap_object(mrb, P, o);
}

static void
snap_object(mrb_state *mrb, ghost_perm *P, struct RBasic *o)
{
  gsnap *s;
  switch (o->tt) {
  case MRB_TT_OBJECT: case MRB_TT_EXCEPTION: case MRB_TT_CDATA:
    s = new_snap(mrb, P, o);
    ghost_collect_iv(mrb, mrb_obj_value(o), &s->iv, &s->niv);
    break;
  case MRB_TT_STRING: case MRB_TT_ARRAY:
    s = new_snap(mrb, P, o);
    s->digest = ghost_content_digest(mrb, o);
    break;
  case MRB_TT_HASH:
    s = new_snap(mrb, P, o);
    s->digest = ghost_content_digest(mrb, o);
    ghost_collect_iv(mrb, mrb_obj_value(o), &s->iv, &s->niv);
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
visit_class(mrb_state *mrb, ghost_perm *P, struct RClass *c, const char *path)
{
  char buf[512];

  if (named_obj(P, c)) return;
  add_name(mrb, P, path, c, 0);

  gsnap *s = new_snap(mrb, P, (struct RBasic*)c);
  s->super = c->super;
  ghost_collect_mt(mrb, c, &s->mt, &s->nmt);
  ghost_collect_iv(mrb, mrb_obj_value(c), &s->iv, &s->niv);

  /* methods, in name order */
  if (s->nmt > 0) {
    gmt_entry *mt = (gmt_entry*)mrb_malloc(mrb, sizeof(gmt_entry) * s->nmt);
    memcpy(mt, s->mt, sizeof(gmt_entry) * s->nmt);
    sort_mrb = mrb;
    qsort(mt, s->nmt, sizeof(gmt_entry), cmp_mt);
    for (int i = 0; i < s->nmt; i++) {
      const char *mname = mrb_sym_name(mrb, mt[i].sym);
      if (mt[i].ptr == NULL) continue; /* undef */
      if (mt[i].flags & MRB_METHOD_FUNC_FL) {
        if (!pm_get(&P->func2name, mt[i].ptr, NULL)) {
          snprintf(buf, sizeof(buf), "F:%s#%s", path, mname);
          add_name(mrb, P, buf, mt[i].ptr, 1);
        }
        continue;
      }
      const struct RProc *p = (const struct RProc*)mt[i].ptr;
      if (!named_obj(P, p)) {
        snprintf(buf, sizeof(buf), "M:%s#%s", path, mname);
        add_name(mrb, P, buf, p, 0);
      }
      if (MRB_PROC_CFUNC_P(p)) {
        if (!pm_get(&P->func2name, (const void*)p->body.func, NULL)) {
          snprintf(buf, sizeof(buf), "F:%s#%s", path, mname);
          add_name(mrb, P, buf, (const void*)p->body.func, 1);
        }
      }
      else if (!MRB_PROC_ALIAS_P(p)) {  /* an alias proc holds a symbol, not an irep */
        add_irep(mrb, P, p->body.irep);
      }
      if (MRB_PROC_ENV_P(p) && !named_obj(P, p->e.env)) {
        snprintf(buf, sizeof(buf), "E:%s#%s", path, mname);
        add_name(mrb, P, buf, p->e.env, 0);
      }
    }
    mrb_free(mrb, mt);
  }

  /* included-module classes directly above */
  int k = 0;
  for (struct RClass *ic = c->super; ic && ic->tt == MRB_TT_ICLASS; ic = ic->super, k++) {
    if (named_obj(P, ic)) continue;
    snprintf(buf, sizeof(buf), "I:%s/%d", path, k);
    add_name(mrb, P, buf, ic, 0);
    gsnap *is = new_snap(mrb, P, (struct RBasic*)ic);
    is->super = ic->super;
  }

  /* singleton class */
  if (c->c && c->c->tt == MRB_TT_SCLASS) {
    snprintf(buf, sizeof(buf), "S:%s", path);
    visit_class(mrb, P, c->c, buf);
  }

  /* ivars and constants, in name order */
  if (s->niv > 0) {
    giv_entry *iv = (giv_entry*)mrb_malloc(mrb, sizeof(giv_entry) * s->niv);
    memcpy(iv, s->iv, sizeof(giv_entry) * s->niv);
    sort_mrb = mrb;
    qsort(iv, s->niv, sizeof(giv_entry), cmp_iv);
    /* plain values first, so a class reached by two constants is named
       by the first one in a stable order */
    for (int i = 0; i < s->niv; i++) {
      mrb_value v = iv[i].val;
      if (mrb_immediate_p(v)) continue;
      enum mrb_vtype t = mrb_type(v);
      const char *ivname = mrb_sym_name(mrb, iv[i].sym);
      if ((t == MRB_TT_CLASS || t == MRB_TT_MODULE) && const_name_p(mrb, iv[i].sym)) {
        if (strcmp(path, "Object") == 0) snprintf(buf, sizeof(buf), "%s", ivname);
        else snprintf(buf, sizeof(buf), "%s::%s", path, ivname);
        visit_class(mrb, P, mrb_class_ptr(v), buf);
      }
      else if (t == MRB_TT_CLASS || t == MRB_TT_MODULE || t == MRB_TT_SCLASS) {
        /* e.g. __attached__: named elsewhere */
      }
      else {
        snprintf(buf, sizeof(buf), "V:%s@%s", path, ivname);
        name_value(mrb, P, v, buf);
      }
    }
    mrb_free(mrb, iv);
  }
}

typedef struct { pmap *heap; mrb_state *mrb; } heap_acc;

static int
heap_cb(mrb_state *mrb, struct RBasic *obj, void *data)
{
  heap_acc *h = (heap_acc*)data;
  if (obj->tt == MRB_TT_FREE) return MRB_EACH_OBJ_OK;
  pm_put(mrb, h->heap, obj, 1);
  return MRB_EACH_OBJ_OK;
}

ghost_perm*
ghost_perm_build(mrb_state *mrb)
{
  char buf[512];
  ghost_perm *P = (ghost_perm*)mrb_calloc(mrb, 1, sizeof(ghost_perm));
  pm_init(&P->obj2name); pm_init(&P->func2name); pm_init(&P->heap); pm_init(&P->irep2idx);

  /* C functions the core puts in closures */
  add_name(mrb, P, "F:attr_reader", (const void*)mrb_attr_reader, 1);
  add_name(mrb, P, "F:attr_writer", (const void*)mrb_attr_writer, 1);

  add_name(mrb, P, "T", mrb->top_self, 0);
  snap_object(mrb, P, (struct RBasic*)mrb->top_self);
  visit_class(mrb, P, mrb->object_class, "Object");
  if (mrb->top_self->c && mrb->top_self->c->tt == MRB_TT_SCLASS) {
    visit_class(mrb, P, mrb->top_self->c, "S:T");
  }
  if (mrb->nomem_err) add_name(mrb, P, "X:nomem_err", mrb->nomem_err, 0);
  if (mrb->stack_err) add_name(mrb, P, "X:stack_err", mrb->stack_err, 0);

  /* globals */
  ghost_collect_gv(mrb, &P->gv, &P->ngv);
  if (P->ngv > 0) {
    giv_entry *gv = (giv_entry*)mrb_malloc(mrb, sizeof(giv_entry) * P->ngv);
    memcpy(gv, P->gv, sizeof(giv_entry) * P->ngv);
    sort_mrb = mrb;
    qsort(gv, P->ngv, sizeof(giv_entry), cmp_iv);
    for (int i = 0; i < P->ngv; i++) {
      mrb_value v = gv[i].val;
      if (mrb_immediate_p(v)) continue;
      enum mrb_vtype t = mrb_type(v);
      if (t == MRB_TT_CLASS || t == MRB_TT_MODULE) {
        snprintf(buf, sizeof(buf), "G:%s", mrb_sym_name(mrb, gv[i].sym));
        visit_class(mrb, P, mrb_class_ptr(v), buf);
      }
      else {
        snprintf(buf, sizeof(buf), "G:%s", mrb_sym_name(mrb, gv[i].sym));
        name_value(mrb, P, v, buf);
      }
    }
    mrb_free(mrb, gv);
  }

  /* every heap object alive now is permanent, named or not */
  heap_acc h = { &P->heap, mrb };
  mrb_objspace_each_objects(mrb, heap_cb, &h);
  return P;
}

void
ghost_perm_free(mrb_state *mrb, ghost_perm *P)
{
  if (!P) return;
  for (uint32_t i = 0; i < P->n; i++) mrb_free(mrb, P->names[i]);
  mrb_free(mrb, P->names); mrb_free(mrb, P->ptrs); mrb_free(mrb, P->is_func);
  pm_free(mrb, &P->obj2name); pm_free(mrb, &P->func2name); pm_free(mrb, &P->heap);
  pm_free(mrb, &P->irep2idx);
  mrb_free(mrb, P->ireps);
  for (uint32_t i = 0; i < P->nsnaps; i++) {
    mrb_free(mrb, P->snaps[i].mt); mrb_free(mrb, P->snaps[i].iv);
  }
  mrb_free(mrb, P->snaps);
  mrb_free(mrb, P->gv);
  mrb_free(mrb, P);
}

const char*
ghost_perm_name_of(ghost_perm *P, const void *obj)
{
  uint32_t i;
  if (!pm_get(&P->obj2name, obj, &i)) return NULL;
  return P->names[i - 1];
}

const char*
ghost_perm_func_name(ghost_perm *P, mrb_func_t f)
{
  uint32_t i;
  if (!pm_get(&P->func2name, (const void*)f, &i)) return NULL;
  return P->names[i - 1];
}

const void*
ghost_perm_lookup(ghost_perm *P, const char *name, size_t len, int *is_func)
{
  for (uint32_t i = 0; i < P->n; i++) {
    if (strlen(P->names[i]) == len && memcmp(P->names[i], name, len) == 0) {
      if (is_func) *is_func = P->is_func[i];
      return P->ptrs[i];
    }
  }
  return NULL;
}

int
ghost_perm_irep(ghost_perm *P, const mrb_irep *irep, uint64_t *hash)
{
  uint32_t i;
  if (!pm_get(&P->irep2idx, irep, &i)) return 0;
  if (hash) *hash = P->ireps[i - 1].hash;
  return 1;
}

const mrb_irep*
ghost_perm_irep_by_hash(ghost_perm *P, uint64_t hash)
{
  for (uint32_t i = 0; i < P->nireps; i++) {
    if (P->ireps[i].hash == hash) return P->ireps[i].irep;
  }
  return NULL;
}

gsnap*
ghost_perm_snap(ghost_perm *P, const void *obj)
{
  for (uint32_t i = 0; i < P->nsnaps; i++) {
    if (P->snaps[i].obj == obj) return &P->snaps[i];
  }
  return NULL;
}
