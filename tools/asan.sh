#!/bin/sh
# Rebuild the 64-bit AddressSanitizer variant (debugging aid).
#   tools/asan.sh          -> build/asan
#   tools/asan.sh stress   -> build/asan-stress (also MRB_GC_STRESS)
#   tools/asan.sh fuzz     -> build/asan-fuzz (the test build with checkpoint-fuzz)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
name=asan; mode=asan; fuzz=
if [ "$1" = stress ]; then name=asan-stress; mode=asan-stress; fi
if [ "$1" = fuzz ]; then name=asan-fuzz; fuzz=1; fi
cd "$root/build/src/patched"
env MRUBY_CONFIG="$root/build_config/checkpoint.rb" MRUBY_BUILD_DIR="$root/build/$name" \
    CHECKPOINT_TARGETS=host CHECKPOINT_DEBUG=$mode CHECKPOINT_FUZZ=$fuzz rake -j16 all > "$root/build/$name.log" 2>&1 || { tail -30 "$root/build/$name.log"; exit 1; }
