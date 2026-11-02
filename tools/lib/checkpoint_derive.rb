# The derived fields of the inspect JSON (doc/inspect_json.md): what makes an
# image readable without knowing the format. Run by CheckpointImage::Reader
# after the whole image has been read.
#
#   ireps[]     hash_ok, file, line
#   objects[]   class_name, label, refs (the edges of the graph), path
#   contexts[]  role, task, fiber_objects, key, backtrace
#   frames[]    irep, file, line, location, self, locals, captured
#   tasks[]     name_text, main, running, host, queue_index, key
#   roots       the named starting points of the paths

module CheckpointImage
  # ---- shown values (shared with the diff and the CLI) ----

  def self.perm_label(name)
    case name
    when 'T' then 'main'
    when /\AS:(.*)\z/m then "#<Class:#{perm_label($1)}>"
    when /\AM:(.*)\z/m then "#{$1} (method)"
    when /\AE:(.*)\z/m then "env of #{$1}"
    when /\AV:(.*)@(.*)\z/m then "#{$1}::#{$2}"
    when /\AG:(.*)\z/m then $1
    else name
    end
  end

  def self.float_text(v)
    f = v['float']
    f.is_a?(String) ? f : f.to_s
  end

  # one value as a short text, objects by their label (and number)
  def self.value_text(doc, v, max = 60, number: true)
    case v
    when nil then 'nil'
    when true, false, Integer then v.to_s
    when Hash
      if v.key?('ref')
        o = doc['objects'][v['ref'] - 1]
        l = o['label'] || o['kind']
        l = "#{l[0, max - 1]}…" if l.size > max
        number ? "#{l} (##{v['ref']})" : l
      elsif v.key?('sym') then v['sym'] ? ":#{v['sym']}" : ':?'
      elsif v.key?('int') then v['int']
      elsif v.key?('float') then float_text(v)
      elsif v.key?('undef') then 'undef'
      else v.inspect
      end
    else v.inspect
    end
  end

  def self.scalar?(v) = !(v.is_a?(Hash) && v.key?('ref'))

  # ---- FNV-1a 64 (§6.3) ----

  FNV_OFFSET = 0xcbf29ce484222325
  FNV_PRIME = 0x100000001b3
  MASK64 = 0xffffffffffffffff

  def self.fnv1a(h, bytes)
    bytes.each_byte { |c| h = ((h ^ c) * FNV_PRIME) & MASK64 }
    h
  end

  def self.u64be(x) = [x & MASK64].pack('Q>')

  class Derive
    def initialize(doc, hash_input)
      @doc = doc
      @hash_input = hash_input
      @objs = doc['objects']
      @ireps = doc['ireps']
      @ctxs = doc['contexts']
      @sched = doc['scheduler']
      @tasks = @sched['tasks']
      @lines = {}
      (doc['sections'].find { |s| s['tag'] == 'DBG0' } || { 'ireps' => [] })['ireps'].each do |e|
        @lines[e['irep']] = e['files']
      end
    end

    def run
      check_hashes
      irep_places
      @objs.each { |o| o['class_name'] = class_name(o['id']) if class_like?(o) }
      @objs.each { |o| o['label'] = label(o) }
      tasks
      contexts
      @objs.each { |o| o['refs'] = refs(o) }
      paths
    end

    private

    def obj(id) = @objs[id - 1]
    def str_of(v) = v.is_a?(Hash) && v['ref'] && (o = obj(v['ref']))['kind'] == 'string' ? (o['text'] || "<#{o['hex']}>") : nil

    # -- ireps --

    def check_hashes
      @hash_input.each do |id, (nlocals, nregs, clen, ilen, code, pool, syms, reps, h)|
        x = CheckpointImage::FNV_OFFSET
        u = ->(v) { x = CheckpointImage.fnv1a(x, CheckpointImage.u64be(v)) }
        b = ->(s) { x = CheckpointImage.fnv1a(x, s) }
        [nlocals, nregs, clen, ilen].each(&u)
        b.(code)
        u.(pool.size)
        pool.each do |k, v|
          case k
          when :i then u.('i'.ord); u.(v)
          when :f then u.('f'.ord); u.(v)
          when :b then u.('b'.ord); u.(v.bytesize); b.(v)
          when :s then u.('s'.ord); u.(v.bytesize); b.(v)
          end
        end
        u.(syms.size)
        syms.each { |s| s.nil? ? u.(CheckpointImage::MASK64) : (u.(s.bytesize); b.(s.b)) }
        u.(reps.size)
        reps.each { |c| u.(c ? @ireps[c - 1]['hash'].to_i(16) : 0) }
        @ireps[id - 1]['hash_ok'] = x == h
        @doc['warnings'] << "irep @#{id}: its contents do not hash to #{format('%016x', h)}" unless x == h
      end
    end

    def irep_places
      @ireps.each do |ir|
        files = @lines[ir['id']]
        f = files&.first
        ir['file'] = f && f['file']
        ir['line'] = f && f['lines'].map(&:last).find(&:positive?)
      end
    end

    # mruby's lookup (debug.c): the file whose range holds pc, then the line of
    # the last change at or before pc; nil when mruby finds no line
    def line_at(irep, pc)
      files = @lines[irep] or return nil
      return nil if pc.negative? || pc >= files.last['end']
      f = files.reverse_each.find { |e| e['start'] <= pc } or return nil
      line = 0
      f['lines'].each { |pos, l| break if pc < pos; line = l }
      line.positive? ? [f['file'], line] : nil
    end

    def proc_irep(id)
      return nil unless id
      o = obj(id)
      case o['kind']
      when 'proc' then o['body']['kind'] == 'irep' ? o['body']['irep'] : nil
      when 'permanent_proc' then o['irep']
      end
    end

    # -- labels --

    def class_like?(o) = %w(class module singleton_class iclass origin).include?(o['kind']) ||
                         (o['kind'] == 'permanent' && o['name'] !~ /\A[MEFVGX]:|\AT\z/)

    def ivar(o, name) = (o['ivars'] || []).find { |n, _| n == name }&.last

    def class_name(id, depth = 0)
      return '?' if depth > 20
      o = obj(id)
      case o['kind']
      when 'permanent' then CheckpointImage.perm_label(o['name'])
      when 'class', 'module'
        cn = ivar(o, '__classname__')
        name = cn.is_a?(Hash) && cn['sym'] ? cn['sym'] : str_of(cn)
        return "#<#{o['kind'] == 'class' ? 'Class' : 'Module'}:##{id}>" unless name
        outer = ivar(o, '__outer__')
        if cn.is_a?(Hash) && cn['sym'] && outer.is_a?(Hash) && outer['ref']
          on = class_name(outer['ref'], depth + 1)
          name = "#{on}::#{name}" unless on == 'Object'
        end
        name
      when 'singleton_class'
        at = ivar(o, '__attached__')
        "#<Class:#{at.is_a?(Hash) && at['ref'] ? (class_like?(obj(at['ref'])) ? class_name(at['ref'], depth + 1) : "##{at['ref']}") : '?'}>"
      when 'iclass' then "#{o['class'] ? class_name(o['class'], depth + 1) : '?'} (included)"
      when 'origin' then "#{o['class'] ? class_name(o['class'], depth + 1) : '?'} (origin)"
      else "##{id}"
      end
    end

    def cname(o) = o['class'] ? (obj(o['class'])['class_name'] || class_name(o['class'])) : nil

    def short(text, max = 40) = text.size > max ? "#{text[0, max - 1]}…" : text

    # a short text of values: scalars, Strings and permanent objects only
    def inline_text(v)
      return CheckpointImage.value_text(@doc, v) if CheckpointImage.scalar?(v)
      o = obj(v['ref'])
      case o['kind']
      when 'string' then (o['text'] || "<#{o['hex']}>").inspect
      when 'permanent' then CheckpointImage.perm_label(o['name'])
      end
    end

    def inline(values, open, close, max = 40)
      return nil if values.size > 8
      parts = values.map { |v| inline_text(v) }
      return nil if parts.any?(&:nil?)
      t = "#{open}#{parts.join(', ')}#{close}"
      t.size <= max ? t : nil
    end

    def place(irep)
      ir = irep && @ireps[irep - 1]
      ir && ir['file'] ? "#{ir['file']}:#{ir['line']}" : nil
    end

    def label(o)
      case o['kind']
      when 'permanent' then CheckpointImage.perm_label(o['name'])
      when 'permanent_proc' then CheckpointImage.perm_label(o['name'])
      when 'object' then "#<#{cname(o)}>"
      when 'string' then short((o['text'] || "<#{o['hex']}>").inspect)
      when 'array' then inline(o['items'], '[', ']') || "Array(#{o['items'].size})"
      when 'hash'
        flat = o['pairs'].flatten(1)
        texts = o['pairs'].size <= 8 ? flat.map { |v| inline_text(v) } : [nil]
        if o['pairs'].empty? then '{}'
        elsif texts.none?(&:nil?) && (t = "{#{texts.each_slice(2).map { |k, v| "#{k} => #{v}" }.join(', ')}}").size <= 40 then t
        else "Hash(#{o['pairs'].size})"
        end
      when 'range'
        return 'Range (not initialized)' if o['uninitialized']
        b = inline_text(o['begin'])
        e = inline_text(o['end'])
        b && e ? "#{b}#{o['exclusive'] ? '...' : '..'}#{e}" : 'Range'
      when 'exception' then "#<#{cname(o)}: #{short(str_of(o['message']) || '', 30)}>"
      when 'class' then "class #{o['class_name']}"
      when 'module' then "module #{o['class_name']}"
      when 'singleton_class', 'iclass', 'origin' then o['class_name']
      when 'proc'
        kind = o['lambda'] ? 'lambda' : 'Proc'
        case o['body']['kind']
        when 'irep' then "#{kind} (#{place(o['body']['irep']) || "irep @#{o['body']['irep']}"})"
        when 'cfunc' then "#{kind} (C #{o['body']['name']})"
        else "#{kind} (alias of #{o['body']['method']})"
        end
      when 'env' then "Env of #{o['method'] || 'a block'}#{o['on_stack'] ? ' (on the stack)' : ''}"
      when 'data' then o['type'].empty? ? "#<#{cname(o)} (not initialized)>" : "#<#{cname(o) || o['type']}>"
      when 'bigint' then short(o['value'])
      when 'struct' then "#<struct #{cname(o)}>"
      when 'set' then o['uninitialized'] ? 'Set (not initialized)' : (inline(o['items'], 'Set{', '}') || "Set(#{o['items'].size})")
      when 'rational' then "(#{o['numerator']}/#{o['denominator']})"
      when 'complex' then "(#{CheckpointImage.float_text(o['real'])}+#{CheckpointImage.float_text(o['imaginary'])}i)"
      when 'break' then "#{o['tag']} to frame #{o['frame']}"
      when 'random' then "#<#{cname(o)}>"
      when 'task'
        t = o['task'] && @tasks[o['task'] - 1]
        name = t && (t['queue'].nil? ? 'main' : str_of(t['name']))
        "Task #{name ? name.inspect : "##{o['id']}"}"
      when 'fiber' then o['context'] ? "Fiber (context #{o['context']})" : 'Fiber (not initialized)'
      else o['kind']
      end
    end

    # -- Tasks --

    def tasks
      by_queue = Hash.new(0)
      @tasks.each do |t|
        t['main'] = t['queue'].nil?
        t['name_text'] = t['main'] ? 'main' : str_of(t['name'])
        t['running'] = @sched['running_task'] == t['id']
        t['host'] = @sched['host_task'] == t['id']
        t['queue_index'] = t['queue'] ? by_queue[t['queue']] : nil
        by_queue[t['queue']] += 1 if t['queue']
      end
      # a key that does not depend on the numbering: the name, else where the
      # body of the Task is written
      keys = @tasks.map do |t|
        if t['main'] then 'main'
        elsif t['name_text'] then t['name_text'].inspect
        else
          c = t['context'] && @ctxs[t['context'] - 1]
          ir = c && c['frames'].first && proc_irep(c['frames'].first['proc'])
          "@#{place(ir) || '?'}"
        end
      end
      uniq(keys).each_with_index { |k, i| @tasks[i]['key'] = k }
    end

    def uniq(keys)
      seen = Hash.new(0)
      keys.map { |k| (seen[k] += 1) == 1 ? k : "#{k}##{seen[k]}" }
    end

    # -- contexts and frames --

    def contexts
      task_of = {}
      @tasks.each { |t| task_of[t['context']] = t if t['context'] }
      fibers = Hash.new { |h, k| h[k] = [] }
      @objs.each { |o| fibers[o['context']] << o['id'] if o['kind'] == 'fiber' && o['context'] }
      fiber_keys = []
      @ctxs.each do |c|
        t = task_of[c['id']]
        c['role'] = c['id'] == 1 ? 'root' : (t ? 'task' : (fibers.key?(c['id']) ? 'fiber' : 'unknown'))
        c['task'] = t && t['id']
        c['fiber_objects'] = fibers[c['id']]
        c['current'] = c['id'] == @doc['current_context']
        c['key'] =
          case c['role']
          when 'root' then 'root'
          when 'task' then "task #{t['key']}"
          else
            ir = c['frames'].first && proc_irep(c['frames'].first['proc'])
            fiber_keys << c
            "fiber @#{place(ir) || (c['frames'].empty? ? 'ended' : '?')}"
          end
        c['frames'].each { |f| frame(c, f) }
        c['backtrace'] = backtrace(c)
      end
      uniq(fiber_keys.map { |c| c['key'] }).each_with_index { |k, i| fiber_keys[i]['key'] = k }
    end

    def stack_value(c, i) = i < c['stack'].size ? c['stack'][i] : nil

    def frame(c, f)
      ir = f['kind'] == 'ruby' ? proc_irep(f['proc']) : nil
      f['irep'] = ir
      fl = ir && f['pc'] && line_at(ir, f['pc'] - 1)
      f['file'], f['line'] = fl
      f['self'] = stack_value(c, f['reg_base'])
      names = ir ? @ireps[ir - 1]['locals'] || [] : []
      f['locals'] = names.each_with_index.map do |n, i|
        { 'name' => n, 'reg' => i + 1, 'value' => stack_value(c, f['reg_base'] + i + 1) }
      end
      f['captured'] = captured(f['proc'])
    end

    # the variables a block sees from the methods and blocks around it
    def captured(pid)
      out = []
      seen = {}
      p = pid && obj(pid)
      while p && p['kind'] == 'proc' && p['env'] && p['env']['kind'] == 'env' && !seen[p['id']]
        seen[p['id']] = true
        e = obj(p['env']['ref'])
        up = p['upper'] && obj(p['upper'])
        ir = up && proc_irep(up['id'])
        names = ir ? @ireps[ir - 1]['locals'] || [] : []
        level = { 'env' => e['id'], 'method' => e['method'], 'on_stack' => e['on_stack'] }
        vals =
          if e['on_stack']
            c = @ctxs[e['context'] - 1]
            fr = c['frames'].find { |x| x['reg_base'] == e['position'] }
            level['context'] = e['context']
            level['frame'] = fr && fr['index']
            Array.new([e['length'] - 1, 0].max) { |i| stack_value(c, e['position'] + i + 1) }
          else
            (e['values'] || [])[1..] || []
          end
        level['variables'] = vals.each_with_index.map { |v, i| { 'name' => names[i], 'value' => v } }
        out << level
        p = up
      end
      out
    end

    # what mruby's backtrace prints for this context (backtrace.c), top first
    def backtrace(c)
      fr = c['frames']
      out = []
      fr.each_index.reverse_each do |i|
        f = fr[i]
        if f['kind'] == 'ruby'
          f['location'] = nil
          next unless f['irep'] && @lines[f['irep']] && f['pc']
          loc = location(f['irep'], f['pc'], f['method'])
        else
          f['location'] = nil
          next unless f['method']
          lower = (0...i).to_a.reverse.map { |j| fr[j] }.find do |g|
            g['kind'] == 'ruby' && g['irep'] && @lines[g['irep']] && g['pc']
          end
          loc = lower ? location(lower['irep'], lower['pc'], f['method']) : '(unknown):0'
        end
        f['location'] = loc
        out << loc
      end
      out
    end

    def location(irep, pc, mid)
      fl = line_at(irep, pc - 1)
      return '(unknown):0' unless fl
      "#{fl[0]}:#{fl[1]}#{mid ? ":in #{mid}" : ''}"
    end

    # -- the edges of the graph --

    def push_value(out, label, v)
      out << [label, v['ref']] if v.is_a?(Hash) && v['ref']
    end

    def push_ref(out, label, id)
      out << [label, id] if id
    end

    def key_label(k)
      CheckpointImage.scalar?(k) ? CheckpointImage.value_text(@doc, k) : (str_of(k)&.then { |s| short(s.inspect, 24) } || "##{k['ref']}")
    end

    def refs(o)
      out = []
      iv = ->(list) { (list || []).each { |n, v| push_value(out, n.to_s.match?(/\A[A-Z]/) ? "::#{n}" : n.to_s, v) } }
      case o['kind']
      when 'object', 'random' then iv.(o['ivars'])
      when 'array' then o['items'].each_with_index { |v, i| push_value(out, "[#{i}]", v) }
      when 'struct'
        o['items'].each_with_index { |v, i| push_value(out, "[#{i}]", v) }
        iv.(o['ivars'])
      when 'hash'
        o['pairs'].each_with_index do |(k, v), i|
          push_value(out, "(key #{i})", k)
          push_value(out, "{#{key_label(k)}}", v)
        end
        iv.(o['ivars'])
      when 'set'
        (o['items'] || []).each_with_index { |v, i| push_value(out, "<#{i}>", v) }
        iv.(o['ivars'])
      when 'range'
        push_value(out, 'begin', o['begin'])
        push_value(out, 'end', o['end'])
      when 'exception'
        push_value(out, 'message', o['message'])
        push_value(out, 'backtrace', o['backtrace'])
        iv.(o['ivars'])
      when 'class', 'module', 'singleton_class'
        iv.(o['ivars'])
        o['methods'].each { |m| push_ref(out, "##{m['name']}", m['proc']) }
        push_ref(out, 'superclass', o['superclass'])
      when 'iclass' then push_ref(out, 'superclass', o['superclass'])
      when 'origin'
        o['methods'].each { |m| push_ref(out, "##{m['name']}", m['proc']) }
        push_ref(out, 'superclass', o['superclass'])
      when 'proc'
        push_ref(out, 'upper', o['upper'])
        push_ref(out, o['env']['kind'] == 'env' ? 'env' : 'target_class', o['env']['ref']) if o['env']
      when 'env' then (o['values'] || []).each_with_index { |v, i| push_value(out, "[#{i}]", v) }
      when 'data' then iv.(o['ivars'])
      when 'break' then push_value(out, 'value', o['value'])
      when 'task'
        iv.(o['ivars'])
        t = o['task'] && @tasks[o['task'] - 1]
        if t
          push_value(out, 'name', t['name'])
          push_value(out, 'value', t['value'])
          push_ref(out, 'waits on', t['wait'] && t['wait']['queue'])
        end
      end
      push_ref(out, 'class', o['class']) if o['class']
      out
    end

    # -- paths from the roots --

    def perm_text(id) = (o = obj(id))['kind'] == 'permanent' ? o['name'] : "##{id}"

    def roots
      list = []
      @doc['changes'].each do |c|
        case c['kind']
        when 'set_global'
          list << { 'key' => c['name'], 'kind' => 'global', 'refs' => vref(c['value']) }
        when 'set_variable'
          n = c['name'].to_s
          sep = n.match?(/\A[A-Z]/) ? '::' : '.'
          list << { 'key' => "#{perm_text(c['target'])}#{sep}#{n}", 'kind' => 'variable', 'refs' => vref(c['value']) }
        when 'define_method'
          list << { 'key' => "#{perm_text(c['target'])}##{c['method']['name']}", 'kind' => 'method',
                    'refs' => c['method']['proc'] ? [['', c['method']['proc']]] : [] }
        when 'set_superclass'
          list << { 'key' => "#{perm_text(c['target'])}.superclass", 'kind' => 'superclass', 'refs' => c['superclass'] ? [['', c['superclass']]] : [] }
        when 'contents'
          list << { 'key' => "#{perm_text(c['target'])}.contents", 'kind' => 'contents',
                    'refs' => ((c['items'] || []).each_with_index.map { |v, i| v.is_a?(Hash) && v['ref'] ? ["[#{i}]", v['ref']] : nil } +
                               (c['pairs'] || []).each_with_index.flat_map { |(k, v), i| [k.is_a?(Hash) && k['ref'] ? ["(key #{i})", k['ref']] : nil, v.is_a?(Hash) && v['ref'] ? ["{#{key_label(k)}}", v['ref']] : nil] }).compact }
        end
      end
      list.sort_by! { |r| [%w(global variable method superclass contents).index(r['kind']), r['key']] }
      # frames: the bottom ones of every context first (they live longest, so
      # the paths through them change least), then the next ones up
      frames = []
      @ctxs.sort_by { |c| [%w(root task fiber unknown).index(c['role']), c['key']] }.each_with_index do |c, ci|
        c['frames'].each_with_index do |f, k|
          top = k == c['frames'].size - 1
          hi = top ? c['stack'].size : c['frames'][k + 1]['reg_base']
          named = {}
          f['locals'].each { |l| named[l['reg']] = l['name'] if l['name'] }
          refs = []
          temps = []   # registers that are not variables: last in the paths
          (f['reg_base']...[hi, c['stack'].size].min).each do |i|
            r = i - f['reg_base']
            v = c['stack'][i]
            next unless v.is_a?(Hash) && v['ref']
            if named[r] then refs << [named[r], v['ref']]
            elsif r.zero? then refs << ['self', v['ref']]
            else temps << ["r#{r}", v['ref']]
            end
          end
          refs << ['&block', f['block']] if f['block']
          refs << [f['env']['kind'] == 'env' ? 'env' : 'target_class', f['env']['ref']] if f['env']
          refs << ['proc', f['proc']] if f['proc']
          f['key'] = "#{c['key']}/#{k}:#{f['method'] || f['cfunc'] || '-'}"
          frames << [[k, ci], { 'key' => f['key'], 'kind' => 'frame', 'context' => c['id'], 'frame' => k, 'refs' => refs, 'temps' => temps }]
        end
      end
      list.concat(frames.sort_by(&:first).map(&:last))
      @tasks.sort_by { |t| t['key'] }.each do |t|
        refs = []
        refs << ['', t['object']] if t['object']
        push_value(refs, 'name', t['name'])
        push_value(refs, 'value', t['value'])
        refs << ['waits on', t['wait']['queue']] if t['wait'] && t['wait']['queue']
        list << { 'key' => "task #{t['key']}", 'kind' => 'task', 'task' => t['id'], 'refs' => refs }
      end
      @ctxs.each do |c|
        c['fiber_objects'].each { |id| list << { 'key' => "#{c['key']}.fiber", 'kind' => 'fiber', 'context' => c['id'], 'refs' => [['', id]] } }
      end
      list
    end

    def vref(v) = v.is_a?(Hash) && v['ref'] ? [['', v['ref']]] : []

    def join(path, label)
      return path if label.empty?
      case label[0]
      when '[', '{', '#', '<', ':' then "#{path}#{label}"
      else "#{path}.#{label}"
      end
    end

    # Breadth first from the roots in order: every object gets the shortest
    # path, the first root winning a tie. Permanent objects are their names.
    def paths
      @doc['roots'] = rs = roots
      @objs.each { |o| o['path'] = "=#{o['name']}" if %w(permanent permanent_proc).include?(o['kind']) }
      # first everything the named roots reach, then what only temporary
      # registers reach (those objects are marked "temporary")
      [['refs', false], ['temps', true]].each do |field, temp|
        q = []
        rs.each do |r|
          (r[field] || []).each do |label, id|
            o = obj(id)
            next if o['path']
            o['path'] = join(r['key'], label)
            o['temporary'] = temp
            q << o
          end
        end
        i = 0
        while i < q.size
          o = q[i]
          i += 1
          o['refs'].each do |label, id|
            c = obj(id)
            next if c['path']
            c['path'] = join(o['path'], label)
            c['temporary'] = temp
            q << c
          end
        end
      end
      @objs.each do |o|
        o['path'] ||= "?##{o['id']}"
        o['temporary'] = false unless o.key?('temporary')
      end
      unreached = @objs.count { |o| o['path'].start_with?('?#') }
      @doc['warnings'] << "#{unreached} objects are not reached from the roots" if unreached.positive?
    end
  end
end
