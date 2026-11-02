# ghost-test: hop-after "count 3"
# A counter that hops twice and keeps counting on the next line.
i = 0
while i < 10
  i += 1
  puts "count #{i}"
  Ghost.hop("counter.ghost") if i == 3 || i == 7
end
puts "done #{i}"
