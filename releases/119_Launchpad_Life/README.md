# Launchpad Life

A **Game of Life sequencer** for the Music Thing Modular Workshop Computer, played on and displayed by a **Novation Launchpad X** plugged straight into the Computer's USB-C socket. No laptop needed: the Computer acts as the USB host and powers the Launchpad.

An 8×8 cellular automaton lives on the Launchpad's grid. A playhead sweeps across it column by column, and the live cells it finds become pitches on the CV outputs and gates on the pulse outputs. Every few steps the grid evolves into its next generation, so the melody keeps slowly mutating.

The source code is commented extensively for people learning C++ — start with `main.cpp`.

> **Status: beta, not yet tested on hardware.** It compiles cleanly against Pico SDK 2.2.0. If something doesn't behave as described here, please open an issue.

## Connecting

1. Flash `UF2/launchpad_life.uf2` to a program card (hold BOOTSEL while connecting the Computer to a laptop, then drop the file on the drive that appears).
2. Plug the Launchpad X into the Computer's front **USB-C** socket (USB-C to USB-C cable, or an adapter).
3. The Launchpad switches to *Programmer mode* by itself and shows the grid. Computer LED 4 (bottom left) lights when the Launchpad is connected.

The Launchpad brightness is turned down slightly (`kBrightness` in `launchpad.cpp`) to keep its current draw reasonable on the Computer's USB power.

If the Launchpad ever drops out of Programmer mode (for example if you hold the **Session** button, which opens its Settings menu), flick the Computer's switch **down**. That re-sends Programmer mode (and randomises the grid).

## The Launchpad

```
  [PLAY][RULE][EVOLVE][TEMPO][LENGTH][SCALE][ROOT][OPTIONS]   (logo)
  +-----------------------------------------------+
  |                                               | [side 1]
  |                                               | [side 2]
  |          8 x 8 Game of Life grid              | [side 3]
  |                                               | [side 4]
  |   top row = highest notes                     | [side 5]
  |   bottom row = lowest notes                   | [side 6]
  |   columns = steps, left to right              | [side 7]
  |                                               | [side 8]
  +-----------------------------------------------+
```

* **Grid pads** — press to bring a cell to life or kill it.
* **Top buttons** — choose a page. The current page is bright; the others glow dimly in their own colour.
* **Side buttons** — the controls for the current page, lit to show the current settings.
* **Logo LED** — flashes on every step: white = internal clock, orange = following Pulse In 1, dull red = stopped.

### Grid colours

| Colour | Meaning |
|---|---|
| Green | Newborn cell (born this generation) |
| Blue | Young cell (2–3 generations old) |
| Purple | Old cell |
| White | Live cell under the playhead — **this is what's playing now** |
| Faint grey column | The playhead, on empty cells |
| Dimmed | Columns outside the loop (see LENGTH) — they evolve but aren't played |

### Pages (side buttons listed top to bottom)

| # | Page (colour) | Side buttons |
|---|---|---|
| 1 | **PLAY** (green) | 1 Run / Stop · 2 Next generation (by hand) · 3 Reset playhead · 4 Randomise · 5 Clear · 6 Save snapshot · 7 Recall snapshot · 8 Freeze (keep playing, stop evolving) |
| 2 | **RULE** (red) | 1 Conway's Life B3/S23 · 2 HighLife B36/S23 · 3 Seeds B2/S · 4 Day & Night B3678/S34678 · 5 Life without Death B3/S012345678 · 6 Maze B3/S12345 · 7 2×2 B36/S125 · 8 Move B368/S245 |
| 3 | **EVOLVE** (orange) | New generation every 1 · 2 · 3 · 4 · 6 · 8 · 12 · 16 steps |
| 4 | **TEMPO** (yellow) | 40 · 60 · 80 · 100 · 120 · 140 · 160 · 200 BPM (one step = an eighth note). Ignored while an external clock is patched. |
| 5 | **LENGTH** (cyan) | Loop length 1–8 columns (shown as a bar) |
| 6 | **SCALE** (blue) | Minor pentatonic · Major pentatonic · Major · Minor · Dorian · Mixolydian · Whole tone · Chromatic |
| 7 | **ROOT** (purple) | 1–7 = C D E F G A B · 8 = sharp (♯) on/off |
| 8 | **OPTIONS** (pink) | 1 Wrap edges · 2 Reverse playhead · 3 CV Out 2 = density · 4 Long gates · 5 Auto-reseed · 6 Wide range · 7 Octave down · 8 Octave up |

**Options explained**

1. **Wrap edges** (default on) — the grid is a torus: things leaving one edge come back on the opposite edge.
2. **Reverse** — the playhead moves right to left.
3. **CV Out 2 = density** — CV Out 2 gives the number of live cells in the column (0–5 V) instead of the lowest note.
4. **Long gates** — Pulse Out 1 stays high for half a step instead of a short 10 ms trigger.
5. **Auto-reseed** (default on) — if the grid dies, or gets stuck repeating every one or two generations for four generations, it's re-randomised.
6. **Wide range** — each row jumps two scale steps instead of one, spreading the 8 rows over a wider range.
7. / 8. **Octave down / up** — shift everything by an octave.

## The Computer

### Jacks

| Jack | Function |
|---|---|
| Pulse In 1 | Clock: each rising edge is a step. With no clock for 2 seconds the internal clock (TEMPO page) takes over. |
| Pulse In 2 | Reset: the next step plays the first column. |
| CV In 1 | Transpose, about 1 V/oct, rounded to semitones. |
| CV In 2 | Chaos, added to the Main knob. |
| CV Out 1 | 1 V/oct pitch of the **highest** live cell in the column (with glide). |
| CV Out 2 | 1 V/oct pitch of the **lowest** live cell — or the column density (OPTIONS 3). |
| Pulse Out 1 | Fires on each step whose column has a live cell. |
| Pulse Out 2 | Fires when the column holds a newly born cell. |
| Audio Out 1 | Total grid population as a voltage. |
| Audio Out 2 | Live cells in the current column as a voltage. |

Empty columns are rests: no pulses, and the CV outputs hold their last notes. The bottom row with root C is C an octave below middle C (−1 V).

### Knobs and switch

| Control | Function |
|---|---|
| Main | **Chaos** — randomly flips cells each generation. Fully anticlockwise = pure rules. |
| X | **Skip** — chance that a step is silent. Fully anticlockwise = never. |
| Y | **Glide** — slide time on CV Out 1. Fully anticlockwise = instant. |
| Switch up | Run |
| Switch middle | Stop (when moved from Up) |
| Switch down | Randomise the grid and re-send Programmer mode to the Launchpad |

### LEDs

| | |
|---|---|
| Top left / right | Pulse Out 1 / Pulse Out 2 |
| Middle left / right | Column density / grid population |
| Bottom left | Launchpad connected |
| Bottom right | Flashes on each step |

## How the code is organised

The RP2040 has two cores, and this card uses both:

* **Core 0** (`main.cpp`) runs `ProcessSample()` 48,000 times a second: the clock, the playhead and all the outputs. It must never wait for anything.
* **Core 1** (`usb_core1.cpp`, `ui.cpp`, `launchpad.cpp`, `life.cpp`) runs the USB host, reads the Launchpad's buttons, computes each new generation and redraws the lights.

They talk through the variables in `shared.h`, which explains the techniques used (volatile, double buffering and request counters).

| File | What it does |
|---|---|
| `main.cpp` | Core 0: clock, playhead, scales, CV/pulse outputs |
| `shared.h` | Variables shared between the two cores |
| `usb_core1.cpp` | Core 1: USB callbacks, MIDI parser, main loop |
| `ui.h / ui.cpp` | The paged interface: what each button does, what each LED shows |
| `launchpad.h / launchpad.cpp` | Launchpad X protocol: Programmer mode, RGB LED SysEx, changed-LEDs-only updates |
| `life.h / life.cpp` | The cellular automaton and its eight rules |
| `ComputerCard.h` | Chris Johnson's Workshop Computer library (unmodified) |
| `usb_midi_host.*` | rppicomidi's USB MIDI host driver (unmodified, MIT) |
| `tusb_config.h` | TinyUSB settings |

Easy things to try changing: add your own rule to `kRules` in `life.cpp`, your own scale to `kScales` in `main.cpp`, or new colours in `ui.cpp`.

## Building

You need the [Pico SDK](https://github.com/raspberrypi/pico-sdk) (tested with 2.2.0), CMake and the ARM GCC toolchain.

```bash
cd releases/119_Launchpad_Life
mkdir build && cd build
cmake ..        # with PICO_SDK_PATH pointing at your Pico SDK
make -j4
```

This produces `build/launchpad_life.uf2`.

## Credits and licence

MIT licence. Includes, unmodified and under their own MIT licences:

* `ComputerCard.h` © Chris Johnson
* `usb_midi_host.c/.h`, `usb_midi_host_app_driver.c` © rppicomidi — https://github.com/rppicomidi/usb_midi_host

The Launchpad X protocol details come from Novation's *Launchpad X Programmer's Reference Manual*.
