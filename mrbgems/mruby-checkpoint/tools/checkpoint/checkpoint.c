/*
** checkpoint - run a Ruby program that may save or suspend, or resume an image
**
**   checkpoint run [--ignore] [--inproc] [--as-task] [--with-source] [--out FILE] prog.rb
**       Runs prog.rb. Checkpoint.save(path) writes the state to path (or
**       FILE) and goes on; Checkpoint.suspend(path) writes it and ends the
**       process (status 0).
**       --ignore  save / suspend write nothing (the reference run)
**       --inproc  restore each image into a new mrb_state in this same
**                 process: after a suspend the program goes on there; after
**                 a save the restored program runs to its end first, then
**                 the saving program goes on
**       --as-task the start of a Family mruby application: the program is
**                 compiled, made a Task (mrb_create_task, named after the
**                 file) and the scheduler runs (mrb_task_run) until every
**                 Task is done. An exception the program does not rescue
**                 becomes the Task's value; it is printed as the error.
**       --with-source  images carry the program's source (section SRC0, for
**                 display) unless save / suspend say source: false. Without
**                 it, only a call with source: true writes it. The driver
**                 registers prog.rb's text either way.
**   checkpoint resume [--inproc] [--with-source] [--out FILE] image
**       Restores image in a new process and continues from the line after
**       the save / suspend.
**   checkpoint names
**       Prints the names of the baseline (the permanent objects and C
**       functions an image refers to by name), one per line, in the order
**       they were given; C functions are prefixed by "F:" already.
**
** Exit status: 0 normal end or suspend, 1 uncaught exception, 2 usage / I/O,
** 3 the image could not be restored.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mruby.h>
#include <mruby/compile.h>
#include <mruby/error.h>
#include <mruby/array.h>
#include <mruby/variable.h>
#include <mruby/checkpoint.h>
#include <mruby/proc.h>

#include <mruby/gc.h>
#include "task.h"

/* mruby-task with mruby-fiber (f56d44ecc): Fiber.current inside a Task (as
   Enumerator#next does) makes a Fiber whose context is the one embedded in
   the Task's struct, and mrb_close frees a Fiber's context, that is, an
   address inside the Task (free(): invalid pointer at the end of such a run,
   with checkpoints or without). Such a Fiber is let go of its context
   before the state is closed. */
static int
detach_cb(mrb_state *mrb, struct RBasic *obj, void *ud)
{
  (void)ud;
  if (obj->tt != MRB_TT_FIBER) return MRB_EACH_OBJ_OK;
  struct RFiber *f = (struct RFiber*)obj;
  for (int q = 0; f->cxt && q < 4; q++) {
    for (mrb_task *t = mrb->task.queues[q]; t; t = t->next) {
      if (f->cxt == &t->c) { f->cxt = NULL; break; }
    }
  }
  return MRB_EACH_OBJ_OK;
}

static void
close_state(mrb_state *mrb)
{
  mrb_objspace_each_objects(mrb, detach_cb, NULL);
  mrb_close(mrb);
}

static void
print_name(const char *name, int is_func, void *ud)
{
  (void)is_func; (void)ud;
  puts(name);
}

static int
usage(void)
{
  fprintf(stderr,
          "usage: checkpoint run [--ignore] [--inproc] [--as-task] [--with-source] [--out FILE] prog.rb\n"
          "       checkpoint resume [--inproc] [--with-source] [--out FILE] image\n"
          "       checkpoint names\n");
  return 2;
}

static uint8_t*
read_file(const char *path, size_t *len)
{
  FILE *fp = fopen(path, "rb");
  if (!fp) return NULL;
  fseek(fp, 0, SEEK_END);
  long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  uint8_t *buf = (uint8_t*)malloc(n > 0 ? (size_t)n : 1);
  if (fread(buf, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(buf); return NULL; }
  fclose(fp);
  *len = (size_t)n;
  return buf;
}

typedef struct { const uint8_t *buf; size_t len; } image;

static mrb_value
resume_body(mrb_state *mrb, void *ud)
{
  image *im = (image*)ud;
  return mrb_checkpoint_resume(mrb, im->buf, im->len);
}

static const char *out_override = NULL;
static int inproc = 0;
static int with_source = 0;
static void inproc_handler(mrb_state *mrb, const uint8_t *buf, size_t len, const char *path,
                           enum mrb_checkpoint_kind kind, void *ud);

/* what both sides have before the baseline */
static mrb_state*
open_state(void)
{
  mrb_state *mrb = mrb_open();
  if (!mrb) { fprintf(stderr, "checkpoint: mrb_open failed\n"); exit(2); }
  mrb_define_global_const(mrb, "ARGV", mrb_ary_new(mrb));
  mrb_checkpoint_baseline(mrb);
  if (with_source) mrb_checkpoint_set_include_source(mrb, TRUE);
  return mrb;
}

/* the host's Task ended with an exception: print it as the error */
static int
host_task_status(mrb_state *mrb)
{
  mrb_value t = mrb_checkpoint_host_task(mrb);
  if (mrb_nil_p(t)) return 0;
  mrb_value v = mrb_funcall(mrb, t, "value", 0);
  if (!mrb_obj_is_kind_of(mrb, v, mrb->eException_class)) return 0;
  mrb->exc = mrb_obj_ptr(v);
  mrb_print_error(mrb);
  mrb->exc = NULL;
  return 1;
}

/* restore into a fresh state and run to the end; returns the exit status */
static int
resume_run(const uint8_t *buf, size_t len)
{
  mrb_state *mrb = open_state();
  if (out_override) mrb_checkpoint_set_output(mrb, out_override);
  if (inproc) mrb_checkpoint_set_handler(mrb, inproc_handler, NULL);
  image im = { buf, len };
  mrb_bool error;
  mrb_value r = mrb_protect_error(mrb, resume_body, &im, &error);
  int status = 0;
  if (error) {
    mrb->exc = mrb_obj_ptr(r);
    if (mrb_obj_is_kind_of(mrb, r, mrb_class_get_under(mrb, mrb_module_get(mrb, "Checkpoint"), "Error"))) {
      fprintf(stderr, "checkpoint: ");
      mrb_print_error(mrb);
      status = 3;
    }
    else {
      mrb_print_error(mrb);
      status = 1;
    }
  }
  else if (mrb->exc) {
    mrb_print_error(mrb);
    status = 1;
  }
  else {
    status = host_task_status(mrb);
  }
  fflush(stdout);
  close_state(mrb);
  return status;
}

static void
inproc_handler(mrb_state *mrb, const uint8_t *buf, size_t len, const char *path,
               enum mrb_checkpoint_kind kind, void *ud)
{
  (void)mrb; (void)path; (void)ud;
  fflush(stdout);
  if (kind == MRB_CHECKPOINT_SAVE) {
    /* the restored program runs to its end, then the saving one goes on */
    int status = resume_run(buf, len);
    if (status != 0) exit(status);
    return;
  }
  uint8_t *copy = (uint8_t*)malloc(len);
  memcpy(copy, buf, len);
  /* the old state is left as it is: nothing returns to it */
  int status = resume_run(copy, len);
  free(copy);
  exit(status);
}

int
main(int argc, char **argv)
{
  int ignore = 0, as_task = 0;
  const char *file = NULL;

  if (argc < 2) return usage();
  const char *cmd = argv[1];
  if (strcmp(cmd, "names") == 0) {
    if (argc != 2) return usage();
    mrb_state *mrb = open_state();
    mrb_checkpoint_each_name(mrb, print_name, NULL);
    close_state(mrb);
    return 0;
  }
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--ignore") == 0) ignore = 1;
    else if (strcmp(argv[i], "--as-task") == 0) as_task = 1;
    else if (strcmp(argv[i], "--inproc") == 0) inproc = 1;
    else if (strcmp(argv[i], "--with-source") == 0) with_source = 1;
    else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_override = argv[++i];
    else if (argv[i][0] == '-') return usage();
    else if (!file) file = argv[i];
    else return usage();
  }
  if (!file) return usage();

  if (strcmp(cmd, "resume") == 0) {
    size_t len;
    uint8_t *buf = read_file(file, &len);
    if (!buf) { fprintf(stderr, "checkpoint: cannot read %s\n", file); return 2; }
    if (ignore || as_task) return usage();
    int status = resume_run(buf, len);
    free(buf);
    return status;
  }
  if (strcmp(cmd, "run") != 0) return usage();

  FILE *fp = fopen(file, "r");
  if (!fp) { fprintf(stderr, "checkpoint: cannot open %s\n", file); return 2; }
  mrb_state *mrb = open_state();
  if (ignore) mrb_checkpoint_set_mode(mrb, MRB_CHECKPOINT_MODE_IGNORE);
  if (out_override) mrb_checkpoint_set_output(mrb, out_override);
  if (inproc) mrb_checkpoint_set_handler(mrb, inproc_handler, NULL);
  {
    /* the text of the program, under the name the compiler gives its code */
    size_t slen;
    uint8_t *text = read_file(file, &slen);
    if (text) mrb_checkpoint_add_source(mrb, file, (const char*)text, slen);
    free(text);
  }

  mrb_ccontext *c = mrb_ccontext_new(mrb);
  mrb_ccontext_filename(mrb, c, file);
  int status = 0;
  if (as_task) {
    /* compiled only; the scheduler runs it as a Task */
    c->no_exec = TRUE;
    mrb_value proc = mrb_load_file_cxt(mrb, fp, c);
    fclose(fp);
    if (mrb->exc) {
      mrb_print_error(mrb);
      status = 1;
    }
    else {
      const char *base = strrchr(file, '/');
      mrb_value task = mrb_create_task(mrb, mrb_proc_ptr(proc), mrb_str_new_cstr(mrb, base ? base + 1 : file),
                                       mrb_nil_value(), mrb_nil_value());
      mrb_checkpoint_set_host_task(mrb, task);
      mrb_task_run(mrb);
      if (mrb->exc) {
        mrb_print_error(mrb);
        status = 1;
      }
      else status = host_task_status(mrb);
    }
  }
  else {
    mrb_load_file_cxt(mrb, fp, c);
    fclose(fp);
    if (mrb->exc) {
      mrb_print_error(mrb);
      status = 1;
    }
  }
  mrb_ccontext_free(mrb, c);
  fflush(stdout);
  close_state(mrb);
  return status;
}
