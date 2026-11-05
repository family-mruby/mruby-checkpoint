# tools/test_table.rb - the drivers with a fixed table of their baseline
# (rake build:table) against the drivers without one.
#
#   - every sample writes the same image with and without the table, but for
#     the section TBL0 (the stamp of the baseline the writer used)
#   - an image whose TBL0 names other table rules is refused with the reason
#   - a driver whose state differs from its table says how and keeps what
#     differs itself (here: a method taken away before the baseline, --without)
#
# Run by `rake test` after run_tests.rb has run the samples on the tabled
# drivers.

require 'open3'
require 'tmpdir'
require 'fileutils'

ROOT = File.expand_path('..', __dir__)
PLAIN = {
  '64' => File.join(ROOT, 'build/mruby/host/bin/checkpoint'),
  '32' => File.join(ROOT, 'build/mruby/host32/bin/checkpoint'),
}
TABLED = {
  '64' => File.join(ROOT, 'build/tabled/host/bin/checkpoint'),
  '32' => File.join(ROOT, 'build/tabled/host32/bin/checkpoint'),
}
(PLAIN.values + TABLED.values).each { |b| abort "missing #{b} (rake build:table)" unless File.exist?(b) }

$pass = 0
$fail = 0

def check(name, ok, detail = nil)
  if ok
    $pass += 1
  else
    $fail += 1
    puts "  FAIL #{name}"
    puts detail.to_s.gsub(/^/, '       ') if detail
  end
end

# the image without its TBL0 section (tag, length 12, version, stamp)
def without_tbl0(bytes)
  i = bytes.index("TBL0\x00\x00\x00\x0c".b)
  return nil unless i
  bytes[0, i] + bytes[(i + 20)..]
end

samples = Dir[File.join(ROOT, 'test/samples/*.rb')].sort.reject do |path|
  ann = File.read(path).lines.grep(/^# checkpoint-test:/).join
  ann.include?('refuse') || ann.include?('clock virtual')
end

compared = 0
samples.each do |path|
  name = File.basename(path, '.rb')
  %w(64 32).each do |arch|
    Dir.mktmpdir('checkpoint-table') do |dir|
      FileUtils.cp(path, dir)
      file = File.basename(path)
      imgs = { PLAIN => 'plain.ckpt', TABLED => 'tabled.ckpt' }.map do |bins, out|
        _, err, = Open3.capture3(bins[arch], 'run', '--out', out, file, chdir: dir)
        img = File.join(dir, out)
        File.exist?(img) ? File.binread(img) : (check("#{name} #{arch}: an image", false, err); nil)
      end
      next unless imgs.all?
      a, b = imgs.map { |i| without_tbl0(i) }
      check("#{name} #{arch}: TBL0 in both images", a && b)
      check("#{name} #{arch}: the same image but for TBL0", a == b,
            "plain #{imgs[0].bytesize} B, tabled #{imgs[1].bytesize} B")
      compared += 1
    end
  end
end
puts "images with and without the table: #{compared} compared"

Dir.mktmpdir('checkpoint-table') do |dir|
  FileUtils.cp(File.join(ROOT, 'test/samples/counter.rb'), dir)
  Open3.capture3(TABLED['64'], 'run', '--out', 'c.ckpt', 'counter.rb', chdir: dir)
  img = File.binread(File.join(dir, 'c.ckpt'))
  i = img.index("TBL0\x00\x00\x00\x0c".b)
  check('TBL0 found', i)
  if i
    # another version of the table rules: refused, with the reason
    bad = img.dup
    bad[i + 8, 4] = [2].pack('N')
    File.binwrite(File.join(dir, 'bad.ckpt'), bad)
    TABLED.merge('plain' => PLAIN['64']).each do |arch, bin|
      _, err, st = Open3.capture3(bin, 'resume', 'bad.ckpt', chdir: dir)
      check("table rules v2 refused (#{arch})", st.exitstatus == 3 && err.include?('table version 2'), err)
    end
    # the same image as it was: resumed
    out, err, st = Open3.capture3(TABLED['32'], 'resume', 'c.ckpt', chdir: dir)
    check('resumed on the tabled 32-bit driver', st.exitstatus == 0 && out.include?('count 4'), err)
  end
  # a state that differs from its table: said why, and the image still resumes
  out, err, st = Open3.capture3({ 'CHECKPOINT_TABLE_STATUS' => '1' }, TABLED['64'], 'resume',
                                '--without', 'Integer#times', 'c.ckpt', chdir: dir)
  check('a state that differs from the table says how',
        err.include?('table used with differences') && err.include?('Integer') &&
        st.exitstatus == 0 && out.include?('count 4'), err)
  _, err, = Open3.capture3({ 'CHECKPOINT_TABLE_STATUS' => '1' }, TABLED['64'], 'names')
  check('the table is used', err.strip == 'checkpoint: table used', err)
end

puts "#{$pass} passed, #{$fail} failed"
exit($fail.zero? ? 0 : 1)
