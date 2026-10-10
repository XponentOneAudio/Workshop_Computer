# Budgie (card 114)

This card synthesises budgerigar chirps and chatter from analysed recordings.
It is being built in phases. The design and research behind it are in
[documentation/proposals/budgie.md](../../documentation/proposals/budgie.md).

**Status: Phase 1, the Phonetics Lab. Not yet tested on hardware.** The
element data bundled here (`budgie_data.h`) was generated from a *synthetic*
test recording. It exercises every code path, but it doesn't sound like a
real budgie. To hear budgie-like sound, analyse real recordings (see
[Updating the data](#updating-the-data)).

## The Phonetics Lab

Budgie warble is built from seven kinds of element (Tu, Smith & Dooling 2011;
Madabhushi et al. 2023). The Lab lets you pick one, choose a recorded
variant, bend it, and hear it alone and as part of a syllable.

| Control | Function |
|---|---|
| **Main** | Category, in seven zones: **A** alarm-like, **B** contact-like, **C** long harmonic, **D** short harmonic, **E** noisy, **F** click, **G** soft call |
| **X** | Variant: moves through the templates in that category |
| **Y** | Bend. At the centre the element plays as recorded. What it changes depends on the category (see below). |
| **Z up** | Auto: plays repeatedly, with a 250 ms rest between plays |
| **Z middle** | Manual: waits for a trigger |
| **Z down** (tap) | Trigger |

What Y bends:

| Category | Y left ← centre → Y right |
|---|---|
| A alarm-like | cleaner ← → harsher (noise mix) |
| B contact-like, G soft call | flatter ← → wider pitch sweeps (x0 to x2) |
| C, D harmonic | purer ← → richer (harmonics 2–4, x0 to x2) |
| E noisy | darker ← → brighter (noise centre ±1 octave) |
| F click | darker ← → trains of up to 8 clicks |

| Jack | Function |
|---|---|
| **Pulse In 1** | Trigger, same as Z down |
| **Pulse In 2** | When patched, triggers Out 2's syllable on its own |
| **CV In 1** | Pitch, roughly 1 V/oct (uncalibrated) |
| **CV In 2** | Length, x0.5 to x2 across the CV range |
| **Out 1** | The element on its own |
| **Out 2** | The element as a syllable: a consonant-like click, then the element, then a coda that is longer, quieter and lower. This is the pattern Mann et al. (2021) found in budgie warble. Non-tonal categories (A, E, F) play as they are. |
| **CV Out 1** | Out 1 pitch contour, 1 V/oct with 0 V = 1 kHz, held through silences |
| **CV Out 2** | Out 1 envelope |
| **Pulse Out 1** | 2 ms trigger at each Out 1 element onset (every click of an F train) |
| **Pulse Out 2** | Gate, high while Out 1 sounds |
| **LEDs** | The lit LED shows the category, from LED 1 = A (top left) to LED 6 = F. Soft calls (G) light all six dimly. Brightness follows the envelope. |

The proposal's later phases will add Aviary mode (generative chatter, two
birds, flock behaviour). Holding Z down at power-on is reserved for it.

## How it works

`budgie_voice.h` is the synth: one element at a time per voice. Each element
is a pitch contour driving a sine plus up to three harmonics, with amplitude
modulation (the budgie "buzz"), mixed with band-pass noise, under a
breakpoint envelope. The audio interrupt uses only integer arithmetic. Work
that doesn't depend on the knobs is done once at boot in `Prepare()`.

It has a Python twin, `synth.render()` in
[tools/budgie_analysis](../../tools/budgie_analysis). The analysis uses the
twin to resynthesise recordings, so what you hear from the toolchain's A/B
files is what the card plays. `host/test_parity.py` keeps the two in step.

### Timing

At 144 MHz the card has 3,000 cycles per sample. The two voices together,
measured in the rp2040js emulator (`host/bench`):

| | cycles |
|---|---|
| typical sample | ~690 |
| worst ordinary sample | ~1,250 |
| sample where an element starts | ~2,000 |

Element starts are the expensive samples, so only one voice may start per
sample. These figures don't include ComputerCard's own per-sample work. To
measure the real figure on hardware, build with `-DBUDGIE_PROFILE=ON`. CV
Out 2 then shows the slowest sample seen (0–2047 for 0–3,000 cycles), and
LED 6 lights above 80% of the budget.

## Building

Build like the other ComputerCard cards. With the repository's devcontainer
(Pico SDK 2.2.0), or with `PICO_SDK_PATH` set:

```sh
cmake -S . -B build && make -C build     # -> build/budgie.uf2
```

`UF2/budgie-0.1.0.uf2` is a prebuilt copy, made with the placeholder data.

## Tests (on your computer, no card needed)

```sh
cd host
make test        # voice unit checks, card simulation, firmware-vs-Python parity
make tour.wav    # a 49 s stereo tour of every category: Out 1 left, Out 2 right
```

- `render.cpp` renders every template through the firmware voice. It also
  checks modifier clamping, the retrigger fade and the one-start-per-sample
  rule.
- `card_sim.cpp` runs `main.cpp` against a stand-in for ComputerCard. It
  checks triggers, the switch modes, Pulse In 2, F click trains, CV 1 pitch
  tracking and the LEDs.
- `test_parity.py` compares the firmware voice with the Python twin.
  - **Tonal templates** must match waveform for waveform (correlation
    ≥ 0.98).
  - **Noisy templates** must match in spectrum and loudness, averaged per
    category.
- `bench/` is the cycle benchmark. `run_emulator.js` runs it in rp2040js
  (instructions at the top of the file).

## Updating the data

From `tools/budgie_analysis`, after analysing and calibrating real
recordings (see its README):

```sh
python3 -m budgie_analysis analyze recordings/ -o out --labels hand/
cp out/budgie_data.h out/model.json ../../releases/114_Budgie/
cd ../../releases/114_Budgie/host && make test
```

Then rebuild the firmware. The header holds up to 32 templates per
category. It also holds the sequence model and rhythm tables, which Phase 2
will use.

## Licence

MIT. ComputerCard.h is by Chris Johnson, under its own MIT licence.
