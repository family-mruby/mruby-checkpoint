# The shape of the inspect JSON (doc/inspect_json.md) as data, and a check
# of a doc against it. The tests check every image of the samples with it;
# a GUI can use it to see that it reads the version it was written for.
#
#   CheckpointSchema.check(doc)            # => [] or ["objects[3].label: ...", ...]
#   CheckpointSchema.check_diff(diff)
#
# Types: Integer, String, Float, :bool, :value (a value, doc/inspect_json.md),
# :int (a JSON number, or {"int": "digits"} beyond 2^53), :float ({"float": x}),
# [:opt, T] (T or null), [:array, T], [:tuple, T1, T2, ...], [:enum, a, ...],
# { 'key' => T, 'key?' => T (may be absent) }: no other keys are allowed.

module CheckpointSchema
  OPT = ->(t) { [:opt, t] }
  ARR = ->(t) { [:array, t] }
  REF = [:opt, Integer]
  IVARS = [:array, [:tuple, [:opt, String], :value]]
  BYTES = { 'text?' => String, 'hex?' => String }
  VIS = { 'default' => [:enum, 'public', 'private', 'protected', 0, 1, 2, 3], 'separated' => :bool, 'module_function' => :bool }
  METHOD = { 'name' => [:opt, String], 'visibility' => [:enum, 'public', 'private', 'protected'],
             'kind' => [:enum, 'proc', 'cfunc', 'undef'], 'proc?' => REF, 'cfunc?' => String, 'aspec?' => Integer }
  REFS = [:array, [:tuple, String, Integer]]
  FLOAT = { 'float' => [:either, Float, Integer, String] }

  COMMON = { 'id' => Integer, 'kind' => String, 'kind_code' => Integer, 'bytes' => Integer,
             'label' => String, 'refs' => REFS, 'path' => String, 'temporary' => :bool, 'class_name?' => String }
  HEAD = { 'class' => REF, 'frozen' => :bool }
  OBJECT = {
    'permanent' => { 'name' => String },
    'permanent_proc' => { 'name' => String, 'irep' => Integer },
    'object' => HEAD.merge('ivars' => IVARS),
    'string' => HEAD.merge(BYTES).merge('binary' => :bool),
    'array' => HEAD.merge('items' => ARR.(:value)),
    'struct' => HEAD.merge('items' => ARR.(:value), 'ivars' => IVARS),
    'hash' => HEAD.merge('default' => { 'has_default' => :bool, 'proc' => :bool }, 'pairs' => ARR.([:tuple, :value, :value]), 'ivars' => IVARS),
    'range' => HEAD.merge('begin' => :value, 'end' => :value, 'exclusive' => :bool, 'uninitialized' => :bool),
    'exception' => HEAD.merge('message' => :value, 'backtrace' => :value, 'ivars' => IVARS),
    'class' => :class, 'module' => :class, 'singleton_class' => :class,
    'iclass' => HEAD.merge('superclass' => REF),
    'proc' => HEAD.merge('lambda' => :bool, 'orphan' => :bool, 'scope' => :bool,
                         'body' => { 'kind' => [:enum, 'irep', 'cfunc', 'alias'], 'irep?' => Integer, 'name?' => String,
                                     'aspec?' => [:opt, Integer], 'method?' => [:opt, String] },
                         'upper' => REF, 'env' => [:opt, { 'kind' => [:enum, 'env', 'class'], 'ref' => REF }]),
    'env' => HEAD.merge('method' => [:opt, String], 'length' => Integer, 'block_reg' => Integer, 'visibility' => VIS,
                        'on_stack' => :bool, 'context?' => REF, 'position?' => Integer, 'values?' => ARR.(:value)),
    'data' => HEAD.merge('type' => String, 'data_hex' => String, 'decoded?' => Hash, 'ivars' => IVARS),
    'bigint' => HEAD.merge('negative' => :bool, 'hex' => String, 'value' => String),
    'set' => HEAD.merge('uninitialized' => :bool, 'items?' => ARR.(:value), 'ivars' => IVARS),
    'rational' => HEAD.merge('numerator' => String, 'denominator' => String),
    'complex' => HEAD.merge('real' => FLOAT, 'imaginary' => FLOAT),
    'break' => HEAD.merge('tag' => [:enum, 'break', 'jump', 'stop'], 'frame' => Integer, 'value' => :value),
    'random' => HEAD.merge('state' => String, 'seed' => Integer, 'ivars' => IVARS),
    'origin' => HEAD.merge('superclass' => REF, 'has_methods' => :bool, 'methods' => ARR.(METHOD)),
    'task' => HEAD.merge('task' => REF, 'ivars' => IVARS),
    'fiber' => HEAD.merge('context' => REF),
  }
  CLASS = HEAD.merge('superclass' => REF, 'instance_type' => String,
                     'flags' => { 'no_allocate' => :bool, 'inherited' => :bool, 'prepended' => :bool, 'has_method_table' => :bool },
                     'ivars' => IVARS, 'methods' => ARR.(METHOD))

  IREP = { 'id' => Integer, 'kind' => [:enum, 'carried', 'permanent', 'permanent_carried'], 'kind_code' => Integer,
           'hash' => String, 'bytes' => Integer, 'file' => [:opt, String], 'line' => [:opt, Integer], 'hash_ok?' => :bool,
           'nlocals?' => Integer, 'nregs?' => Integer, 'ilen?' => Integer,
           'catch?' => ARR.({ 'type' => [:either, String, Integer], 'begin' => Integer, 'end' => Integer, 'target' => Integer }),
           'pool?' => ARR.(Hash), 'syms?' => ARR.([:opt, String]), 'children?' => ARR.(REF), 'locals?' => ARR.([:opt, String]),
           'iseq?' => String }

  CHANGE = { 'kind' => [:enum, 'define_method', 'remove_method', 'set_variable', 'remove_variable', 'set_superclass',
                        'set_global', 'remove_global', 'random_state', 'class_flags', 'contents'],
             'kind_code' => Integer, 'target?' => REF, 'method?' => METHOD, 'name?' => [:opt, String], 'value?' => :value,
             'superclass?' => REF, 'state?' => String, 'seed?' => Integer, 'prepended?' => :bool, 'frozen?' => :bool,
             'type?' => [:enum, 'string', 'array', 'hash'], 'text?' => String, 'hex?' => String,
             'items?' => ARR.(:value), 'pairs?' => ARR.([:tuple, :value, :value]) }

  VAR = { 'name' => [:opt, String], 'value' => :value }
  FRAME = { 'index' => Integer, 'kind' => [:enum, 'ruby', 'cfunc'], 'method' => [:opt, String], 'proc' => REF,
            'cfunc' => [:opt, String], 'reg_base' => Integer, 'pc' => [:opt, Integer], 'argc' => Integer, 'kwargc' => Integer,
            'cci' => Integer, 'visibility' => VIS, 'block' => REF, 'env' => [:opt, { 'kind' => [:enum, 'env', 'class'], 'ref' => REF }],
            'irep' => REF, 'file' => [:opt, String], 'line' => [:opt, Integer], 'location' => [:opt, String], 'key' => String,
            'self' => :value, 'locals' => ARR.({ 'name' => [:opt, String], 'reg' => Integer, 'value' => :value }),
            'captured' => ARR.({ 'env' => Integer, 'method' => [:opt, String], 'on_stack' => :bool, 'context?' => Integer,
                                 'frame?' => [:opt, Integer], 'variables' => ARR.(VAR) }) }
  CONTEXT = { 'id' => Integer, 'state' => [:enum, 'created', 'running', 'resumed', 'suspended', 'transferred', 'terminated'],
              'state_code' => Integer, 'vmexec' => :bool, 'prev' => REF, 'fiber' => REF, 'stack' => ARR.(:value),
              'frames' => ARR.(FRAME), 'role' => [:enum, 'root', 'task', 'fiber', 'unknown'], 'task' => REF,
              'fiber_objects' => ARR.(Integer), 'current' => :bool, 'key' => String, 'backtrace' => ARR.(String) }
  TASK = { 'id' => Integer, 'object' => REF, 'context' => REF,
           'state' => [:enum, 'DORMANT', 'READY', 'RUNNING', 'WAITING', 'SUSPENDED'], 'state_code' => Integer,
           'queue' => [:enum, 'dormant', 'ready', 'waiting', 'suspended', nil], 'priority' => Integer, 'timeslice' => Integer,
           'name' => :value, 'value' => :value,
           'wait' => [:opt, { 'reason' => [:enum, 'sleep', 'join', 'queue'], 'ticks_left?' => [:opt, Integer], 'task?' => REF, 'queue?' => REF }],
           'main' => :bool, 'name_text' => [:opt, String], 'running' => :bool, 'host' => :bool, 'queue_index' => [:opt, Integer], 'key' => String }
  SCHED = { 'switching' => :bool, 'loop' => :bool, 'tick_ms' => Integer, 'tick' => Integer, 'next_wakeup' => [:opt, Integer],
            'tasks' => ARR.(TASK), 'running_task' => REF, 'host_task' => REF }
  SECTION = { 'tag' => String, 'offset' => Integer,
              # DBG0
              'ireps?' => ARR.({ 'irep' => Integer, 'files' => ARR.({ 'file' => String, 'start' => Integer, 'end' => Integer,
                                                                        'lines' => ARR.([:tuple, Integer, Integer]) }) }),
              # GCS0
              'disabled?' => :bool, 'generational?' => :bool, 'step_per_allocation?' => :bool, 'scheduler_steps?' => :bool,
              'interval_ratio?' => Integer, 'step_ratio?' => Integer, 'step_limit?' => String, 'malloc_threshold?' => String,
              'debt_limit?' => String,
              # a section this reader does not know
              'unknown?' => :bool, 'bytes?' => Integer, 'head_hex?' => String, 'tail_hex?' => [:opt, String] }
  ROOT = { 'key' => String, 'kind' => [:enum, 'global', 'variable', 'method', 'superclass', 'contents', 'frame', 'task', 'fiber'],
           'refs' => REFS, 'temps?' => REFS, 'context?' => Integer, 'frame?' => Integer, 'task?' => Integer }
  DOC = {
    'inspect_json' => [:enum, 1], 'source' => { 'file' => [:opt, String], 'bytes' => Integer },
    'header' => { 'magic' => String, 'format' => Integer, 'implementation' => String, 'bytecode' => String,
                  'integer_bits' => Integer, 'float_bits' => Integer, 'utf8' => :bool, 'pointer_bits' => Integer },
    'symbols' => ARR.(String), 'ireps' => ARR.(IREP), 'objects' => ARR.(:object), 'changes' => ARR.(CHANGE),
    'contexts' => ARR.(CONTEXT), 'current_context' => Integer, 'scheduler' => SCHED, 'sections' => ARR.(SECTION),
    'warnings' => ARR.(String), 'error' => [:opt, { 'offset' => [:opt, Integer], 'where' => String, 'message' => String }],
    'roots' => ARR.(ROOT), 'omitted?' => ARR.(String),
  }

  FIELD = { 'field' => String, 'a' => :any, 'b' => :any }
  DIFF = {
    'inspect_diff' => [:enum, 1], 'a' => Hash, 'b' => Hash, 'same' => :bool,
    'header' => ARR.(FIELD), 'variables' => ARR.({ 'key' => String, 'a' => [:opt, String], 'b' => [:opt, String] }),
    'contexts' => ARR.({ 'key' => String, 'change' => [:enum, 'same', 'changed', 'added', 'removed'], 'a?' => Integer, 'b?' => Integer,
                         'fields?' => ARR.(FIELD),
                         'frames' => ARR.({ 'index' => Integer, 'change' => [:enum, 'same', 'changed', 'called', 'returned'],
                                            'method?' => [:opt, String], 'a?' => String, 'b?' => String, 'pc?' => ARR.([:opt, Integer]),
                                            'self?' => ARR.(String),
                                            'locals?' => ARR.({ 'name' => String, 'a' => [:opt, String], 'b' => [:opt, String], 'contents_changed?' => :bool }) }) }),
    'tasks' => ARR.({ 'key' => String, 'change' => [:enum, 'changed', 'added', 'removed'], 'a?' => Integer, 'b?' => Integer, 'fields?' => ARR.(FIELD) }),
    'scheduler' => ARR.(FIELD),
    'objects' => {
      'changed' => ARR.({ 'path' => String, 'a' => Integer, 'b' => Integer, 'kind' => String, 'label_a' => String, 'label_b' => String,
                          'temporary' => :bool, 'fields' => ARR.(FIELD) }),
      'added' => ARR.({ 'path' => String, 'b' => Integer, 'label' => String, 'temporary' => :bool }),
      'removed' => ARR.({ 'path' => String, 'a' => Integer, 'label' => String, 'temporary' => :bool }),
      'moved' => ARR.({ 'a_path' => String, 'b_path' => String, 'a' => Integer, 'b' => Integer, 'label' => String, 'temporary' => :bool }),
    },
  }

  module_function

  def check(doc) = walk(doc, DOC, 'doc', [])
  def check_diff(d) = walk(d, DIFF, 'diff', [])

  def value?(v)
    case v
    when nil, true, false, Integer then true
    when Hash
      return false unless v.size == 1
      k, x = v.first
      case k
      when 'ref' then x.is_a?(Integer) && x.positive?
      when 'sym' then x.nil? || x.is_a?(String)
      when 'int' then x.is_a?(String) && x.match?(/\A-?\d+\z/)
      when 'float' then x.is_a?(Float) || x.is_a?(Integer) || %w(NaN Infinity -Infinity).include?(x)
      when 'undef' then x == true
      else false
      end
    else false
    end
  end

  def walk(x, t, at, errs)
    return errs if errs.size > 50
    case t
    when :any then nil
    when :bool then errs << "#{at}: #{x.inspect} is not true or false" unless x == true || x == false
    when :value then errs << "#{at}: #{x.inspect} is not a value" unless value?(x)
    when :object then object(x, at, errs)
    when Class then errs << "#{at}: #{x.inspect[0, 60]} is not #{t}" unless x.is_a?(t)
    when Array
      case t[0]
      when :opt then walk(x, t[1], at, errs) unless x.nil?
      when :either then errs << "#{at}: #{x.inspect[0, 60]} is none of #{t[1..].join(', ')}" unless t[1..].any? { |c| x.is_a?(c) }
      when :enum then errs << "#{at}: #{x.inspect} is not one of #{t[1..].inspect}" unless t[1..].include?(x)
      when :array
        return errs << "#{at}: not an array" unless x.is_a?(Array)
        x.each_with_index { |e, i| walk(e, t[1], "#{at}[#{i}]", errs) }
      when :tuple
        return errs << "#{at}: not a #{t.size - 1}-tuple" unless x.is_a?(Array) && x.size == t.size - 1
        t[1..].each_with_index { |c, i| walk(x[i], c, "#{at}[#{i}]", errs) }
      end
    when Hash
      return errs << "#{at}: not an object" unless x.is_a?(Hash)
      keys = {}
      t.each do |k, c|
        opt = k.end_with?('?')
        name = opt ? k[0..-2] : k
        keys[name] = true
        if x.key?(name) then walk(x[name], c, "#{at}.#{name}", errs)
        elsif !opt then errs << "#{at}: no #{name}"
        end
      end
      (x.keys - keys.keys).each { |k| errs << "#{at}: unexpected #{k}" }
    end
    errs
  end

  def object(o, at, errs)
    return errs << "#{at}: not an object" unless o.is_a?(Hash)
    body = OBJECT[o['kind']] or return errs << "#{at}: unknown kind #{o['kind'].inspect}"
    body = CLASS if body == :class
    walk(o, COMMON.merge(body), "#{at}(#{o['kind']})", errs)
  end
end
