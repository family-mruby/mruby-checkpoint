# Tests of the one image reader and its views (tools/lib/, doc/inspect_json.md),
# run by `rake test`.
#
#   ruby tools/test_inspect.rb
#
# 1. images   every image of test/samples (both start shapes, written by the
#             64-bit and the 32-bit build, along the chains of suspends):
#             it reads, its JSON has the documented shape, every carried irep
#             hashes to its hash, no warning, every view prints, the diff with
#             itself is empty, the diff of the 64-bit and the 32-bit image of
#             one point is the writer's pointer width only
# 2. stack    test/inspect/stack.rb: the stacks derived from the images are
#             the backtraces mruby prints at those points (the running
#             context, and the contexts that waited)
# 3. diff     test/inspect/diff.rb, diff_task.rb: two saves of one program
#             differ in what the program changed between them, and only that
# 4. broken   images cut at every byte or with a byte changed: the reader
#             stops with where it broke (never another exception); a section
#             it does not know is kept as hex; bytes after END0 are refused
# 5. html     the page of an image and of a diff: one file that loads
#             nothing from outside, whose embedded JSON reads back as the
#             doc; with a headless Chromium (CHECKPOINT_CHROME, or the one
#             of Playwright's cache), every view draws without a script error
#
# Exit status 1 when a check fails.

require 'open3'
require 'tmpdir'
require 'fileutils'
require 'json'
require_relative 'lib/checkpoint_image'
require_relative 'lib/checkpoint_text'
require_relative 'lib/checkpoint_diff'
require_relative 'lib/checkpoint_html'
require_relative 'lib/checkpoint_schema'

ROOT = File.expand_path('..', __dir__)
CLI = File.join(ROOT, 'tools/checkpoint_inspect.rb')
NORMAL = { '64' => File.join(ROOT, 'build/mruby/host/bin/checkpoint'), '32' => File.join(ROOT, 'build/mruby/host32/bin/checkpoint') }
VIRTUAL = { '64' => File.join(ROOT, 'build/fuzz/host/bin/checkpoint'), '32' => File.join(ROOT, 'build/fuzz/host32/bin/checkpoint') }
(NORMAL.values + VIRTUAL.values).each { |b| abort "missing #{b} (rake build, rake build:fuzz)" unless File.exist?(b) }

$pass = 0
$fail = 0
def check(name, ok, detail = nil)
  if ok
    $pass += 1
  else
    $fail += 1
    puts "  FAIL #{name}"
    puts detail.to_s.lines.first(20).join.gsub(/^/, '       ') if detail
  end
  ok
end

def run(bin, *args, dir:)
  out, err, st = Open3.capture3(bin, *args, chdir: dir)
  [out, err, st.exitstatus]
end

def annotations(path)
  File.read(path).lines.grep(/^# checkpoint-test:/).map { |l| l.sub(/^# checkpoint-test:\s*/, '').strip }
end

# every image of a program: [label, bytes], along its chain of suspends
def images_of(path, bins, arch, as_task, dir)
  file = File.basename(path)
  FileUtils.cp(path, dir)
  img = File.join(dir, 'img.ckpt')
  FileUtils.rm_f(img)
  run(bins[arch], 'run', *(as_task ? ['--as-task'] : []), '--out', img, file, dir: dir)
  out = []
  k = 0
  while File.exist?(img) && k < 20
    k += 1
    out << [k, File.binread(img)]
    FileUtils.mv(img, img + '.in')
    run(bins[arch], 'resume', '--out', img, img + '.in', dir: dir)
  end
  out
end

def view_errors(doc)
  errs = []
  errs << 'dump does not end with "end"' unless CheckpointText.dump(doc).end_with?("end\n")
  errs << '--stack is empty' if CheckpointText.stack(doc).empty?
  CheckpointText.objects(doc, depth: 3)
  CheckpointText.tasks(doc)
  errs
rescue StandardError => e
  errs << "#{e.class}: #{e.message} #{e.backtrace[0]}"
end

# ---- 1. the images of the samples ----

puts 'images of test/samples:'
all_images = []
Dir.mktmpdir('checkpoint-inspect') do |dir|
  Dir[File.join(ROOT, 'test/samples/*.rb')].sort.each do |path|
    ann = annotations(path)
    next if ann.include?('refuse')
    bins = ann.include?('clock virtual') ? VIRTUAL : NORMAL
    [false, true].each do |as_task|
      next if as_task && ann.include?('no-as-task')
      by_arch = {}
      %w(64 32).each do |arch|
        by_arch[arch] = images_of(path, bins, arch, as_task, dir)
        by_arch[arch].each do |k, bytes|
          name = "#{File.basename(path, '.rb')}#{as_task ? ' (as a Task)' : ''} #{arch} ##{k}"
          all_images << [name, bytes]
          begin
            doc = CheckpointImage.read(bytes, name: "#{File.basename(path, '.rb')}.#{k}.ckpt")
          rescue CheckpointImage::FormatError => e
            check("#{name}: reads", false, e.message)
            next
          end
          check("#{name}: JSON shape", (e = CheckpointSchema.check(doc)).empty?, e.join("\n"))
          check("#{name}: JSON shape without code", (e = CheckpointSchema.check(CheckpointImage.without_code(doc))).empty?, e.join("\n"))
          check("#{name}: irep hashes", doc['ireps'].all? { |ir| ir['hash_ok'] != false })
          check("#{name}: no warnings", doc['warnings'].empty?, doc['warnings'].join("\n"))
          check("#{name}: JSON round trip", JSON.parse(JSON.generate(doc)) == doc)
          check("#{name}: views", (e = view_errors(doc)).empty?, e.join("\n"))
          d = CheckpointDiff.diff(doc, doc)
          check("#{name}: diff with itself is empty", d['same'], CheckpointDiff.text(d))
          check("#{name}: diff JSON shape", (e = CheckpointSchema.check_diff(d)).empty?, e.join("\n"))
        end
      end
      a = by_arch['64']
      b = by_arch['32']
      name = "#{File.basename(path, '.rb')}#{as_task ? ' (as a Task)' : ''}"
      check("#{name}: as many images from 64 and 32 (#{a.size}, #{b.size})", a.size == b.size && !a.empty?)
      a.zip(b).each do |(k, x), (_, y)|
        next unless y
        d = CheckpointDiff.diff(CheckpointImage.read(x), CheckpointImage.read(y))
        only_width = d['header'].map { |h| h['field'] } == ['pointer_bits'] && d['variables'].empty? &&
                     d['tasks'].empty? && d['scheduler'].empty? && d['contexts'].all? { |c| c['change'] == 'same' } &&
                     d['objects'].values.all?(&:empty?)
        check("#{name} ##{k}: 64 -> 32 differs in the pointer width only", only_width, CheckpointDiff.text(d))
      end
    end
  end
end
puts "  #{all_images.size} images (#{all_images.sum { |_, b| b.bytesize }} bytes)"

# ---- 2. the stacks are the backtraces ----

puts 'stacks = backtraces (test/inspect/stack.rb):'
Dir.mktmpdir('checkpoint-inspect') do |dir|
  FileUtils.cp(File.join(ROOT, 'test/inspect/stack.rb'), dir)
  %w(64 32).product([false, true]).each do |arch, as_task|
    shape = "#{arch}#{as_task ? ' as a Task' : ''}"
    Dir[File.join(dir, '*.ckpt')].each { |f| File.delete(f) }
    out, err, st = run(VIRTUAL[arch], 'run', *(as_task ? ['--as-task'] : []), 'stack.rb', dir: dir)
    check("#{shape}: runs", st == 0 && err.empty?, err)
    lines = out.lines.grep(/\A(BT|WAIT) /)
    check("#{shape}: 6 backtraces printed", lines.size == 6, out)
    lines.each do |l|
      l = l.chomp
      f = l.start_with?('BT') ? l.split(' ', 3) : l.split(' ', 4)
      img = File.join(dir, "#{f[1]}.ckpt")
      doc = CheckpointImage.read_file(img)
      cur = doc['contexts'][doc['current_context'] - 1]
      if f[0] == 'BT'
        want = f[2].split('|')
        check("#{shape} #{f[1]}: the running context's stack = the backtrace", cur['backtrace'] == want,
              "image:     #{cur['backtrace'].inspect}\nbacktrace: #{want.inspect}")
        # the text view prints the same lines first
        text = CheckpointText.stack(doc).lines
        at = text.index { |t| t.start_with?("context #{cur['id']}:") }
        printed = text[(at + 1)..].take_while { |t| !t.start_with?('context ') }.reject { |t| t.start_with?('      ') }
                                  .map { |t| t.strip.sub(/\s+\[.*\z/, '') }
        check("#{shape} #{f[1]}: --stack prints that backtrace", printed == want, printed.inspect)
      else
        c =
          case f[2]
          when 'prev' then doc['contexts'][cur['prev'] - 1]
          else
            t = doc['scheduler']['tasks'].find { |x| x['name_text'] == f[2] }
            t && doc['contexts'][t['context'] - 1]
          end
        want = f[3].split('|')
        got = c ? c['frames'].reverse.select { |x| x['kind'] == 'ruby' && x['location'] }.map { |x| x['location'] } : nil
        check("#{shape} #{f[1]} #{f[2]}: the waiting context's frames = its backtrace when it goes on", got == want,
              "image:     #{got.inspect}\nbacktrace: #{want.inspect}")
      end
    end
  end
end

# ---- 3. the diff of two saves ----

def summary(d)
  s = []
  d['header'].each { |h| s << "header #{h['field']}: #{h['a']} -> #{h['b']}" }
  d['variables'].each { |v| s << "var #{v['key']}: #{v['a']} -> #{v['b']}" }
  d['contexts'].each do |c|
    s << "context #{c['key']} #{c['change']}" if %w(added removed).include?(c['change'])
    (c['fields'] || []).each { |f| s << "context #{c['key']} #{f['field']}: #{f['a']} -> #{f['b']}" }
    (c['frames'] || []).each do |f|
      case f['change']
      when 'called', 'returned' then s << "frame #{c['key']}/#{f['index']} #{f['change']}"
      when 'changed'
        s << "frame #{c['key']}/#{f['index']} #{f['a']} -> #{f['b']}" if f['a'] != f['b']
        f['locals'].each { |l| s << (l['contents_changed'] ? "local #{c['key']}/#{f['index']} #{l['name']} (contents)" : "local #{c['key']}/#{f['index']} #{l['name']}: #{l['a']} -> #{l['b']}") }
      end
    end
  end
  d['tasks'].each { |t| (t['fields'] || [["#{t['change']}"]]).each { |f| s << (f.is_a?(Hash) ? "task #{t['key']} #{f['field']}: #{f['a']} -> #{f['b']}" : "task #{t['key']} #{f[0]}") } }
  d['scheduler'].each { |f| s << "scheduler #{f['field']}: #{f['a']} -> #{f['b']}" }
  ob = d['objects']
  ob['changed'].reject { |c| c['temporary'] }.each { |c| c['fields'].each { |f| s << "changed #{c['path']} #{f['field']}: #{f['a'] || '-'} -> #{f['b'] || '-'}" } }
  ob['added'].reject { |c| c['temporary'] }.each { |c| s << "added #{c['path']}" }
  ob['removed'].reject { |c| c['temporary'] }.each { |c| s << "removed #{c['path']}" }
  ob['moved'].reject { |c| c['temporary'] }.each { |c| s << "moved #{c['a_path']} -> #{c['b_path']}" }
  s.sort
end

EXPECT = {
  'diff.rb' => [%w(diff_a diff_b), <<~E],
    var $count: 1 -> 2
    frame MAIN/1 diff.rb:20:in work -> diff.rb:28:in work
    local MAIN/1 h (contents)
    local MAIN/1 n: 10 -> 15
    local MAIN/1 old_home: ["moves"] (MAIN/1:work.old_home) -> nil
    local MAIN/1 new_home: nil -> ["moves"] (MAIN/1:work.new_home)
    changed MAIN/1:work.h {:drop}: MAIN/1:work.h{:drop} -> -
    changed MAIN/1:work.h {:new}: - -> MAIN/1:work.h{:new}
    changed MAIN/1:work.box.@items [2]: - -> 3
    added MAIN/1:work.h{:new}
    removed MAIN/1:work.h{:drop}
    moved MAIN/1:work.old_home -> MAIN/1:work.new_home
    moved MAIN/1:work.old_home[0] -> MAIN/1:work.new_home[0]
  E
  'diff_task.rb' => [%w(task_a task_b), <<~E],
    local MAIN/0 log (contents)
    frame task "s"/0 diff_task.rb:20 -> diff_task.rb:23
    local task "a"/2 i: 0 -> 1
    task "a" wait: sleep, 5 ticks left -> sleep, 8 ticks left
    scheduler tick: 5 -> 12
    scheduler next_wakeup: 5 -> 8
    changed MAIN/0:-.log [0]: - -> 0
  E
}

puts 'diff of two saves (test/inspect/diff.rb, diff_task.rb):'
Dir.mktmpdir('checkpoint-inspect') do |dir|
  EXPECT.each do |file, (names, expect)|
    FileUtils.cp(File.join(ROOT, 'test/inspect', file), dir)
    [false, true].each do |as_task|
      main = as_task ? "task #{file.inspect}" : 'root'
      want = expect.lines.map { |l| l.chomp.gsub('MAIN', main) }.sort
      docs = {}
      %w(64 32).each do |arch|
        Dir[File.join(dir, '*.ckpt')].each { |f| File.delete(f) }
        _, err, st = run(VIRTUAL[arch], 'run', *(as_task ? ['--as-task'] : []), file, dir: dir)
        check("#{file} #{arch}#{as_task ? ' as a Task' : ''}: runs", st == 0 && err.empty?, err)
        docs[arch] = names.map { |n| CheckpointImage.read_file(File.join(dir, "#{n}.ckpt")) }
      end
      [%w(64 64), %w(32 32), %w(64 32)].each do |x, y|
        d = CheckpointDiff.diff(docs[x][0], docs[y][1])
        w = x == y ? want : (want + ["header pointer_bits: #{x} -> #{y}"]).sort
        got = summary(d)
        check("#{file} #{x}->#{y}#{as_task ? ' as a Task' : ''}: only the expected differences", got == w,
              "missing:    #{(w - got).inspect}\nunexpected: #{(got - w).inspect}")
        check("#{file} #{x}->#{y}: diff JSON shape", CheckpointSchema.check_diff(d).empty?)
      end
      # the command line: text and JSON (the images of the last run: 32)
      a = File.join(dir, "#{names[0]}.ckpt")
      b = File.join(dir, "#{names[1]}.ckpt")
      out, err, st = run('ruby', CLI, 'diff', a, b, '--json', dir: dir)
      check("#{file}: diff --json", st == 0 && summary(JSON.parse(out)) == want, err)
      out, _, st = run('ruby', CLI, 'diff', a, a, dir: dir)
      check("#{file}: diff of an image with itself says so", st == 0 && out.end_with?("no differences\n"), out)
    end
  end
end

# ---- 4. broken images ----

puts 'broken images:'
srand(5)
cut = 0
flipped = 0
read_ok = 0
small = all_images.sort_by { |_, b| b.bytesize }
[small[0], small[small.size / 2], small[-1]].each do |name, bytes|
  (0...bytes.bytesize).each do |n|
    cut += 1
    begin
      CheckpointImage.read(bytes.byteslice(0, n))
      check("#{name} cut to #{n} bytes: refused", false)
    rescue CheckpointImage::FormatError => e
      check("#{name} cut to #{n} bytes: says where", e.offset.nil? || e.offset <= n, e.message) unless e.offset.nil? || e.offset <= n
    rescue StandardError => e
      check("#{name} cut to #{n} bytes: only FormatError", false, "#{e.class}: #{e.message} #{e.backtrace[0]}")
    end
  end
end
all_images.each_slice(4).map(&:first).each do |name, bytes|
  60.times do
    c = bytes.dup
    i = rand(c.bytesize)
    c.setbyte(i, (c.getbyte(i) + 1 + rand(255)) % 256)
    flipped += 1
    begin
      doc = CheckpointImage.read(c)
      read_ok += 1
      JSON.generate(doc)
      e = view_errors(doc)
      check("#{name} byte #{i} changed: the views still print", e.empty?, e.join("\n"))
    rescue CheckpointImage::FormatError
      nil
    rescue StandardError => e
      check("#{name} byte #{i} changed: only FormatError", false, "#{e.class}: #{e.message} #{e.backtrace[0]}")
    end
  end
end
puts "  #{cut} cut images refused, #{flipped} changed images (#{read_ok} of them still well formed)"
name, bytes = small[small.size / 2]
body = bytes.byteslice(0, bytes.bytesize - 4)
doc = CheckpointImage.read(body + 'ZZZ9' + "\x01\x02\x03".b + 'END0')
sec = doc['sections'].find { |s| s['unknown'] }
check('an unknown section is kept as hex up to END0', sec && sec['tag'] == 'ZZZ9' && sec['bytes'] == 3 && sec['head_hex'] == '010203' &&
      doc['warnings'].any? { |w| w.include?('ZZZ9') })
begin
  CheckpointImage.read(bytes + 'x')
  check('a byte after END0 is refused', false)
rescue CheckpointImage::FormatError => e
  check('a byte after END0 is refused', e.message.include?('after the end mark'), e.message)
end
Dir.mktmpdir('checkpoint-inspect') do |dir|
  f = File.join(dir, 'cut.ckpt')
  File.binwrite(f, bytes.byteslice(0, bytes.bytesize / 2))
  out, err, st = run('ruby', CLI, f, '--stack', dir: dir)
  check('the command line says where a cut image breaks (status 1)', st == 1 && out.empty? && err =~ /broken image at byte \d+ \(.+\)/, err)
  out, _, st = run('ruby', CLI, f, '--json', dir: dir)
  j = JSON.parse(out) rescue nil
  check('--json of a cut image prints the part read and the error (status 1)', st == 1 && j && j['error'] && j['error']['offset'].is_a?(Integer), out[0, 200])
end

# ---- 5. the page ----

def chrome
  c = ENV['CHECKPOINT_CHROME']
  return c if c && File.executable?(c)
  Dir[File.join(Dir.home, '.cache/ms-playwright/chromium_headless_shell-*/*/chrome-headless-shell')].max
end

puts 'html:'
Dir.mktmpdir('checkpoint-inspect') do |dir|
  big = small[-1][1]
  a = File.join(dir, 'a.ckpt')
  File.binwrite(a, big)
  pages = {}
  out, err, st = run('ruby', CLI, a, '--html', File.join(dir, 'one.html'), dir: dir)
  check('--html writes the page', st == 0 && File.exist?(File.join(dir, 'one.html')), err)
  pages['one'] = File.join(dir, 'one.html')
  x = File.join(dir, 'diff_a.ckpt')
  y = File.join(dir, 'diff_b.ckpt')
  FileUtils.cp(File.join(ROOT, 'test/inspect/diff.rb'), dir)
  run(VIRTUAL['64'], 'run', 'diff.rb', dir: dir)
  out, err, st = run('ruby', CLI, 'diff', x, y, '--html', File.join(dir, 'diff.html'), dir: dir)
  check('diff --html writes the page', st == 0 && File.exist?(File.join(dir, 'diff.html')), err)
  pages['diff'] = File.join(dir, 'diff.html')
  pages.each do |k, p|
    html = File.read(p)
    data = CheckpointHtml.extract(html)
    docs = k == 'one' ? [a] : [x, y]
    want = docs.map { |f| JSON.parse(JSON.generate(CheckpointImage.without_code(CheckpointImage.read_file(f)))) }
    check("#{k}: the embedded JSON reads back as the doc", data && data['docs'] == want)
    check("#{k}: the embedded diff reads back", k == 'one' ? data['diff'].nil? : data['diff'] == JSON.parse(JSON.generate(CheckpointDiff.diff(*docs.map { |f| CheckpointImage.read_file(f) }))))
    outside = html.scan(/<script[^>]+src=|<link\b|<img\b|<iframe\b|@import|url\((?!#)|\bfetch\(|XMLHttpRequest|\bhref=/)
    check("#{k}: loads nothing from outside", outside.empty?, outside.inspect)
  end
  if (c = chrome)
    views = { 'one' => %w(stack tasks objects image), 'diff' => %w(stack tasks objects diff image) }
    views.each do |k, vs|
      vs.each do |v|
        dom, = Open3.capture2e(c, '--no-sandbox', '--headless', '--disable-gpu', '--dump-dom', "file://#{pages[k]}#view=#{v}")
        ok = !dom.include?('id="js-error"') && dom.include?('<main id="main">') &&
             { 'stack' => 'class="card', 'tasks' => '<svg', 'objects' => 'class="node', 'diff' => 'class="diff"', 'image' => '<table' }.then { |m| dom.include?(m[v]) }
        check("#{k} #{v}: draws in a headless browser", ok, dom[/id="js-error">[^<]*/] || dom[-400..])
      end
    end
  else
    puts '  SKIP the headless browser check (no CHECKPOINT_CHROME and no Playwright Chromium)'
  end
end

puts
puts "inspect: #{$pass} passed, #{$fail} failed"
exit($fail == 0 ? 0 : 1)
