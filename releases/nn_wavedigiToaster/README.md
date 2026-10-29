# nn_wavedigiToaster

**A circuit model of Ray Wilson's MFOS Noise Toaster, built from wave digital
filters, for the Music Thing Modular Workshop System Computer.**

> **Status: 0.1, not yet tested on hardware.** It builds, and it has been
> checked offline: the real card code runs in a host simulator, and the
> filter is checked against the analytic circuit response. It has not run
> on a Workshop Computer yet. See [CPU](#cpu) below.

The model was derived from the KiCad redraw of the MFOS schematic in
[XponentOneAudio/MFOS-NoiseToaster](https://github.com/XponentOneAudio/MFOS-NoiseToaster).
Every block of the original appears: VCO with expo converter, LFO, AREG,
white noise, resonant JFET low-pass filter, and JFET VCA. Each one keeps its
real component values, comparator thresholds and quirks.

- **Wave digital filters (WDFs)** cover everything with a capacitor in it:
  - the VCF, including the op-amp root and rail saturation
  - the AREG's diode/pot network
  - the LFO timing and its triangle shaper
  - the sync differentiator, square-wave averager, coupling caps and noise
    high-pass
- **The JFET buffer and the JFET VCA** have no capacitors. Their nonlinear
  circuit equations are solved exactly, offline, and stored as tables.
- **Pot tapers** are made friendlier than the linear pots of the original.

How each block is modelled, the derivations, the problems found in the
schematic redraw, and the deliberate departures are in
[docs/circuit-notes.md](docs/circuit-notes.md).

## Panel

The Noise Toaster has 10 pots and 10 switches. The Computer has 3 knobs and a
3-position switch, so the switch selects one of three **knob pages**.

Each knob uses soft takeover. When you change page, a knob does nothing until
you move it past its stored value. While any knob on the page hasn't been
picked up yet, that page's LED is dim.

| Switch | Main knob | Knob X | Knob Y |
| --- | --- | --- | --- |
| **Middle** | VCF cutoff (R28) | VCO frequency (R1) | VCF resonance (R29) |
| **Up** | LFO rate (R49) | AREG attack (R51) | AREG release (R53) |
| **Down** (hold) | VCF mod: LFO ◀ off ▶ AREG (S6 + R38) | VCO mod: LFO ◀ off ▶ AREG (R13, R20 + S1) | Source: ramp · square · ramp+noise · square+noise · noise (S3, S4) |

The mod knobs are bipolar. Turning anticlockwise from the centre dials in the
LFO, turning clockwise dials in the AREG, and the centre is off.

**Down** is also the **Manual Gate** button (S10), but only when Pulse In 1 is
patched. With Pulse In 1 empty, the AREG is in repeat mode, and the button
does nothing, just as on the original.

### Jacks

| Jack | Function |
| --- | --- |
| **Audio In 1** | External audio into the VCF, summed with the VCO. ±5 V lands at the same level as the VCO. |
| **Audio In 2** | Exponential FM into the VCO's expo converter at audio rate. 1 V/oct. |
| **CV In 1** | Pitch, 1 V/oct, through the modelled expo converter. Tracks to about 1% over 4 octaves. |
| **CV In 2** | Filter cutoff CV, injected into Q8's gate network. Positive opens the filter. |
| **Pulse In 1** | AREG gate. When patched, the AREG follows this gate (Repeat switch set to manual). When unpatched, the AREG self-cycles (Repeat on). |
| **Pulse In 2** | VCO hard sync. A rising edge resets the ramp through the C6 differentiator, which is how the toaster's LFO sync works. Unpatched means sync off. |
| **Audio Out 1** | Main output, after the VCA. This is the toaster's line out, ×2. |
| **Audio Out 2** | VCF output before the VCA, the same as the toaster with VCA Bypass on. |
| **CV Out 1** | AREG envelope, roughly 0 to +6 V. |
| **CV Out 2** | LFO "triangle" (the R56/C19 shaped wave), about ±5 V. |
| **Pulse Out 1** | LFO square. |
| **Pulse Out 2** | AREG gate node: high during attack in repeat mode, or echoing the gate. |

### LEDs

| | Left | Right |
| --- | --- | --- |
| Top | LFO rate indicator: lit when the LFO square is low, like LED1 on the toaster | AREG envelope |
| Middle | VCO square | VCF output level |
| Bottom | Page "Up" (dim = knob not picked up) | Page "Down" (dim = knob not picked up) |

## Patch ideas

These are the classic Noise Toaster sounds:

- **Squelchy drone:** nothing patched. The AREG repeats and gates the VCA.
  Turn up resonance and sweep the cutoff.
- **Zaps and lasers:** on page Down, turn VCO mod clockwise (AREG). On page
  Up, set a short attack and a medium release.
- **Wind and surf:** on page Down, set Source fully clockwise (noise). Add
  some VCF mod towards the LFO, then slow the LFO down on page Up.
- **Sync sweeps:** patch a clock or oscillator into Pulse In 2 and sweep
  Knob X.
- **Plucked bass line:** patch a sequencer's gate into Pulse In 1 and its
  pitch CV into CV In 1. Use a short attack.

## Building

The firmware uses the Pico SDK with ComputerCard.

```bash
cd src
mkdir build && cd build
PICO_SDK_PATH=/path/to/pico-sdk cmake ..
make
```

That produces `src/build/wavedigitoaster.uf2`. A prebuilt copy is in
`UF2/nn_wavedigiToaster.uf2`.

The JFET tables in `src/toaster_tables.h` are generated by `tools/build_tables.sh`.

## Offline simulator and tests

These need only `g++`:

```bash
sim/build.sh
```

The script does two things:

1. **Runs `sim/vcf_test`.** This checks the fixed-point VCF against the
   circuit's analytic transfer function at six pot settings. The current
   error is ≤ 0.02 dB.
2. **Renders demo WAVs into `sim/out/`.** It compiles the real
   `src/wavedigitoaster.cpp` against a mock of the hardware. Audio Out 1 goes
   to the left channel and Audio Out 2 to the right.

`sim/probe.cpp` dumps the model's internal signals (VCO, LFO, ARG, VCF, VCA),
for checking frequencies and envelope times.

## CPU

The audio path runs from RAM in integer arithmetic. The RP2040 has no FPU and
no 64-bit multiply, so the fixed-point multiplies are built inline from
16-bit partial products. Float work runs at a 6 kHz control rate, and pot
tapers are only recomputed when a knob moves.

At 192 MHz, the estimate from the compiled code is roughly 65–75% of the
per-sample budget, including the ComputerCard overhead. This is an estimate,
not a measurement.

To measure it on hardware:

1. Build and flash `tools/bench`, which runs the model under SysTick.
2. Open the card's USB serial port. It prints cycles per sample.

If the load turns out too high, the easiest lever is `CONTROL_DIV` in
`src/wavedigitoaster.cpp`.

## Credits and licence

- **Original design:** Noise Toaster by Ray Wilson, Music From Outer Space.
- **Schematic redraw:** Sam Zeter / XponentOneAudio, CC-BY-NC-SA.
- **This card:** CC BY-NC-SA 4.0, matching the schematic it is derived from.
- **`src/ComputerCard.h`:** Chris Johnson, MIT.
