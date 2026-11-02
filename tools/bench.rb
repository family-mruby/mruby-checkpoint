# Compare mruby's own benchmarks between three 64-bit builds:
#   orig     unpatched mruby (same config, mruby-task, no mruby-ghost)
#   patched  with patches/*.patch, no mruby-ghost
#   ghost    with patches/*.patch and mruby-ghost (the hook is installed)
#
#   ruby tools/bench.rb BUILD_DIR [rounds]
#
# Runs are interleaved (orig, patched, ghost, orig, ...) on one CPU and the
# median wall time of each is reported.

require 'benchmark'

build = ARGV[0] || File.expand_path('../build', __dir__)
rounds = (ARGV[1] || ENV['ROUNDS'] || 7).to_i
root = File.expand_path('..', __dir__)
bench_dir = File.join(root, 'vendor/mruby/benchmark')
benches = %w(bm_fib bm_so_lists bm_ao_render bm_so_mandelbrot bm_mandel_term)
variants = %w(orig patched ghost)
bins = variants.to_h { |v| [v, File.join(build, "bench-#{v}", 'host', 'bin', 'mruby')] }
bins.each { |v, b| abort "missing #{b}" unless File.exist?(b) }
cpu = ENV['CPU'] || '2'

def median(a)
  s = a.sort
  s.size.odd? ? s[s.size / 2] : (s[s.size / 2 - 1] + s[s.size / 2]) / 2.0
end

puts "rounds=#{rounds}, taskset -c #{cpu}, median wall seconds"
puts format('%-20s %9s %9s %9s %9s %9s', 'benchmark', 'orig', 'patched', 'ghost', 'patch%', 'ghost%')
total = Hash.new(0.0)
benches.each do |bm|
  file = File.join(bench_dir, "#{bm}.rb")
  times = Hash.new { |h, k| h[k] = [] }
  rounds.times do
    variants.each do |v|
      t = Benchmark.realtime { system('taskset', '-c', cpu, bins[v], file, out: File::NULL) or abort "#{v} #{bm} failed" }
      times[v] << t
    end
  end
  m = variants.to_h { |v| [v, median(times[v])] }
  variants.each { |v| total[v] += m[v] }
  puts format('%-20s %9.3f %9.3f %9.3f %+8.2f%% %+8.2f%%', bm, m['orig'], m['patched'], m['ghost'],
              (m['patched'] / m['orig'] - 1) * 100, (m['ghost'] / m['orig'] - 1) * 100)
end
puts format('%-20s %9.3f %9.3f %9.3f %+8.2f%% %+8.2f%%', 'total', total['orig'], total['patched'], total['ghost'],
            (total['patched'] / total['orig'] - 1) * 100, (total['ghost'] / total['orig'] - 1) * 100)
