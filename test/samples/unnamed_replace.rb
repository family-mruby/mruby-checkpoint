# checkpoint-test: suspend-after "moved"
# checkpoint-test: sabiruby-refuse 1 "contents"
# What the baseline holds without a name of its own, moved around: an inner
# Hash replaced by a new one (the outer Hash is written again, its key
# "ja" by its name), objects moved from one constant to another, swapped,
# taken out and held only by locals, the key of a Hash held by a Hash.
old_ja = CkBase::STRINGS["ja"]
CkBase::STRINGS["ja"] = { hello: "yaa" }
second = CkBase::NESTED.delete_at(1)            # held by a local only
CkBase::BOXED.inner[:moved] = CkBase::NESTED[0] # in two places now
CkBase::PAIRS[0].b, CkBase::ERRS[0] = CkBase::ERRS[0], CkBase::PAIRS[0].b   # swapped
key_en = CkBase::STRINGS.keys[0]
CkBase::KEYS[key_en] = :string_key              # a permanent String as a key elsewhere
$ck_global = nil                                # the global's Array dropped
kept = CkBase::DEEP.delete(:list)
puts "moved"
Checkpoint.suspend("unnamed_replace.ckpt")
p CkBase::STRINGS, old_ja, second, CkBase::NESTED, CkBase::BOXED.inner
p CkBase::BOXED.inner[:moved].equal?(CkBase::NESTED[0])
p CkBase::PAIRS[0].b.class, CkBase::ERRS[0], CkBase::KEYS, CkBase::KEYS[key_en]
p key_en.equal?(CkBase::STRINGS.keys[0]), CkBase::DEEP, kept[0].inner, $ck_global
second << "c"
old_ja[:hello] << "?"
p second, old_ja
