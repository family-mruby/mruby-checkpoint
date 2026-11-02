# tools/test_inspect.rb: frames that have not started (pc 0). An image is
# written inside Task "cons" while a Fiber made with Fiber.new has not been
# resumed and Task "late" has not run yet. The inspector must show where each
# will start (the first line of its code), the self the Fiber's block will
# get, and what each context is doing (the Task's state, not the context's
# status, which mruby-task leaves at "created").
def run_tasks(*tasks)
  tasks.each(&:join)   # the program is a Task (checkpoint run --as-task)
rescue RuntimeError
  Task.run             # the program is at the root
end

gen = Fiber.new do |n|
  puts "gen starts"
  Fiber.yield n + 1
end
cons = Task.new(name: "cons", priority: 100) do
  got = []
  Checkpoint.save("unstarted.ckpt")
  got << gen.resume(1)
  got
end
late = Task.new(name: "late", priority: 200) do
  puts "late starts"
  :late
end
run_tasks(cons, late)
p [cons.value, late.value]
