# Build configuration for mruby-ghost.
#
# Two builds of the same mruby, differing only in pointer width:
#   host   -- 64-bit (the host gcc)
#   host32 -- 32-bit (gcc -m32)
#
# Value representation follows Family mruby: no boxing, 64-bit Integer,
# double Float, UTF-8 strings.
#
# GHOST_GEM=no   leaves out mrbgems/mruby-ghost (benchmark baseline)
# GHOST_TARGETS  comma separated subset of "host,host32" (default: both)
# GHOST_DEBUG=asan  -g -O0 with AddressSanitizer (for debugging only)
#             asan-stress  the same plus MRB_GC_STRESS (a full GC at every allocation)

GHOST_ROOT = File.expand_path("..", __dir__)

def ghost_common(conf)
  conf.toolchain :gcc
  conf.gembox 'default'
  conf.gem core: 'mruby-task'
  conf.gem "#{GHOST_ROOT}/mrbgems/mruby-ghost" unless ENV['GHOST_GEM'] == 'no'
  conf.defines += %w(MRB_NO_BOXING MRB_INT64 MRB_UTF8_STRING)
  conf.cc.flags << '-O2'
  if ENV['GHOST_DEBUG'].to_s.start_with?('asan')
    conf.cc.flags << '-g' << '-O0' << '-fsanitize=address'
    conf.linker.flags << '-fsanitize=address'
    conf.defines << 'MRB_GC_STRESS' if ENV['GHOST_DEBUG'] == 'asan-stress'
  end
end

targets = (ENV['GHOST_TARGETS'] || 'host,host32').split(',')

if targets.include?('host')
  MRuby::Build.new('host') do |conf|
    ghost_common(conf)
  end
end

if targets.include?('host32')
  MRuby::Build.new(targets.include?('host') ? 'host32' : 'host') do |conf|
    ghost_common(conf)
    conf.cc.flags << '-m32'
    conf.linker.flags << '-m32'
  end
end
