/*
** mruby/checkpoint.h - carry a running VM state to another mrb_state
**
** The state is written as a logical graph (format v1, see doc/format_v1.md):
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
  MRB_CHECKPOINT_EXTERNAL = 2, /* mrb_checkpoint_request: asked from outside the program */
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

/* The names of the baseline (doc/format_v0.md section 8), in the order they
   were given: fn is called once per name; is_func is 1 for a C function
   (F:...), 0 for an object. A reader of another implementation builds its
   table of names from this list. Call after mrb_checkpoint_baseline. */
typedef void (*mrb_checkpoint_name_fn)(const char *name, int is_func, void *ud);
MRB_API void mrb_checkpoint_each_name(mrb_state *mrb, mrb_checkpoint_name_fn fn, void *ud);

/* Name a C function the program's C code makes Procs or methods of at run
   time (mrb_proc_new_cfunc_with_env and the like): an image can then refer
   to it by "F:<name>". Call before mrb_checkpoint_baseline, the same on
   both sides. */
MRB_API void mrb_checkpoint_register_func(mrb_state *mrb, const char *name, mrb_func_t func);

MRB_API void mrb_checkpoint_set_mode(mrb_state *mrb, enum mrb_checkpoint_mode mode);
MRB_API void mrb_checkpoint_set_output(mrb_state *mrb, const char *path); /* overrides the path given to save / suspend */
MRB_API void mrb_checkpoint_set_handler(mrb_state *mrb, mrb_checkpoint_handler fn, void *ud);

/* Ask for a checkpoint from outside the program (no Checkpoint.save call):
   from a hook, a timer or another thread's flag. Like save, it only marks
   the state and sets mrb->task.switching; the image is written at the next
   instruction boundary that is a safe point, and handed to the handler with
   kind MRB_CHECKPOINT_EXTERNAL (path is the output set by
   mrb_checkpoint_set_output, may be NULL). Nothing in the program sees it:
   no register is changed. Returns MRB_CHECKPOINT_REQ_OK, or why the request
   cannot be taken where the VM stands now (the program goes on untouched).
   A request taken here can still be refused at the safe point (a value of a
   type that cannot be carried, a Fiber entered before the boundary...): the
   refusal handler gets the reason; nothing is raised. */
enum mrb_checkpoint_req {
  MRB_CHECKPOINT_REQ_OK = 0,
  MRB_CHECKPOINT_REQ_NOT_READY,   /* no baseline, or not initialized */
  MRB_CHECKPOINT_REQ_FIBER,       /* (not returned since P4: Fibers and Tasks are carried) */
  MRB_CHECKPOINT_REQ_C_BOUNDARY,  /* inside a block called from C (or a Fiber resumed from C) */
  MRB_CHECKPOINT_REQ_TASKS,       /* (not returned since P4) */
  MRB_CHECKPOINT_REQ_PENDING,     /* a checkpoint is already pending */
  MRB_CHECKPOINT_REQ_EXCEPTION,   /* (not returned since P3: the request waits until it is caught) */
};
MRB_API enum mrb_checkpoint_req mrb_checkpoint_request(mrb_state *mrb);
/* The same, from a place where the VM stands at an instruction boundary with
   every frame in memory (the per-instruction hook, code_fetch_hook): the
   image is taken right here when this boundary is a safe point, else at the
   first safe point after (an exception in flight). Nothing runs between the
   request and the image, so the scheduler's state at the point is exact. */
MRB_API enum mrb_checkpoint_req mrb_checkpoint_request_here(mrb_state *mrb);
typedef void (*mrb_checkpoint_refusal_handler)(mrb_state *mrb, const char *reason, void *ud);
MRB_API void mrb_checkpoint_set_refusal_handler(mrb_state *mrb, mrb_checkpoint_refusal_handler fn, void *ud);

/* Dead registers (a temporary the code never reads again) are written as nil
   (the liveness pass, checkpoint_live.c). keep = TRUE writes them as they are. */
MRB_API void mrb_checkpoint_keep_dead_registers(mrb_state *mrb, mrb_bool keep);
/* How many non-nil registers the last image wrote as nil. */
MRB_API uint32_t mrb_checkpoint_last_dead(mrb_state *mrb);

/* Restore an image into mrb (which must have had mrb_checkpoint_baseline and must
   not be running anything, nor have any Task) and continue it to the end.
   Returns the value of the top-level program (nil when the host ran the
   scheduler, see below); on a restore error raises Checkpoint::Error. */
MRB_API mrb_value mrb_checkpoint_resume(mrb_state *mrb, const uint8_t *buf, size_t len);
/* The two halves of mrb_checkpoint_resume: build the state (raises
   Checkpoint::Error on a restore error; no Ruby code of the program runs
   except #hash / #eql? of restored Hash keys), then run it to the end.
   mrb_checkpoint_continue starts again what was running: the root context,
   a Fiber, or a Task (the rest of its slice, then the scheduler: the loop of
   Task.run or the step of Task.pass the program was in, or, for a host that
   ran mrb_task_run from C, mrb_task_run itself, which returns when every
   Task is done; continue then returns nil). */
MRB_API void mrb_checkpoint_restore(mrb_state *mrb, const uint8_t *buf, size_t len);
MRB_API mrb_value mrb_checkpoint_continue(mrb_state *mrb);

/* The embedder's Task (the one it made with mrb_create_task to run the
   program, as Family mruby runs an application): the image keeps which one
   it is, so that the restoring side finds it again (its value, its end).
   mrb_checkpoint_host_task returns it after mrb_checkpoint_restore, or nil. */
/* The Task that was running goes on with a full timeslice (the default), or
   with what was left of it in the image (keep = TRUE; an experiment: the
   writer's ticks to the end of the slice, as if the run had not stopped).
   Set on the restoring side. */
MRB_API void mrb_checkpoint_keep_timeslice(mrb_state *mrb, mrb_bool keep);

MRB_API void mrb_checkpoint_set_host_task(mrb_state *mrb, mrb_value task);
MRB_API mrb_value mrb_checkpoint_host_task(mrb_state *mrb);

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
/* For C data whose struct holds Ruby values that the object also keeps in
   its ivars (as mruby-regexp's MatchData does, for the GC): restore_obj is
   called once every object of the image is built and filled, with the
   object (ivars set, data pointer NULL); it returns the data pointer, or
   NULL with a reason in why. */
typedef void *(*mrb_checkpoint_data_restore_obj)(mrb_state *mrb, mrb_value obj, const uint8_t *bytes,
                                                 size_t len, char *why, size_t whylen);
MRB_API void mrb_checkpoint_register_data_obj(mrb_state *mrb, const mrb_data_type *type, const char *name,
                                              mrb_checkpoint_data_dump dump, mrb_checkpoint_data_restore_obj restore);

MRB_END_DECL

#endif /* MRUBY_CHECKPOINT_H */
