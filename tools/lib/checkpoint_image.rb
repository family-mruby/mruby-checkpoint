# The one reader of checkpoint images (doc/format_v1.md): an image becomes a
# plain Ruby Hash (the inspect JSON, doc/inspect_json.md). The command line
# (tools/checkpoint_inspect.rb), the diff and the HTML are all made from it.
#
#   doc = CheckpointImage.read_file(path)          # raises CheckpointImage::FormatError
#   doc = CheckpointImage.read_file(path, partial: true)   # doc['error'] instead
#
# Written from doc/format_v1.md alone (not from the C sources), like the
# earlier tools/checkpoint_inspect.rb it replaces.
#
# The doc has two kinds of fields: the image as it is written (every record of
# every section, in order), and derived fields that make it readable without
# knowing the format (labels of objects, file:line of frames, locals, the
# roles of contexts, the edges of the object graph, a path from the roots for
# every object). doc/inspect_json.md says which is which.

module CheckpointImage
  JSON_VERSION = 1
  FORMAT_VERSION = 1

  class FormatError < StandardError
    attr_reader :offset, :where, :reason
    def initialize(reason, offset, where)
      @reason, @offset, @where = reason, offset, where
      super("broken image#{offset ? " at byte #{offset}" : ''} (#{where}): #{reason}")
    end
    def to_h = { 'offset' => offset, 'where' => where, 'message' => reason }
  end

  # ---- the bytes ----

  class Bytes
    attr_reader :pos
    attr_accessor :where

    def initialize(bytes)
      @b = bytes.b
      @n = @b.bytesize
      @pos = 0
      @where = 'header'
    end

    def size = @n
    def rest = @n - @pos
    def eof? = @pos >= @n
    def peek(k) = @b.byteslice(@pos, k)
    def slice(from, len) = @b.byteslice(from, len)

    def fail!(reason, at = @pos) = raise(FormatError.new(reason, at, @where))

    def need(k)
      fail!("cut short: #{k} byte#{k == 1 ? '' : 's'} needed, #{rest} left") if @pos + k > @n
    end

    def u8 = (need(1); v = @b.getbyte(@pos); @pos += 1; v)
    def u16 = (need(2); v = @b.unpack1('n', offset: @pos); @pos += 2; v)
    def u32 = (need(4); v = @b.unpack1('N', offset: @pos); @pos += 4; v)
    def i32 = (need(4); v = @b.unpack1('l>', offset: @pos); @pos += 4; v)
    def u64 = (need(8); v = @b.unpack1('Q>', offset: @pos); @pos += 8; v)
    def i64 = (need(8); v = @b.unpack1('q>', offset: @pos); @pos += 8; v)
    def f64 = (need(8); v = @b.unpack1('G', offset: @pos); @pos += 8; v)

    def take(k)
      need(k)
      s = @b.byteslice(@pos, k)
      @pos += k
      s
    end

    def str
      at = @pos
      len = u32
      fail!("a string of #{len} bytes, only #{rest} left", at) if len > rest
      take(len)
    end

    # a name: text for JSON (bytes that are not UTF-8 shown as \xHH)
    def text = CheckpointImage.text(str)
  end

  # ---- small helpers shared with the diff and the HTML ----

  SAFE_INT = 2**53 - 1

  def self.json_int(v) = v.abs <= SAFE_INT ? v : { 'int' => v.to_s }

  def self.json_float(f)
    return { 'float' => 'NaN' } if f.nan?
    return { 'float' => f.positive? ? 'Infinity' : '-Infinity' } if f.infinite?
    { 'float' => f }
  end

  def self.text(s)
    u = s.dup.force_encoding(Encoding::UTF_8)
    return u if u.valid_encoding?
    s.b.each_char.map { |c| c.ord < 0x80 ? c : format('\\x%02X', c.ord) }.join.force_encoding(Encoding::UTF_8)
  end

  # a String's bytes for JSON: text when it is valid UTF-8, else hex
  def self.json_bytes(s)
    u = s.dup.force_encoding(Encoding::UTF_8)
    u.valid_encoding? ? { 'text' => u } : { 'hex' => s.unpack1('H*') }
  end

  OBJECT_KINDS = {
    1 => 'permanent', 2 => 'object', 3 => 'string', 4 => 'array', 5 => 'hash', 6 => 'range',
    7 => 'exception', 8 => 'class', 9 => 'module', 10 => 'singleton_class', 11 => 'iclass',
    12 => 'proc', 13 => 'env', 14 => 'data', 15 => 'permanent_proc', 16 => 'bigint',
    17 => 'struct', 18 => 'set', 19 => 'rational', 20 => 'complex', 21 => 'break',
    23 => 'random', 24 => 'origin', 25 => 'string', 26 => 'task', 27 => 'fiber',
  }.freeze
  INSTANCE_TYPES = %w(default object exception string array hash range proc data struct set
                      complex rational bigint random).freeze
  VISIBILITY = %w(public private protected).freeze
  CONTEXT_STATES = %w(created running resumed suspended transferred terminated).freeze
  TASK_STATES = { 0 => 'DORMANT', 2 => 'READY', 3 => 'RUNNING', 4 => 'WAITING', 8 => 'SUSPENDED' }.freeze
  QUEUES = { 0 => 'dormant', 1 => 'ready', 2 => 'waiting', 3 => 'suspended', 255 => nil }.freeze
  WAIT_REASONS = { 0 => nil, 1 => 'sleep', 4 => 'join', 8 => 'queue' }.freeze
  CHANGE_KINDS = {
    1 => 'define_method', 2 => 'remove_method', 3 => 'set_variable', 4 => 'remove_variable',
    5 => 'set_superclass', 6 => 'set_global', 7 => 'remove_global', 8 => 'random_state',
    9 => 'class_flags', 10 => 'contents',
  }.freeze
  NONE32 = 0xffffffff
  CLASSY = %w(permanent class module singleton_class iclass origin).freeze
  PROCS = %w(proc permanent permanent_proc).freeze

  # the doc without the instructions of the ireps (the HTML does not show
  # them); the doc says so in "omitted"
  def self.without_code(doc)
    d = doc.dup
    d['ireps'] = doc['ireps'].map { |ir| ir.reject { |k, _| k == 'iseq' } }
    d['omitted'] = ['ireps.iseq']
    d
  end

  def self.read_file(path, partial: false)
    read(File.binread(path), name: File.basename(path), partial: partial)
  end

  def self.read(bytes, name: nil, partial: false)
    Reader.new(bytes, name).run(partial)
  end

  # ---- the reader ----

  class Reader
    def initialize(bytes, name)
      @r = Bytes.new(bytes)
      @doc = {
        'inspect_json' => JSON_VERSION,
        'source' => { 'file' => name, 'bytes' => @r.size },
        'header' => nil, 'symbols' => [], 'ireps' => [], 'objects' => [], 'changes' => [],
        'contexts' => [], 'current_context' => nil, 'scheduler' => nil, 'sections' => [],
        'warnings' => [], 'error' => nil,
      }
      @later = []   # refs into tables read further on: [table, n, offset, where]
    end

    def run(partial)
      read_all
      check_later
      begin
        Derive.new(@doc, @hash_input || {}).run
      rescue StandardError => e
        # every record is well formed, but they do not fit together in a way
        # the checks above do not catch
        raise FormatError.new("the records do not fit together (#{e.class}: #{e.message})", nil, 'derived fields')
      end
      @doc
    rescue FormatError => e
      raise unless partial
      @doc['error'] = e.to_h
      @doc
    end

    private

    def read_all
      r = @r
      header
      r.where = 'symbols'
      n = count('symbols')
      @syms_raw = Array.new(n) { |i| r.where = "symbols > #{i + 1}"; r.str }
      @syms = @syms_raw.map { |x| CheckpointImage.text(x) }
      @doc['symbols'] = @syms

      r.where = 'ireps'
      @nirep = count('ireps')
      @doc['ireps'] = Array.new(@nirep) { |i| irep(i + 1) }

      r.where = 'objects'
      @nobj = count('objects')
      @doc['objects'] = Array.new(@nobj) { |i| object(i + 1) }

      r.where = 'changes'
      n = count('changes')
      @doc['changes'] = Array.new(n) { |i| change(i + 1) }

      r.where = 'contexts'
      @nctx = count('contexts')
      r.fail!('no contexts (at least the root one is written)') if @nctx.zero?
      @doc['contexts'] = Array.new(@nctx) { |i| context(i + 1) }
      r.where = 'current context'
      at = r.pos
      @doc['current_context'] = cur = r.u32
      r.fail!("current context #{cur} is not in 1..#{@nctx}", at) unless (1..@nctx).cover?(cur)

      scheduler
      sections
    end

    def count(what)
      at = @r.pos
      n = @r.u32
      # every record takes at least one byte: a count past the end is broken
      @r.fail!("#{n} #{what} cannot fit in the #{@r.rest} bytes left", at) if n > @r.rest
      n
    end

    def header
      r = @r
      magic = r.peek(4)
      r.fail!("not a checkpoint image (starts with #{magic.inspect}, not \"MRCK\")") unless magic == 'MRCK'
      r.take(4)
      at = r.pos
      ver = r.u16
      r.fail!("format v#{ver}: this reader reads v#{FORMAT_VERSION}", at) unless ver == FORMAT_VERSION
      impl = r.text
      rite = r.text
      ib, fb, utf8, pw = r.u8, r.u8, r.u8, r.u8
      @doc['header'] = {
        'magic' => 'MRCK', 'format' => ver, 'implementation' => impl,
        'bytecode' => rite, 'integer_bits' => ib, 'float_bits' => fb, 'utf8' => utf8 == 1, 'pointer_bits' => pw,
      }
      unless [ib, fb, utf8] == [64, 64, 1]
        @doc['warnings'] << "value representation #{ib}/#{fb}/#{utf8} is not 64/64/1: readers refuse this image"
      end
    end

    # -- references --

    def sym_id
      at = @r.pos
      i = @r.u32
      return nil if i.zero?
      @r.fail!("symbol #{i} is not in 1..#{@syms.size}", at) if i > @syms.size
      i
    end

    def sym = (i = sym_id) && @syms[i - 1]

    def irep_ref(allow_zero = true)
      at = @r.pos
      i = @r.u32
      return nil if i.zero? && allow_zero
      @r.fail!("irep #{i} is not in 1..#{@nirep}", at) unless (1..@nirep).cover?(i)
      i
    end

    # kinds: what the reference must point at (checked when all objects are
    # read: references may point forward)
    def obj_ref(kinds = nil)
      at = @r.pos
      i = @r.u32
      return nil if i.zero?
      @r.fail!("object #{i} is not in 1..#{@nobj}", at) if i > @nobj
      @later << [:object, i, at, @r.where, kinds] if kinds
      i
    end

    def ctx_ref
      at = @r.pos
      i = @r.u32
      return nil if i.zero?
      if @nctx
        @r.fail!("context #{i} is not in 1..#{@nctx}", at) if i > @nctx
      else
        @later << [:context, i, at, @r.where]
      end
      i
    end

    def task_ref
      at = @r.pos
      i = @r.u32
      return nil if i.zero?
      @later << [:task, i, at, @r.where]
      i
    end

    def check_later
      ntask = @doc.dig('scheduler', 'tasks')&.size || 0
      @later.each do |table, i, at, where, kinds|
        max = { object: @nobj, context: @nctx, task: ntask }[table]
        raise FormatError.new("#{table} #{i} is not in 1..#{max}", at, where) if i > max
        next unless kinds
        k = @doc['objects'][i - 1]['kind']
        next if kinds.include?(k)
        raise FormatError.new("object #{i} is a #{k}, not a #{kinds.join(' or ')}", at, where)
      end
    end

    # -- values --

    def value
      at = @r.pos
      case (t = @r.u8)
      when 0 then nil
      when 1 then false
      when 2 then true
      when 3 then CheckpointImage.json_int(@r.i64)
      when 4 then CheckpointImage.json_float(@r.f64)
      when 5 then { 'sym' => sym }
      when 6
        ref = obj_ref
        @r.fail!('an object value with object number 0', at + 1) unless ref
        { 'ref' => ref }
      when 7 then { 'undef' => true }
      else @r.fail!("unknown value tag #{t}", at)
      end
    end

    def ivars
      at = @r.pos
      n = @r.u32
      @r.fail!("#{n} ivars cannot fit", at) if n > @r.rest
      Array.new(n) { [sym, value] }
    end

    def method_entry
      name = sym
      at = @r.pos
      vis = @r.u8
      @r.fail!("unknown visibility #{vis}", at) unless vis < 3
      at = @r.pos
      case (k = @r.u8)
      when 0 then { 'name' => name, 'visibility' => VISIBILITY[vis], 'kind' => 'proc', 'proc' => obj_ref(PROCS) }
      when 1 then { 'name' => name, 'visibility' => VISIBILITY[vis], 'kind' => 'cfunc', 'cfunc' => @r.text, 'aspec' => @r.u32 }
      when 2 then { 'name' => name, 'visibility' => VISIBILITY[vis], 'kind' => 'undef' }
      else @r.fail!("unknown method kind #{k}", at)
      end
    end

    def methods_list
      at = @r.pos
      n = @r.u32
      @r.fail!("#{n} methods cannot fit", at) if n > @r.rest
      Array.new(n) { method_entry }
    end

    # -- ireps (§6) --

    def irep(id)
      r = @r
      r.where = "ireps > @#{id}"
      start = r.pos
      at = r.pos
      kind = r.u8
      h = r.u64
      rec = { 'id' => id, 'kind' => %w(carried permanent permanent_carried)[kind], 'kind_code' => kind,
              'hash' => format('%016x', h) }
      r.fail!("unknown irep kind #{kind}", at) unless kind <= 2
      return rec.merge('bytes' => r.pos - start) if kind == 1
      nlocals, nregs, clen, ilen = r.u16, r.u16, r.u16, r.u32
      code = r.take(ilen + 13 * clen)
      iseq = code.byteslice(0, ilen)
      catches = Array.new(clen) do |i|
        t, b, e, tgt = code.unpack('CNNN', offset: ilen + 13 * i)
        { 'type' => t == 0 ? 'rescue' : (t == 1 ? 'ensure' : t), 'begin' => b, 'end' => e, 'target' => tgt }
      end
      raw_pool = []
      pool = Array.new(r.u16) do |i|
        r.where = "ireps > @#{id} > pool #{i}"
        at = r.pos
        case (pk = r.u8)
        when 0 then raw_pool << [:s, s = r.str]; { 'string' => CheckpointImage.json_bytes(s) }
        when 1 then raw_pool << [:i, v = r.i64]; { 'int' => CheckpointImage.json_int(v) }
        when 2 then raw_pool << [:f, v = r.u64]; CheckpointImage.json_float([v].pack('Q>').unpack1('G'))
        when 3 then raw_pool << [:b, s = r.str]; { 'bigint_hex' => s.unpack1('H*') }
        else r.fail!("unknown pool kind #{pk}", at)
        end
      end
      r.where = "ireps > @#{id} > symbols"
      sym_ids = Array.new(r.u16) { sym_id }
      syms = sym_ids.map { |i| i && @syms[i - 1] }
      r.where = "ireps > @#{id} > children"
      reps = Array.new(r.u16) { irep_ref }
      r.where = "ireps > @#{id} > local names"
      at = r.pos
      nlv = r.u16
      r.fail!("#{nlv} local names for #{nlocals} locals", at) unless nlv.zero? || nlv == nlocals - 1
      lv = Array.new(nlv) { sym }
      rec.merge!('nlocals' => nlocals, 'nregs' => nregs, 'ilen' => ilen, 'catch' => catches,
                 'pool' => pool, 'syms' => syms, 'children' => reps, 'locals' => lv,
                 'iseq' => iseq.unpack1('H*'), 'bytes' => r.pos - start)
      @hash_input ||= {}
      @hash_input[id] = [nlocals, nregs, clen, ilen, code, raw_pool, sym_ids.map { |i| i && @syms_raw[i - 1] }, reps, h]
      rec
    end

    # -- objects (§7) --

    def object(id)
      r = @r
      r.where = "objects > ##{id}"
      start = r.pos
      at = r.pos
      k = r.u8
      kind = OBJECT_KINDS[k] or r.fail!("unknown object kind #{k}", at)
      r.where = "objects > ##{id} (#{kind})"
      o = { 'id' => id, 'kind' => kind, 'kind_code' => k }
      case k
      when 1
        o['name'] = r.text
      when 15
        o['name'] = r.text
        o['irep'] = irep_ref(false)
      else
        o['class'] = obj_ref(CLASSY)
        o['frozen'] = r.u8 == 1
        object_body(o, k)
      end
      o['bytes'] = r.pos - start
      o
    end

    def object_body(o, k)
      r = @r
      case k
      when 2 then o['ivars'] = ivars
      when 3, 25
        o.merge!(CheckpointImage.json_bytes(r.str))
        o['binary'] = k == 25
      when 4, 17
        n = count('items')
        o['items'] = Array.new(n) { value }
        o['ivars'] = ivars if k == 17
      when 5
        fl = r.u8
        o['default'] = { 'has_default' => fl & 1 == 1, 'proc' => fl & 2 == 2 }
        n = count('pairs')
        o['pairs'] = Array.new(n) { [value, value] }
        o['ivars'] = ivars
      when 6
        o['begin'] = value
        o['end'] = value
        fl = r.u8
        o['exclusive'] = fl & 1 == 1
        o['uninitialized'] = fl & 2 == 2
      when 7
        o['message'] = value
        o['backtrace'] = value
        o['ivars'] = ivars
      when 8, 9, 10
        o['superclass'] = obj_ref(CLASSY)
        at = r.pos
        it = r.u8
        r.fail!("unknown instance type #{it}", at) unless it < INSTANCE_TYPES.size
        o['instance_type'] = INSTANCE_TYPES[it]
        fl = r.u8
        o['flags'] = { 'no_allocate' => fl & 1 == 1, 'inherited' => fl & 2 == 2,
                       'prepended' => fl & 4 == 4, 'has_method_table' => fl & 8 == 8 }
        o['ivars'] = ivars
        o['methods'] = methods_list
      when 11 then o['superclass'] = obj_ref(CLASSY)
      when 12
        fl = r.u8
        o['lambda'] = fl & 1 == 1
        o['orphan'] = fl & 2 == 2
        o['scope'] = fl & 4 == 4
        at = r.pos
        o['body'] =
          case (bk = r.u8)
          when 0 then { 'kind' => 'irep', 'irep' => irep_ref(false) }
          when 1
            name = r.text
            a = r.u32
            { 'kind' => 'cfunc', 'name' => name, 'aspec' => a & 0x80000000 != 0 ? a & 0x7fffffff : nil }
          when 2 then { 'kind' => 'alias', 'method' => sym }
          else r.fail!("unknown proc body #{bk}", at)
          end
        o['upper'] = obj_ref(PROCS)
        o['env'] = env_link('proc environment')
      when 13
        o['method'] = sym
        o['length'] = r.u8
        o['block_reg'] = r.u8
        o['visibility'] = visibility_flags(r.u8)
        at = r.pos
        case (s = r.u8)
        when 1
          o['on_stack'] = true
          o['context'] = ctx_ref
          o['position'] = r.u32
        when 0
          o['on_stack'] = false
          o['values'] = Array.new(o['length']) { value }
        else r.fail!("Env stack mark #{s} is not 0 or 1", at)
        end
      when 14
        o['type'] = r.text
        blob = r.str
        o['data_hex'] = blob.unpack1('H*')
        d = decode_data(o['type'], blob)
        o['decoded'] = d if d
        o['ivars'] = ivars
      when 16
        o['negative'] = r.u8 == 1
        hex = r.text
        r.fail!("Integer digits #{hex[0, 20].inspect} are not hex") unless hex.match?(/\A[0-9a-f]+\z/)
        o['hex'] = hex
        o['value'] = (o['negative'] ? '-' : '') + hex.to_i(16).to_s
      when 18
        n = r.u32
        o['uninitialized'] = n == NONE32
        unless o['uninitialized']
          r.fail!("#{n} Set items cannot fit") if n > r.rest
          o['items'] = Array.new(n) { value }
        end
        o['ivars'] = ivars
      when 19
        o['numerator'] = r.text
        o['denominator'] = r.text
      when 20
        o['real'] = CheckpointImage.json_float(r.f64)
        o['imaginary'] = CheckpointImage.json_float(r.f64)
      when 21
        at = r.pos
        t = r.u8
        o['tag'] = %w(break jump stop)[t] || r.fail!("unknown break tag #{t}", at)
        o['frame'] = r.u32
        o['value'] = value
      when 23
        o['state'] = format('%016x', r.u64)
        o['seed'] = r.u32
        o['ivars'] = ivars
      when 24
        o['superclass'] = obj_ref(CLASSY)
        o['has_methods'] = r.u8 == 1
        o['methods'] = methods_list
      when 26
        o['task'] = task_ref
        o['ivars'] = ivars
      when 27
        o['context'] = ctx_ref
      end
    end

    def env_link(what)
      at = @r.pos
      case (k = @r.u8)
      when 0 then @r.u32; nil
      when 1 then { 'kind' => 'env', 'ref' => obj_ref(%w(env permanent)) }
      when 2 then { 'kind' => 'class', 'ref' => obj_ref(CLASSY) }
      else @r.fail!("unknown #{what} kind #{k}", at)
      end
    end

    def visibility_flags(v)
      { 'default' => VISIBILITY[v & 3] || (v & 3), 'separated' => v & 4 == 4, 'module_function' => v & 8 == 8 }
    end

    def decode_data(type, blob)
      b = Bytes.new(blob)
      b.where = @r.where + " > #{type} contents"
      d =
        case type
        when 'Time' then { 'sec' => b.i64, 'nsec' => b.i64, 'zone' => { 1 => 'utc', 2 => 'local' }[b.u8] || 'other' }
        when 'MatchData'
          n = b.u64
          { 'captures' => Array.new(n) { [b.i64, b.i64] } }
        when 'CombinationState'
          k = b.u64
          d = { 'k' => k, 'n' => b.u64, 'mode' => b.u64 }
          d['indices'] = b.u8 == 1 ? Array.new(k) { b.u64 } : nil
          d
        when 'ary_product_generator' then { 'total' => b.u64, 'cursor' => b.u64 }
        when 'Task::Queue' then { 'closed' => b.u8 == 1 }
        when 'Checkpoint::Box' then { 'value' => CheckpointImage.json_int(b.u64) }
        else return nil
        end
      @doc['warnings'] << "#{@r.where}: #{b.rest} bytes left in the #{type} contents" unless b.eof?
      d
    rescue FormatError => e
      @doc['warnings'] << "#{@r.where}: the #{type} contents do not decode: #{e.reason}"
      nil
    end

    # -- changes (§9) --

    def change(i)
      r = @r
      r.where = "changes > #{i}"
      at = r.pos
      k = r.u8
      kind = CHANGE_KINDS[k] or r.fail!("unknown change kind #{k}", at)
      c = { 'kind' => kind, 'kind_code' => k }
      case k
      when 1 then c['target'] = obj_ref(%w(permanent)); c['method'] = method_entry
      when 2 then c['target'] = obj_ref(%w(permanent)); c['name'] = sym
      when 3 then c['target'] = obj_ref(%w(permanent)); c['name'] = sym; c['value'] = value
      when 4 then c['target'] = obj_ref(%w(permanent)); c['name'] = sym
      when 5 then c['target'] = obj_ref(%w(permanent)); c['superclass'] = obj_ref(CLASSY)
      when 6 then c['name'] = sym; c['value'] = value
      when 7 then c['name'] = sym
      when 8 then c['target'] = obj_ref(%w(permanent)); c['state'] = format('%016x', r.u64); c['seed'] = r.u32
      when 9 then c['target'] = obj_ref(%w(permanent)); c['prepended'] = r.u8 & 1 == 1
      when 10
        c['target'] = obj_ref(%w(permanent))
        c['frozen'] = r.u8 == 1
        at = r.pos
        t = r.u8
        case t
        when 3 then c['type'] = 'string'; c.merge!(CheckpointImage.json_bytes(r.str))
        when 4 then c['type'] = 'array'; c['items'] = Array.new(count('items')) { value }
        when 5 then c['type'] = 'hash'; c['pairs'] = Array.new(count('pairs')) { [value, value] }
        else r.fail!("unknown contents type #{t}", at)
        end
      end
      c
    end

    # -- contexts (§10) --

    def context(id)
      r = @r
      r.where = "contexts > #{id}"
      at = r.pos
      st = r.u8
      r.fail!("unknown context state #{st}", at) unless st < CONTEXT_STATES.size
      fl = r.u8
      c = { 'id' => id, 'state' => CONTEXT_STATES[st], 'state_code' => st, 'vmexec' => fl & 1 == 1,
            'prev' => ctx_ref, 'fiber' => obj_ref(%w(fiber)) }
      r.where = "contexts > #{id} > stack"
      n = count('stack values')
      c['stack'] = Array.new(n) { value }
      r.where = "contexts > #{id} > frames"
      nf = count('frames')
      c['frames'] = Array.new(nf) { |k| frame(id, k) }
      c
    end

    def frame(cid, k)
      r = @r
      r.where = "contexts > #{cid} > frame #{k}"
      at = r.pos
      fk = r.u8
      r.fail!("unknown frame kind #{fk}", at) unless fk <= 1
      f = { 'index' => k, 'kind' => fk == 0 ? 'ruby' : 'cfunc', 'method' => sym, 'proc' => obj_ref(PROCS) }
      f['cfunc'] = fk == 1 ? r.text : nil
      f['reg_base'] = r.u32
      pc = r.u32
      f['pc'] = pc == NONE32 ? nil : pc
      f['argc'] = r.u8
      f['kwargc'] = r.u8
      f['cci'] = r.u8
      f['visibility'] = visibility_flags(r.u8)
      f['block'] = obj_ref(PROCS)
      f['env'] = env_link('frame environment')
      f
    end

    # -- the scheduler (§11) --

    def scheduler
      r = @r
      r.where = 'scheduler'
      fl = r.u8
      s = { 'switching' => fl & 1 == 1, 'loop' => fl & 2 == 2, 'tick_ms' => r.u16, 'tick' => r.u32 }
      has = r.u8
      wk = r.i32
      s['next_wakeup'] = has == 1 ? wk : nil
      n = count('tasks')
      s['tasks'] = Array.new(n) { |i| task(i + 1) }
      r.where = 'scheduler'
      at = r.pos
      s['running_task'] = (v = r.u32).zero? ? nil : v
      r.fail!("running Task #{v} is not in 1..#{n}", at) if v > n
      at = r.pos
      s['host_task'] = (v = r.u32).zero? ? nil : v
      r.fail!("host Task #{v} is not in 1..#{n}", at) if v > n
      @doc['scheduler'] = s
    end

    def task(id)
      r = @r
      r.where = "scheduler > Task #{id}"
      t = { 'id' => id, 'object' => obj_ref(%w(task)), 'context' => ctx_ref }
      at = r.pos
      st = r.u8
      t['state'] = TASK_STATES[st] || r.fail!("unknown Task state #{st}", at)
      t['state_code'] = st
      at = r.pos
      q = r.u8
      r.fail!("unknown Task queue #{q}", at) unless QUEUES.key?(q)
      t['queue'] = QUEUES[q]
      t['priority'] = r.u8
      at = r.pos
      reason = r.u8
      r.fail!("unknown wait reason #{reason}", at) unless WAIT_REASONS.key?(reason)
      t['timeslice'] = r.u8
      t['name'] = value
      t['value'] = value
      t['wait'] =
        case reason
        when 0 then nil
        when 1 then { 'reason' => 'sleep', 'ticks_left' => r.i32 }
        when 4 then { 'reason' => 'join', 'task' => task_ref }
        when 8
          w = { 'reason' => 'queue', 'queue' => obj_ref(%w(data permanent)) }
          has = r.u8
          rem = r.i32
          w['ticks_left'] = has == 1 ? rem : nil
          w
        end
      t
    end

    # -- the optional sections (§13) --

    def sections
      r = @r
      loop do
        r.where = 'sections'
        at = r.pos
        r.fail!('no end mark END0: the image is cut short') if r.rest < 4
        tag = r.take(4)
        case tag
        when 'END0'
          r.where = 'END0'
          r.fail!("#{r.rest} bytes after the end mark END0") unless r.eof?
          return
        when 'DBG0' then @doc['sections'] << dbg0(at)
        when 'GCS0'
          r.where = 'sections > GCS0'
          fl = r.u8
          @doc['sections'] << {
            'tag' => 'GCS0', 'offset' => at,
            'disabled' => fl & 1 == 1, 'generational' => fl & 2 == 2, 'step_per_allocation' => fl & 4 == 4,
            'scheduler_steps' => fl & 8 == 8, 'interval_ratio' => r.u32, 'step_ratio' => r.u32,
            'step_limit' => r.u64.to_s, 'malloc_threshold' => r.u64.to_s, 'debt_limit' => r.i64.to_s,
          }
        else
          # A section this reader does not know: the format gives it no length,
          # so everything up to the final END0 is taken as its contents (later
          # sections, if any, are inside it). Readers that resume refuse it.
          r.where = "sections > #{tag.inspect}"
          r.fail!('an unknown section and no END0 at the end of the file') unless r.slice(r.size - 4, 4) == 'END0'
          body = r.take(r.rest - 4)
          @doc['sections'] << {
            'tag' => tag.match?(/\A[\x20-\x7e]{4}\z/n) ? tag : tag.unpack1('H*'),
            'unknown' => true, 'offset' => at, 'bytes' => body.bytesize,
            'head_hex' => body.byteslice(0, 64).unpack1('H*'), 'tail_hex' => body.bytesize > 64 ? body.byteslice(-16, 16).unpack1('H*') : nil,
          }
          @doc['warnings'] << "unknown section #{tag.inspect} at byte #{at}: kept as hex up to the final END0"
        end
      end
    end

    def dbg0(at)
      r = @r
      r.where = 'sections > DBG0'
      n = count('line tables')
      list = Array.new(n) do |i|
        r.where = "sections > DBG0 > #{i + 1}"
        id = irep_ref(false)
        files = Array.new(r.u16) do |j|
          r.where = "sections > DBG0 > #{i + 1} (irep @#{id}) > file #{j}"
          name = r.text
          st = r.u32
          en = r.u32
          r.fail!("file range #{st}...#{en} is empty or reversed") if en < st
          m = count('line entries')
          lines = Array.new(m) { [r.u32, r.u32] }
          { 'file' => name, 'start' => st, 'end' => en, 'lines' => lines }
        end
        { 'irep' => id, 'files' => files }
      end
      { 'tag' => 'DBG0', 'offset' => at, 'ireps' => list }
    end
  end
end

require_relative 'checkpoint_derive'
