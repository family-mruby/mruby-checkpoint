# checkpoint-test: suspend-after "a 1"
# An image that carries the program's source (section SRC0, for display
# only): the side that resumes goes on as it does without it. (Once: the
# Checkpoint of SabiRuby does not take source: yet, and it runs the rest.)
def step(n)
  puts "a #{n}"
  Checkpoint.suspend("source.ckpt", source: true) if n == 1
  puts "b #{n}"
end

step(1)
step(2)
puts "done"
