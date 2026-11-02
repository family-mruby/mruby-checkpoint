# Print a checkpoint image (format v0, with the P2 additions) in readable form.
#
#   ruby tools/checkpoint_inspect.rb IMAGE [--iseq]
#
# Written from doc/format_v0.md alone, as a second reader of the format.

class Reader
  def initialize(bytes) = (@b = bytes; @p = 0)
  def eof? = @p >= @b.bytesize
  def rest = @b.bytesize - @p
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
          14 => 'Data' }
nobj = r.u32
puts "objects: #{nobj}"
nobj.times do |i|
  k = r.u8
  if k == 1
    puts "  ##{i + 1} = #{r.str}"
    next
  end
  if k == 15
    puts "  ##{i + 1} = #{r.str} (runs irep @#{r.u32})"
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
      b = bodyk == 0 ? "irep @#{r.u32}" : (f = r.str; a = r.u32; "C #{f} aspec=#{a.to_s(16)}")
      up = r.u32
      ek = r.u8
      e = r.u32
      "flags=#{fl} #{b} upper=##{up} #{%w(none env class)[ek]}=##{e}"
    when 13
      mid = sym.(r.u32); len = r.u8; bidx = r.u8; v = r.u8
      if r.u8 == 1
        "mid=#{mid} len=#{len} bidx=#{bidx} vis=#{v} on stack at #{r.u32}"
      else
        "mid=#{mid} len=#{len} bidx=#{bidx} vis=#{v} [#{Array.new(len) { value.() }.join(', ')}]"
      end
    when 14 then n = r.str; blob = r.str; "#{n} #{blob.unpack1('H*')} #{ivars.()}"
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
  else raise "bad change #{k}"
  end
end

len = r.u32
stack = Array.new(len) { value.() }
puts "stack: #{len}"
stack.each_slice(8).with_index { |s, i| puts "  [#{i * 8}] #{s.join(' ')}" }
nf = r.u32
puts "frames: #{nf}"
nf.times do |k|
  mid = sym.(r.u32); proc_ = r.u32; off = r.u32; pc = r.u32
  n, nk, cci, v = r.u8, r.u8, r.u8, r.u8
  blk = r.u32; uk = r.u8; u = r.u32
  puts "  #{k}: #{mid} proc=##{proc_} R0=stack[#{off}] pc=#{pc} n=#{n} nk=#{nk} cci=#{cci} vis=#{v} " \
       "blk=##{blk} #{%w(none env class)[uk]}=##{u}"
end
puts r.eof? ? 'end' : "#{r.rest} bytes left!"
