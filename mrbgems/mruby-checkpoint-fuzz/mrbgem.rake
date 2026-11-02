# mruby-checkpoint-fuzz: the test-only external trigger (doc/report/p3.md).
#
# Built only into the fuzz builds (CHECKPOINT_FUZZ=1, build/fuzz/), never into
# the normal ones. It brings:
#   - checkpoint-fuzz, a driver that runs an mruby test file (mrbtest's
#     assert.rb + the file) and stops it from outside at pseudo-random
#     instruction boundaries (a seed and a count of fetched instructions),
#     writes the image, restores it into a fresh mrb_state (or another
#     process, or the 32-bit build), drops the old state and goes on;
#   - the environment mrbtest gives the tests (t_print, _str_match?,
#     Mrbtest, the vformat helpers and the gems' C test helpers).
MRuby::Gem::Specification.new('mruby-checkpoint-fuzz') do |spec|
  spec.license = 'MIT'
  spec.authors = 'mruby-checkpoint developers'
  spec.summary = 'test-only: stop the mruby test suite at random safe points'
  spec.bins = %w(checkpoint-fuzz)
  spec.add_dependency 'mruby-checkpoint'
  %w(mruby-binding mruby-dir mruby-error mruby-fiber mruby-io mruby-object-ext
     mruby-proc-binding mruby-proc-ext mruby-socket mruby-task).each do |g|
    spec.add_dependency g, core: g
  end
  spec.cc.include_paths << "#{MRUBY_ROOT}/mrbgems"
  spec.cc.include_paths << "#{MRUBY_ROOT}/mrbgems/mruby-dir/include"
  spec.cc.include_paths << "#{MRUBY_ROOT}/mrbgems/mruby-io/include"
  spec.cc.defines << 'MRBTEST_COMPILER_PRISM'
end
