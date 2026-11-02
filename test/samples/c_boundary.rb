# ghost-test: hop-after "cmp 3"
# Ghost.hop inside a block that a C function calls (Array#sort! via sort,
# Array.new with a block). There is a C frame between the root frame and
# the block, so the hop is deferred until execution is back out of the C
# function: the first process still prints every "cmp" line.
n = 0
r = [3, 1, 2].sort do |a, b|
  n += 1
  puts "cmp #{n}"
  Ghost.hop("c.ghost") if n == 1
  a <=> b
end
puts "sorted #{r.inspect}"

made = Array.new(3) do |i|
  Ghost.hop("c.ghost") if i == 0
  puts "make #{i}"
  i * i
end
puts "made #{made.inspect}"
