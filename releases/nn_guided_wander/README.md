# Guided Wander

Random pitch CV for the Workshop Computer, with the randomness guided by a
[Music Thing 8mu](https://www.musicthing.co.uk/8mu_page/): its faders are
probabilities. Two voices wander at once. At every step each draws a note of
the scale, an octave, a gate length and a ratchet, each weighted by the faders,
and plays them as a 1V/oct pitch, a gate and an envelope.

Built from the same template as WaveSeq and CVSeq: the same 8mu paging and
pickup, USB modes, knob pickup, web editor, 8mu sharing and presets. It works
with an 8mu alone, no computer needed.

> The folder is `nn_guided_wander`: give it a release number when it goes in.

## The 8mu

The four buttons choose the page, acted on when you **let go**. Press a button
again for its **second page**, and again to go back.

| Button | First page | Second page |
|---|---|---|
| A | **NOTES**: the scale's notes | **NOTES 2** |
| B | **OCTAVES**: −2 V (fader 1) to +5 V (fader 8) | **OCTAVES 2** |
| C | **GATES**: 0% (a rest), then evenly up to 100%, which is tied | **GATES 2** |
| D | **RATCHETS**: 1×, 1.5×, 2×, 3× on one note; the same with a new note at every hit | **RATCHETS 2** |

**Faders are relative probabilities.** A fader at zero is never chosen, and
one twice as high as another is chosen twice as often. With a whole page at
zero, every choice is equally likely.

**Two voices.** The first pages are shared by both voices. The second pages
belong to voice 2, but only once they are used:

- While **every fader on a second page is at zero**, voice 2 draws from the
  first page as well. You get two different lines with the same probabilities.
- **Raise any fader** on a second page and voice 2 uses that page from then
  on, independently of voice 1. Voice 1 keeps the first page.

Each pair decides this on its own. Voice 2 can share the notes and octaves
while having its own gates, for example. Pull every fader on a second page
back to zero to share that pair again.

**Notes.** One fader per note of the scale, from the root up. Scales with
fewer than eight notes ignore the extra faders.

**Gates.** Fader 1 is a rest: no gate and no envelope, though the pitch still
moves. Faders 2-7 are 14% to 86% of the time to the next hit. Fader 8 is
**tied**: the gate stays up into the next note, which glides in without
retriggering the envelope. A gate always falls a millisecond before the next
hit, so every new note gets a fresh envelope.

**Ratchets.** Faders 1-4 play 1×, 1.5×, 2× and 3× the step rate, every hit on
the step's note, octave and gate. Faders 5-8 play the same speeds, but draw a
new note, octave and gate at every hit. At 1.5× a step has two hits, the
second two thirds of the way through.

**Pickup.** After a page change the faders do nothing until they reach (or
pass) the stored value, so changing page never changes the probabilities by
accident.

**LEDs.** For **3 seconds** after a fader or button is touched, the LEDs show
the page's probabilities, the likeliest fully lit. After that they show what
is playing:

| Page | Probabilities (3 s after a touch) | Then |
|---|---|---|
| Notes, Octaves | brightness = probability | the note or octave just chosen |
| Gates | brightness = probability | a bar up to the gate length playing: 0% lights the first LED dimly, 50% the first four, 100% all eight |
| Ratchets | each fader flashes its own speed in time with the clock | the ratchet in use flashes at every hit |

On a first page that voice 2 shares, voice 2's choice also shows, dimmer.

Without an 8mu the card plays its default probabilities, still under the
panel controls.

## Panel

| Control | Function |
|---|---|
| Switch up | Main = **scale** (below). X = **clock** ÷8, ÷6, ÷4, ÷3, ÷2, ×1 (centre), ×2, ×3, ×4, ×6, ×8. Y = **slew** on the pitch: off across the bottom of the knob, then 1 ms to about 2 s |
| Switch middle | Main = envelope **decay**, 5 ms to about 5 s. X = **attack**, 0.5 ms to about 1 s. Y = **pluckiness**: down, the decay is a straight fall; up, it drops fast and leaves a long tail (exponential) |
| Switch down | Nothing; the knobs keep the settings they had in the position the switch came from |

The six knob settings each keep their value. After the switch moves, a knob
does nothing until it is turned to (or past) its new setting's value. LED 6
(bottom right) blinks fast while a knob is waiting.

| Jack | Function |
|---|---|
| Pulse In 1 | Clock. Each step is one beat, divided or multiplied by X. The card follows the clock from its second pulse; multiplied beats are spaced evenly and locked to it. With no clock (or four of its periods, at least 2 s, after it stops) the card keeps its own 120 BPM, still divided or multiplied by X |
| CV Out 1, 2 | **Pitch** of voice 1, 2: 1 V/oct, C at 0 V, slewed |
| Audio Out 1, 2 | **Envelope** of voice 1, 2, 0 to 5 V (uncalibrated), triggered by every hit of the ratchet |
| Pulse Out 1, 2 | **Gate** of voice 1, 2 |
| Others | Not used |

Every hit retriggers its envelope from wherever it is, so fast ratchets never
click. A rest or a tied note doesn't retrigger it.

**LEDs.** With an 8mu or the web editor connected, LEDs 1-4 show the page:
steady for a first page, blinking for a second. Without either they show the
two gates and the two envelopes. LED 5 flashes at each step. When the scale
changes, LEDs 1-5 show its number in binary for a second (LED 1 the lowest
bit).

## Scales

32 scales, in order round the Main knob. None has more than eight notes.

| | |
|---|---|
| 1-9 | Major, Natural minor, Harmonic minor, Melodic minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian |
| 10-15 | Major pentatonic, Minor pentatonic, Blues, Whole tone, Octatonic, Augmented |
| 16-20 | Hirajoshi, In-sen, Hungarian minor, Phrygian dominant, Double harmonic |
| 21 | Root and fifth |
| 22-24 | **5-TET**, **7-TET**, **13-TET** (eight of its thirteen steps: 0 2 4 5 7 9 10 12) |
| 25-28 | 19-TET diatonic, Maqam Rast and Maqam Bayati (24-TET quarter tones), 31-TET diatonic |
| 29-30 | Just major (5-limit), Overtones 8-15 (harmonics 8 to 15) |
| 31-32 | **Slendro** and **Pelog** (gamelan) |

The gamelan scales use typical published tunings. Real gamelans each have
their own, so treat them as a flavour rather than a reference. The tables are
in [`wander.h`](wander.h), in cents, if you want to add your own.

## Three ways to use it

| Plugged into the Computer's USB socket | The card is | The faders are |
|---|---|---|
| an 8mu | USB host | the 8mu's, read directly. No computer needed |
| a computer | a USB MIDI device called **Guided Wander** | the web editor's, and any 8mu plugged into the *computer* is passed on by the editor |
| nothing | USB host, waiting | (plug an 8mu in any time) |

The card picks its mode once, at power-up, so **after plugging a computer in,
power-cycle the module**. This needs Computer Rev 1.1 hardware; older boards
are always a USB device.

## Web editor

Open [`web/index.html`](web/index.html) in Chrome or Edge, with the computer
plugged into the Computer's USB socket and the module power-cycled. Press
**Connect card & 8mu**. It's a single file and needs no network.

- **Playing**: the scale, clock, slew and envelope settings live from the
  card (a dotted outline while a knob is waiting to pick up), both voices'
  pitch, note, octave, gate, ratchet, gate lamp and envelope, and a scope of
  both pitches.
- **Faders**: the eight pages as tabs (A, A2, B, B2…). A second page that
  voice 2 is sharing has a dashed tab and says so. Under each fader is its
  share of the draws. **Even**, **Zero** (on a second page, voice 2 shares
  again), **Copy first page to second** (a starting point for voice 2) and
  **Randomise**.
- **Probabilities**: each kind as a chart, voice 1 filled and voice 2
  outlined when it has its own, with dots for what each voice last drew.
- **Presets**: eight built in, plus your own, kept in the browser. Save,
  update, rename, delete, export and import as JSON. Loading one sends it to
  the card.
- **Several 8mus**: every 8mu on the computer drives the page together, and
  their LEDs all follow it.
- **Several editors**: with the WaveSeq or CVSeq editor open too, one 8mu
  drives whichever editor you last clicked in. The header shows **8mu: here**
  or **8mu: elsewhere**; clicking it, or anywhere on the page, takes the 8mu.
  The other page ignores it but keeps track of where the faders are, so they
  pick up as usual when it comes back. Use two windows side by side rather
  than tabs, as Chrome slows a background tab.

Without a card the page still edits, keeps presets and previews: it plays the
two voices itself at 120 BPM, in a scale you choose.

The protocol is documented in [`sysex.h`](sysex.h).

## Building

```
mkdir build && cd build
cmake .. && make
```

Needs the Pico SDK (built here with 2.1.1). `EightMU.h` and `ComputerCard.h`
are copied from CVSeq, unmodified.

### Tests

The engine ([`wander.h`](wander.h)) has no hardware in it, so it is tested on
a computer. The whole card (`main.cpp`) is also run sample by sample against
stand-ins for the Computer and the 8mu:

```
test/run_tests.sh        # engine, then the whole card: needs only g++
python3 test/web_test.py # the web editor in headless Chromium, with a fake card and two fake 8mus (needs playwright)
```

## Not yet

- Probabilities aren't saved on the card, so they're lost at power-off. Keep
  them as presets in the web editor and send them again after power-up.
- Not yet tried on hardware: the tests run on a computer, and the firmware
  builds, but the timing of a sample on the RP2040 hasn't been measured.
