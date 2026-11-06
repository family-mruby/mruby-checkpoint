# checkpoint-test: suspend-after "changed"
# checkpoint-test: sabiruby-refuse 1 "contents"
# checkpoint-test: sabiruby-refuse 2 "contents"
# Changes inside what the baseline holds without a name of its own: the
# driver's CkBase (made before the baseline, as a library's mrblib makes its
# constants) has Hashes in a Hash, Arrays in an Array, an object in an ivar,
# a Struct, a Set, an Exception, a Hash's default value, a Range, a closure's
# variable, an anonymous class. Each is named by a path from the constant
# (V:CkBase@STRINGS/{"en"} ...); a change in one is carried as a change of
# that object, and a local holding one keeps holding the same object.
CkBase::STRINGS["en"][:bye] = "bye"            # a Hash in a Hash
CkBase::STRINGS["ja"][:hello] << "!"           # a String in a Hash in a Hash (<<)
CkBase::NESTED[0] << 3                          # an Array in an Array
CkBase::NESTED[1][0] << "z"                     # a String in an Array in an Array
CkBase::BOXED.inner[:n] = 5                     # a Hash in an ivar of a named object
CkBase::DEEP[:list][0].inner << 2               # an Array in an ivar of an unnamed object
CkBase::DEEP[:list][0].instance_variable_set(:@extra, "e")   # an ivar added to it
CkBase::PAIRS[0].a = 2                          # a Struct's member
CkBase::PAIRS[0].b << "y"                       # a String in it
CkBase::SETS[0] << 3                            # a Set
CkBase::ERRS[0].message << "?"                  # an Exception's message (<<)
CkBase::ERRS[1].send(:initialize, "bong")       # ... and a new message
CkBase::DEFAULTS[0].default << "!"              # a Hash's default value
CkBase::RANGES[0].begin << "a"                  # a Range's end
CkBase::COUNTER.call                            # a closure's variable
CkBase::COUNTER.call
CkBase::KEYS.keys[0] << 2                       # an Array used as a key
CkBase::KEYS.rehash
CkBase::BIGKEYS.keys[5] << 0                    # ... in a Hash too big to search key by key
CkBase::BIGKEYS.rehash
CkBase::FROZEN[1] << "more"                     # an Array in a frozen Array
CkBase::SHARED << "twice"                       # held by a constant and by an Array
CkBase::ANON[0].define_method(:bye) { "bye" }   # an anonymous class
def (CkBase::NESTED[1]).tag = "tagged"          # a singleton method
$ck_global[0] << "h"                            # an Array in a global's Array
CkBase::NAMES[1] << "!"                         # a String in a frozen Array
word = CkBase::NAMES[2]                         # a String held by a local, not changed
CkBase::SHARED << CkBase::NAMES[0]              # ... and one held in a second place now
en = CkBase::STRINGS["en"]                      # the inner Hash, held by a local
key = CkBase::STRINGS.keys[1]                   # the key String "ja"
deep = CkBase::DEEP[:list][0]                   # the unnamed object
puts "changed"
Checkpoint.suspend("unnamed.ckpt")
p CkBase::STRINGS, CkBase::NESTED, CkBase::BOXED.inner
p CkBase::DEEP[:list][0].inner, CkBase::DEEP[:list][0].instance_variable_get(:@extra)
p CkBase::PAIRS, CkBase::SETS, CkBase::ERRS.map(&:message)
p CkBase::DEFAULTS[0][:x], CkBase::RANGES, CkBase::COUNTER.call
p CkBase::KEYS[[1, 2]], CkBase::BIGKEYS[[5, 0]], CkBase::BIGKEYS[[5]], CkBase::FROZEN, CkBase::TWICE, CkBase::ANON[0].new.bye
p CkBase::NESTED[1].tag, $ck_global
p en.equal?(CkBase::STRINGS["en"]), key.equal?(CkBase::STRINGS.keys[1]), deep.equal?(CkBase::DEEP[:list][0])
p CkBase::TWICE[0].equal?(CkBase::SHARED), CkBase::TWICE[1][0].equal?(CkBase::SHARED)
p CkBase::NAMES, CkBase::NAMES.frozen?, word.equal?(CkBase::NAMES[2]), CkBase::SHARED[-1].equal?(CkBase::NAMES[0])
word << "s"                                     # through the local: the same String as the constant's
p CkBase::NAMES
en[:later] = "later"                            # changed again on the restored side
CkBase::NESTED[0] << 4
puts "again"
Checkpoint.suspend("unnamed.ckpt")
p CkBase::STRINGS["en"], CkBase::NESTED[0], en.equal?(CkBase::STRINGS["en"])
