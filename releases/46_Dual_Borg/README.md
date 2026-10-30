# 46_Dual_Borg

**Two contrasting vactrol-style filters** for the Music Thing Workshop System
Computer, modelled on the Malekko / Richter **Dual Borg**: one module, two
filters that deliberately *do not* sound or respond the same.

- **Borg I** (left column) is slow and mellow. Its response is vactrol-like
  (cutoff opens in ~5 ms and closes in ~80 ms), it saturates softly, and its
  resonance sings but never self-oscillates.
- **Borg II** (right column) is fast and bright. It follows control changes
  almost instantly, and its resonance loop clips hard in the style of the MS-20,
  so it shrieks and, at the top of the knob, self-oscillates.

As on the hardware, with resonance **off** each Borg is a **low-pass gate**
(filter and VCA together, Buchla 292 style), and with resonance **on** it is a
**12 dB filter**. A continuous **low-pass → notch → high-pass** control and
**series / parallel** routing are also modelled.

> This is an unofficial software model inspired by the published description of
> the hardware. It is not affiliated with or endorsed by Malekko, and it does not
> reproduce that module's circuit or panel exactly.
>
> Status: **0.2 beta**. The firmware builds and the DSP has been tested in the
> host simulator (`sim/`), but it has not yet been tested on hardware.
> Prebuilt firmware: [`build/dual_borg.uf2`](build/dual_borg.uf2).

## Panel / I/O

Left column of jacks = **Borg I**, right column = **Borg II**.

| Control | Function |
| --- | --- |
| **Audio In 1 / 2** | **Parallel:** filter inputs, with **Audio In 2 normalled to Audio In 1** so a single source feeds both filters. **Series:** both inputs are mixed into Borg I. |
| **Audio Out 1 / 2** | **Parallel:** Borg I / Borg II. **Series:** both outputs carry the end of the chain (Borg I → Borg II). |
| **CV In 1 / 2** | Cutoff CV, roughly 1 V/oct (uncalibrated). In LPG mode a positive CV also opens the gate. |
| **Pulse In 1 / 2** | **Ping.** Fires a decaying envelope: Borg I decays slowly (~350 ms) and Borg II quickly (~120 ms). In filter mode it sweeps the cutoff up about 4 octaves. In LPG mode it strikes the gate fully open. If no audio is patched, a ping also injects a short noise burst, so the filters can be played as drums. |
| **CV Out 1 / 2** | Each filter's slewed cutoff control as 0 to +6 V. This includes Borg I's lazy response, so it doubles as a smoothed envelope. |
| **Knob X / Knob Y** | Cutoff for Borg I (25 Hz to 9 kHz) and Borg II (25 Hz to 16 kHz). In LPG mode this also sets how far each gate rests open. |
| **Main knob** | Switch **Up**: resonance for both filters. Switch **Middle**: **LP → notch → HP** for both filters (fully anticlockwise = low-pass, noon = notch, fully clockwise = high-pass). |
| **Switch** | **Up = filter, resonance on** (12 dB). **Middle = low-pass gate**, resonance off. **Down (momentary) = tap to toggle parallel / series.** |
| **LEDs** | Top row: output level. Middle row: cutoff. Bottom row: ping envelope. For one second after a routing change, **all six LEDs = series** and **top pair only = parallel**. |

The Main knob keeps both settings. When you move the switch, the setting for
the new position stays where you left it, and the knob takes over again once
you turn it past that value. Changing the switch never makes the resonance or
the LP/HP setting jump. At power-on, the knob takes over the setting for the
switch position straight away. The other setting starts at zero (no resonance,
or low-pass).

Tapping Down leaves the switch in Middle (LPG mode). To go back to the
resonant filter, flick it Up again.

### LPG mode tips

Turn X / Y fully anticlockwise to close the gates, then open them with pings
or a positive CV. Keep Main anticlockwise (low-pass) for classic "bonks".
Turning it towards high-pass gives thinner, clickier strikes.

### Differences from the hardware

- The hardware has a CV attenuator on one CV input. Here both CV inputs are
  fixed at about 1 V/oct.
- The hardware's resonance and LP/HP controls belong to each Borg. Here one
  Main knob sets them for both.
- The hardware gives a mix of both filters when only one output is patched in
  parallel mode. The Computer can't sense its outputs, so this is not modelled.
- The pulse-input pings are an addition. The hardware has no ping inputs.

## Patch ideas

- **One source, two characters.** Patch an oscillator into Audio In 1 only, put
  the switch Up, and listen to both outputs (or pan them left and right). Patch
  the same LFO into both CV inputs: Borg I swoops lazily, while Borg II snaps
  and screams.
- **Plucks.** Switch Middle, Main and X / Y fully anticlockwise, oscillator into
  Audio In 1 and a clock into Pulse In 1 and 2. Borg I gives soft, woody
  "bonks" and Borg II gives brighter, snappier plucks.
- **Four-pole squelch.** Tap Down for series, then flick Up. Borg I's slow,
  rounded sweep feeds Borg II's fast, shrieking one for a steeper, two-stage
  filter. Patch an envelope into CV In 1 and an LFO into CV In 2.
- **Screaming sine.** Switch Up, Main fully clockwise, nothing in the audio
  inputs. Borg II self-oscillates. Its pitch is set by Knob Y, and it tracks
  CV In 2 at roughly 1 V/oct.
- **Filter drums.** Unplug the audio inputs, switch Up with resonance high, and
  send clocks to the pulse inputs. Each ping strikes the filter with a noise
  burst and an upward sweep.

## How it works (brief)

Each side is a zero-delay-feedback state-variable filter (Cytomic / Simper
form), whose low-pass and high-pass outputs are crossfaded for the LP → notch →
HP control, in 32/64-bit integer arithmetic. The band-pass state passes
through a cubic soft clipper, which is what bounds the resonance. Borg II's
clipper starts at full scale (gritty, and allows self-oscillation with a
slightly negative damping), while Borg I's starts at 4× full scale (round), and
Borg I's damping never goes below Q ≈ 6.

Cutoff is computed in 1/96-octave steps from the knob, CV and ping envelope,
then slewed with asymmetric attack and release times. These times, the top
cutoff and the ping decay are what give the two Borgs their different "speed".
Filter coefficients are recomputed with float maths at a 6 kHz control rate;
the per-sample audio path is integer only.

The constants for each Borg are in the `chars[]` table in `src/dual_borg.cpp`.

## Listen without hardware

`sim/` compiles the real card source against a mock ComputerCard and renders
test patches to WAV (sweep, self-oscillation, LPG plucks, LP/HP morph,
series routing, drums). It
also prints Borg II's self-oscillation pitch for each CV voltage, to check
tracking:

```sh
cd releases/46_Dual_Borg/sim
./build.sh
```

## Build

Requires the [RPi Pico SDK](https://github.com/raspberrypi/pico-sdk) with
`PICO_SDK_PATH` set:

```sh
cd releases/46_Dual_Borg/src
mkdir -p build && cd build
cmake ..
make
```

This produces `dual_borg.uf2`. To flash it, hold BOOTSEL and copy the file onto
the `RPI-RP2` drive.

Built with [Chris Johnson's ComputerCard library](https://github.com/TomWhitwell/Workshop_Computer/tree/main/Demonstrations%2BHelloWorlds/PicoSDK/ComputerCard).
License: MIT.
