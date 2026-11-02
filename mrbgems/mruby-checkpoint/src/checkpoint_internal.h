/*
** checkpoint_internal.h - shared between the parts of mruby-checkpoint
*/

#ifndef CHECKPOINT_INTERNAL_H
#define CHECKPOINT_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/checkpoint.h>

/* ---- format constants (doc/format_v1.md) ---- */

#define CHECKPOINT_MAGIC "MRCK"
#define CHECKPOINT_FORMAT_VERSION 1

/* value tags */
enum {
  GV_NIL = 0, GV_FALSE = 1, GV_TRUE = 2, GV_INT = 3, GV_FLOAT = 4,
  GV_SYM = 5, GV_OBJ = 6, GV_UNDEF = 7,
};

/* object record kinds */
enum {
  GK_PERM = 1, GK_OBJECT = 2, GK_STRING = 3, GK_ARRAY = 4, GK_HASH = 5,
  GK_RANGE = 6, GK_EXCEPTION = 7, GK_CLASS = 8, GK_MODULE = 9,
  GK_SCLASS = 10, GK_ICLASS = 11, GK_PROC = 12, GK_ENV = 13, GK_DATA = 14,
  GK_PERM_BODY = 15,  /* a permanent Proc a frame runs: its name and its irep (carried) */
  /* P3 */
  GK_BIGINT = 16, GK_STRUCT = 17, GK_SET = 18, GK_RATIONAL = 19, GK_COMPLEX = 20,
  GK_BREAK = 21, GK_RANDOM = 23, GK_ORIGIN = 24,
  GK_BSTRING = 25,   /* a String marked binary (String#b): like 3 */
  /* v1 */
  GK_TASK = 26,      /* a Task object: its record in the scheduler section */
  GK_FIBER = 27,     /* a Fiber object: its execution context */
};

/* frame kinds (v1) */
enum { GF_RUBY = 0, GF_CFUNC = 1 };

/* irep record kinds */
enum { GR_CARRIED = 0, GR_PERM = 1, GR_PERM_CARRIED = 2 };

/* instance types of a class (what Class#allocate makes) */
enum {
  GI_NONE = 0, GI_OBJECT = 1, GI_EXCEPTION = 2, GI_STRING = 3, GI_ARRAY = 4,
  GI_HASH = 5, GI_RANGE = 6, GI_PROC = 7, GI_DATA = 8,
  GI_STRUCT = 9, GI_SET = 10, GI_COMPLEX = 11, GI_RATIONAL = 12, GI_BIGINT = 13,
  GI_RANDOM = 14,   /* an inline C struct: Random's */
  GI_OTHER = 255,
};

/* method entry kinds */
enum { GM_PROC = 0, GM_FUNC = 1, GM_UNDEF = 2 };

/* patch kinds */
enum {
  GP_METHOD_SET = 1, GP_METHOD_REMOVE = 2, GP_IV_SET = 3, GP_IV_REMOVE = 4,
  GP_SUPER_SET = 5, GP_GV_SET = 6, GP_GV_REMOVE = 7,
  GP_RANDOM_SET = 8,   /* the state of a permanent Random (the default one) */
  GP_CLASS_FLAGS = 9,  /* a permanent class got prepended modules */
  GP_CONTENT_SET = 10, /* the contents of a permanent String / Array / Hash (ARGV << x) */
};

/* mruby-random's state, kept inline in the object (MRB_TT_ISTRUCT):
   struct rand_state in mruby-random/src/random.c, 64-bit PCG state + the
   last seed; on 32-bit the state is two 32-bit halves, low first */
#include <mruby/istruct.h>
static inline void
checkpoint_random_get(mrb_value v, uint64_t *state, uint32_t *seed)
{
  const uint8_t *p = (const uint8_t*)mrb_istruct_ptr(v);
#ifdef MRB_32BIT
  uint32_t lo, hi;
  memcpy(&lo, p, 4); memcpy(&hi, p + 4, 4);
  *state = ((uint64_t)hi << 32) | lo;
#else
  memcpy(state, p, 8);
#endif
  memcpy(seed, p + 8, 4);
}
static inline void
checkpoint_random_set(mrb_value v, uint64_t state, uint32_t seed)
{
  uint8_t *p = (uint8_t*)mrb_istruct_ptr(v);
#ifdef MRB_32BIT
  uint32_t lo = (uint32_t)state, hi = (uint32_t)(state >> 32);
  memcpy(p, &lo, 4); memcpy(p + 4, &hi, 4);
#else
  memcpy(p, &state, 8);
#endif
  memcpy(p + 8, &seed, 4);
}

/* ---- byte buffer ---- */

struct mrb_checkpoint_buf {
  uint8_t *p;
  size_t len, cap;
};
typedef struct mrb_checkpoint_buf gbuf;

void gb_init(gbuf *b);
void gb_free(mrb_state *mrb, gbuf *b);
void gb_bytes(mrb_state *mrb, gbuf *b, const void *p, size_t n);
void gb_u8(mrb_state *mrb, gbuf *b, uint8_t v);
void gb_u16(mrb_state *mrb, gbuf *b, uint16_t v);
void gb_u32(mrb_state *mrb, gbuf *b, uint32_t v);
void gb_u64(mrb_state *mrb, gbuf *b, uint64_t v);
void gb_str(mrb_state *mrb, gbuf *b, const char *s, size_t n); /* u32 len + bytes */

/* ---- pointer map (uintptr_t -> uint32_t), open addressing ---- */

typedef struct {
  uintptr_t *keys;
  uint32_t *vals;
  size_t cap, n;
} pmap;

void pm_init(pmap *m);
void pm_free(mrb_state *mrb, pmap *m);
int pm_get(const pmap *m, const void *key, uint32_t *val);
void pm_put(mrb_state *mrb, pmap *m, const void *key, uint32_t val);

/* ---- FNV-1a 64 ---- */
#define FNV_INIT 0xcbf29ce484222325ULL
uint64_t fnv_bytes(uint64_t h, const void *p, size_t n);
uint64_t fnv_u64(uint64_t h, uint64_t v);

/* ---- baseline: permanent objects and their names ---- */

typedef struct {
  mrb_sym sym;
  uint32_t flags;
  const void *ptr;
} gmt_entry;

typedef struct {
  mrb_sym sym;
  mrb_value val;
} giv_entry;

typedef struct {
  struct RBasic *obj;     /* class, module, sclass, iclass or plain object */
  struct RClass *super;   /* classes only */
  uint32_t cflags;        /* classes only: MRB_FL_CLASS_IS_PREPENDED */
  gmt_entry *mt; int nmt;
  giv_entry *iv; int niv;
  uint64_t digest;        /* string/array/hash contents */
} gsnap;

typedef struct {
  const mrb_irep *irep;
  uint64_t hash;
} girep_perm;

/* the settings of the GC a program can change (GC.disable, GC.interval_ratio=, ...) */
typedef struct {
  uint8_t flags;              /* bit0 disabled, bit1 generational, bit2 auto_step, bit3 sched_driven */
  uint32_t interval_ratio, step_ratio;
  uint64_t step_limit, malloc_threshold;
  int64_t debt_limit;
} gc_settings;

void checkpoint_gc_settings(mrb_state *mrb, gc_settings *out);

typedef struct checkpoint_perm {
  /* named permanent things (objects and C functions) */
  char **names;
  const void **ptrs;
  uint8_t *is_func;
  uint32_t n, cap;
  pmap obj2name;     /* object pointer -> name index + 1 */
  pmap func2name;    /* C function pointer -> name index + 1 */

  pmap heap;         /* every heap object alive at baseline */

  girep_perm *ireps; uint32_t nireps, capireps;
  pmap irep2idx;     /* irep pointer -> index + 1 */

  gsnap *snaps; uint32_t nsnaps, capsnaps;
  giv_entry *gv; int ngv;
  gc_settings gc;    /* at the baseline */
} checkpoint_perm;

typedef struct checkpoint_data_ent {
  const mrb_data_type *type;
  char *name;
  mrb_checkpoint_data_dump dump;
  mrb_checkpoint_data_restore restore;
  mrb_checkpoint_data_restore_obj restore_obj;
} checkpoint_data_ent;

/* the entries for the C data of mruby's bundled gems (checkpoint_bundled.c) */
void checkpoint_bundled_register(mrb_state *mrb);

typedef struct checkpoint_state {
  mrb_state *mrb;
  checkpoint_perm *perm;
  int mode;
  int pending;                /* a save / suspend waits for the next safe point */
  enum mrb_checkpoint_kind pending_kind;
  char *pending_path;
  ptrdiff_t ret_ci;           /* the calling frame (index from cibase) and */
  ptrdiff_t ret_slot;         /* the stack slot that receives the call's value */
  char *output;
  int resumed;
  mrb_checkpoint_handler handler;
  void *handler_ud;
  checkpoint_data_ent *data; int ndata;
  mrb_checkpoint_refusal_handler refusal;   /* where external requests report a refusal */
  void *refusal_ud;
  int keep_dead;              /* write dead registers too (no liveness pass) */
  char **fnames; mrb_func_t *funcs; int nfuncs;   /* mrb_checkpoint_register_func */
  uint32_t last_dead;         /* non-nil registers the last image wrote as nil */
  /* v1: Tasks and Fibers */
  struct mrb_context *ret_ctx;  /* the context of the save / suspend call */
  int dump_switch;            /* the image is taken where the scheduler asked for a switch */
  void *host_task;            /* the embedder's Task (mrb_checkpoint_set_host_task), an mrb_task */
  const mrb_data_type *task_type;   /* mruby-task's (static there; probed) */
  int keep_timeslice;         /* the running Task goes on with what was left of its timeslice */
  /* the scheduler as the request found it (see mark_switch in checkpoint.c) */
  int own_switch;             /* the request set task.switching (it was clear) */
  void *snap_task;            /* the running Task then */
  void **snap_wait; uint8_t *snap_reason; int nsnap_wait;   /* the waiting Tasks and why */
  void **snap_ready; int nsnap_ready;                       /* the ready ones */
  /* what mrb_checkpoint_continue starts again (set by the restore) */
  struct checkpoint_resume_plan {
    int set;
    struct mrb_context *current;  /* the context that was running */
    void *task;                   /* the Task that was running, an mrb_task (NULL: none) */
    int switch_pending;           /* the scheduler had asked for a switch there */
    int loop_running;             /* the scheduler's loop was running */
  } plan;
} checkpoint_state;

checkpoint_state *checkpoint_get(mrb_state *mrb);

checkpoint_perm *checkpoint_perm_build(mrb_state *mrb, checkpoint_state *g);
void checkpoint_perm_free(mrb_state *mrb, checkpoint_perm *P);
const char *checkpoint_perm_name_of(checkpoint_perm *P, const void *obj);
const char *checkpoint_perm_func_name(checkpoint_perm *P, mrb_func_t f);
const void *checkpoint_perm_lookup(checkpoint_perm *P, const char *name, size_t len, int *is_func);
uint32_t checkpoint_perm_count(checkpoint_perm *P);
const char *checkpoint_perm_name_at(checkpoint_perm *P, uint32_t i, int *is_func);
int checkpoint_perm_irep(checkpoint_perm *P, const mrb_irep *irep, uint64_t *hash);
const mrb_irep *checkpoint_perm_irep_by_hash(checkpoint_perm *P, uint64_t hash);
gsnap *checkpoint_perm_snap(checkpoint_perm *P, const void *obj);
uint64_t checkpoint_irep_hash(mrb_state *mrb, const mrb_irep *irep);
uint64_t checkpoint_content_digest(mrb_state *mrb, struct RBasic *obj);

/* snapshots of a class's method table and ivars (also used by the dumper) */
void checkpoint_collect_mt(mrb_state *mrb, struct RClass *c, gmt_entry **out, int *n);
void checkpoint_collect_iv(mrb_state *mrb, mrb_value obj, giv_entry **out, int *n);
void checkpoint_collect_gv(mrb_state *mrb, giv_entry **out, int *n);

/* liveness: keep[s] = 1 for the stack slots whose values are still read */
void checkpoint_live_slots(mrb_state *mrb, struct mrb_context *c, size_t nslots, uint8_t *keep);

/* dump: returns 0 on success; on failure fills err */
int checkpoint_dump(mrb_state *mrb, gbuf *out, char *err, size_t errlen);

/* the instruction that ends at pos (checkpoint_live.c) */
int checkpoint_insn_ending_at(const mrb_irep *irep, uint32_t pos, uint32_t *start, uint8_t *op,
                              uint32_t *a, uint32_t *b, uint32_t *c);

/* Tasks and Fibers (checkpoint_task.c) */
int checkpoint_switch_func_name(const char *name, size_t len);
const mrb_data_type *checkpoint_task_type(mrb_state *mrb);
void checkpoint_task_register(mrb_state *mrb);
struct mrb_task *checkpoint_task_of_context(mrb_state *mrb, struct mrb_context *c);
struct mrb_task *checkpoint_running_task(mrb_state *mrb);

#endif
