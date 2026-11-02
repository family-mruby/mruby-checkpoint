# mruby-checkpoint: build, test and benchmark entry points.
#
#   rake build   -- patched mruby + mruby-checkpoint, 64-bit (host) and 32-bit (host32)
#   rake test    -- run test/samples with and without checkpoints, all combinations
#   rake test:asan, rake test:stress
#                -- the same with the 64-bit driver built with AddressSanitizer
#                   (stress: also MRB_GC_STRESS, a full GC at every allocation)
#   rake bench   -- benchmark: unpatched / patched / patched + mruby-checkpoint
#   rake sabiruby -- build the `sabiruby` command with its feature checkpoint in
#                   SABIRUBY_DIR (a SabiRuby checkout); rake test then also resumes
#                   every image on it (skipped, with a notice, without one)
#   rake build:fuzz -- the test builds (build/fuzz): the external trigger
#                   checkpoint-fuzz and the virtual clock of mruby-task
#                   (mrbgems/hal-task-virtual), never in the normal builds
#   rake fuzz[seed,runs,points] -- mruby's test suite stopped from outside at random
#                   safe points, compared with the run without (tools/fuzz.rb)
#   rake fuzz:sabiruby[seed,points] -- a part of it resumed on SabiRuby
#
# vendor/mruby (the submodule) is never modified. Its tree is copied into
# build/src/<variant> and patches/*.patch are applied to the copy.

require 'fileutils'
require 'digest'

ROOT = __dir__
VENDOR = File.join(ROOT, 'vendor/mruby')
BUILD = File.join(ROOT, 'build')
CONFIG = File.join(ROOT, 'build_config/checkpoint.rb')
PATCHES = Dir[File.join(ROOT, 'patches/*.patch')].sort

# mruby's compiler needs Prism, a nested submodule of mruby. It is not
# checked out inside vendor/mruby (which stays untouched); a clone at the
# pinned commit lives in build/prism. PRISM_SRC may point at a local clone
# to avoid the network.
PRISM_PATH = 'mrbgems/mruby-compiler/lib/prism'

def prism_dir
  dir = File.join(BUILD, 'prism')
  commit = `git -C #{VENDOR} ls-tree HEAD #{PRISM_PATH}`.split[2]
  unless File.exist?(File.join(dir, '.git')) &&
         `git -C #{dir} rev-parse HEAD`.strip == commit
    FileUtils.rm_rf(dir)
    src = ENV['PRISM_SRC'] || 'https://github.com/ruby/prism'
    sh "git clone -q --no-checkout #{src} #{dir}"
    sh "git -C #{dir} checkout -q #{commit}"
  end
  dir
end

def prepare_source(variant, patches)
  dst = File.join(BUILD, 'src', variant)
  stamp = File.join(dst, '.checkpoint-stamp')
  key = Digest::SHA256.hexdigest(
    `git -C #{VENDOR} rev-parse HEAD` + patches.map { |p| File.read(p) }.join)
  return dst if File.exist?(stamp) && File.read(stamp) == key
  FileUtils.rm_rf(dst)
  FileUtils.mkdir_p(dst)
  sh "tar -C #{VENDOR} --exclude=.git --exclude=./build --exclude=./bin -cf - . | tar -C #{dst} -xf -"
  prism = File.join(dst, PRISM_PATH)
  FileUtils.mkdir_p(prism)
  sh "tar -C #{prism_dir} --exclude=.git -cf - . | tar -C #{prism} -xf -"
  patches.each do |p|
    sh "patch -d #{dst} -p1 --no-backup-if-mismatch < #{p}"
  end
  File.write(stamp, key)
  dst
end

def mruby_build(src, build_dir, env = {})
  env = { 'MRUBY_CONFIG' => CONFIG, 'MRUBY_BUILD_DIR' => build_dir }.merge(env)
  jobs = ENV['JOBS'] || '8'
  sh env, "rake -f #{src}/Rakefile -j#{jobs} all > #{build_dir}.log 2>&1" do |ok, _|
    unless ok
      sh "tail -40 #{build_dir}.log"
      abort "mruby build failed (see #{build_dir}.log)"
    end
  end
end

desc 'build patched mruby with mruby-checkpoint (64-bit and 32-bit)'
task :build do
  FileUtils.mkdir_p(BUILD)
  src = prepare_source('patched', PATCHES)
  mruby_build(src, File.join(BUILD, 'mruby'))
  %w(host host32).each do |t|
    bin = File.join(BUILD, 'mruby', t, 'bin', 'checkpoint')
    abort "missing #{bin}" unless File.exist?(bin)
    puts "#{t}: #{`file -b #{bin}`.split(',')[0..1].join(',')}"
  end
end

FUZZ_BUILD = File.join(BUILD, 'fuzz')

desc 'build the test builds with the external trigger (build/fuzz, 64-bit and 32-bit)'
task 'build:fuzz' do
  FileUtils.mkdir_p(BUILD)
  src = prepare_source('patched', PATCHES)
  # mruby-task's POSIX timer left in the archive by a build from before the
  # virtual clock (hal-task-virtual replaces it): the archive is made again
  %w(host host32).each do |t|
    lib = File.join(FUZZ_BUILD, t, 'lib', 'libmruby.a')
    FileUtils.rm_f(lib) if File.exist?(lib) && `ar t #{lib}`.split.include?('task_hal.o')
  end
  mruby_build(src, FUZZ_BUILD, 'CHECKPOINT_FUZZ' => '1')
  %w(host host32).each do |t|
    bin = File.join(FUZZ_BUILD, t, 'bin', 'checkpoint-fuzz')
    abort "missing #{bin}" unless File.exist?(bin)
  end
end

desc 'run the samples with and without checkpoints, and the short fuzz of the test suite'
task :test => [:build, 'build:fuzz'] do
  ruby File.join(ROOT, 'tools/run_tests.rb')
  # the short version: one fixed seed, about 30 points per test file, as the
  # top-level program and as a Task (the start of a Family mruby application)
  ruby File.join(ROOT, 'tools/fuzz.rb'), '--seeds', '1', '--points', '30', '--quiet'
  ruby File.join(ROOT, 'tools/fuzz.rb'), '--seeds', '1', '--points', '30', '--quiet', '--as-task'
end

desc 'fuzz the test suite: rake fuzz[seed,runs,points] (defaults 1, 10, 200); FUZZ_ARGS="--as-task --quantum 300" adds options'
task :fuzz, [:seed, :runs, :points] => 'build:fuzz' do |_, a|
  args = ['--seed-from', (a[:seed] || 1).to_s, '--runs', (a[:runs] || 10).to_s,
          '--points', (a[:points] || 200).to_s, '--isolate']
  args += ENV['FUZZ_ARGS'].split if ENV['FUZZ_ARGS']
  args += ['--json', ENV['FUZZ_JSON']] if ENV['FUZZ_JSON']
  args += ['--table', ENV['FUZZ_TABLE']] if ENV['FUZZ_TABLE']
  ruby File.join(ROOT, 'tools/fuzz.rb'), *args
end

namespace :fuzz do
  desc 'resume a part of the fuzzed test suite on SabiRuby: rake fuzz:sabiruby[seed,points]'
  task :sabiruby, [:seed, :points] => ['build:fuzz', :sabiruby] do |_, a|
    ruby File.join(ROOT, 'tools/fuzz_sabiruby.rb'), '--seed', (a[:seed] || 1).to_s,
         '--points', (a[:points] || 10).to_s
  end
end

namespace :test do
  { asan: 'asan', stress: 'asan-stress' }.each do |t, name|
    desc "rake test with the 64-bit driver built as #{name}"
    task t => :build do
      sh 'sh', File.join(ROOT, 'tools/asan.sh'), *(t == :stress ? ['stress'] : [])
      bin = File.join(BUILD, name, 'host', 'bin', 'checkpoint')
      sh({ 'CHECKPOINT_BIN64' => bin }, 'ruby', File.join(ROOT, 'tools/run_tests.rb'))
    end
  end
end

desc 'build sabiruby (feature checkpoint) in SABIRUBY_DIR for the resume tests'
task :sabiruby do
  dir = ENV['SABIRUBY_DIR'] or abort 'set SABIRUBY_DIR to a SabiRuby checkout'
  sh "cargo build --release -p sabiruby-cli --features checkpoint --manifest-path #{File.join(dir, 'Cargo.toml')}"
end

desc 'benchmark: unpatched vs patched vs patched + mruby-checkpoint (64-bit)'
task :bench do
  FileUtils.mkdir_p(BUILD)
  orig = prepare_source('orig', [])
  patched = prepare_source('patched', PATCHES)
  only64 = { 'CHECKPOINT_TARGETS' => 'host' }
  mruby_build(orig, File.join(BUILD, 'bench-orig'), only64.merge('CHECKPOINT_GEM' => 'no'))
  mruby_build(patched, File.join(BUILD, 'bench-patched'), only64.merge('CHECKPOINT_GEM' => 'no'))
  mruby_build(patched, File.join(BUILD, 'bench-checkpoint'), only64)
  ruby File.join(ROOT, 'tools/bench.rb'), BUILD
end

task :default => :build
