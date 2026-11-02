/*
** ghost.c - mruby-ghost gem: Ghost.hop and the safe point hook
**
** Ghost.hop(path) only raises a request: it marks the state and sets
** mrb->task.switching, the flag the VM already tests at every instruction
** boundary for task switches. The VM then calls mrb_vm_safepoint_hook
** (patches/0001-vm-safepoint-hook.patch) at the next boundary where a task
** switch would be allowed (no C function between the root frame and the
** current one, no exception in flight). For a plain call that is the very
** next instruction, so the hop lands right after the call. Inside a block
** called from C (Array#sort!, Array.new { }, ...) the request waits until
** execution is back out of the C function.
*/

#include <stdio.h>
#include <stdlib.h>
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/string.h>
#include <mruby/data.h>
#include <mruby/variable.h>
#include "ghost_internal.h"

extern void (*mrb_vm_safepoint_hook)(mrb_state *mrb);

/* ---- per-state side table ---- */

#define GHOST_MAX_STATES 8
static ghost_state *states[GHOST_MAX_STATES];

ghost_state*
ghost_get(mrb_state *mrb)
{
  for (int i = 0; i < GHOST_MAX_STATES; i++) {
    if (states[i] && states[i]->mrb == mrb) return states[i];
  }
  return NULL;
}

static ghost_state*
ghost_new(mrb_state *mrb)
{
  for (int i = 0; i < GHOST_MAX_STATES; i++) {
    if (!states[i]) {
      ghost_state *g = (ghost_state*)calloc(1, sizeof(ghost_state));
      g->mrb = mrb;
      states[i] = g;
      return g;
    }
  }
  return NULL;
}

static void
ghost_delete(mrb_state *mrb)
{
  for (int i = 0; i < GHOST_MAX_STATES; i++) {
    if (states[i] && states[i]->mrb == mrb) {
      ghost_state *g = states[i];
      ghost_perm_free(mrb, g->perm);
      free(g->pending_path);
      free(g->output);
      for (int k = 0; k < g->ndata; k++) free(g->data[k].name);
      free(g->data);
      free(g);
      states[i] = NULL;
    }
  }
}

static char*
dupstr(const char *s)
{
  if (!s) return NULL;
  size_t n = strlen(s);
  char *d = (char*)malloc(n + 1);
  memcpy(d, s, n + 1);
  return d;
}

/* ---- C API ---- */

MRB_API void
mrb_ghost_baseline(mrb_state *mrb)
{
  ghost_state *g = ghost_get(mrb);
  if (!g) return;
  ghost_perm_free(mrb, g->perm);
  g->perm = NULL;
  g->perm = ghost_perm_build(mrb);
}

MRB_API void
mrb_ghost_set_mode(mrb_state *mrb, enum mrb_ghost_mode mode)
{
  ghost_state *g = ghost_get(mrb);
  if (g) g->mode = mode;
}

MRB_API void
mrb_ghost_set_output(mrb_state *mrb, const char *path)
{
  ghost_state *g = ghost_get(mrb);
  if (!g) return;
  free(g->output);
  g->output = dupstr(path);
}

MRB_API void
mrb_ghost_set_hop_handler(mrb_state *mrb, mrb_ghost_hop_handler fn, void *ud)
{
  ghost_state *g = ghost_get(mrb);
  if (!g) return;
  g->handler = fn;
  g->handler_ud = ud;
}

MRB_API void
mrb_ghost_register_data(mrb_state *mrb, const mrb_data_type *type, const char *name,
                        mrb_ghost_data_dump dump, mrb_ghost_data_restore restore)
{
  ghost_state *g = ghost_get(mrb);
  if (!g) return;
  g->data = (ghost_data_ent*)realloc(g->data, sizeof(ghost_data_ent) * (g->ndata + 1));
  g->data[g->ndata].type = type;
  g->data[g->ndata].name = dupstr(name);
  g->data[g->ndata].dump = dump;
  g->data[g->ndata].restore = restore;
  g->ndata++;
}

/* ---- the hop ---- */

static void
default_handler(mrb_state *mrb, const uint8_t *buf, size_t len, const char *path, void *ud)
{
  FILE *fp = fopen(path, "wb");
  if (!fp || fwrite(buf, 1, len, fp) != len || fclose(fp) != 0) {
    fprintf(stderr, "ghost: cannot write %s\n", path);
    exit(2);
  }
  fflush(stdout);
  exit(0);
}

static void
ghost_safepoint(mrb_state *mrb)
{
  ghost_state *g = ghost_get(mrb);
  if (!g || !g->pending) return;
  if (mrb->c != mrb->root_c) return;  /* wait until back on the root context */

  g->pending = 0;
  mrb->task.switching = FALSE;

  gbuf out;
  gb_init(&out);
  char err[512];
  if (ghost_dump(mrb, &out, err, sizeof(err)) != 0) {
    gb_free(mrb, &out);
    free(g->pending_path);
    g->pending_path = NULL;
    struct RClass *e = mrb_class_get_under(mrb, mrb_module_get(mrb, "Ghost"), "Error");
    mrb_raise(mrb, e, err);
  }
  const char *path = g->pending_path;
  if (g->handler) g->handler(mrb, out.p, out.len, path, g->handler_ud);
  else default_handler(mrb, out.p, out.len, path, NULL);
  /* a handler that returns lets the program go on in this process too */
  gb_free(mrb, &out);
  free(g->pending_path);
  g->pending_path = NULL;
}

static mrb_value
ghost_hop(mrb_state *mrb, mrb_value self)
{
  const char *path = NULL;
  mrb_get_args(mrb, "|z!", &path);
  ghost_state *g = ghost_get(mrb);
  struct RClass *e = mrb_class_get_under(mrb, mrb_class_ptr(self), "Error");

  if (!g) mrb_raise(mrb, e, "ghost is not initialized");
  if (g->mode == MRB_GHOST_MODE_IGNORE) return mrb_nil_value();
  if (!g->perm) mrb_raise(mrb, e, "no baseline: run the program through the ghost driver");
  if (mrb->c != mrb->root_c) mrb_raise(mrb, e, "Ghost.hop from a Fiber or Task is not supported in format v0");
  for (int i = 0; i < 4; i++) {
    if (mrb->task.queues[i]) mrb_raise(mrb, e, "Tasks exist; format v0 does not carry Tasks");
  }
  if (g->output) path = g->output;
  if (!path) mrb_raise(mrb, e, "Ghost.hop needs a path");

  free(g->pending_path);
  g->pending_path = dupstr(path);
  g->pending = 1;
  mrb->task.switching = TRUE;   /* the VM tests this at every instruction boundary */
  return mrb_nil_value();
}

static mrb_value
ghost_resumed_p(mrb_state *mrb, mrb_value self)
{
  ghost_state *g = ghost_get(mrb);
  return mrb_bool_value(g && g->resumed);
}

/* ---- Ghost::Box: a C data type carried through the side table ---- */

typedef struct { int64_t v; } ghost_box;

static void
box_free(mrb_state *mrb, void *p)
{
  mrb_free(mrb, p);
}

static const struct mrb_data_type box_type = { "Ghost::Box", box_free };

static mrb_value
box_init(mrb_state *mrb, mrb_value self)
{
  mrb_int v = 0;
  mrb_get_args(mrb, "|i", &v);
  ghost_box *b = (ghost_box*)DATA_PTR(self);
  if (b) mrb_free(mrb, b);
  b = (ghost_box*)mrb_malloc(mrb, sizeof(ghost_box));
  b->v = v;
  mrb_data_init(self, b, &box_type);
  return self;
}

static mrb_value
box_value(mrb_state *mrb, mrb_value self)
{
  ghost_box *b = (ghost_box*)mrb_data_get_ptr(mrb, self, &box_type);
  return mrb_int_value(mrb, b->v);
}

static mrb_value
box_set_value(mrb_state *mrb, mrb_value self)
{
  mrb_int v;
  mrb_get_args(mrb, "i", &v);
  ghost_box *b = (ghost_box*)mrb_data_get_ptr(mrb, self, &box_type);
  b->v = v;
  return mrb_int_value(mrb, v);
}

static int
box_dump(mrb_state *mrb, mrb_value obj, mrb_ghost_buf *out)
{
  ghost_box *b = (ghost_box*)DATA_PTR(obj);
  uint8_t x[8];
  for (int i = 0; i < 8; i++) x[i] = (uint8_t)((uint64_t)b->v >> (56 - 8*i));
  mrb_ghost_buf_bytes(mrb, out, x, 8);
  return 0;
}

static void*
box_restore(mrb_state *mrb, const uint8_t *bytes, size_t len)
{
  ghost_box *b = (ghost_box*)mrb_malloc(mrb, sizeof(ghost_box));
  uint64_t u = 0;
  for (size_t i = 0; i < 8 && i < len; i++) u = (u << 8) | bytes[i];
  b->v = (int64_t)u;
  return b;
}

/* ---- gem ---- */

void
mrb_mruby_ghost_gem_init(mrb_state *mrb)
{
  ghost_new(mrb);
  mrb_vm_safepoint_hook = ghost_safepoint;

  struct RClass *m = mrb_define_module(mrb, "Ghost");
  mrb_define_class_under(mrb, m, "Error", mrb->eStandardError_class);
  mrb_define_module_function(mrb, m, "hop", ghost_hop, MRB_ARGS_OPT(1));
  mrb_define_module_function(mrb, m, "resumed?", ghost_resumed_p, MRB_ARGS_NONE());

  struct RClass *box = mrb_define_class_under(mrb, m, "Box", mrb->object_class);
  MRB_SET_INSTANCE_TT(box, MRB_TT_CDATA);
  mrb_define_method(mrb, box, "initialize", box_init, MRB_ARGS_OPT(1));
  mrb_define_method(mrb, box, "value", box_value, MRB_ARGS_NONE());
  mrb_define_method(mrb, box, "value=", box_set_value, MRB_ARGS_REQ(1));
  mrb_ghost_register_data(mrb, &box_type, "Ghost::Box", box_dump, box_restore);
}

void
mrb_mruby_ghost_gem_final(mrb_state *mrb)
{
  ghost_delete(mrb);
}
