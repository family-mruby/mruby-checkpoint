# hal-task-virtual: a virtual clock for mruby-task, for the test builds only
# (CHECKPOINT_FUZZ=1, build/fuzz/). The gem's name makes it mruby-task's HAL
# in place of ports/posix (mruby's build: List#resolve_external_hal!).
#
# No timer and no signal: the tick advances when the scheduler has nothing
# to run (each idle turn is one tick) and when the root context sleeps (the
# ticks of the sleep), and from outside with mrb_tick() (checkpoint-fuzz
# --quantum: one tick every N instructions). The same program then sees the
# same ticks in every run, with checkpoints or without.
MRuby::Gem::Specification.new('hal-task-virtual') do |spec|
  spec.license = 'MIT'
  spec.authors = 'mruby-checkpoint developers'
  spec.summary = 'test-only: a virtual clock for mruby-task'
  spec.add_dependency 'mruby-task', core: 'mruby-task'
end
