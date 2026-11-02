/*
** mruby/checkpoint.h - carry a running VM state to another mrb_state
**
** The state is written as a logical graph (format v0, see doc/format_v0.md):
** objects are numbered and refer to each other by number, symbols travel
** by name, ireps by content hash, and the objects that mrb_open() made are
** bound by name on the other side.
*/

#ifndef MRUBY_CHECKPOINT_H
#define MRUBY_CHECKPOINT_H

#include <mruby.h>
#include <mruby/data.h>

MRB_BEGIN_DECL

/* What Checkpoint.save / Checkpoint.suspend do in this state. */
enum mrb_checkpoint_mode {
  MRB_CHECKPOINT_MODE_WRITE = 0,   /* write the state and call the handler */
  MRB_CHECKPOINT_MODE_IGNORE = 1,  /* write nothing (reference runs): save returns
                                      :saved, suspend returns :resumed */
};

/* Which call asked for the checkpoint. */
enum mrb_checkpoint_kind {
  MRB_CHECKPOINT_SAVE = 0,     /* Checkpoint.save: the program goes on */
  MRB_CHECKPOINT_SUSPEND = 1,  /* Checkpoint.suspend: the process ends */
};

/* Called at the safe point after a successful dump. buf/len is the whole
   checkpoint image; path is what save / suspend was given (or the override).
   The default handler writes the file; for SUSPEND it then exits the process
   with status 0, for SAVE it returns and the program goes on. A handler that
   returns from a SUSPEND lets the program go on in this process too.
   In the image, the value of the save / suspend call is :resumed; the
   writing side sees :saved. */
typedef void (*mrb_checkpoint_handler)(mrb_state *mrb, const uint8_t *buf, size_t len,
                                       const char *path, enum mrb_checkpoint_kind kind,
                                       void *ud);

/* Record what mrb_open() (and the driver) made: the permanent objects and
   their names, and a snapshot of the permanent classes. Call after mrb_open
   and before loading the program, on both the writing and the reading side. */
MRB_API void mrb_checkpoint_baseline(mrb_state *mrb);

MRB_API void mrb_checkpoint_set_mode(mrb_state *mrb, enum mrb_checkpoint_mode mode);
MRB_API void mrb_checkpoint_set_output(mrb_state *mrb, const char *path); /* overrides the path given to save / suspend */
MRB_API void mrb_checkpoint_set_handler(mrb_state *mrb, mrb_checkpoint_handler fn, void *ud);

/* Restore an image into mrb (which must have had mrb_checkpoint_baseline and must
   not be running anything) and continue it to the end. Returns the value of
   the top-level program; on a restore error raises Checkpoint::Error. */
MRB_API mrb_value mrb_checkpoint_resume(mrb_state *mrb, const uint8_t *buf, size_t len);

/* Side table for C data (RData). There is no field in mrb_data_type for this:
   the table maps a data type pointer to a pair of callbacks and a name the
   other side uses to find the same type.
   dump: append the object's state to *out (see mrb_checkpoint_buf_*), return 0 on
         success or non-zero to refuse.
   restore: return the data pointer built from bytes[0..len). */
typedef struct mrb_checkpoint_buf mrb_checkpoint_buf;
typedef int (*mrb_checkpoint_data_dump)(mrb_state *mrb, mrb_value obj, mrb_checkpoint_buf *out);
typedef void *(*mrb_checkpoint_data_restore)(mrb_state *mrb, const uint8_t *bytes, size_t len);
MRB_API void mrb_checkpoint_register_data(mrb_state *mrb, const mrb_data_type *type, const char *name,
                                     mrb_checkpoint_data_dump dump, mrb_checkpoint_data_restore restore);
MRB_API void mrb_checkpoint_buf_bytes(mrb_state *mrb, mrb_checkpoint_buf *b, const void *p, size_t len);

MRB_END_DECL

#endif /* MRUBY_CHECKPOINT_H */
