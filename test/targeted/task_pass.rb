# Task.pass as the one instruction between an external request and its safe
# point (tools/targeted.rb runs it with checkpoint-fuzz --at-send pass: the
# request is raised right before every call of `pass`). Two Tasks of the same
# priority take turns only through Task.pass (the virtual clock does not end
# a timeslice here), so a pass that the image loses shows in the order.

def run_both(a, b)
  a.join
  b.join
rescue RuntimeError
  # the top-level program: the root runs the scheduler (join is for Tasks)
  Task.run
end

assert('Task.pass right after an external request') do
  log = []
  a = Task.new do
    i = 0
    while i < 4
      log << "a#{i}"
      Task.pass
      i += 1
    end
  end
  b = Task.new do
    i = 0
    while i < 4
      log << "b#{i}"
      Task.pass
      i += 1
    end
  end
  run_both(a, b)
  assert_equal %w(a0 b0 a1 b1 a2 b2 a3 b3), log
end
