# Run test/samples/*.rb with and without checkpoints and compare the outputs.
#
#   ruby tools/run_tests.rb [--bless] [sample ...]
#
# For each sample:
#   reference   checkpoint run --ignore (64-bit); must equal test/samples/NAME.out
#               and the 32-bit reference
# Samples that suspend (Checkpoint.suspend):
#   inproc      checkpoint run --inproc: each suspend restores into a new
#               mrb_state in the same process (64 and 32)
#   A->B        checkpoint run on A, then checkpoint resume alternating B, A, B ...
#               while the program keeps suspending; the joined output must equal
#               the reference. A->B is 64->64, 64->32, 32->64, 32->32.
# Samples that save (Checkpoint.save, annotation save-after):
#   A           checkpoint run on A: the saving side goes on, its output must
#               equal the reference, and it must have written an image
#   A->B        checkpoint resume of that image on B: the restored side's output
#               must equal test/samples/NAME.resumed.out
#   inproc      checkpoint run --inproc: the restored program runs to its end
#               inside the save, then the saving one goes on; the output must be
#               the reference up to LINE, NAME.resumed.out, the rest of the
#               reference
# Every image must also read cleanly with tools/checkpoint_inspect.rb.
#
# SabiRuby (the Rust mruby VM, built with its feature `checkpoint`) resumes the images too,
# one way (mruby -> SabiRuby). For each sample and each writer (64 and 32):
#   A->sabiruby #k   the k-th image along the chain of suspends (the chain goes on in the
#                    C mruby of the same width) resumed on SabiRuby: what the C processes
#                    printed before it + SabiRuby's output must equal the reference.
#                    SabiRuby writes no image: its save / suspend answer what the reference
#                    run (--ignore) answers, so it runs on to the end.
#   A save->sabiruby the image of a save sample resumed on SabiRuby: NAME.resumed.out.
#   Annotation `sabiruby-refuse K "TEXT"`: SabiRuby must refuse the K-th image (exit
#   status not 0, nothing on stdout) with a message that contains TEXT.
# The SabiRuby binary is SABIRUBY_BIN, or SABIRUBY_DIR/target/release/sabiruby (build it
# with `rake sabiruby`). Without one, or with one built without the feature, these checks
# are skipped and the run says so.
#
# Annotations in the sample (lines "# checkpoint-test: ..."):
#   suspend-after "LINE"  the first process's output must end with LINE
#                         (the suspend happened right there)
#   save-after "LINE"     a save sample; the save happens right after LINE
#   refuse                no image may be written; the output of a run that
#                         asks for checkpoints must equal NAME.out
# --bless writes NAME.out from the reference run (and NAME.resumed.out from
# the 64->64 resume of a save sample).
#
# CHECKPOINT_BIN64 replaces the 64-bit driver (the ASan builds, see Rakefile).

require 'open3'
require 'tmpdir'
require 'fileutils'

ROOT = File.expand_path('..', __dir__)
INSPECT = File.join(ROOT, 'tools/checkpoint_inspect.rb')
CHECKPOINT = {
  '64' => ENV['CHECKPOINT_BIN64'] ? File.expand_path(ENV['CHECKPOINT_BIN64']) : File.join(ROOT, 'build/mruby/host/bin/checkpoint'),
  '32' => File.join(ROOT, 'build/mruby/host32/bin/checkpoint'),
}
CHECKPOINT.each_value { |b| abort "missing #{b}" unless File.exist?(b) }

bless = ARGV.delete('--bless')
samples = ARGV.empty? ? Dir[File.join(ROOT, 'test/samples/*.rb')].sort : ARGV.map { |a| File.expand_path(a) }

def run(arch, *args, dir:)
  out, err, st = Open3.capture3(CHECKPOINT.fetch(arch), *args, chdir: dir)
  [out, err, st.exitstatus]
end

def inspect_ok?(img)
  `ruby #{INSPECT} #{img}`.lines.last&.chomp == 'end'
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
  ann = src.lines.grep(/^# checkpoint-test:/).map { |l| l.sub(/^# checkpoint-test:\s*/, '').strip }
  refuse = ann.include?('refuse')
  suspend_after = ann.map { |a| a[/\Asuspend-after "(.*)"\z/, 1] }.compact.first
  save_after = ann.map { |a| a[/\Asave-after "(.*)"\z/, 1] }.compact.first
  golden_path = path.sub(/\.rb\z/, '.out')
  resumed_path = path.sub(/\.rb\z/, '.resumed.out')
  puts "#{name}:"

  Dir.mktmpdir('checkpoint-test') do |dir|
    FileUtils.cp(path, dir)
    file = File.basename(path)
    img = File.join(dir, 'img.ckpt')

    ref, err, st = run('64', 'run', '--ignore', file, dir: dir)
    check('reference runs', st == 0 && err.empty?, err)
    ref32, err32, st32 = run('32', 'run', '--ignore', file, dir: dir)
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

    if save_after
      at = ref.lines.index { |l| l.chomp == save_after }
      check("reference has the line #{save_after.inspect}", at)
      next unless at
      if bless
        FileUtils.rm_f(img)
        run('64', 'run', '--out', img, file, dir: dir)
        out, = run('64', 'resume', img, dir: dir)
        File.write(resumed_path, out)
      end
      resumed = File.exist?(resumed_path) ? File.read(resumed_path) : nil
      check('resumed golden file exists', resumed)
      next unless resumed

      %w(64 32).each do |a|
        FileUtils.rm_f(img)
        out, err, st = run(a, 'run', '--out', img, file, dir: dir)
        check("#{a} save: the saving side goes on, output = reference",
              out == ref && st == 0 && err.empty? && File.exist?(img), err + diff(ref, out))
        next unless File.exist?(img)
        check("#{a} save: the image reads with tools/checkpoint_inspect.rb", inspect_ok?(img))
        %w(64 32).each do |b|
          o, e, s = run(b, 'resume', img, dir: dir)
          check("#{a}->#{b} save: the restored side goes on, output = #{File.basename(resumed_path)}",
                o == resumed && s == 0 && e.empty?, e + diff(resumed, o))
        end
      end

      expect = ref.lines[0..at].join + resumed + ref.lines[(at + 1)..].join
      %w(64 32).each do |a|
        FileUtils.rm_f(img)
        out, err, st = run(a, 'run', '--inproc', '--out', img, file, dir: dir)
        check("#{a} inproc save (restored side first, then the saving side)",
              out == expect && st == 0 && err.empty?, err + diff(expect, out))
      end
      next
    end

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
      n = 0
      archs = [b, a]
      parsed = true
      while File.exist?(img) && n < 20 && st == 0
        # the image must also read cleanly with the reader written from the spec
        parsed &&= inspect_ok?(img)
        FileUtils.mv(img, img + '.in')
        o, e, st = run(archs[n % 2], 'resume', '--out', img, img + '.in', dir: dir)
        out << o
        errs << e
        n += 1
      end
      ok = n > 0 && out == ref && st == 0 && errs.empty? && parsed
      ok &&= first.lines.last&.chomp == suspend_after if suspend_after
      check("#{a}->#{b} (#{n} suspend#{n == 1 ? '' : 's'})", ok,
            "#{errs}#{diff(ref, out)}\nfirst process ended with #{first.lines.last.inspect}" \
            "#{parsed ? '' : "\ntools/checkpoint_inspect.rb could not read an image"}")
    end
  end
end

def sabiruby_bin
  bin = ENV['SABIRUBY_BIN'] || (ENV['SABIRUBY_DIR'] && File.join(ENV['SABIRUBY_DIR'], 'target/release/sabiruby'))
  return [nil, 'set SABIRUBY_DIR (a SabiRuby checkout with the feature checkpoint) or SABIRUBY_BIN'] unless bin
  bin = File.expand_path(bin)
  return [nil, "#{bin} does not exist (rake sabiruby builds it)"] unless File.executable?(bin)
  help, = Open3.capture2e(bin, '--help')
  return [nil, "#{bin} was built without the feature checkpoint"] unless help.include?('--resume')
  [bin, nil]
end

SABIRUBY, sabiruby_skip = sabiruby_bin
$sabiruby_skipped = 0

def sabiruby_resume(img, dir)
  out, err, st = Open3.capture3(SABIRUBY, '--resume', img, chdir: dir)
  [out, err, st.exitstatus]
end

puts
if SABIRUBY.nil?
  puts "SKIP SabiRuby resume: #{sabiruby_skip}"
else
  puts "SabiRuby resume (#{SABIRUBY}):"
  samples.each do |path|
    name = File.basename(path, '.rb')
    src = File.read(path)
    ann = src.lines.grep(/^# checkpoint-test:/).map { |l| l.sub(/^# checkpoint-test:\s*/, '').strip }
    next if ann.include?('refuse')
    save_after = ann.any? { |a| a.start_with?('save-after') }
    refused = ann.filter_map { |a| (m = a.match(/\Asabiruby-refuse (\d+) "(.*)"\z/)) && [m[1].to_i, m[2]] }.to_h
    golden = File.read(path.sub(/\.rb\z/, '.out'))
    puts "#{name}:"
    Dir.mktmpdir('checkpoint-sabiruby') do |dir|
      FileUtils.cp(path, dir)
      file = File.basename(path)
      img = File.join(dir, 'img.ckpt')
      %w(64 32).each do |a|
        FileUtils.rm_f(img)
        prefix, = run(a, 'run', '--out', img, file, dir: dir)
        if save_after
          resumed = File.read(path.sub(/\.rb\z/, '.resumed.out'))
          o, e, s = sabiruby_resume(img, dir)
          check("#{a} save->sabiruby: output = #{name}.resumed.out", o == resumed && s == 0 && e.empty?, e + diff(resumed, o))
          next
        end
        k = 0
        while File.exist?(img) && k < 20
          k += 1
          FileUtils.mv(img, img + '.in')
          o, e, s = sabiruby_resume(img + '.in', dir)
          if (why = refused[k])
            check("#{a}->sabiruby image #{k} refused: #{why}", s != 0 && o.empty? && e.include?(why), e)
          else
            joined = prefix + o
            check("#{a}->sabiruby image #{k}", joined == golden && s == 0 && e.empty?, e + diff(golden, joined))
          end
          # the chain goes on in C mruby to the next image
          o, = run(a, 'resume', '--out', img, img + '.in', dir: dir)
          prefix += o
        end
        check("#{a}: at least one image", k > 0)
      end
    end
  end
end

puts
puts "#{$pass} passed, #{$fail} failed#{SABIRUBY ? '' : ' (SabiRuby resume skipped)'}"
exit($fail == 0 ? 0 : 1)
