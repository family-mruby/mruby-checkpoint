# The difference of two checkpoint images, from their inspect JSON
# (doc/inspect_json.md, "the diff").
#
#   d = CheckpointDiff.diff(doc_a, doc_b)   # a Hash (JSON-ready)
#   CheckpointDiff.text(d)                   # the same as text
#
# Object numbers are given again in every image, so nothing is matched by
# number. Objects are matched by their path from the roots (the derived
# `path` of each object: globals, constants and methods set on permanent
# objects, the frames of the contexts, the Tasks); contexts and Tasks by
# their keys (the name of a Task, or where its body is written); frames by
# their place from the bottom of their context while the method and the code
# stay the same. Objects left unmatched on both sides are paired as "moved"
# when their contents (with the references left out) are the same and no
# other unmatched object has those contents.

require_relative 'checkpoint_image'

module CheckpointDiff
  JSON_VERSION = 1

  module_function

  # what a value is, independent of the numbering: scalars by value,
  # objects by their path (permanent ones by their name)
  def ident(doc, v)
    return CheckpointImage.value_text(doc, v) if CheckpointImage.scalar?(v)
    doc['objects'][v['ref'] - 1]['path']
  end

  def shown(doc, v)
    return CheckpointImage.value_text(doc, v) if CheckpointImage.scalar?(v)
    o = doc['objects'][v['ref'] - 1]
    "#{o['label']} (#{o['path']})"
  end

  def oident(doc, id) = id ? doc['objects'][id - 1]['path'] : '-'
  def ctx_key(doc, id) = id ? doc['contexts'][id - 1]['key'] : '-'
  def task_key(doc, id) = id ? doc['scheduler']['tasks'][id - 1]['key'] : '-'
  def irep_hash(doc, id) = id ? doc['ireps'][id - 1]['hash'] : '-'

  # ---- the fields of an object, as [name, ident] ----

  def fields(doc, o, refs: true)
    r = ->(v) { refs || CheckpointImage.scalar?(v) ? ident(doc, v) : '*' }
    ro = ->(id) { refs ? oident(doc, id) : (id ? '*' : '-') }
    f = [['kind', o['kind']]]
    f << ['class', oident(doc, o['class'])] if o.key?('class')
    f << ['frozen', o['frozen'].to_s] if o['frozen']
    iv = ->(list) { (list || []).each { |n, v| f << [n.to_s, r.(v)] } }
    case o['kind']
    when 'permanent', 'permanent_proc' then f << ['name', o['name']]
    when 'object', 'random', 'data', 'task'
      f << ['type', o['type']] << ['contents', o['data_hex']] if o['kind'] == 'data'
      f << ['state', o['state']] << ['seed', o['seed'].to_s] if o['kind'] == 'random'
      f << ['task', task_key(doc, o['task'])] if o['kind'] == 'task'
      iv.(o['ivars'])
    when 'string'
      f << ['bytes', o['text'] ? o['text'].inspect : "<#{o['hex']}>"]
      f << ['binary', 'true'] if o['binary']
    when 'array', 'struct'
      o['items'].each_with_index { |v, i| f << ["[#{i}]", r.(v)] }
      iv.(o['ivars'])
    when 'hash'
      o['pairs'].each { |k, v| f << ["{#{r.(k)}}", r.(v)] }
      f << ['default', o['default'].select { |_, b| b }.keys.join(',')] if o['default'].values.any?
      iv.(o['ivars'])
    when 'set'
      f << ['uninitialized', 'true'] if o['uninitialized']
      (o['items'] || []).each { |v| f << ["<#{r.(v)}>", 'in'] }
      iv.(o['ivars'])
    when 'range'
      f << ['begin', r.(o['begin'])] << ['end', r.(o['end'])] << ['exclusive', o['exclusive'].to_s]
    when 'exception'
      f << ['message', r.(o['message'])] << ['backtrace', r.(o['backtrace'])]
      iv.(o['ivars'])
    when 'class', 'module', 'singleton_class', 'origin'
      f << ['superclass', ro.(o['superclass'])]
      f << ['instance_type', o['instance_type']] if o['instance_type']
      iv.(o['ivars'])
      o['methods'].each do |m|
        what = case m['kind']
               when 'proc' then ro.(m['proc'])
               when 'cfunc' then "C #{m['cfunc']}"
               else 'undef'
               end
        f << ["##{m['name']}", "#{m['visibility']} #{what}"]
      end
    when 'iclass' then f << ['superclass', ro.(o['superclass'])]
    when 'proc'
      b = o['body']
      f << ['body', case b['kind']
                    when 'irep' then "irep #{irep_hash(doc, b['irep'])}"
                    when 'cfunc' then "C #{b['name']}"
                    else "alias #{b['method']}"
                    end]
      f << ['flags', %w(lambda orphan scope).select { |k| o[k] }.join(',')]
      f << ['upper', ro.(o['upper'])]
      f << ['env', o['env'] ? "#{o['env']['kind']} #{ro.(o['env']['ref'])}" : '-']
    when 'env'
      f << ['method', o['method'].to_s] << ['length', o['length'].to_s]
      if o['on_stack']
        c = doc['contexts'][o['context'] - 1]
        fr = c['frames'].find { |x| x['reg_base'] == o['position'] }
        f << ['on the stack', "#{c['key']} frame #{fr ? fr['index'] : "at #{o['position']}"}"]
      else
        o['values'].each_with_index { |v, i| f << ["[#{i}]", r.(v)] }
      end
    when 'bigint' then f << ['value', o['value']]
    when 'rational' then f << ['value', "#{o['numerator']}/#{o['denominator']}"]
    when 'complex' then f << ['value', o['label']]
    when 'break' then f << ['tag', o['tag']] << ['frame', o['frame'].to_s] << ['value', r.(o['value'])]
    when 'fiber' then f << ['context', ctx_key(doc, o['context'])]
    end
    f
  end

  def field_changes(fa, fb)
    ha = fa.to_h
    hb = fb.to_h
    keys = (fa.map(&:first) + fb.map(&:first)).uniq
    keys.filter_map do |k|
      next if ha[k] == hb[k]
      { 'field' => k, 'a' => ha[k], 'b' => hb[k] }
    end
  end

  # ---- the diff ----

  def diff(a, b)
    d = {
      'inspect_diff' => JSON_VERSION,
      'a' => a['source'], 'b' => b['source'],
      'header' => header(a, b),
      'variables' => variables(a, b),
      'contexts' => contexts(a, b),
      'tasks' => tasks(a, b),
      'scheduler' => scheduler(a, b),
      'objects' => objects(a, b),
    }
    d['same'] = d['header'].empty? && d['variables'].empty? && d['tasks'].empty? && d['scheduler'].empty? &&
                d['contexts'].all? { |c| c['change'] == 'same' } &&
                d['objects'].values.all?(&:empty?)
    d
  end

  def header(a, b)
    a['header'].keys.filter_map do |k|
      next if a['header'][k] == b['header'][k]
      { 'field' => k, 'a' => a['header'][k], 'b' => b['header'][k] }
    end
  end

  # the change table (§9): globals, constants and ivars of permanent
  # objects, methods defined or removed on them, by what they change
  def ref_of(v) = v.is_a?(Hash) ? v['ref'] : nil

  def change_map(doc, refs = {})
    m = {}
    doc['changes'].each do |c|
      t = c['target'] && doc['objects'][c['target'] - 1]
      tn = t ? (t['kind'] == 'permanent' ? t['name'] : t['path']) : nil
      key, val =
        case c['kind']
        when 'set_global' then [c['name'], shown(doc, c['value'])]
        when 'remove_global' then [c['name'], '(removed)']
        when 'set_variable' then ["#{tn}#{c['name'].to_s.match?(/\A[A-Z]/) ? '::' : '.'}#{c['name']}", shown(doc, c['value'])]
        when 'remove_variable' then ["#{tn}.#{c['name']}", '(removed)']
        when 'define_method'
          mm = c['method']
          ["#{tn}##{mm['name']}", "#{mm['visibility']} #{mm['kind'] == 'proc' ? shown(doc, { 'ref' => mm['proc'] }) : (mm['cfunc'] || 'undef')}"]
        when 'remove_method' then ["#{tn}##{c['name']}", '(removed)']
        when 'set_superclass' then ["#{tn}.superclass", oident(doc, c['superclass'])]
        when 'random_state' then ["#{tn}.state", "#{c['state']} seed #{c['seed']}"]
        when 'class_flags' then ["#{tn}.prepended", c['prepended'].to_s]
        when 'set_class' then ["#{tn}.singleton_class", oident(doc, c['class'])]
        when 'contents' then ["#{tn}.contents", c['items'] ? "[#{c['items'].map { |v| ident(doc, v) }.join(', ')}]" : (c['pairs'] ? "{#{c['pairs'].map { |k, v| "#{ident(doc, k)} => #{ident(doc, v)}" }.join(', ')}}" : (c['type'] == 'exception' ? "message #{ident(doc, c['message'])}" : c['text'].inspect))]
        end
      m[key] = val
      refs[key] = ref_of(c['value']) || (c['method'] && c['method']['proc'])
    end
    m
  end

  def variables(a, b)
    ra = {}
    rb = {}
    ma = change_map(a, ra)
    mb = change_map(b, rb)
    (ma.keys + mb.keys).uniq.filter_map do |k|
      next if ma[k] == mb[k]
      # a value that is the same object in both (same path) differs only in
      # its contents, which the objects part shows
      { 'key' => k, 'a' => ma[k], 'b' => mb[k], 'a_ref' => ra[k], 'b_ref' => rb[k] }
    end
  end

  def by_key(list) = list.to_h { |x| [x['key'], x] }

  def frame_text(f) = f['location'] || "#{f['file'] ? "#{f['file']}:#{f['line']}" : '(no line)'}#{f['method'] ? ":in #{f['method']}" : ''}"

  def contexts(a, b)
    ca = by_key(a['contexts'])
    cb = by_key(b['contexts'])
    (ca.keys + cb.keys).uniq.map do |k|
      x = ca[k]
      y = cb[k]
      next { 'key' => k, 'change' => 'added', 'b' => y['id'], 'frames' => y['frames'].map { |f| { 'index' => f['index'], 'change' => 'called', 'b' => frame_text(f) } } } unless x
      next { 'key' => k, 'change' => 'removed', 'a' => x['id'], 'frames' => x['frames'].map { |f| { 'index' => f['index'], 'change' => 'returned', 'a' => frame_text(f) } } } unless y
      c = { 'key' => k, 'a' => x['id'], 'b' => y['id'], 'fields' => [] }
      %w(activity vmexec current).each do |fld|
        c['fields'] << { 'field' => fld, 'a' => x[fld].to_s, 'b' => y[fld].to_s } if x[fld] != y[fld]
      end
      pa = x['prev'] && ctx_key(a, x['prev'])
      pb = y['prev'] && ctx_key(b, y['prev'])
      c['fields'] << { 'field' => 'resumed by', 'a' => pa || '-', 'b' => pb || '-' } if pa != pb
      c['frames'] = frames(a, b, x, y)
      c['change'] = c['fields'].empty? && c['frames'].all? { |f| f['change'] == 'same' } ? 'same' : 'changed'
      c
    end
  end

  def same_code?(a, b, f, g)
    f['method'] == g['method'] && f['kind'] == g['kind'] && f['cfunc'] == g['cfunc'] &&
      (f['irep'] ? irep_hash(a, f['irep']) : nil) == (g['irep'] ? irep_hash(b, g['irep']) : nil)
  end

  def frames(a, b, x, y)
    fa = x['frames']
    fb = y['frames']
    n = 0
    n += 1 while n < fa.size && n < fb.size && same_code?(a, b, fa[n], fb[n])
    out = []
    n.times do |i|
      f = fa[i]
      g = fb[i]
      e = { 'index' => i, 'method' => f['method'], 'a' => frame_text(f), 'b' => frame_text(g), 'locals' => [] }
      e['pc'] = [f['pc'], g['pc']] if f['pc'] != g['pc']
      names = (f['locals'].map { |l| l['name'] } + g['locals'].map { |l| l['name'] }).compact.uniq
      la = f['locals'].to_h { |l| [l['name'], l['value']] }
      lb = g['locals'].to_h { |l| [l['name'], l['value']] }
      names.each do |nm|
        va = la.key?(nm) ? ident(a, la[nm]) : nil
        vb = lb.key?(nm) ? ident(b, lb[nm]) : nil
        if va != vb
          e['locals'] << { 'name' => nm, 'a' => la.key?(nm) ? shown(a, la[nm]) : nil, 'b' => lb.key?(nm) ? shown(b, lb[nm]) : nil,
                           'a_ref' => ref_of(la[nm]), 'b_ref' => ref_of(lb[nm]) }
        elsif !CheckpointImage.scalar?(la[nm]) && fields(a, a['objects'][la[nm]['ref'] - 1]) != fields(b, b['objects'][lb[nm]['ref'] - 1])
          e['locals'] << { 'name' => nm, 'a' => shown(a, la[nm]), 'b' => shown(b, lb[nm]), 'contents_changed' => true,
                           'a_ref' => ref_of(la[nm]), 'b_ref' => ref_of(lb[nm]) }
        end
      end
      sa = ident(a, f['self'])
      sb = ident(b, g['self'])
      e['self'] = [shown(a, f['self']), shown(b, g['self'])] if sa != sb
      e['change'] = e['pc'] || !e['locals'].empty? || e['self'] ? 'changed' : 'same'
      out << e
    end
    fa[n..].each { |f| out << { 'index' => f['index'], 'change' => 'returned', 'method' => f['method'], 'a' => frame_text(f) } }
    fb[n..].each { |f| out << { 'index' => f['index'], 'change' => 'called', 'method' => f['method'], 'b' => frame_text(f) } }
    out
  end

  # the wait of a Task without numbers: the Task waited for by its key, the
  # Queue by its path
  def wait_ident(doc, w)
    case w['reason']
    when 'sleep' then { 'reason' => 'sleep', 'ticks_left' => w['ticks_left'] }
    when 'join' then { 'reason' => 'join', 'task' => task_key(doc, w['task']) }
    else { 'reason' => 'queue', 'queue' => oident(doc, w['queue']), 'ticks_left' => w['ticks_left'] }
    end
  end

  def wait_text(w)
    return w.to_s unless w.is_a?(Hash)
    case w['reason']
    when 'sleep' then "sleep, #{w['ticks_left']} ticks left"
    when 'join' then "join #{w['task']}"
    else "Task::Queue #{w['queue']}#{w['ticks_left'] ? ", #{w['ticks_left']} ticks left" : ''}"
    end
  end

  def task_fields(doc, t)
    {
      'state' => t['state'], 'queue' => t['queue'] ? "#{t['queue']} ##{t['queue_index']}" : 'main',
      'priority' => t['priority'].to_s, 'timeslice' => t['timeslice'].to_s,
      'wait' => t['wait'] ? wait_ident(doc, t['wait']) : nil,
      'value' => t['value'] ? shown(doc, t['value']) : 'nil',
      'running' => t['running'].to_s, 'host' => t['host'].to_s,
    }
  end

  def tasks(a, b)
    ta = by_key(a['scheduler']['tasks'])
    tb = by_key(b['scheduler']['tasks'])
    (ta.keys + tb.keys).uniq.filter_map do |k|
      next { 'key' => k, 'change' => 'added', 'b' => tb[k]['id'] } unless ta[k]
      next { 'key' => k, 'change' => 'removed', 'a' => ta[k]['id'] } unless tb[k]
      fa = task_fields(a, ta[k])
      fb = task_fields(b, tb[k])
      ch = fa.keys.filter_map { |f| fa[f] == fb[f] ? nil : { 'field' => f, 'a' => fa[f], 'b' => fb[f] } }
      next if ch.empty?
      { 'key' => k, 'change' => 'changed', 'a' => ta[k]['id'], 'b' => tb[k]['id'], 'fields' => ch }
    end
  end

  def scheduler(a, b)
    sa = a['scheduler']
    sb = b['scheduler']
    out = []
    %w(switching loop tick_ms tick next_wakeup).each do |f|
      out << { 'field' => f, 'a' => sa[f], 'b' => sb[f] } if sa[f] != sb[f]
    end
    { 'running_task' => 'running Task', 'host_task' => 'host Task' }.each do |f, name|
      x = task_key(a, sa[f])
      y = task_key(b, sb[f])
      out << { 'field' => name, 'a' => x, 'b' => y } if x != y
    end
    x = ctx_key(a, a['current_context'])
    y = ctx_key(b, b['current_context'])
    out << { 'field' => 'current context', 'a' => x, 'b' => y } if x != y
    out
  end

  def objects(a, b)
    pa = {}
    pb = {}
    a['objects'].each { |o| pa[o['path']] = o unless o['path'].start_with?('=') }
    b['objects'].each { |o| pb[o['path']] = o unless o['path'].start_with?('=') }
    changed = []
    gone = []
    new = []
    pa.each do |p, o|
      q = pb[p]
      if q.nil? || q['kind'] != o['kind']
        gone << o
        next
      end
      ch = field_changes(fields(a, o), fields(b, q))
      changed << { 'path' => p, 'a' => o['id'], 'b' => q['id'], 'kind' => o['kind'], 'label_a' => o['label'],
                   'label_b' => q['label'], 'temporary' => o['temporary'] || q['temporary'], 'fields' => ch } unless ch.empty?
    end
    pb.each { |p, q| new << q if pa[p].nil? || pa[p]['kind'] != q['kind'] }
    # pair what is left by contents
    sig = ->(doc, o) { fields(doc, o, refs: false) }
    ga = gone.group_by { |o| sig.(a, o) }
    nb = new.group_by { |o| sig.(b, o) }
    moved = []
    ga.each do |s, xs|
      ys = nb[s]
      next unless xs.size == 1 && ys&.size == 1
      moved << { 'a_path' => xs[0]['path'], 'b_path' => ys[0]['path'], 'a' => xs[0]['id'], 'b' => ys[0]['id'], 'label' => xs[0]['label'],
                 'temporary' => xs[0]['temporary'] || ys[0]['temporary'] }
    end
    ma = moved.to_h { |m| [m['a'], true] }
    mb = moved.to_h { |m| [m['b'], true] }
    {
      'changed' => changed,
      'added' => new.reject { |o| mb[o['id']] }.map { |o| { 'path' => o['path'], 'b' => o['id'], 'label' => o['label'], 'temporary' => o['temporary'] } },
      'removed' => gone.reject { |o| ma[o['id']] }.map { |o| { 'path' => o['path'], 'a' => o['id'], 'label' => o['label'], 'temporary' => o['temporary'] } },
      'moved' => moved,
    }
  end

  # ---- as text ----

  def text(d)
    o = +"A: #{d['a']['file']} (#{d['a']['bytes']} bytes)\nB: #{d['b']['file']} (#{d['b']['bytes']} bytes)\n"
    return o << "no differences\n" if d['same']
    fv = ->(f, x) { x.nil? ? '(none)' : (f == 'wait' ? wait_text(x) : x) }
    fl = ->(list, ind) { list.each { |c| o << "#{ind}#{c['field']}: #{fv.(c['field'], c['a'])} -> #{fv.(c['field'], c['b'])}\n" } }
    unless d['header'].empty?
      o << "header:\n"
      fl.(d['header'], '  ')
    end
    unless d['variables'].empty?
      o << "globals, constants and methods:\n"
      d['variables'].each { |x| o << "  #{x['key']}: #{x['a'] || '(none)'} -> #{x['b'] || '(none)'}\n" }
    end
    cs = d['contexts'].reject { |c| c['change'] == 'same' }
    unless cs.empty?
      o << "call stacks:\n"
      cs.each do |c|
        o << "  #{c['key']}#{c['change'] == 'changed' ? '' : " (#{c['change']})"}\n"
        fl.(c['fields'] || [], '    ')
        (c['frames'] || []).reverse_each do |f|
          case f['change']
          when 'same' then next
          when 'called' then o << "    + #{f['index']}: #{f['b']}\n"
          when 'returned' then o << "    - #{f['index']}: #{f['a']}\n"
          else
            o << "    ~ #{f['index']}: #{f['a'] == f['b'] ? f['a'] : "#{f['a']} -> #{f['b']}"}#{f['pc'] ? " (pc #{f['pc'][0]} -> #{f['pc'][1]})" : ''}\n"
            o << "        self: #{f['self'][0]} -> #{f['self'][1]}\n" if f['self']
            f['locals'].each do |l|
              o << (l['contents_changed'] ? "        #{l['name']}: #{l['b']} (contents changed)\n" : "        #{l['name']}: #{l['a'] || '(none)'} -> #{l['b'] || '(none)'}\n")
            end
          end
        end
      end
    end
    unless d['tasks'].empty?
      o << "Tasks:\n"
      d['tasks'].each do |t|
        o << "  #{t['key']}#{t['change'] == 'changed' ? '' : " (#{t['change']})"}\n"
        fl.(t['fields'] || [], '    ')
      end
    end
    unless d['scheduler'].empty?
      o << "scheduler:\n"
      fl.(d['scheduler'], '  ')
    end
    ob = d['objects']
    [false, true].each do |temp|
      sel = ->(list) { list.select { |c| !!c['temporary'] == temp } }
      ch, ad, rm, mv = sel.(ob['changed']), sel.(ob['added']), sel.(ob['removed']), sel.(ob['moved'])
      next if temp && [ch, ad, rm, mv].all?(&:empty?)
      o << (temp ? 'objects reached only through temporary registers' : 'objects')
      o << ": #{ch.size} changed, #{ad.size} added, #{rm.size} removed, #{mv.size} moved\n"
      ch.each do |c|
        o << "  ~ #{c['path']} #{c['label_a']}#{c['label_b'] == c['label_a'] ? '' : " -> #{c['label_b']}"}\n"
        fl.(c['fields'], '      ')
      end
      ad.each { |c| o << "  + #{c['path']} #{c['label']}\n" }
      rm.each { |c| o << "  - #{c['path']} #{c['label']}\n" }
      mv.each { |c| o << "  > #{c['a_path']} -> #{c['b_path']} #{c['label']}\n" }
    end
    o
  end
end
