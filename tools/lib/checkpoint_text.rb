# Text views of the inspect JSON (tools/checkpoint_inspect.rb): the whole
# image record by record, the call stacks, the object graph, the Tasks.
# Each takes the doc made by CheckpointImage.read and returns a String.

require_relative 'checkpoint_image'

module CheckpointText
  module_function

  def v(doc, x, max = 60) = CheckpointImage.value_text(doc, x, max)
  def ref(id) = id ? "##{id}" : '-'
  def sym(s) = s ? ":#{s}" : '-'
  def ivars(doc, list) = (list || []).map { |n, x| "#{sym(n)}=#{v(doc, x)}" }.join(' ')
  def bytes(o) = o['text'] ? o['text'].inspect : "<hex #{o['hex']}>"

  def method_text(m)
    case m['kind']
    when 'proc' then "#{sym(m['name'])}(#{m['visibility']})->#{ref(m['proc'])}"
    when 'cfunc' then "#{sym(m['name'])}(#{m['visibility']})->C #{m['cfunc']} aspec=#{m['aspec'].to_s(16)}"
    else "#{sym(m['name'])} undef"
    end
  end

  # ---- everything, record by record ----

  def dump(doc, iseq: false)
    o = +''
    h = doc['header']
    o << "format v#{h['format']}, #{h['implementation']}, #{h['bytecode']}, Integer #{h['integer_bits']} bit, " \
         "Float #{h['float_bits']} bit, utf8=#{h['utf8'] ? 1 : 0}, writer #{h['pointer_bits']}-bit\n"
    o << "symbols: #{doc['symbols'].size}\n"
    o << "ireps: #{doc['ireps'].size}\n"
    doc['ireps'].each do |ir|
      if ir['kind'] == 'permanent'
        o << "  @#{ir['id']} permanent #{ir['hash']}\n"
        next
      end
      tag = ir['kind'] == 'permanent_carried' ? 'permanent, carried ' : ''
      pool = ir['pool'].map do |p|
        if p['string'] then bytes(p['string'])
        elsif p.key?('int') then p['int'].is_a?(Hash) ? p['int']['int'] : p['int'].to_s
        elsif p.key?('float') then CheckpointImage.float_text(p)
        else "bigint(#{p['bigint_hex'].size / 2})"
        end
      end
      o << "  @#{ir['id']} #{tag}#{ir['hash']}#{ir['hash_ok'] ? '' : ' (HASH MISMATCH)'} locals=#{ir['nlocals']} " \
           "regs=#{ir['nregs']} iseq=#{ir['ilen']}B catch=#{ir['catch'].size} pool=[#{pool.join(', ')}] " \
           "syms=#{ir['syms'].map { |s| sym(s) }.join(',')} reps=#{ir['children'].map { |c| "@#{c || 0}" }.join(',')} " \
           "lv=#{ir['locals'].map { |s| sym(s) }.join(',')}#{ir['file'] ? " #{ir['file']}:#{ir['line']}" : ''}\n"
      o << "     #{ir['iseq']}\n" if iseq && ir['iseq']
    end
    o << "objects: #{doc['objects'].size}\n"
    doc['objects'].each { |ob| o << "  #{object_line(doc, ob)}\n" }
    o << "changes: #{doc['changes'].size}\n"
    doc['changes'].each { |c| o << "  #{change_line(doc, c)}\n" }
    o << "contexts: #{doc['contexts'].size}\n"
    doc['contexts'].each do |c|
      o << "  context #{c['id']}: #{c['state']}#{c['vmexec'] ? ' vmexec' : ''} prev=#{c['prev'] || 0} " \
           "fiber=#{ref(c['fiber'])} stack=#{c['stack'].size} (#{c['key']})\n"
      c['stack'].each_slice(8).with_index { |s, i| o << "    [#{i * 8}] #{s.map { |x| v(doc, x, 20) }.join(' ')}\n" }
      c['frames'].each do |f|
        o << "    #{f['index']}: #{sym(f['method'])}#{f['cfunc'] ? " C #{f['cfunc']}" : ''} proc=#{ref(f['proc'])} " \
             "R0=stack[#{f['reg_base']}] pc=#{f['pc'] || '-'} n=#{f['argc']} nk=#{f['kwargc']} cci=#{f['cci']} " \
             "blk=#{ref(f['block'])} #{f['env'] ? "#{f['env']['kind']}=##{f['env']['ref']}" : 'none'}" \
             "#{f['file'] ? " #{f['file']}:#{f['line']}" : ''}\n"
      end
    end
    o << "current context: #{doc['current_context']}\n"
    s = doc['scheduler']
    o << "scheduler: switch=#{s['switching'] ? 1 : 0} loop=#{s['loop'] ? 1 : 0} tick=#{s['tick']} (#{s['tick_ms']} ms) " \
         "next wakeup #{s['next_wakeup'] ? "in #{s['next_wakeup']}" : 'none'}\n"
    s['tasks'].each { |t| o << "  Task #{t['id']}: #{task_line(doc, t)}\n" }
    o << "running Task: #{s['running_task'] || 0}, host Task: #{s['host_task'] || 0}\n"
    doc['sections'].each do |sec|
      case sec['tag']
      when 'DBG0'
        o << "line numbers: #{sec['ireps'].size} ireps\n"
        sec['ireps'].each do |e|
          o << "  @#{e['irep']} #{e['files'].map { |f| "#{f['file']}[#{f['start']}...#{f['end']}] #{f['lines'].size} changes" }.join(', ')}\n"
        end
      when 'SRC0'
        o << "sources: #{sec['files'].map { |f| "#{f['file']} (#{f['bytes']} bytes)" }.join(', ')}\n"
      when 'GCS0'
        o << "GC settings: #{sec.reject { |k, _| %w(tag offset).include?(k) }.map { |k, x| "#{k}=#{x}" }.join(' ')}\n"
      else
        o << "unknown section #{sec['tag']} at byte #{sec['offset']}: #{sec['length']} bytes, skipped, " \
             "starting #{sec['head_hex']}\n"
      end
    end
    doc['warnings'].each { |w| o << "warning: #{w}\n" }
    o << "end\n"
  end

  def object_line(doc, ob)
    head = "##{ob['id']}"
    case ob['kind']
    when 'permanent' then return "#{head} = #{ob['name']}"
    when 'permanent_proc' then return "#{head} = #{ob['name']} (runs irep @#{ob['irep']})"
    end
    head = "#{head} #{ob['kind']} class=#{ref(ob['class'])}#{ob['frozen'] ? ' frozen' : ''}"
    body =
      case ob['kind']
      when 'object', 'random', 'data', 'task'
        extra = case ob['kind']
                when 'random' then "state=#{ob['state']} seed=#{ob['seed']} "
                when 'data' then "#{ob['type'].empty? ? '(not initialized)' : ob['type']} #{ob['data_hex']} "
                when 'task' then "task=#{ob['task'] || 0} "
                else ''
                end
        extra + ivars(doc, ob['ivars'])
      when 'string' then "#{bytes(ob)}#{ob['binary'] ? ' (binary)' : ''}"
      when 'array' then "[#{ob['items'].map { |x| v(doc, x, 30) }.join(', ')}]"
      when 'struct' then "[#{ob['items'].map { |x| v(doc, x, 30) }.join(', ')}] #{ivars(doc, ob['ivars'])}"
      when 'hash'
        "{#{ob['pairs'].map { |k, x| "#{v(doc, k, 30)}=>#{v(doc, x, 30)}" }.join(', ')}} " \
          "default=#{ob['default'].select { |_, b| b }.keys.join(',')} #{ivars(doc, ob['ivars'])}"
      when 'range' then "#{v(doc, ob['begin'])}#{ob['exclusive'] ? '...' : '..'}#{v(doc, ob['end'])}#{ob['uninitialized'] ? ' (not initialized)' : ''}"
      when 'exception' then "mesg=#{v(doc, ob['message'])} bt=#{v(doc, ob['backtrace'])} #{ivars(doc, ob['ivars'])}"
      when 'class', 'module', 'singleton_class'
        "#{ob['class_name']} super=#{ref(ob['superclass'])} inst=#{ob['instance_type']} " \
          "#{ob['flags'].select { |_, b| b }.keys.join(',')} #{ivars(doc, ob['ivars'])} methods: #{ob['methods'].map { |m| method_text(m) }.join(' ')}"
      when 'iclass' then "super=#{ref(ob['superclass'])}"
      when 'origin' then "super=#{ref(ob['superclass'])} methods: #{ob['methods'].map { |m| method_text(m) }.join(' ')}"
      when 'proc'
        b = ob['body']
        bt = case b['kind']
             when 'irep' then "irep @#{b['irep']}"
             when 'cfunc' then "C #{b['name']}"
             else "alias of #{sym(b['method'])}"
             end
        fl = %w(lambda orphan scope).select { |k| ob[k] }.join(',')
        "#{fl} #{bt} upper=#{ref(ob['upper'])} #{ob['env'] ? "#{ob['env']['kind']}=##{ob['env']['ref']}" : 'none'}"
      when 'env'
        base = "mid=#{sym(ob['method'])} len=#{ob['length']} bidx=#{ob['block_reg']}"
        ob['on_stack'] ? "#{base} on the stack of context #{ob['context']} at #{ob['position']}" : "#{base} [#{ob['values'].map { |x| v(doc, x, 30) }.join(', ')}]"
      when 'bigint' then ob['value']
      when 'set' then ob['uninitialized'] ? "(not initialized) #{ivars(doc, ob['ivars'])}" : "{#{ob['items'].map { |x| v(doc, x, 30) }.join(', ')}} #{ivars(doc, ob['ivars'])}"
      when 'rational' then "#{ob['numerator']}/#{ob['denominator']}"
      when 'complex' then ob['label']
      when 'break' then "tag=#{ob['tag']} frame=#{ob['frame']} value=#{v(doc, ob['value'])}"
      when 'fiber' then ob['context'] ? "context #{ob['context']}" : '(not initialized)'
      end
    "#{head} #{body}"
  end

  def change_line(doc, c)
    t = c['target'] && doc['objects'][c['target'] - 1]
    tn = t ? (t['kind'] == 'permanent' ? t['name'] : "##{t['id']}") : '-'
    case c['kind']
    when 'define_method' then "def #{tn} #{method_text(c['method'])}"
    when 'remove_method' then "remove method #{tn} #{sym(c['name'])}"
    when 'set_variable' then "set #{tn} #{sym(c['name'])} = #{v(doc, c['value'])}"
    when 'remove_variable' then "remove #{tn} #{sym(c['name'])}"
    when 'set_superclass' then "super of #{tn} = #{ref(c['superclass'])}"
    when 'set_global' then "global #{c['name']} = #{v(doc, c['value'])}"
    when 'remove_global' then "remove global #{c['name']}"
    when 'random_state' then "Random #{tn} state=#{c['state']} seed=#{c['seed']}"
    when 'class_flags' then "class flags #{tn} prepended=#{c['prepended'] ? 1 : 0}"
    when 'contents'
      body = case c['type']
             when 'string' then bytes(c)
             when 'array' then "[#{c['items'].map { |x| v(doc, x, 30) }.join(', ')}]"
             else "{#{c['pairs'].map { |k, x| "#{v(doc, k, 30)}=>#{v(doc, x, 30)}" }.join(', ')}}"
             end
      "contents #{tn} frozen=#{c['frozen'] ? 1 : 0} #{body}"
    end
  end

  def wait_text(doc, t)
    w = t['wait'] or return nil
    case w['reason']
    when 'sleep' then "sleep, #{w['ticks_left']} ticks left"
    when 'join' then "join Task #{w['task'] ? doc['scheduler']['tasks'][w['task'] - 1]['key'] : '-'}"
    when 'queue' then "Task::Queue #{ref(w['queue'])}#{w['ticks_left'] ? ", #{w['ticks_left']} ticks left" : ''}"
    end
  end

  def task_line(doc, t)
    where = t['main'] ? 'main (Task.current at the root)' : "#{t['queue']} queue ##{t['queue_index']}"
    flags = [t['running'] ? 'running' : nil, t['host'] ? 'host' : nil].compact
    w = wait_text(doc, t)
    "#{t['key']} #{t['state']} #{where} priority=#{t['priority']} timeslice=#{t['timeslice']} " \
      "context=#{t['context'] || '-'} object=#{ref(t['object'])}" \
      "#{w ? " #{t['state'] == 'WAITING' ? 'waits' : 'last waited'}: #{w}" : ''}" \
      "#{t['value'] ? " value=#{v(doc, t['value'])}" : ''}#{flags.empty? ? '' : " (#{flags.join(', ')})"}"
  end

  # ---- call stacks, per context, in the order of a backtrace ----

  ACTIVITY = {
    'current' => 'running: the image was written here',
    'not_started' => 'not started',
    'yielded' => 'stopped in Fiber.yield',
    'resuming' => 'waits for the Fiber it resumed',
    'transferring' => 'transferred to another Fiber',
    'finished' => 'finished',
    'scheduler' => 'runs the Task scheduler from a C function',
    'no_frames' => 'no frames (the host runs the scheduler)',
    'other' => 'other',
  }.freeze

  def activity_text(doc, c)
    t = c['task'] && doc['scheduler']['tasks'][c['task'] - 1]
    task = t && "Task #{t['state']}#{t['queue'] ? " (#{t['queue']} queue ##{t['queue_index']})" : ''}"
    return task if c['activity'] == 'task'
    [ACTIVITY[c['activity']], task].compact.join(', ')
  end

  def context_title(doc, c)
    t = c['task'] && doc['scheduler']['tasks'][c['task'] - 1]
    bits = [activity_text(doc, c)]
    bits << "resumed by context #{c['prev']}" if c['prev']
    bits << 'the running Task' if t && t['running']
    "context #{c['id']}: #{c['key']} (#{bits.join(', ')})"
  end

  def stack(doc)
    o = +''
    doc['contexts'].each do |c|
      o << "#{context_title(doc, c)}\n"
      if c['frames'].empty?
        o << "  (no frames)\n"
        next
      end
      c['frames'].reverse_each do |f|
        if f['placeholder']
          o << "  (frame #{f['index']}: a copy of frame 0 that stands in for the call of Fiber#resume; popped when it returns)\n"
          next
        end
        loc = f['location'] || "(no line)#{f['method'] ? ":in #{f['method']}" : ''}"
        note = f['kind'] == 'cfunc' ? "   [C function #{f['cfunc']}, waiting]" : ''
        note += '   [not started: starts here]' if f['not_started']
        note += '   [not in a backtrace]' if f['location'].nil? && f['kind'] == 'ruby'
        o << "  #{loc}#{note}\n"
        (CheckpointImage.source_excerpt(doc, f) || []).each do |k, line, here|
          o << "      #{here ? '>' : ' '} #{k.to_s.rjust(4)} | #{line}\n".rstrip << "\n"
        end
        if f['self_register']
          o << "      self = #{v(doc, f['self'])} (from the first resume; until then R0 holds #{v(doc, f['self_register'])})\n"
        elsif f['method'] && !(f['self'].is_a?(Hash) && f['self']['ref'] && doc['objects'][f['self']['ref'] - 1]['name'] == 'T')
          o << "      self = #{v(doc, f['self'])}\n"
        end
        f['locals'].each do |l|
          o << "      #{l['name'] || "r#{l['reg']}"} = #{v(doc, l['value'])}\n"
        end
        f['captured'].each do |lv|
          where = lv['on_stack'] ? "from #{lv['frame'] ? "frame #{lv['frame']} of context #{lv['context']}" : "the stack of context #{lv['context']}"}" : 'kept by the block'
          names = lv['variables'].select { |x| x['name'] }
          next if names.empty?
          if lv['on_stack'] && lv['context'] == c['id']
            o << "      (sees #{names.map { |x| x['name'] }.join(', ')} #{where})\n"
          else
            o << "      (sees, #{where}) #{names.map { |x| "#{x['name']} = #{v(doc, x['value'])}" }.join(', ')}\n"
          end
        end
      end
    end
    o
  end

  # ---- the object graph, from the roots, as indented text ----

  def objects(doc, depth: 2, from: nil)
    o = +''
    objs = doc['objects']
    shown = {}
    walk = lambda do |id, label, d, indent|
      ob = objs[id - 1]
      perm = %w(permanent permanent_proc).include?(ob['kind'])
      line = "#{'  ' * indent}#{label.empty? ? '' : "#{label} = "}#{ob['label']} (##{id})"
      if perm
        o << "#{line} [permanent]\n"
      elsif shown[id]
        o << "#{line} -> shown at #{ob['path']}\n"
      else
        shown[id] = true
        more = ob['refs'].reject { |l, _| l == 'class' }
        o << "#{line}#{d >= depth && !more.empty? ? " … #{more.size} more" : ''}\n"
        more.each { |l, cid| walk.(cid, l, d + 1, indent + 1) } if d < depth
      end
    end
    if from
      id = from.start_with?('#') ? from[1..].to_i : objs.find { |x| x['path'] == from }&.dig('id')
      return "no object #{from}\n" unless id && id.between?(1, objs.size)
      o << "#{objs[id - 1]['path']}\n"
      walk.(id, '', 0, 1)
      return o
    end
    doc['roots'].each do |r|
      next if r['refs'].empty?
      o << "#{r['key']}\n"
      r['refs'].each { |l, id| walk.(id, l, 1, 1) }
    end
    o
  end

  # ---- the Tasks and the contexts ----

  def tasks(doc)
    s = doc['scheduler']
    o = +"scheduler: tick #{s['tick']} (#{s['tick_ms']} ms a tick)#{s['loop'] ? ', the loop runs' : ''}" \
         "#{s['switching'] ? ', a switch was asked for' : ''}, next wakeup #{s['next_wakeup'] ? "in #{s['next_wakeup']} ticks" : 'none'}\n"
    s['tasks'].each { |t| o << "  Task #{t['id']}: #{task_line(doc, t)}\n" }
    o << "contexts:\n"
    doc['contexts'].each do |c|
      top = c['frames'].last
      o << "  #{context_title(doc, c)}#{top ? " at #{top['location'] || '?'}#{top['not_started'] ? ' (not started)' : ''}" : ''}\n"
    end
    o
  end
end
