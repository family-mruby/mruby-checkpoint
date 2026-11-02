# Run mruby's test suite with checkpoints taken from outside at random safe
# points, and compare with the run without them (doc/report/p3.md).
#
#   ruby tools/fuzz.rb [--seeds 1,2,3 | --seed-from N --runs K] [--points N]
#                      [--xproc N] [--keep-dead] [--only PATTERN] [--jobs N]
#                      [--json FILE] [--table FILE] [--quiet] [--as-task] [--quantum N]
#
# For every test file (vendor/mruby/test/t/*.rb, and test/*.rb of each gem in
# the build that has one):
#   reference   checkpoint-fuzz --ref (64-bit): the output and the number of
#               instructions; the same with the 32-bit build must print the same
#   runs        for each seed: checkpoint-fuzz --seed S --mean M, with M chosen
#               so that about --points trigger points fall in the file. Every
#               image is restored into a fresh mrb_state and the old one is
#               closed; every --xproc-th image goes on in another process
#               (32-bit and 64-bit builds in turn). The output and the exit
#               status must equal the reference.
# --as-task runs every program (and its reference) as a Task, the way Family
# mruby starts an application; --quantum N ticks mruby-task's virtual clock
# once every N instructions (timeslices end), in every run alike.
# Outputs are compared after dropping assert.rb's "Time:" line (wall clock) and
# masking mkstemp's random file names.
#
# test/fuzz_exclude.txt lists the assertions whose result may legitimately
# change after a restore (each with its reason). Nothing is left out: they
# run and are counted, and a difference whose first differing line belongs to
# one of them is reported as expected instead of as a failure.
#
# Exit status 1 when a run differs anywhere else.

require 'open3'
require 'json'
require 'tmpdir'
require 'timeout'

ROOT = File.expand_path('..', __dir__)
FUZZ = {
  '64' => ENV['CHECKPOINT_FUZZ64'] ? File.expand_path(ENV['CHECKPOINT_FUZZ64']) : File.join(ROOT, 'build/fuzz/host/bin/checkpoint-fuzz'),
  '32' => File.join(ROOT, 'build/fuzz/host32/bin/checkpoint-fuzz'),
}
MRUBY = File.join(ROOT, 'vendor/mruby')
ASSERT = File.join(MRUBY, 'test/assert.rb')
EXCLUDE = File.join(ROOT, 'test/fuzz_exclude.txt')
FUZZ.each_value { |b| abort "missing #{b} (rake build:fuzz)" unless File.exist?(b) }

opts = { seeds: [1], points: 40, xproc: 10, jobs: (ENV['JOBS'] || 8).to_i, timeout: 120 }
args = ARGV.dup
until args.empty?
  a = args.shift
  case a
  when '--seeds' then opts[:seeds] = args.shift.split(',').map(&:to_i)
  when '--seed-from' then opts[:seed_from] = args.shift.to_i
  when '--runs' then opts[:runs] = args.shift.to_i
  when '--points' then opts[:points] = args.shift.to_i
  when '--xproc' then opts[:xproc] = args.shift.to_i
  when '--keep-dead' then opts[:keep_dead] = true
  when '--only' then opts[:only] = Regexp.new(args.shift)
  when '--jobs' then opts[:jobs] = args.shift.to_i
  when '--json' then opts[:json] = args.shift
  when '--table' then opts[:table] = args.shift
  when '--timeout' then opts[:timeout] = args.shift.to_i
  when '--quiet' then opts[:quiet] = true
  when '--isolate' then opts[:isolate] = true
  when '--as-task' then opts[:as_task] = true
  when '--quantum' then opts[:quantum] = args.shift.to_i
  else abort "unknown option #{a}"
  end
end
if opts[:runs]
  from = opts[:seed_from] || 1
  opts[:seeds] = (from...(from + opts[:runs])).to_a
end

# ---- the test files ----

def test_files
  files = Dir[File.join(MRUBY, 'test/t/*.rb')].sort.map { |f| ['mruby-test', f] }
  active = File.read(File.join(ROOT, 'build/fuzz/host/mrbgems/active_gems.txt')).split
  active.sort.each do |g|
    dir = File.join(MRUBY, 'mrbgems', g, 'test')
    Dir[File.join(dir, '*.rb')].sort.each { |f| files << [g, f] }
  end
  files
end

def label(gem, file)
  gem == 'mruby-test' ? "core/#{File.basename(file)}" : "#{gem}/#{File.basename(file)}"
end

# file => [{assertion:, when:, reason:}]
def exclusions
  return {} unless File.exist?(EXCLUDE)
  File.readlines(EXCLUDE).each_with_object(Hash.new { |h, k| h[k] = [] }) do |l, h|
    next if l.strip.empty? || l.start_with?('#')
    file, assertion, w, reason = l.chomp.split(' | ', 4)
    h[file] << { assertion: assertion, when: w, reason: reason }
  end
end

# the listed assertion the first differing line belongs to, if any
def excluded_by(list, ref, out)
  return nil if list.nil? || list.empty?
  la = normalize(ref).lines; lb = normalize(out).lines
  i = (0...[la.size, lb.size].max).find { |k| la[k] != lb[k] }
  return nil unless i
  line = la[i] || lb[i]
  # the assertion's own line, or its line in the failure summary ("Class: name => message")
  list.find { |e| line.start_with?(e[:assertion]) || line =~ /\A\w+(::\w+)*: #{Regexp.escape(e[:assertion])}/ }
end

# what differs between two runs without any checkpoint: the wall clock in
# assert.rb's report, and the names mkstemp makes (mruby-io's tests put them
# in messages)
def normalize(out)
  out.lines.reject { |l| l.start_with?('   Time: ') }.join.gsub(/(mrbtest_io_\w+\.XX)[A-Za-z0-9]{6}/, '\1??????')
end

def run(arch, args, log, timeout)
  out = err = ''
  st = nil
  Open3.popen3(FUZZ.fetch(arch), *args, '--log', log, chdir: Dir.tmpdir) do |i, o, e, t|
    i.close
    ro = Thread.new { o.read }
    re = Thread.new { e.read }
    if t.join(timeout)
      st = t.value.exitstatus || 128 + t.value.termsig.to_i
    else
      Process.kill('KILL', t.pid) rescue nil
      t.join
      st = :timeout
    end
    out = ro.value
    err = re.value
  end
  [out, err, st]
end

# outcome categories of a trigger point
REQ = {
  'c_boundary' => 'C frame (inside a block called from C)',
  'fiber' => 'Fiber / Task running',
  'tasks' => 'Tasks exist',
  'pending' => 'merged (a request still waiting)',
  'exception' => 'exception in flight',
  'not_ready' => 'not ready',
}
OS_DATA = %w(IO File Dir Socket BasicSocket IPSocket TCPSocket TCPServer UDPSocket UNIXSocket UNIXServer Addrinfo)

def category(line)
  f = line.split(' ', 5)
  case f[3]
  when 'ok' then 'written'
  when 'req' then REQ.fetch(f[4].to_s.split.first.to_s, "req #{f[4]}")
  when 'restore_failed'
    f[4].to_s =~ /#hash or #eql\?/ ? "restore: a Hash key's #hash / #eql? (user code) failed or changed a Hash" : "restore failed: #{f[4].to_s[0, 60]}"
  when 'refused'
    r = f[4].to_s
    case r
    when /C data of class (\S+)/ then OS_DATA.include?($1) ? "OS resource (#{$1})" : "C data #{$1}"
    when /type (\S+) \(class (\S+)\) is not in format v1/ then "type #{$1} (#{$2})"
    when /resumed a Fiber \(mrb_fiber_resume\)/, /was entered from C/ then 'C frame (a Fiber resumed from C)'
    when /waits in the C function (\S+)/ then "C frame (a context waits in #{$1})"
    when /the scheduler runs from the C function (\S+)/ then "C frame (the scheduler run from #{$1})"
    when /synchronous Task/ then 'C frame (a synchronous Task)'
    when /stands outside its code/ then 'a frame past the end of its code (the VM ran past it)'
    when /permanent (\S+) \((\w+)\) was modified/ then "permanent #{$2} modified"
    when /object made by mrb_open has no name: (\S+) (of class )?(\S+)/ then "unnamed permanent #{$1} (reached only through ObjectSpace)"
    when /makes instances of type (\S+)/ then "class of #{$1} instances"
    when /running in a Fiber/ then 'Fiber entered after the request'
    when /Tasks exist/ then 'Tasks exist'
    when /cannot carry a (\S+) value/ then "value #{$1}"
    when /env shared with another context/ then 'env of another context (Fiber)'
    when /C function that has no name/, /Proc made from a C function/ then 'unnamed C function'
    else r[0, 60]
    end
  else f[3].to_s
  end
end

def parse_log(path)
  points = Hash.new(0)
  ends = []
  dead = 0
  bytes = 0
  return [points, ends, 0, 0] unless File.exist?(path)
  File.foreach(path) do |l|
    if l.start_with?('P ')
      points[category(l)] += 1
      f = l.split
      if f[3] == 'ok'
        bytes += f[4].to_i
        dead += f[5].to_i
      end
    elsif l.start_with?('E ')
      ends << l.split[1].to_i
    end
  end
  [points, ends, dead, bytes]
end

# the trigger points of a differing run that make it differ (in-process,
# --at): a single point when one does it alone; otherwise the shortest
# prefix of the written points that differs, then points dropped from it
# while it still differs. "xproc" when the in-process run of all the
# written points does not differ (the other processes matter).
def isolate(file, common, ref, st, log, opts)
  # every point whose request was taken (written, refused at the safe point,
  # or not restorable): its instruction count is where the request was made
  pts = log.lines.select { |l| %w(ok refused restore_failed).include?(l.split[3]) }
           .map { |l| l.split[2].to_i }.uniq
  Dir.mktmpdir('ckiso') do |dir|
    bad = lambda do |set|
      out, _, fst = run('64', [*common, '--at', set.join(','), '--assert', ASSERT, file], File.join(dir, 'l'), opts[:timeout])
      !(normalize(out) == normalize(ref) && fst == st)
    end
    return ['xproc'] unless bad.(pts)
    single = pts.first(40).select { |ic| bad.([ic]) }
    return single unless single.empty?
    lo, hi = 1, pts.size           # shortest failing prefix
    while lo < hi
      mid = (lo + hi) / 2
      bad.(pts[0, mid]) ? hi = mid : lo = mid + 1
    end
    set = pts[0, lo]
    i = set.size - 2
    while i >= 0                   # drop what is not needed, keeping the last
      trial = set[0...i] + set[(i + 1)..]
      set = trial if bad.(trial)
      i -= 1
    end
    ["together: #{set.join(',')}"]
  end
end

# ---- one file ----

def run_file(gem, file, opts, excl)
  name = label(gem, file)
  res = { name: name, gem: gem, file: file, runs: [], counts: Hash.new(0), mismatches: [],
          expected: [] }
  Dir.mktmpdir('ckfuzz') do |dir|
    common = ['--gem', gem, '--tmp', dir]
    common << '--as-task' if opts[:as_task]
    common += ['--quantum', opts[:quantum].to_s] if opts[:quantum].to_i > 0
    log = File.join(dir, 'ref.log')
    ref, err, st = run('64', ['--ref', *common, '--assert', ASSERT, file], log, opts[:timeout])
    _, ends, = parse_log(log)
    res[:insns] = ends.last.to_i
    res[:ref_status] = st
    res[:ref_err] = err[0, 300]
    log32 = File.join(dir, 'ref32.log')
    ref32, _, st32 = run('32', ['--ref', *common, '--assert', ASSERT, file], log32, opts[:timeout])
    res[:ref32_same] = normalize(ref32) == normalize(ref) && st32 == st
    if st == :timeout
      res[:mismatches] << { seed: nil, why: 'reference run timed out' }
      return res
    end
    mean = [res[:insns] / [opts[:points], 1].max, 1].max
    res[:mean] = mean
    opts[:seeds].each do |seed|
      flog = File.join(dir, "s#{seed}.log")
      fargs = [*common, '--seed', seed.to_s, '--mean', mean.to_s]
      if opts[:xproc] > 0
        fargs += ['--xproc', opts[:xproc].to_s, '--peer', FUZZ['32'], '--peer', FUZZ['64']]
      end
      fargs << '--keep-dead' if opts[:keep_dead]
      out, ferr, fst = run('64', [*fargs, '--assert', ASSERT, file], flog, opts[:timeout])
      points, _, dead, bytes = parse_log(flog)
      points.each { |k, v| res[:counts][k] += v }
      res[:dead] = res[:dead].to_i + dead
      res[:bytes] = res[:bytes].to_i + bytes
      same = normalize(out) == normalize(ref) && fst == st
      run = { seed: seed, same: same, status: fst, points: points.values.sum,
              written: points['written'] }
      ex = !same && excluded_by(excl[name], ref, out)
      ex = nil if ex && ex[:when] == 'as-task' && !opts[:as_task]
      ex = nil if ex && ex[:when] == 'sabiruby'   # another VM only (tools/fuzz_sabiruby.rb)
      if ex && ex[:when] == 'another process'
        # listed for other processes only: the same run kept in this process must not differ
        o2, _, s2 = run('64', [*common, '--seed', seed.to_s, '--mean', mean.to_s, '--xproc', '0',
                               *(opts[:keep_dead] ? ['--keep-dead'] : []), '--assert', ASSERT, file],
                        File.join(dir, "s#{seed}-in.log"), opts[:timeout])
        ex = nil unless normalize(o2) == normalize(ref) && s2 == st
      end
      if ex
        res[:expected] << { seed: seed, assertion: ex[:assertion] }
        run[:expected] = ex[:assertion]
      elsif !same
        la = normalize(ref).lines; lb = normalize(out).lines
        i = (0...[la.size, lb.size].max).find { |k| la[k] != lb[k] }
        run[:diff] = i ? "line #{i + 1}: expected #{la[i].inspect}, got #{lb[i].inspect}" : "status #{st} vs #{fst}"
        run[:stderr] = ferr[0, 500]
        run[:log] = File.read(flog) rescue ''
        run[:alone] = isolate(file, common, ref, st, run[:log], opts) if opts[:isolate]
        res[:mismatches] << run
      end
      res[:runs] << run
    end
  end
  res
end

files = test_files
files.select! { |g, f| opts[:only] =~ label(g, f) } if opts[:only]
excl = exclusions
queue = Queue.new
files.each { |x| queue << x }
results = []
lock = Mutex.new
workers = Array.new([opts[:jobs], 1].max) do
  Thread.new do
    loop do
      g, f = begin queue.pop(true) rescue break end
      r = run_file(g, f, opts, excl)
      lock.synchronize do
        results << r
        unless opts[:quiet]
          bad = r[:mismatches].size
          tag = bad == 0 ? (r[:expected].empty? ? 'ok  ' : 'xdiff') : 'FAIL'
          w = r[:counts]['written']; t = r[:counts].values.sum
          puts format('%-5s %-40s points %5d written %5d%s', tag, r[:name], t, w,
                      bad > 0 ? "  #{bad} run(s) differ" : '')
          r[:mismatches].first(2).each do |m|
            puts "        seed #{m[:seed]}: #{m[:diff] || m[:why]}"
            puts "        alone at: #{m[:alone].join(' ')}" if m[:alone]
          end if bad > 0
          $stdout.flush
        end
      end
    end
  end
end
workers.each(&:join)
results.sort_by! { |r| r[:name] }

# ---- summary ----

reasons = Hash.new(0)
results.each { |r| r[:counts].each { |k, v| reasons[k] += v } }
total = reasons.values.sum
failed = results.select { |r| !r[:mismatches].empty? }
xfail = results.select { |r| !r[:expected].empty? }
puts
puts "files #{results.size}, seeds #{opts[:seeds].join(',')}, points #{total}, " \
     "written #{reasons['written']}, refused #{total - reasons['written']}"
reasons.sort_by { |k, v| -v }.each { |k, v| puts format('  %-45s %7d', k, v) }
tb = results.sum { |r| r[:bytes].to_i }; td = results.sum { |r| r[:dead].to_i }
w = [reasons['written'], 1].max
puts format('images: %d bytes on average; dead registers written as nil: %d (%.2f per image)', tb / w, td, td.to_f / w)
puts "differing files: #{failed.size}; expected differences (test/fuzz_exclude.txt): " +
     (xfail.empty? ? 'none' : xfail.map { |r| "#{r[:name]} (#{r[:expected].size} run(s))" }.join(', '))
failed.each do |r|
  r[:mismatches].each { |m| puts "  #{r[:name]} seed #{m[:seed]}: #{m[:diff] || m[:why]}" }
end
diff32 = results.reject { |r| r[:ref32_same] }
puts "reference 32-bit differs from 64-bit: #{diff32.map { |r| r[:name] }.join(' ')}" unless diff32.empty?

if opts[:json]
  File.write(opts[:json], JSON.pretty_generate(results.map { |r|
    r.merge(counts: r[:counts], runs: r[:runs].map { |x| x.reject { |k, _| k == :log } },
            mismatches: r[:mismatches])
  }))
end
if opts[:table]
  keys = reasons.keys.reject { |k| k == 'written' }.sort_by { |k| -reasons[k] }
  File.open(opts[:table], 'w') do |f|
    f.puts "| file | insns | tried | written | refused | differ | refused by reason |"
    f.puts "|---|---:|---:|---:|---:|---:|---|"
    results.each do |r|
      t = r[:counts].values.sum; w = r[:counts]['written']
      why = r[:counts].reject { |k, _| k == 'written' }.sort_by { |_, v| -v }.map { |k, v| "#{k} #{v}" }.join(', ')
      f.puts "| #{r[:name]} | #{r[:insns]} | #{t} | #{w} | #{t - w} | #{r[:mismatches].size}#{r[:expected].empty? ? '' : " (+#{r[:expected].size} expected)"} | #{why} |"
    end
  end
end
exit(failed.empty? ? 0 : 1)
