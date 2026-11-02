# checkpoint-test: refuse
# checkpoint-test: no-as-task
# The state holds what cannot be carried: an open file (a resource of this
# process), and a suspend inside a block that a C function (Array#sort) calls,
# here inside a Fiber, whose C frame cannot be carried. The suspend is refused
# with the reason (Checkpoint::Error), nothing is written and the program
# goes on. (Only as the top-level program: the reason names the context.)
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
  [2, 1].sort do |a, b|
    begin
      Checkpoint.suspend("x.ckpt")
    rescue Checkpoint::Error => e
      puts "refused in fiber: #{e.message}"
    end
    a <=> b
  end
  :fiber_done
end
puts f.resume
puts "still running"
