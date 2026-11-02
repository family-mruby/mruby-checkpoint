# Points aimed at on purpose (doc/report/p4b.md): test/targeted/*.rb run by
# checkpoint-fuzz with the request raised right before every call of a
# method, compared with the run without checkpoints.
#
#   ruby tools/targeted.rb
#
# task_pass.rb: the request right before every Task.pass, so that the pass is
# the one instruction between the request and its safe point.
# ci_grow.rb: images written at every depth of calls while the writer calls a
# method of the program's objects, so that its call moves the call-info stack.
# Each runs as the top-level program and as a Task, in this process and in
# other processes (64 -> 32 -> 64 ...). Every point must write an image and
# every run must print what the run without checkpoints prints.
#
# CHECKPOINT_FUZZ64 replaces the 64-bit build (build/asan-fuzz: tools/asan.sh fuzz).
#
# Exit status 1 when a run differs or a point wrote no image.

require 'open3'
require 'tmpdir'

ROOT = File.expand_path('..', __dir__)
FUZZ64 = ENV['CHECKPOINT_FUZZ64'] ? File.expand_path(ENV['CHECKPOINT_FUZZ64']) : File.join(ROOT, 'build/fuzz/host/bin/checkpoint-fuzz')
FUZZ32 = File.join(ROOT, 'build/fuzz/host32/bin/checkpoint-fuzz')
ASSERT = File.join(ROOT, 'vendor/mruby/test/assert.rb')
[FUZZ64, FUZZ32].each { |b| abort "missing #{b} (rake build:fuzz)" unless File.exist?(b) }

TARGETS = [['task_pass.rb', 'pass'], ['ci_grow.rb', 'mark']]

def normalize(out)
  out.lines.reject { |l| l.start_with?('   Time: ') }.join
end

failed = 0
TARGETS.each do |file, meth|
  path = File.join(ROOT, 'test/targeted', file)
  [[], ['--as-task']].each do |shape|
    [0, 1].each do |xproc|
      Dir.mktmpdir('cktarget') do |dir|
        common = ['--tmp', dir, *shape]
        ref, = Open3.capture2e(FUZZ64, '--ref', *common, '--assert', ASSERT, path)
        log = File.join(dir, 'run.log')
        args = [*common, '--at-send', meth, '--log', log]
        args += ['--xproc', '1', '--peer', FUZZ32, '--peer', FUZZ64] if xproc > 0
        out, st = Open3.capture2e(FUZZ64, *args, '--assert', ASSERT, path)
        lines = File.exist?(log) ? File.readlines(log) : []
        points = lines.count { |l| l.start_with?('P ') }
        written = lines.count { |l| l =~ /\AP \d+ \d+ ok / }
        same = normalize(out) == normalize(ref) && st.success? && !out.include?('AddressSanitizer')
        ok = same && points > 0 && written == points && ref.include?('OK: 1')
        name = "#{file} #{shape.empty? ? 'top-level' : 'as-task'} #{xproc > 0 ? 'xproc' : 'inproc'}"
        puts "#{ok ? 'ok  ' : 'FAIL'} #{name}: #{written}/#{points} points written, " \
             "#{same ? 'same output' : 'output differs'}"
        unless ok
          failed += 1
          puts out.lines.first(20).map { |l| "    #{l}" }.join
          puts lines.reject { |l| l =~ /\AP \d+ \d+ ok / }.first(10).map { |l| "    #{l}" }.join
        end
      end
    end
  end
end
puts failed.zero? ? 'targeted: all passed' : "targeted: #{failed} failed"
exit(failed.zero? ? 0 : 1)
