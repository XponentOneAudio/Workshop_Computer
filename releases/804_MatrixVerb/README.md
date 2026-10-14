# MatrixVerb

A reverb laboratory for the Music Thing Workshop Computer, played from a
[Music Thing 8mu](https://www.musicthing.co.uk/8mu_page/). It's a **feedback
delay network** (FDN): eight delay lines whose outputs are mixed back into
their inputs by an **8×8 matrix**, and every one of the matrix's 64 values is
on the 8mu, a row per page, for direct control.

It's for exploring: how the matrix, the delay lengths and the usual reverb
controls shape the sound a network makes. It isn't meant to be an everyday
reverb, and plenty of settings sound strange. But it's built so that **no
setting can run away or blast**: see [Safety](#safety).

Built on the same structure as [CVSeq](../802_CVSeq),
[WaveSeq](../801_WaveSeq) and EightTrack: the same 8mu paging and fader
pickup, the same USB host/device split, card presets in flash and the same
style of web editor.

## How it works

Every sample, each line's output goes through its **decay** gain and
**damping** filter and the loop's **low cut**; the **matrix** mixes the eight
back into the eight lines, along with each line's share of the **input**.
Each line's **output** share is heard, at its **pan**.

```
 in ─ tone ─ predelay ─ diffusers ─┬─ input ─► (+) ─► line 1..8 ─┬─► output, pan ─► wet
                                   │           ▲                  │
                                   │           └── matrix ◄── decay, damping, low cut
```

- **The matrix.** Row *r* says how much line *r* takes from each line. A
  matrix can amplify, and in a loop that runs away, so before it's used the
  card scales it by its **spectral norm** σ (its largest singular value, the
  most it can amplify anything), worked out exactly every time it changes.
  With **Normalise** up it's scaled to σ = 1, losing nothing, so the decay is
  all down to the Decay fader; down, a matrix is only ever scaled *down*, so
  a weak one (mostly zeros, say) dies away on its own. An **orthogonal**
  matrix (all its singular values 1) mixes without losing anything: that's
  the smooth reverb. Uneven singular values make some patterns of sound die
  faster than others.
- **Coprime lengths.** If two lines' lengths share a factor, their echoes
  pile up on the same moments and ring. So the lengths are always pairwise
  coprime: line 1 takes its length, then each later line is snapped to the
  nearest length (above before below) that shares no factor with the lines
  before it. A line usually moves by a sample or two at most.
- **Decay and damping** (Jot's method). Each line's gain is set from its
  length, so every line dies at the same rate whatever its length, and its
  one-pole damping filter makes the highs die faster than the lows by the
  Damping fader's ratio.

## The 8mu

The eight faders are the eight lines (on a matrix page, the eight lines that
row takes from). The buttons act **when you let go**:

| Button | Pages (press again for the next) |
|---|---|
| A | **Row 1** → Row 2 → … → **Row 8** of the matrix, then back to row 1 |
| B | **Length**: each line's delay |
| C | **Reverb** → **Lab** |
| D | **Input** → **Output** → **Pan** |

On the matrix pages the **Computer's top-left LED flashes the row's number**:
once for row 1, six times for row 6, then a pause, over and over. Coming back
to A from another page returns to the row you were last on.

| Gesture | What it does |
|---|---|
| Short press | The page |
| Long press A (½ s) | The previous row |
| Long press C | Clear: empty the network |
| Long press D | Freeze on or off |
| Hold A and move a fader | Load card preset 1-8 (the fader's number). Its LED flashes three times |
| Hold B and move a fader | Load matrix template 1-8 (below). Its LED flashes twice |
| Hold C and move a fader | A click into that line, to hear how the matrix spreads it |
| Hold D and move a fader | Save the settings as card preset 1-8. Its LED blinks fast |

Faders **pick up**: after a page change, a fader does nothing until you move
it to (or past) the stored value, so nothing jumps. The 8mu's LEDs show the
stored values; on the two-sided pages (matrix, input, output, pan) they show
how far each is from the centre.

| Tilt | Does |
|---|---|
| Back up / front up | Decay ×4 (back lifted) to ×¼ (front lifted), on top of Main |
| Left / right | Darker (tipped left) or brighter (tipped right), on top of Y |

Both ignore about 15° either side of flat.

### Matrix pages (A)

Each fader is −1 (down) to +1 (up), with the middle three positions all
exactly 0. Fader *c* on row *r* is how much line *r* takes from line *c*;
fader *r* on row *r* is the line feeding itself.

### Templates (hold B and move a fader)

| Fader | Template | Sound |
|---|---|---|
| 1 | Hadamard | Every line feeds every other equally, with signs: the densest mixing. Orthogonal |
| 2 | Householder | Every line feeds every other, mostly itself. Orthogonal |
| 3 | Identity | Each line feeds only itself: eight separate combs, metallic and pitched |
| 4 | Loop | Each line feeds the next, round all eight: one long loop |
| 5 | Pairs | Four separate pairs, each a 45° rotation: four small reverbs |
| 6 | Random | A random orthogonal matrix |
| 7 | Negative | Each line feeds itself inverted: hollow, odd harmonics only |
| 8 | Zero | No feedback: each line heard once, eight echoes |

The faders are 7-bit, so a template is as near as they go: Hadamard comes out
with singular values 0.99 to 1.01.

### Length page (B)

Each line from 1 ms (down) to 340 ms (up), evenly spaced in log, times the X
knob's size, then snapped coprime.

### Reverb page (C)

| Fader | Setting |
|---|---|
| 1 | **Predelay**, 0 to 170 ms |
| 2 | **Decay** (RT60, the time to fall by 60 dB), 0.1 s to 30 s; the top two positions are infinite |
| 3 | **Damping**: how much faster the highs die than the lows, none (down) to 32× faster |
| 4 | **Low cut** in the loop, 10 Hz to 1 kHz |
| 5 | **Tone**: a low-pass on the way in, 200 Hz to 12.8 kHz, off at the top |
| 6 | **Mod depth**: a slow wobble of every line's length, up to ±1.5 ms, which smooths ringing |
| 7 | **Width**: every line's pan, from all centred (down) to as set (up) |
| 8 | **Mix**: dry only (down), both full (centre), wet only (up) |

### Lab page (C, pressed twice)

| Fader | Setting |
|---|---|
| 1 | **Normalise**: how the matrix is scaled. Down: only ever scaled down. Up: scaled to lose nothing |
| 2 | **Blend**: from the identity (down, eight separate combs) to the matrix (up) |
| 3 | **Drive**: saturation in the loop, clean to ×8 |
| 4 | **Diffuse**: four allpass filters on the way in, off to a coefficient of 0.7 |
| 5 | **Mod rate**, 0.05 to 5 Hz (each line at its own rate) |
| 6 | **Input level** into the network, off (down), 0 dB (95) to +12 dB |
| 7 | **Output level** of the wet signal, the same |
| 8 | **Ceiling**: the wet signal's limiter, −30 to −1 dBFS (default −3 dBFS) |

### Input, Output and Pan pages (D)

**Input**: each line's share of the input, −1 to +1. **Output**: how much of
each line is heard, −1 to +1. A line at zero output still runs in the
network. **Pan**: each line's place, left (down) to right (up). A line takes
its input from its place and plays back there. Signs matter: with a dense
matrix, a mix of signs on the input spreads it through the network faster.

## Panel

| Control | Function |
|---|---|
| Main | **Decay** multiplier, ×¼ (fully down) to ×4 (fully up), ×1 at the centre |
| X | **Size**: every delay length ×¼ to ×4, ×1 at the centre. Lengths are snapped coprime again, and the lines glide to them, bending the pitch |
| Y | **Damping**: darker (down) or brighter (up) than the Damping fader |
| Switch up | **Freeze**: the input is cut, and the network holds what's in it |
| Switch middle | Run |
| Switch down | **Clear** (momentary): the wet signal is muted and the network emptied while it's held, then fades back in |

| Jack | Function |
|---|---|
| Audio In 1, 2 | Left and right in. With only Audio In 1 patched, it feeds both sides |
| CV In 1 | Decay, 1 V/oct (added to Main) |
| CV In 2 | Size, 1 V/oct (added to X) |
| Pulse In 1 | A click into the network, through the input vector |
| Pulse In 2 | Freeze, while high |
| Audio Out 1, 2 | Left and right out |
| CV Out 1 | Envelope of the wet signal, 0 to 5 V |
| CV Out 2 | Safety: how far the limiter and guard are turning the wet signal down, 0 V (not at all) to 5 V |
| Pulse Out 1 | High while the limiter or guard is acting (more than 1 dB) |
| Pulse Out 2 | High while frozen |

| LED | Shows |
|---|---|
| 0-3 | With an 8mu or the editor: the page's button. A flashes the row number; C and D blink for their second and third pages. Without one: the wet level, the network's level, frozen, and safety acting |
| 4 | Input level; fully lit while frozen |
| 5 | 8mu or editor connected; blinks when a preset is saved or loaded, flickers while the limiter or guard is acting |

## Safety

Exploring a feedback network means finding settings that misbehave, so the
card is built in layers:

1. **The matrix is normalised** so it can never gain energy (above). Changes
   to it crossfade over 30 ms as a blend of two normalised matrices, which
   can't gain either.
2. **Every line is written through a soft saturation**, so nothing in the
   network can grow past full scale, whatever the settings.
3. **A guard** watches the network: if it sits near full scale for a quarter
   of a second, the guard pulls every line's feedback down (about 50 dB a
   second) until it falls, then lets go over a second or so.
4. **The wet signal** has its DC removed and passes a **look-ahead peak
   limiter**: it sees peaks 1.3 ms ahead and turns the gain down smoothly
   before they arrive, holds it for 30 ms, then lets it back over about
   170 ms, so it never passes the **Ceiling** (−3 dBFS unless you change it)
   and never clips the waveform.
5. **Power-up, Clear, and loading a preset** mute the wet signal and empty
   the network, then fade back in, so a preset that sounds nothing like the
   last can't arrive with a bang.
6. **The output is soft clipped** above three quarters of full scale, dry
   and wet together.
7. **Switch down is a panic button**: it mutes and empties the network for as
   long as it's held.

`test/host_test.cpp` checks this on a computer: 60 runs of random matrices,
random lengths, long and infinite decays, full drive and +12 dB in and out,
fed loud noise, never get the wet signal past the ceiling or the network past
full scale. It also checks that the lengths are always coprime, that the
normalised matrix never amplifies, and that the decay time comes out as set.
Even so, start with your monitors down.

## Presets

**On the card:** eight presets in the program card's flash (its last 4 kB
sector, so a 2 MB card is enough). Save and load them with the 8mu alone
(hold D or A and move a fader), or from the web editor. At power-up the card
loads whichever preset you last saved or loaded.

**In the browser:** the web editor keeps as many presets as you like, has a
dozen built in, exports and imports them as JSON, copies any of them into a
card slot, and **backs up** the card's eight presets (then Export them to a
file).

## Web editor

Open [`web/index.html`](web/index.html) in Chrome or Edge, plug a computer
into the Computer's USB socket, power-cycle the module, then press **Connect
card & 8mu**. It's a single file and needs no network.

- **Network**: decay, size, damping, the matrix's σ and scale, the safety
  state (limiter and guard), levels and CPU load, live from the card.
- **Matrix**: the 64 values as a grid, teal positive and orange negative.
  Click a row to put it on the faders, drag a cell up or down to change it,
  double-click to zero it. Beside it, an **analysis**: the singular values,
  before (dashed) and after normalising, and the eigenvalues on the unit
  circle (on it, lossless; inside, dying). Template buttons, and tools:
  **Orthogonalise** (the nearest lossless matrix), **Random orthogonal**,
  **Random signs**, **Transpose**, **Negate**, **Rotate lines**, with Undo.
- **Faders**: the fourteen pages, as on the 8mu.
- **Delay lines**: each line's length, in ms and samples, with its prime
  factors, so you can see they share none.
- **Impulse response (simulated)**: the network simulated in the page (with
  the card's knobs, when it's connected), drawn as an echogram of the first
  300 ms and a decay curve against the set decay. **Hear the impulse** and
  **Hear a burst** play it. The simulation leaves out the modulation, drive
  and limiter, so the card sounds a little different, but the shape is the
  same. It works without a card too.
- **Card presets** and **browser presets**.
- **8mu on the computer**: its faders, buttons and tilt drive the page, with
  the same pickup and gestures as the card. With another Workshop editor open
  too, one 8mu drives whichever you last clicked in.

The card chooses its USB mode once, at power-up: if the port is supplying
power (an 8mu, or nothing yet), it's a USB host for the 8mu; if a computer is
plugged in, it's a USB MIDI device called **MatrixVerb** for the editor. The
card runs on its own either way. The protocol is documented in
[`sysex.h`](sysex.h).

## Tips

- **Hear the matrix.** Set Mix to wet, Decay long, Diffuse and Mod depth to
  zero, then hold C and move a fader to click into one line. With Identity
  you hear one comb; with Loop the click walks round all eight lines; with
  Hadamard it's smeared into a wash at once.
- **Blend** from 0 to the top with a Hadamard matrix: metallic combs melt into
  a smooth tail.
- **Normalise down** with a sparse matrix (a few values, the rest zero) and an
  infinite Decay: the matrix alone decides how long it rings.
- **Freeze**, then turn X: the frozen sound slides in pitch as the lines
  change length.
- **Drive** with a long decay: the loop saturates and thickens; the guard
  keeps it in check.

## How it's made

- The network runs at 24 kHz, half the sample rate, so that the eight lines
  (16-bit, 8192 samples each, 128 kB) fit the RP2040's RAM, up to 340 ms a
  line. Lines glide when their length changes, and read between samples.
  Going between the two rates, a 47-tap half-band filter each way keeps the
  input from aliasing into the network and the output from leaving an image
  near the top of the audio range: flat to 10 kHz, at least 55 dB down from
  14 kHz.
- All the audio is fixed-point, on core0 ([`fdn.h`](fdn.h)). The design maths
  (the matrix's normalisation by Jacobi rotations, the coprime snapping, the
  decay and damping coefficients) is floating point and runs on core1, next
  to the USB ([`design.h`](design.h)); core0 only ever takes finished, safe
  designs from it.
- `design.h` and `fdn.h` don't touch the hardware, so the tests build on a
  computer:

  ```
  cd test && g++ -O2 -std=c++17 -I.. host_test.cpp -o host_test && ./host_test
  ```

## Building

```
mkdir build && cd build
cmake .. && make
```

Needs the Pico SDK (built with 2.1.1). `EightMU.h` and `ComputerCard.h` are
copied from `Demonstrations+HelloWorlds/PicoSDK/ComputerCard`. The binary
runs from RAM (`copy_to_ram`), so the USB core can write presets to flash
while the audio carries on.

## Not yet

- Tested in simulation and on the host, not yet on hardware: watch the CPU
  readout in the web editor, and please report anything odd.
