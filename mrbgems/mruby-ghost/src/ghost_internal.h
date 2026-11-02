/*
** ghost_internal.h - shared between the parts of mruby-ghost
*/

#ifndef GHOST_INTERNAL_H
#define GHOST_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/ghost.h>

/* ---- format constants (doc/format_v0.md) ---- */

#define GHOST_MAGIC "MRGH"
#define GHOST_FORMAT_VERSION 0

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
};

/* instance types of a class (what Class#allocate makes) */
enum {
  GI_NONE = 0, GI_OBJECT = 1, GI_EXCEPTION = 2, GI_STRING = 3, GI_ARRAY = 4,
  GI_HASH = 5, GI_RANGE = 6, GI_PROC = 7, GI_DATA = 8, GI_OTHER = 255,
};

/* method entry kinds */
enum { GM_PROC = 0, GM_FUNC = 1, GM_UNDEF = 2 };

/* patch kinds */
enum {
  GP_METHOD_SET = 1, GP_METHOD_REMOVE = 2, GP_IV_SET = 3, GP_IV_REMOVE = 4,
  GP_SUPER_SET = 5, GP_GV_SET = 6, GP_GV_REMOVE = 7,
};

/* ---- byte buffer ---- */

struct mrb_ghost_buf {
  uint8_t *p;
  size_t len, cap;
};
typedef struct mrb_ghost_buf gbuf;

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
  gmt_entry *mt; int nmt;
  giv_entry *iv; int niv;
  uint64_t digest;        /* string/array/hash contents */
} gsnap;

typedef struct {
  const mrb_irep *irep;
  uint64_t hash;
} girep_perm;

typedef struct ghost_perm {
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
} ghost_perm;

typedef struct ghost_data_ent {
  const mrb_data_type *type;
  char *name;
  mrb_ghost_data_dump dump;
  mrb_ghost_data_restore restore;
} ghost_data_ent;

typedef struct ghost_state {
  mrb_state *mrb;
  ghost_perm *perm;
  int mode;
  int pending;
  char *pending_path;
  char *output;
  int resumed;
  mrb_ghost_hop_handler handler;
  void *handler_ud;
  ghost_data_ent *data; int ndata;
} ghost_state;

ghost_state *ghost_get(mrb_state *mrb);

ghost_perm *ghost_perm_build(mrb_state *mrb);
void ghost_perm_free(mrb_state *mrb, ghost_perm *P);
const char *ghost_perm_name_of(ghost_perm *P, const void *obj);
const char *ghost_perm_func_name(ghost_perm *P, mrb_func_t f);
const void *ghost_perm_lookup(ghost_perm *P, const char *name, size_t len, int *is_func);
int ghost_perm_irep(ghost_perm *P, const mrb_irep *irep, uint64_t *hash);
const mrb_irep *ghost_perm_irep_by_hash(ghost_perm *P, uint64_t hash);
gsnap *ghost_perm_snap(ghost_perm *P, const void *obj);
uint64_t ghost_irep_hash(mrb_state *mrb, const mrb_irep *irep);
uint64_t ghost_content_digest(mrb_state *mrb, struct RBasic *obj);

/* snapshots of a class's method table and ivars (also used by the dumper) */
void ghost_collect_mt(mrb_state *mrb, struct RClass *c, gmt_entry **out, int *n);
void ghost_collect_iv(mrb_state *mrb, mrb_value obj, giv_entry **out, int *n);
void ghost_collect_gv(mrb_state *mrb, giv_entry **out, int *n);

/* dump: returns 0 on success; on failure fills err */
int ghost_dump(mrb_state *mrb, gbuf *out, char *err, size_t errlen);

#endif
