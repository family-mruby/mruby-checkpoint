# Write checkpoint images of test/samples for SabiRuby's own tests (its tests/checkpoint/).
#
#   ruby tools/sabiruby_fixtures.rb OUTDIR [sample ...]
#
# For each sample that suspends, every image along its chain of suspends (the chain goes
# on in C mruby), written by the 64-bit build (and the first one also by the 32-bit
# build, NAME.1-32.ckpt):
#   NAME.K.ckpt      the image
#   NAME.K.out       what the program prints after it, to its end (from the reference run)
#   NAME.K.refused   instead of .out: the text SabiRuby's refusal must contain
#                    (the sample's `sabiruby-refuse K "TEXT"` annotation)
# For a save sample: NAME.1.ckpt and NAME.1.out (= NAME.resumed.out).
# NAME.rb is copied too, so that the images can be made again from it.
# The samples of Tasks and Fibers (task_*, fiber_*), counter, lines and lines_mrblib are also run as a
# Task (checkpoint run --as-task, the start of a Family mruby application): NAME-task.K.ckpt
# and so on. A sample marked `clock virtual` is run with the test builds (build/fuzz), whose
# mruby-task clock is virtual.

require 'open3'
require 'tmpdir'
require 'fileutils'

ROOT = File.expand_path('..', __dir__)
NORMAL = { '64' => File.join(ROOT, 'build/mruby/host/bin/checkpoint'), '32' => File.join(ROOT, 'build/mruby/host32/bin/checkpoint') }
VIRTUAL = { '64' => File.join(ROOT, 'build/fuzz/host/bin/checkpoint'), '32' => File.join(ROOT, 'build/fuzz/host32/bin/checkpoint') }
AS_TASK = /\A(task_|fiber_|counter\z|lines\z|lines_mrblib\z)/
out_dir = ARGV.shift or abort 'usage: sabiruby_fixtures.rb OUTDIR [sample ...]'
out_dir = File.expand_path(out_dir)
FileUtils.mkdir_p(out_dir)
samples = ARGV.empty? ? Dir[File.join(ROOT, 'test/samples/*.rb')].sort : ARGV.map { |a| File.expand_path(a) }

def run(arch, *args, dir:)
  args = [args[0], '--as-task', *args[1..]] if $as_task && args[0] == 'run'
  out, _err, _st = Open3.capture3($bins.fetch(arch), *args, chdir: dir)
  out
end

samples.product([false, true]).each do |path, as_task|
  base = File.basename(path, '.rb')
  src = File.read(path)
  ann = src.lines.grep(/^# checkpoint-test:/).map { |l| l.sub(/^# checkpoint-test:\s*/, '').strip }
  next if ann.include?('refuse')
  next if as_task && (base !~ AS_TASK || ann.include?('no-as-task') || ann.any? { |a| a.start_with?('sabiruby-skip-as-task') })
  $as_task = as_task
  $bins = ann.include?('clock virtual') ? VIRTUAL : NORMAL
  name = as_task ? "#{base}-task" : base
  refused = ann.filter_map { |a| (m = a.match(/\Asabiruby-refuse (\d+) "(.*)"\z/)) && [m[1].to_i, m[2]] }.to_h
  Dir.mktmpdir('checkpoint-fixtures') do |dir|
    FileUtils.cp(path, dir)
    FileUtils.cp(path, out_dir)
    file = File.basename(path)
    img = File.join(dir, 'img.ckpt')
    ref = run('64', 'run', '--ignore', file, dir: dir)
    prefix = run('64', 'run', '--out', img, file, dir: dir)
    if ann.any? { |a| a.start_with?('save-after') }
      FileUtils.cp(img, File.join(out_dir, "#{name}.1.ckpt"))
      FileUtils.cp(path.sub(/\.rb\z/, '.resumed.out'), File.join(out_dir, "#{name}.1.out"))
      next
    end
    k = 0
    while File.exist?(img)
      k += 1
      FileUtils.mv(img, img + '.in')
      FileUtils.cp(img + '.in', File.join(out_dir, "#{name}.#{k}.ckpt"))
      if refused[k]
        File.write(File.join(out_dir, "#{name}.#{k}.refused"), refused[k] + "\n")
      else
        abort "#{name}: image #{k}: the output so far is not a prefix of the reference" unless ref.start_with?(prefix)
        File.write(File.join(out_dir, "#{name}.#{k}.out"), ref[prefix.size..])
      end
      prefix += run('64', 'resume', '--out', img, img + '.in', dir: dir)
    end
    # the same first image from the 32-bit build
    FileUtils.rm_f(img)
    run('32', 'run', '--out', img, file, dir: dir)
    if File.exist?(img)
      FileUtils.cp(img, File.join(out_dir, "#{name}.1-32.ckpt"))
      %w(out refused).each do |ext|
        f = File.join(out_dir, "#{name}.1.#{ext}")
        FileUtils.cp(f, File.join(out_dir, "#{name}.1-32.#{ext}")) if File.exist?(f)
      end
    end
    puts "#{name}: #{k} image#{k == 1 ? '' : 's'}"
  end
end
