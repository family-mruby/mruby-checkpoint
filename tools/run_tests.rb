# Run test/samples/*.rb with and without a hop and compare the outputs.
#
#   ruby tools/run_tests.rb [--bless] [sample ...]
#
# For each sample:
#   reference   ghost run --no-hop (64-bit); must equal test/samples/NAME.out
#               and the 32-bit reference
#   inproc      ghost run --inproc: the hop restores into a new mrb_state in
#               the same process (64 and 32)
#   A->B        ghost run on A, then ghost resume alternating B, A, B ... while
#               the program keeps hopping; the joined output must equal the
#               reference. A->B is 64->64, 64->32, 32->64, 32->32.
# Annotations in the sample (lines "# ghost-test: ..."):
#   hop-after "LINE"  the first process's output must end with LINE
#                     (the hop happened right there)
#   refuse            no image may be written; the hop output must equal NAME.out
# --bless writes NAME.out from the reference run.

require 'open3'
require 'tmpdir'
require 'fileutils'

ROOT = File.expand_path('..', __dir__)
GHOST = {
  '64' => File.join(ROOT, 'build/mruby/host/bin/ghost'),
  '32' => File.join(ROOT, 'build/mruby/host32/bin/ghost'),
}

bless = ARGV.delete('--bless')
samples = ARGV.empty? ? Dir[File.join(ROOT, 'test/samples/*.rb')].sort : ARGV.map { |a| File.expand_path(a) }

def run(arch, *args, dir:)
  out, err, st = Open3.capture3(GHOST.fetch(arch), *args, chdir: dir)
  [out, err, st.exitstatus]
end

$fail = 0
$pass = 0

def check(name, ok, detail = nil)
  if ok
    $pass += 1
    puts "  ok   #{name}"
  else
    $fail += 1
    puts "  FAIL #{name}"
    puts detail.to_s.gsub(/^/, '       ') if detail
  end
end

def diff(a, b)
  la = a.lines; lb = b.lines
  i = (0...[la.size, lb.size].max).find { |k| la[k] != lb[k] }
  i ? "line #{i + 1}: expected #{la[i].inspect}, got #{lb[i].inspect}" : 'same'
end

samples.each do |path|
  name = File.basename(path, '.rb')
  src = File.read(path)
  ann = src.lines.grep(/^# ghost-test:/).map { |l| l.sub(/^# ghost-test:\s*/, '').strip }
  refuse = ann.include?('refuse')
  hop_after = ann.map { |a| a[/\Ahop-after "(.*)"\z/, 1] }.compact.first
  golden_path = path.sub(/\.rb\z/, '.out')
  puts "#{name}:"

  Dir.mktmpdir('ghost-test') do |dir|
    FileUtils.cp(path, dir)
    file = File.basename(path)
    img = File.join(dir, 'img.ghost')

    ref, err, st = run('64', 'run', '--no-hop', file, dir: dir)
    check('reference runs', st == 0 && err.empty?, err)
    ref32, err32, st32 = run('32', 'run', '--no-hop', file, dir: dir)
    check('reference 32 = 64', ref32 == ref && st32 == 0, err32 + diff(ref, ref32))

    if refuse
      if bless
        out, = run('64', 'run', '--out', img, file, dir: dir)
        File.write(golden_path, out)
      end
      golden = File.exist?(golden_path) ? File.read(golden_path) : nil
      check('golden file exists', golden)
      %w(64 32).each do |a|
        FileUtils.rm_f(img)
        out, err, st = run(a, 'run', '--out', img, file, dir: dir)
        check("#{a}: refused, nothing written, output = golden",
              st == 0 && !File.exist?(img) && out == golden, err + diff(golden.to_s, out))
      end
      next
    end

    File.write(golden_path, ref) if bless
    golden = File.exist?(golden_path) ? File.read(golden_path) : nil
    check('reference = golden', golden == ref, golden ? diff(golden, ref) : 'no golden file')

    %w(64 32).each do |a|
      FileUtils.rm_f(img)
      out, err, st = run(a, 'run', '--inproc', '--out', img, file, dir: dir)
      check("#{a} inproc (same process, new mrb_state)", out == ref && st == 0 && err.empty?,
            err + diff(ref, out))
    end

    [%w(64 64), %w(64 32), %w(32 64), %w(32 32)].each do |a, b|
      FileUtils.rm_f(img)
      out, err, st = run(a, 'run', '--out', img, file, dir: dir)
      first = out.dup
      errs = err.dup
      hops = 0
      archs = [b, a]
      while File.exist?(img) && hops < 20 && st == 0
        FileUtils.mv(img, img + '.in')
        o, e, st = run(archs[hops % 2], 'resume', '--out', img, img + '.in', dir: dir)
        out << o
        errs << e
        hops += 1
      end
      ok = hops > 0 && out == ref && st == 0 && errs.empty?
      ok &&= first.lines.last&.chomp == hop_after if hop_after
      check("#{a}->#{b} (#{hops} hop#{hops == 1 ? '' : 's'})", ok,
            "#{errs}#{diff(ref, out)}\nfirst process ended with #{first.lines.last.inspect}")
    end
  end
end

puts
puts "#{$pass} passed, #{$fail} failed"
exit($fail == 0 ? 0 : 1)
