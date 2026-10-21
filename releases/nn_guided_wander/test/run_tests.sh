#!/bin/sh
# Builds and runs the host-side tests: the engine on its own, and the whole
# card (main.cpp) against stand-ins for the Computer and the 8mu.  Needs only
# a C++17 compiler.
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
CXX=${CXX:-g++}
FLAGS="-std=c++17 -O2 -Wall -Wextra -Wdouble-promotion -Wfloat-conversion"

$CXX $FLAGS -I"$here/.." "$here/engine_test.cpp" -o "$build/engine_test"
"$build/engine_test"

# main.cpp's quoted includes are looked for next to it first, so it is copied
# beside the stubs for them to be found instead of the real headers
cp "$here/../main.cpp" "$here/../wander.h" "$here/../sysex.h" "$here"/stubs/*.h "$here/card_test.cpp" "$build/"
$CXX $FLAGS -Wno-unused-parameter "$build/card_test.cpp" -o "$build/card_test"
"$build/card_test"
