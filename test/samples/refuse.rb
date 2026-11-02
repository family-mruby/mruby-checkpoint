# checkpoint-test: refuse
# The state holds objects format v0 cannot carry: the suspend is refused with
# the reason (Checkpoint::Error), nothing is written and the program goes on.
t = Time.at(0)
begin
  Checkpoint.suspend("x.ckpt")
  puts "not reached"
rescue Checkpoint::Error => e
  puts "refused: #{e.message}"
end
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

s = "permanent?"
ARGV << "extra"
begin
  Checkpoint.suspend("x.ckpt")
rescue Checkpoint::Error => e
  puts "refused: #{e.message}"
end
ARGV.pop
puts "still running #{s}"
