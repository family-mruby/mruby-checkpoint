# ghost-test: hop-after "box 41"
# C data (RData) travels only through the side table: Ghost::Box registers
# a dump and a restore callback for its mrb_data_type.
b = Ghost::Box.new(41)
puts "box #{b.value}"
Ghost.hop("d.ghost")
b.value += 1
puts "box #{b.value} #{b.class}"
