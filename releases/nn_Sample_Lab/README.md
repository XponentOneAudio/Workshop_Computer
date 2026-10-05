# Sample Lab

Sampling, time-stretching and granular synthesis for the Workshop Computer,
edited from a [Music Thing 8mu](https://www.musicthing.co.uk/8mu_page/) over
USB MIDI host, with a web app that shows what each grain is doing and
explains why.

The card holds a little over two seconds of sound and plays it back in one
of four ways:

| 8mu button | Engine | What it does |
|---|---|---|
| A | **Varispeed** | Reads the sample faster or slower, like tape: speed and pitch are one thing ([varispeed](https://en.wikipedia.org/wiki/Varispeed)) |
| B | **Overlap-add** | Cuts it into overlapping windowed grains, taken at the speed and played at the pitch, so the two come apart ([overlap-add](https://en.wikipedia.org/wiki/Overlap%E2%80%93add_method)) |
| C | **WSOLA** | Overlap-add, but each grain slides to where it best continues the last, so the joins line up ([time stretching](https://en.wikipedia.org/wiki/Audio_time_stretching_and_pitch_scaling)) |
| D | **Cloud** | Many grains scattered in time and pitch around the playhead ([granular synthesis](https://en.wikipedia.org/wiki/Granular_synthesis)) |

The card is mostly for learning, so it's deliberately small, and the web app
carries the explanations.

## The sample

At power-up the card makes a **demo loop**: one bar at 120 bpm, two seconds,
of kick, snare, hats and a plucked melody (the plucks are
[Karplus-Strong](https://en.wikipedia.org/wiki/Karplus%E2%80%93Strong_string_synthesis)
synthesis). The drums' sharp attacks and the plucks' clear pitch between them
show off what each engine does well and badly. It's made by `demo.h`, not
stored, and the web app makes the same loop.

**Recording.** Put the switch up, or send Pulse In 1 high, and the card
records Audio In 1 from that moment, until the switch comes down (or the
pulse goes low) or the buffer, 100,000 samples (about 2.08 s at 48 kHz), is
full. While recording, both outputs monitor the input. To record again after
filling the buffer, bring the switch down and up again.

The web app can also send the card a sample (from a file, the microphone or
the demo loop) and read back what the card holds.

## The 8mu

The buttons choose the engine; the eight faders are:

| Fader | Sets | Used by | Range |
|-------|------|---------|-------|
| 1 | Position | all | where in the sample to play, added to the playhead (and CV In 2) |
| 2 | Overlap / density | B, C / D | grains overlapping, 1 to 8 / grains per second, 5 to 200 |
| 3 | Spray | B, D | random scatter of where each grain starts, up to 500 ms |
| 4 | Window | B, C, D | grain shape, from square (clicks) to a Hann curve |
| 5 | Search | C | how far WSOLA may move a grain to line it up, up to 15 ms |
| 6 | Pitch spray | D | random pitch per grain, up to an octave either way |
| 7 | Bits | all | output bit depth, 12 down to 1 |
| 8 | Rate | all | output sample rate, 48 kHz down to 1 kHz, unfiltered |

**Pickup.** The faders don't do anything until they reach the value already
stored (or pass it), then take it over, so nothing jumps. The 8mu's LEDs show
the stored values.

## Panel

| Control     | Function |
|-------------|----------|
| Main knob   | Speed, -2 to 2: reverse to the left of centre, stopped in the middle, snapping to 0 and to 1 either way |
| X knob      | Pitch, -24 to 24 semitones, snapping to 0. For varispeed it changes the speed too |
| Y knob      | Grain size, 10 ms to 500 ms |
| Switch up   | Record |
| Switch middle | Play |
| Switch down | Tap for the next engine |

| Jack        | Function |
|-------------|----------|
| Audio In 1  | Record input |
| Audio In 2  | Speed, added to the Main knob |
| CV In 1     | Pitch, 1V/oct |
| CV In 2     | Position, added to fader 1 |
| Pulse In 1  | Record while high |
| Pulse In 2  | Restart: playhead back to the start |
| Audio Out 1 | The chosen engine, after Bits and Rate |
| Audio Out 2 | Varispeed at the same speed, in step with Out 1: the sound without time-stretching |
| CV Out 1    | Playhead, 0-5V across the sample |
| CV Out 2    | The newest grain's window, 0-5V |
| Pulse Out 1 | A trigger at the start of each grain |
| Pulse Out 2 | A trigger each time the playhead loops |

**LEDs.** LEDs 1-4 show the engine (A-D), all four flashing on a change.
LED 5 is lit while recording and otherwise flickers with each grain. LED 6 is
lit while an 8mu or the web app is connected.

### What to listen for

- **Varispeed** at half speed is an octave down and twice as long. At 0 it's
  silent, as stopped tape is, and below 0 it plays backwards.
- **Overlap-add** at half speed keeps the pitch, but grains meet out of step,
  so pitched sounds flutter and drums flam. On a pure 440 Hz tone at half
  speed the card's overlap-add wobbles in level by 4-8% and drifts several Hz
  sharp; WSOLA holds it at 0.4% and exactly 440 Hz.
- **WSOLA** with Search at 0 is exactly overlap-add, so the Search fader
  shows what the search buys.
- **Cloud** at speed 0 freezes a moment indefinitely, which varispeed can
  never do.
- **Bits** and **Rate** reduce the output as cheap or early converters would:
  each bit is about 6 dB of noise, and with no filtering, anything above the
  new [Nyquist frequency](https://en.wikipedia.org/wiki/Nyquist_frequency)
  folds back as [aliasing](https://en.wikipedia.org/wiki/Aliasing).
- **Audio Out 2** is always plain varispeed at the same speed and position,
  so Out 1 against Out 2 is "stretched" against "not stretched".

### How it works

Every engine has a **playhead** moving through the sample at the speed. For
varispeed a read head moves at speed × pitch and that's the output. For the
grain engines, each grain is a short slice read from around the playhead at
the pitch, faded in and out by a
[Tukey window](https://en.wikipedia.org/wiki/Window_function#Tukey_window)
(square at one extreme, [Hann](https://en.wikipedia.org/wiki/Hann_function)
at the other), and up to 16 sound at once. Overlap-add and WSOLA start one
every grain size ÷ overlap; their sum is scaled by the overlap, as lined-up
grains add as levels. The cloud starts them at random intervals; its sum is
scaled by the square root of the overlap, as random grains add as power.

WSOLA's search runs a little at a time between grains, within a fixed budget
per sample: candidates up to the Search range either side of the playhead
are scored by their total difference, sample by sample, from how the
previous grain would have carried on, coarsely first and then finely around
the best.

Between samples the card interpolates in a straight line. The code runs from
flash to leave RAM for the 200 KB buffer, with the audio path in RAM.

## Web app

Open [`web/index.html`](web/index.html) in Chrome or Edge. It's a single file
and needs no network, other than for its links to Wikipedia.

- **The sample**, with the playhead, position, spray range and the grains
  playing now. Click to set Position. Load the demo, a file or 2 s from the
  microphone, send it to the card or read the card's back, and save it as a
  WAV file.
- **Four ways to play it back**, each with an explanation.
- **Grains in slow motion.** Above, where in the sample each grain is read
  from; below, where it's placed in the output, joined by a band, with the
  output waveform (and Audio Out 2) drawn over it. Time-stretching is the
  difference between the two spacings. WSOLA's moves show as orange ticks.
- **Faders and knobs,** with the faders an engine doesn't use faded, and
  **experiments** that set them up, each with an explanation.
- **Bits and rate.** A few milliseconds of output before and after reduction,
  with its spectrum and the new Nyquist frequency.
- **Compare, offline.** Renders the whole sample through all four engines and
  a [phase vocoder](https://en.wikipedia.org/wiki/Phase_vocoder) (which the
  card can't run) at the current settings, as playable spectrograms.
- **Explanations** of sampling, samplers, varispeed, time-stretching,
  WSOLA, the phase vocoder, granular synthesis and grain size, with
  Wikipedia links, and things to try.
- **Listen in the browser** plays the same engines from the computer, so the
  page works as a lesson on its own, with no card.

To use it with the card, plug the computer into the Computer's USB socket with
a data cable, power-cycle the module, and press **Connect card & 8mu**. The
page reads the card's settings and its sample, sends each edit as it's made,
and follows the module's knobs. An 8mu plugged into the computer drives the
page, and through it the card.

The card picks its USB mode once, at power-up: host if the port is supplying
power (an 8mu, or nothing yet), a USB MIDI device called **Sample Lab** if a
computer is. Telling them apart needs Computer Rev 1.1 hardware; older
boards are always a USB device. The card and page talk SysEx, including
the sample in 48-sample chunks; the protocol is documented in
[`sysex.h`](sysex.h).

## Building

```
mkdir build && cd build
cmake .. && make
```

Needs the Pico SDK. `EightMU.h` and `ComputerCard.h` are copied from
`Demonstrations+HelloWorlds/PicoSDK/ComputerCard`.

## Not yet

- The sample isn't saved on the card, so a recording is lost at power-off,
  and the demo loop comes back. To keep one, read it into the web app
  (**Get the card's sample**) and press **Save as WAV**.
