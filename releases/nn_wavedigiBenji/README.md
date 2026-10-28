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
                   tri1 > tri2 (PWM comparator, polyBLEP) ─► WDF resonant filter ─► LP / BP

 Cross FM: Osc 2 tri ─► Osc 1 rate, Osc 1 tri ─► Osc 2 rate
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

**Cross FM.** Each oscillator's exponential rate is modulated by the other's triangle, with up to ±4 octaves of depth. Rates are clamped at about 8 kHz, so deep FM can't run an oscillator past what the triangle core handles cleanly.

**Band-limited comparator.** Cross FM pushes the oscillators high and moves the PWM edges around quickly, which aliases badly with a naive comparator. Each edge is found to sub-sample accuracy by interpolating `tri1 - tri2`, and corrected with a two-sample polyBLEP. In the host test this lowers aliasing by about 16 dB (from -17 dB to -34 dB at 1.2 kHz). It adds one sample of latency.

### How faithful is it?

It is a Benjolin-*style* circuit model, not a component-level model of Hordijk's schematic. The topology and behaviour are Benjolin-like: two triangle-core oscillators, a rungler with XOR/loop, a PWM comparator, and a resonant self-oscillating filter. But the filter is an idealised RLC with a PWL nonlinearity rather than the original transistor circuit, and the component values are normalised. The comparator output feeding the filter is band-limited. The triangles and the pulse outputs are not.

## Controls

Tap Z down to step through three knob pages. The lit LED in the left column shows the page.

| Page | Main | X | Y |
|---|---|---|---|
| 1 (top LED) | Osc 1 rate (0.5 Hz to ~6 kHz) | Rungler → Osc 1 (0 to 5 oct) | FM Osc 2 → Osc 1 (0 to ±4 oct) |
| 2 (middle LED) | Osc 2 rate (0.5 Hz to ~6 kHz) | Rungler → Osc 2 (0 to 5 oct) | FM Osc 1 → Osc 2 (0 to ±4 oct) |
| 3 (bottom LED) | Filter cutoff (30 Hz to 15 kHz) | Rungler → cutoff (0 to 5 oct) | Resonance (self-oscillates in roughly the top 10%) |

Z up = **Loop**: the rungler recirculates its pattern. Z middle = **Chaos**: new data comes from Osc 1, XORed with the register output.

Knobs use soft takeover. After changing page, a parameter only follows its knob once the knob reaches the parameter's current value, so nothing jumps. On power-up, page 1 takes the knob positions straight away; the other pages start from defaults (FM off, rungler to cutoff at 3 octaves).

## Jacks

| Jack | Function |
|---|---|
| Audio In 1 | Filter input; replaces the PWM comparator when patched |
| Audio In 2 | Cutoff CV (~1V/oct), added to the knob and rungler |
| CV In 1 / 2 | Osc 1 / Osc 2 pitch CV (~1V/oct) |
| Pulse In 1 | External rungler clock; replaces Osc 2 when patched |
| Pulse In 2 | While high, flips the rungler between Loop and Chaos |
| Audio Out 1 / 2 | Filter LP / BP |
| CV Out 1 | Rungler voltage (0 to +6V) |
| CV Out 2 | Osc 2 triangle |
| Pulse Out 1 / 2 | Osc 1 / Osc 2 square |

LEDs: the left column shows the current page. The right column shows, top to bottom, the rungler voltage, the PWM comparator state and the LP level.

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
* aliasing of the polyBLEP comparator against a naive one (at least 10 dB better);
* cross FM shifting the mean oscillator rate by the amount exponential FM should;
* decay below the oscillation threshold, and bounded self-oscillation at the cutoff frequency above it;
* a 20-second stress test at extreme, randomly jumping settings.

They also render a few example patches to WAV.
