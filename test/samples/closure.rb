# ghost-test: hop-after "before hop count=11 total=5"
# Closures that write outer local variables: one whose frame is still
# running (its env lives on the VM stack) and one whose frame has returned
# (its env lives on the heap).
def make_counter(start)
  count = start
  inc = lambda { count += 1 }
  get = proc { count }
  [inc, get]
end

inc, get = make_counter(10)
inc.call
total = 0
add = lambda { |x| total += x }
add.(5)
puts "before hop count=#{get.call} total=#{total}"
Ghost.hop("cl.ghost")
inc.call
inc.call
add.(7)
puts "after hop count=#{get.call} total=#{total}"
shared = []
pusher = proc { |v| shared << v; shared.size }
[1, 2].each { |v| pusher.call(v) }
Ghost.hop("cl.ghost")
pusher.call(3)
puts "shared #{shared.inspect} lambda?=#{inc.lambda?} #{pusher.lambda?}"
