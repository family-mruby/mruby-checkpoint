/*
** task_hal_virtual.c - mruby-task HAL with a virtual clock (test builds only)
**
** The tick is not time: it moves on when the scheduler is idle (one tick
** per idle turn, mrb_hal_task_idle_cpu) and when the root context sleeps
** (the sleep's ticks at once, mrb_hal_task_sleep_us). A driver may also
** call mrb_tick() itself (once every N instructions). No interrupt exists,
** so the exclusion is empty.
*/

#include <mruby.h>
#include "task_hal.h"

void
mrb_hal_task_init(mrb_state *mrb)
{
  for (int i = 0; i < 4; i++) mrb->task.queues[i] = NULL;
  mrb->task.tick = 0;
  mrb->task.wakeup_tick = UINT32_MAX;
  mrb->task.switching = FALSE;
}

void
mrb_hal_task_final(mrb_state *mrb)
{
  (void)mrb;
}

void
mrb_task_enable_irq(void)
{
}

void
mrb_task_disable_irq(void)
{
}

void
mrb_hal_task_idle_cpu(mrb_state *mrb)
{
  mrb_tick(mrb);
}

void
mrb_hal_task_sleep_us(mrb_state *mrb, mrb_int usec)
{
  /* what a real sleep of usec would see: one tick per MRB_TICK_UNIT ms */
  mrb_int ticks = usec / (MRB_TICK_UNIT * 1000);
  for (mrb_int i = 0; i < ticks; i++) mrb_tick(mrb);
}

void
mrb_hal_task_virtual_gem_init(mrb_state *mrb)
{
  (void)mrb;
}

void
mrb_hal_task_virtual_gem_final(mrb_state *mrb)
{
  (void)mrb;
}
