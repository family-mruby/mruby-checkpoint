/*
** checkpoint_sends.c - the method names the program's code calls (SND0)
**
** A permanent method is bound by name only when the image refers to its
** object (the names of section 8). A call is a symbol in an instruction, so
** a method the program calls but does not hold is not checked by the names:
** on a reader whose build lacks it, the restore succeeds and the call raises
** NoMethodError later. The writer therefore lists the names its program's
** code sends (doc/format_v1.md section 13, SND0), and the reader says which
** of them no class of its baseline defines.
**
** The list is name-level, not receiver-level:
**   - the code: every Proc in the heap whose irep is the program's (not the
**     baseline's), and their child ireps; the operands of OP_SEND, OP_SEND0,
**     OP_SENDB, OP_SSEND, OP_SSEND0 and OP_SSENDB (calls with and without a
**     receiver, attribute calls, safe navigation `&.`). super, the operators
**     the VM handles itself ([], +, ...), `send` with a computed name, eval'd
**     code not compiled yet and calls made from C are not seen;
**   - kept: the names some class or module of the writer's baseline defines
**     (a method of the build), leaving out those the program defines itself
**     (in a class of its own, or added to a permanent class), so that an
**     app's own methods and its method_missing names are not listed;
**   - the reader's answer: a name no class or module of its baseline defines.
**     A name another class defines passes (the check does not know the
**     receiver).
** The same list is what mrb_checkpoint_sent_names gives without an image (a
** check before anything is stopped).
*/

#include <stdlib.h>
#include <mruby.h>
#include <mruby/irep.h>
#include <mruby/proc.h>
#include <mruby/opcode.h>
#include <mruby/class.h>
#include <mruby/internal.h>
#include <mruby/gc.h>
#include "checkpoint_internal.h"

typedef struct {
  checkpoint_state *G;
  checkpoint_perm *P;
  const mrb_irep **ireps; uint32_t nirep, cap;
  pmap seen;                 /* ireps already listed */
  mrb_sym *defs; uint32_t ndef, capdef;   /* names the program defines */
} sends_walk;

static void
add_irep(mrb_state *mrb, sends_walk *w, const mrb_irep *irep)
{
  if (!irep || pm_get(&w->seen, irep, NULL)) return;
  if (checkpoint_perm_irep(w->P, irep, NULL)) return;
  pm_put(mrb, &w->seen, irep, 1);
  if (w->nirep == w->cap) {
    w->cap = w->cap ? w->cap * 2 : 64;
    w->ireps = (const mrb_irep**)ck_realloc(w->G, w->ireps, sizeof(void*) * w->cap);
  }
  w->ireps[w->nirep++] = irep;
  for (int i = 0; irep->reps && i < irep->rlen; i++) add_irep(mrb, w, irep->reps[i]);
}

static void
add_def(sends_walk *w, mrb_sym sym)
{
  if (w->ndef == w->capdef) {
    w->capdef = w->capdef ? w->capdef * 2 : 64;
    w->defs = (mrb_sym*)ck_realloc(w->G, w->defs, sizeof(mrb_sym) * w->capdef);
  }
  w->defs[w->ndef++] = sym;
}

static int
walk_cb(mrb_state *mrb, struct RBasic *obj, void *ud)
{
  sends_walk *w = (sends_walk*)ud;
  switch (obj->tt) {
  case MRB_TT_PROC: {
    const struct RProc *p = (const struct RProc*)obj;
    /* a Proc alive at the baseline is the build's, whatever its irep */
    if (checkpoint_perm_heap_p(w->P, obj)) break;
    if (!MRB_PROC_CFUNC_P(p) && !MRB_PROC_ALIAS_P(p)) add_irep(mrb, w, p->body.irep);
    break;
  }
  case MRB_TT_CLASS: case MRB_TT_MODULE: case MRB_TT_SCLASS: {
    /* a class of the program's own: what it defines is the program's */
    if (checkpoint_perm_heap_p(w->P, obj)) break;
    gmt_entry *mt; int n;
    checkpoint_collect_mt(mrb, (struct RClass*)obj, &mt, &n);
    for (int i = 0; i < n; i++) if (mt[i].ptr) add_def(w, mt[i].sym);
    checkpoint_collect_free(mrb, mt);
    break;
  }
  default:
    break;
  }
  return MRB_EACH_OBJ_OK;
}

/* methods the program set on a permanent class since the baseline */
static void
patched_defs(mrb_state *mrb, sends_walk *w)
{
  checkpoint_perm *P = w->P;
  for (uint32_t k = 0; k < P->nsnaps; k++) {
    gsnap *s = &P->snaps[k];
    struct RBasic *o = s->obj;
    if (o->tt != MRB_TT_CLASS && o->tt != MRB_TT_MODULE && o->tt != MRB_TT_SCLASS) continue;
    gmt_entry *mt; int n;
    checkpoint_collect_mt(mrb, (struct RClass*)o, &mt, &n);
    const gsnap_mt *smt = P->mt + s->mt;
    for (int i = 0; i < n; i++) {
      if (!mt[i].ptr) continue;
      int j;
      for (j = 0; j < s->nmt; j++) if (smt[j].sym == mt[i].sym) break;
      if (j < s->nmt && checkpoint_perm_ref(P, smt[j].ref) == mt[i].ptr) continue;
      add_def(w, mt[i].sym);
    }
    checkpoint_collect_free(mrb, mt);
  }
}

static int
cmp_sym(const void *a, const void *b)
{
  mrb_sym x = *(const mrb_sym*)a, y = *(const mrb_sym*)b;
  return x < y ? -1 : x > y;
}

static int
has_sym(const mrb_sym *s, uint32_t n, mrb_sym k)
{
  uint32_t lo = 0, hi = n;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (s[mid] == k) return 1;
    if (s[mid] < k) lo = mid + 1;
    else hi = mid;
  }
  return 0;
}

/* sorts and drops the repeats */
static uint32_t
uniq_syms(mrb_sym *s, uint32_t n)
{
  if (n == 0) return 0;
  qsort(s, n, sizeof(mrb_sym), cmp_sym);
  uint32_t k = 1;
  for (uint32_t i = 1; i < n; i++) if (s[i] != s[k - 1]) s[k++] = s[i];
  return k;
}

uint32_t
checkpoint_method_syms(checkpoint_state *G, checkpoint_perm *P, mrb_sym **out)
{
  mrb_sym *s = (mrb_sym*)ck_malloc(G, sizeof(mrb_sym) * (P->nmt ? P->nmt : 1));
  uint32_t n = 0;
  for (uint32_t i = 0; i < P->nmt; i++) if (P->mt[i].ref != 0) s[n++] = P->mt[i].sym;
  *out = s;
  return uniq_syms(s, n);
}

static mrb_state *sort_mrb;   /* for the name order below (qsort has no user data) */

static int
cmp_name(const void *a, const void *b)
{
  mrb_int la, lb;
  const char *x = mrb_sym_name_len(sort_mrb, *(const mrb_sym*)a, &la);
  const char *y = mrb_sym_name_len(sort_mrb, *(const mrb_sym*)b, &lb);
  int c = memcmp(x, y, (size_t)(la < lb ? la : lb));
  return c ? c : (la < lb ? -1 : la > lb);
}

uint32_t
checkpoint_sent_syms(mrb_state *mrb, checkpoint_state *G, checkpoint_perm *P, mrb_sym **out)
{
  sends_walk w;
  memset(&w, 0, sizeof(w));
  w.G = G; w.P = P;
  pm_init(&w.seen);
  mrb_bool gc_was = mrb->gc.disabled;
  mrb->gc.disabled = TRUE;
  mrb_objspace_each_objects(mrb, walk_cb, &w);
  mrb->gc.disabled = gc_was;
  patched_defs(mrb, &w);
  w.ndef = uniq_syms(w.defs, w.ndef);

  mrb_sym *have; uint32_t nhave = checkpoint_method_syms(G, P, &have);
  mrb_sym *sent = NULL; uint32_t nsent = 0, cap = 0;
  for (uint32_t i = 0; i < w.nirep; i++) {
    const mrb_irep *irep = w.ireps[i];
    uint32_t pos = 0;
    while (pos < irep->ilen) {
      uint32_t end; uint8_t op; uint32_t a, b, c;
      if (!checkpoint_insn_at(irep, pos, &end, &op, &a, &b, &c)) break;
      pos = end;
      switch (op) {
      case OP_SEND: case OP_SEND0: case OP_SENDB:
      case OP_SSEND: case OP_SSEND0: case OP_SSENDB:
        break;
      default:
        continue;
      }
      if (!irep->syms || b >= irep->slen) continue;
      mrb_sym s = irep->syms[b];
      if (!has_sym(have, nhave, s) || has_sym(w.defs, w.ndef, s)) continue;
      if (nsent == cap) {
        cap = cap ? cap * 2 : 64;
        sent = (mrb_sym*)ck_realloc(G, sent, sizeof(mrb_sym) * cap);
      }
      sent[nsent++] = s;
    }
  }
  nsent = uniq_syms(sent, nsent);
  sort_mrb = mrb;
  if (nsent > 1) qsort(sent, nsent, sizeof(mrb_sym), cmp_name);
  ck_free(G, have);
  ck_free(G, w.defs);
  ck_free(G, w.ireps);
  pm_free(mrb, &w.seen);
  *out = sent;
  return nsent;
}

void
checkpoint_set_missing(mrb_state *mrb, checkpoint_state *G, const mrb_sym *names, uint32_t n)
{
  mrb_free(mrb, G->missing);
  G->missing = NULL;
  G->missing_len = 0;
  G->nmissing = 0;
  if (n == 0) return;
  checkpoint_perm *P = G->perm;
  mrb_sym *have; uint32_t nhave = checkpoint_method_syms(G, P, &have);
  size_t len = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (names[i] == 0 || has_sym(have, nhave, names[i])) continue;
    mrb_int l; mrb_sym_name_len(mrb, names[i], &l);
    len += (size_t)l + 1;
  }
  if (len > 0) {
    G->missing = (char*)mrb_malloc(mrb, len);
    for (uint32_t i = 0; i < n; i++) {
      if (names[i] == 0 || has_sym(have, nhave, names[i])) continue;
      mrb_int l; const char *s = mrb_sym_name_len(mrb, names[i], &l);
      memcpy(G->missing + G->missing_len, s, (size_t)l);
      G->missing[G->missing_len + l] = 0;
      G->missing_len += (size_t)l + 1;
      G->nmissing++;
    }
  }
  ck_free(G, have);
}

MRB_API int
mrb_checkpoint_sent_names(mrb_state *mrb, mrb_checkpoint_method_fn fn, void *ud)
{
  checkpoint_state *G = checkpoint_get(mrb);
  if (!G || !G->perm) return -1;
  /* the tables are work areas, as in a dump (a nearly full pool can ask) */
  int work = G->work;
  G->work = 1;
  mrb_sym *s;
  uint32_t n = checkpoint_sent_syms(mrb, G, G->perm, &s);
  for (uint32_t i = 0; fn && i < n; i++) {
    mrb_int l; const char *name = mrb_sym_name_len(mrb, s[i], &l);
    fn(mrb, name, (size_t)l, ud);
  }
  ck_free(G, s);
  G->work = work;
  return (int)n;
}

MRB_API int
mrb_checkpoint_has_method(mrb_state *mrb, const char *name, size_t len)
{
  checkpoint_state *G = checkpoint_get(mrb);
  if (!G || !G->perm) return -1;
  mrb_sym sym = mrb_intern_check(mrb, name, len);
  if (sym == 0) return 0;
  checkpoint_perm *P = G->perm;
  for (uint32_t i = 0; i < P->nmt; i++) if (P->mt[i].ref != 0 && P->mt[i].sym == sym) return 1;
  return 0;
}

MRB_API int
mrb_checkpoint_missing_names(mrb_state *mrb, mrb_checkpoint_method_fn fn, void *ud)
{
  checkpoint_state *G = checkpoint_get(mrb);
  if (!G) return 0;
  const char *p = G->missing;
  for (uint32_t i = 0; fn && i < G->nmissing; i++) {
    size_t l = strlen(p);
    fn(mrb, p, l, ud);
    p += l + 1;
  }
  return (int)G->nmissing;
}
