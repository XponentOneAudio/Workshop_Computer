# 46_Dual_Borg

**Two contrasting 12 dB filters** for the Music Thing Workshop System Computer,
modelled on the idea behind the Malekko / Richter **Dual Borg**: one module, two
filters that deliberately *do not* sound or respond the same.

- **Borg I** (left column) is slow and mellow. Its response is vactrol-like
  (cutoff opens in ~5 ms and closes in ~80 ms), it saturates softly, and its
  resonance sings but never self-oscillates.
- **Borg II** (right column) is fast and bright. It follows control changes
  almost instantly, and its resonance loop clips hard in the style of the MS-20,
  so it shrieks and, at the top of the knob, self-oscillates.

Both are 2-pole (12 dB/oct) low-pass filters. Both also have a **VCA mode**,
where they behave like a Buchla-292-style low-pass gate (level and cutoff open
together) for short, percussive sounds.

> This is an unofficial software model inspired by the published description of
> the hardware. It is not affiliated with or endorsed by Malekko, and it does not
> reproduce that module's circuit or panel exactly.
>
> Status: **0.1 beta**. The DSP has only been tested in the host simulator
> (`sim/`), not yet on hardware, and no `.uf2` is committed yet (see **Build**).

## Panel / I/O

Left column of jacks = **Borg I**, right column = **Borg II**.

| Control | Function |
| --- | --- |
| **Audio In 1 / 2** | Filter inputs. **Audio In 2 is normalled to Audio In 1**, so a single source feeds both filters. |
| **Audio Out 1 / 2** | Filter outputs. |
| **CV In 1 / 2** | Cutoff CV, roughly 1 V/oct (uncalibrated). In VCA mode a positive CV also opens the gate. |
| **Pulse In 1 / 2** | **Ping.** Fires a decaying envelope: Borg I decays slowly (~350 ms) and Borg II quickly (~120 ms). In filter modes it sweeps the cutoff up about 4 octaves; in VCA mode it strikes the gate fully open. If no audio is patched, a ping also injects a short noise burst, so the filters can be played as drums. |
| **CV Out 1 / 2** | Each filter's slewed cutoff control as 0 to +6 V. This includes Borg I's lazy response, so it doubles as a smoothed envelope. |
| **Knob X / Knob Y** | Cutoff for Borg I (25 Hz to 9 kHz) and Borg II (25 Hz to 16 kHz). |
| **Main knob** | Switch **Up** or **Down**: resonance for both filters. Switch **Middle**: input drive (1× to 8×). |
| **Switch** | **Down = VCA mode** (low-pass gate). **Middle = filter, resonance off.** **Up = filter, resonance on.** |
| **LEDs** | Top row: output level. Middle row: cutoff. Bottom row: ping envelope. |

### VCA mode tips

In VCA mode the cutoff knobs set how far the gate is open **at rest**. Turn
X / Y fully anticlockwise to close the gates, then open them with pings or a
positive CV. Resonance is limited in this mode, so the gate rings without
running away.

## Patch ideas

- **One source, two characters.** Patch an oscillator into Audio In 1 only, put
  the switch Up, and listen to both outputs (or pan them left and right). Patch
  the same LFO into both CV inputs: Borg I swoops lazily, while Borg II snaps
  and screams.
- **Plucks.** Switch Down, X / Y fully anticlockwise, oscillator into Audio In 1
  and a clock into Pulse In 1 and 2. Borg I gives soft, woody "bonks" and
  Borg II gives brighter, snappier plucks.
- **Screaming sine.** Switch Up, Main fully clockwise, nothing in the audio
  inputs. Borg II self-oscillates. Its pitch is set by Knob Y, and it tracks
  CV In 2 at roughly 1 V/oct.
- **Filter drums.** Unplug the audio inputs, switch Up with resonance high, and
  send clocks to the pulse inputs. Each ping strikes the filter with a noise
  burst and an upward sweep.
- **Grit.** Switch Middle and turn Main up to drive the inputs. Borg II
  distorts harder than Borg I.

## How it works (brief)

Each side is a zero-delay-feedback state-variable low-pass filter (Cytomic /
Simper form) in 32/64-bit integer arithmetic. The band-pass state passes
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
test patches to WAV (sweep, self-oscillation, VCA plucks, drive, drums). It
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
