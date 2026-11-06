# checkpoint-test: refuse
# What the baseline holds and an image still cannot carry: the inside of C
# data made before the baseline (a reader can make new C data from the bytes
# its dump wrote, not put them into its own object), and an element of a Set
# that is not a String, Symbol or Integer (it has no name: a Set's order may
# follow addresses). The image is refused with the reason, instead of
# leaving the change behind.
CkBase::BOXES[0].value = 5
begin
  Checkpoint.suspend("x.ckpt")
  puts "not reached"
rescue Checkpoint::Error => e
  puts "refused: #{e.message}"
end
CkBase::BOXES[0].value = 1                       # as it was: nothing to carry
CkBase::OBJSET.first << 2
begin
  Checkpoint.suspend("x.ckpt")
  puts "not reached"
rescue Checkpoint::Error => e
  puts "refused: #{e.message}"
end
puts "now #{CkBase::BOXES[0].value} #{CkBase::OBJSET.inspect}"
