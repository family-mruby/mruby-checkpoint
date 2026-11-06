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
**   <name>/<step>      an object without a name of its own, reached from a
**                      named one: /3 (element), /{"en"} /{:sym} /{7} (the
**                      value of that key), /k{"en"} (the key itself), /v#2
**                      /k#2 (pair 2, other keys), /@<ivar>, /mesg, /begin,
**                      /end, /env
**
** Names are given in a fixed order (sorted by symbol name), so the same
** build gives the same names in every process and on every pointer width.
** The objects reached from named ones are named last, breadth first (see
** "named by a path" below), so they never take a name another rule gives.
*/

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <mruby/proc.h>
#include <mruby/string.h>
#include <mruby/array.h>
#include <mruby/hash.h>
#include <mruby/range.h>
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
  case MRB_TT_STRING:
    /* a String held is part of its holder's contents: a String reached
       only as an element or a value has no name and no snapshot of its own
       (see "named by a path"), so a change in it shows here */
    h = fnv_u64(h, (uint64_t)(uintptr_t)mrb_ptr(v));
    h = fnv_bytes(h, RSTRING_PTR(v), RSTRING_LEN(v));
    return fnv_u64(h, ((uint64_t)RSTRING_LEN(v) << 1) | mrb_frozen_p(mrb_basic_ptr(v)));
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
  case MRB_TT_STRUCT:   /* the members are an Array's body (mruby-struct) */
    h = fnv_u64(h, RARRAY_LEN(v));
    for (mrb_int i = 0; i < RARRAY_LEN(v); i++) h = val_bits(h, RARRAY_PTR(v)[i]);
    break;
  case MRB_TT_SET: {
    mrb_value a = checkpoint_set_elements(mrb, obj);
    if (!mrb_array_p(a)) { h = fnv_u64(h, UINT64_MAX); break; }
    h = fnv_u64(h, RARRAY_LEN(a));
    for (mrb_int i = 0; i < RARRAY_LEN(a); i++) {
      mrb_value e = RARRAY_PTR(a)[i];
      h = val_bits(h, e);
      /* an element other than a String gets no name (a Set's order may
         follow addresses: "named by a path"): its own contents count here,
         so a change in it is seen (and refused, as it cannot be named) */
      switch (mrb_type(e)) {
      case MRB_TT_ARRAY: case MRB_TT_HASH: case MRB_TT_STRUCT: case MRB_TT_OBJECT: {
        h = fnv_u64(h, checkpoint_content_digest(mrb, mrb_basic_ptr(e)));
        giv_entry *iv; int n;
        checkpoint_collect_iv(mrb, e, &iv, &n);
        for (int j = 0; j < n; j++) { h = fnv_u64(h, (uint64_t)iv[j].sym); h = val_bits(h, iv[j].val); }
        checkpoint_collect_free(mrb, iv);
        break;
      }
      default:
        break;
      }
    }
    break;
  }
  case MRB_TT_ENV: {
    struct REnv *e = (struct REnv*)obj;
    if (MRB_ENV_ONSTACK_P(e)) { h = fnv_u64(h, UINT64_MAX); break; }
    h = fnv_u64(h, MRB_ENV_LEN(e));
    for (mrb_int i = 0; i < MRB_ENV_LEN(e); i++) h = val_bits(h, e->stack[i]);
    break;
  }
  case MRB_TT_EXCEPTION: {
    struct RException *e = (struct RException*)obj;
    h = val_bits(h, e->mesg ? mrb_obj_value(e->mesg) : mrb_nil_value());
    break;
  }
  case MRB_TT_CDATA: {
    /* the bytes its registered dump writes (none registered: nothing) */
    checkpoint_state *g = checkpoint_get(mrb);
    struct RData *rd = (struct RData*)obj;
    for (int i = 0; g && rd->type && i < g->ndata; i++) {
      if (g->data[i].type != rd->type || !g->data[i].dump) continue;
      gbuf b;
      gb_init(&b);
      int rc = g->data[i].dump(mrb, v, &b);
      h = fnv_u64(h, (uint64_t)rc);
      if (rc == 0) h = fnv_bytes(h, b.p, b.len);
      gb_free(mrb, &b);
      break;
    }
    break;
  }
  default:
    break;
  }
  h = fnv_u64(h, obj->frozen);
  return h;
}

/* a Set's elements (Set#to_a), or nil while it is not initialized
   (Set.allocate): RSet embeds a khash whose first field is its table
   (mruby-set/src/set.c, mruby/khash.h), NULL until Set#initialize ran */
mrb_value
checkpoint_set_elements(mrb_state *mrb, struct RBasic *o)
{
  struct { MRB_OBJECT_HEADER; void *data; } *rs = (void*)o;
  if (rs->data == NULL) return mrb_nil_value();
  return mrb_funcall_argv(mrb, mrb_obj_value(o), mrb_intern_lit(mrb, "to_a"), 0, NULL);
}

/* what a snapshot of an object of this type holds: SNAP_DIGEST its contents
   (checkpoint_content_digest), SNAP_IVARS its ivars (classes have their own
   kind of snapshot) */
int
checkpoint_snap_kind(enum mrb_vtype tt)
{
  switch (tt) {
  case MRB_TT_OBJECT: return SNAP_IVARS;
  case MRB_TT_CDATA: case MRB_TT_EXCEPTION: case MRB_TT_HASH:
  case MRB_TT_STRUCT: case MRB_TT_SET:
    return SNAP_DIGEST | SNAP_IVARS;
  case MRB_TT_STRING: case MRB_TT_ARRAY: case MRB_TT_ISTRUCT: case MRB_TT_ENV:
    return SNAP_DIGEST;
  default:
    return 0;
  }
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
  /* with a table (mrb_checkpoint_set_table): each name the walk gives is
     looked up in T, and only the state's own part is kept (in P): the
     address of a table name, and the names and snapshots T does not have
     as they would be kept without a table (blob/off/isf and snaps/mt/iv
     above). The first difference is noted in why, for the status. */
  const mrb_checkpoint_table *T;
  checkpoint_perm *P;
  int failed;
  char why[200];
  uint32_t *name2snap;                  /* T name -> its snapshot + 1 */
  struct RBasic **xobj; uint32_t nxobj, capxobj;   /* snapshots of the state's own */
  /* the named objects whose contents are named by a path at the end, in
     the order named (name_reached), and two whose contents are not the
     program's: the hidden Array that keeps the baseline alive, the C side's
     GC roots */
  struct { struct RBasic *o; char *name; } *q; uint32_t nq, capq;
  const void *noseed[2];
  pmap leaves;                /* Strings seen once as an element or a value */
  uint32_t ndiff;                       /* table snapshots this state sees otherwise */
} pbuild;

static void
mismatch(pbuild *B, const char *fmt, ...)
{
  if (B->failed) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(B->why, sizeof(B->why), fmt, ap);
  va_end(ap);
  B->failed = 1;
}

static const char*
tname(const mrb_checkpoint_table *T, uint32_t i)
{
  return T->names + T->name_off[i];
}

static int
tbit(const uint8_t *bits, uint32_t i)
{
  return (bits[i / 8] >> (i % 8)) & 1;
}

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
  if (B->T) {
    /* a table name: its address; a name of the state's own: kept as below,
       numbered after the table's */
    const mrb_checkpoint_table *T = B->T;
    uint32_t lo = 0, hi = T->nnames;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      int r = strcmp(name, tname(T, T->by_name[mid]));
      if (r == 0) { lo = mid; break; }
      if (r > 0) lo = mid + 1;
      else hi = mid;
    }
    if (lo < hi) {
      uint32_t t = T->by_name[lo];
      if (tbit(T->is_func, t) == is_func && !B->P->ptrs[t]) {
        B->P->ptrs[t] = ptr;
        pm_put(B->mrb, is_func ? &B->func2name : &B->obj2name, ptr, t + 1);
        return;
      }
    }
    mismatch(B, "%s%s is not in the table", is_func ? "C function " : "", name);
    uint32_t idx = T->nnames + B->n;   /* (B->n counts the state's own here) */
    pm_put(B->mrb, is_func ? &B->func2name : &B->obj2name, ptr, idx + 1);
    /* kept as without a table (finish_table moves them after the table's) */
    uint32_t len = (uint32_t)strlen(name) + 1;
    if (B->blen + len > B->bcap) {
      uint32_t c = B->bcap ? B->bcap : 1024;
      while (c < B->blen + len) c *= 2;
      B->blob = (char*)mrb_realloc(B->mrb, B->blob, c);
      B->bcap = c;
    }
    if (B->n == B->cap) {
      uint32_t c = B->cap ? B->cap * 2 : 32;
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
    return;
  }
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

/* with a table: the snapshot of a table name is the table's (compared at
   the end, in reconcile_snaps, when every name has its address; a
   String's (Array's, ...) contents digest is the state's own value, kept in
   its slot); one of a name of the state's own is made at the end too. */
static void
tsnap(pbuild *B, struct RBasic *o)
{
  const mrb_checkpoint_table *T = B->T;
  uint32_t idx = 0;
  pm_get(&B->obj2name, o, &idx);
  uint32_t k1 = (idx >= 1 && idx <= T->nnames) ? B->name2snap[idx - 1] : 0;
  if (k1 == 0) {
    if (o->tt == MRB_TT_STRING && o->frozen) return;   /* (as snap_object) */
    if (B->nxobj == B->capxobj) {
      B->capxobj = B->capxobj ? B->capxobj * 2 : 16;
      B->xobj = (struct RBasic**)mrb_realloc(B->mrb, B->xobj, sizeof(void*) * B->capxobj);
    }
    B->xobj[B->nxobj++] = o;
    return;
  }
  const mrb_checkpoint_table_snap *ts = &T->snaps[k1 - 1];
  if (ts->tt != (uint8_t)o->tt) return;   /* (reconcile_snaps keeps its own) */
  if ((checkpoint_snap_kind(o->tt) & SNAP_DIGEST) && ts->slot < T->nslots) {
    B->P->slotv[ts->slot] = checkpoint_content_digest(B->mrb, o);
    B->P->slott[ts->slot] = 0;
  }
}

static void snap_object(pbuild *B, struct RBasic *o);
static void visit_class(pbuild *B, struct RClass *c, const char *path);

/* a name made of two parts (mrb_malloc'ed; no length limit) */
static char*
cat_name(pbuild *B, const char *a, const char *b)
{
  size_t la = strlen(a), lb = strlen(b);
  char *s = (char*)mrb_malloc(B->mrb, la + lb + 1);
  memcpy(s, a, la);
  memcpy(s + la, b, lb + 1);
  return s;
}

/* a named object whose contents get names by a path (name_reached) */
static void
seed(pbuild *B, struct RBasic *o, const char *name)
{
  switch (o->tt) {
  case MRB_TT_CLASS: case MRB_TT_MODULE: case MRB_TT_SCLASS: case MRB_TT_ICLASS:
    return;   /* their contents are named by visit_class */
  default:
    break;
  }
  if ((const void*)o == B->noseed[0] || (const void*)o == B->noseed[1]) return;
  if (B->nq == B->capq) {
    B->capq = B->capq ? B->capq * 2 : 256;
    B->q = mrb_realloc(B->mrb, B->q, sizeof(B->q[0]) * B->capq);
  }
  B->q[B->nq].o = o;
  B->q[B->nq].name = cat_name(B, name, "");
  B->nq++;
}

static void
name_value(pbuild *B, mrb_value v, const char *name)
{
  if (mrb_immediate_p(v)) return;
  struct RBasic *o = mrb_basic_ptr(v);
  if (named_obj(B, o)) return;
  add_name(B, name, o, 0);
  snap_object(B, o);
  seed(B, o, name);
  /* an object's own singleton class (ENV's methods live there) */
  if (o->tt != MRB_TT_CLASS && o->tt != MRB_TT_MODULE && o->tt != MRB_TT_SCLASS &&
      o->c && o->c->tt == MRB_TT_SCLASS) {
    char *sname = cat_name(B, "S:", name);
    visit_class(B, o->c, sname);
    mrb_free(B->mrb, sname);
  }
}

/* what a permanent object (not a class) looks like at the baseline, to see
   later what the program changed in it (the dump's change records). A
   frozen String cannot change and has none. (A frozen Array or Hash has
   one: the Strings it holds can change, and they show in its digest.) */
static void
snap_object(pbuild *B, struct RBasic *o)
{
  int kind = checkpoint_snap_kind(o->tt);
  if (!kind) return;
  /* the hidden Array that keeps the baseline, the C side's GC roots: not
     the program's (the dump does not look at them) */
  if ((const void*)o == B->noseed[0] || (const void*)o == B->noseed[1]) return;
  if (B->T) {
    tsnap(B, o);
    return;
  }
  if (o->tt == MRB_TT_STRING && o->frozen) return;
  uint32_t s = new_snap(B, o);
  if (kind & SNAP_DIGEST) B->snaps[s].u.digest = checkpoint_content_digest(B->mrb, o);
  if (kind & SNAP_IVARS) snap_iv(B, s, o);
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

  uint32_t sidx = 0;
  giv_entry *civ = NULL; int nciv = 0;   /* (with a table) its ivars, for the walk below */
  if (B->T) {
    tsnap(B, (struct RBasic*)c);
    checkpoint_collect_iv(mrb, mrb_obj_value(c), &civ, &nciv);
  }
  else {
    sidx = new_snap(B, (struct RBasic*)c);
    B->snaps[sidx].u.super = c->super;
    B->snaps[sidx].cflags = (c->flags & MRB_FL_CLASS_IS_PREPENDED) ? 1 : 0;
  }
  gmt_entry *mt0; int nmt;
  checkpoint_collect_mt(mrb, c, &mt0, &nmt);
  snap_count(B, (uint32_t)nmt, "methods");
  if (!B->T) snap_iv(B, sidx, (struct RBasic*)c);

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
        snap_object(B, (struct RBasic*)p->e.env);
        seed(B, (struct RBasic*)p->e.env, buf);
      }
    }
    mrb_free(B->mrb, mt);
  }
  /* the snapshot of the methods, in the table's own order, each referring
     to its Proc or C function by name (every one has a name by now) */
  if (B->T) nmt = 0;   /* compared with the table's at the end */
  B->mt = (gsnap_mt*)grow(B, B->mt, &B->capmt, B->nmt + (uint32_t)nmt, sizeof(gsnap_mt), 1024);
  if (!B->T) {
    B->snaps[sidx].mt = B->nmt;
    B->snaps[sidx].nmt = (uint16_t)nmt;
  }
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
    if (B->T) {
      tsnap(B, (struct RBasic*)ic);
      continue;
    }
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
  int niv = B->T ? nciv : B->snaps[sidx].niv;
  if (niv > 0) {
    giv_entry *iv = (giv_entry*)mrb_malloc(B->mrb, sizeof(giv_entry) * niv);
    memcpy(iv, B->T ? civ : B->iv + B->snaps[sidx].iv, sizeof(giv_entry) * niv);
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
  if (civ) checkpoint_collect_free(mrb, civ);
}

/* ---- named by a path ----

   An object the walk above does not name (an element of a constant's
   Array, the value of a key in a Hash held by a constant, an ivar of such
   an object, ...) is permanent too, and the program can change it
   (FmrbI18n::STRINGS["en"][:word] = "..."). It is named by how it is
   reached from the first named object that holds it: the parent's name, a
   slash and the step. The named objects are taken in the order they were
   named, and what they hold is named breadth first, after every other
   name, so these names never take one of the names above.

     /<i>          element i of an Array, member i of a Struct, slot i of an Env
     /{"en"}       the value of the String key "en" of a Hash (bytes outside
                   printable ASCII, the space and the characters " \ / { }
                   are written \hh, two hex digits)
     /{:name}      ... of the Symbol key :name, /{12} of the Integer key 12
     /{#i}         ... of pair i, when the key is something else
     /k{"en"}, /k{#i}   the key itself
     /{"en"}, /{:name}, /{12}   an element of a Set (other elements are not
                   named: a Set's order depends on the elements' hash values,
                   which may be addresses)
     /@<ivar>      an ivar (/@@count for @count, /@ifnone for a Hash's default)
     /mesg         an Exception's message
     /begin, /end  a Range's ends
     /env          a Proc's env

   A class or module reached this way is visited like one reached by a
   constant, with the path as its name. Each named object has a snapshot,
   so the dump sees what the program changed in it.

   Except a String held only as an element of an Array, Struct or Set, or
   as a key or a value of a Hash (GM_NAMES = ["Piano", ...]): it gets no
   name and no snapshot, which would cost a pointer, an index and a slot in
   every state. Its holder's digest covers its bytes (val_bits), and the
   dump carries such a String as a new one and writes its holders again
   with it (checkpoint_dump.c, "Strings without a name"). A String reached
   a second time (held in two places) is named, so the two stay one. */

typedef struct { char *p; size_t len, cap; mrb_state *mrb; } nbuf;

static void
nb_add(nbuf *b, const char *s, size_t n)
{
  if (b->len + n + 1 > b->cap) {
    size_t c = b->cap ? b->cap : 64;
    while (c < b->len + n + 1) c *= 2;
    b->p = (char*)mrb_realloc(b->mrb, b->p, c);
    b->cap = c;
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = 0;
}

static void
nb_esc(nbuf *b, const char *s, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c > 0x20 && c < 0x7f && c != '"' && c != '\\' && c != '/' && c != '{' && c != '}') {
      nb_add(b, (const char*)&c, 1);
    }
    else {
      char hx[4];
      snprintf(hx, sizeof(hx), "\\%02x", c);
      nb_add(b, hx, 3);
    }
  }
}

/* the step of a key: 1, or 0 when the key is not a String, Symbol or Integer */
static int
nb_key(mrb_state *mrb, nbuf *b, mrb_value k)
{
  char tmp[32];
  switch (mrb_type(k)) {
  case MRB_TT_STRING:
    nb_add(b, "{\"", 2);
    nb_esc(b, RSTRING_PTR(k), (size_t)RSTRING_LEN(k));
    nb_add(b, "\"}", 2);
    return 1;
  case MRB_TT_SYMBOL: {
    mrb_int len;
    const char *n = mrb_sym_name_len(mrb, mrb_symbol(k), &len);
    nb_add(b, "{:", 2);
    nb_esc(b, n, (size_t)len);
    nb_add(b, "}", 1);
    return 1;
  }
  case MRB_TT_INTEGER:
    snprintf(tmp, sizeof(tmp), "{%lld}", (long long)mrb_integer(k));
    nb_add(b, tmp, strlen(tmp));
    return 1;
  default:
    return 0;
  }
}

/* a value held by the named object parent: its name is parent/step. A
   String held as an element or a value (leaf) and found only there is left
   without a name (see above); one found a second time is named then. */
static void
reach(pbuild *B, mrb_value v, const char *parent, const char *step, size_t slen, int leaf)
{
  if (mrb_immediate_p(v)) return;
  struct RBasic *o = mrb_basic_ptr(v);
  if (o->tt == MRB_TT_FREE || o->tt == MRB_TT_SCLASS || o->tt == MRB_TT_ICLASS) return;
  if (named_obj(B, o)) return;
  if (o->tt == MRB_TT_STRING && !(o->c && o->c->tt == MRB_TT_SCLASS)) {
    int seen = pm_get(&B->leaves, o, NULL);
    if (leaf && !seen) {
      pm_put(B->mrb, &B->leaves, o, 1);
      return;
    }
  }
  nbuf n = { NULL, 0, 0, B->mrb };
  nb_add(&n, parent, strlen(parent));
  nb_add(&n, "/", 1);
  nb_add(&n, step, slen);
  if (o->tt == MRB_TT_CLASS || o->tt == MRB_TT_MODULE) {
    /* (visit_class makes names of its methods in buffers of 512 bytes) */
    if (n.len < 400) visit_class(B, (struct RClass*)o, n.p);
  }
  else {
    name_value(B, v, n.p);
  }
  mrb_free(B->mrb, n.p);
}

static void
reach_i(pbuild *B, mrb_value v, const char *parent, mrb_int i, int leaf)
{
  char step[32];
  snprintf(step, sizeof(step), "%d", (int)i);
  reach(B, v, parent, step, strlen(step), leaf);
}

typedef struct { pbuild *B; const char *parent; uint32_t i; } reach_hash_ctx;

static int
reach_hash_cb(mrb_state *mrb, mrb_value k, mrb_value v, void *p)
{
  reach_hash_ctx *x = (reach_hash_ctx*)p;
  nbuf st = { NULL, 0, 0, mrb };
  if (!nb_key(mrb, &st, k)) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "{#%u}", (unsigned)x->i);
    nb_add(&st, tmp, strlen(tmp));
  }
  reach(x->B, v, x->parent, st.p, st.len, 1);
  {
    /* the key: k + the same step */
    nbuf ks = { NULL, 0, 0, mrb };
    nb_add(&ks, "k", 1);
    nb_add(&ks, st.p, st.len);
    reach(x->B, k, x->parent, ks.p, ks.len, 1);
    mrb_free(mrb, ks.p);
  }
  mrb_free(mrb, st.p);
  x->i++;
  return 0;
}

static void
reach_contents(pbuild *B, struct RBasic *o, const char *name)
{
  mrb_state *mrb = B->mrb;
  mrb_value v = mrb_obj_value(o);
  int ai = mrb_gc_arena_save(mrb);
  switch (o->tt) {
  case MRB_TT_ARRAY: case MRB_TT_STRUCT:
    for (mrb_int i = 0; i < RARRAY_LEN(v); i++) reach_i(B, RARRAY_PTR(v)[i], name, i, 1);
    break;
  case MRB_TT_HASH: {
    reach_hash_ctx x = { B, name, 0 };
    mrb_hash_foreach(mrb, mrb_hash_ptr(v), reach_hash_cb, &x);
    break;
  }
  case MRB_TT_SET: {
    mrb_value a = checkpoint_set_elements(mrb, o);
    for (mrb_int i = 0; mrb_array_p(a) && i < RARRAY_LEN(a); i++) {
      nbuf st = { NULL, 0, 0, mrb };
      if (nb_key(mrb, &st, RARRAY_PTR(a)[i])) reach(B, RARRAY_PTR(a)[i], name, st.p, st.len, 1);
      mrb_free(mrb, st.p);
    }
    break;
  }
  case MRB_TT_RANGE:
    if (o->flags & 1) {   /* RANGE_INITIALIZED_FLAG (range.c) */
      reach(B, RANGE_BEG((struct RRange*)o), name, "begin", 5, 0);
      reach(B, RANGE_END((struct RRange*)o), name, "end", 3, 0);
    }
    break;
  case MRB_TT_EXCEPTION: {
    struct RException *e = (struct RException*)o;
    if (e->mesg) reach(B, mrb_obj_value(e->mesg), name, "mesg", 4, 0);
    break;
  }
  case MRB_TT_PROC: {
    struct RProc *p = (struct RProc*)o;
    if (MRB_PROC_ENV_P(p) && p->e.env) reach(B, mrb_obj_value(p->e.env), name, "env", 3, 0);
    break;
  }
  case MRB_TT_ENV: {
    struct REnv *e = (struct REnv*)o;
    if (!MRB_ENV_ONSTACK_P(e)) {
      for (mrb_int i = 0; i < MRB_ENV_LEN(e); i++) reach_i(B, e->stack[i], name, i, 0);
    }
    break;
  }
  default:
    break;
  }
  if (o->tt == MRB_TT_OBJECT || (checkpoint_snap_kind(o->tt) & SNAP_IVARS)) {
    giv_entry *iv; int n;
    checkpoint_collect_iv(mrb, v, &iv, &n);
    if (n > 0) {
      giv_entry *s = (giv_entry*)mrb_malloc(mrb, sizeof(giv_entry) * n);
      memcpy(s, iv, sizeof(giv_entry) * n);
      sort_by_name(mrb, s, (size_t)n, sizeof(giv_entry), cmp_iv);
      for (int i = 0; i < n; i++) {
        mrb_int len;
        const char *ivn = mrb_sym_name_len(mrb, s[i].sym, &len);
        nbuf st = { NULL, 0, 0, mrb };
        nb_add(&st, "@", 1);
        nb_esc(&st, ivn, (size_t)len);
        reach(B, s[i].val, name, st.p, st.len, 0);
        mrb_free(mrb, st.p);
      }
      mrb_free(mrb, s);
    }
    checkpoint_collect_free(mrb, iv);
  }
  mrb_gc_arena_restore(mrb, ai);
}

/* names for what the named objects hold, breadth first (the queue grows
   while it is read: each object named here is taken in turn) */
static void
name_reached(pbuild *B)
{
  pm_init(&B->leaves);
  for (uint32_t i = 0; i < B->nq; i++) {
    struct RBasic *o = B->q[i].o;
    char *name = B->q[i].name;
    reach_contents(B, o, name);
  }
  for (uint32_t i = 0; i < B->nq; i++) mrb_free(B->mrb, B->q[i].name);
  mrb_free(B->mrb, B->q);
  B->q = NULL; B->nq = B->capq = 0;
  pm_free(B->mrb, &B->leaves);
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

/* ---- a value in a slot (with a table: what is the state's own) ---- */

static void
slot_put(checkpoint_perm *P, uint32_t slot, mrb_value v)
{
  uint64_t bits = 0;
  enum mrb_vtype t = mrb_type(v);
  switch (t) {
  case MRB_TT_FALSE: bits = mrb_nil_p(v) ? 0 : 1; break;
  case MRB_TT_TRUE: bits = 1; break;
  case MRB_TT_INTEGER: bits = (uint64_t)mrb_integer(v); break;
  case MRB_TT_FLOAT: { double d = (double)mrb_float(v); memcpy(&bits, &d, 8); break; }
  case MRB_TT_SYMBOL: bits = (uint64_t)mrb_symbol(v); break;
  case MRB_TT_UNDEF: break;
  case MRB_TT_CPTR: bits = (uint64_t)(uintptr_t)mrb_cptr(v); break;
  default: bits = (uint64_t)(uintptr_t)mrb_ptr(v); break;
  }
  P->slotv[slot] = bits;
  P->slott[slot] = (uint8_t)t;
}

static mrb_value
slot_get(mrb_state *mrb, checkpoint_perm *P, uint32_t slot)
{
  uint64_t bits = P->slotv[slot];
  switch ((enum mrb_vtype)P->slott[slot]) {
  case MRB_TT_FALSE: return bits ? mrb_false_value() : mrb_nil_value();
  case MRB_TT_TRUE: return mrb_true_value();
  case MRB_TT_INTEGER: return mrb_int_value(mrb, (mrb_int)bits);
  case MRB_TT_FLOAT: { double d; memcpy(&d, &bits, 8); return mrb_float_value(mrb, (mrb_float)d); }
  case MRB_TT_SYMBOL: return mrb_symbol_value((mrb_sym)bits);
  case MRB_TT_UNDEF: return mrb_undef_value();
  case MRB_TT_CPTR: return mrb_cptr_value(mrb, (void*)(uintptr_t)bits);
  default: return mrb_obj_value((void*)(uintptr_t)bits);
  }
}

/* (name, len) against the table's string s: <0, 0, >0 */
static int
cmp_name(const char *name, size_t len, const char *s)
{
  size_t sl = strlen(s);
  int r = memcmp(name, s, len < sl ? len : sl);
  if (r) return r;
  return len < sl ? -1 : len > sl ? 1 : 0;
}

static int
same_sym_name(mrb_state *mrb, mrb_sym sym, const char *s)
{
  mrb_int len;
  const char *n = mrb_sym_name_len(mrb, sym, &len);
  return n && cmp_name(n, (size_t)len, s) == 0;
}

/* with a table: whether this state sees table snapshot k (of o) as the
   table has it -- methods, ivars, superclass, marker. The ivars that are not
   names go to their slots on the way. */
static int
snap_matches(pbuild *B, uint32_t k, struct RBasic *o)
{
  mrb_state *mrb = B->mrb;
  const mrb_checkpoint_table *T = B->T;
  checkpoint_perm *P = B->P;
  const mrb_checkpoint_table_snap *ts = &T->snaps[k];
  const char *oname = tname(T, ts->name);
  int ok = 1;
  if (ts->tt != (uint8_t)o->tt) {
    mismatch(B, "%s is of another type than in the table", oname);
    return 0;
  }
  int cls = o->tt == MRB_TT_CLASS || o->tt == MRB_TT_MODULE || o->tt == MRB_TT_SCLASS;
  if (cls || o->tt == MRB_TT_ICLASS) {
    struct RClass *c = (struct RClass*)o;
    const void *want = (ts->super && ts->super <= T->nnames) ? P->ptrs[ts->super - 1] : NULL;
    if ((const void*)c->super != want) { mismatch(B, "%s has another superclass than in the table", oname); return 0; }
  }
  if (cls) {
    struct RClass *c = (struct RClass*)o;
    if (((c->flags & MRB_FL_CLASS_IS_PREPENDED) ? 1 : 0) != ts->cflags) {
      mismatch(B, "%s: prepended here, not in the table (or the other way)", oname);
      return 0;
    }
    gmt_entry *mt; int n;
    checkpoint_collect_mt(mrb, c, &mt, &n);
    sort_by_name(mrb, mt, (size_t)n, sizeof(gmt_entry), cmp_mt);
    if ((uint32_t)n != ts->nmt) {
      mismatch(B, "%s has %d methods here, %u in the table", oname, n, (unsigned)ts->nmt);
      ok = 0;
    }
    for (int i = 0; ok && i < n; i++) {
      const mrb_checkpoint_table_mt *e = &T->mt[ts->mt + i];
      const void *want = (e->ref == 0 || e->ref > T->nnames) ? NULL : P->ptrs[e->ref - 1];
      if (!same_sym_name(mrb, mt[i].sym, T->syms + e->sym) || mt[i].flags != e->flags || mt[i].ptr != want) {
        mismatch(B, "%s#%s is not the method of the table", oname, mrb_sym_name(mrb, mt[i].sym));
        ok = 0;
      }
    }
    checkpoint_collect_free(mrb, mt);
    if (!ok) return 0;
  }
  if (cls || (checkpoint_snap_kind(o->tt) & SNAP_IVARS)) {
    giv_entry *iv; int n;
    checkpoint_collect_iv(mrb, mrb_obj_value(o), &iv, &n);
    sort_by_name(mrb, iv, (size_t)n, sizeof(giv_entry), cmp_iv);
    if ((uint32_t)n != ts->niv) {
      mismatch(B, "%s has %d ivars here, %u in the table", oname, n, (unsigned)ts->niv);
      ok = 0;
    }
    for (int i = 0; ok && i < n; i++) {
      const mrb_checkpoint_table_iv *e = &T->iv[ts->iv + i];
      if (!same_sym_name(mrb, iv[i].sym, T->syms + e->sym)) {
        mismatch(B, "%s: ivar %s is not in the table", oname, mrb_sym_name(mrb, iv[i].sym));
        ok = 0;
      }
      else if (e->ref & MRB_CHECKPOINT_TABLE_SLOT) {
        uint32_t slot = e->ref & ~MRB_CHECKPOINT_TABLE_SLOT;
        if (slot < T->nslots) slot_put(P, slot, iv[i].val);
        else ok = 0;
      }
      else if (e->ref == 0 || e->ref > T->nnames || mrb_immediate_p(iv[i].val) ||
               mrb_ptr(iv[i].val) != P->ptrs[e->ref - 1]) {
        mismatch(B, "%s: %s holds another object than in the table", oname, mrb_sym_name(mrb, iv[i].sym));
        ok = 0;
      }
    }
    checkpoint_collect_free(mrb, iv);
    return ok;
  }
  return ts->nmt == 0 && ts->niv == 0;
}

/* a snapshot of o as without a table (in B->snaps, B->mt, B->iv): one the
   table does not have, or one this state sees otherwise */
static uint32_t
own_snap(pbuild *B, struct RBasic *o)
{
  mrb_state *mrb = B->mrb;
  uint32_t s = new_snap(B, o);
  switch (o->tt) {
  case MRB_TT_CLASS: case MRB_TT_MODULE: case MRB_TT_SCLASS: {
    struct RClass *c = (struct RClass*)o;
    B->snaps[s].u.super = c->super;
    B->snaps[s].cflags = (c->flags & MRB_FL_CLASS_IS_PREPENDED) ? 1 : 0;
    gmt_entry *mt; int n;
    checkpoint_collect_mt(mrb, c, &mt, &n);
    snap_count(B, (uint32_t)n, "methods");
    B->mt = (gsnap_mt*)grow(B, B->mt, &B->capmt, B->nmt + (uint32_t)n, sizeof(gsnap_mt), 64);
    B->snaps[s].mt = B->nmt;
    B->snaps[s].nmt = (uint16_t)n;
    for (int i = 0; i < n; i++) {
      gsnap_mt *e = &B->mt[B->nmt++];
      uint32_t ref = 0;
      if (mt[i].ptr) pm_get((mt[i].flags & MRB_METHOD_FUNC_FL) ? &B->func2name : &B->obj2name, mt[i].ptr, &ref);
      e->sym = mt[i].sym;
      e->flags = mt[i].flags;
      e->ref = ref;
    }
    checkpoint_collect_free(mrb, mt);
    snap_iv(B, s, o);
    break;
  }
  case MRB_TT_ICLASS:
    B->snaps[s].u.super = ((struct RClass*)o)->super;
    break;
  default: {
    int kind = checkpoint_snap_kind(o->tt);
    if (kind & SNAP_DIGEST) B->snaps[s].u.digest = checkpoint_content_digest(mrb, o);
    if (kind & SNAP_IVARS) snap_iv(B, s, o);
    break;
  }
  }
  return s;
}

/* with a table: the snapshots as the table has them, those this state sees
   otherwise and those of its own names */
static void
reconcile_snaps(pbuild *B)
{
  const mrb_checkpoint_table *T = B->T;
  checkpoint_perm *P = B->P;
  for (uint32_t k = 0; k < T->nsnaps; k++) {
    struct RBasic *o = (struct RBasic*)P->ptrs[T->snaps[k].name];
    if (!o || snap_matches(B, k, o)) continue;   /* (absent: the name is missing) */
    if (!P->snap_over) P->snap_over = (uint32_t*)mrb_calloc(B->mrb, T->nsnaps, sizeof(uint32_t));
    P->snap_over[k] = own_snap(B, o) + 1;
    B->ndiff++;
  }
  if (B->nxobj > 0) {
    P->xsnap = (uint32_t*)mrb_malloc(B->mrb, sizeof(uint32_t) * B->nxobj);
    for (uint32_t i = 0; i < B->nxobj; i++) P->xsnap[i] = own_snap(B, B->xobj[i]);
    P->nxsnap = B->nxobj;
  }
}

static const char *name_str(checkpoint_perm *P, uint32_t i);
static int name_is_func(checkpoint_perm *P, uint32_t i);

/* the names of objects / of C functions, sorted by address (with a table:
   the table's that this state has, then its own) */
static uint32_t*
table_sorted_index(mrb_state *mrb, checkpoint_perm *P, int funcs, uint32_t *count)
{
  uint32_t total = P->T->nnames + P->nx;
  uint32_t m = 0;
  for (uint32_t i = 0; i < total; i++) if (P->ptrs[i] && name_is_func(P, i) == funcs) m++;
  *count = m;
  if (m == 0) return NULL;
  keyed *k = (keyed*)mrb_malloc(mrb, sizeof(keyed) * m);
  m = 0;
  for (uint32_t i = 0; i < total; i++) {
    if (!P->ptrs[i] || name_is_func(P, i) != funcs) continue;
    k[m].key = (uintptr_t)P->ptrs[i];
    k[m].idx = i;
    m++;
  }
  qsort(k, m, sizeof(keyed), cmp_keyed);
  uint32_t *idx = (uint32_t*)mrb_malloc(mrb, sizeof(uint32_t) * m);
  for (uint32_t i = 0; i < m; i++) idx[i] = k[i].idx;
  mrb_free(mrb, k);
  return idx;
}

static void
finish_table(pbuild *B, checkpoint_perm *P)
{
  mrb_state *mrb = B->mrb;
  const mrb_checkpoint_table *T = B->T;
  P->T = T;
  /* the names of the state's own, after the table's */
  P->nx = B->n;
  P->n = T->nnames + B->n;
  P->ptrs = (const void**)mrb_realloc(mrb, (void*)P->ptrs, sizeof(void*) * (P->n ? P->n : 1));
  for (uint32_t i = 0; i < B->n; i++) P->ptrs[T->nnames + i] = B->ptrs[i];
  for (uint32_t i = 0; i < T->nnames; i++) if (!P->ptrs[i]) P->nmissing++;
  if (B->n > 0) {
    P->names = (char*)shrink(mrb, B->blob, B->blen);
    P->name_off = (uint32_t*)mrb_realloc(mrb, B->off, sizeof(uint32_t) * (B->n + 1));
    P->name_off[B->n] = B->blen;
    P->is_func = (uint8_t*)mrb_calloc(mrb, (B->n + 7) / 8 + 1, 1);
    for (uint32_t i = 0; i < B->n; i++) if (B->isf[i]) P->is_func[i / 8] |= (uint8_t)(1 << (i % 8));
  }
  else {
    mrb_free(mrb, B->blob); mrb_free(mrb, B->off);
  }
  mrb_free(mrb, (void*)B->ptrs); mrb_free(mrb, B->isf);
  B->blob = NULL; B->off = NULL; B->ptrs = NULL; B->isf = NULL;
  /* the snapshots of the state's own */
  P->nsnaps = T->nsnaps + P->nxsnap;
  P->snaps = (gsnap*)shrink(mrb, B->snaps, sizeof(gsnap) * B->nsnaps);
  P->mt = (gsnap_mt*)shrink(mrb, B->mt, sizeof(gsnap_mt) * B->nmt);
  P->nmt = B->nmt;
  P->iv = (giv_entry*)shrink(mrb, B->iv, sizeof(giv_entry) * B->niv);
  P->niv = B->niv;
  B->snaps = NULL; B->mt = NULL; B->iv = NULL;
  /* the mrblib ireps: the table's hashes when they are the same code in the
     same order, the state's own (as without a table) otherwise */
  int same = B->nireps == T->nireps;
  for (uint32_t i = 0; same && i < T->nireps; i++) same = B->ireps[i].hash == T->irep_hash[i];
  P->nireps = B->nireps;
  if (same) {
    P->irepp = (const mrb_irep**)mrb_malloc(mrb, sizeof(void*) * (B->nireps ? B->nireps : 1));
    for (uint32_t i = 0; i < B->nireps; i++) P->irepp[i] = B->ireps[i].irep;
    mrb_free(mrb, B->ireps);
  }
  else {
    mismatch(B, "the mrblib ireps differ from the table's (%u here, %u there)", (unsigned)B->nireps,
             (unsigned)T->nireps);
    P->ireps = (girep_perm*)shrink(mrb, B->ireps, sizeof(girep_perm) * B->nireps);
  }
  B->ireps = NULL;
  pm_free(mrb, &B->obj2name); pm_free(mrb, &B->func2name); pm_free(mrb, &B->irep2idx);
  P->obj_idx = table_sorted_index(mrb, P, 0, &P->nobj);
  P->func_idx = table_sorted_index(mrb, P, 1, &P->nfunc);
  if (P->nireps > 0) {
    keyed *k = (keyed*)mrb_malloc(mrb, sizeof(keyed) * P->nireps);
    for (uint32_t i = 0; i < P->nireps; i++) {
      k[i].key = (uintptr_t)(P->irepp ? P->irepp[i] : P->ireps[i].irep);
      k[i].idx = i;
    }
    qsort(k, P->nireps, sizeof(keyed), cmp_keyed);
    P->irep_idx = (uint32_t*)mrb_malloc(mrb, sizeof(uint32_t) * P->nireps);
    for (uint32_t i = 0; i < P->nireps; i++) P->irep_idx[i] = k[i].idx;
    mrb_free(mrb, k);
  }
  /* the stamp is the table's only when this state has the table's names
     and snapshots exactly */
  if (!B->failed) {
    P->stamp = T->stamp;
    P->stamp_set = 1;
  }
  mrb_free(mrb, B->name2snap);
  mrb_free(mrb, B->xobj);
}

static void perm_free_parts(mrb_state *mrb, checkpoint_perm *P);

static checkpoint_perm*
build_perm(mrb_state *mrb, checkpoint_state *g, pbuild *B)
{
  char buf[512];
  checkpoint_perm *P = (checkpoint_perm*)mrb_calloc(mrb, 1, sizeof(checkpoint_perm));
  const mrb_checkpoint_table *T = B->T;
  B->P = P;
  if (T) {
    P->ptrs = (const void**)mrb_calloc(mrb, T->nnames ? T->nnames : 1, sizeof(void*));
    B->name2snap = (uint32_t*)mrb_calloc(mrb, T->nnames ? T->nnames : 1, sizeof(uint32_t));
    for (uint32_t k = 0; k < T->nsnaps; k++) {
      if (T->snaps[k].name < T->nnames) B->name2snap[T->snaps[k].name] = k + 1;
    }
    P->slotv = (uint64_t*)mrb_calloc(mrb, T->nslots ? T->nslots : 1, sizeof(uint64_t));
    P->slott = (uint8_t*)mrb_calloc(mrb, T->nslots ? T->nslots : 1, 1);
  }

  /* every heap object alive now is permanent, named or not. None of them
     may be freed later (the program can drop the last reference, e.g.
     remove_const, and a new object would then take the slot and the name).
     Without a table, a hidden Array keeps them all alive, in an ivar of
     Checkpoint whose name Ruby code cannot spell; its elements are sorted
     by address, so the same Array is the set of these objects
     (checkpoint_perm_heap_p). With a table (and for writing one) the set is
     an array of addresses, 4 bytes each on a 32-bit machine instead of a
     value each, and the GC's root hook marks them (checkpoint_perm_mark). */
  {
    heap_acc h = { B, NULL, 0, 0 };
    mrb_objspace_each_objects(mrb, heap_cb, &h);
    if (h.n > 1) qsort(h.a, h.n, sizeof(uintptr_t), cmp_uintptr);
    if (T || g->for_table) {
      P->keepp = (uintptr_t*)shrink(mrb, h.a, sizeof(uintptr_t) * h.n);
      P->nkeep = h.n;
    }
    else {
      int ai = mrb_gc_arena_save(mrb);
      mrb_value keep = mrb_ary_new_capa(mrb, (mrb_int)h.n);
      for (uint32_t i = 0; i < h.n; i++) mrb_ary_push(mrb, keep, mrb_obj_value((void*)h.a[i]));
      mrb_free(mrb, h.a);
      mrb_obj_ptr(keep)->c = NULL;   /* hidden from ObjectSpace, like the GC roots */
      P->keep = mrb_ary_ptr(keep);
      B->noseed[0] = P->keep;
      struct RClass *ck = mrb_module_get(mrb, "Checkpoint");
      mrb_obj_iv_set(mrb, (struct RObject*)ck, mrb_intern_lit(mrb, "__checkpoint_keep__"), keep);
      mrb_gc_arena_restore(mrb, ai);
    }
  }

  {
    mrb_value roots = mrb_gv_get(mrb, mrb_intern_lit(mrb, "_gc_root_"));
    if (!mrb_immediate_p(roots)) B->noseed[1] = mrb_ptr(roots);
  }

  /* C functions the core puts in closures */
  add_name(B, "F:attr_reader", (const void*)mrb_attr_reader, 1);
  add_name(B, "F:attr_writer", (const void*)mrb_attr_writer, 1);
  /* ... and the bundled gems: the accessors Struct.new and Data.define make
     (static functions there; found through a class made for the purpose) */
  /* (the functions of the classes gems make at run time come last) */

  add_name(B, "T", mrb->top_self, 0);
  snap_object(B, (struct RBasic*)mrb->top_self);
  seed(B, (struct RBasic*)mrb->top_self, "T");
  visit_class(B, mrb->object_class, "Object");
  if (mrb->top_self->c && mrb->top_self->c->tt == MRB_TT_SCLASS) {
    visit_class(B, mrb->top_self->c, "S:T");
  }
  if (mrb->nomem_err) {
    add_name(B, "X:nomem_err", mrb->nomem_err, 0);
    snap_object(B, (struct RBasic*)mrb->nomem_err);
    seed(B, (struct RBasic*)mrb->nomem_err, "X:nomem_err");
  }
  if (mrb->stack_err) {
    add_name(B, "X:stack_err", mrb->stack_err, 0);
    snap_object(B, (struct RBasic*)mrb->stack_err);
    seed(B, (struct RBasic*)mrb->stack_err, "X:stack_err");
  }

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

  /* last, what the named objects hold */
  name_reached(B);

  checkpoint_gc_settings(mrb, &P->gc);
  if (T) {
    reconcile_snaps(B);
    finish_table(B, P);
    for (uint32_t i = 0; i < T->nnames && !B->failed; i++) {
      if (!P->ptrs[i]) mismatch(B, "%s of the table is not in this build", tname(T, i));
    }
    if (B->failed) {
      snprintf(g->table_why, sizeof(g->table_why),
               "used with differences (%u names of its own, %u missing, %u snapshots seen otherwise); first: %s",
               (unsigned)P->nx, (unsigned)P->nmissing, (unsigned)B->ndiff, B->why);
      uint64_t h = fnv_u64(T->stamp, P->nx);
      h = fnv_u64(h, P->nmissing);
      P->stamp = fnv_u64(h, B->ndiff);
      P->stamp_set = 1;
    }
    else {
      g->table_why[0] = 0;
    }
    return P;
  }
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
  if (g->table) {
    if (g->table->version != MRB_CHECKPOINT_TABLE_VERSION) {
      snprintf(g->table_why, sizeof(g->table_why), "not used: the table is of version %u, this gem reads %u",
               (unsigned)g->table->version, (unsigned)MRB_CHECKPOINT_TABLE_VERSION);
    }
    else {
      B.T = g->table;
    }
  }
  else {
    snprintf(g->table_why, sizeof(g->table_why), "not used: no table");
  }
  B.mrb = mrb;
  pm_init(&B.obj2name); pm_init(&B.func2name); pm_init(&B.irep2idx);
  return build_perm(mrb, g, &B);
}

static void
perm_free_parts(mrb_state *mrb, checkpoint_perm *P)
{
  mrb_free(mrb, P->names); mrb_free(mrb, P->name_off);
  mrb_free(mrb, (void*)P->ptrs); mrb_free(mrb, P->is_func);
  mrb_free(mrb, P->obj_idx); mrb_free(mrb, P->func_idx);
  mrb_free(mrb, P->ireps); mrb_free(mrb, P->irep_idx);
  mrb_free(mrb, P->snaps); mrb_free(mrb, P->mt); mrb_free(mrb, P->iv);
  mrb_free(mrb, P->gv);
  mrb_free(mrb, P->keepp);
  mrb_free(mrb, (void*)P->irepp); mrb_free(mrb, P->slotv); mrb_free(mrb, P->slott);
  mrb_free(mrb, P->snap_over); mrb_free(mrb, P->xsnap);
}

void
checkpoint_perm_free(mrb_state *mrb, checkpoint_perm *P)
{
  if (!P) return;
  perm_free_parts(mrb, P);
  mrb_free(mrb, P);
}

void
checkpoint_perm_mark(mrb_state *mrb, checkpoint_perm *P)
{
  if (!P || !P->keepp) return;
  for (uint32_t i = 0; i < P->nkeep; i++) mrb_gc_mark(mrb, (struct RBasic*)P->keepp[i]);
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

static const char*
name_str(checkpoint_perm *P, uint32_t i)
{
  if (P->T && i < P->T->nnames) return tname(P->T, i);
  if (P->T) i -= P->T->nnames;
  return P->names + P->name_off[i];
}

static int
name_is_func(checkpoint_perm *P, uint32_t i)
{
  if (P->T && i < P->T->nnames) return tbit(P->T->is_func, i);
  if (P->T) i -= P->T->nnames;
  return tbit(P->is_func, i);
}

const char*
checkpoint_perm_name_of(checkpoint_perm *P, const void *obj)
{
  if (!obj) return NULL;
  int64_t i = idx_find(P->obj_idx, P->nobj, P->ptrs, (uintptr_t)obj);
  return i < 0 ? NULL : name_str(P, (uint32_t)i);
}

const char*
checkpoint_perm_func_name(checkpoint_perm *P, mrb_func_t f)
{
  if (!f) return NULL;
  int64_t i = idx_find(P->func_idx, P->nfunc, P->ptrs, (uintptr_t)f);
  return i < 0 ? NULL : name_str(P, (uint32_t)i);
}

int
checkpoint_perm_heap_p(checkpoint_perm *P, const void *obj)
{
  if (!obj) return 0;
  uintptr_t k = (uintptr_t)obj;
  if (P->keepp) {
    uint32_t lo = 0, hi = P->nkeep;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      if (P->keepp[mid] == k) return 1;
      if (P->keepp[mid] < k) lo = mid + 1;
      else hi = mid;
    }
    return 0;
  }
  if (!P->keep) return 0;
  if (obj == (const void*)P->keep) return 1;
  const mrb_value *a = ARY_PTR(P->keep);
  mrb_int lo = 0, hi = ARY_LEN(P->keep);
  while (lo < hi) {
    mrb_int mid = lo + (hi - lo) / 2;
    uintptr_t m = (uintptr_t)mrb_ptr(a[mid]);
    if (m == k) return 1;
    if (m < k) lo = mid + 1;
    else hi = mid;
  }
  return 0;
}

int
checkpoint_perm_internal_p(checkpoint_perm *P, const void *obj)
{
  return obj && obj == (const void*)P->keep;
}

const void*
checkpoint_perm_lookup(checkpoint_perm *P, const char *name, size_t len, int *is_func)
{
  if (P->T) {
    /* the table's names in name order: a binary search */
    const mrb_checkpoint_table *T = P->T;
    uint32_t lo = 0, hi = T->nnames;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      uint32_t i = T->by_name[mid];
      int r = cmp_name(name, len, tname(T, i));
      if (r == 0) {
        if (!P->ptrs[i]) break;   /* the table's, not in this state */
        if (is_func) *is_func = tbit(T->is_func, i);
        return P->ptrs[i];
      }
      if (r > 0) lo = mid + 1;
      else hi = mid;
    }
    /* the names of the state's own */
    for (uint32_t j = 0; j < P->nx; j++) {
      if (P->name_off[j + 1] - P->name_off[j] - 1 == len && memcmp(P->names + P->name_off[j], name, len) == 0) {
        if (is_func) *is_func = tbit(P->is_func, j);
        return P->ptrs[T->nnames + j];
      }
    }
    return NULL;
  }
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
  if (is_func) *is_func = name_is_func(P, i);
  return name_str(P, i);
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
    uint32_t i = P->irep_idx[mid];
    uintptr_t m = (uintptr_t)(P->irepp ? P->irepp[i] : P->ireps[i].irep);
    if (m == k) {
      if (hash) *hash = P->irepp ? P->T->irep_hash[i] : P->ireps[i].hash;
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
    if (P->irepp) {
      if (P->T->irep_hash[i] == hash) return P->irepp[i];
    }
    else if (P->ireps[i].hash == hash) return P->ireps[i].irep;
  }
  return NULL;
}

/* ---- the snapshots ---- */

uint32_t
checkpoint_perm_nsnaps(checkpoint_perm *P)
{
  return P->nsnaps;
}

/* snapshot k as kept without a table (NULL: the table's) */
static gsnap*
own_of(checkpoint_perm *P, uint32_t k)
{
  if (!P->T) return &P->snaps[k];
  if (k >= P->T->nsnaps) return &P->snaps[P->xsnap[k - P->T->nsnaps]];
  if (P->snap_over && P->snap_over[k]) return &P->snaps[P->snap_over[k] - 1];
  return NULL;
}

/* NULL for a table snapshot whose object this state lacks */
struct RBasic*
checkpoint_perm_snap_obj(checkpoint_perm *P, uint32_t k)
{
  gsnap *o = own_of(P, k);
  return o ? o->obj : (struct RBasic*)P->ptrs[P->T->snaps[k].name];
}

struct RClass*
checkpoint_perm_snap_super(checkpoint_perm *P, uint32_t k)
{
  gsnap *o = own_of(P, k);
  if (o) return o->u.super;
  uint32_t s = P->T->snaps[k].super;
  return s ? (struct RClass*)P->ptrs[s - 1] : NULL;
}

int
checkpoint_perm_snap_cflags(checkpoint_perm *P, uint32_t k)
{
  gsnap *o = own_of(P, k);
  return o ? o->cflags : P->T->snaps[k].cflags;
}

uint64_t
checkpoint_perm_snap_digest(checkpoint_perm *P, uint32_t k)
{
  gsnap *o = own_of(P, k);
  return o ? o->u.digest : P->slotv[P->T->snaps[k].slot];
}

int
checkpoint_perm_snap_mt(mrb_state *mrb, checkpoint_perm *P, uint32_t k, gsnap_mt **out)
{
  gsnap *o = own_of(P, k);
  int n = o ? o->nmt : P->T->snaps[k].nmt;
  gsnap_mt *a = (gsnap_mt*)coll_realloc(mrb, NULL, sizeof(gsnap_mt) * (n ? n : 1));
  if (o) {
    if (n > 0) memcpy(a, P->mt + o->mt, sizeof(gsnap_mt) * n);
  }
  else {
    const mrb_checkpoint_table_mt *e = P->T->mt + P->T->snaps[k].mt;
    for (int i = 0; i < n; i++) {
      a[i].sym = mrb_intern_check_cstr(mrb, P->T->syms + e[i].sym);
      a[i].flags = e[i].flags;
      a[i].ref = e[i].ref;
    }
  }
  *out = a;
  return n;
}

int
checkpoint_perm_snap_iv(mrb_state *mrb, checkpoint_perm *P, uint32_t k, giv_entry **out)
{
  gsnap *o = own_of(P, k);
  int n = o ? o->niv : P->T->snaps[k].niv;
  giv_entry *a = (giv_entry*)coll_realloc(mrb, NULL, sizeof(giv_entry) * (n ? n : 1));
  if (o) {
    if (n > 0) memcpy(a, P->iv + o->iv, sizeof(giv_entry) * n);
  }
  else {
    const mrb_checkpoint_table_iv *e = P->T->iv + P->T->snaps[k].iv;
    for (int i = 0; i < n; i++) {
      a[i].sym = mrb_intern_check_cstr(mrb, P->T->syms + e[i].sym);
      if (e[i].ref & MRB_CHECKPOINT_TABLE_SLOT) a[i].val = slot_get(mrb, P, e[i].ref & ~MRB_CHECKPOINT_TABLE_SLOT);
      else a[i].val = mrb_obj_value((void*)P->ptrs[e[i].ref - 1]);
    }
  }
  *out = a;
  return n;
}

int
checkpoint_perm_has_method(mrb_state *mrb, checkpoint_perm *P, const char *name, size_t len)
{
  if (P->T) {
    const mrb_checkpoint_table *T = P->T;
    uint32_t lo = 0, hi = T->nmethods;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2;
      int r = cmp_name(name, len, T->syms + T->methods[mid]);
      if (r == 0) return 1;
      if (r > 0) lo = mid + 1;
      else hi = mid;
    }
    /* (the snapshots of the state's own, below) */
  }
  mrb_sym sym = mrb_intern_check(mrb, name, len);
  if (sym == 0) return 0;
  for (uint32_t i = 0; i < P->nmt; i++) if (P->mt[i].ref != 0 && P->mt[i].sym == sym) return 1;
  return 0;
}

/* ---- a table from the state's own tables (writing one; the stamp) ---- */

typedef struct {
  mrb_checkpoint_table t;
  uint32_t *by_name;
  mrb_checkpoint_table_snap *snaps;
  mrb_checkpoint_table_mt *mt;
  mrb_checkpoint_table_iv *iv;
  char *syms; uint32_t slen, scap;
  pmap sym2off;              /* mrb_sym -> offset + 1 */
  uint64_t *irep_hash;
  uint32_t *methods;
} tbuild;

static uint32_t
tb_sym(mrb_state *mrb, checkpoint_state *G, tbuild *tb, mrb_sym sym)
{
  uint32_t off;
  if (pm_get(&tb->sym2off, (const void*)(uintptr_t)sym, &off)) return off - 1;
  mrb_int len;
  const char *s = mrb_sym_name_len(mrb, sym, &len);
  if (tb->slen + (uint32_t)len + 1 > tb->scap) {
    uint32_t c = tb->scap ? tb->scap : 4096;
    while (c < tb->slen + (uint32_t)len + 1) c *= 2;
    tb->syms = (char*)ck_realloc(G, tb->syms, c);
    tb->scap = c;
  }
  off = tb->slen;
  memcpy(tb->syms + off, s, (size_t)len);
  tb->syms[off + len] = 0;
  tb->slen += (uint32_t)len + 1;
  pm_put(mrb, &tb->sym2off, (const void*)(uintptr_t)sym, off + 1);
  return off;
}

/* index sort by the strings base + off[idx] (merge sort: no static context) */
static void
sort_by_str(checkpoint_state *G, uint32_t *idx, uint32_t n, const char *base, const uint32_t *off)
{
  if (n < 2) return;
  uint32_t *tmp = (uint32_t*)ck_malloc(G, sizeof(uint32_t) * n);
  for (uint32_t w = 1; w < n; w *= 2) {
    for (uint32_t lo = 0; lo < n - w; lo += 2 * w) {
      uint32_t mid = lo + w, hi = lo + 2 * w < n ? lo + 2 * w : n;
      uint32_t i = lo, j = mid, k = 0;
      while (i < mid && j < hi) {
        const char *a = base + (off ? off[idx[i]] : idx[i]);
        const char *b = base + (off ? off[idx[j]] : idx[j]);
        if (strcmp(b, a) < 0) tmp[k++] = idx[j++];
        else tmp[k++] = idx[i++];
      }
      while (i < mid) tmp[k++] = idx[i++];
      while (j < hi) tmp[k++] = idx[j++];
      memcpy(idx + lo, tmp, sizeof(uint32_t) * k);
    }
  }
  ck_free(G, tmp);
}

static int64_t
obj_index(checkpoint_perm *P, const void *p)
{
  return p ? idx_find(P->obj_idx, P->nobj, P->ptrs, (uintptr_t)p) : -1;
}

static uint64_t
table_digest(const mrb_checkpoint_table *t)
{
  uint64_t h = FNV_INIT;
  h = fnv_u64(h, t->version);
  h = fnv_u64(h, t->nnames);
  h = fnv_bytes(h, t->names, t->name_off[t->nnames]);
  h = fnv_bytes(h, t->is_func, (t->nnames + 7) / 8);
  h = fnv_u64(h, t->nsnaps);
  for (uint32_t i = 0; i < t->nsnaps; i++) {
    const mrb_checkpoint_table_snap *s = &t->snaps[i];
    h = fnv_u64(h, s->name); h = fnv_u64(h, s->super); h = fnv_u64(h, s->mt); h = fnv_u64(h, s->iv);
    h = fnv_u64(h, s->slot); h = fnv_u64(h, s->nmt); h = fnv_u64(h, s->niv);
    h = fnv_u64(h, s->tt); h = fnv_u64(h, s->cflags);
  }
  h = fnv_u64(h, t->nmt);
  for (uint32_t i = 0; i < t->nmt; i++) {
    h = fnv_u64(h, t->mt[i].sym); h = fnv_u64(h, t->mt[i].flags); h = fnv_u64(h, t->mt[i].ref);
  }
  h = fnv_u64(h, t->niv);
  for (uint32_t i = 0; i < t->niv; i++) { h = fnv_u64(h, t->iv[i].sym); h = fnv_u64(h, t->iv[i].ref); }
  h = fnv_u64(h, t->nslots);
  uint32_t slen = 0;
  /* the strings up to the end of the last one any entry names */
  for (uint32_t i = 0; i < t->nmt; i++) { uint32_t e = t->mt[i].sym + (uint32_t)strlen(t->syms + t->mt[i].sym) + 1; if (e > slen) slen = e; }
  for (uint32_t i = 0; i < t->niv; i++) { uint32_t e = t->iv[i].sym + (uint32_t)strlen(t->syms + t->iv[i].sym) + 1; if (e > slen) slen = e; }
  h = fnv_bytes(h, t->syms, slen);
  h = fnv_u64(h, t->nireps);
  for (uint32_t i = 0; i < t->nireps; i++) h = fnv_u64(h, t->irep_hash[i]);
  h = fnv_u64(h, t->nmethods);
  for (uint32_t i = 0; i < t->nmethods; i++) h = fnv_u64(h, t->methods[i]);
  return h;
}

static void
tb_free(checkpoint_state *G, tbuild *tb)
{
  ck_free(G, tb->by_name); ck_free(G, tb->snaps); ck_free(G, tb->mt); ck_free(G, tb->iv);
  ck_free(G, tb->syms); ck_free(G, tb->irep_hash); ck_free(G, tb->methods);
  pm_free(G->mrb, &tb->sym2off);
}

static int
cmp_mt_name(mrb_state *mrb, const void *a, const void *b)
{
  return sym_cmp(mrb, ((const gsnap_mt*)a)->sym, ((const gsnap_mt*)b)->sym);
}

/* the table of this state's own (not a table's) baseline; 0, or -1 with why */
static int
table_from_perm(mrb_state *mrb, checkpoint_state *G, checkpoint_perm *P, tbuild *tb, char *why, size_t whylen)
{
  memset(tb, 0, sizeof(*tb));
  pm_init(&tb->sym2off);
  mrb_checkpoint_table *t = &tb->t;
  t->version = MRB_CHECKPOINT_TABLE_VERSION;
  t->nnames = P->n;
  t->names = P->names;
  t->name_off = P->name_off;
  t->is_func = P->is_func;
  tb->by_name = (uint32_t*)ck_malloc(G, sizeof(uint32_t) * (P->n ? P->n : 1));
  for (uint32_t i = 0; i < P->n; i++) tb->by_name[i] = i;
  sort_by_str(G, tb->by_name, P->n, P->names, P->name_off);
  t->by_name = tb->by_name;

  tb->snaps = (mrb_checkpoint_table_snap*)ck_calloc(G, P->nsnaps ? P->nsnaps : 1, sizeof(mrb_checkpoint_table_snap));
  tb->mt = (mrb_checkpoint_table_mt*)ck_malloc(G, sizeof(mrb_checkpoint_table_mt) * (P->nmt ? P->nmt : 1));
  tb->iv = (mrb_checkpoint_table_iv*)ck_malloc(G, sizeof(mrb_checkpoint_table_iv) * (P->niv ? P->niv : 1));
  uint32_t nmt = 0, niv = 0, nslots = 0;
  for (uint32_t k = 0; k < P->nsnaps; k++) {
    gsnap *s = &P->snaps[k];
    mrb_checkpoint_table_snap *ts = &tb->snaps[k];
    int64_t ni = obj_index(P, s->obj);
    if (ni < 0) { snprintf(why, whylen, "snapshot %u has no name", (unsigned)k); return -1; }
    ts->name = (uint32_t)ni;
    ts->tt = (uint8_t)s->obj->tt;
    switch (s->obj->tt) {
    case MRB_TT_CLASS: case MRB_TT_MODULE: case MRB_TT_SCLASS: case MRB_TT_ICLASS: {
      int64_t si = obj_index(P, s->u.super);
      if (s->u.super && si < 0) {
        snprintf(why, whylen, "the superclass of %s has no name", P->names + P->name_off[ni]);
        return -1;
      }
      ts->super = s->u.super ? (uint32_t)si + 1 : 0;
      ts->cflags = s->obj->tt == MRB_TT_ICLASS ? 0 : s->cflags;
      break;
    }
    default:
      if (checkpoint_snap_kind(s->obj->tt) & SNAP_DIGEST) ts->slot = nslots++;
      break;
    }
    /* methods, in name order */
    ts->mt = nmt;
    ts->nmt = s->nmt;
    if (s->nmt > 0) {
      gsnap_mt *m = (gsnap_mt*)ck_malloc(G, sizeof(gsnap_mt) * s->nmt);
      memcpy(m, P->mt + s->mt, sizeof(gsnap_mt) * s->nmt);
      sort_by_name(mrb, m, s->nmt, sizeof(gsnap_mt), cmp_mt_name);
      for (uint32_t i = 0; i < s->nmt; i++) {
        tb->mt[nmt].sym = tb_sym(mrb, G, tb, m[i].sym);
        tb->mt[nmt].flags = m[i].flags;
        tb->mt[nmt].ref = m[i].ref;
        nmt++;
      }
      ck_free(G, m);
    }
    /* ivars, in name order: a named object by its name, anything else in a
       slot of the state's own */
    ts->iv = niv;
    ts->niv = s->niv;
    if (s->niv > 0) {
      giv_entry *v = (giv_entry*)ck_malloc(G, sizeof(giv_entry) * s->niv);
      memcpy(v, P->iv + s->iv, sizeof(giv_entry) * s->niv);
      sort_by_name(mrb, v, s->niv, sizeof(giv_entry), cmp_iv);
      for (uint32_t i = 0; i < s->niv; i++) {
        tb->iv[niv].sym = tb_sym(mrb, G, tb, v[i].sym);
        int64_t oi = mrb_immediate_p(v[i].val) ? -1 : obj_index(P, mrb_ptr(v[i].val));
        tb->iv[niv].ref = oi >= 0 ? (uint32_t)oi + 1 : (MRB_CHECKPOINT_TABLE_SLOT | nslots++);
        niv++;
      }
      ck_free(G, v);
    }
  }
  t->nsnaps = P->nsnaps; t->snaps = tb->snaps;
  t->nmt = nmt; t->mt = tb->mt;
  t->niv = niv; t->iv = tb->iv;
  t->nslots = nslots;

  /* every method name some class defines (an undefined method is no
     definition) */
  uint32_t *m = (uint32_t*)ck_malloc(G, sizeof(uint32_t) * (nmt ? nmt : 1));
  uint32_t nm = 0;
  for (uint32_t i = 0; i < nmt; i++) if (tb->mt[i].ref != 0) m[nm++] = tb->mt[i].sym;
  sort_by_str(G, m, nm, tb->syms ? tb->syms : "", NULL);
  uint32_t u = 0;
  for (uint32_t i = 0; i < nm; i++) if (u == 0 || strcmp(tb->syms + m[i], tb->syms + m[u - 1]) != 0) m[u++] = m[i];
  tb->methods = m;
  t->nmethods = u; t->methods = m;
  t->syms = tb->syms ? tb->syms : "";

  tb->irep_hash = (uint64_t*)ck_malloc(G, sizeof(uint64_t) * (P->nireps ? P->nireps : 1));
  for (uint32_t i = 0; i < P->nireps; i++) tb->irep_hash[i] = P->ireps[i].hash;
  t->nireps = P->nireps; t->irep_hash = tb->irep_hash;
  t->stamp = table_digest(t);
  return 0;
}

uint64_t
checkpoint_perm_stamp(mrb_state *mrb, checkpoint_perm *P)
{
  if (P->stamp_set) return P->stamp;
  checkpoint_state *G = checkpoint_get(mrb);
  tbuild tb;
  char why[160];
  P->stamp = (G && table_from_perm(mrb, G, P, &tb, why, sizeof(why)) == 0) ? tb.t.stamp : 0;
  if (G) tb_free(G, &tb);
  P->stamp_set = 1;
  return P->stamp;
}

/* ---- writing a table as C source ---- */

typedef struct {
  mrb_checkpoint_text_fn fn;
  void *ud;
  char buf[1024];
  size_t len;
} emitter;

static void
em_flush(emitter *e)
{
  if (e->len) e->fn(e->buf, e->len, e->ud);
  e->len = 0;
}

static void
em(emitter *e, const char *fmt, ...)
{
  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n >= sizeof(tmp)) n = sizeof(tmp) - 1;
  if (e->len + (size_t)n > sizeof(e->buf)) em_flush(e);
  memcpy(e->buf + e->len, tmp, (size_t)n);
  e->len += (size_t)n;
}

/* a C string literal of s (one name), with its NUL; a long name in pieces
   (names made of paths have no length limit) */
static void
em_cstr(emitter *e, const char *s)
{
  char out[300];
  size_t o = 0;
  out[o++] = '"';
  for (const unsigned char *p = (const unsigned char*)s; *p; p++) {
    if (o > sizeof(out) - 16) {
      out[o] = 0;
      em(e, "%s", out);
      o = 0;
    }
    if (*p == '"' || *p == '\\' || *p == '?') { out[o++] = '\\'; out[o++] = (char)*p; }
    else if (*p < 0x20 || *p >= 0x7f) o += (size_t)snprintf(out + o, sizeof(out) - o, "\\%03o", *p);
    else out[o++] = (char)*p;
  }
  memcpy(out + o, "\\0\"\n", 4);
  o += 4;
  out[o] = 0;
  em(e, "%s", out);
}

static void
em_u32s(emitter *e, const char *name, const uint32_t *a, uint32_t n)
{
  em(e, "static const uint32_t %s[] = {", name);
  for (uint32_t i = 0; i < n; i++) em(e, "%s%u,", i % 12 ? "" : "\n  ", (unsigned)a[i]);
  if (n == 0) em(e, "0");
  em(e, "\n};\n");
}

MRB_API int
mrb_checkpoint_write_table(mrb_state *mrb, const char *symbol, mrb_checkpoint_text_fn fn, void *ud,
                           char *why, size_t whylen)
{
  checkpoint_state *G = checkpoint_get(mrb);
  if (!G || !G->perm) { snprintf(why, whylen, "no baseline"); return -1; }
  checkpoint_perm *P = G->perm;
  if (P->T || !P->keepp) {
    snprintf(why, whylen, "the baseline was not taken for a table (mrb_checkpoint_prepare_table)");
    return -1;
  }
  tbuild tb;
  if (table_from_perm(mrb, G, P, &tb, why, whylen) != 0) { tb_free(G, &tb); return -1; }
  const mrb_checkpoint_table *t = &tb.t;
  emitter e = { fn, ud, {0}, 0 };

  em(&e, "/* The checkpoint table of this build (mruby-checkpoint, mrb_checkpoint_write_table).\n"
         "   Made when the build is built; do not edit. %u names, %u snapshots, %u mrblib ireps,\n"
         "   %u values of the state's own. */\n\n", (unsigned)t->nnames, (unsigned)t->nsnaps,
     (unsigned)t->nireps, (unsigned)t->nslots);
  em(&e, "#include <mruby.h>\n#include <mruby/checkpoint.h>\n\n");
  em(&e, "static const char names[] =\n");
  for (uint32_t i = 0; i < t->nnames; i++) em_cstr(&e, t->names + t->name_off[i]);
  if (t->nnames == 0) em(&e, "\"\"\n");
  em(&e, ";\n");
  {
    uint32_t *off = (uint32_t*)ck_malloc(G, sizeof(uint32_t) * (t->nnames + 1));
    memcpy(off, t->name_off, sizeof(uint32_t) * (t->nnames + 1));
    em_u32s(&e, "name_off", off, t->nnames + 1);
    ck_free(G, off);
  }
  em(&e, "static const uint8_t is_func[] = {");
  for (uint32_t i = 0; i < (t->nnames + 7) / 8; i++) em(&e, "%s%u,", i % 16 ? "" : "\n  ", (unsigned)t->is_func[i]);
  em(&e, "0\n};\n");
  em_u32s(&e, "by_name", t->by_name, t->nnames);
  em(&e, "static const mrb_checkpoint_table_snap snaps[] = {\n");
  for (uint32_t i = 0; i < t->nsnaps; i++) {
    const mrb_checkpoint_table_snap *s = &t->snaps[i];
    em(&e, "  {%u,%u,%u,%u,%u,%u,%u,%u,%u},\n", (unsigned)s->name, (unsigned)s->super, (unsigned)s->mt,
       (unsigned)s->iv, (unsigned)s->slot, (unsigned)s->nmt, (unsigned)s->niv, (unsigned)s->tt, (unsigned)s->cflags);
  }
  if (t->nsnaps == 0) em(&e, "  {0,0,0,0,0,0,0,0,0}\n");
  em(&e, "};\nstatic const mrb_checkpoint_table_mt mt[] = {\n");
  for (uint32_t i = 0; i < t->nmt; i++) {
    em(&e, "  {%u,0x%x,%u},\n", (unsigned)t->mt[i].sym, (unsigned)t->mt[i].flags, (unsigned)t->mt[i].ref);
  }
  if (t->nmt == 0) em(&e, "  {0,0,0}\n");
  em(&e, "};\nstatic const mrb_checkpoint_table_iv iv[] = {\n");
  for (uint32_t i = 0; i < t->niv; i++) em(&e, "  {%u,0x%x},\n", (unsigned)t->iv[i].sym, (unsigned)t->iv[i].ref);
  if (t->niv == 0) em(&e, "  {0,0}\n");
  em(&e, "};\nstatic const char syms[] =\n");
  for (uint32_t o = 0; o < tb.slen; o += (uint32_t)strlen(t->syms + o) + 1) em_cstr(&e, t->syms + o);
  if (tb.slen == 0) em(&e, "\"\"\n");
  em(&e, ";\nstatic const uint64_t irep_hash[] = {");
  for (uint32_t i = 0; i < t->nireps; i++) {
    em(&e, "%s0x%08x%08xULL,", i % 4 ? "" : "\n  ", (unsigned)(t->irep_hash[i] >> 32), (unsigned)(t->irep_hash[i] & 0xffffffffu));
  }
  if (t->nireps == 0) em(&e, "0");
  em(&e, "\n};\n");
  em_u32s(&e, "methods", t->methods, t->nmethods);
  em(&e, "\nconst mrb_checkpoint_table %s = {\n", symbol);
  em(&e, "  %u, 0x%08x%08xULL,\n", (unsigned)t->version, (unsigned)(t->stamp >> 32), (unsigned)(t->stamp & 0xffffffffu));
  em(&e, "  %u, names, name_off, is_func, by_name,\n", (unsigned)t->nnames);
  em(&e, "  %u, snaps, %u, mt, %u, iv,\n", (unsigned)t->nsnaps, (unsigned)t->nmt, (unsigned)t->niv);
  em(&e, "  %u, syms, %u, irep_hash, %u, methods,\n};\n", (unsigned)t->nslots, (unsigned)t->nireps, (unsigned)t->nmethods);
  em_flush(&e);
  tb_free(G, &tb);
  return 0;
}

/* ---- what the baseline keeps (measuring) ---- */

MRB_API size_t
mrb_checkpoint_baseline_size(mrb_state *mrb, mrb_checkpoint_size_fn fn, void *ud)
{
  checkpoint_state *G = checkpoint_get(mrb);
  if (!G || !G->perm) return 0;
  checkpoint_perm *P = G->perm;
  size_t total = 0;
#define PART(name, count, bytes) do { \
    size_t b_ = (size_t)(bytes); \
    if (b_ > 0) { if (fn) fn(name, (size_t)(count), b_, ud); total += b_; } \
  } while (0)
  PART("struct", 1, sizeof(checkpoint_perm));
  uint32_t nown = P->T ? P->nx : P->n;   /* the names kept in the state */
  PART(P->T ? "names (own)" : "names", nown, nown ? P->name_off[nown] : 0);
  PART(P->T ? "name_off (own)" : "name_off", nown + 1, nown ? sizeof(uint32_t) * (nown + 1) : 0);
  PART(P->T ? "is_func (own)" : "is_func", nown, nown ? (nown + 7) / 8 + 1 : 0);
  PART("ptrs", P->n, sizeof(void*) * P->n);
  PART("obj_idx", P->nobj, sizeof(uint32_t) * P->nobj);
  PART("func_idx", P->nfunc, sizeof(uint32_t) * P->nfunc);
  if (P->keep) PART("keep (Array)", ARY_LEN(P->keep), sizeof(struct RArray) + sizeof(mrb_value) * ARY_CAPA(P->keep));
  if (P->keepp) PART("keep (addresses)", P->nkeep, sizeof(uintptr_t) * P->nkeep);
  if (P->irepp) PART("ireps", P->nireps, sizeof(void*) * P->nireps);
  else PART("ireps", P->nireps, sizeof(girep_perm) * P->nireps);
  PART("irep_idx", P->nireps, sizeof(uint32_t) * P->nireps);
  uint32_t nown_snaps = P->T ? P->nxsnap : P->nsnaps;
  if (P->T) {
    for (uint32_t k = 0; P->snap_over && k < P->T->nsnaps; k++) if (P->snap_over[k]) nown_snaps++;
    PART("snap_over", P->T->nsnaps, P->snap_over ? sizeof(uint32_t) * P->T->nsnaps : 0);
    PART("xsnap", P->nxsnap, sizeof(uint32_t) * P->nxsnap);
    PART("slots", P->T->nslots, (sizeof(uint64_t) + 1) * P->T->nslots);
  }
  PART(P->T ? "snaps (own)" : "snaps", nown_snaps, sizeof(gsnap) * nown_snaps);
  PART(P->T ? "mt (own)" : "mt", P->nmt, sizeof(gsnap_mt) * P->nmt);
  PART(P->T ? "iv (own)" : "iv", P->niv, sizeof(giv_entry) * P->niv);
  PART("gv", P->ngv, sizeof(giv_entry) * P->ngv);
#undef PART
  return total;
}
