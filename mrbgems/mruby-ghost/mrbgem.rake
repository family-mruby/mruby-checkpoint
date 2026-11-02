MRuby::Gem::Specification.new('mruby-ghost') do |spec|
  spec.license = 'MIT'
  spec.authors = 'mruby-ghost developers'
  spec.summary = 'carry a running VM state to another mrb_state (format v0)'
  spec.bins = %w(ghost)
  spec.add_dependency 'mruby-task', core: 'mruby-task'
  spec.add_dependency 'mruby-compiler', core: 'mruby-compiler'
  spec.add_dependency 'mruby-error', core: 'mruby-error'
end
