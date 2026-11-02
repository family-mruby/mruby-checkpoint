/*
** checkpoint_bundled.c - checkpoint data entries for the C data of mruby's
** bundled gems (format v1, section 7, kind 14)
**
** Registered by mrb_checkpoint_baseline() for the gems the build has (found
** at run time: no link dependency). The gems keep their structs and data
** types private, so:
**   - the data type is taken from an instance made for the purpose, before
**     the baseline (made objects are garbage by then; globals a probe sets
**     are removed again);
**   - the head of each struct is mirrored here; the comment names the file
**     the mirror follows (mruby f56d44ecc).
** What is written is what the value means, not the struct: seconds and
** nanoseconds, capture offsets, counters.
**
**   Time                    mruby-time      sec, nsec, zone
**   Regexp                  mruby-regexp    nothing: @source and @flags are
**                                           ivars; compiled again on restore
**   MatchData               mruby-regexp    the capture offsets; source and
**                                           regexp are ivars too
**   CombinationState        mruby-array-ext k, n, mode, indices (or none: done)
**   ary_product_generator   mruby-array-ext total, cursor
*/

#include <string.h>
#include <time.h>
#include <mruby.h>
#include <mruby/data.h>
#include <mruby/class.h>
#include <mruby/string.h>
#include <mruby/array.h>
#include <mruby/variable.h>
#include <mruby/error.h>
#include <mruby/proc.h>
#include "checkpoint_internal.h"

static void
put64(uint8_t *p, uint64_t v)
{
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i));
}

static uint64_t
get64(const uint8_t *p)
{
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

static const mrb_data_type*
type_of(mrb_value v)
{
  return (mrb_type(v) == MRB_TT_CDATA && DATA_PTR(v)) ? DATA_TYPE(v) : NULL;
}

/* ---- Time (mruby-time/src/time.c, struct mrb_time) ---- */

struct ck_time {
  time_t sec;
  time_t nsec;
  int timezone;           /* enum mrb_timezone: 1 UTC, 2 local */
  struct tm datetime;
};

static int
time_dump(mrb_state *mrb, mrb_value obj, mrb_checkpoint_buf *out)
{
  struct ck_time *t = (struct ck_time*)DATA_PTR(obj);
  uint8_t b[17];
  if (!t) return 1;
  put64(b, (uint64_t)(int64_t)t->sec);
  put64(b + 8, (uint64_t)(int64_t)t->nsec);
  b[16] = (uint8_t)t->timezone;
  mrb_checkpoint_buf_bytes(mrb, out, b, sizeof(b));
  return 0;
}

static void*
time_restore(mrb_state *mrb, const uint8_t *bytes, size_t len)
{
  struct ck_time *t = (struct ck_time*)mrb_malloc(mrb, sizeof(struct ck_time));
  memset(t, 0, sizeof(*t));
  if (len >= 17) {
    t->sec = (time_t)(int64_t)get64(bytes);
    t->nsec = (time_t)(int64_t)get64(bytes + 8);
    t->timezone = bytes[16];
  }
  time_t s = t->sec;   /* the broken-down time, computed again as mruby-time does */
  if (t->timezone == 1) gmtime_r(&s, &t->datetime);
  else localtime_r(&s, &t->datetime);
  return t;
}

/* ---- Regexp, MatchData (mruby-regexp/src/regexp.c) ---- */

static int
regexp_dump(mrb_state *mrb, mrb_value obj, mrb_checkpoint_buf *out)
{
  (void)mrb; (void)out;
  return DATA_PTR(obj) ? 0 : 1;
}

static void*
regexp_restore(mrb_state *mrb, mrb_value obj, const uint8_t *bytes, size_t len, char *why, size_t wl)
{
  (void)bytes; (void)len;
  /* Regexp.new(obj) copies @source and @flags from obj; the compiled
     pattern of that new Regexp becomes obj's */
  mrb_value src = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "@source"));
  if (!mrb_string_p(src)) { snprintf(why, wl, "no @source"); return NULL; }
  mrb_value tmp = mrb_funcall_argv(mrb, mrb_obj_value(mrb_class_get(mrb, "Regexp")),
                                   mrb_intern_lit(mrb, "new"), 1, &obj);
  if (mrb_type(tmp) != MRB_TT_CDATA || !DATA_PTR(tmp)) { snprintf(why, wl, "not compiled"); return NULL; }
  void *pat = DATA_PTR(tmp);
  DATA_PTR(tmp) = NULL;   /* regexp_free(NULL) does nothing */
  return pat;
}

typedef struct {
  mrb_value source;
  mrb_value regexp;
  int *captures;           /* [start0, end0, start1, end1, ...] in bytes, -1 = none */
  int num_captures;
} ck_match_data;

static int
match_dump(mrb_state *mrb, mrb_value obj, mrb_checkpoint_buf *out)
{
  ck_match_data *md = (ck_match_data*)DATA_PTR(obj);
  if (!md) return 1;
  uint8_t b[8];
  put64(b, (uint64_t)md->num_captures);
  mrb_checkpoint_buf_bytes(mrb, out, b, 8);
  for (int i = 0; i < md->num_captures * 2; i++) {
    put64(b, (uint64_t)(int64_t)md->captures[i]);
    mrb_checkpoint_buf_bytes(mrb, out, b, 8);
  }
  return 0;
}

static void*
match_restore(mrb_state *mrb, mrb_value obj, const uint8_t *bytes, size_t len, char *why, size_t wl)
{
  if (len < 8) { snprintf(why, wl, "short"); return NULL; }
  int n = (int)get64(bytes);
  if (n < 0 || len < 8 + (size_t)n * 16) { snprintf(why, wl, "short"); return NULL; }
  ck_match_data *md = (ck_match_data*)mrb_malloc(mrb, sizeof(ck_match_data));
  md->source = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "source"));
  md->regexp = mrb_iv_get(mrb, obj, mrb_intern_lit(mrb, "regexp"));
  md->num_captures = n;
  md->captures = (int*)mrb_malloc(mrb, sizeof(int) * (n > 0 ? n * 2 : 1));
  for (int i = 0; i < n * 2; i++) md->captures[i] = (int)(int64_t)get64(bytes + 8 + 8 * i);
  return md;
}

/* ---- Array#combination etc. (mruby-array-ext/src/array.c) ---- */

struct ck_combination_state {
  mrb_int *indices;
  mrb_int n, k;
  int mode;
};

static int
comb_dump(mrb_state *mrb, mrb_value obj, mrb_checkpoint_buf *out)
{
  struct ck_combination_state *s = (struct ck_combination_state*)DATA_PTR(obj);
  if (!s) return 1;
  uint8_t b[8];
  put64(b, (uint64_t)s->k); mrb_checkpoint_buf_bytes(mrb, out, b, 8);
  put64(b, (uint64_t)s->n); mrb_checkpoint_buf_bytes(mrb, out, b, 8);
  put64(b, (uint64_t)s->mode); mrb_checkpoint_buf_bytes(mrb, out, b, 8);
  /* indices is freed (NULL) when the iteration is over */
  b[0] = s->indices ? 1 : 0; mrb_checkpoint_buf_bytes(mrb, out, b, 1);
  for (mrb_int i = 0; s->indices && i < s->k; i++) {
    put64(b, (uint64_t)s->indices[i]);
    mrb_checkpoint_buf_bytes(mrb, out, b, 8);
  }
  return 0;
}

static void*
comb_restore(mrb_state *mrb, const uint8_t *bytes, size_t len)
{
  struct ck_combination_state *s = (struct ck_combination_state*)mrb_malloc(mrb, sizeof(*s));
  memset(s, 0, sizeof(*s));
  if (len >= 24) {
    s->k = (mrb_int)get64(bytes);
    s->n = (mrb_int)get64(bytes + 8);
    s->mode = (int)get64(bytes + 16);
  }
  if (len < 25 || !bytes[24]) { s->indices = NULL; return s; }
  if (s->k < 0 || len < 25 + (size_t)s->k * 8) s->k = 0;
  s->indices = (mrb_int*)mrb_calloc(mrb, s->k > 0 ? s->k : 1, sizeof(mrb_int));
  for (mrb_int i = 0; i < s->k; i++) s->indices[i] = (mrb_int)get64(bytes + 25 + 8 * i);
  return s;
}

struct ck_product_generator {
  mrb_int total;
  mrb_int cursor;
};

static int
prod_dump(mrb_state *mrb, mrb_value obj, mrb_checkpoint_buf *out)
{
  struct ck_product_generator *g = (struct ck_product_generator*)DATA_PTR(obj);
  if (!g) return 1;
  uint8_t b[16];
  put64(b, (uint64_t)g->total);
  put64(b + 8, (uint64_t)g->cursor);
  mrb_checkpoint_buf_bytes(mrb, out, b, 16);
  return 0;
}

static void*
prod_restore(mrb_state *mrb, const uint8_t *bytes, size_t len)
{
  struct ck_product_generator *g = (struct ck_product_generator*)mrb_malloc(mrb, sizeof(*g));
  g->total = len >= 16 ? (mrb_int)get64(bytes) : 0;
  g->cursor = len >= 16 ? (mrb_int)get64(bytes + 8) : 0;
  return g;
}

/* ---- the probes ---- */

static mrb_value
probe_block(mrb_state *mrb, mrb_value self)
{
  (void)mrb; (void)self;
  return mrb_nil_value();
}

typedef struct { const mrb_data_type *time, *regexp, *match, *comb, *prod; } probed;

static mrb_value
probe(mrb_state *mrb, void *ud)
{
  probed *p = (probed*)ud;
  if (mrb_class_defined(mrb, "Time")) {
    mrb_value z = mrb_fixnum_value(0);
    p->time = type_of(mrb_funcall_argv(mrb, mrb_obj_value(mrb_class_get(mrb, "Time")),
                                       mrb_intern_lit(mrb, "at"), 1, &z));
  }
  if (mrb_class_defined(mrb, "Regexp") && mrb_class_defined(mrb, "MatchData")) {
    mrb_value s = mrb_str_new_lit(mrb, "a");
    mrb_value re = mrb_funcall_argv(mrb, mrb_obj_value(mrb_class_get(mrb, "Regexp")),
                                    mrb_intern_lit(mrb, "new"), 1, &s);
    p->regexp = type_of(re);
    p->match = type_of(mrb_funcall_argv(mrb, re, mrb_intern_lit(mrb, "match"), 1, &s));
  }
  mrb_value ary = mrb_ary_new(mrb);
  mrb_ary_push(mrb, ary, mrb_fixnum_value(1));
  mrb_sym ci = mrb_intern_lit(mrb, "__combination_init");
  if (mrb_respond_to(mrb, ary, ci)) {
    mrb_value a[2] = { mrb_symbol_value(mrb_intern_lit(mrb, "combination")), mrb_fixnum_value(1) };
    p->comb = type_of(mrb_funcall_argv(mrb, ary, ci, 2, a));
  }
  mrb_sym pg = mrb_intern_lit(mrb, "__product_generate");
  if (mrb_respond_to(mrb, ary, pg)) {
    mrb_value arys = mrb_ary_new(mrb);
    mrb_ary_push(mrb, arys, ary);
    /* the generator is made only for a block */
    struct RProc *blk = mrb_proc_new_cfunc(mrb, probe_block);
    p->prod = type_of(mrb_funcall_with_block(mrb, ary, pg, 1, &arys, mrb_obj_value(blk)));
  }
  return mrb_nil_value();
}

static int
registered(mrb_state *mrb, const mrb_data_type *t)
{
  checkpoint_state *g = checkpoint_get(mrb);
  for (int i = 0; g && i < g->ndata; i++) if (g->data[i].type == t) return 1;
  return 0;
}

void
checkpoint_bundled_register(mrb_state *mrb)
{
  probed p;
  memset(&p, 0, sizeof(p));
  int ai = mrb_gc_arena_save(mrb);
  mrb_bool err;
  mrb_protect_error(mrb, probe, &p, &err);
  mrb->exc = NULL;
  /* the match above set $~ (and its friends): leave the globals as mrb_open did */
  static const char *gvs[] = { "$~", "$1", "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$9" };
  for (size_t i = 0; i < sizeof(gvs) / sizeof(gvs[0]); i++) {
    mrb_gv_remove(mrb, mrb_intern_cstr(mrb, gvs[i]));
  }
  mrb_gc_arena_restore(mrb, ai);

  if (p.time && !registered(mrb, p.time))
    mrb_checkpoint_register_data(mrb, p.time, "Time", time_dump, time_restore);
  if (p.regexp && !registered(mrb, p.regexp))
    mrb_checkpoint_register_data_obj(mrb, p.regexp, "Regexp", regexp_dump, regexp_restore);
  if (p.match && !registered(mrb, p.match))
    mrb_checkpoint_register_data_obj(mrb, p.match, "MatchData", match_dump, match_restore);
  if (p.comb && !registered(mrb, p.comb))
    mrb_checkpoint_register_data(mrb, p.comb, "CombinationState", comb_dump, comb_restore);
  if (p.prod && !registered(mrb, p.prod))
    mrb_checkpoint_register_data(mrb, p.prod, "ary_product_generator", prod_dump, prod_restore);
}
