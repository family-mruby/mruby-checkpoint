# Print a checkpoint image (format v1) in readable form.
#
#   ruby tools/checkpoint_inspect.rb IMAGE [--iseq]
#
# Written from doc/format_v1.md alone, as a second reader of the format.

class Reader
  def initialize(bytes) = (@b = bytes; @p = 0)
  def eof? = @p >= @b.bytesize
  def rest = @b.bytesize - @p
  def peek(n) = @b.byteslice(@p, n)
  def u8 = take(1).unpack1('C')
  def u16 = take(2).unpack1('n')
  def u32 = take(4).unpack1('N')
  def u64 = take(8).unpack1('Q>')
  def i64 = take(8).unpack1('q>')
  def str = take(u32)
  def take(n)
    raise "truncated at #{@p}" if @p + n > @b.bytesize
    s = @b.byteslice(@p, n)
    @p += n
    s
  end
end

path = ARGV.find { |a| !a.start_with?('--') } or abort 'usage: checkpoint_inspect.rb IMAGE [--iseq]'
show_iseq = ARGV.include?('--iseq')
r = Reader.new(File.binread(path))

abort 'not a checkpoint image' unless r.take(4) == 'MRCK'
ver = r.u16
abort "format v#{ver}: this reader reads v1" unless ver == 1
impl = r.str
rite = r.str
ib, fb, utf8, pw = r.u8, r.u8, r.u8, r.u8
puts "format v#{ver}, #{impl}, #{rite}, Integer #{ib} bit, Float #{fb} bit, utf8=#{utf8}, writer #{pw}-bit"

syms = Array.new(r.u32) { r.str }
sym = ->(i) { i.zero? ? '-' : ":#{syms[i - 1]}" }
puts "symbols: #{syms.size}"

value = lambda do
  case (t = r.u8)
  when 0 then 'nil'
  when 1 then 'false'
  when 2 then 'true'
  when 3 then r.i64.to_s
  when 4 then [r.u64].pack('Q>').unpack1('G').to_s
  when 5 then sym.(r.u32)
  when 6 then "##{r.u32}"
  when 7 then 'undef'
  else raise "bad value tag #{t}"
  end
end
ivars = -> { Array.new(r.u32) { "#{sym.(r.u32)}=#{value.()}" }.join(' ') }
vis = %w(public private protected default)
meth = lambda do
  name = sym.(r.u32)
  v = vis[r.u8]
  case r.u8
  when 0 then "#{name}(#{v})->##{r.u32}"
  when 1 then f = r.str; a = r.u32; "#{name}(#{v})->C #{f} aspec=#{a.to_s(16)}"
  when 2 then "#{name} undef"
  end
end

nirep = r.u32
puts "ireps: #{nirep}"
nirep.times do |i|
  kind = r.u8
  h = r.u64
  if kind == 1
    puts "  @#{i + 1} permanent #{h.to_s(16)}"
    next
  end
  raise "bad irep kind #{kind}" unless kind == 0 || kind == 2
  tag = kind == 2 ? 'permanent, carried ' : ''
  nlocals, nregs, clen, ilen = r.u16, r.u16, r.u16, r.u32
  iseq = r.take(ilen + 13 * clen)
  pool = Array.new(r.u16) do
    case r.u8
    when 0 then r.str.inspect
    when 1 then r.i64.to_s
    when 2 then [r.u64].pack('Q>').unpack1('G').to_s
    when 3 then "bigint(#{r.str.bytesize})"
    end
  end
  isyms = Array.new(r.u16) { sym.(r.u32) }
  reps = Array.new(r.u16) { "@#{r.u32}" }
  lv = Array.new(r.u16) { sym.(r.u32) }
  puts "  @#{i + 1} #{tag}#{h.to_s(16)} locals=#{nlocals} regs=#{nregs} iseq=#{ilen}B catch=#{clen} " \
       "pool=#{pool.inspect} syms=#{isyms.join(',')} reps=#{reps.join(',')} lv=#{lv.join(',')}"
  puts "     #{iseq.unpack1('H*')}" if show_iseq
end

kinds = { 2 => 'Object', 3 => 'String', 4 => 'Array', 5 => 'Hash', 6 => 'Range', 7 => 'Exception',
          8 => 'Class', 9 => 'Module', 10 => 'SClass', 11 => 'IClass', 12 => 'Proc', 13 => 'Env',
          14 => 'Data', 16 => 'Bigint', 17 => 'Struct', 18 => 'Set', 19 => 'Rational',
          20 => 'Complex', 21 => 'Break', 23 => 'Random', 24 => 'Origin', 25 => 'String',
          26 => 'Task', 27 => 'Fiber' }
nobj = r.u32
puts "objects: #{nobj}"
proc_irep = {}   # Proc object -> the irep it runs (for the lines of the frames)
nobj.times do |i|
  k = r.u8
  if k == 1
    puts "  ##{i + 1} = #{r.str}"
    next
  end
  if k == 15
    n = r.str; ir = r.u32
    proc_irep[i + 1] = ir
    puts "  ##{i + 1} = #{n} (runs irep @#{ir})"
    next
  end
  klass = r.u32
  frozen = r.u8 == 1 ? ' frozen' : ''
  head = "  ##{i + 1} #{kinds[k]} class=##{klass}#{frozen}"
  body =
    case k
    when 2 then ivars.()
    when 3 then r.str.inspect
    when 4 then "[#{Array.new(r.u32) { value.() }.join(', ')}]"
    when 5
      fl = r.u8
      pairs = Array.new(r.u32) { "#{value.()}=>#{value.()}" }.join(', ')
      "{#{pairs}} default=#{fl} #{ivars.()}"
    when 6 then b = value.(); e = value.(); "#{b}#{r.u8 == 1 ? '...' : '..'}#{e}"
    when 7 then "mesg=#{value.()} bt=#{value.()} #{ivars.()}"
    when 8, 9, 10
      sup = r.u32; it = r.u8; fl = r.u8
      iv = ivars.()
      ms = Array.new(r.u32) { meth.() }
      "super=##{sup} inst=#{it} flags=#{fl} #{iv} methods: #{ms.join(' ')}"
    when 11 then "super=##{r.u32}"
    when 12
      fl = r.u8
      bodyk = r.u8
      b = case bodyk
          when 0 then ir = r.u32; proc_irep[i + 1] = ir; "irep @#{ir}"
          when 1 then f = r.str; a = r.u32; "C #{f} aspec=#{a.to_s(16)}"
          when 2 then "alias of #{sym.(r.u32)}"
          else raise "bad proc body #{bodyk}"
          end
      up = r.u32
      ek = r.u8
      e = r.u32
      "flags=#{fl} #{b} upper=##{up} #{%w(none env class)[ek]}=##{e}"
    when 13
      mid = sym.(r.u32); len = r.u8; bidx = r.u8; v = r.u8
      if r.u8 == 1
        cx = r.u32
        "mid=#{mid} len=#{len} bidx=#{bidx} vis=#{v} on the stack of context #{cx} at #{r.u32}"
      else
        "mid=#{mid} len=#{len} bidx=#{bidx} vis=#{v} [#{Array.new(len) { value.() }.join(', ')}]"
      end
    when 14 then n = r.str; blob = r.str; "#{n.empty? ? '(not initialized)' : n} #{blob.unpack1('H*')} #{ivars.()}"
    when 16 then neg = r.u8 == 1 ? '-' : ''; "#{neg}0x#{r.str}"
    when 17 then "[#{Array.new(r.u32) { value.() }.join(', ')}] #{ivars.()}"
    when 18
      n = r.u32
      n == 0xffffffff ? "(not initialized) #{ivars.()}" : "{#{Array.new(n) { value.() }.join(', ')}} #{ivars.()}"
    when 19 then "#{r.str}/#{r.str}"
    when 20 then "(#{[r.u64].pack('Q').unpack1('D')}+#{[r.u64].pack('Q').unpack1('D')}i)"
    when 21 then "tag=#{r.u8} frame=#{r.u32} value=#{value.()}"
    when 23 then "state=#{r.u64.to_s(16)} seed=#{r.u32} #{ivars.()}"
    when 24
      sup = r.u32; mt = r.u8
      "super=##{sup} table=#{mt} methods: #{Array.new(r.u32) { meth.() }.join(' ')}"
    when 25 then "#{r.str.inspect} (binary)"
    when 26 then "task=#{r.u32} #{ivars.()}"
    when 27 then cx = r.u32; cx.zero? ? '(not initialized)' : "context #{cx}"
    else raise "bad kind #{k}"
    end
  puts "#{head} #{body}"
end

npatch = r.u32
puts "changes: #{npatch}"
npatch.times do
  case (k = r.u8)
  when 1 then puts "  def ##{r.u32} #{meth.()}"
  when 2 then puts "  remove method ##{r.u32} #{sym.(r.u32)}"
  when 3 then puts "  set ##{r.u32} #{sym.(r.u32)} = #{value.()}"
  when 4 then puts "  remove ##{r.u32} #{sym.(r.u32)}"
  when 5 then puts "  super of ##{r.u32} = ##{r.u32}"
  when 6 then puts "  global #{sym.(r.u32)} = #{value.()}"
  when 7 then puts "  remove global #{sym.(r.u32)}"
  when 8 then puts "  Random ##{r.u32} state=#{r.u64.to_s(16)} seed=#{r.u32}"
  when 9 then puts "  class flags ##{r.u32} prepended=#{r.u8}"
  when 10
    o = r.u32; fr = r.u8; t = r.u8
    c = case t
        when 3 then r.str.inspect
        when 4 then "[#{Array.new(r.u32) { value.() }.join(', ')}]"
        when 5 then "{#{Array.new(r.u32) { "#{value.()}=>#{value.()}" }.join(', ')}}"
        else raise "bad contents type #{t}"
        end
    puts "  contents ##{o} frozen=#{fr} #{c}"
  else raise "bad change #{k}"
  end
end

fstates = %w(created running resumed suspended transferred terminated)
frames = []   # [context, frame, method, proc, pc] of the Ruby frames
nctx = r.u32
puts "contexts: #{nctx}"
nctx.times do |c|
  st = r.u8; fl = r.u8; prev = r.u32; fib = r.u32
  len = r.u32
  stack = Array.new(len) { value.() }
  puts "  context #{c + 1}: #{fstates[st] || st}#{fl & 1 == 1 ? ' vmexec' : ''} prev=#{prev} fiber=##{fib} stack=#{len}"
  stack.each_slice(8).with_index { |s, i| puts "    [#{i * 8}] #{s.join(' ')}" }
  nf = r.u32
  nf.times do |k|
    fk = r.u8
    mid = sym.(r.u32); proc_ = r.u32
    fname = fk == 1 ? " C #{r.str}" : ''
    off = r.u32; pc = r.u32
    frames << [c + 1, k, mid, proc_, pc] if fk == 0 && pc != 0xffffffff
    n, nk, cci, v = r.u8, r.u8, r.u8, r.u8
    blk = r.u32; uk = r.u8; u = r.u32
    puts "    #{k}: #{mid}#{fname} proc=##{proc_} R0=stack[#{off}] pc=#{pc == 0xffffffff ? '-' : pc} n=#{n} nk=#{nk} " \
         "cci=#{cci} vis=#{v} blk=##{blk} #{%w(none env class)[uk]}=##{u}"
  end
end
puts "current context: #{r.u32}"
i32 = -> { [r.u32].pack('N').unpack1('l>') }
sfl = r.u8; unit = r.u16; tick = r.u32; hw = r.u8; wk = i32.()
puts "scheduler: switch=#{sfl & 1} loop=#{(sfl >> 1) & 1} tick=#{tick} (#{unit} ms) next wakeup #{hw == 1 ? "in #{wk}" : 'none'}"
tstat = { 0 => 'DORMANT', 2 => 'READY', 3 => 'RUNNING', 4 => 'WAITING', 8 => 'SUSPENDED' }
ntask = r.u32
ntask.times do |i|
  obj = r.u32; cx = r.u32; st = r.u8; q = r.u8; pri = r.u8; reason = r.u8; ts = r.u8
  name = value.(); res = value.()
  wait = case reason
         when 0 then ''
         when 1 then " sleep, #{i32.()} ticks left"
         when 4 then " join Task #{r.u32}"
         when 8 then qo = r.u32; hd = r.u8; rem = i32.(); " queue ##{qo}#{hd == 1 ? ", #{rem} ticks left" : ''}"
         else raise "bad reason #{reason}"
         end
  where = q == 255 ? 'main (Task.current at the root)' : "queue #{%w(dormant ready waiting suspended)[q]}"
  puts "  Task #{i + 1}: ##{obj} context #{cx} #{tstat[st] || st} #{where} priority=#{pri} timeslice=#{ts} " \
       "name=#{name} value=#{res}#{wait}"
end
puts "running Task: #{r.u32}, host Task: #{r.u32}"
lines = {}   # irep -> [[file, start, end, [[pc, line], ...]], ...]
if r.peek(4) == 'DBG0'
  r.take(4)
  nd = r.u32
  puts "line numbers: #{nd} ireps"
  nd.times do
    id = r.u32
    lines[id] = Array.new(r.u16) do
      [r.str, r.u32, r.u32, Array.new(r.u32) { [r.u32, r.u32] }]
    end
    puts "  @#{id} #{lines[id].map { |f, st, en, e| "#{f}[#{st}...#{en}] #{e.size} changes" }.join(', ')}"
  end
end
# the line of each Ruby frame (the instruction before its pc: the one that
# is running, or the call it waits in), as a backtrace shows it
line_at = lambda do |id, pc|
  (lines[id] || []).each do |f, st, en, ents|
    next unless pc >= st && pc < en
    l = ents.select { |p, _| p <= pc }.last
    return "#{f}:#{l ? l[1] : '?'}"
  end
  nil
end
unless frames.empty?
  puts "frames:"
  frames.each do |cx, k, mid, pr, pc|
    id = proc_irep[pr]
    where = id && line_at.(id, pc > 0 ? pc - 1 : 0)
    puts "  context #{cx} frame #{k}: #{mid} irep @#{id || '?'} pc=#{pc} #{where || '(no line)'}"
  end
end
if r.peek(4) == 'GCS0'
  r.take(4)
  puts "GC settings: flags=#{r.u8} interval_ratio=#{r.u32} step_ratio=#{r.u32} step_limit=#{r.u64} " \
       "malloc_threshold=#{r.u64} debt_limit=#{r.i64}"
end
abort 'no end mark (END0): truncated' unless r.peek(4) == 'END0'
r.take(4)
puts r.eof? ? 'end' : "#{r.rest} bytes left!"
