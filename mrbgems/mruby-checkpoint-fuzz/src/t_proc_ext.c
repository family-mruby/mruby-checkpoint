/* the C test helpers of mruby-proc-ext (what mrbtest links for its test) */
#include "mruby-proc-ext/test/proc.c"

/* the C functions that test makes Procs of at run time */
#include <mruby/checkpoint.h>
void
checkpoint_fuzz_proc_ext_funcs(mrb_state *mrb)
{
  mrb_checkpoint_register_func(mrb, "test:mruby-proc-ext#return_func_name", return_func_name);
  mrb_checkpoint_register_func(mrb, "test:mruby-proc-ext#return_env", return_env);
}
