# isr_timing.py - estimate the cost of the real firmware's audio interrupt.
#
# Runs ComputerCard::BufferFull() (which calls ProcessSample) from the built
# .elf in a Cortex-M0+ emulator, and counts approximate RP2040 cycles per
# sample. The boot ROM's float/double/bit-op routines are stood in for in
# Python with datasheet-style cycle costs, and the SIO hardware divider is
# modelled. Counts are estimates, good for comparing builds; at 192MHz the
# budget is 4000 cycles per 48kHz sample, and an overrun upsets
# ComputerCard's knob multiplexer (the knobs misread).
#
#   pip install unicorn pyelftools
#   python3 isr_timing.py ../src/build/seven_of_nine.elf [samples]
import sys,struct,math,collections
from unicorn import *
from unicorn.arm_const import *
from elftools.elf.elffile import ELFFile
elf=sys.argv[1]; f=ELFFile(open(elf,'rb'))
syms={}
for s in f.get_section_by_name('.symtab').iter_symbols(): syms.setdefault(s.name,s['st_value'])
def S(part):
    c=[n for n in syms if part in n]; assert c, part; return syms[sorted(c,key=len)[0]]
mu=Uc(UC_ARCH_ARM, UC_MODE_THUMB|UC_MODE_MCLASS)
mu.mem_map(0x00000000, 0x10000)                 # fake ROM
mu.mem_map(0x10000000, 2<<20)                    # flash
mu.mem_map(0x20000000, 0x100000)                 # RAM (+ scratch)
mu.mem_map(0x40000000, 0x20000000)               # peripherals as RAM
mu.mem_map(0xe0000000, 0x100000)                 # PPB
for seg in f.iter_segments():
    if seg['p_type']=='PT_LOAD' and seg['p_filesz']:
        mu.mem_write(seg['p_paddr'], seg.data())
        if seg['p_vaddr']!=seg['p_paddr']: mu.mem_write(seg['p_vaddr'], seg.data())
# --- SIO with hardware divider ---
div={'dvd':0,'dvs':1,'q':0,'r':0,'signed':False}
def sio_read(uc,off,size,_):
    if off==0x70: return div['q']&0xffffffff
    if off==0x74: return div['r']&0xffffffff
    if off==0x78: return 1
    if off==0x60 or off==0x68: return div['dvd']&0xffffffff
    if off==0x64 or off==0x6c: return div['dvs']&0xffffffff
    if off==0x04: return 0xffffffff  # GPIO_IN
    return 0
def s32(x): x&=0xffffffff; return x-(1<<32) if x&0x80000000 else x
def sio_write(uc,off,size,val,_):
    if off in (0x60,0x68): div['dvd']=val; div['signed']=off==0x68
    elif off in (0x64,0x6c):
        div['dvs']=val; signed = off==0x6c
        a,b=(s32(div['dvd']),s32(val)) if signed else (div['dvd']&0xffffffff,val&0xffffffff)
        if b==0: q,r=(0xffffffff if a>=0 else 1),a
        else:
            q=abs(a)//abs(b); q= q if (a>=0)==(b>=0) else -q; r=a-q*b
        div['q'],div['r']=q,r
    elif off==0x70: div['q']=val
    elif off==0x74: div['r']=val
mu.mmio_map(0xd0000000, 0x1000, sio_read, None, sio_write, None)
# --- fake ROM float table ---
def fb(x): return struct.unpack('<f',struct.pack('<I',x&0xffffffff))[0]
def bf(v):
    try: return struct.unpack('<I',struct.pack('<f',v))[0]
    except OverflowError: return 0x7f800000 if v>0 else 0xff800000
def i2f(x): return bf(float(s32(x)))
def f2i(x):
    v=fb(x)
    if v!=v: return 0
    v=max(min(v,2147483647.0),-2147483648.0); return int(v)&0xffffffff
ops={0x7c:(None,40), 0x00:(lambda a,b: bf(fb(a)+fb(b)),60), 0x04:(lambda a,b: bf(fb(a)-fb(b)),60), 0x08:(lambda a,b: bf(fb(a)*fb(b)),55),
     0x0c:(lambda a,b: bf(fb(a)/fb(b)) if fb(b)!=0 else 0x7f800000,75), 0x18:(lambda a,b: bf(math.sqrt(max(fb(a),0))),75),
     0x1c:(lambda a,b: f2i(a),25), 0x20:(lambda a,b: int(fb(a)*2**s32(b))&0xffffffff,30), 0x24:(lambda a,b: max(0,int(fb(a)))&0xffffffff,25),
     0x28:(lambda a,b: max(0,int(fb(a)*2**b))&0xffffffff,30), 0x2c:(lambda a,b: i2f(a),25), 0x30:(lambda a,b: bf(s32(a)/2**s32(b)),30),
     0x34:(lambda a,b: bf(float(a&0xffffffff)),25), 0x38:(lambda a,b: bf((a&0xffffffff)/2**b),30),
     0x3c:(lambda a,b: bf(math.cos(fb(a))),600), 0x40:(lambda a,b: bf(math.sin(fb(a))),600), 0x44:(lambda a,b: bf(math.tan(fb(a))),700),
     0x4c:(lambda a,b: bf(math.exp(fb(a))) if fb(a)<88 else 0x7f800000,550), 0x50:(lambda a,b: bf(math.log(fb(a))) if fb(a)>0 else 0xff800000,550),
     0x54:(lambda a,b: (0 if fb(a)==fb(b) else (0xffffffff if fb(a)<fb(b) else 1)),30)}
tbl=S('sf_table'); ROMBASE=0x1000
for i in range(64): mu.mem_write(tbl+4*i, struct.pack('<I',(ROMBASE+4*i)|1))
mu.mem_write(ROMBASE, b'\x70\x47\x00\xbf'*64)   # bx lr ; nop
def db(lo,hi): return struct.unpack('<d',struct.pack('<II',lo&0xffffffff,hi&0xffffffff))[0]
def bd(v):
    try: return struct.unpack('<II',struct.pack('<d',v))
    except OverflowError: return (0,0x7ff00000)
dops={0x00:lambda a,b:a+b,0x04:lambda a,b:a-b,0x08:lambda a,b:a*b,0x0c:lambda a,b:a/b if b else float('inf'),0x18:lambda a,b:math.sqrt(max(a,0)),
      0x3c:lambda a,b:math.cos(a),0x40:lambda a,b:math.sin(a),0x44:lambda a,b:math.tan(a),0x4c:lambda a,b:math.exp(min(a,700)),0x50:lambda a,b:math.log(a) if a>0 else -float('inf')}
dtbl=S('sd_table'); DROM=0x2000
for i in range(64): mu.mem_write(dtbl+4*i, struct.pack('<I',(DROM+4*i)|1))
mu.mem_write(DROM, b'\x70\x47\x00\xbf'*64)
BROM=0x3100
bitops={0:lambda x: bin(x).count('1'), 4:lambda x: 32-x.bit_length(), 8:lambda x: ((x&-x).bit_length()-1) if x else 32, 12:lambda x:int(f'{x:032b}'[::-1],2)}
mu.mem_write(BROM, b'\x70\x47\x00\xbf'*8)
bt=syms.get('sf_clz_func')
if bt: mu.mem_write(bt, struct.pack('<I',(BROM+4)|1))
# bit-ops function table used by __wrap___clz etc (address taken from the wrapper's literal pool)
if '__wrap___clz' in syms:
    clzw=syms['__wrap___clz']; lit=struct.unpack('<I',mu.mem_read((clzw&~1)+8,4))[0]
    for i in range(4): mu.mem_write(lit+4*i, struct.pack('<I',(BROM+4*i)|1))
unknown=set()
st={'cyc':0,'prev':None,'on':False,'extra':0}
def cls(op):
    if op>>11 in (0x1D,0x1E,0x1F): return 3
    if 0x4800<=op<=0x4FFF or 0x5000<=op<=0x9FFF: return 2
    if (op & 0xFE00)==0xB400 or (op & 0xFE00)==0xBC00: return 1+bin(op&0x1FF).count('1')+(2 if (op&0xFF00)==0xBD00 else 0)
    if 0xC000<=op<=0xCFFF: return 1+bin(op&0xFF).count('1')
    if (op & 0xFF00)==0x4700: return 3
    if 0xE000<=op<=0xE7FF: return 2
    return 1
CTOR_CC=S('_ZN12ComputerCardC2Ev') if '_ZN12ComputerCardC2Ev' in syms else S('_ZN12ComputerCardC1Ev')
STOP=0x3800
skip_ret={}; stop_at=set()
def hook(uc,addr,size,_):
    p=st['prev']
    if p is not None and st['on']:
        pa,ps,c,isb=p
        if isb and addr!=pa+ps: c+=1
        st['cyc']+=c
    if ROMBASE<=addr<ROMBASE+256:
        idx=addr-ROMBASE
        if idx==0x7c:   # float2double
            lo,hi=bd(fb(uc.reg_read(UC_ARM_REG_R0))); uc.reg_write(UC_ARM_REG_R0,lo); uc.reg_write(UC_ARM_REG_R1,hi)
        elif idx in ops:
            fn,c=ops[idx]; uc.reg_write(UC_ARM_REG_R0, fn(uc.reg_read(UC_ARM_REG_R0),uc.reg_read(UC_ARM_REG_R1)))
            if st['on']: st['cyc']+=c
        else: unknown.add(idx)
    if BROM<=addr<BROM+16:
        uc.reg_write(UC_ARM_REG_R0, bitops[addr-BROM](uc.reg_read(UC_ARM_REG_R0)))
        if st['on']: st['cyc']+=8
    if DROM<=addr<DROM+256:
        idx=addr-DROM; r=[uc.reg_read(x) for x in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
        a=db(r[0],r[1]); b=db(r[2],r[3])
        if idx in dops: lo,hi=bd(dops[idx](a,b)); uc.reg_write(UC_ARM_REG_R0,lo); uc.reg_write(UC_ARM_REG_R1,hi)
        elif idx==0x7c: uc.reg_write(UC_ARM_REG_R0,bf(a))
        elif idx==0x1c: uc.reg_write(UC_ARM_REG_R0,int(max(min(a,2**31-1),-2**31))&0xffffffff)
        elif idx==0x2c: lo,hi=bd(float(s32(r[0]))); uc.reg_write(UC_ARM_REG_R0,lo); uc.reg_write(UC_ARM_REG_R1,hi)
        elif idx==0x54: uc.reg_write(UC_ARM_REG_R0, 0 if a==b else (0xffffffff if a<b else 1))
        else: unknown.add(('d',idx))
        if st['on']: st['cyc']+=100
    if addr==(CTOR_CC&~1):   # skip hardware init: return immediately
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR)); st['prev']=None; return
    if addr==STOP or addr in stop_at: uc.emu_stop(); return
    if addr in skip_ret:
        uc.reg_write(UC_ARM_REG_R0, skip_ret[addr]); uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR)); st['prev']=None; return
    op=int.from_bytes(uc.mem_read(addr,2),'little')
    st['prev']=(addr,size,cls(op),0xD000<=op<=0xDEFF)
mu.hook_add(UC_HOOK_CODE, hook)
mu.mem_write(STOP, b'\x00\xbe')
def call(fn,r0):
    mu.reg_write(UC_ARM_REG_SP, 0x200f0000); mu.reg_write(UC_ARM_REG_R0, r0); mu.reg_write(UC_ARM_REG_LR, STOP|1)
    st['prev']=None
    try: mu.emu_start(fn|1, 0)
    except UcError as e:
        print('fault', e, 'pc', hex(mu.reg_read(UC_ARM_REG_PC))); raise
ctors=[v for n,v in syms.items() if n.startswith('_ZN8DualBorgC')]
if ctors:
    OBJ=0x20080000; call(ctors[0], OBJ)
else:   # constructor inlined into main(): run main, skipping clock setup, stopping at AudioWorker
    OBJ=[v for n,v in syms.items() if n.startswith('_ZZ4mainE') and 'borg' in n and 'guard' not in n.lower() and not n.startswith('_ZGV')][0]
    for n in ('check_sys_clock_khz','set_sys_clock_pll','clock_configure_int_divider','clock_configure_undivided'): skip_ret[syms[n]&~1]=1
    stop_at.add(syms['_ZN12ComputerCard11AudioWorkerEv']&~1)
    call(syms['main'], 0)
mu.mem_write(S('_ZN12ComputerCard7thisptrE'), struct.pack('<I',OBJ))
BF=S('_ZN12ComputerCard10BufferFullEv')
cycles=[]
for n in range(int(sys.argv[2]) if len(sys.argv)>2 else 400):
    st['on']=True; st['cyc']=0; call(BF,OBJ); st['on']=False; cycles.append(st['cyc'])
c=cycles[16:]
by=collections.defaultdict(list)
for i,x in enumerate(c): by[i%8].append(x)
print(f"{elf.split('/')[-1]}: ISR body mean {sum(c)/len(c):.0f}  max {max(c)}  cycles;  per phase of 8: "+" ".join(f"{max(v)}" for k,v in sorted(by.items())))
print("  budget at 192MHz/48kHz = 4000 cycles per sample; unknown ROM fns:",sorted(unknown))
