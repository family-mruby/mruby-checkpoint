#!/bin/sh
# Rebuild the 64-bit AddressSanitizer variant (debugging aid).
#   tools/asan.sh          -> build/asan
#   tools/asan.sh stress   -> build/asan-stress (also MRB_GC_STRESS)
#   tools/asan.sh fuzz     -> build/asan-fuzz (the test build with checkpoint-fuzz)
#   CHECKPOINT_TABLE=file.c tools/asan.sh [stress] -> build/asan[-stress]-tabled
# All three run mruby-task on the virtual clock (mrbgems/hal-task-virtual): under
# AddressSanitizer (and more so a full GC at every allocation) a program is so
# slow that the POSIX timer ends timeslices where the normal build never does,
# and a timeslice that ends inside a Fiber in a Task stops that Task (mruby-task,
# doc/report/p4.md U3): the samples' Task shapes would end early at random.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
name=asan; mode=asan; fuzz=
if [ "$1" = stress ]; then name=asan-stress; mode=asan-stress; fi
if [ "$1" = fuzz ]; then name=asan-fuzz; fuzz=1; fi
# CHECKPOINT_TABLE=file.c (rake test:asan, test:stress): the driver links that
# fixed table of its baseline, in build/<name>-tabled
if [ -n "$CHECKPOINT_TABLE" ]; then name=$name-tabled; fi
cd "$root/build/src/patched"
# the POSIX timer left in the archive by a build from before the virtual clock
lib="$root/build/$name/host/lib/libmruby.a"
if [ -f "$lib" ] && ar t "$lib" | grep -qx task_hal.o; then rm -f "$lib"; fi
env MRUBY_CONFIG="$root/build_config/checkpoint.rb" MRUBY_BUILD_DIR="$root/build/$name" \
    CHECKPOINT_TARGETS=host CHECKPOINT_DEBUG=$mode CHECKPOINT_FUZZ=$fuzz CHECKPOINT_VIRTUAL_CLOCK=1 rake -j16 all > "$root/build/$name.log" 2>&1 || { tail -30 "$root/build/$name.log"; exit 1; }
