# A part of the test suite resumed on SabiRuby (doc/report/p3.md): C mruby
# runs a test file and is stopped from outside at a random safe point
# (checkpoint-fuzz --at N --suspend IMG: the image is written and the process
# ends); SabiRuby resumes the image (sabiruby -r test/sabiruby_prelude.rb
# --resume IMG). What C printed before the point + what SabiRuby printed must
# equal the reference run of C mruby.
#
#   ruby tools/fuzz_sabiruby.rb [--seed S] [--points K] [--only PATTERN] [--keep-dead]
#                               [--gems G1,G2,...] [--as-task]
#
# --gems adds the test files of those gems (mruby-fiber, mruby-task, ...) to the core
# ones; --as-task runs C mruby's side as a Task (checkpoint-fuzz --as-task: the image is
# of a host that runs the scheduler), compared with C mruby's reference run of that shape.
#
# Only the files SabiRuby itself runs from their start to the same output as
# C mruby take part (others differ for reasons that are not the checkpoint's;
# they are listed). Needs SABIRUBY_BIN or SABIRUBY_DIR (target/release/sabiruby,
# built with the feature checkpoint: rake sabiruby).

require 'open3'
require 'tmpdir'

ROOT = File.expand_path('..', __dir__)
FUZZ = File.join(ROOT, 'build/fuzz/host/bin/checkpoint-fuzz')
MRUBY = File.join(ROOT, 'vendor/mruby')
ASSERT = File.join(MRUBY, 'test/assert.rb')
PRELUDE = File.join(ROOT, 'test/sabiruby_prelude.rb')
SABI = ENV['SABIRUBY_BIN'] || (ENV['SABIRUBY_DIR'] && File.join(ENV['SABIRUBY_DIR'], 'target/release/sabiruby'))
abort 'set SABIRUBY_BIN or SABIRUBY_DIR' unless SABI && File.exist?(SABI)
abort "missing #{FUZZ} (rake build:fuzz)" unless File.exist?(FUZZ)

seed = 1; points = 20; only = nil; keep_dead = false; gems = []; as_task = false
args = ARGV.dup
until args.empty?
  case (a = args.shift)
  when '--seed' then seed = args.shift.to_i
  when '--points' then points = args.shift.to_i
  when '--only' then only = Regexp.new(args.shift)
  when '--keep-dead' then keep_dead = true
  when '--gems' then gems = args.shift.split(',')
  when '--as-task' then as_task = true
  else abort "unknown option #{a}"
  end
end

def normalize(out)
  out.lines.reject { |l| l.start_with?('   Time: ') }.join
end

# test/fuzz_exclude.txt: the assertions whose result may legitimately change (tools/fuzz.rb);
# here every entry counts (SabiRuby is another process, another VM) but the ones for the Task
# shape only, unless --as-task, and "sabiruby" names the ones for another VM only
EXCLUDE = File.join(ROOT, 'test/fuzz_exclude.txt')
def exclusions(as_task)
  File.readlines(EXCLUDE).each_with_object(Hash.new { |h, k| h[k] = [] }) do |l, h|
    next if l.strip.empty? || l.start_with?('#')
    file, assertion, w, = l.chomp.split(' | ', 4)
    next if w == 'as-task' && !as_task
    h[file] << assertion
  end
end

def excluded?(list, ref, out)
  la = normalize(ref).lines; lb = normalize(out).lines
  i = (0...[la.size, lb.size].max).find { |k| la[k] != lb[k] }
  return false unless i
  line = la[i] || lb[i]
  list.any? { |a| line.start_with?(a) || line =~ /\A\w+(::\w+)*: #{Regexp.escape(a)}/ }
end

def cap(*cmd, chdir:)
  o, e, s = Open3.capture3(*cmd, chdir: chdir)
  [o, e, s.exitstatus]
end

files = Dir[File.join(MRUBY, 'test/t/*.rb')].sort.map { |f| ['mruby-test', f] }
gems.each { |g| Dir[File.join(MRUBY, 'mrbgems', g, 'test', '*.rb')].sort.each { |f| files << [g, f] } }
files.select! { |_, f| only =~ File.basename(f) } if only
shape = as_task ? ['--as-task'] : []
rng = Random.new(seed)
excl = exclusions(as_task)
total = Hash.new(0)
skipped = []
rows = []

Dir.mktmpdir('cksabi') do |dir|
  files.each do |gem, file|
    name = gem == 'mruby-test' ? "core/#{File.basename(file)}" : "#{gem}/#{File.basename(file)}"
    log = File.join(dir, 'ref.log')
    File.delete(log) if File.exist?(log)
    ref, _, st = cap(FUZZ, '--ref', *shape, '--gem', gem, '--log', log, '--assert', ASSERT, file, chdir: dir)
    insns = File.read(log)[/^E (\d+)/, 1].to_i
    # SabiRuby from the start: the same program, the same prelude (as the top-level program:
    # it has no start as a Task)
    prog = File.join(dir, 'prog.rb')
    File.write(prog, "GEMNAME = #{gem.inspect}\n" + File.read(ASSERT) + "\n" + File.read(file) + "\nreport\n")
    sref, = cap(SABI, '-r', PRELUDE, prog, chdir: dir)
    unless normalize(sref) == normalize(ref)
      la = normalize(ref).lines; lb = normalize(sref).lines
      i = (0...[la.size, lb.size].max).find { |k| la[k] != lb[k] }
      skipped << "#{name}: SabiRuby from the start differs at line #{i + 1}: #{(lb[i] || '(end)').strip[0, 70]}"
      next
    end
    c = Hash.new(0)
    reasons = Hash.new(0)
    points.times do
      at = 1 + rng.rand(insns)
      img = File.join(dir, 'x.ckpt')
      File.delete(img) if File.exist?(img)
      plog = File.join(dir, 'p.log')
      File.delete(plog) if File.exist?(plog)
      a = [FUZZ, '--at', at.to_s, '--suspend', img, '--log', plog, *shape, '--gem', gem]
      a << '--keep-dead' if keep_dead
      pre, = cap(*a, '--assert', ASSERT, file, chdir: dir)
      c[:tried] += 1
      unless File.exist?(img)
        l = File.exist?(plog) ? File.read(plog).lines.find { |x| x.start_with?('P ') } : nil
        c[:c_refused] += 1
        reasons["C: #{l ? l.split(' ', 5)[3..].join(' ').strip[0, 60] : 'no request'}"] += 1
        next
      end
      c[:written] += 1
      post, err, sst = cap(SABI, '-r', PRELUDE, '--resume', img, chdir: dir)
      if sst != 0 && err.include?('restore failed')
        c[:s_refused] += 1
        reasons["SabiRuby: #{err[/restore failed: (.*?)( \(Checkpoint::Error\))?$/, 1].to_s[0, 70]}"] += 1
        next
      end
      if normalize(pre + post) == normalize(ref)
        c[:same] += 1
      elsif excluded?(excl[name], ref, pre + post)
        c[:expected] += 1
        reasons["expected (test/fuzz_exclude.txt) at #{at}"] += 1
      else
        c[:differ] += 1
        la = normalize(ref).lines; lb = normalize(pre + post).lines
        i = (0...[la.size, lb.size].max).find { |k| la[k] != lb[k] }
        reasons["differs at #{at}: #{(lb[i] || '(end)').strip[0, 60]} / #{err.lines.first.to_s.strip[0, 60]}"] += 1
      end
    end
    c.each { |k, v| total[k] += v }
    rows << [name, c, reasons]
    puts format('%-32s tried %3d written %3d C-refused %3d SabiRuby-refused %3d same %3d expected %2d differ %3d',
                name, c[:tried], c[:written], c[:c_refused], c[:s_refused], c[:same], c[:expected], c[:differ])
    reasons.sort_by { |_, v| -v }.each { |k, v| puts "    #{v} #{k}" }
  end
end
puts
puts "total: tried #{total[:tried]} written #{total[:written]} C-refused #{total[:c_refused]} " \
     "SabiRuby-refused #{total[:s_refused]} same #{total[:same]} expected #{total[:expected]} differ #{total[:differ]}"
puts "files left out (SabiRuby differs from C mruby without any checkpoint): #{skipped.size}"
skipped.each { |s| puts "  #{s}" }
exit(total[:differ] == 0 ? 0 : 1)
