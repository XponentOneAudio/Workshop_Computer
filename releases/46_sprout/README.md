# Sprout

Plant biodata sonification for the Workshop Computer, after the [MIDI Sprout](https://github.com/XponentOneAudio/MIDIsprout).

Sprout plays notes from tiny changes in a plant's electrical resistance. Each note goes out as **pitch CV + gate**, through a **built-in sine voice**, and as **USB MIDI** when the Computer is plugged into a computer. A smoothed **change signal** follows the plant continuously, like the MIDI Sprout's control output.

No extra hardware is needed beyond two electrodes. The Workshop System's own amplifier does the job of the MIDI Sprout's 555 sensor circuit.

**Status:** the measurement appeared to work on hardware with the earlier probe version. Play mode, the voice, USB MIDI and saving settings are untested on hardware so far.

## Patching

```
Audio Out 1 ──► electrode A ── [PLANT] ── electrode B ──► Amplifier in (mic channel)
Audio In 1  ◄──────────────────────────────────────────── Amplifier out
```

**Electrodes:** ECG/TENS pads on the top and underside of a leaf, or crocodile clips on a patch cable with the far plug cut off.

**Amplifier:** use the **mic** channel with low gain. The Minimoog-style channel distorts deliberately, which squashes the changes being measured. If your plant and electrodes have low resistance, electrode B can also go straight into Audio In 1.

## First time: calibrate

1. **Hold the switch down** while powering on (or pressing reset). All LEDs flash 3 times to show calibration mode. Release the switch.
2. **Main** sets the tone level. Turn it up until the left-column meter shows **two LEDs** (LED 4 full, LED 2 partly lit). If **LED 1** (clip) lights, turn Main or the amplifier gain down.
3. **X** sets the tone frequency: 250 Hz, 500 Hz, 1 kHz, 2 kHz or 4 kHz. Pick the one where the meter is high and steady while nobody touches the plant.
4. **Y** sets the zoom of the change signal (CV Out 2 and MIDI CC 80). **Tap down** to re-zero, touch the leaf, and set Y so **LED 3** responds clearly without staying fully lit.
5. **Hold the switch down for 2 seconds.** The LEDs flash 3 times, the settings are saved, and the card starts playing.

The saved settings are used from then on. Calibrate again whenever you move the electrodes or change plants.

Until a card has been calibrated, play mode uses defaults: 1 kHz tone at a quarter level, 16× zoom.

### Calibration mode jacks and LEDs

| | |
|---|---|
| CV Out 1 | Returned tone level |
| CV Out 2 | Change signal, (level − baseline) × zoom |
| Audio Out 2 | Phase of the returned tone |
| Pulse Out 1 | Trigger whenever a note *would* play, using a fixed middle sensitivity |
| Pulse Out 2 | High while clipping |
| LEDs 0, 2, 4 | Level meter (aim for 2 lit) |
| LED 1 / 3 / 5 | Clip / change size / would-play-a-note |

## Play mode

| Control | Function |
|---|---|
| **Main** (+ **CV In 1**) | **Sensitivity.** CCW: only big events play notes. CW: a busy stream of notes. |
| **X** | **Scale:** chromatic, major, minor, dorian, major pentatonic, minor pentatonic, whole tone (7 zones, CCW to CW) |
| **Y** | **Range:** 1 to 5 octaves upwards from C2 |
| **CV In 2** | **Transpose**, about 1 V/oct in semitone steps (±2 octaves). CV inputs aren't calibrated, so expect it to be slightly off. |
| **Switch Up** | Long notes (about 0.15–2.7 s, like the MIDI Sprout) |
| **Switch Middle** | Short notes (about 40–300 ms) |
| **Switch Down** (tap) | Re-zero the change signal |

| Jack | Output |
|---|---|
| **Audio Out 1** | Excitation tone, to the plant (always on) |
| **Audio Out 2** | Built-in sine voice with a decaying envelope |
| **CV Out 1** | Note pitch, 1 V/oct (calibrated) |
| **CV Out 2** | Change signal, smoothed, centred on 0 V |
| **Pulse Out 1** | Gate, high for the note length. Drops briefly if a new note interrupts. |
| **Pulse Out 2** | 10 ms trigger at the start of each note |

| LED | Meaning |
|---|---|
| 0 | Gate |
| 1 | Input clipping |
| 2 / 3 | Change signal below / above baseline |
| 4 | On = plant signal OK. Blinking = signal lost (electrodes off, or level below 30% of the calibrated level). No notes play while it blinks. |
| 5 | USB MIDI connected |

### USB MIDI

Plug the Computer's USB into a computer and it appears as a MIDI device called **Sprout**:

- Note On / Note Off on **channel 1**, velocity from the size of the jump
- **CC 80** follows the change signal (64 = baseline), the same CC number as the MIDI Sprout

## How it works

**Measuring.** Audio Out 1 sends a sine tone through the plant. The plant and the amplifier's input resistance form a voltage divider, so the level that comes back follows the plant's resistance. The level is measured by lock-in detection: the input is multiplied by the tone and by a 90° copy of it, and summed over 20 ms windows that always hold a whole number of cycles. DC, drift and mains hum cancel out. The current through the plant is tiny (microamps) and alternates direction every cycle, so the electrodes don't polarise and the plant isn't harmed.

**Deciding when to play.** Every 20 ms, the last 10 readings must pass two tests:

- **Shape** (from the MIDI Sprout): `max − min > standard deviation × 2.6`. This picks out sudden jumps.
- **Size** (new): `max − min > typical wobble × sensitivity`. "Typical wobble" is a slow (about 20 s) average of how much the reading changes between windows, so the test adapts to each plant. The Main knob sets the multiplier, from 20× (CCW) to 2× (CW).

On its own, the shape test also fires on plain measurement noise. In simulation it gave 5–16 notes a second whatever the knob setting. The size test makes the knob range from silence to a busy stream.

**Choosing the note.** As in the MIDI Sprout, the fine detail of the reading picks the note from the scale, so tiny changes give different notes. The size of the jump sets the note length, and how far the jump beat the size test sets the velocity. Notes are at least 60 ms apart.

## Building

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build
cmake --build build
```

This produces `build/sprout.uf2`. A prebuilt copy is in `UF2/`.

Settings are stored in the last 4 KB sector of the program card's flash. The program runs entirely from RAM, so audio keeps running while settings are saved.

## License

MIT
