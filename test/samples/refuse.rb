# ghost-test: refuse
# The state holds objects format v0 cannot carry: the hop is refused with
# the reason (Ghost::Error), nothing is written and the program goes on.
t = Time.at(0)
begin
  Ghost.hop("x.ghost")
  puts "not reached"
rescue Ghost::Error => e
  puts "refused: #{e.message}"
end
t = nil

f = Fiber.new do
  begin
    Ghost.hop("x.ghost")
  rescue Ghost::Error => e
    puts "refused in fiber: #{e.message}"
  end
  :fiber_done
end
puts f.resume

s = "permanent?"
ARGV << "extra"
begin
  Ghost.hop("x.ghost")
rescue Ghost::Error => e
  puts "refused: #{e.message}"
end
ARGV.pop
puts "still running #{s}"
