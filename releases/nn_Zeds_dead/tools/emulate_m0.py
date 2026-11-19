#!/usr/bin/env python3
# ============================================================================
#  emulate_m0.py — run the audio-path maths as real Cortex-M0+ code
# ============================================================================
#
#  The fast multiply in zplane.h (smull32) is hand-written Thumb assembly.
#  Assembly is easy to get subtly wrong, so this script:
#
#    1. compiles tools/m0_bench.cpp for the Cortex-M0+ with arm-none-eabi-g++,
#    2. runs it in the Unicorn CPU emulator (pip install unicorn),
#    3. checks smull32 against Python's exact integer maths on edge cases and
#       100 000 random pairs, checks the soft clipper, and checks the new
#       biquad gives bit-identical results to the old one,
#    4. counts the instructions each version executes and estimates clock
#       cycles with the Cortex-M0+ timings (most instructions 1 cycle; loads
#       and stores 2; taken branches 2; BL 3; PUSH/POP 1 + registers).
#
#  Usage (from this tools/ directory):   python3 emulate_m0.py
# ============================================================================
import os, random, struct, subprocess, sys, tempfile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
TEXT, STACK, DATA, STOP = 0x10000, 0x20040000, 0x20000000, 0x0000F000

tmp = tempfile.mkdtemp()
elf, binf = os.path.join(tmp, "bench.elf"), os.path.join(tmp, "bench.bin")
subprocess.check_call(["arm-none-eabi-g++", "-O2", "-mcpu=cortex-m0plus", "-mthumb", "-std=c++17", "-fno-exceptions",
                       "-nostdlib", "-nostartfiles", "-Wl,-Ttext=0x10000", "-Wl,-e,test_smull",
                       "-I" + os.path.join(HERE, ".."), os.path.join(HERE, "m0_bench.cpp"), "-lgcc", "-o", elf])
subprocess.check_call(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text", elf, binf])
syms = {}
for line in subprocess.check_output(["arm-none-eabi-nm", elf], text=True).splitlines():
    p = line.split()
    if len(p) == 3: syms[p[2]] = int(p[0], 16)
dis = subprocess.check_output(["arm-none-eabi-objdump", "-d", elf], text=True)
mnemonic = {}                                    # address → instruction text, for cycle costs
for line in dis.splitlines():
    p = line.split("\t")
    if len(p) >= 3 and p[0].strip().endswith(":"):
        try: mnemonic[int(p[0].strip()[:-1], 16)] = p[2].strip() + " " + (p[3] if len(p) > 3 else "")
        except ValueError: pass

def cycles_of(ins):
    op = ins.split()[0]
    if op in ("bl",): return 3
    if op.startswith(("push", "pop")): return 1 + ins.count(",") + 1
    if op.startswith(("ldm", "stm")): return 1 + ins.count(",") + 1
    if op.startswith(("ldr", "str")): return 2
    if op.startswith("b"): return 2
    return 1

mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
mu.mem_map(0, 0x100000)
mu.mem_write(TEXT, open(binf, "rb").read())
mu.mem_map(DATA, 0x40000)
count = {"ins": 0, "cyc": 0}
def hook(uc, addr, size, user):
    count["ins"] += 1
    count["cyc"] += cycles_of(mnemonic.get(addr, "nop"))
mu.hook_add(1 << 2, hook)   # UC_HOOK_CODE

def call(name, *args):
    for r, v in zip([UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3], args):
        mu.reg_write(r, v & 0xFFFFFFFF)
    mu.reg_write(UC_ARM_REG_SP, STACK)
    mu.reg_write(UC_ARM_REG_LR, STOP | 1)
    count["ins"] = count["cyc"] = 0
    mu.emu_start(syms[name] | 1, STOP)
    return mu.reg_read(UC_ARM_REG_R0), mu.reg_read(UC_ARM_REG_R1)

s32 = lambda v: v - (1 << 32) if v & 0x80000000 else v
failures = 0

# ---- 1. smull32 -------------------------------------------------------------
edges = [0, 1, -1, 2, -2, 0x7FFFFFFF, -0x80000000, 0xFFFF, -0xFFFF, 0x10000, -0x10000, 0x8000, -0x8000,
         0x7FFF8000, -0x7FFF8001, 12345678, -87654321]
pairs = [(a, b) for a in edges for b in edges] + \
        [(random.randint(-2**31, 2**31 - 1), random.randint(-2**31, 2**31 - 1)) for _ in range(100000)]
for a, b in pairs:
    lo, hi = call("test_smull", a, b)
    got = (hi << 32) | lo
    if got >= 1 << 63: got -= 1 << 64
    if got != a * b:
        failures += 1
        if failures < 5: print(f"FAIL smull32({a}, {b}) = {got}, expected {a * b}")
print(f"[smull32] {len(pairs)} products checked bit-for-bit; {count['ins']} instructions per call")

# ---- 2. soft clipper ----------------------------------------------------------
F = 2048 << 12
def softclip_ref(v):   # the same integer maths in Python (>> on negatives floors, like ARM's ASR)
    knee = F + (F >> 1)
    if v >= knee: return F
    if v <= -knee: return -F
    w = v >> 9
    cube = (w * w * w) >> 19
    return v - ((cube * 9709) >> 16)
vals = list(range(-(F * 2), F * 2, 997)) + [random.randint(-F * 3, F * 3) for _ in range(20000)]
for v in vals:
    got = s32(call("test_softclip", v)[0])
    if got != softclip_ref(v):
        failures += 1
        if failures < 10: print(f"FAIL softClip({v}) = {got}, expected {softclip_ref(v)}")
print(f"[softClip] {len(vals)} values checked")

# ---- 3. biquad: new == old, and how long each takes -----------------------
Q, ST = DATA, DATA + 0x100
totals = {"run_old": [0, 0], "run_new": [0, 0]}
rng = random.Random(7)
for trial in range(3000):
    coeffs = [rng.randint(-(1 << 29), 1 << 29) for _ in range(5)]
    state = [rng.randint(-(1 << 30), 1 << 30) for _ in range(4)] + [rng.randint(-(1 << 27), 1 << 27)]
    x = rng.randint(-(1 << 23), 1 << 23)
    results = {}
    for name in ("run_old", "run_new"):
        mu.mem_write(Q, struct.pack("<5i", *coeffs))
        mu.mem_write(ST, struct.pack("<4iq", *state))
        y = s32(call(name, Q, ST, x)[0])
        totals[name][0] += count["ins"]; totals[name][1] += count["cyc"]
        results[name] = (y, mu.mem_read(ST, 24))
    if results["run_old"] != results["run_new"]:
        failures += 1
        if failures < 10: print("FAIL: new biquad differs from old", coeffs, state, x)
for name, (ins, cyc) in totals.items():
    print(f"[{name}] {ins / 3000:.0f} instructions, ~{cyc / 3000:.0f} cycles per biquad sample")
print("(run_old uses the Pico SDK's 64-bit multiply, as the firmware did before this change.)")

print("\nALL EMULATOR CHECKS PASSED" if not failures else f"\n{failures} EMULATOR CHECK(S) FAILED")
sys.exit(1 if failures else 0)
