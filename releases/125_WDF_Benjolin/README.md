# WDF Benjolin

A Benjolin, after Rob Hordijk, for the Workshop System Computer. The analogue parts that give it its voice are modelled as **Wave Digital Filters (WDFs)**, running in 32-bit fixed point on the RP2040.

## Signal flow

```
 Osc 1 (tri core) ──┬── tri ──┐
                    └── pulse ──── data ──┐          ┌─────────────┐
                                          XOR ─► 8-bit ─► R/2R/4R  ─► rungler CV ─┬─► Osc 1 rate
 Osc 2 (tri core) ──┬── pulse ──── clock ─┘   shift reg │ WDF DAC     │           ├─► Osc 2 rate
                    └── tri ──┐                         └─────────────┘           └─► cutoff
                              ▼
                   tri1 > tri2 (PWM comparator) ─► WDF resonant filter ─► LP / BP
```

## What is modelled as a WDF

**Rungler DAC.** The three most significant shift-register bits drive resistors of 4R, 2R and R into a smoothing capacitor. In the WDF each bit is an adapted resistive voltage source, so the whole network is one parallel junction with a capacitor. The stepped voltage glides a little between steps, as it does on the hardware.

**Resonant filter.** A voltage-controlled series RLC:

```
 Vin ──[R_s]──[L]──[NL]──┬── LP
                         │
                        [C]
                         │
                        GND
```

The WDF tree is a series adaptor joining the input source with `R_s`, the inductor and the capacitor. A nonlinear one-port `NL` sits at the root:

* **Normalised units.** `sqrt(L/C) = 1` and `R_s = 1`. Cutoff scales L and C together using the prewarped bilinear `k = tan(pi fc / fs)`, so `R_L = 1/k` and `R_C = k`.
* **Resonance.** For small currents `NL` is a negative resistance `-r_n` that cancels `R_s`. Q rises without moving the cutoff, and the filter self-oscillates once `r_n > R_s`.
* **Amplitude limit.** Beyond a knee current, `NL` turns into a stiff positive resistance. That limits the loop current, and with it the amplitude of the self-oscillation.
* **No iteration.** `NL` is piecewise linear, so the root is solved explicitly in each sample.
* **Outputs.** LP is the capacitor voltage, BP is the voltage across `R_s` (unity gain at cutoff), and HP is the inductor voltage.

The filter state is stored as Kirchhoff variables (loop current, capacitor and inductor voltages). The wave variables are rebuilt from them with the current port resistances every sample. Without this, fast cutoff modulation from the rungler re-interprets the stored waves under new port resistances (R_L varies 500× across the range) and injects energy. With it, the cutoff behaves like a VCF whose integrator states are continuous.

**Oscillators.** Each oscillator is an ideal current source charging a capacitor between Schmitt-trigger thresholds. Overshoot at each threshold is reflected, which keeps the frequency accurate at high rates. This part is a plain integrator, not a WDF.

### How faithful is it?

It is a Benjolin-*style* circuit model, not a component-level model of Hordijk's schematic. The topology and behaviour are Benjolin-like: two triangle-core oscillators, a rungler with XOR/loop, a PWM comparator, and a resonant self-oscillating filter. But the filter is an idealised RLC with a PWL nonlinearity rather than the original transistor circuit, and the component values are normalised. The pulse waveforms are not band-limited.

## Controls

| Control | Normal | Z held down (shift) |
|---|---|---|
| Main | Osc 1 rate (0.5 Hz to ~6 kHz) | Rungler → Osc 1 depth (0 to 5 oct) |
| X | Osc 2 rate (0.5 Hz to ~6 kHz) | Rungler → Osc 2 depth (0 to 5 oct) |
| Y | Filter cutoff (30 Hz to 15 kHz) | Resonance (self-oscillates in roughly the top 10%) |

Z up = **Loop**: the rungler recirculates its pattern. Z middle = **Chaos**: new data comes from Osc 1, XORed with the register output.

Knobs use soft takeover. After switching page, a parameter only follows its knob once the knob reaches the parameter's current value.

## Jacks

| Jack | Function |
|---|---|
| Audio In 1 | Filter input; replaces the PWM comparator when patched |
| Audio In 2 | Cutoff CV (~1V/oct); normalled to the rungler at 3 octaves depth |
| CV In 1 / 2 | Osc 1 / Osc 2 pitch CV (~1V/oct) |
| Pulse In 1 | External rungler clock; replaces Osc 2 when patched |
| Pulse In 2 | While high, flips the rungler between Loop and Chaos |
| Audio Out 1 / 2 | Filter LP / BP |
| CV Out 1 | Rungler voltage (0 to +6V) |
| CV Out 2 | Osc 2 triangle |
| Pulse Out 1 / 2 | Osc 1 / Osc 2 square |

LEDs: the top row shows the Osc 1 and Osc 2 triangles. The middle row shows the rungler voltage and the PWM comparator state. The bottom row shows the LP and BP levels. In shift mode the LEDs show the rungler depths and the resonance.

## Building

```sh
mkdir build && cd build
PICO_SDK_PATH=/path/to/pico-sdk cmake ..
make
```

This produces `wdf_benjolin.1.0.uf2`. The card runs at 144 MHz from RAM.

`-DWDFB_PROFILE=ON` builds a timing check. It lights the bottom-right LED permanently if `ProcessSample` ever uses more than 75% of its 20.8 µs budget.

### Host tests

The DSP core (`benjolin_wdf.h`) has no Pico dependencies:

```sh
g++ -O2 -std=c++17 -I. test/host_sim.cpp -o host_sim && ./host_sim /tmp
```

The tests check:

* the fixed-point filter's LP response against the prewarped analogue prototype `1/(s² + (R_s - r_n)s + 1)` (within 0.2 dB for cutoffs from 60 Hz to 11 kHz);
* BP and HP behaviour;
* decay below the oscillation threshold, and bounded self-oscillation at the cutoff frequency above it;
* a 20-second stress test at extreme, randomly jumping settings.

They also render a few example patches to WAV.
