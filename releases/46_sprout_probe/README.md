# Sprout Probe

A test card for reading a plant's electrical resistance with the Workshop Computer. It is the first step towards a [MIDI Sprout](https://github.com/XponentOneAudio/MIDIsprout) style biodata card.

The card sends a quiet tone through the plant and measures how much of it comes back. It shows the result on the LEDs and on the outputs, so you can find electrode placement, tone level, frequency and amplifier gain settings that give a strong, clean signal before the full card is written.

**Status:** untested on hardware. It builds, and the measurement maths has been checked in simulation.

## How it works

```
Audio Out 1 ──► electrode A ── [PLANT] ── electrode B ──► Amplifier in (mic channel)
Audio In 1  ◄──────────────────────────────────────────── Amplifier out
```

The plant and the amplifier's input resistance form a voltage divider. When the plant's resistance changes, the level of the returned tone changes.

The card measures that level with **lock-in detection**. It multiplies the incoming signal by the tone it sent (and by a 90° shifted copy), then averages over a 20 ms window. Each window always holds a whole number of tone cycles, so:

- DC offsets and slow drift cancel out, which suits the amplifier being AC-coupled
- mains hum and other sounds are largely rejected
- averaging 960 samples gives much finer resolution than a single ADC reading

The current through the plant alternates direction every cycle and is tiny (microamps), so the electrodes don't polarise and the plant is not harmed.

You can also try it **without the amplifier**: patch electrode B straight into Audio In 1. This may work for plants and electrodes with lower resistance.

## Electrodes

- ECG / TENS pads stuck to the top and underside of a leaf (as the MIDI Sprout uses), or
- crocodile clips on a spare patch cable with the far plug cut off (tip wire to the clip)

Moisten the contact a little if the signal is weak.

## Controls

| Control | Function |
|---|---|
| **Main** | Tone output level. Start around 9 o'clock. |
| **X** | Tone frequency in five steps: 250 Hz, 500 Hz, 1 kHz, 2 kHz, 4 kHz |
| **Y** | Zoom (1× to 128×) for the change signal on CV Out 2 |
| **Switch Down** (hold) | Re-zero: snaps the baseline to the current level |
| **Switch Up / Middle** | Normal |

## Jacks

| Jack | Function |
|---|---|
| **Audio Out 1** | Excitation tone, to electrode A |
| **Audio In 1** | Returned tone, from the amplifier output (or electrode B directly) |
| **Audio Out 2** | Phase of the returned tone (−π..+π maps to about −6 V..+6 V). Changes with plant and electrode capacitance, and with touch. |
| **CV Out 1** | Returned tone level (0 V up to about +6 V at full scale) |
| **CV Out 2** | Change signal: (level − slow baseline) × zoom, centred on 0 V |
| **Pulse Out 1** | 10 ms trigger whenever the MIDI Sprout style detector fires. Each one would be a note in the full card. |
| **Pulse Out 2** | High while the input is clipping |

## LEDs

```
 0  1      0 (top), 2, 4 (bottom): level meter
 2  3      1: input clipping
 4  5      3: size of the change signal     5: detector fired
```

## Setting up

1. Patch as above. Set the amplifier to its **mic** channel with low gain. The Minimoog-style channel distorts deliberately, and distortion squashes the very level changes being measured.
2. Turn **Main** up slowly until the level meter shows **two LEDs** (LED 4 full, LED 2 part lit). If **LED 1** (clipping) lights, turn Main or the amplifier gain down.
3. If the level hardly moves however far you turn Main up, the plant resistance is too high for the amplifier's input. Add more amplifier gain, or improve the electrode contact.
4. Try each **X** frequency. Pick the one where the level is high and steady with nobody touching the plant.
5. Hold the switch **down** to re-zero. Turn **Y** up and watch **LED 3** or patch **CV Out 2** to a scope or a filter cutoff. Touch the leaf, breathe near it, or water the plant, and the change signal should move.
6. Patch **Pulse Out 1** to an envelope or drum voice to hear how often the full card would play notes.

Any patch cable from electrode B to the amplifier is a high-impedance wire, so keep it short and away from mains leads.

## Detector

The detector uses the MIDI Sprout's test. Over the last 10 readings (200 ms), it fires when

```
max − min  >  standard deviation × 2.6
```

The test responds to the *shape* of a fluctuation, such as a sudden jump, not its size. It waits 100 ms after each trigger before firing again. These values are constants at the top of the `SproutProbe` class in `main.cpp`.

## Building

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build
cmake --build build
```

The UF2 is `build/sprout_probe.uf2`. A prebuilt copy is in `UF2/`.

## License

MIT
