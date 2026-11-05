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
#                     suite at random safe points, and mrbgems/hal-task-virtual,
#                     a virtual clock for mruby-task in place of the POSIX timer.
#                     Never in the normal builds.
# CHECKPOINT_TABLE=file.c  the driver links that fixed table (rake build:table)
# CHECKPOINT_VIRTUAL_CLOCK=1  only the virtual clock (mrbgems/hal-task-virtual), for
#                     measuring Task programs with instruction counts (a timer
#                     makes them depend on time)

CHECKPOINT_ROOT = File.expand_path("..", __dir__)

# mrblib (mruby's and the gems') is compiled with line numbers (mrbc -g), so
# that a frame running mrblib code has a file and a line, in the image too
# (section DBG0 of doc/format_v1.md). The file names are relative to the
# source tree (mrblib/array.rb, mrbgems/mruby-task/mrblib/task.rb): mrbc
# names a file as it is given, and runs here from the tree's root. The
# options of a build are kept; only the directory mrbc runs in changes (a
# spawn option, not Dir.chdir: rake builds in threads).
module CheckpointMrbcRelativeNames
  def run(out, infiles, funcname, cdump: true, static: false)
    infiles = [infiles].flatten.map { |f| File.expand_path(f) }
    root = [MRUBY_ROOT, CHECKPOINT_ROOT].map { |r| File.expand_path(r) }
                                        .find { |r| infiles.all? { |f| f.start_with?("#{r}/") } }
    return super unless root
    infiles.each_with_index { |f, i| _pp i == 0 ? "MRBC" : "", f.relative_path, indent: 2 }
    rel = infiles.map { |f| f.delete_prefix("#{root}/") }
    opt = @compile_options % {funcname: funcname}
    opt << " -S" if cdump
    opt << " -s" if static
    tmpout = File.expand_path("#{out.path}.#{funcname}.mrbcout")
    opt = opt.sub(/\s-o-(?=\s|\z)/, %Q[ -o "#{tmpout}"])
    cmd = %["#{File.expand_path(@command || @build.mrbcfile)}" #{opt} #{rel.map { |f| %["#{f}"] }.join(' ')}]
    puts cmd if Rake.verbose
    unless system(cmd, chdir: root)
      rm_f tmpout
      rm_f out.path
      fail "Command failed with status (#{$?.exitstatus}): [#{cmd[0, 42]}...]"
    end
    out.write File.binread(tmpout)
    rm_f tmpout
  end
end
MRuby::Command::Mrbc.prepend(CheckpointMrbcRelativeNames)

def checkpoint_common(conf)
  conf.toolchain :gcc
  conf.gembox 'default'
  conf.gem core: 'mruby-task'
  conf.gem "#{CHECKPOINT_ROOT}/mrbgems/mruby-checkpoint" unless ENV['CHECKPOINT_GEM'] == 'no'
  if ENV['CHECKPOINT_FUZZ'] == '1' || ENV['CHECKPOINT_VIRTUAL_CLOCK'] == '1'
    conf.gem "#{CHECKPOINT_ROOT}/mrbgems/hal-task-virtual"   # mruby-task's HAL: the virtual clock
  end
  if ENV['CHECKPOINT_FUZZ'] == '1'
    conf.gem "#{CHECKPOINT_ROOT}/mrbgems/mruby-checkpoint-fuzz"
    conf.defines << 'MRB_USE_DEBUG_HOOK'   # the per-instruction hook the trigger counts with
    conf.defines << 'CHECKPOINT_TEST_HELPERS'   # Checkpoint.__pass_and_save / __pass_and_suspend
  end
  if ENV['CHECKPOINT_TABLE']
    # the driver links a fixed table (rake build:table): the C source that
    # `checkpoint table checkpoint_driver_table` printed
    conf.defines << %Q[CHECKPOINT_DRIVER_TABLE=\\"#{File.expand_path(ENV['CHECKPOINT_TABLE'])}\\"]
  end
  conf.mrbc.compile_options += ' -g'   # mrblib with line numbers (above)
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
