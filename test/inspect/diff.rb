# tools/test_inspect.rb: `checkpoint_inspect.rb diff` between two saves of
# one program shows what the program changed between them, and nothing else
# (the expected differences are listed in the test).
$count = 1

class Box
  def initialize
    @items = [1, 2]
    @name = "box"
  end
  attr_reader :items
end

def work
  box = Box.new
  h = { keep: "k", drop: "d" }
  n = 10
  old_home = ["moves"]   # the same Array, held by another variable in B
  new_home = nil
  Checkpoint.save("diff_a.ckpt")
  new_home = old_home
  old_home = nil
  n += 5
  box.items << 3
  h.delete(:drop)
  h[:new] = [9]
  $count = 2
  Checkpoint.save("diff_b.ckpt")
  [box, h, n, new_home]
end
p work
