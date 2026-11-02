/*
** ghost_util.c - byte buffer, pointer map, hash
*/

#include "ghost_internal.h"

void
gb_init(gbuf *b)
{
  b->p = NULL; b->len = 0; b->cap = 0;
}

void
gb_free(mrb_state *mrb, gbuf *b)
{
  mrb_free(mrb, b->p);
  gb_init(b);
}

void
gb_bytes(mrb_state *mrb, gbuf *b, const void *p, size_t n)
{
  if (b->len + n > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + n) cap *= 2;
    b->p = (uint8_t*)mrb_realloc(mrb, b->p, cap);
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
mrb_ghost_buf_bytes(mrb_state *mrb, mrb_ghost_buf *b, const void *p, size_t len)
{
  gb_bytes(mrb, b, p, len);
}

/* ---- pointer map ---- */

void
pm_init(pmap *m)
{
  m->keys = NULL; m->vals = NULL; m->cap = 0; m->n = 0;
}

void
pm_free(mrb_state *mrb, pmap *m)
{
  mrb_free(mrb, m->keys);
  mrb_free(mrb, m->vals);
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
    m->keys = (uintptr_t*)mrb_calloc(mrb, ncap, sizeof(uintptr_t));
    m->vals = (uint32_t*)mrb_calloc(mrb, ncap, sizeof(uint32_t));
    m->cap = ncap; m->n = 0;
    for (size_t i = 0; i < ocap; i++) {
      if (ok[i]) pm_put_raw(m, ok[i], ov[i]);
    }
    mrb_free(mrb, ok); mrb_free(mrb, ov);
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
