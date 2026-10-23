# Epicycles

Circles turning on circles, for the Workshop Computer, edited from a
[Music Thing 8mu](https://www.musicthing.co.uk/8mu_page/) over USB MIDI host,
with a web app that animates what the card is doing, explains it, and turns
your own drawings and pictures into circles for the card to play.

- **Partials.** Eight sine waves, the *partials*, are added together. Each is
  drawn as a circle turning at its own speed, and the circles are nested: each
  one rides on the rim of the one before. The height of the outermost point,
  followed over time, is the waveform. This is
  [additive synthesis](https://en.wikipedia.org/wiki/Additive_synthesis), and
  at whole-number multiples of one frequency, a
  [Fourier series](https://en.wikipedia.org/wiki/Fourier_series).
- **Drawing.** Up to 64 circles, some turning anticlockwise and some
  clockwise, whose outermost point traces a picture. Its position across comes
  out of Audio Out 2 and its height out of Audio Out 1, so the two into an
  oscilloscope's X-Y mode draw it, and each on its own is a sound whose
  timbre is the shape. The circles are the drawing's
  [complex Fourier series](https://en.wikipedia.org/wiki/Fourier_series#Complex-valued_functions);
  the web app finds them for anything you draw, or for a picture, and sends
  them to the card.

Partials mode was the additive half of the Sine Lab card, which now does FM
only.

**Swapping mode.** Hold the switch down for a second, or hold the 8mu's
button D for a second. All the Computer's LEDs light briefly. The web app has
a tab for each. The card starts in drawing mode, on the heart.

## Three ways to use it

The card works on its own; the 8mu and the web app are both optional.

| Plugged into the Computer's USB socket | The card is | The faders are |
|---|---|---|
| an 8mu | USB host | the 8mu's, read directly. No computer needed |
| a computer | a USB MIDI device called **Epicycles** | the web app's, and an 8mu plugged into the *computer* is passed on by the web app |
| nothing | USB host, waiting | (plug an 8mu in any time) |

The card picks its USB mode once, at power-up, so **after plugging a computer
in, power-cycle the module**. Telling the two apart needs Computer Rev 1.1
hardware; older boards are always a USB device.

Drawings of your own come from the web app, so for those the card needs the
computer. The eight built-in drawings are always there.

**Pickup.** After a page, mode or shape change, the 8mu's faders
don't do anything until they reach the value already stored (or pass it), then
take it over. So nothing jumps. The 8mu's LEDs show the stored values.

## Drawing

### The circles

Each circle turns a whole number of times, *k*, for every time round the
drawing, from −127 to 127: anticlockwise above 0, clockwise below. Add the
circles up as arrows, tip to tail, and the last tip draws the picture:

z(t) = Σ c<sub>k</sub> e<sup>2πikt</sup>, &nbsp; Audio Out 2 = Re z, &nbsp; Audio Out 1 = Im z

To find the circles for a drawing, the web app puts 1024 points evenly along
it and works out each c<sub>k</sub> with a
[discrete Fourier transform](https://en.wikipedia.org/wiki/Discrete_Fourier_transform).
It keeps the 64 biggest, biggest first, and scales them so the drawing reaches
0.95 of full scale from the centre.

On its own, each output is an ordinary repeating wave: a circle turning *k*
times per drawing puts harmonic |*k*| into both. Round, smooth drawings need
few, slow circles and sound mellow; drawings with corners need many, fast
ones, and sound bright.

### The built-in drawings

Switch down (a tap), Pulse In 2 or the 8mu's button D loads the next one.

| Drawing | Circles | Shows |
|---|---|---|
| Circle | 1 | One circle: each output a pure sine |
| Ellipse | 2 | The same speed, opposite ways |
| Figure eight | 4 | A Lissajous figure: a sine across, twice as fast up |
| Trefoil | 2 | Two speeds, opposite ways: three petals |
| Flower | 2 | One forwards, one six times as fast backwards: seven loops |
| Heart | 8 | A smooth curve, drawn exactly by eight circles |
| Square | 64 | Corners need every circle there is, and still overshoot ([Gibbs phenomenon](https://en.wikipedia.org/wiki/Gibbs_phenomenon)) |
| Star | 51 | Five-fold symmetry: only speeds one off a multiple of five |

They're made by the same code the web app uses (see [Building](#building)).

### The 8mu

The eight faders are **bands**: the circles grouped by speed, an octave each.

| Fader | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| Turns per drawing | 1 | 2 | 3–4 | 5–8 | 9–16 | 17–32 | 33–64 | 65–127 |

All at the top, the drawing is as it was made. Pull the top bands down and it
blurs, and the sound dulls: a low-pass filter for a picture. Button D: a tap
loads the next drawing, a hold swaps to partials. Buttons A–C do nothing in
drawing mode.

### Panel

| Control     | Function |
|-------------|----------|
| Main knob   | Pitch: how many times a second the drawing is drawn, C1 to C7 |
| X knob      | Circles: how many turn, 1 to 64, biggest first, each fading in. A squared law, so the first few have room |
| Y knob      | Rotate the drawing, a full turn |
| Switch up   | Slow: seven octaves down (about a turn every four seconds to sixteen a second), so the outputs draw it as CV |
| Switch middle | Audio |
| Switch down | Tap to load the next drawing; hold to swap to partials |

| Jack        | Function |
|-------------|----------|
| Audio In 1  | Rotate, added to Y, at audio rate (±half a turn) |
| CV In 1     | Pitch, 1V/oct |
| CV In 2     | Circles, added to X |
| Pulse In 1  | Sync: back to the start of the drawing |
| Pulse In 2  | Next drawing |
| Audio Out 1 | The height of the drawing's point (Y) |
| Audio Out 2 | Its position across (X) |
| CV Out 1    | Pitch, 1V/oct, 0V at middle C |
| Pulse Out 1 | Square at the pitch, high for the first half of the drawing |

**LEDs.** A tiny scope: each of the six LEDs lights as the drawing's point
passes near it (they sit at its corners and the middle of each side). In slow
mode you can watch the point go round.

Circles faster than 16 kHz fade out by 20 kHz, so high notes don't alias
above the [Nyquist frequency](https://en.wikipedia.org/wiki/Nyquist_frequency):
at C7 only the first seven or so circles are left, and the drawing melts into
its outline.

## Partials

### The 8mu

The eight faders are the eight partials, lowest on the left. The buttons
choose what the faders change:

| Button | Page | Fader sets | Range |
|--------|------|------------|-------|
| A | LEVEL     | The partial's amplitude, the circle's size | 0 to 1 |
| B | PHASE     | Where the partial starts, where on its circle the point begins | 0° to 360° |
| C | FREQUENCY | How fast it turns | the middle is exactly harmonic; either side up to 12 semitones away |
| D | (shape)   | A tap loads the next built-in shape; a hold swaps to drawing | saw, square, triangle, sine, pulse, bell |

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
| Switch down | Tap to load the next shape; hold to swap to drawing |

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
Nyquist frequency.

## Web app

Open [`web/index.html`](web/index.html) in Chrome or Edge. It's a single file
and needs no network, other than for its links to Wikipedia. Its two tabs
follow the card's mode, and **Listen in the browser** plays either mode from
the computer (drawing mode in stereo: left is Audio Out 2, right Audio Out 1,
as an X-Y scope expects), so the page works as a lesson on its own, with no
card.

**Drawing tab**

- **Drawn by circles.** The circles, biggest first, slowed down, with the
  drawing they trace, the drawing as it should be faintly behind, and the two
  outputs running off to the right (Out 1, the height) and downwards (Out 2,
  across). In slow mode it runs at the card's real speed.
- **Bands,** the 8mu's faders in this mode, each with its circles drawn on
  their own above it. Pointing at one picks out its circles.
- **Drawings,** the eight built in, each with a short explanation.
- **Make your own.** *Draw* with the mouse, a pen or a finger; each stroke is
  turned into circles as you lift off and sent to the card. *Picture*:
  choose, drop or paste one. The page finds the edges of its dark parts
  ([Otsu's](https://en.wikipedia.org/wiki/Otsu%27s_method) threshold, then
  [marching squares](https://en.wikipedia.org/wiki/Marching_squares)), joins
  them into one path, and turns that into circles. The threshold, light on
  dark, and how small an outline to leave out can all be changed. Bold,
  simple pictures work best.
- **Spectrum.** Every circle by its speed, clockwise to the left; and below,
  the harmonics each output holds.
- **Explanations** of the complex Fourier series, negative frequencies, the
  discrete Fourier transform, why corners need many circles, what the shape
  does to the sound, scopes, slow mode as CV, bands and Nyquist, and tracing
  pictures, and things to try.

**Partials tab**

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

To use it with the card, plug the computer into the Computer's USB socket with
a data cable, power-cycle the module, and press **Connect card & 8mu**. The
page reads the card's settings (and any drawing sent to it earlier), sends
each edit as it's made, and follows the module's knobs, switch and mode. An
8mu plugged into the computer drives the page, and through it the card, with
the same buttons as on the card.

The card and page talk SysEx; the protocol is documented in
[`sysex.h`](sysex.h).

## Building

```
mkdir build && cd build
cmake .. && make
```

Needs the Pico SDK (built with 2.2.0 and its own TinyUSB, 0.18). `EightMU.h`
and `ComputerCard.h` are copied from
`Demonstrations+HelloWorlds/PicoSDK/ComputerCard`.

The built-in drawings in `drawings.h` are made from the web app: after
changing one there (the `<script id="fourier">` block of `web/index.html`),
run `node make_drawings.js`.

The drawing's 64 circles run per sample on one core, at about 24 cycles each:
a single table holds sine and cosine together so one load gives both, and the
drawing is rotated after the circles are added up, not before.

## Not yet

- Edits and drawings aren't saved on the card, so they're lost at power-off.
  The web app remembers the last settings and drawing in the browser; **Send
  to card** sends them.
