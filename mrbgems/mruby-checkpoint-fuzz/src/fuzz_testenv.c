/*
** fuzz_testenv.c - the environment mrbtest gives a test file
**
** mrbtest's own helpers (t_print, _str_match?, the Mrbtest module and the
** vformat helpers) come from mruby-test's sources, included here without
** their main(). The gems' C test helpers are in t_*.c.
*/

#include <mruby.h>
#include <mruby/variable.h>
#include <mruby/string.h>

#define main checkpoint_fuzz_unused_mrbtest_main
#include "mruby-test/driver.c"
#undef main
#include "mruby-test/vformat.c"

/* referenced only by the main() above */
const uint8_t mrbtest_assert_irep[1] = {0};
void mrbgemtest_init(mrb_state *mrb) { (void)mrb; }

void mrb_mruby_binding_gem_test(mrb_state *mrb);
void mrb_mruby_dir_gem_test(mrb_state *mrb);
void mrb_mruby_error_gem_test(mrb_state *mrb);
void mrb_mruby_fiber_gem_test(mrb_state *mrb);
void mrb_mruby_io_gem_test(mrb_state *mrb);
void mrb_mruby_object_ext_gem_test(mrb_state *mrb);
void mrb_mruby_proc_binding_gem_test(mrb_state *mrb);
void mrb_mruby_proc_ext_gem_test(mrb_state *mrb);
void mrb_mruby_socket_gem_test(mrb_state *mrb);
void mrb_mruby_task_gem_test(mrb_state *mrb);
void checkpoint_fuzz_proc_ext_funcs(mrb_state *mrb);

/* what mrbtest does before loading a test file: the driver's helpers
   (verbose: the name of every test is printed), GEMNAME, the gem's C
   helpers. All the gems' helpers are installed for every file, so that the
   writing and the reading side have the same baseline whatever the file. */
void
checkpoint_fuzz_test_env(mrb_state *mrb, const char *gemname)
{
  int ai = mrb_gc_arena_save(mrb);
  mrb_const_set(mrb, mrb_obj_value(mrb->object_class), mrb_intern_lit(mrb, "GEMNAME"),
                mrb_str_new_cstr(mrb, gemname));
  mrb_init_test_driver(mrb, TRUE);
  mrb_mruby_binding_gem_test(mrb);
  mrb_mruby_dir_gem_test(mrb);
  mrb_mruby_error_gem_test(mrb);
  mrb_mruby_fiber_gem_test(mrb);
  mrb_mruby_io_gem_test(mrb);
  mrb_mruby_object_ext_gem_test(mrb);
  mrb_mruby_proc_binding_gem_test(mrb);
  mrb_mruby_proc_ext_gem_test(mrb);
  mrb_mruby_socket_gem_test(mrb);
  mrb_mruby_task_gem_test(mrb);
  checkpoint_fuzz_proc_ext_funcs(mrb);
  mrb_gc_arena_restore(mrb, ai);
}

void mrb_mruby_checkpoint_fuzz_gem_init(mrb_state *mrb) { (void)mrb; }
void mrb_mruby_checkpoint_fuzz_gem_final(mrb_state *mrb) { (void)mrb; }
