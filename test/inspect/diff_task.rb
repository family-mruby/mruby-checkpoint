# tools/test_inspect.rb: `checkpoint_inspect.rb diff` between two saves in a
# Task while another Task sleeps (virtual clock): the clock, the sleeping
# Task's ticks left and what it logged between the saves.
def run_tasks(*tasks)
  tasks.each(&:join)   # the program is a Task (checkpoint run --as-task)
rescue RuntimeError
  Task.run             # the program is at the root
end

log = []
a = Task.new(name: "a", priority: 140) do
  2.times do |i|
    sleep_ms 40
    log << i
  end
  :a
end
s = Task.new(name: "s", priority: 150) do
  sleep_ms 20
  Checkpoint.save("task_a.ckpt")   # a sleeps (5 ticks left), the log is empty
  sleep_ms 28
  # a woke on tick 10, logged 0 and sleeps again (8 ticks left)
  Checkpoint.save("task_b.ckpt")
  :s
end
run_tasks(a, s)
p [log, a.value, s.value]
