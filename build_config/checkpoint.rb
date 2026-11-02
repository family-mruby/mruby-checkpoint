# Build configuration for mruby-checkpoint.
#
# Two builds of the same mruby, differing only in pointer width:
#   host   -- 64-bit (the host gcc)
#   host32 -- 32-bit (gcc -m32)
#
# Value representation follows Family mruby: no boxing, 64-bit Integer,
# double Float, UTF-8 strings.
#
# CHECKPOINT_GEM=no   leaves out mrbgems/mruby-checkpoint (benchmark baseline)
# CHECKPOINT_TARGETS  comma separated subset of "host,host32" (default: both)
# CHECKPOINT_DEBUG=asan  -g -O0 with AddressSanitizer (for debugging only)
#             asan-stress  the same plus MRB_GC_STRESS (a full GC at every allocation)
# CHECKPOINT_FUZZ=1   the test builds (build/fuzz): adds mrbgems/mruby-checkpoint-fuzz,
#                     the external trigger (checkpoint-fuzz) that stops the test
#                     suite at random safe points. Never in the normal builds.

CHECKPOINT_ROOT = File.expand_path("..", __dir__)

def checkpoint_common(conf)
  conf.toolchain :gcc
  conf.gembox 'default'
  conf.gem core: 'mruby-task'
  conf.gem "#{CHECKPOINT_ROOT}/mrbgems/mruby-checkpoint" unless ENV['CHECKPOINT_GEM'] == 'no'
  if ENV['CHECKPOINT_FUZZ'] == '1'
    conf.gem "#{CHECKPOINT_ROOT}/mrbgems/mruby-checkpoint-fuzz"
    conf.defines << 'MRB_USE_DEBUG_HOOK'   # the per-instruction hook the trigger counts with
  end
  conf.defines += %w(MRB_NO_BOXING MRB_INT64 MRB_UTF8_STRING)
  conf.cc.flags << '-O2'
  if ENV['CHECKPOINT_DEBUG'].to_s.start_with?('asan')
    conf.cc.flags << '-g' << '-O0' << '-fsanitize=address'
    conf.linker.flags << '-fsanitize=address'
    conf.defines << 'MRB_GC_STRESS' if ENV['CHECKPOINT_DEBUG'] == 'asan-stress'
  end
end

targets = (ENV['CHECKPOINT_TARGETS'] || 'host,host32').split(',')

if targets.include?('host')
  MRuby::Build.new('host') do |conf|
    checkpoint_common(conf)
  end
end

if targets.include?('host32')
  MRuby::Build.new(targets.include?('host') ? 'host32' : 'host') do |conf|
    checkpoint_common(conf)
    conf.cc.flags << '-m32'
    conf.linker.flags << '-m32'
  end
end
