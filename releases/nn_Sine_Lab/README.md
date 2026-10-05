# Sine Lab

Two ways of building complex sounds from sine waves, for the Workshop
Computer, edited from a [Music Thing 8mu](https://www.musicthing.co.uk/8mu_page/)
over USB MIDI host, with a web app that draws what each is doing and explains
why.

- **Additive.** Eight sine waves, the *partials*, are added together. Each is
  drawn as a circle turning at its own speed, and the circles are nested: each
  one rides on the rim of the one before. The height of the outermost point,
  followed over time, is the waveform. This is
  [additive synthesis](https://en.wikipedia.org/wiki/Additive_synthesis), and
  at whole-number multiples of one frequency, a
  [Fourier series](https://en.wikipedia.org/wiki/Fourier_series).
- **FM.** Two sine waves, where one, the *modulator*, bends the other, the
  *carrier*. Four ways of bending it can be compared at the touch of a button:
  linear, exponential and through-zero
  [frequency modulation](https://en.wikipedia.org/wiki/Frequency_modulation_synthesis),
  and [phase modulation](https://en.wikipedia.org/wiki/Phase_modulation).

The card is mostly for learning, so each mode is deliberately small, and the
web app carries the explanations.

**Swapping mode.** Hold the switch down for a second, or hold the 8mu's
button D for a second. All the Computer's LEDs light briefly. The web app has
a tab for each. Each mode keeps its own settings while the other is in use.

## Three ways to use it

The card works on its own; the 8mu and the web app are both optional.

| Plugged into the Computer's USB socket | The card is | The faders are |
|---|---|---|
| an 8mu | USB host | the 8mu's, read directly. No computer needed |
| a computer | a USB MIDI device called **Sine Lab** | the web app's, and an 8mu plugged into the *computer* is passed on by the web app |
| nothing | USB host, waiting | (plug an 8mu in any time) |

The card picks its USB mode once, at power-up, so **after plugging a computer
in, power-cycle the module**. Telling the two apart needs Computer Rev 1.1
hardware; older boards are always a USB device.

**Pickup.** After a page, mode, shape or example change, the 8mu's faders
don't do anything until they reach the value already stored (or pass it), then
take it over. So nothing jumps. The 8mu's LEDs show the stored values.

## Additive

The card starts in this mode, on Saw.

### The 8mu

The eight faders are the eight partials, lowest on the left. The buttons
choose what the faders change:

| Button | Page | Fader sets | Range |
|--------|------|------------|-------|
| A | LEVEL     | The partial's amplitude, the circle's size | 0 to 1 |
| B | PHASE     | Where the partial starts, where on its circle the point begins | 0° to 360° |
| C | FREQUENCY | How fast it turns | the middle is exactly harmonic; either side up to 12 semitones away |
| D | (shape)   | A tap loads the next built-in shape; a hold swaps to FM | saw, square, triangle, sine, pulse, bell |

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

### The shapes

| Shape | Recipe | Read more |
|-------|--------|-----------|
| Saw | every harmonic, amplitude 1/n | [Sawtooth wave](https://en.wikipedia.org/wiki/Sawtooth_wave) |
| Square | odd harmonics, 1/n | [Square wave](https://en.wikipedia.org/wiki/Square_wave_(waveform)), [Gibbs phenomenon](https://en.wikipedia.org/wiki/Gibbs_phenomenon) |
| Triangle | odd harmonics, 1/n², every other one at 180° | [Triangle wave](https://en.wikipedia.org/wiki/Triangle_wave) |
| Sine | the first partial alone | [Sine wave](https://en.wikipedia.org/wiki/Sine_wave) |
| Pulse | every harmonic at the same level, all at 90° (cosines) | [Pulse wave](https://en.wikipedia.org/wiki/Pulse_wave), [Dirichlet kernel](https://en.wikipedia.org/wiki/Dirichlet_kernel) |
| Bell | inharmonic partials, about 1, 2.3, 4.2, 6.0, 7.3, 8.8, 10.5 and 12.9 times the fundamental | [Inharmonicity](https://en.wikipedia.org/wiki/Inharmonicity), [Strike tone](https://en.wikipedia.org/wiki/Strike_tone) |

### Panel

| Control     | Function |
|-------------|----------|
| Main knob   | Pitch, C1 to C7 |
| X knob      | Partials: how many sound, 1 to 8. Each fades in as the knob turns, so turning it up shows a Fourier series converging |
| Y knob      | Stretch, 0 to 0.5: partial *n* moves from *n* to *n*<sup>1+stretch</sup> times the fundamental, as in [stiff piano strings](https://en.wikipedia.org/wiki/Piano_acoustics#Inharmonicity_and_piano_size) |
| Switch up   | Harmonic lock: every partial exactly harmonic, ignoring the FREQUENCY page and stretch. Flick between up and middle to compare |
| Switch middle | Free: the FREQUENCY page and stretch apply |
| Switch down | Tap to load the next shape; hold to swap to FM |

| Jack        | Function |
|-------------|----------|
| Audio In 1  | Stretch, added to Y |
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

## FM

Two operators: a modulator sine wave and a carrier sine wave. Audio Out 1 is
the carrier, Audio Out 2 the modulator. The mode starts on the Clarinet
example.

### Four types

The 8mu's buttons choose how the modulator *m* (from -1 to 1) bends the
carrier, at depth *D*. A tap down on the switch steps through them too.

| Button | Type | The modulator is added to | Formula |
|--------|------|---------------------------|---------|
| A | Linear FM | the carrier's frequency, in Hz, stopping at 0 Hz | f = f<sub>c</sub> (1 + D·m), never below 0 |
| B | Exponential FM | the carrier's pitch, in octaves, as 1V/oct | f = f<sub>c</sub> · 2<sup>0.4·D·m</sup> |
| C | Through-zero FM | the carrier's frequency, allowed below 0 Hz | f = f<sub>c</sub> (1 + D·m) |
| D | Phase modulation | the carrier's phase, in radians | φ = 2π f<sub>c</sub> t + D·m |

What to listen for:

- **Linear** stays in tune while *D* is below 1. Above that, its frequency
  would need to go below 0 Hz; it can't, so it waits at 0 Hz, the average
  frequency rises and the note goes sharp.
- **Exponential** goes sharp at any depth, because an octave up adds more
  hertz than an octave down takes away. It's ideal for vibrato.
- **Through-zero** runs backwards below 0 Hz, so it stays in tune at any
  depth, and its sidebands follow the
  [Bessel functions](https://en.wikipedia.org/wiki/Bessel_function) exactly.
- **Phase modulation** gives the same sidebands as through-zero with sine
  waves. But its depth *is* the index, so the brightness holds when the
  modulator ratio changes (in FM the depth sets the deviation, so a faster
  modulator means a smaller index). And feedback or an external modulator
  only shifts its phase, never its tuning. This is what Yamaha's DX7 does.

For the three kinds of FM, the depth sets the deviation as a multiple of the
carrier frequency (depth 1 swings it between 0 Hz and twice the carrier). For
phase modulation it's the index in radians. With the modulator at the
carrier's frequency, the two agree.

### The faders

| Fader | Sets | Range |
|-------|------|-------|
| 1 | Carrier ratio | 0.5, 1, 2, 3 … 8 times the pitch |
| 2 | Modulator ratio | 2 Hz and 6 Hz (fixed, for vibrato), then 0.25, 0.5, 0.75, 1, 1.5, 2, 2.5, 3, 3.5, 4, 5 … 12, 14, 16 times the pitch |
| 3 | Modulator fine | up to a semitone either way; the middle three steps are exact |
| 4 | Depth | 0 to 10, squared law |
| 5 | Feedback | the modulator modulating its own phase, 0 to 2 radians |
| 6 | Decay | of the envelope, 20 ms to 8 s |
| 7 | Envelope to depth | added to the depth at the envelope's peak, 0 to 10 |
| 8 | Envelope to level | 0: the level stays put; at the top, the level follows the envelope |

The **envelope** starts at a pulse into Pulse In 1, which also starts both
operators from 0, and decays from there. With the switch up (drone) it's held
at its peak instead, so the sound is steady.

### Examples

Pulse In 2, or the web app, loads these:

| Example | Shows |
|---------|-------|
| Vibrato | A 6 Hz modulator, exponential: slow FM is vibrato. Raise fader 2 to hear it turn into timbre |
| Clarinet | 1:2, so odd harmonics only |
| Electric piano | 1:1, with the envelope adding depth at the strike: bright attack, mellow decay |
| Bell | Modulator at about √2 times the carrier, an irrational ratio, so inharmonic |
| Feedback | The modulator bent towards a sawtooth by its own output |
| Deep 1:1 | Depth 2.5, past where linear FM clips: switch between A, B, C and D |

### Panel

| Control     | Function |
|-------------|----------|
| Main knob   | Pitch, C1 to C7 |
| X knob      | Depth, added to fader 4 |
| Y knob      | Modulator ratio, up to 12 steps up the list from fader 2 |
| Switch up   | Drone: the envelope held at its peak |
| Switch middle | Envelope: Pulse In 1 starts it |
| Switch down | Tap for the next FM type; hold to swap to additive |

| Jack        | Function |
|-------------|----------|
| Audio In 1  | External modulator, added to the internal one |
| CV In 1     | Pitch, 1V/oct |
| CV In 2     | Depth, added to X, 1 per volt |
| Pulse In 1  | Trigger: starts the envelope, and both operators from 0 |
| Pulse In 2  | Next example |
| Audio Out 1 | The carrier, modulated |
| Audio Out 2 | The modulator |
| CV Out 1    | Pitch, 1V/oct, 0V at middle C |
| Pulse Out 1 | Square at the pitch |

**LEDs.** LEDs 1-4 show the FM type (A-D), LED 5 the envelope, and LED 6 is
lit while an 8mu or the web app is connected.

Deep FM spreads sidebands widely, and those above 24 kHz fold back as
[aliasing](https://en.wikipedia.org/wiki/Aliasing): high notes at high depths
get gritty.

## Web app

Open [`web/index.html`](web/index.html) in Chrome or Edge. It's a single file
and needs no network, other than for its links to Wikipedia. Its two tabs
follow the card's mode, and **Listen in the browser** plays either mode from
the computer, so the page works as a lesson on its own, with no card.

**Additive tab**

- **Circles on circles.** The eight partials as nested circles, slowed down
  hundreds of times, with the waveform they trace running off to the right.
  Optionally the trail the outer point draws (what an X-Y scope shows) and the
  shadow on the other axis (Audio Out 2). Pointing at a fader picks out its
  circle and draws its partial on its own.
- **Faders.** Pages A, B and C, like the 8mu's buttons, each fader with its
  own partial drawn above it and its frequency below. Drag, use the arrow
  keys, or double-click to reset.
- **Shapes,** each with a short explanation of its recipe.
- **Spectrum.** Where each partial sits against the harmonic series, and how
  loud.
- **Explanations** of additive synthesis, Fourier series, epicycles, phase,
  inharmonicity, the two outputs and Nyquist, and things to try.

**FM tab**

- **Two operators.** The modulator and carrier as turning circles, slowed
  down, with three traces: the modulator, the carrier's frequency (with 0 Hz
  marked, so linear FM's clipping and through-zero's running backwards both
  show), and the output.
- **Four ways to bend a sine.** The same settings made all four ways side by
  side, each with its waveform, spectrum and how far its average pitch has
  drifted. Click one to choose it.
- **Faders and examples,** each example with an explanation.
- **Spectrum,** measured from the sound, with the Bessel-function prediction
  for each sideband drawn over it.
- **Bessel functions,** J<sub>0</sub> to J<sub>5</sub>, with the current index
  marked.
- **Readouts** of carrier, modulator, C:M ratio, index, deviation, Carson
  bandwidth, and whether the sound is pitched.
- **Explanations** of FM, Chowning and the DX7, index and Bessel functions,
  ratios, linear against exponential, through-zero, phase modulation, feedback
  and aliasing, and things to try.

To use it with the card, plug the computer into the Computer's USB socket with
a data cable, power-cycle the module, and press **Connect card & 8mu**. The
page reads the card's settings, sends each edit as it's made, and follows the
module's knobs, switch and mode. An 8mu plugged into the computer drives the
page, and through it the card, with the same buttons as on the card.

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
