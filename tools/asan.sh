#!/bin/sh
# Rebuild the 64-bit AddressSanitizer variant (debugging aid).
#   tools/asan.sh          -> build/asan
#   tools/asan.sh stress   -> build/asan-stress (also MRB_GC_STRESS)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
name=asan; mode=asan
if [ "$1" = stress ]; then name=asan-stress; mode=asan-stress; fi
cd "$root/build/src/patched"
env MRUBY_CONFIG="$root/build_config/ghost.rb" MRUBY_BUILD_DIR="$root/build/$name" \
    GHOST_TARGETS=host GHOST_DEBUG=$mode rake -j16 all > "$root/build/$name.log" 2>&1 || { tail -30 "$root/build/$name.log"; exit 1; }
