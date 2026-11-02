# Compare mruby's own benchmarks between three 64-bit builds:
#   orig     unpatched mruby (same config, mruby-task, no mruby-checkpoint)
#   patched  with patches/*.patch, no mruby-checkpoint
#   checkpoint with patches/*.patch and mruby-checkpoint (the hook is installed)
#
#   ruby tools/bench.rb BUILD_DIR [rounds]
#
# Runs are interleaved (orig, patched, checkpoint, orig, ...) on one CPU and the
# median wall time of each is reported. Wall time on the host is only a
# hint (the layout of mrb_vm_exec moves it by +-10%): the acceptance check
# is the instruction count under valgrind on the host and the time on the
# ESP32 (doc/plan.md).

require 'benchmark'

build = ARGV[0] || File.expand_path('../build', __dir__)
rounds = (ARGV[1] || ENV['ROUNDS'] || 7).to_i
root = File.expand_path('..', __dir__)
bench_dir = File.join(root, 'vendor/mruby/benchmark')
benches = %w(bm_fib bm_so_lists bm_ao_render bm_so_mandelbrot bm_mandel_term)
variants = %w(orig patched checkpoint)
bins = variants.to_h { |v| [v, File.join(build, "bench-#{v}", 'host', 'bin', 'mruby')] }
bins.each { |v, b| abort "missing #{b}" unless File.exist?(b) }
cpu = ENV['CPU'] || '2'

def median(a)
  s = a.sort
  s.size.odd? ? s[s.size / 2] : (s[s.size / 2 - 1] + s[s.size / 2]) / 2.0
end

puts "rounds=#{rounds}, taskset -c #{cpu}, median wall seconds"
puts format('%-20s %9s %9s %11s %9s %12s', 'benchmark', 'orig', 'patched', 'checkpoint', 'patch%', 'checkpoint%')
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
  puts format('%-20s %9.3f %9.3f %11.3f %+8.2f%% %+11.2f%%', bm, m['orig'], m['patched'], m['checkpoint'],
              (m['patched'] / m['orig'] - 1) * 100, (m['checkpoint'] / m['orig'] - 1) * 100)
end
puts format('%-20s %9.3f %9.3f %11.3f %+8.2f%% %+11.2f%%', 'total', total['orig'], total['patched'], total['checkpoint'],
            (total['patched'] / total['orig'] - 1) * 100, (total['checkpoint'] / total['orig'] - 1) * 100)
