# Estimate cycles per sample of the engine on the RP2040 (Cortex-M0+ at
# 200MHz, 4167 cycles a sample at 48kHz) by running bench.bin in an emulator
# and costing each instruction: loads, stores and taken branches 2 cycles,
# the single-cycle multiplier, push/pop by register count, and library
# division and 64-bit multiply calls at what the Pico SDK's hardware-divider
# versions cost.  An estimate: it leaves out ComputerCard's own work and the
# card's control (8mu, LEDs), which share the same sample time.
import sys, subprocess, collections
from unicorn import *
from unicorn.arm_const import *
from capstone import *
name = sys.argv[1]
base = 0x20000000
code = open(name + '.bin', 'rb').read()
syms = {}
funcs = []
for line in subprocess.run(['arm-none-eabi-nm', '-S', '-C', '--defined-only', name + '.elf'], capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 3)
    if len(p) == 4 and p[2] in 'tTW':
        a, s = int(p[0], 16), int(p[1], 16); funcs.append((a, a + s, p[3]))
    if len(p) >= 3: syms[p[-1]] = int(p[0], 16)
funcs.sort()
# Library calls costed as the RP2040 SDK does them (hardware divider, fast int64 multiply)
FIXED = {'__aeabi_idiv': 20, '__aeabi_uidiv': 20, '__aeabi_idivmod': 22, '__aeabi_uidivmod': 22,
         '__aeabi_lmul': 18, '__aeabi_ldivmod': 120, '__aeabi_uldivmod': 110, '__divsi3': 20, '__udivsi3': 20}
fixed_at = {}
for a, e, n in funcs:
    for k, v in FIXED.items():
        if n == k: fixed_at[a] = (v, n)
mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
mu.mem_map(base, 0x80000)
mu.mem_write(base, code)
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
cache = {}
def cost(insn):
    m = insn.mnemonic
    if m.startswith(('ldr', 'str')): return 2
    if m.startswith(('ldm', 'stm')): return 1 + insn.op_str.count(',') + 1
    if m == 'push': return 1 + insn.op_str.count(',') + 1
    if m == 'pop': return 1 + insn.op_str.count(',') + 1 + (2 if 'pc' in insn.op_str else 0)
    if m == 'bl': return 3
    if m in ('bx', 'blx'): return 3
    return 1
st = dict(cyc=0, prev=None, skip_until=None, timing=False)
per = []
fcount = collections.Counter()
import bisect
starts = [f[0] for f in funcs]
def fname(a):
    i = bisect.bisect_right(starts, a) - 1
    return funcs[i][2] if i >= 0 and a < funcs[i][1] else '?'
def hook(uc, addr, size, ud):
    if st['skip_until'] is not None:
        if addr == st['skip_until']: st['skip_until'] = None
        else: return
    if addr in fixed_at:
        c, n = fixed_at[addr]
        st['cyc'] += c; fcount[n] += c
        st['skip_until'] = uc.reg_read(UC_ARM_REG_LR) & ~1
        st['prev'] = None
        return
    i = cache.get(addr)
    if i is None:
        i = next(md.disasm(uc.mem_read(addr, size), addr)); cache[addr] = i
    pv = st['prev']
    c = cost(i)
    if pv is not None and pv.mnemonic.startswith('b') and pv.mnemonic not in ('bl', 'bx', 'blx', 'bic', 'bics') and addr != pv.address + pv.size:
        c += 1   # taken branch
    st['prev'] = i
    if st['timing']:
        st['cyc'] += c; fcount[fname(addr)] += c
mu.hook_add(UC_HOOK_CODE, hook)
def wr(uc, access, addr, size, value, ud):
    if value == 0xFFFFFFFF: st['timing'] = True; st['cyc'] = 0; return
    per.append((value, st['cyc'])); st['cyc'] = 0
mu.hook_add(UC_HOOK_MEM_WRITE, wr, begin=syms['gMarker'], end=syms['gMarker'] + 3)
slots = []
def ws(uc, access, addr, size, value, ud): slots.append(value)
mu.hook_add(UC_HOOK_MEM_WRITE, ws, begin=syms['gSlot'], end=syms['gSlot'] + 3)
done = []
def wd(uc, access, addr, size, value, ud): done.append(1); uc.emu_stop()
mu.hook_add(UC_HOOK_MEM_WRITE, wd, begin=syms['gDone'], end=syms['gDone'] + 3)
mu.reg_write(UC_ARM_REG_SP, 0x20070000)
mu.emu_start(base | 1, base + len(code) + 0x40000, count=400_000_000)
cyc = [c for n, c in per]
# per slot statistics (skip the first 64 samples)
by = collections.defaultdict(list)
for k, ((n, c), s) in enumerate(zip(per, slots)):
    if n >= 64: by[s].append(c)
B = 4167
ev = [max(by[s]) for s in range(0, 32, 2)]; od = [max(by[s]) for s in range(1, 32, 2)]
print(f"{name}: mean {sum(cyc[64:])/len(cyc[64:]):.0f} cycles ({100*sum(cyc[64:])/len(cyc[64:])/B:.0f}% of {B}), worst {max(cyc[64:])} ({100*max(cyc[64:])/B:.0f}%)")
print("  worst by slot (even samples):", ' '.join(f"{s}:{max(by[s])}" for s in range(0, 32, 2)))
print("  worst by slot (odd samples): ", ' '.join(f"{s}:{max(by[s])}" for s in range(1, 32, 2)))
tot = sum(fcount.values())
print("  where the time goes:", ', '.join(f"{n.split('(')[0].replace('mv::Engine::','')} {100*c/tot:.0f}%" for n, c in fcount.most_common(9)))
