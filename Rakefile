# mruby-ghost: build, test and benchmark entry points.
#
#   rake build   -- patched mruby + mruby-ghost, 64-bit (host) and 32-bit (host32)
#   rake test    -- run test/samples with and without a hop, all combinations
#   rake bench   -- benchmark: unpatched / patched / patched + mruby-ghost
#
# vendor/mruby (the submodule) is never modified. Its tree is copied into
# build/src/<variant> and patches/*.patch are applied to the copy.

require 'fileutils'
require 'digest'

ROOT = __dir__
VENDOR = File.join(ROOT, 'vendor/mruby')
BUILD = File.join(ROOT, 'build')
CONFIG = File.join(ROOT, 'build_config/ghost.rb')
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
  stamp = File.join(dst, '.ghost-stamp')
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

desc 'build patched mruby with mruby-ghost (64-bit and 32-bit)'
task :build do
  FileUtils.mkdir_p(BUILD)
  src = prepare_source('patched', PATCHES)
  mruby_build(src, File.join(BUILD, 'mruby'))
  %w(host host32).each do |t|
    bin = File.join(BUILD, 'mruby', t, 'bin', 'ghost')
    abort "missing #{bin}" unless File.exist?(bin)
    puts "#{t}: #{`file -b #{bin}`.split(',')[0..1].join(',')}"
  end
end

desc 'run the samples with and without a hop'
task :test => :build do
  ruby File.join(ROOT, 'tools/run_tests.rb')
end

desc 'benchmark: unpatched vs patched vs patched + mruby-ghost (64-bit)'
task :bench do
  FileUtils.mkdir_p(BUILD)
  orig = prepare_source('orig', [])
  patched = prepare_source('patched', PATCHES)
  only64 = { 'GHOST_TARGETS' => 'host' }
  mruby_build(orig, File.join(BUILD, 'bench-orig'), only64.merge('GHOST_GEM' => 'no'))
  mruby_build(patched, File.join(BUILD, 'bench-patched'), only64.merge('GHOST_GEM' => 'no'))
  mruby_build(patched, File.join(BUILD, 'bench-ghost'), only64)
  ruby File.join(ROOT, 'tools/bench.rb'), BUILD
end

task :default => :build
