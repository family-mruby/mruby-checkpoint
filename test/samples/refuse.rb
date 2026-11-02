# checkpoint-test: refuse
# The state holds what format v0 cannot carry: an open file (a resource of
# this process) and a running Fiber. The suspend is refused with the reason
# (Checkpoint::Error), nothing is written and the program goes on.
t = File.open(__FILE__)
begin
  Checkpoint.suspend("x.ckpt")
  puts "not reached"
rescue Checkpoint::Error => e
  puts "refused: #{e.message}"
end
t.close
t = nil

f = Fiber.new do
  begin
    Checkpoint.suspend("x.ckpt")
  rescue Checkpoint::Error => e
    puts "refused in fiber: #{e.message}"
  end
  :fiber_done
end
puts f.resume
puts "still running"
