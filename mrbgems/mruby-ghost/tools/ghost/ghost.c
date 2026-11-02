/*
** ghost - run a Ruby program that may hop, or resume a ghost image
**
**   ghost run [--no-hop] [--inproc] [--out FILE] prog.rb
**       Runs prog.rb. Ghost.hop(path) writes the state to path (or FILE)
**       and ends the process (status 0).
**       --no-hop  Ghost.hop does nothing (the reference run)
**       --inproc  on a hop, restore the image into a new mrb_state in this
**                 same process and continue there
**   ghost resume [--out FILE] image
**       Restores image in a new process and continues from the hop.
**
** Exit status: 0 normal end or hop, 1 uncaught exception, 2 usage / I/O,
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
#include <mruby/ghost.h>

static int
usage(void)
{
  fprintf(stderr,
          "usage: ghost run [--no-hop] [--inproc] [--out FILE] prog.rb\n"
          "       ghost resume [--out FILE] image\n");
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
  return mrb_ghost_resume(mrb, im->buf, im->len);
}

static const char *out_override = NULL;
static int inproc = 0;
static void inproc_handler(mrb_state *mrb, const uint8_t *buf, size_t len, const char *path, void *ud);

/* what both sides have before the baseline */
static mrb_state*
open_state(void)
{
  mrb_state *mrb = mrb_open();
  if (!mrb) { fprintf(stderr, "ghost: mrb_open failed\n"); exit(2); }
  mrb_define_global_const(mrb, "ARGV", mrb_ary_new(mrb));
  mrb_ghost_baseline(mrb);
  return mrb;
}

/* restore into a fresh state and run to the end; never returns */
static void
resume_and_exit(const uint8_t *buf, size_t len)
{
  mrb_state *mrb = open_state();
  if (out_override) mrb_ghost_set_output(mrb, out_override);
  if (inproc) mrb_ghost_set_hop_handler(mrb, inproc_handler, NULL);
  image im = { buf, len };
  mrb_bool error;
  mrb_value r = mrb_protect_error(mrb, resume_body, &im, &error);
  int status = 0;
  if (error) {
    mrb->exc = mrb_obj_ptr(r);
    if (mrb_obj_is_kind_of(mrb, r, mrb_class_get_under(mrb, mrb_module_get(mrb, "Ghost"), "Error"))) {
      fprintf(stderr, "ghost: ");
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
  fflush(stdout);
  mrb_close(mrb);
  exit(status);
}

static void
inproc_handler(mrb_state *mrb, const uint8_t *buf, size_t len, const char *path, void *ud)
{
  (void)mrb; (void)path; (void)ud;
  uint8_t *copy = (uint8_t*)malloc(len);
  memcpy(copy, buf, len);
  fflush(stdout);
  /* the old state is left as it is: nothing returns to it */
  resume_and_exit(copy, len);
}

int
main(int argc, char **argv)
{
  int no_hop = 0;
  const char *file = NULL;

  if (argc < 2) return usage();
  const char *cmd = argv[1];
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--no-hop") == 0) no_hop = 1;
    else if (strcmp(argv[i], "--inproc") == 0) inproc = 1;
    else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_override = argv[++i];
    else if (argv[i][0] == '-') return usage();
    else if (!file) file = argv[i];
    else return usage();
  }
  if (!file) return usage();

  if (strcmp(cmd, "resume") == 0) {
    size_t len;
    uint8_t *buf = read_file(file, &len);
    if (!buf) { fprintf(stderr, "ghost: cannot read %s\n", file); return 2; }
    resume_and_exit(buf, len);
  }
  if (strcmp(cmd, "run") != 0) return usage();

  FILE *fp = fopen(file, "r");
  if (!fp) { fprintf(stderr, "ghost: cannot open %s\n", file); return 2; }
  mrb_state *mrb = open_state();
  if (no_hop) mrb_ghost_set_mode(mrb, MRB_GHOST_MODE_IGNORE);
  if (out_override) mrb_ghost_set_output(mrb, out_override);
  if (inproc) mrb_ghost_set_hop_handler(mrb, inproc_handler, NULL);

  mrb_ccontext *c = mrb_ccontext_new(mrb);
  mrb_ccontext_filename(mrb, c, file);
  mrb_load_file_cxt(mrb, fp, c);
  fclose(fp);
  int status = 0;
  if (mrb->exc) {
    mrb_print_error(mrb);
    status = 1;
  }
  mrb_ccontext_free(mrb, c);
  fflush(stdout);
  mrb_close(mrb);
  return status;
}
