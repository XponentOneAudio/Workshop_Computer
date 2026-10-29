# Circuit notes: MFOS Noise Toaster as wave digital filters

These notes explain how the card models each part of Ray Wilson's MFOS Noise
Toaster, and where the numbers come from.

## Source

The schematic comes from the KiCad 8 redraw of the MFOS design in
[XponentOneAudio/MFOS-NoiseToaster](https://github.com/XponentOneAudio/MFOS-NoiseToaster),
rev 0.6.1 (`PCB/*.kicad_sch`). That redraw is CC-BY-NC-SA, and the original
design is by Ray Wilson,
<http://musicfromouterspace.com/analogsynth_new/NOISETOASTER/NOISETOASTER.php>.

`tools/extract_netlist.py` flattens the hierarchical schematic into
[`netlist.txt`](netlist.txt) without needing KiCad. Every component reference
used below and in `src/toaster_model.h` (R33, C9, Q8 and so on) matches that
netlist.

Supply: a 9 V battery. B- = 0 V, B+ = 9 V, and the virtual ground
VG = 4.5 V comes from the R37/R42 divider. The model treats the LM324 outputs
as swinging 0.02 V to 7.5 V.

### Problems in the KiCad redraw, and what the model assumes instead

| Redraw | Model |
| --- | --- |
| Q1 (the ramp reset JFET) has no connections | Q1 sits across C2 and is driven by U1B through D1/R6. This is the standard MFOS ramp core. |
| Both ends of R32 are on GND | R31/R32 form a 100k/10k divider from U2B's output to VG, which gives the SQR level. |
| S6 (VCF mod) and the top of R38 are unconnected | A schematic note says ARG reaches the board through S6. S6 connects the top of R38 to AREG, off, or LFO. |
| Q10 is a PNP with its emitter on B- | Ignored. The manual gate is S10 to B+ through R57, with R61 to B-. |

## Overview of the model

| Block | Parts | How it is modelled |
| --- | --- | --- |
| VCF | U2A, Q8, C9, C10, R28, R29, R35, R39–R45, R38 | WDF. A 3-port parallel junction at node X9, with the op-amp as a constraint-solving root that can saturate. The JFET is a voltage-controlled resistor driven by the solved gate network. |
| VCF input legs | C8/R33 (VCO), C12/R34 (noise) | WDF. A resistive source in series with a capacitor, terminated in the virtual ground. |
| VCO expo converter | U1C, Q3, Q4, U1D, R2, R3, R11, R14, R16, R21 | Physics: `I = Iref * exp(dV / VT)`, with dV from the U1C summer. |
| VCO ramp | U1A, C2, U1B, Q1, R7, R12, R17, R18 | The integrator is exact: a constant current into a bilinear capacitor is a ramp. U1B is a comparator with the R7/R12 hysteresis and a 6 µs reset dead time. The reset discontinuity uses polyBLEP. |
| VCO square | U2B, R26, R30/C11, R31/R32 | A WDF RC averager (R30/C11). U2B is a comparator. Edges use polyBLEP. |
| Sync | C6, R17, R18 | WDF. A CR differentiator drives the U1B threshold node. |
| LFO | U2C, R48, R49, C16, R55, R59, R60 | WDF. An RC (C16) charged from the U2C output, with a Schmitt comparator. |
| LFO "triangle" | R56, C19, loaded by R13 | WDF RC. |
| AREG | U2D, C18, D2, D3, R50, R51, R53, R57, R61 | WDF. A series adaptor of the gate source, R50 and C18, under a nonlinear root: the D2/R51 ‖ D3/R53 network, which is solved exactly. U2D is a Schmitt comparator for repeat mode. |
| AREG buffer | Q9, R54 | Exact nonlinear solve, tabulated. |
| Noise | Q5 avalanche, Q2, Q6, C7, R25, R19 | White noise from a hashed LFSR. C7/R25 is a WDF high-pass into Q6's shunt-feedback stage (gain R19/R25), with saturation. |
| VCA | Q7, R41, R46, R47, R66 | Exact nonlinear JFET solve (triode and saturation, gate clamp), tabulated over gate × drain voltage. |
| Output coupling | C21 into R67 + R68 | WDF RC. |

The tabulated stages (Q9 and Q7) have no capacitors. A WDF with one nonlinear
root and no reactive elements reduces to "solve the node equation". That solve
is done exactly, by bisection, in `tools/gen_tables.cpp`, and the card only
interpolates.

The audio-rate code is integer. Voltages are Q24 volts and coefficients are Q28.
The coefficients are recomputed in float at a 6 kHz control rate.

## The VCF

The netlist around U2A:

```
            C9 (1n)                      R29 (pot, rheostat)       C10 (1n)
  N (U2A-) ----||---- X9 ---------------/\/\/\---- X10 -------------||---- O (U2A out)
  |                    |
  |                    +-- R28 (pot, rheostat) -- VG
  |                    +-- Q8 drain/source ------ VG     (gate network: R39/R40/R44/R45, R38 + R43 + S6)
  +-- R35 (4M7) ---------------------------------------------------------- O
  +-- R33 (4M7) -- C8 (100n) -- VCO (S3)
  +-- R34 (3M)  -- C12 (100n) -- noise (S4)
  U2A+ = VG
```

The op-amp feedback is a T-network. Its transfer impedance is
`Z_T = Za + Zb + Za Zb / Zg`, where `Za = 1/sC9`, `Zb = R29 + 1/sC10`, and
`Zg = R28 ‖ Rds(Q8)`. With R35 in parallel, the response is:

```
Vo / (R35 I_in) = -(R35 ‖ Z_T) / R35

denominator: s² C² Rg (R35 + R29) + s C (2 Rg + R29) + 1      (C9 = C10 = C)
f0 = 1 / (2π C sqrt(Rg (R35 + R29)))
Q  = sqrt(Rg (R35 + R29)) / (2 Rg + R29)
```

What this means for the controls:

- **R28 and Q8 together set the cutoff.** With R28 from 100 kΩ down to 150 Ω,
  f0 goes from about 230 Hz to about 6 kHz.
- **R29 is the resonance control.** Q is very high when R29 is small; it
  reaches about 55 at full resonance.
- **The DC gain is -R35/R33 = -1.**

### WDF structure

The input legs are effectively current sources into node N, because
R33 and R34 are megohms. For each leg, `E = R35 * I_in` is computed with a
series-RC WDF terminated in N.

Node X9 is the root junction. It is a parallel adaptor with three branches,
each a Thevenin source (E, R):

| Branch | E | R |
| --- | --- | --- |
| A: through C9 and R35 to the op-amp output, via N | `Vo + R35 I_in - b9` | `R35 + Rc9` |
| B: through R29 and C10 to the op-amp output | `Vo - b10` | `R29 + Rc10` |
| C: R28 ‖ Q8 to VG | `0` | `Rg` |

Here `b9` and `b10` are the capacitor waves, and `Rc = T/2C`.

The ideal op-amp is a nullor. The norator sets Vo, and the nullator forces
V_N = 0. Every wave in the tree is linear in Vo, so V_N can be written in
closed form:

```
G = 1/R per branch,  Gt = GA + GB + GC,  kx = (GA + GB) / Gt
m   = (GA cA + GB cB) / Gt                 cA = R35 I_in - b9,  cB = -b10
V_X9 = kx Vo + m
V_N = p Vo + q,   p = 1 - R35 GA (1 - kx),   q = R35 I_in - R35 GA (cA - m)
```

The root therefore solves `Vo = -q / p`, then clamps Vo to the LM324 rails.
When Vo is clamped, V_N is no longer 0, which is how a saturated op-amp
behaves. The same tree then remains valid with N as a free node.

After that, the capacitor waves are updated from the branch currents:
`b9 += 2 Rc9 GA (E_A - V_X9)` and `b10 += 2 Rc10 GB (E_B - V_X9)`.

Each sample costs about 7 multiplies. `p`, `1/p`, and the junction ratios
depend only on resistances, so they are computed at control rate.

### Checking it

`sim/vcf_test.cpp` drives the fixed-point VCF with steady-state sines. It
compares the result with the analytic `H(s)` above, mapped through the
bilinear transform, at six pot settings, including Q = 55. The error is
0.02 dB or less everywhere the response is above -30 dB, for both the
host and the RP2040 multiply paths. The derivation was first checked in double
precision, where it agrees with the analytic response to 1e-11 dB.

### The JFET (Q8)

The source is on VG. The gate is `R45/(R40+R45)` of node M, which is fed by
R44 from B+, by R39 from the R38 wiper, and loaded by R40 + R45. The top of
R38 is the S6 source (ARG or LFO, or floating), and its bottom goes to B-
through R43.

The channel is a small-signal square-law resistor,
`Rds = 1 / (2 β (Vgs - Vp))`, with the 2N5457 at Vp = -1.8 V and
Idss = 3 mA. The datasheet spread is large, so these values set where the
mod knob starts to bite.

The model ignores the Rds dependence on Vds (signal-dependent distortion
in the filter).

## The AREG

The loop is: gate source, the diode network, R50, C18, then B-. In WDF
terms, the gate source (with R50) and C18 form a series adaptor. The incident
wave at the root is `a = E - b18` and the port resistance is
`Rp = Rsrc + R50 + Rc18`.

The root is the diode pair. Each 1N914 is an ideal diode with a 0.5 V drop
in series with its pot. That root is piecewise linear and is solved exactly:

```
a >  Vd:  i = (a - Vd) / (R51 + Rp)     attack
a < -Vd:  i = (a + Vd) / (R53 + Rp)     release
else      i = 0
```

The gate source depends on the mode:

- **Repeat:** U2D's output (0.02 V or 7.5 V), with the R63/R64/R58 Schmitt
  thresholds at 2.58 V and 5.79 V on ARG.
- **Manual:** S10 to B+ through R57 (200 Ω), or R61 to B- (10.2 kΩ total).

The minimum attack is 4.8 ms, which is (R57 + R50) × C18. The maximum
attack or release is about 4.7 s per time constant.

ARG = Q9's source voltage. The JFET follower adds 0.6–1.3 V of offset, so ARG
rests at about 1.26 V.

## The VCO

- **Expo converter.** The current is `I = IREF exp(dV/VT)`, where
  `IREF = 4.5 V / R16 = 2.25 µA` and `VT = 25.85 mV`. The input is
  `dV = R11 Σ (Vi - VG)/Ri`:
  - R1 wiper through R2 (75 kΩ)
  - R3 (270 kΩ) from B+
  - the R13 LFO-depth wiper through R14 (100 kΩ). Pin 3 of R13 is on B-, so
    raising the LFO depth also raises the mean pitch, as on the original.
  - the R20 AREG-depth wiper through R21 (36 kΩ)

  CV In 1 and Audio In 2 add `VT ln2` per volt, which is 1 V/oct.
- **Ramp.** C2 = 1 nF integrates I upwards from VG. U1B fires when
  `ramp > thresh (R7+R12)/R7`, which is 5.58 V for the 5.4 V threshold, so
  the ramp swings 1.08 V peak-to-peak. Q1 then shorts C2. The 6 µs dead time
  stands for U1B's slew rate plus the discharge time.
- **Measured.** The centre frequency is 233 Hz, against 234 Hz predicted.
  The pitch knob covers about 2 Hz to 20 kHz. V/oct tracking is within 1% over
  4 octaves, then goes slightly flat at the top because of the dead time.
  Aliasing is 48 dB below the fundamental at 1 kHz and 39 dB below at 3.6 kHz,
  measured in-band.

## Measured behaviour

All figures come from `sim/probe.cpp`.

| | Model |
| --- | --- |
| VCO centre (pitch knob 50%) | 233 Hz |
| LFO rate range | 0.66 Hz – 232 Hz |
| AREG repeat cycle, attack and release at 0 | 10.7 ms |
| VCF f0 range | 230 Hz – 6 kHz, Q up to about 55 |

## Departures from the hardware

These were made for usability or CPU, and all are deliberate:

- **Pot tapers:**
  - Cutoff (R28) is exponential.
  - Resonance (R29) is quadratic.
  - LFO rate, attack and release are audio-taper.
  - The hardware pots are linear, which crowds all the action into one end
    of the knob.
- **Merged switch-and-pot controls.** The VCF-mod knob combines S6 and R38:
  CCW is LFO, the centre is off, and CW is AREG. The VCO-mod knob combines
  R13 and R20/S1 in the same way.
- **Fixed switch positions.** The LFO wave switches are fixed: S9 is on the
  RC "triangle" and S7 is closed. The VCA bypass (S5) becomes Audio Out 2,
  which is the VCF output before the VCA.
- **Noise level.** The avalanche noise amplitude is a guess, about 0.5 V RMS
  at X4.
- **Output gain.** The outputs are scaled ×2 above the toaster's line level,
  with a soft knee near full scale.
- **VCA thump is kept.** The JFET VCA's source swings between 0 V and 4.5 V
  as it opens. That step, coupled through C21, is a real Noise Toaster thump,
  and the model keeps it.
