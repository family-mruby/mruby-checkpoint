/*
** mruby/ghost.h - carry a running VM state to another mrb_state
**
** The state is written as a logical graph (format v0, see doc/format_v0.md):
** objects are numbered and refer to each other by number, symbols travel
** by name, ireps by content hash, and the objects that mrb_open() made are
** bound by name on the other side.
*/

#ifndef MRUBY_GHOST_H
#define MRUBY_GHOST_H

#include <mruby.h>
#include <mruby/data.h>

MRB_BEGIN_DECL

/* What Ghost.hop does in this state. */
enum mrb_ghost_mode {
  MRB_GHOST_MODE_HOP = 0,     /* write the state and call the hop handler */
  MRB_GHOST_MODE_IGNORE = 1,  /* Ghost.hop is a no-op (reference runs) */
};

/* Called at the safe point after a successful dump. buf/len is the whole
   ghost image; path is what Ghost.hop was given (or the override). The
   default handler writes the file and exits the process with status 0. */
typedef void (*mrb_ghost_hop_handler)(mrb_state *mrb, const uint8_t *buf, size_t len,
                                      const char *path, void *ud);

/* Record what mrb_open() (and the driver) made: the permanent objects and
   their names, and a snapshot of the permanent classes. Call after mrb_open
   and before loading the program, on both the writing and the reading side. */
MRB_API void mrb_ghost_baseline(mrb_state *mrb);

MRB_API void mrb_ghost_set_mode(mrb_state *mrb, enum mrb_ghost_mode mode);
MRB_API void mrb_ghost_set_output(mrb_state *mrb, const char *path); /* overrides Ghost.hop's path */
MRB_API void mrb_ghost_set_hop_handler(mrb_state *mrb, mrb_ghost_hop_handler fn, void *ud);

/* Restore an image into mrb (which must have had mrb_ghost_baseline and must
   not be running anything) and continue it to the end. Returns the value of
   the top-level program; on a restore error raises Ghost::Error. */
MRB_API mrb_value mrb_ghost_resume(mrb_state *mrb, const uint8_t *buf, size_t len);

/* Side table for C data (RData). There is no field in mrb_data_type for this:
   the table maps a data type pointer to a pair of callbacks and a name the
   other side uses to find the same type.
   dump: append the object's state to *out (see mrb_ghost_buf_*), return 0 on
         success or non-zero to refuse.
   restore: return the data pointer built from bytes[0..len). */
typedef struct mrb_ghost_buf mrb_ghost_buf;
typedef int (*mrb_ghost_data_dump)(mrb_state *mrb, mrb_value obj, mrb_ghost_buf *out);
typedef void *(*mrb_ghost_data_restore)(mrb_state *mrb, const uint8_t *bytes, size_t len);
MRB_API void mrb_ghost_register_data(mrb_state *mrb, const mrb_data_type *type, const char *name,
                                     mrb_ghost_data_dump dump, mrb_ghost_data_restore restore);
MRB_API void mrb_ghost_buf_bytes(mrb_state *mrb, mrb_ghost_buf *b, const void *p, size_t len);

MRB_END_DECL

#endif /* MRUBY_GHOST_H */
