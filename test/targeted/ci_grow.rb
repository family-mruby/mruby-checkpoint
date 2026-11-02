# The image is written while the writer calls a method of the program's
# objects (Rational#numerator and #denominator, C methods), at a safe point
# where the call-info stack is full: a Task's starts with room for 4 frames,
# and the first `mark` is the 4th frame (the Task's block, level1, level2,
# mark), so the writer's call is the one that moves the stack. The VM must
# take its frame again after the safe point
# (patches/0001-vm-safepoint-hook.patch); before, it went on with the freed
# one (AddressSanitizer: heap-use-after-free). tools/targeted.rb runs it with
# checkpoint-fuzz --at-send mark (the safe point is the first instruction of
# `mark`); the later points are after a restore, which gives the stack room.

def mark
end

def level2(r)
  mark
  r * 2
end

def level1(r)
  level2(r) + 1
end

def run_one(t)
  t.join
rescue RuntimeError
  Task.run   # the top-level program: the root runs the scheduler
end

assert('an image written where the call-info stack is full') do
  t = Task.new { level1(Rational(3, 2)) }
  run_one(t)
  assert_equal Rational(4, 1), t.value
end
