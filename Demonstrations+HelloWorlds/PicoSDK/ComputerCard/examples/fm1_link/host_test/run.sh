#!/bin/sh
# Host-side test of the Workshop Link framework, no hardware needed.
#
# Builds the fm1_open browser simulator natively (the real FM1 APP sources:
# screen, MIDI routing, Workshop Link page) and links it with the real
# WorkshopLinkHost.h (Pico SDK / TinyUSB replaced by tiny stubs), then runs a
# session byte-for-byte across a simulated USB MIDI cable: protocol
# round-trips, HELLO/layout, values, scope, encoders/buttons, FM1 restart,
# lost link, and the plain-MIDI fallback for stock FM1 firmware.
#
# Usage: ./run.sh /path/to/fm1_open     (needs clang/clang++ or gcc/g++)
# Writes link_page.ppm and link_lost.ppm (the simulated FM1 screen).
set -e
FM1=${1:?usage: run.sh /path/to/fm1_open}
HERE=$(cd "$(dirname "$0")" && pwd)
CARD="$HERE/../../.."
OUT=${OUT:-"$HERE/build"}
CC=${CC:-clang}
CXX=${CXX:-clang++}
mkdir -p "$OUT/sim"
cmp "$CARD/WorkshopLink.h" "$FM1/app/WorkshopLink.h" || {
  echo "WorkshopLink.h differs between the card library and fm1_open/app"; exit 1; }
cd "$OUT/sim"
$CC -std=c11 -O1 -fwrapv -fno-fast-math -ffp-contract=off -DFM1_HOST_TEST -DFM1_OPEN_WEB \
  -Wall -Wextra -Werror -Wno-unused-function -Wno-array-parameter \
  -I"$FM1/web/engine" -I"$FM1/web/synth" -I"$FM1/third_party/synth" -I"$FM1/sdk/include" \
  -I"$FM1/app/synth" -I"$FM1/app" \
  -c "$FM1"/web/engine/*.c "$FM1"/web/synth/*.c "$FM1"/third_party/synth/*.c "$FM1"/app/*.c
cd "$OUT"
$CXX -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -I"$HERE/stubs" -I"$CARD" \
  "$HERE/e2e.cpp" sim/*.o -lm -o e2e
./e2e
