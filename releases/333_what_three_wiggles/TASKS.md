# what.three.wiggles: design notes and task breakdown

This file breaks the card into tasks small enough to build and test one at a time. Section 1
covers the architecture every task shares. Section 2 proposes a panel layout. Section 3 lists the
decisions still open. Section 4 lists the tasks, grouped into phases, and Section 5 suggests an
order to build them in.

---

## 1. Architecture

```
 ┌──────────────── Webapp (browser) ────────────────┐        ┌──────────── Card (RP2040) ─────────────┐
 │ location ─► what3words API ─┐                    │        │ core1: USB MIDI ─► SysEx parser        │
 │ typed "word.word.word" ─────┴► normalise ─► hash │ SysEx  │           │                            │
 │                     three 32-bit seeds ─────────────────────────────► seed store ─► flash sector  │
 │ preview plots (the same generators, in JS) ◄─────┘        │           ▼                            │
 └──────────────────────────────────────────────────┘        │ core0: ProcessSample() @ 48 kHz        │
                                                             │   FBFM · additive · walk · LFO · gates │
                                                             └────────────────────────────────────────┘
```

Rules that keep the output **deterministic and repeatable**:

1. **One seeding spec, two implementations.** The webapp (JS) and the firmware (C++) must turn the
   same words into the same bits. Write the spec once (T1.1), then build both sides against the
   same test vectors.
2. **Each output gets its own random stream.** Derive every stream as
   `stream = mix(seed_word_n, STREAM_ID)`, never by drawing from one shared generator. Otherwise
   adding a feature, or reordering code, changes every other output for the same location.
3. **No entropy from the outside world.** No ADC noise, timers or uninitialised memory feed the
   random processes. The seeds are the only source of randomness.
4. **Reset replays the sequence.** A reset input re-initialises every stream, so the same patch
   after a reset produces the same phrase again.

The card already has the building blocks:

- `Demonstrations+HelloWorlds/PicoSDK/ComputerCard/examples/web_interface` provides two-way
  WebMIDI SysEx between a web page and the card, with USB on core1.
- `releases/26_clockwork` writes settings to flash from a SysEx-driven web UI.

At 144 MHz and 48 kHz the audio callback has about **3,000 cycles per sample**. Two audio engines
plus four control engines fit in that, but only with fixed-point maths and lookup tables. Measure
early (T4.1).

---

## 2. Proposed panel layout (draft; see the open questions)

| Jack / control | Proposed role |
|---|---|
| **Audio Out 1** | 3-operator feedback FM loop (ring mod as an alternative mode) |
| **Audio Out 2** | Additive oscillator |
| **CV Out 1** | Random-walk V/oct, slewed (calibrated `CVOut1Millivolts`) |
| **CV Out 2** | Complex LFO, bipolar |
| **Pulse Out 1** | Random triggers |
| **Pulse Out 2** | Flip-flop gates |
| **CV In 1** | V/oct for Audio 2, as in the spec |
| **CV In 2** | Sample-and-hold trigger for Audio 2's pitch, as in the spec. Rising edge with hysteresis at about 1 V |
| **Audio In 1** | Global FM depth CV for Audio 1. Both CV inputs are taken by the spec, so this has to be an audio input (check that it works as a DC input) |
| **Audio In 2** | Free. Candidates: V/oct for Audio 1, or FM input |
| **Pulse In 1** | Clock for CV 1, Pulse 1 and Pulse 2. Free-runs from Knob Y when unpatched (`Connected()`) |
| **Pulse In 2** | Reset: restarts every random stream from its seed |
| **Main knob** | Global FM depth (Audio 1) |
| **Knob X** | Quantiser scale (Audio 2) |
| **Knob Y** | Master rate / tempo for CV 1, CV 2 and the pulses |
| **Switch Z** | Up = FBFM, middle = ring mod (if wanted). Down (momentary) = manual reset, or a knob-page shift |
| **LEDs** | Three show seed/word activity; the rest follow Pulse 1, Pulse 2 and CV 2 |

---

## 3. Open questions to decide before or during Phase 0

1. **CV In 2 or Pulse In 2 for the sample-and-hold trigger?** The spec says CV In 2. Pulse In 2
   has a cleaner trigger path, but then it can't also be the reset.
2. **How is Audio 1 pitched?** Only by the seeds (a drone), or also tracking a V/oct input (Audio
   In 2, or shared with CV In 1)?
3. **FBFM or ring mod?** Build both and pick one with the switch? Ring mod is cheap to add once
   three oscillators exist.
4. **What clocks the random processes?** An external clock on Pulse In 1, an internal rate on
   Knob Y, or both?
5. **What toggles the Pulse 2 flip-flop?** The Pulse 1 triggers (÷2 of the random stream), its
   own seeded stream, or Pulse In 1?
6. **Does the CV 1 walk share Audio 2's quantiser scale** (Knob X), or have a fixed or seeded
   scale?
7. **Which word drives what?** Proposal: word *n* drives operator *n* in the FBFM loop and
   partial group *n* of the additive bank. The CV and pulse outputs use hashes of all three
   words, so each word changes everything a little.
8. **what3words API key.** Looking up words from a location needs an API key, and a key inside
   a static page is public. Options:
   (a) users paste their own key, stored in `localStorage`;
   (b) a small proxy that holds the key;
   (c) typed words only in v1, with location lookup added later.
   Also check the what3words API terms for this use, and the naming.
9. **Language and characters.** what3words has non-English word lists. Should the seed come
   from UTF-8 bytes after Unicode normalisation (NFC, lowercase)? Proposal: yes. The firmware
   only ever receives seeds, so the card never has to handle the words themselves.

---

## 4. Tasks

Each task lists what it depends on and what counts as done. Tasks inside a phase can mostly run
in parallel.

### Phase 0: Groundwork

- **T0.1 Card folder and build.** Set up a CMake project from the ComputerCard template with
  TinyUSB MIDI (copy `tusb_config.h`, `usb_descriptors.c` and the core1 USB loop from the
  `web_interface` example). Add a stub `info.yaml`, and confirm the number 333 is free.
  *Done when:* the build produces a UF2 that blinks an LED and shows up as a USB MIDI device.
- **T0.2 Resolve the open questions** in Section 3 and update the table in Section 2.
- **T0.3 Fixed-point toolkit** (`dsp.h`):
  - sine lookup table with interpolation
  - 32-bit phase accumulators
  - V/oct (mV) to phase increment, using an exp2 lookup
  - one-pole smoother for knobs and CV
  - slew limiter
  - soft clipper

  *Done when:* a host-side test (plain `g++`) checks tuning to within ±1 cent over 8 octaves.

### Phase 1: Seed pipeline (shared by everything)

- **T1.1 Seeding spec** (`SEEDING.md`):
  - normalisation: trim, strip `///`, NFC, lowercase, split on `.`
  - hash: FNV-1a 32 or murmur3 over UTF-8 bytes, one seed per word
  - PRNG: splitmix32 or xorshift32
  - stream derivation: `mix(seed_n, STREAM_ID)`, plus the table of stream IDs
  - float and integer ranges: how to get a uniform integer in `[0, n)` and a uniform value in
    `[0, 1)`

  Include about 10 test vectors (words → seeds → the first 8 outputs of each stream).
  *Depends on:* T0.2 (Q9).
- **T1.2 C++ implementation** (`seed.h`), plus a host-side test against the T1.1 vectors.
- **T1.3 JS implementation** (`web/seed.js`), plus a browser or node test against the same
  vectors. *Done when:* T1.2 and T1.3 both pass the vectors.
- **T1.4 SysEx protocol:**
  - `SET_SEEDS`: three 32-bit seeds packed into 7-bit bytes, plus a CRC. Optionally the words as
    7-bit-safe text, so the card can report back which location it holds.
  - `GET_STATE`
  - `STATE` / `ACK`
  - a firmware version byte

  Use a manufacturer ID for non-commercial use (`0x7D`), like the example does.
- **T1.5 Flash persistence.** Store seeds (and words) in the last flash sector with a magic
  number and CRC, and load factory-default words if the sector is blank or corrupt. Keep flash
  writes safe while audio runs. `26_clockwork` builds a `copy_to_ram` binary, so core0 never
  touches flash, and disables interrupts on the writing core. The other way is
  `multicore_lockout`. *Done when:* the seeds survive a power cycle and no write crashes the card.
- **T1.6 Applying new seeds without clicks.** Re-seed at a safe point: ramp Audio 1 and 2 down
  and back up over a few ms, and restart the control engines on their next step.
  *Depends on:* T1.2, T1.4.

### Phase 2: Webapp (`web/index.html`, one static file like other cards)

WebMIDI and geolocation both need HTTPS, and WebMIDI works in Chromium browsers but not Safari.

- **T2.1 Skeleton.** Page layout, WebMIDI connect, detecting the card, and a status line
  (reuse `interface.html` from the example).
- **T2.2 Manual entry.** A `word.word.word` box with normalisation and validation, and a
  shareable URL (`?w=index.home.raft`). *Depends on:* T1.3.
- **T2.3 Location lookup.** `navigator.geolocation`, then the what3words
  `convert-to-3wa` endpoint, then fill in the words. Handle the API key as decided in Q8.
  *Depends on:* T0.2.
- **T2.4 Send to card.** Send `SET_SEEDS`, wait for `ACK` / `STATE`, and show which words are on
  the card now. *Depends on:* T1.4, T1.5.
- **T2.5 Preview (stretch).** Draw the CV 1 walk, CV 2 LFO and the pulse patterns by running
  JS ports of the generators. This also checks that the JS and firmware match: hold a scope up
  next to the plot. *Depends on:* Phase 3 engine specs.
- **T2.6 Audio preview (stretch).** WebAudio versions of the two oscillators.

### Phase 3: Firmware engines

Each engine lives in its own header, has its own random streams, and exposes `Seed()`, `Reset()`
and `Process()`. Build them simplest first. Each can be tested alone with hard-coded seeds
before the webapp exists.

- **T3.1 Pulse 1: random triggers.** On each clock tick (from Pulse In 1 or the internal
  clock), fire with a probability taken from the seed. Knob control of density is optional.
  Fixed 5–10 ms trigger length. Optionally a seeded 8/16-step pattern that loops, which makes
  the result more recognisable than a fresh coin toss every tick.
- **T3.2 Pulse 2: flip-flop gates.** A toggle flip-flop driven by the source chosen in Q5,
  optionally with a seeded ÷N or a probability of skipping a toggle.
- **T3.3 CV 1: meandering V/oct.**
  - Pick the next note as a seeded random walk in scale degrees: step size taken from a seeded
    distribution, bounded range, reflecting off the ends.
  - Hold each note for a seeded random time.
  - Glide to the next note over a seeded random slew time.
  - Output through `CVOut1Millivolts` for calibrated 1 V/oct.
  - Quantise to the scale chosen in Q6.
- **T3.4 CV 2: complex LFO.** Proposal:
  - sum of three sines, one per word, with seeded rate ratios, phases and amplitudes
  - plus seeded phase distortion or a wavefolder amount
  - Knob Y scales the overall rate
  - normalise to the full bipolar range

  Add optional slow seeded drift of the ratios so the shape never repeats exactly within a
  phrase. It still restarts identically after reset.
- **T3.5 Audio 2: additive oscillator.**
  - A bank of N partials (start with 16, raise it if cycles allow), with seeded levels and
    phases grouped by word (see Q7).
  - Drop partials above Nyquist, so no aliasing.
  - Normalise the sum of the levels.
  - Pitch: CV In 1 is read and held on a CV In 2 edge (with hysteresis), then quantised with the
    scale from Knob X. Needs a scale table and a hysteresis quantiser to stop chatter.
- **T3.6 Audio 1: 3-operator FBFM loop.**
  - Operators 1→2→3→1 with a one-sample feedback delay.
  - Per-operator seeded frequency ratio (choose from a musical ratio table, or allow free
    ratios), phase and FM depth.
  - Global depth = Main knob + Audio In 1, smoothed.
  - Keep it stable: soft-clip the feedback path, and use a DC blocker on the output.
  - Ring-mod mode if chosen in Q3.
  - Base pitch as decided in Q2.
- **T3.7 LEDs and switch.** LED mapping, switch modes, and a manual reset.
- **T3.8 Reset and repeatability.** Pulse In 2 (and switch-down, if chosen) calls `Reset()` on
  every engine. *Done when:* two scope or audio recordings of the outputs after reset, with the
  same seeds, line up sample for sample for the control outputs.

### Phase 4: Integration and performance

- **T4.1 Cycle budget.** Instrument `ProcessSample()` (toggle a GPIO or read the cycle counter),
  measure the worst case with every engine running, and keep a safety margin of at least 20%. If
  it's over:
  - run CV 1, CV 2 and the pulses at control rate (every 8–16 samples)
  - reduce the number of partials
  - move the additive bank's sine lookups into a precomputed wavetable that is rebuilt when the
    seeds change
- **T4.2 Calibration checks.** Confirm V/oct tracking on CV In 1 → Audio 2 and on CV Out 1 using
  a tuner.
- **T4.3 Soak test.** Run for an hour. Check that re-seeding repeatedly from the webapp causes no
  lockups, flash wear stays reasonable (only write when the seeds actually change), and nothing
  goes wrong when USB is unplugged mid-transfer.

### Phase 5: Documentation and release

- **T5.1** Full `info.yaml` with the panel map (copy the layout of `releases/60_markov/info.yaml`)
  and run `npm run validate-info`.
- **T5.2** README with usage, the webapp link, and the what3words disclaimer and API note.
- **T5.3** Leaflet or overlay, and a demo video: "same place, same patch".
- **T5.4** Host the webapp (GitHub Pages or similar, needs HTTPS), and add the UF2 to `UF2/`.

---

## 5. Suggested order and milestones

| Milestone | Tasks | Result |
|---|---|---|
| **M1: Seeds make wiggles** | T0.1, T0.3, T1.1, T1.2, T3.1, T3.2, T3.3 | Hard-coded words drive CV 1 and both pulse outputs; reset replays the sequence |
| **M2: Modulation and first voice** | T3.4, T3.5 | Complex LFO and the additive osc with S&H and quantiser |
| **M3: FM voice** | T3.6, T4.1 | Everything runs together within the cycle budget |
| **M4: Talk to the card** | T1.3–T1.6, T2.1, T2.2, T2.4 | Type three words in a browser and hear the card change; survives power-off |
| **M5: Where am I?** | T2.3, T2.5 | Location lookup and preview plots |
| **M6: Release** | T3.7, T3.8, T4.2, T4.3, Phase 5 | Documented, validated, released |

The webapp (Phase 2, apart from T2.4) can be built alongside M1–M3 once T1.1 is fixed.
