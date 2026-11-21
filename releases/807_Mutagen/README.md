# Mutagen

An oscillator for the Workshop Computer that plays with **pulse width and
sync**, inspired by the "mutants" of a well-known digital polysynth. Its wave
goes through two **mutants** in series. Each one bends the wave's *timing*
(its phase) rather than filtering it, so the same handful of controls gives
PWM on any wave, hard sync, formants, sub-harmonics and drawn phase
modulation.

Built from the same template as WaveSeq, CVSeq and Weights: the same 8mu
handling and pickup, USB modes, knob pickup, web editor, 8mu sharing between
editor windows, and presets. It works with an 8mu alone, or with just the
knobs.

## The mutants

| Mode | What it does | Depth |
|---|---|---|
| **PULSE** | The first half of the wave (a square's high half) is squeezed into width *w*, the rest stretched: pulse width for any wave | *w* from 50% (no change) to 1.5% |
| **SQUEEZE** | The wave starts slowly and speeds up through the cycle | from no change to the phase's 8th power |
| **MIRROR** | Forwards to a turning point, then backwards | the turning point, from the end of the cycle (no change) to 2% |
| **WARP** | The phase pushed back and forth by a curve drawn with the eight faders of the WARP page: a drawn FM | how far, up to half a cycle |
| **PHASEDIFF** | The wave minus a copy shifted along. Two saws make a pulse, and this is the cleanest PWM of all (it is band-limited exactly) | the shift, from half a cycle to 1.5% |
| **SYNC** | Hard sync: a second copy restarts with the first | the copy's speed, 1× to 16× |

Every mutant also has:

- **Ratio**: how many times its process happens in one cycle, ¼ to 16,
  exponential, settling on whole numbers and ¼, ⅓, ½, ⅔, ¾ and 1½ when near
  them. Whole numbers keep the pitch. Below 1, the process stretches over
  several cycles and adds sub-harmonics; in between, it adds sync-like edges.
- **Window**: a raised-cosine fade of the mutated wave at each restart. On
  SYNC it turns the hard edge into a formant.
- **Feedback**: the mutant's output pushes its own phase, up to 150%.
- **Mix**: dry to mutated. At zero the mutant is off (and costs nothing).

Depth 0 leaves the wave alone in every mode but PHASEDIFF (which is then a
50% pulse from a saw, or the odd harmonics of other waves). Mutant 2 bends
the output of mutant 1.

## The 8mu

The buttons choose the page; press the same button again for its second.

| Button | First page | Second page |
|---|---|---|
| A | **MUTANT 1**: mode, ratio, depth, window, feedback, mix, LFO 1 → depth, LFO 2 → depth | **WARP 1**: eight points of mutant 1's curve |
| B | **MUTANT 2** (the same) | **WARP 2** |
| C | **OSC**: wave, semitones (±24), fine (±50¢), bit crush, drive, Out 2, glide, level | **MOD**: CV In 2 → depth 1, depth 2, ratio 1, ratio 2; Audio In 2 → depth 1, depth 2; tilt (left-right) → depth 1, tilt (front-back) → depth 2 |
| D | **LFO 1**: rate (tempo sync, or 0.02-40 Hz), shape, → ratio 1, → ratio 2, → wave, → pitch, → mix 1, → mix 2 | **LFO 2** (the same) |

**LFO rate and tempo sync.** The first third of each LFO's RATE fader is
synced to the clock at **Pulse In 1**: twelve choices of how many beats one
cycle lasts, 16, 8, 4, 3, 2, 1½, 1, ⅔, ½, ⅓, ¼ and ⅛ (bottom to top), each
cycle starting on the beat. A clock is followed from its second pulse (a
rising edge counts only after 2 ms low, and at most every 4 ms); with none
for four beats (and at least 2 s), the synced LFOs carry on at 120 BPM.
Pulse In 2 restarts both LFOs and the beat count. Above the first third the
rate is free, 0.02 Hz to 40 Hz.

LFO shapes: sine, triangle, ramp up, ramp down, square, sample & hold,
drift (eased random). The amounts, SEMI, FINE and the warp points are zero
in the middle of the fader, with a small dead zone. LFO → ratio moves up to
two octaves either way, LFO → pitch up to an octave (squared, so fine near
the centre). Tilt counts past about 15° from flat.

**Pickup.** After a page change a fader does nothing until it reaches (or
passes) the value already stored; the same after a knob or the web editor
has changed that value. A fader that chooses from a list (mode, shape,
Out 2) picks up as soon as it is in the same choice.

**LEDs.** Each fader's setting: its level, how far from zero for one that
goes either way, or which choice. A mutant's depth and mix LEDs show them
after modulation, and an LFO's rate LED pulses with the LFO.

## Panel

| Control | Function |
|---|---|
| Switch up | Main = **pitch**, C1 to C7 (C4 at the centre). X = **mutant 1 depth**, Y = **mutant 2 depth** |
| Switch middle | Main = pitch. X = **mutant 1 ratio**, Y = **mutant 2 ratio** |
| Switch down | Hold it: Main = **wave**, X = **mutant 1 mode**, Y = **mutant 2 mode**. The LEDs show the modes (1-6) in binary, mutant 1 down the left column and mutant 2 down the right, lowest bit at the top |

X and Y set the same values as the 8mu's faders. After the switch moves, or
something else changes the value, a knob does nothing until it reaches it
(LED 6 blinks fast meanwhile). A knob that shares its value with a fader
only writes while it is being turned (and for half a second after), so a
knob left alone never takes a value back from the 8mu. At power-up the
knobs take over at once.
Main is the pitch in both up and middle, so it only waits after the switch
has been held down.

| Jack | Function |
|---|---|
| CV In 1 | Pitch, 1 V/oct, added to Main |
| CV In 2 | Modulation, as the MOD page sets; at first mutant 1's depth (+5 V for all of it) |
| Audio In 1 | **Hard sync** from another oscillator, while patched: the wave restarts at each rising zero crossing, placed between samples |
| Audio In 2 | Audio-rate modulation of the depths (MOD faders 5 and 6) |
| Pulse In 1 | **Clock** for the LFOs' tempo sync (see above) |
| Pulse In 2 | Restarts both LFOs |
| Audio Out 1 | The oscillator |
| Audio Out 2 | OSC fader 6: the same, the dry wave, after mutant 1 only, a square an octave down, or two |
| CV Out 1, 2 | LFO 1, 2, ±5 V |
| Pulse Out 1 | LFO 1's square |
| Pulse Out 2 | **Sync out**: the oscillator's square, to sync another oscillator |

**LEDs.** With an 8mu or the editor, LEDs 1-4 (A B / C D) show the page's
button, lit for its first page and blinking for its second; without, the
two depths, sync patched, and a flash at each clock. LEDs 5 and 6 follow the LFOs.

Like an analogue pulse output, a narrow pulse keeps its two levels and sits
mostly at the low one: there is no DC blocker (it would push the pulse's
short peak far past full scale).

## Three ways to use it

| Plugged into the Computer's USB socket | The card is | The faders are |
|---|---|---|
| an 8mu | USB host | the 8mu's, read directly. No computer needed |
| a computer | a USB MIDI device called **Mutagen** | the web editor's, and an 8mu plugged into the *computer* is passed on by the editor |
| nothing | USB host, waiting | (plug an 8mu in any time) |

The card picks its mode once, at power-up, so **after plugging a computer
in, power-cycle the module**. This needs Computer Rev 1.1 hardware; older
boards are always a USB device.

## Web editor

Open [`web/index.html`](web/index.html) in Chrome or Edge. It is one file
and needs no network.

- **Now playing**: pitch, wave, both mutants and the LFOs, live from the
  card after modulation (a dotted outline while a knob waits to pick up),
  and the card's load.
- **Wave**: Out 1 over two cycles (with mutant 1's output and the dry wave
  behind it) and its first 64 harmonics, quarter-harmonics shown too.
- **Faders**: the eight pages as on the 8mu, each fader with what it means
  (a ratio, a width, a time…), the faders the knobs also set shaded, and the
  8mu's LEDs.
- **Listen here**: a copy of the engine running in the browser plays the
  faders on screen, with a pitch slider standing in for Main and the
  computer keyboard (A-K) to play notes. Useful for shaping sounds without
  the module.
- **Presets**: eleven built in, plus your own in the browser; export and
  import as JSON. Loading one sends it to the card.
- **8mu on the computer**: faders, buttons and tilt drive the page with the
  card's pickup, and the page lights its LEDs. With another Workshop editor
  open, the 8mu drives whichever you last clicked in.

The protocol is in [`sysex.h`](sysex.h) (version 1).

## How it's made, and how clean it is

Everything is integer arithmetic (the RP2040 has no floating point). The
oscillator runs at **96 kHz** and a 23-tap half-band filter brings it to
48 kHz. The waves come from band-limited tables, nine per wave, each with
half the harmonics of the last. Each mutant knows how fast it is playing
through the wave (PULSE at 5% width plays the first half ten times as fast),
and the table is picked for the pitch times that speed. PULSE changes speed
only where a square is flat, never on its edges.

Measured on a PC with the test harness: the strongest component below
18 kHz that is not a harmonic, in dB below the strongest harmonic (output
12-bit and dithered, as on the card):

| | 110 Hz | 440 Hz | 1760 Hz | 3520 Hz |
|---|---|---|---|---|
| saw, dry | −69 | −84 | −92 | −94 |
| saw, PULSE 50% | −75 | −85 | −61 | −53 |
| square, PULSE 95% (3% wide) | −76 | −62 | −41 | −25 |
| saw, SQUEEZE 70% | −82 | −84 | −74 | −61 |
| triangle, MIRROR 80% | −93 | −79 | −59 | −44 |
| saw, WARP 70% | −78 | −85 | −61 | −51 |
| saw, PHASEDIFF 60% | −65 | −80 | −88 | −90 |
| saw, PULSE ×3 50% | −77 | −74 | −54 | −44 |
| saw, SYNC 60% | −66 | −54 | −45 | −34 |
| saw, SYNC 60%, full window | −79 | −74 | −63 | −67 |

Sync's restart is not band-limited, so hard sync is the roughest, as on
most digital synths; the window smooths it away. Very narrow pulses at high
pitch are rough too: use PHASEDIFF for the cleanest PWM.

**Processor time.** Every sample must be made in well under the ~20 µs
between samples. Measured by running the compiled firmware's engine in a
Cortex-M0+ instruction emulator (an estimate, not yet confirmed on
hardware): the default patch takes about 7.4 µs a sample (10 µs at worst),
the heaviest possible patch (both mutants part-wet, windowed, with
feedback, a wave blend, drive, Out 2 on mutant 1) about 13.5 µs (16 µs at
worst). So the card guards itself: if a sample ever takes 15 µs it goes
**lean** - Out 1 made once a sample instead of twice (about 9 µs for the
heaviest patch), a little rougher at high pitch - and goes back once the
lean patch takes 9 µs or less for a second. The web editor shows the load
and when it is lean.

## Building

```
mkdir build && cd build
cmake .. && make
```

Needs the Pico SDK (built here with 2.2.0 and the TinyUSB it pins, 0.18.0:
`EightMU.h` needs that version's host driver). `EightMU.h` and
`ComputerCard.h` are copied from `Demonstrations+HelloWorlds/PicoSDK/ComputerCard`.

The engine (`mutagen.h`) and the 8mu handling (`ui.h`) have no hardware in
them. `test/run_tests.sh` builds and runs their tests - pitch, every phase
map against a floating-point model, pulse widths, sync, modulation, a fuzz
of every fader, and the aliasing table above - and a test of the whole card
(`main.cpp`) against stand-ins for the Computer, the 8mu and USB, on a PC
with any C++17 compiler.

## Not yet

- Not yet tried on hardware: the processor time above is an emulated
  estimate, and the load guard is there in case it is out.
- Settings aren't saved on the card, so they're lost at power-off. Keep them
  as presets in the web editor and send them again after power-up.

Written with AI help (Claude Code).
