# Nested Circles

An additive oscillator for the Workshop Computer, edited from a
[Music Thing 8mu](https://www.musicthing.co.uk/8mu_page/) over USB MIDI host,
with a web app that draws what it's doing and explains why.

Eight sine waves, the *partials*, are added together. Each is drawn as a
circle turning at its own speed, and the circles are nested: each one rides on
the rim of the one before. The height of the outermost point, followed over
time, is the waveform, and it's what comes out of Audio Out 1. Adding sine
waves like this is [additive synthesis](https://en.wikipedia.org/wiki/Additive_synthesis);
adding them at whole-number multiples of one frequency is a
[Fourier series](https://en.wikipedia.org/wiki/Fourier_series), which can
build any repeating shape.

The card is mostly for learning, so it's deliberately small: three things to
set per partial, six starting shapes, and a web app with the explanations.

## Three ways to use it

The card works on its own; the 8mu and the web app are both optional.

| Plugged into the Computer's USB socket | The card is | The faders are |
|---|---|---|
| an 8mu | USB host | the 8mu's, read directly. No computer needed |
| a computer | a USB MIDI device called **Nested Circles** | the web app's, and an 8mu plugged into the *computer* is passed on by the web app |
| nothing | USB host, waiting | (plug an 8mu in any time) |

The card picks its mode once, at power-up, so **after plugging a computer in,
power-cycle the module**. Telling the two apart needs Computer Rev 1.1
hardware; older boards are always a USB device.

## The 8mu

The eight faders are the eight partials, lowest on the left. The buttons
choose what the faders change:

| Button | Page | Fader sets | Range |
|--------|------|------------|-------|
| A | LEVEL     | The partial's amplitude, the circle's size | 0 to 1 |
| B | PHASE     | Where the partial starts, where on its circle the point begins | 0° to 360° |
| C | FREQUENCY | How fast it turns | the middle is exactly harmonic; either side up to 12 semitones away |
| D | (shape)   | Loads the next built-in shape | saw, square, triangle, sine, pulse, bell |

**FREQUENCY defaults to harmonic.** At the middle of its fader, partial *n*
turns exactly *n* times for every turn of the first: the
[harmonic series](https://en.wikipedia.org/wiki/Harmonic_series_(music)). The
middle three fader steps all count as exact, so it's easy to find. Moving off
the middle detunes that partial, the wave stops repeating, and the sound turns
[inharmonic](https://en.wikipedia.org/wiki/Inharmonicity): beating, metallic,
bell-like.

**Levels add up.** If the circles together would reach beyond what the output
can carry, they are all scaled down together, so the shape is kept and nothing
clips. A saw is therefore a little quieter than a sine.

**Pickup.** After a page change or a new shape, the faders don't do anything
until they reach the value already stored for their partial (or pass it), then
take it over. So nothing jumps.

**LEDs.** The 8mu's LEDs show the stored values on the current page.

### The shapes

| Shape | Recipe | Read more |
|-------|--------|-----------|
| Saw | every harmonic, amplitude 1/n | [Sawtooth wave](https://en.wikipedia.org/wiki/Sawtooth_wave) |
| Square | odd harmonics, 1/n | [Square wave](https://en.wikipedia.org/wiki/Square_wave_(waveform)), [Gibbs phenomenon](https://en.wikipedia.org/wiki/Gibbs_phenomenon) |
| Triangle | odd harmonics, 1/n², every other one at 180° | [Triangle wave](https://en.wikipedia.org/wiki/Triangle_wave) |
| Sine | the first partial alone | [Sine wave](https://en.wikipedia.org/wiki/Sine_wave) |
| Pulse | every harmonic at the same level, all at 90° (cosines) | [Pulse wave](https://en.wikipedia.org/wiki/Pulse_wave), [Dirichlet kernel](https://en.wikipedia.org/wiki/Dirichlet_kernel) |
| Bell | inharmonic partials, about 1, 2.3, 4.2, 6.0, 7.3, 8.8, 10.5 and 12.9 times the fundamental | [Inharmonicity](https://en.wikipedia.org/wiki/Inharmonicity), [Strike tone](https://en.wikipedia.org/wiki/Strike_tone) |

The card starts on Saw.

## Panel

| Control     | Function |
|-------------|----------|
| Main knob   | Pitch, C1 to C7 |
| X knob      | Partials: how many sound, 1 to 8. Each fades in as the knob turns, so turning it up shows a Fourier series converging |
| Y knob      | Stretch, 0 to 0.5: partial *n* moves from *n* to *n*<sup>1+stretch</sup> times the fundamental, as in [stiff piano strings](https://en.wikipedia.org/wiki/Piano_acoustics#Inharmonicity_and_piano_size) |
| Switch up   | Harmonic lock: every partial exactly harmonic, ignoring the FREQUENCY page and stretch. Flick between up and middle to compare |
| Switch middle | Free: the FREQUENCY page and stretch apply |
| Switch down | Tap to load the next shape |

| Jack        | Function |
|-------------|----------|
| Audio In 1  | Stretch, added to Y |
| Audio In 2  | Unused |
| CV In 1     | Pitch, 1V/oct |
| CV In 2     | Partials, added to X |
| Pulse In 1  | Sync: every partial back to its start phase |
| Pulse In 2  | Next shape |
| Audio Out 1 | The sum of the partials (sines): the outermost circle's height |
| Audio Out 2 | The same partials as cosines: the outermost circle's position across |
| CV Out 1    | Pitch, 1V/oct, 0V at middle C |
| Pulse Out 1 | Square at the fundamental, high for the first half of each turn of the first circle |

**Two outputs.** Audio Out 2 is Audio Out 1's partner a quarter turn behind.
Into an oscilloscope's X-Y mode (Out 2 on X, Out 1 on Y) they draw the path
of the outermost circle, the same trail the web app draws.

**LEDs.** With an 8mu or the web app, LEDs 1-3 show the page (A-C) and LED 4
flashes when a shape loads. Without, LEDs 1-4 show the levels of partials 1-4.
LED 5 follows Pulse Out 1, and LED 6 is lit while an 8mu or the web app is
connected.

Partials fade out between 16 and 20 kHz, so high notes don't alias above the
[Nyquist frequency](https://en.wikipedia.org/wiki/Nyquist_frequency).

## Web app

Open [`web/index.html`](web/index.html) in Chrome or Edge. It's a single file
and needs no network, other than for its links to Wikipedia.

- **Circles on circles.** The eight partials as nested circles, slowed down
  hundreds of times, with the waveform they trace running off to the right.
  Optionally the trail the outer point draws (what an X-Y scope shows) and the
  shadow on the other axis (Audio Out 2). Pointing at a fader picks out its
  circle and draws its partial on its own.
- **Faders.** Pages A, B and C, like the 8mu's buttons, each fader with its
  own partial drawn above it and its frequency below. Drag, use the arrow
  keys, or double-click to reset.
- **Shapes,** each with a short explanation of its recipe.
- **Spectrum.** The same sound as bars: where each partial sits against the
  harmonic series, and how loud.
- **Explanations** of additive synthesis, Fourier series, epicycles, phase,
  inharmonicity, the two outputs and Nyquist, each with Wikipedia links, and a
  list of things to try.
- **Listen in the browser** plays the same sound from the computer, so the
  page works as a lesson on its own, with no card.

To use it with the card, plug the computer into the Computer's USB socket with
a data cable, power-cycle the module, and press **Connect card & 8mu**. The
page reads the partials from the card, sends each edit as it's made, and
follows the module's knobs and switch. An 8mu plugged into the computer drives
the page, and through it the card.

The card and page talk SysEx; the protocol is documented in
[`sysex.h`](sysex.h).

## Building

```
mkdir build && cd build
cmake .. && make
```

Needs the Pico SDK. `EightMU.h` and `ComputerCard.h` are copied from
`Demonstrations+HelloWorlds/PicoSDK/ComputerCard`.

## Not yet

- Edits aren't saved on the card, so they're lost at power-off. The web app
  remembers the last settings in the browser; **Send to card** sends them.
