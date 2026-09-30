# what.three.wiggles: design notes and tasks

This file records the decisions behind the card, what's built, and what's next. [README.md](README.md)
describes how the card works as it stands.

---

## 1. Architecture

```
 ┌───────────── web/index.html ─────────────┐          ┌──────────── Card (RP2040) ─────────────┐
 │ typed "word.word.word" ─► normalise      │  SysEx   │ core 1: USB MIDI ─► protocol.h         │
 │   ─► seeds (seed.js) ─► shown on page    │ ───────► │   words ─► seeds (seed.h) ─► Patch     │
 │ "✓ seeds match" ◄── card's own seeds ◄───────────── │   words ─► flash (last sector)         │
 │ CPU readout ◄────────── peak cycles ◄─────────────── │ core 0: ProcessSample() @ 48 kHz       │
 └──────────────────────────────────────────┘          │   FM loop · additive · walk · LFO ·    │
                                                       │   triggers · flip-flop (engines.h)     │
                                                       └────────────────────────────────────────┘
```

What keeps the output **deterministic and repeatable** (details in [SEEDING.md](SEEDING.md)):

1. **One spec, two implementations.** The C++ and JS seeding are both checked against the same test
   vectors. The card also reports its own seeds back to the page, which checks them live.
2. **Each output has its own random stream**, keyed by a stream ID, so changing one engine leaves
   the others alone.
3. **No outside entropy.** The words are the only source of randomness.
4. **Reset replays the sequence.** The walk makes a fixed number of draws per step, and the triggers
   one per tick, so the same clock after a reset gives the same phrase.

The seeds become a `Patch` (every engine's settings) on core 1, away from the audio callback. Core 0
fades out, swaps in the new patch and fades back in. The binary runs from RAM (`copy_to_ram`), so
saving the words to flash doesn't interrupt the audio.

---

## 2. Decisions

| # | Question | Decision |
|---|---|---|
| 1 | Sample and hold trigger | CV In 2, as specified. Pulse In 2 is the reset. |
| 2 | Global FM depth CV | Audio In 1 (DC-coupled, so it handles slow CV). |
| 3 | Audio 1 pitch | Drone, pitch set only by the words. Ranges to be tuned by ear; feeding the CVs back in is an idea for later. |
| 4 | FM or ring mod | Both, chosen with the switch: middle = FM loop, up = ring mod. Middle is FM because the switch returns there after a tap. |
| 5 | Clock | Internal clock, replaced by a clock plugged into Pulse In 1 (jack detection). |
| 6 | Knobs | Tapping the switch down flips between two pages. Soft takeover on every knob. |
| 7 | what3words API | None for now: typed words only. The page links to what3words.com to show where the words are. |
| — | Pulse 2 source | Flip-flop toggled by Pulse 1's triggers (the classic random-gate patch). |
| — | CV 1 scale | Shares the quantiser scale on Knob X with Audio 2. |
| — | Extra switch gesture | Holding the switch down for a second resets, like Pulse In 2. |
| — | Card number | 333. |

---

## 3. Tasks

✅ = done and tested on a desktop · 🔧 = built, needs checking on hardware · ⬜ = to do

### Groundwork
- ✅ T0.1 Card folder, CMake build with TinyUSB MIDI, `info.yaml` (passes `npm run validate-info`)
- ✅ T0.2 Open questions resolved (Section 2)
- ✅ T0.3 Fixed-point toolkit (`dsp.h`): sine table, V/oct to phase increment (0.004 cents worst
  error from −6 to +5 V), scales, quantiser with hysteresis

### Seed pipeline
- ✅ T1.1 Seeding spec ([SEEDING.md](SEEDING.md)) with test vectors
- ✅ T1.2 C++ (`seed.h`), tested against the vectors
- ✅ T1.3 JS (`web/seed.js`), tested against the same vectors
- ✅ T1.4 SysEx protocol (`protocol.h`): HELLO, SET_WORDS, STATE, ERROR, GET_STATS, STATS
- 🔧 T1.5 Save the words to the last flash sector, with a magic number and check value; default
  words `what.three.wiggles`
- 🔧 T1.6 New words fade out, swap and fade in, with no clicks

### Webapp
- ✅ T2.1 Page, WebMIDI connection, finding the card
- ✅ T2.2 Typed words: normalising, validating, a shareable `?w=` link, and a link to the place on
  what3words.com
- ⬜ T2.3 Location lookup (needs a what3words API key; left out for now)
- ✅ T2.4 Send to card and show what the card reports, with the seed check. Browser test against
  the emulated card.
- ⬜ T2.5 Preview plots of the CV and pulse outputs in the page
- ⬜ T2.6 Audio preview in the page

### Firmware engines
- 🔧 T3.1 Pulse 1: random triggers; density from the words, adjusted by a knob
- 🔧 T3.2 Pulse 2: flip-flop
- 🔧 T3.3 CV 1: random walk with random holds, glides and leaps
- 🔧 T3.4 CV 2: three sines, cross-modulated and wavefolded
- 🔧 T3.5 Audio 2: 16-partial additive oscillator, V/oct, sample and hold, quantiser
- 🔧 T3.6 Audio 1: 3-operator feedback FM loop and ring mod
- 🔧 T3.7 LEDs, switch, knob pages with soft takeover
- 🔧 T3.8 Reset and repeatability (tested on a desktop: the same seeds give identical output)

### Integration
- 🔧 T4.1 CPU budget. The card measures its own audio callback, and the page shows the peak as a
  percentage of the 3,000 cycles per sample.
- ⬜ T4.2 Calibration check of V/oct on CV In 1 → Audio 2, and on CV Out 1
- ⬜ T4.3 Soak test: an hour running, repeated re-seeding, unplugging USB mid-send

### Release
- ✅ T5.1 `info.yaml` with the panel map
- ✅ T5.2 README
- ⬜ T5.3 Leaflet or overlay, demo video
- ✅ T5.4 UF2 in `UF2/`. The catalogue site publishes `web/` over HTTPS.

---

## 4. Checking on hardware (next)

Do these in order. Each step depends on the ones before it.

1. **Boots and sounds.** Flash the UF2. Audio 1 and Audio 2 should fade in, and LED 5 should flash
   with the clock.
2. **CPU.** Connect USB and open the page. The CPU readout should stay well under 80%. If it doesn't,
   first reduce `kNumPartials` in `patch.h`, then run the control engines every few samples.
3. **Words.** Send some words. The LEDs should chase round, the sound should change, and the page
   should show "✓ seeds match". Power-cycle: the words should still be there.
4. **Pages and takeover.** Tap the switch down. LED 4 should light, and LED 5 should blink until the
   knobs catch up. Hold the switch down for a second to reset.
5. **Jacks.** Plug a clock into Pulse In 1 (the internal clock should stop), reset on Pulse In 2,
   CV In 1 → Audio 2 pitch, triggers into CV In 2 for sample and hold, CV into Audio In 1 for FM
   depth.
6. **Tuning.** Check CV Out 1 and CV In 1 → Audio 2 with a tuner.

## 5. Ideas for later

- Tune the ranges by ear: drone pitch range, FM ratio table, partial tilt, LFO fold amount. Use
  `test/render.cpp` to compare sets of words quickly.
- Feed CV 1 or CV 2 into the drone (for example, slow FM depth or operator detune).
- Location lookup (T2.3) with the user's own what3words API key.
- Preview in the page (T2.5), by porting `engines.h` to JS and checking it against the card.
