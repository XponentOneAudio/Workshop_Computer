# what.three.wiggles

*Prototype, not yet tested on hardware. See [TASKS.md](TASKS.md) for what's done and what's next.*

A program card for the Music Thing Workshop Computer that takes its name from what3words.

Type three words into the web page and send them to the card over USB. A what3words address works
well, but any three words will do. Each word becomes a seed, and the seeds set up every oscillator,
LFO, random walk and gate pattern on the card. The same words always give the same patch, so a place
always has the same sound. The card saves the words, so the patch is still there after power-off.

## Getting started

1. Flash `UF2/what_three_wiggles.uf2` to a card.
2. Connect the Computer to a computer by USB, and open [`web/index.html`](web/index.html) in Chrome,
   Edge or Opera. Once the card is in the catalogue, the site hosts this page too.
3. Type three words and press **Send to card**. The LEDs chase round when the card takes them.

The page shows the seed for each word. The card works the seeds out again for itself and reports
them back, and "✓ seeds match" confirms that the page and the card agree.

## Panel

| Jack | What it does |
|---|---|
| **Audio Out 1** | Drone: three operators, one per word, in a 1 → 2 → 3 → 1 feedback FM loop (switch middle) or ring modulated (switch up). |
| **Audio Out 2** | Additive oscillator of 16 harmonic partials. Word 1 sets the low partials, word 2 the middle, word 3 the top. |
| **CV Out 1** | Random walk: calibrated V/oct, wandering through the scale above C4, with random note lengths and random glides. |
| **CV Out 2** | Complex LFO: three sines at word-seeded ratios, cross-modulated and wavefolded, about ±5 V. |
| **Pulse Out 1** | Random triggers (10 ms), on random clock ticks. |
| **Pulse Out 2** | Flip-flop gates, which change state on every Pulse 1 trigger. |
| **Audio In 1** | Adds to the FM depth (or ring mod amount). 0 to 5 V covers the full range. |
| **CV In 1** | V/oct for Audio 2 (0 V = C4), quantised to the scale on Knob X. |
| **CV In 2** | Sample and hold trigger for Audio 2's pitch (rising edge above about 1.2 V). With nothing plugged in, Audio 2 follows CV In 1 continuously. |
| **Pulse In 1** | Clock. Plugging a clock in replaces the internal one. |
| **Pulse In 2** | Reset: the walk, LFO, triggers and flip-flop restart from the seeds, so the same phrase plays again. |

## Knobs and switch

There are two pages of knobs. **Tap the switch down** to flip between them. LED 4 is lit on page B.

| Knob | Page A | Page B |
|---|---|---|
| **Main** | FM depth (or ring mod amount) | CV 1 range: 1 to 48 semitones |
| **X** | Quantiser scale (Audio 2 and CV 1): chromatic, major, minor, dorian, mixolydian, major pentatonic, minor pentatonic, whole tone | Trigger density: silent → the words' own density (at noon) → every tick |
| **Y** | Internal clock: 0.25 to 20 Hz | LFO rate: 0.02 to 20 Hz |

Knobs use **soft takeover**. After a page flip, a knob does nothing until it reaches the value it had
on that page, so nothing jumps. LED 5 blinks until every knob has caught up.

**Switch:** middle = FM loop, up = ring mod. Tapping down flips the page. Holding it down for a
second resets, like Pulse In 2. Because the switch springs back to the middle after a tap, flipping
pages while in ring mod passes briefly through FM mode. The change fades in and out, so it doesn't
click.

**LEDs:** 0 = Pulse 1, 1 = Pulse 2, 2 = CV 1, 3 = CV 2, 4 = page B, 5 = clock tick (or blinking
while a knob waits for soft takeover).

### Try this

Patch **CV Out 1 → CV In 1** and **Pulse Out 1 → CV In 2**. Audio 2 then plays the random walk,
changing note on the random triggers.

## Building and testing

The firmware builds with the Pico SDK (2.2.0 with TinyUSB 0.20.0, as in the repo's DevContainer):

```sh
mkdir build && cd build
PICO_SDK_PATH=/path/to/pico-sdk cmake .. && make
```

The seeding, DSP, engines and USB protocol are plain C++ with no Pico SDK dependencies, so they are
tested on a desktop:

```sh
test/run.sh
```

`test/run.sh` runs three things:
- the C++ tests, with sanitisers that catch integer overflow in the fixed-point maths
- the webapp's seeding tests in node
- if Playwright is installed, a browser test that drives `web/index.html` against an emulation of the
  card built from the firmware's own `protocol.h`

To hear a set of words without a card:

```sh
g++ -std=c++17 -O2 test/render.cpp -o build/render
build/render index.home.raft 20
```

This writes `index.home.raft_audio.wav` (Audio 1 and 2) and `index.home.raft_cv.wav` (CV 1, CV 2,
Pulse 1, Pulse 2), using the same engine code as the card.

## Files

| File | What's in it |
|---|---|
| `main.cpp` | The card: controls, fades, LEDs, USB SysEx, saving the words to flash |
| `engines.h` | Clock, random triggers, flip-flop, random walk, LFO, additive osc, FM loop |
| `patch.h` | How the three seeds become every engine's settings |
| `seed.h`, `web/seed.js` | The seeding spec in C++ and JS (see [SEEDING.md](SEEDING.md)) |
| `protocol.h` | The SysEx messages between the web page and the card |
| `dsp.h` | Sine table, V/oct, scales and quantiser |
| `ComputerCard.h` | Chris Johnson's ComputerCard 0.4.0, plus a `CVInMillivolts()` accessor for calibrated CV input |

*what3words is a trademark of what3words Ltd. This project is not affiliated with or endorsed by
what3words.*
