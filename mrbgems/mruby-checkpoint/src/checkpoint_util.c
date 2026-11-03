/*
** checkpoint_util.c - byte buffer, pointer map, hash
*/

#include "checkpoint_internal.h"

/* ---- work areas ---- */

static int
work_alloc_p(checkpoint_state *g)
{
  return g && g->alloc && g->work > 0;
}

static void*
emb_realloc(mrb_state *mrb, mrb_checkpoint_realloc_fn fn, void *ud, void *p, size_t n)
{
  void *q = fn(ud, p, n);
  if (!q && n > 0) mrb_raise_nomemory(mrb);
  return q;
}

void*
ck_realloc(checkpoint_state *g, void *p, size_t n)
{
  if (work_alloc_p(g)) return emb_realloc(g->mrb, g->alloc, g->alloc_ud, p, n ? n : 1);
  return mrb_realloc(g->mrb, p, n);
}

void*
ck_malloc(checkpoint_state *g, size_t n)
{
  return ck_realloc(g, NULL, n);
}

void*
ck_calloc(checkpoint_state *g, size_t n, size_t size)
{
  if (!work_alloc_p(g)) return mrb_calloc(g->mrb, n, size);
  if (size > 0 && n > SIZE_MAX / size) mrb_raise_nomemory(g->mrb);
  void *p = ck_realloc(g, NULL, n * size);
  memset(p, 0, n * size);
  return p;
}

void
ck_free(checkpoint_state *g, void *p)
{
  if (!p) return;
  if (work_alloc_p(g)) g->alloc(g->alloc_ud, p, 0);
  else mrb_free(g->mrb, p);
}

/* the allocator a new buffer or table takes: the embedder's while a dump or
   restore runs (a work area), else the state's (NULL) */
static void
area_alloc(mrb_state *mrb, mrb_checkpoint_realloc_fn *fn, void **ud)
{
  checkpoint_state *g = checkpoint_get(mrb);
  if (work_alloc_p(g)) { *fn = g->alloc; *ud = g->alloc_ud; }
  else { *fn = NULL; *ud = NULL; }
}

static void*
area_realloc(mrb_state *mrb, mrb_checkpoint_realloc_fn fn, void *ud, void *p, size_t n)
{
  if (fn) return emb_realloc(mrb, fn, ud, p, n ? n : 1);
  return mrb_realloc(mrb, p, n);
}

static void
area_free(mrb_state *mrb, mrb_checkpoint_realloc_fn fn, void *ud, void *p)
{
  if (!p) return;
  if (fn) fn(ud, p, 0);
  else mrb_free(mrb, p);
}

/* ---- byte buffer ---- */

void
gb_init(gbuf *b)
{
  b->p = NULL; b->len = 0; b->cap = 0; b->fn = NULL; b->ud = NULL;
}

void
gb_free(mrb_state *mrb, gbuf *b)
{
  area_free(mrb, b->fn, b->ud, b->p);
  gb_init(b);
}

void
gb_bytes(mrb_state *mrb, gbuf *b, const void *p, size_t n)
{
  if (b->len + n > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + n) cap *= 2;
    if (!b->p) area_alloc(mrb, &b->fn, &b->ud);
    b->p = (uint8_t*)area_realloc(mrb, b->fn, b->ud, b->p, cap);
    b->cap = cap;
  }
  if (n > 0) memcpy(b->p + b->len, p, n);
  b->len += n;
}

void
gb_u8(mrb_state *mrb, gbuf *b, uint8_t v)
{
  gb_bytes(mrb, b, &v, 1);
}

void
gb_u16(mrb_state *mrb, gbuf *b, uint16_t v)
{
  uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v };
  gb_bytes(mrb, b, x, 2);
}

void
gb_u32(mrb_state *mrb, gbuf *b, uint32_t v)
{
  uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
  gb_bytes(mrb, b, x, 4);
}

void
gb_u64(mrb_state *mrb, gbuf *b, uint64_t v)
{
  gb_u32(mrb, b, (uint32_t)(v >> 32));
  gb_u32(mrb, b, (uint32_t)v);
}

void
gb_str(mrb_state *mrb, gbuf *b, const char *s, size_t n)
{
  gb_u32(mrb, b, (uint32_t)n);
  gb_bytes(mrb, b, s, n);
}

MRB_API void
mrb_checkpoint_buf_bytes(mrb_state *mrb, mrb_checkpoint_buf *b, const void *p, size_t len)
{
  gb_bytes(mrb, b, p, len);
}

/* ---- pointer map ---- */

void
pm_init(pmap *m)
{
  m->keys = NULL; m->vals = NULL; m->cap = 0; m->n = 0; m->fn = NULL; m->ud = NULL;
}

void
pm_free(mrb_state *mrb, pmap *m)
{
  area_free(mrb, m->fn, m->ud, m->keys);
  area_free(mrb, m->fn, m->ud, m->vals);
  pm_init(m);
}

static size_t
pm_slot(uintptr_t k, size_t cap)
{
  uint64_t h = (uint64_t)k * 0x9e3779b97f4a7c15ULL;
  return (size_t)(h >> 17) & (cap - 1);
}

int
pm_get(const pmap *m, const void *key, uint32_t *val)
{
  uintptr_t k = (uintptr_t)key;
  if (m->cap == 0 || k == 0) return 0;
  size_t i = pm_slot(k, m->cap);
  while (m->keys[i]) {
    if (m->keys[i] == k) {
      if (val) *val = m->vals[i];
      return 1;
    }
    i = (i + 1) & (m->cap - 1);
  }
  return 0;
}

static void
pm_put_raw(pmap *m, uintptr_t k, uint32_t v)
{
  size_t i = pm_slot(k, m->cap);
  while (m->keys[i]) {
    if (m->keys[i] == k) { m->vals[i] = v; return; }
    i = (i + 1) & (m->cap - 1);
  }
  m->keys[i] = k; m->vals[i] = v; m->n++;
}

void
pm_put(mrb_state *mrb, pmap *m, const void *key, uint32_t val)
{
  uintptr_t k = (uintptr_t)key;
  if (k == 0) return;
  if ((m->n + 1) * 2 > m->cap) {
    size_t ncap = m->cap ? m->cap * 2 : 64;
    uintptr_t *ok = m->keys; uint32_t *ov = m->vals; size_t ocap = m->cap;
    if (!ok) area_alloc(mrb, &m->fn, &m->ud);
    m->keys = (uintptr_t*)area_realloc(mrb, m->fn, m->ud, NULL, ncap * sizeof(uintptr_t));
    memset(m->keys, 0, ncap * sizeof(uintptr_t));
    m->vals = (uint32_t*)area_realloc(mrb, m->fn, m->ud, NULL, ncap * sizeof(uint32_t));
    memset(m->vals, 0, ncap * sizeof(uint32_t));
    m->cap = ncap; m->n = 0;
    for (size_t i = 0; i < ocap; i++) {
      if (ok[i]) pm_put_raw(m, ok[i], ov[i]);
    }
    area_free(mrb, m->fn, m->ud, ok); area_free(mrb, m->fn, m->ud, ov);
  }
  pm_put_raw(m, k, val);
}

/* ---- FNV-1a ---- */

uint64_t
fnv_bytes(uint64_t h, const void *p, size_t n)
{
  const uint8_t *s = (const uint8_t*)p;
  for (size_t i = 0; i < n; i++) {
    h ^= s[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

uint64_t
fnv_u64(uint64_t h, uint64_t v)
{
  uint8_t x[8];
  for (int i = 0; i < 8; i++) x[i] = (uint8_t)(v >> (56 - 8*i));
  return fnv_bytes(h, x, 8);
}
