#!/usr/bin/env python3
"""Count Cortex-M0+ cycles taken by the NAM engine for a model header.

Builds tools/cycle_bench.cpp for the RP2040's Cortex-M0+ and runs it in the
Unicorn CPU emulator, adding up the documented Cortex-M0+ cycle cost of each
instruction executed (RAM with no wait states, as when the card runs from RAM;
contention between the cores isn't modelled). Used to calibrate the cost
estimates in nam2header.py.

Usage:
    python3 tools/nam2header.py some_model.nam -o /tmp/model.h --force
    python3 tools/cycle_count.py /tmp/model.h [function name to profile]

Needs arm-none-eabi-g++ and: pip install unicorn capstone pyelftools
"""

import bisect
import collections
import os
import subprocess
import sys
import tempfile

from capstone import CS_ARCH_ARM, CS_MODE_MCLASS, CS_MODE_THUMB, Cs
from elftools.elf.elffile import ELFFile
from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_MCLASS, UC_MODE_THUMB, Uc
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP

STOP = 0x000FFF00
N = 32  # samples per measurement


def build(header, out):
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    subprocess.run(
        ["arm-none-eabi-g++", "-mcpu=cortex-m0plus", "-mthumb", "-O2", "-std=c++17",
         f"-I{here}", f"-DMODEL_HEADER=\"{os.path.abspath(header)}\"",
         "--specs=nosys.specs", "-nostartfiles", "-Wl,-e,setup",
         os.path.join(here, "tools", "cycle_bench.cpp"), "-o", out, "-lm"],
        check=True, stderr=subprocess.DEVNULL)


def cost(ins):
    """Cortex-M0+ cycles, excluding conditional branches (handled by the caller)"""
    m = ins.mnemonic.split(".")[0]
    ops = ins.op_str
    if m in ("ldr", "ldrh", "ldrsh", "ldrb", "ldrsb", "str", "strh", "strb"):
        return 2
    if m in ("ldm", "ldmia", "stm", "stmia"):
        return 1 + ops.count(",") if "{" in ops else 2
    if m == "push":
        return 2 + ops.count(",")
    if m == "pop":
        n = ops.count(",") + 1
        return 3 + n if "pc" in ops else 1 + n
    if m == "bl":
        return 3
    if m in ("bx", "blx"):
        return 2
    return 1


def is_branch(m):
    return m == "b" or (m.startswith("b") and len(m) == 3 and m not in ("bic", "blx"))


class Bench:
    def __init__(self, elf_path):
        self.elf = ELFFile(open(elf_path, "rb"))
        self.mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.mu.mem_map(0, 0x100000)
        self.mu.mem_map(0x20000000, 0x100000)  # stack
        for seg in self.elf.iter_segments():
            if seg["p_type"] == "PT_LOAD":
                self.mu.mem_write(seg["p_vaddr"], seg.data())
        for sec in self.elf.iter_sections():
            if sec.name == ".bss":
                self.mu.mem_write(sec["sh_addr"], b"\0" * sec["sh_size"])
        symtab = self.elf.get_section_by_name(".symtab")
        self.syms = {s.name: s["st_value"] for s in symtab.iter_symbols()}
        self.funcs = sorted((s["st_value"] & ~1, s.name) for s in symtab.iter_symbols()
                            if s["st_info"]["type"] == "STT_FUNC" and s["st_size"])
        self.starts = [f[0] for f in self.funcs]
        self.cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
        self.cache = {}
        self.prof = collections.Counter()
        self.aprof = collections.Counter()
        self.mu.hook_add(UC_HOOK_CODE, self.hook)

    def hook(self, uc, addr, size, _):
        if self.prev is not None:
            pa, ps, ins = self.prev
            c = cost(ins)
            if is_branch(ins.mnemonic.split(".")[0]):
                c = 2 if addr != pa + ps else 1
            self.cycles += c
            self.prof[self.funcs[bisect.bisect_right(self.starts, pa) - 1][1]] += c
            self.aprof[pa] += c
        if addr not in self.cache:
            self.cache[addr] = next(self.cs.disasm(bytes(uc.mem_read(addr, size)), addr))
        self.prev = (addr, size, self.cache[addr])

    def call(self, name, *args):
        for r, a in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), args):
            self.mu.reg_write(r, a)
        self.mu.reg_write(UC_ARM_REG_SP, 0x200FFF00)
        self.mu.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.cycles = 0
        self.prev = None
        self.mu.emu_start(self.syms[name] | 1, STOP)
        return self.cycles


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    with tempfile.TemporaryDirectory() as tmp:
        elf = os.path.join(tmp, "bench.elf")
        build(sys.argv[1], elf)
        b = Bench(elf)
        b.call("setup")
        b.call("num_units")
        n = b.mu.reg_read(UC_ARM_REG_R0)
        b.call("run", 0, n, 64)  # warm up
        b.prof.clear()
        b.aprof.clear()
        total = b.call("run", 0, n, N) / N
        print(f"{n} units, {total:.0f} cycles/sample")
        for name, c in b.prof.most_common(8):
            print(f"  {c / N:8.0f}  {name}")
        per = [b.call("run", i, i + 1, N) / N for i in range(n)]
        print("per unit:", " ".join(f"{p:.0f}" for p in per))
        if len(sys.argv) > 2:
            # Instruction-level profile of one function (mangled name substring)
            matches = [f for f in b.funcs if sys.argv[2] in f[1]]
            start = matches[0][0]
            end = b.starts[b.starts.index(start) + 1]
            for a in sorted(x for x in b.cache if start <= x < end):
                ins = b.cache[a]
                print(f"{b.aprof[a] / N:7.1f}  {a:x}: {ins.mnemonic} {ins.op_str}")


if __name__ == "__main__":
    main()
