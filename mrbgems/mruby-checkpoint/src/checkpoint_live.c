/*
** checkpoint_live.c - which registers are still live (decided at write time)
**
** A frame's register window keeps the values of earlier instructions until
** they are overwritten. A value that the rest of the code never reads again
** is dead: carrying it costs space, and it may point at something the reader
** cannot bind (an object of a class the other VM lacks, a hidden internal
** object). The writer computes, from the bytecode, the registers that are
** live where each frame stands, and writes the others as nil.
**
** Only the temporaries are judged. These are always kept:
**   - R0 (self) and the argument registers up to the block slot (ci->n,
**     ci->nk): the method's own arguments, read by super without arguments,
**     block_given?, Proc#call (OP_CALL) and the keyword operations;
**   - the local variables (R0 .. nlocals-1): an Env, binding, eval or
**     local_variables can read them by name without an instruction here;
**   - every register of a frame that has not run its first instruction
**     (pc 0: OP_ENTER reads the arguments as the caller passed them).
** A stack slot is written when any frame whose window covers it keeps it
** (the callee's R0 is the caller's R[a], and so on).
**
** The analysis is a backward data flow over the instructions, with the
** jumps, the exception handlers (an instruction inside a handler's range
** may continue at its target) and the jumps that go through ensure clauses
** (OP_RAISEIF may resume any OP_JMPUW target). Where an instruction's use
** of registers is not known exactly it counts as reading all of them, and a
** write counts only where it is certain, so the result errs toward live.
** It runs only while writing an image; the VM is not involved.
*/

#include <mruby.h>
#include <mruby/irep.h>
#include <mruby/proc.h>
#include <mruby/opcode.h>
#include <mruby/internal.h>
#include "checkpoint_internal.h"

static uint32_t
ch_u32(const uint8_t *b)
{
  return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

typedef uint32_t lword;
#define LBITS 32

typedef struct {
  uint32_t pos, end;        /* byte range of the instruction (with its EXT prefix) */
  uint8_t op;
  uint32_t a, b, c;
} linsn;

typedef struct {
  const mrb_irep *irep;
  uint32_t ninsn;
  linsn *insn;
  int words;                /* lwords per set */
  lword *in;                /* live-in set of each instruction */
  int ok;                   /* 0: could not decode; keep everything */
} lirep;

static void
set_bit(lword *s, int words, uint32_t r)
{
  if (r < (uint32_t)words * LBITS) s[r / LBITS] |= (lword)1 << (r % LBITS);
}

static void
set_range(lword *s, int words, uint32_t from, uint32_t to)  /* [from, to] */
{
  for (uint32_t r = from; r <= to && r < (uint32_t)words * LBITS; r++) set_bit(s, words, r);
}

static int
get_bit(const lword *s, int words, uint32_t r)
{
  if (r >= (uint32_t)words * LBITS) return 1;
  return (s[r / LBITS] >> (r % LBITS)) & 1;
}

/* operand layout of each instruction (mruby/ops.h) */
enum { F_Z, F_B, F_BB, F_BBB, F_BS, F_BSS, F_S, F_W };
static const uint8_t op_fmt[] = {
#define OPCODE(i, x) F_ ## x,
#include <mruby/ops.h>
#undef OPCODE
};

static int
decode(const mrb_irep *irep, uint32_t pos, linsn *in)
{
  const mrb_code *p = irep->iseq + pos, *end = irep->iseq + irep->ilen;
  int ext = 0;
  in->pos = pos;
  if (p >= end) return 0;
  uint8_t op = *p++;
  if (op == OP_EXT1 || op == OP_EXT2 || op == OP_EXT3) {
    ext = op == OP_EXT1 ? 1 : op == OP_EXT2 ? 2 : 3;
    if (p >= end) return 0;
    op = *p++;
  }
  if (op >= sizeof(op_fmt)) return 0;
  int sa = 1, sb = 1;
  switch (op_fmt[op]) {
  case F_B: case F_BB: case F_BBB:
    if (ext == 1 || ext == 3) sa = 2;
    if ((ext == 2 || ext == 3) && op_fmt[op] != F_B) sb = 2;
    break;
  case F_BS: case F_BSS:
    if (ext == 1 || ext == 3) sa = 2;
    break;
  default:
    break;
  }
#define RD(n, v) do { if (p + (n) > end) return 0; \
    v = (n) == 1 ? p[0] : (n) == 2 ? (uint32_t)(p[0] << 8 | p[1]) : (uint32_t)(p[0] << 16 | p[1] << 8 | p[2]); \
    p += (n); } while (0)
  in->a = in->b = in->c = 0;
  switch (op_fmt[op]) {
  case F_Z: break;
  case F_B: RD(sa, in->a); break;
  case F_BB: RD(sa, in->a); RD(sb, in->b); break;
  case F_BBB: RD(sa, in->a); RD(sb, in->b); RD(1, in->c); break;
  case F_BS: RD(sa, in->a); RD(2, in->b); break;
  case F_BSS: RD(sa, in->a); RD(2, in->b); RD(2, in->c); break;
  case F_S: RD(2, in->a); break;
  case F_W: RD(3, in->a); break;
  }
#undef RD
  in->op = op;
  in->end = (uint32_t)(p - irep->iseq);
  return 1;
}

static int
find_insn(const lirep *L, uint32_t pos)
{
  uint32_t lo = 0, hi = L->ninsn;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (L->insn[mid].pos < pos) lo = mid + 1;
    else hi = mid;
  }
  return (lo < L->ninsn && L->insn[lo].pos == pos) ? (int)lo : -1;
}

/* the registers a call reads: receiver, arguments, keywords and (B forms) the block */
static void
use_call(lword *u, int w, uint32_t a, uint32_t c, int with_block)
{
  uint32_t n = c & 0xf, nk = (c >> 4) & 0xf;
  uint32_t slots = (n == 15 ? 1 : n) + (nk == 15 ? 1 : 2 * nk);
  if (slots > 0) set_range(u, w, a + 1, a + slots);
  if (with_block) set_bit(u, w, a + slots + 1);
}

/* uses (u) and certain writes (d) of one instruction; returns 0 when it reads everything */
static int
use_def(const linsn *in, lword *u, lword *d, int w)
{
  uint32_t a = in->a, b = in->b, c = in->c;
  switch (in->op) {
  case OP_NOP: case OP_JMP: case OP_JMPUW: case OP_ALIAS: case OP_UNDEF:
  case OP_DEBUG: case OP_ERR: case OP_STOP: case OP_RETNIL: case OP_RETTRUE:
  case OP_RETFALSE:
    return 1;
  case OP_MOVE: set_bit(u, w, b); set_bit(d, w, a); return 1;
  case OP_LOADL: case OP_LOADI8: case OP_LOADINEG: case OP_LOADI__1: case OP_LOADI_0:
  case OP_LOADI_1: case OP_LOADI_2: case OP_LOADI_3: case OP_LOADI_4: case OP_LOADI_5:
  case OP_LOADI_6: case OP_LOADI_7: case OP_LOADI16: case OP_LOADI32: case OP_LOADSYM:
  case OP_LOADNIL: case OP_LOADTRUE: case OP_LOADFALSE:
  case OP_GETGV: case OP_GETSV: case OP_GETIV: case OP_GETCV: case OP_GETCONST:
  case OP_GETUPVAR: case OP_EXCEPT: case OP_SYMBOL: case OP_STRING:
  case OP_LAMBDA: case OP_BLOCK: case OP_METHOD: case OP_OCLASS: case OP_TCLASS:
  case OP_TDEF:
    set_bit(d, w, a); return 1;
  case OP_LOADSELF: set_bit(u, w, 0); set_bit(d, w, a); return 1;
  case OP_SETGV: case OP_SETSV: case OP_SETIV: case OP_SETCV: case OP_SETCONST:
  case OP_SETUPVAR: case OP_JMPIF: case OP_JMPNOT: case OP_JMPNIL:
  case OP_RAISEIF: case OP_MATCHERR: case OP_RETURN: case OP_RETURN_BLK: case OP_BREAK:
  case OP_ARYSPLAT: case OP_INTERN: case OP_GETMCNST: case OP_MODULE: case OP_EXEC:
  case OP_SCLASS: case OP_ADDI: case OP_SUBI: case OP_SDEF:
    set_bit(u, w, a);
    if (in->op == OP_ARYSPLAT || in->op == OP_INTERN || in->op == OP_GETMCNST ||
        in->op == OP_MODULE || in->op == OP_EXEC || in->op == OP_SCLASS ||
        in->op == OP_ADDI || in->op == OP_SUBI || in->op == OP_SDEF) set_bit(d, w, a);
    return 1;
  case OP_RETSELF: set_bit(u, w, 0); return 1;
  case OP_SETMCNST: case OP_ARYCAT: case OP_STRCAT:
    set_range(u, w, a, a + 1); return 1;
  case OP_GETIDX: case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_EQ:
  case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_HASHCAT: case OP_RANGE_INC:
  case OP_RANGE_EXC: case OP_CLASS: case OP_DEF:
    set_range(u, w, a, a + 1); set_bit(d, w, a); return 1;
  case OP_GETIDX0: set_bit(u, w, b); set_bit(d, w, a); return 1;
  case OP_SETIDX: set_range(u, w, a, a + 2); return 1;
  case OP_RESCUE: set_bit(u, w, a); set_bit(u, w, b); return 1;
  case OP_ADDILV: case OP_SUBILV:
    set_bit(u, w, a); set_range(u, w, b, b + 1); set_bit(d, w, a); return 1;
  case OP_ARRAY: set_range(u, w, a, a + b); set_bit(d, w, a); return 1;
  case OP_ARRAY2: set_range(u, w, b, b + c); set_bit(d, w, a); return 1;
  case OP_ARYPUSH: set_range(u, w, a, a + b); return 1;
  case OP_AREF: set_bit(u, w, b); set_bit(d, w, a); return 1;
  case OP_ASET: set_bit(u, w, a); set_bit(u, w, b); return 1;
  case OP_APOST: set_bit(u, w, a); set_bit(d, w, a); return 1;
  case OP_HASH: if (b > 0) set_range(u, w, a, a + 2 * b - 1); set_bit(d, w, a); return 1;
  case OP_HASHADD: set_range(u, w, a, a + 2 * b); return 1;
  case OP_SEND: use_call(u, w, a, c, 0); set_bit(u, w, a); set_bit(d, w, a); return 1;
  case OP_SENDB: use_call(u, w, a, c, 1); set_bit(u, w, a); set_bit(d, w, a); return 1;
  case OP_SEND0: set_bit(u, w, a); set_bit(d, w, a); return 1;
  case OP_SSEND: use_call(u, w, a, c, 0); set_bit(u, w, 0); set_bit(d, w, a); return 1;
  case OP_SSENDB: use_call(u, w, a, c, 1); set_bit(u, w, 0); set_bit(d, w, a); return 1;
  case OP_SSEND0: set_bit(u, w, 0); set_bit(d, w, a); return 1;
  case OP_SUPER: use_call(u, w, a, b, 1); set_bit(u, w, 0); set_bit(d, w, a); return 1;
  case OP_BLKCALL: set_range(u, w, a, a + b + 1); set_bit(d, w, a); return 1;
  default:
    /* OP_CALL, OP_ENTER, OP_ARGARY, OP_BLKPUSH, OP_KARG, OP_KEY_P, OP_KEYEND and
       anything unknown: read every register */
    return 0;
  }
}

static int
is_terminal(uint8_t op)
{
  switch (op) {
  case OP_JMP: case OP_JMPUW: case OP_RETURN: case OP_RETURN_BLK: case OP_BREAK:
  case OP_RETSELF: case OP_RETNIL: case OP_RETTRUE: case OP_RETFALSE: case OP_ERR:
  case OP_STOP:
    return 1;
  default:
    return 0;
  }
}

static int
jump_target(const linsn *in, uint32_t *t)
{
  switch (in->op) {
  case OP_JMP: case OP_JMPUW: *t = in->end + (int16_t)in->a; return 1;
  case OP_JMPIF: case OP_JMPNOT: case OP_JMPNIL: *t = in->end + (int16_t)in->b; return 1;
  default: return 0;
  }
}

static void
lirep_free(mrb_state *mrb, lirep *L)
{
  checkpoint_state *G = checkpoint_get(mrb);   /* work areas (ck_*) */
  ck_free(G, L->insn);
  ck_free(G, L->in);
  L->insn = NULL; L->in = NULL;
}

/* positions where a raise at byte position e (the end of the faulting
   instruction, as ci->pc is then) continues: catch_handler_find() matches
   begin < e <= end */
#define COVERS(h, e) (ch_u32((h)->begin) < (e) && \
                      (e) <= ch_u32((h)->end))

static void
analyze(mrb_state *mrb, const mrb_irep *irep, lirep *L)
{
  checkpoint_state *G = checkpoint_get(mrb);   /* work areas (ck_*) */
  memset(L, 0, sizeof(*L));
  L->irep = irep;
  uint32_t nr = irep->nregs + 1;
  L->words = (int)((nr + LBITS - 1) / LBITS);
  int w = L->words;

  /* decode */
  uint32_t cap = 64;
  L->insn = (linsn*)ck_malloc(G, sizeof(linsn) * cap);
  for (uint32_t pos = 0; pos < irep->ilen; ) {
    if (L->ninsn == cap) {
      cap *= 2;
      L->insn = (linsn*)ck_realloc(G, L->insn, sizeof(linsn) * cap);
    }
    if (!decode(irep, pos, &L->insn[L->ninsn])) { L->ok = 0; return; }
    pos = L->insn[L->ninsn].end;
    L->ninsn++;
  }
  uint32_t n = L->ninsn;
  const struct mrb_irep_catch_handler *ch = mrb_irep_catch_handler_table(irep);

  /* successors are found on the fly; use / def per instruction */
  lword *use = (lword*)ck_calloc(G, (size_t)n * w + 1, sizeof(lword));
  lword *def = (lword*)ck_calloc(G, (size_t)n * w + 1, sizeof(lword));
  uint8_t *all = (uint8_t*)ck_calloc(G, n + 1, 1);
  int32_t *jt = (int32_t*)ck_malloc(G, sizeof(int32_t) * (n + 1));
  int has_raiseif = 0;
  for (uint32_t i = 0; i < n; i++) {
    linsn *in = &L->insn[i];
    if (!use_def(in, use + (size_t)i * w, def + (size_t)i * w, w)) all[i] = 1;
    uint32_t t;
    jt[i] = -1;
    if (jump_target(in, &t)) {
      int k = find_insn(L, t);
      if (k < 0) { L->ok = 0; goto out; }
      jt[i] = k;
    }
    if (in->op == OP_RAISEIF) has_raiseif = 1;
  }
  int32_t *ht = (int32_t*)ck_malloc(G, sizeof(int32_t) * (irep->clen + 1));
  for (int h = 0; h < irep->clen; h++) {
    ht[h] = find_insn(L, ch_u32(ch[h].target));
    if (ht[h] < 0) { ck_free(G, ht); L->ok = 0; goto out; }
  }

  L->in = (lword*)ck_calloc(G, (size_t)n * w + 1, sizeof(lword));
  lword *tmp = (lword*)ck_malloc(G, sizeof(lword) * w);
  int changed = 1;
  while (changed) {
    changed = 0;
    for (uint32_t ii = n; ii-- > 0; ) {
      linsn *in = &L->insn[ii];
      memset(tmp, 0, sizeof(lword) * w);
      /* live-out: fall through, jump target, handlers, ensure resumption */
      if (!is_terminal(in->op) && ii + 1 < n) {
        for (int k = 0; k < w; k++) tmp[k] |= L->in[(size_t)(ii + 1) * w + k];
      }
      if (jt[ii] >= 0) {
        for (int k = 0; k < w; k++) tmp[k] |= L->in[(size_t)jt[ii] * w + k];
      }
      for (int h = 0; h < irep->clen; h++) {
        if (COVERS(&ch[h], in->end)) {
          for (int k = 0; k < w; k++) tmp[k] |= L->in[(size_t)ht[h] * w + k];
        }
      }
      if (in->op == OP_RAISEIF && has_raiseif) {
        for (uint32_t j = 0; j < n; j++) {
          if (L->insn[j].op == OP_JMPUW && jt[j] >= 0) {
            for (int k = 0; k < w; k++) tmp[k] |= L->in[(size_t)jt[j] * w + k];
          }
        }
        for (int h = 0; h < irep->clen; h++) {
          for (int k = 0; k < w; k++) tmp[k] |= L->in[(size_t)ht[h] * w + k];
        }
      }
      /* live-in = use | (out & ~def), or everything */
      lword *li = L->in + (size_t)ii * w;
      for (int k = 0; k < w; k++) {
        lword v = all[ii] ? ~(lword)0 :
          (use[(size_t)ii * w + k] | (tmp[k] & ~def[(size_t)ii * w + k]));
        if ((v | li[k]) != li[k]) { li[k] |= v; changed = 1; }
      }
    }
  }
  ck_free(G, tmp);
  ck_free(G, ht);
  L->ok = 1;
out:
  ck_free(G, use);
  ck_free(G, def);
  ck_free(G, all);
  ck_free(G, jt);
}

/* the registers live where a frame stands (pc = the next instruction for
   the top frame, the instruction after the call for the others); NULL = all */
static const lword*
live_at(mrb_state *mrb, lirep *L, uint32_t pc, lword *buf)
{
  if (!L->ok) return NULL;
  int k = find_insn(L, pc);
  if (k < 0) return NULL;
  int w = L->words;
  memcpy(buf, L->in + (size_t)k * w, sizeof(lword) * w);
  /* an exception from the callee continues at a handler covering pc */
  const struct mrb_irep_catch_handler *ch = mrb_irep_catch_handler_table(L->irep);
  for (int h = 0; h < L->irep->clen; h++) {
    if (COVERS(&ch[h], pc)) {
      int t = find_insn(L, ch_u32(ch[h].target));
      if (t < 0) return NULL;
      for (int j = 0; j < w; j++) buf[j] |= L->in[(size_t)t * w + j];
    }
  }
  return buf;
}

void
checkpoint_live_slots(mrb_state *mrb, struct mrb_context *c, size_t nslots, uint8_t *keep)
{
  checkpoint_state *G = checkpoint_get(mrb);   /* work areas (ck_*) */
  memset(keep, 0, nslots);
  uint8_t *covered = (uint8_t*)ck_calloc(G, nslots + 1, 1);
  lirep *cache = NULL; int ncache = 0, capcache = 0;

  for (mrb_callinfo *ci = c->cibase; ci <= c->ci; ci++) {
    size_t base = (size_t)(ci->stack - c->stbase);
    const struct RProc *p = ci->proc;
    if (!p || MRB_PROC_CFUNC_P(p) || !p->body.irep) {
      /* not expected at a safe point: keep the rest of the stack */
      for (size_t s = base; s < nslots; s++) { keep[s] = 1; covered[s] = 1; }
      continue;
    }
    const mrb_irep *irep = p->body.irep;
    size_t top = base + (size_t)irep->nregs;
    if (ci == c->ci) top = nslots;          /* the top frame's window runs to the end */
    if (top > nslots) top = nslots;
    uint32_t pc = ci->pc ? (uint32_t)(ci->pc - irep->iseq) : 0;

    /* the analysis of this irep, computed once per image */
    lirep *L = NULL;
    for (int i = 0; i < ncache; i++) if (cache[i].irep == irep) { L = &cache[i]; break; }
    if (!L) {
      if (ncache == capcache) {
        capcache = capcache ? capcache * 2 : 8;
        cache = (lirep*)ck_realloc(G, cache, sizeof(lirep) * capcache);
      }
      L = &cache[ncache++];
      analyze(mrb, irep, L);
    }
    lword buf[16];
    lword *heapbuf = NULL;
    lword *b = buf;
    if ((size_t)L->words > sizeof(buf) / sizeof(buf[0])) {
      heapbuf = (lword*)ck_malloc(G, sizeof(lword) * L->words);
      b = heapbuf;
    }
    const lword *live = (ci->pc == NULL || pc == 0) ? NULL : live_at(mrb, L, pc, b);

    size_t fixed = (size_t)irep->nlocals;
    mrb_int bidx = mrb_ci_bidx(ci);
    if (bidx >= 0 && (size_t)bidx + 1 > fixed) fixed = (size_t)bidx + 1;
    struct REnv *e = mrb_vm_ci_env(ci);
    if (e && (size_t)MRB_ENV_LEN(e) > fixed) fixed = (size_t)MRB_ENV_LEN(e);

    for (size_t s = base; s < top; s++) {
      size_t r = s - base;
      covered[s] = 1;
      if (keep[s]) continue;
      if (r < fixed || !live || r >= (size_t)irep->nregs || get_bit(live, L->words, (uint32_t)r)) keep[s] = 1;
    }
    ck_free(G, heapbuf);
  }
  for (size_t s = 0; s < nslots; s++) if (!covered[s]) keep[s] = 1;

  for (int i = 0; i < ncache; i++) lirep_free(mrb, &cache[i]);
  ck_free(G, cache);
  ck_free(G, covered);
}

/* The instruction that ends at byte pos (a caller's pc right after a call):
   its start, opcode and operands. 0 if no instruction ends there. */
int
checkpoint_insn_ending_at(const mrb_irep *irep, uint32_t pos, uint32_t *start, uint8_t *op,
                          uint32_t *a, uint32_t *b, uint32_t *c)
{
  uint32_t p = 0;
  linsn in;
  while (p < pos) {
    if (!decode(irep, p, &in)) return 0;
    if (in.end == pos) {
      *start = in.pos; *op = in.op; *a = in.a; *b = in.b; *c = in.c;
      return 1;
    }
    p = in.end;
  }
  return 0;
}
