MRuby::Gem::Specification.new('mruby-checkpoint') do |spec|
  spec.license = 'MIT'
  spec.authors = 'mruby-checkpoint developers'
  spec.summary = 'carry a running VM state to another mrb_state (format v1)'
  # the driver (tools/checkpoint) is this repository's: an embedder that
  # copies only the gem (include/, src/, mrbgem.rake) gets no executable
  spec.bins = %w(checkpoint) if File.directory?("#{dir}/tools/checkpoint")
  spec.add_dependency 'mruby-task', core: 'mruby-task'
  spec.add_dependency 'mruby-compiler', core: 'mruby-compiler'
  spec.add_dependency 'mruby-error', core: 'mruby-error'
end
