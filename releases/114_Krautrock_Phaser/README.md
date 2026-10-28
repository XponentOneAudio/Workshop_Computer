# Krautrock Phaser

This card is a wave digital filter model of the **JH Krautrock Phaser**, designed by Juergen
Haible in 2008. It is an 8-stage lamp-and-LDR phaser with a resonance loop. The model
follows the schematic part by part: eight LM741 all-pass stages, the U6 input amplifier
and its feedback bridge, the Schmitt/integrator LFO, the lamp driver, two 7 V / 100 mA
lamps and eight LDRs.

## Controls

| Control | Function |
|---|---|
| **Main: RATE** | LFO speed, about 0.09 Hz to 20 Hz (R25, OSC_PERIOD) |
| **X: AMOUNT** | Sweep depth (R26, PHASING_AMPLITUDE) |
| **Y: FEEDBACK** | Resonance (R66). Goes up to about +28 dB, just short of self-oscillation |
| **Switch Middle** | Phaser: equal dry and wet (the MODULATION pot at noon) |
| **Switch Up** | Vibrato: wet only |
| **Switch Down** | Tap to toggle bypass, with a crossfade |

| Jack | Function |
|---|---|
| Audio In 1 + 2 | Summed mono input |
| CV In 1 | Rate, about 1 octave per volt |
| CV In 2 | AMOUNT jack: volts into the lamp driver, for a manual sweep |
| Pulse In 1 | Reset the LFO to the bottom of its sweep |
| Pulse In 2 | Hold the LFO while high |
| Audio Out 1 | Effect |
| Audio Out 2 | Dry, after the input amplifier (the original's second output) |
| CV Out 1 | LFO triangle, about ±5 V |
| CV Out 2 | Notch position: 0 V at 25 Hz, +1 V per octave, after the lamp lag |
| Pulse Out 1 | LFO square (high while rising) |
| Pulse Out 2 | High while the input amplifier is clipping |

**LEDs:** the top row glows with the lamps' light output. The middle row shows the LFO
crossfading between rising and falling. Bottom-left is lit while the effect is in, and
bottom-right lights when the input amplifier clips.

## The lamps cannot keep up

On this phaser the lamps are part of the sound. A filament has thermal mass: it
heats in about 10 ms, but when dim it takes well over 100 ms to cool. The LDRs also
respond faster to light than to darkness. The card simulates both. The table below
shows how far each all-pass pole sweeps, measured on the host from the same code
that runs on the card:

| RATE | LFO | AMOUNT ½ | AMOUNT full |
|---|---|---|---|
| 0.2 | 0.3 Hz | 26 – 950 Hz | 93 – 3770 Hz |
| 0.5 | 1.8 Hz | 68 – 790 Hz | 294 – 3530 Hz |
| 0.7 | 5.5 Hz | 201 – 528 Hz | 881 – 2870 Hz |
| 1.0 | 20 Hz | 314 – 362 Hz | 1600 – 1960 Hz |

At slow rates the sweep is wide. As RATE goes up, the lamps cannot cool fast enough
between peaks, so the sweep narrows and rises. At the fastest setting it becomes a
shallow shimmer. A phaser with an ideal LFO cannot do this.

## How the circuit becomes a wave digital filter

All of the modelling is in `phaser.h`.

- **Capacitors** are WDF one-ports under the bilinear transform. **Op-amps** are ideal
  (V+ = V−, no input current).
- **Each all-pass stage** has Cx and its LDR in series, driven by the stage input. That
  makes an adapted series loop whose only coefficient is `q = Rp / (Rp + R_ldr)`.
  The LDR is a leaf of the tree, so its resistance can change every sample at no
  extra cost.
- **The input amplifier's feedback network** is a bridge between R60, R61 + C19, the
  two halves of the FEEDBACK pot, and R62 / C21 / R63. Series and parallel adaptors
  cannot express a bridge, so it is an **R-type adaptor**, solved in closed form by
  nodal analysis.
- **The resonance loop is delay-free.** R63 is driven by stage 2's output, which
  depends on U6's output in the same sample. Each stage is affine in its input given
  its state, so stages 1–2 compose to `Vs = G·Vo + H`. That is substituted into the
  network equation and solved exactly, at the cost of one division per sample. No
  unit delay is added, so the resonant peak sits at the pole frequency, as in the
  hardware.

**Part values** come from the schematic, with three exceptions:

- The schematic leaves **Cx** as a parameter. This card uses 22 nF.
- The **reso trimmer R60** (470k max) is set to 100k. At 470k the loop
  self-oscillates from a quarter of FEEDBACK's travel. At 100k the end stop gives
  about +28 dB of resonance and stays stable across the whole sweep.
- **LOWLIM and RANGE** are set so the lamps idle at a low glow (notches near 25 Hz).

**The lamps and LDRs** need constants that a schematic does not give: filament heat
capacity, conduction loss, the light curve, and LDR gamma and response times. These
were chosen to be physically plausible and to give the sweep ranges above. They are
estimates, not measurements of Haible's unit.

## Implementation notes

- **Core 0** runs the audio path in fixed point. Signals are Q20 volts and
  coefficients Q30. At the bottom of the sweep, q is about 2e-4. Plain truncation
  there built up a low-frequency noise floor about 50 dB down. The multiplies that
  feed a state therefore carry their truncation remainder into the next sample
  (error feedback). That brought the fixed-point error against a double-precision
  model to −80 to −105 dB.
- **Core 1** runs the control model in float, once per 16-sample block. Results are
  passed back through a double buffer. Core 0 ramps the eight all-pass coefficients
  across each block, so there is no 3 kHz zipper noise.
- **Clock and memory:** the card runs at 192 MHz. The audio path, the 64-bit multiply
  and the divider are all placed in RAM.

## Verifying

`tools/phaser_check.cpp` builds on any desktop compiler. It runs the card's own
engine against a double-precision reference and checks four things:

- fixed-point error;
- resonance growth with FEEDBACK, and impulse decay at the end stop across the
  whole LDR range;
- sweep range against rate;
- 20 s of full-scale noise at maximum feedback, followed by a return to silence.

It can also render a demo WAV through the full card model:

```
cd tools
g++ -O2 -std=c++17 -DKRAUT_HOST_BUILD -I.. phaser_check.cpp -o phaser_check
./phaser_check demo.wav
```

## Building

This is a standard Pico SDK project for the RP2040:

```
mkdir build && cd build
cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
make
```

## Status

Beta. The card builds, and every host check passes. It has **not** yet been run on
Workshop Computer hardware. The CPU load is estimated from the disassembly at roughly
half the per-sample budget, but it has not been measured.

## Credits

The circuit is the JH Krautrock Phaser, © 2008 Juergen Haible. This card is an
independent simulation written from his published schematic, and does not include
the schematic.
