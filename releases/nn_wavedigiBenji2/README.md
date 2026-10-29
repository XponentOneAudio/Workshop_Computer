# WDF Benjolin 2 — Benjolin Gorga circuit model

A component-level model of the [Benjolin Gorga](https://github.com/XponentOneAudio/benjolin_gorga) (a eurorack v1.6 Benjolin by squaresolid, after Forestcaver and Rob Hordijk) for the Workshop System Computer.

The model is built from the Gorga's KiCad netlist and BOM. Every resistor, capacitor, supply rail and op-amp stage that shapes the sound is in it with its real value and reference designator (see `benjolin_circuit.h`). Capacitors are Wave Digital Filter one-ports; the op-amp and VCA networks around them are solved exactly each sample.

The earlier `nn_wavedigiBenji` card is a Benjolin-*style* model. This one follows a specific circuit.

## The circuit

### Oscillators

Each oscillator (IC2 integrator with C3/C4 = 1 nF, IC4 Schmitt trigger) is a current-controlled triangle core. The charging current is the pulse output through 100k into an **SSM2164** VCA channel (IC3), so the rate is exponential in that channel's control voltage.

The control voltage comes from a precision rectifier (IC2 with D1–D4, 10k feedback) summing three inputs:

* the rate pot through 30k;
* the CV jack through 51k, normalled to the *other* oscillator's triangle (cross modulation);
* the rungler through 51k.

The rectifier stops the control voltage going below 0 V, so the top rate is about 5 kHz (unity VCA gain). The rate pot covers about 16 octaves.

The 15k/30k hysteresis puts the triangle at ±VSAT/2. The pulse output is the Schmitt output.

### Rungler

CD4021 (IC1), clocked by oscillator 2's pulse.

* **Data** comes from an op-amp XOR (IC5.3 with Q1). Q1 is switched by oscillator 1's pulse through 100k, giving `Q8 XOR P1`.
* **SW1** adds the Chaos pot (R60) to Q1's base through 47k. Fully one way this locks the register into an 8-step loop, fully the other into a 16-step inverted loop, with chaos in between.
* **DAC:** Q6, Q7 and Q8 drive an R-2R ladder into IC5.4, so the rungler CV is **bipolar**, in 8 steps from −5.25 V to +5.25 V.

### Filter

A state-variable loop of two SSM2164 current-mode integrators (C10, C8 = 220 pF) with a summer (IC6.1). It has some unusual details, all modelled:

* **Input mix:** the filter input is a mix node (R35/R36/R38/R44/R31). The PWM comparator goes in through 150k, but the **rungler CV also leaks in** through 51k, which you can hear stepping. The bandpass also feeds back into that node.
* **C7** (220 pF) feeds the bandpass back into its own VCA input. This tames the top of the range.
* **Resonance:** the pot (R59, loaded by 1.5k and 15k) sets *damping*. With the wiper at ground the filter peaks about 26–30 dB above the passband (Q ≈ 20–30). Most of the resonance is near the clockwise end, as on the hardware if the pot is linear.
* **Cutoff CV:** a precision rectifier (IC6.4, D5/D6) sums the cutoff pot, the rungler, the filter CV jack (normalled to triangle 2) and **the filter's own bandpass output through 100k**. The filter modulates its own cutoff, which is a big part of its character.
* **Output:** IC9 amplifies the lowpass ×5 and clips at the op-amp rails.

The cutoff range is about 35 Hz to 17 kHz (analogue). The PWM comparator (IC6.3, triangle 2 vs triangle 1) runs open loop at the rails.

## Model

| Part | How it's modelled |
|---|---|
| Capacitors C3, C4, C5, C7, C8, C10 | WDF one-ports (trapezoidal: stored wave in series with T/2C) |
| Filter network around C7/C8/C10 | R-type adaptor, solved in Kirchhoff form as a 2×2 system each sample. Capacitor values never change, so the waves stay valid while the VCA gain is modulated at audio rate. |
| SSM2164 | Ideal exponential current gain, −33 mV/dB, unity at 0 V |
| Precision rectifiers | Ideal: `CTRL = max(0, −Rf · Σ input currents)` |
| Op-amps | Ideal virtual grounds, outputs clipped at ±7.5 V (TL07x on ±9 V) |
| Oscillator integrators | Exact for the piecewise-constant charging current; Schmitt crossings reflected within the sample |
| CD4021, XOR, Q1 | Logic at the ±9 V rails, 0 V input threshold, Q1 on above 0.6 V base drive (Thevenin of R17, R18 and the pot) |
| R-2R DAC, mix nodes, pot loading | Exact nodal analysis from the component values, including each pot's wiper resistance |

### Assumptions and simplifications

* **Pots:** all assumed linear, 10k.
* **Op-amp swing:** VSAT = 7.5 V.
* **Filter self-modulation:** the bandpass→cutoff path uses the previous sample's bandpass value, because a nonlinear delay-free loop can't be solved explicitly.
* **SSM2164 compensation:** the 510 Ω + 560 pF networks on the inputs (corners above 500 kHz) are left out.
* **Integrator saturation:** an integrator at the rails is clamped, rather than modelling the op-amp losing its virtual ground.
* **Frequency warping:** the trapezoidal rule warps the filter's top octave. A 17 kHz analogue cutoff lands near 13 kHz.
* **Aliasing:** the PWM comparator feeding the filter is band-limited with polyBLEP. The triangles and pulses are not.
* **Not modelled:** SSM2164 temperature drift and distortion, and TL07x slew rate.

## Controls

Tap Z down to step through four pages. The left column of LEDs shows the page; page 4 lights all three.

| Page | Main | X | Y |
|---|---|---|---|
| 1 | Osc 1 rate (R54) | Rungler → Osc 1 (R55) | Osc 1 CV / cross mod (R51) |
| 2 | Osc 2 rate (R56) | Rungler → Osc 2 (R57) | Osc 2 CV / cross mod (R52) |
| 3 | Filter cutoff (R58) | Rungler → filter (R61) | Resonance (R59) |
| 4 | Chaos (R60) | Filter CV depth (R53) | Output level |

* **Switch:** Z up closes **SW1**, connecting the Chaos pot. Z middle opens it, giving pure XOR chaos.
* **Pickup:** knobs use soft takeover, so a parameter only follows its knob once the knob reaches its value.
* **Direction:** every pot is mapped so clockwise means more: faster, more depth, higher cutoff, more resonance.

## Jacks

| Jack | Benjolin jack | Function |
|---|---|---|
| CV In 1 | J1 | Osc 1 CV through R51; normalled to triangle 2 |
| CV In 2 | J2 | Osc 2 CV through R52; normalled to triangle 1 |
| Audio In 1 | J3 | Filter input through C5/R30 |
| Audio In 2 | J4 | Filter CV through R53; normalled to triangle 2 |
| Pulse In 1 | — | External rungler clock (replaces oscillator 2) |
| Pulse In 2 | — | While high, flips SW1 |
| Audio Out 1 | J9 | Filter out (×5, clipped), scaled by output level |
| Audio Out 2 | J10 | PWM comparator, scaled by output level |
| CV Out 1 | J11 | Rungler CV (bipolar, ±5.25 V) |
| CV Out 2 | J7 | Triangle 2 |
| Pulse Out 1 / 2 | J8 / J5 | Oscillator 1 / 2 pulse |

LEDs, right column: rungler CV, PWM state, filter output level.

## Building

```sh
mkdir build && cd build
PICO_SDK_PATH=/path/to/pico-sdk cmake ..
make
```

The card runs at 192 MHz (1.15 V core) from RAM.

* **Core 0** runs the circuit in integer arithmetic.
* **Core 1** recomputes circuit coefficients in floating point when a pot moves.
* **Profiling:** `-DWDFB_PROFILE=ON` lights the bottom-right LED permanently if `ProcessSample` ever takes more than 75% of its budget.

In a Cortex-M0+ instruction-level emulator, using the Pico SDK's 64-bit multiply, the circuit takes about 1,530 cycles per sample. That's about 38% of the budget at 192 MHz, before the card glue and the ComputerCard interrupt. It hasn't been timed on hardware.

### Host tests

```sh
g++ -O2 -std=c++17 -I. test/host_sim.cpp -o host_sim && ./host_sim /tmp
```

* **Oscillator rates:** match the integrator/Schmitt analysis within 0.3% from 40 Hz to 5 kHz.
* **Rungler:** the DAC levels match the R-2R closed form, the XOR truth table matches, and SW1 with Chaos fully either way gives 8-step and 16-step loops.
* **Filter:** the lowpass response matches an independent complex-frequency solve of the circuit equations, written directly from the netlist, within 0.03 dB over cutoff, resonance and frequency.
* **Stress:** a 20-second test with random pots and external input stays bounded.
* **Renders:** the tests also write example patches to WAV.

## Licence and credits

The Benjolin Gorga design is © squaresolid, licensed **CC BY-NC 4.0**: non-commercial use only. This model is derived from that design and carries the same licence. The Benjolin is Rob Hordijk's design (RIP). The Eurorack v1.6 layout this builds on is by Forestcaver.

ComputerCard is © Chris Johnson, MIT licence (see `ComputerCard.h`).
