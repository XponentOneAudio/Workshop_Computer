# Tonnetz

Four voices walk around a [Tonnetz](https://en.wikipedia.org/wiki/Tonnetz), the lattice of pitches in which each note's six neighbours are a perfect fifth, a major third and a minor third above and below it. Each step moves **one voice**, in turn, one place across the lattice in the chosen direction. Once every voice has moved, the whole chord has slid one place along the lattice; the half-moved chords in between are the passing harmonies.

The chord starts as a major seventh on the root (root, 3rd, 5th, 7th: a small square on the lattice). Heading right (+P5), for example, four steps carry Cmaj7 through C-E-G-B → G-E-G-B → G-B-G-B → G-B-D-B → G-B-D-F#, which is Gmaj7.

## Jacks and controls

| | |
|---|---|
| **Main knob** | Root note, C2 to C4 in semitones |
| **Knob X / Y** | Direction of travel (centre is neutral), added to CV In 1 / 2 and the Nunchuk |
| **Switch up** | Glide: voices slide to each new note over about 60ms |
| **Switch middle** | Voices jump straight to each new note |
| **Switch down** | Momentary: reset to the starting chord and restart the voice order |
| **CV In 1 / 2** | Direction of travel, X / Y |
| **Pulse In 1** | Step: move the next voice |
| **Pulse In 2** | Toggle the fourth sine voice on/off |
| **CV Out 1 / 2** | Voice 1 / 2 pitch, calibrated 1V/oct (0V = middle C) for two external oscillators |
| **Audio Out 1 / 2** | Voice 1 / 2 sine, plus the fourth voice's sine, mixed into both, when it is on |
| **Pulse Out 1** | Voice 3 square |
| **Pulse Out 2** | Sub: square one octave below the lowest sounding voice |

While the fourth voice is off, stepping cycles through voices 1-3 only, so every step changes something you can hear; turning it on adds it back into the cycle where it was left.

## Directions and LEDs

The direction vector (CV ins + knobs X/Y + Nunchuk joystick) is quantised to the six lattice neighbours. If it is close to the centre, the last direction is kept (starting at +P5).

```
   up-left   -m3   +M3   up-right        LED 0   LED 1
   left      -P5    o    +P5  right    LED 2       LED 3
   down-left -M3   +m3   down-right      LED 4   LED 5
```

The LED for the current direction is lit and blinks off briefly on each step. The other five glow dimly while the fourth voice is on.

Each voice takes the shorter way to its new note (a fifth up may be played as a fourth down, whichever stays nearer the voice's starting note) and folds by an octave if it would wander more than eight semitones from it, so the voicing stays close.

## Wii Nunchuk

A Wii Nunchuk can steer the card through the Pico Nunchuk adapter (`Demonstrations+HelloWorlds/PicoSDK/NunchukAdapter` on the `ccr-881cab88-e1fiaf` / `nn_wavedigiHypster` branch), which presents the Nunchuk to the Computer as an 8mu over USB MIDI. Plug the Pico into the Computer's USB socket (needs a Rev 1.1 or later Computer).

| Nunchuk | |
|---|---|
| Joystick | Direction of travel, added to CV ins and knobs X/Y |
| Z button | Step the next voice in the joystick's direction |
| C button | Step the next voice in the opposite direction |

Leave the joystick alone while plugging in: the adapter takes its centre from the resting position.

## Building

```
mkdir build && cd build
cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
make
```

`ComputerCard.h` and `EightMU.h` are vendored unmodified from `Demonstrations+HelloWorlds/PicoSDK/ComputerCard`.
