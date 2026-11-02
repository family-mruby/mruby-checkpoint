# tools/test_inspect.rb: the call stacks that checkpoint_inspect.rb --stack
# derives from an image must be the backtraces mruby itself prints.
# At every point the program saves an image and raises on the same line
# (a "BT" line: the backtrace of the context that was running). A context
# that waited when an image was written raises on the line where it waited,
# as soon as it goes on (a "WAIT" line: its Ruby frames in that image).
# Run as the top-level program and as a Task (--as-task), on the virtual
# clock (the same switches in every run).
def run_tasks(*tasks)
  tasks.each(&:join)   # the program is a Task (checkpoint run --as-task)
rescue RuntimeError
  Task.run             # the program is at the root
end

def deep(n)
  if n == 0
    x = [n, :bottom]
    Checkpoint.save("deep.ckpt"); begin; raise "bt"; rescue => e; puts "BT deep #{e.backtrace.join('|')}"; end
    x
  else
    deep(n - 1)
  end
end

def each_of(a)
  a.each { |v| yield v }   # Array#each is mrblib's: its frame has lines too
end

deep(2)
total = 0
each_of([5]) do |v|
  total += v
  Checkpoint.save("each.ckpt"); begin; raise "bt"; rescue => e; puts "BT each #{e.backtrace.join('|')}"; end
end

f = Fiber.new do |a|
  b = a + 1
  Checkpoint.save("fiber.ckpt"); begin; raise "bt"; rescue => e; puts "BT fiber #{e.backtrace.join('|')}"; end
  Fiber.yield b
end
r = f.resume(1); begin; raise "bt"; rescue => e; puts "WAIT fiber prev #{e.backtrace.join('|')}"; end

t1 = Task.new(name: "t1") do
  k = [7]
  Task.pass; begin; raise "bt"; rescue => e; puts "WAIT task t1 #{e.backtrace.join('|')}"; end
  k
end
t2 = Task.new(name: "t2") do
  m = { m: 1 }
  [1].each do |i|
    Checkpoint.save("task.ckpt"); begin; raise "bt"; rescue => e; puts "BT task #{e.backtrace.join('|')}"; end
  end
  m
end
run_tasks(t1, t2)
p [r, total, t1.value, t2.value]
