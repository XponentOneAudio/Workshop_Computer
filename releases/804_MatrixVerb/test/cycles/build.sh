#!/bin/sh
# Build the engine for the RP2040's Cortex-M0+ to estimate its cycles.
#   ./build.sh [dir with fdn.h and design.h, default ../..] [name, default bench]
#   python3 emu.py bench
# Needs arm-none-eabi-g++ (with newlib), and pip install unicorn capstone.
SRC="${1:-../..}"
OUT="${2:-bench}"
arm-none-eabi-g++ -mcpu=cortex-m0plus -mthumb -O2 -std=c++17 $EXTRA -fno-exceptions -fno-rtti -fno-threadsafe-statics -ffunction-sections \
  -I"$SRC" bench.cpp -T link.ld -nostartfiles --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -lm -o "$OUT.elf" 2>/dev/null && \
arm-none-eabi-objcopy -O binary "$OUT.elf" "$OUT.bin"
