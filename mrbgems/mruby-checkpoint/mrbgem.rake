MRuby::Gem::Specification.new('mruby-checkpoint') do |spec|
  spec.license = 'MIT'
  spec.authors = 'mruby-checkpoint developers'
  spec.summary = 'carry a running VM state to another mrb_state (format v0)'
  spec.bins = %w(checkpoint)
  spec.add_dependency 'mruby-task', core: 'mruby-task'
  spec.add_dependency 'mruby-compiler', core: 'mruby-compiler'
  spec.add_dependency 'mruby-error', core: 'mruby-error'
end
