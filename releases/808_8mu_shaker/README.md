# 8mu Shaker

*Pick up a Music Thing [8mu](https://www.musicthing.co.uk/8mu.html) and shake it. A physically modelled shaker.*

8mu Shaker turns the 8mu's accelerometer into a percussion instrument. Inside the card is a simulated shell with a simulated handful of grains in it. Shake, tilt or twist the 8mu and the grains are thrown around; the eight faders decide what the shaker is: its size, what it is made of, how many grains it holds and how hard they are. One set of faders is a maraca, another a strap of sleigh bells, another a rainstick you play by tipping the controller slowly from side to side.

The code is written to be read by someone learning C++, with comments explaining both the model and the language as it goes. Start with `Shaker.h`.

> **Status: untested on hardware.** The firmware builds, and the model has been run and measured on a desktop computer, but nobody has yet shaken a real 8mu at it. The feel of the motion response in particular will probably want adjusting. See [Tuning it](#tuning-it).

## Controls

**8mu faders**

| Fader | Name | Bottom | Top |
|---|---|---|---|
| 1 | Size | small, high pitched | big, low pitched |
| 2 | Material | dead: gourd, wood, card | ringing: metal |
| 3 | Grains | a few beads | about a thousand |
| 4 | Hardness | soft and dull: sand | hard and bright: steel shot |
| 5 | Decay | rattle stops at once | rattle carries on for seconds |
| 6 | Sensitivity | needs a hard shake | the slightest movement |
| 7 | Travel | grains packed tight | a long tube: rainstick |
| 8 | Scatter | one shell | many differently tuned bells or jingles |

**8mu buttons**

| Button | What it does |
|---|---|
| 1 | Soft shake |
| 2 | Hard shake |
| 3 | Hold to roll: the grains keep rattling |
| 4 | Hold to choke: a hand gripped round the shell |

**8mu movement**

| Movement | What it does |
|---|---|
| Shake side to side, or front to back | Throws the grains from wall to wall |
| Tilt | Lets the grains slide downhill |
| Spin flat on the table | Swirls the grains round the inside |

**8mu LEDs** show a bar graph of how hard the shaker is rattling.

**Computer panel**

| Control | Job |
|---|---|
| Main | Volume. Fully clockwise is deliberately too loud, to bring up quiet settings |
| X | Tune: shifts the Size up or down by about two octaves. Middle is no change |
| Y | Stereo width, from mono to wide |
| Switch Up | Roll, the same as holding button 3 |
| Switch Middle | Normal |
| Switch Down | Momentary: one hard shake |

**Jacks**

| Jack | Job |
|---|---|
| Audio Out 1 / 2 | The shaker, left and right |
| Audio In 1 | An external sound fed into the shell, so the shell works as a resonator |
| CV In 1 | Size. Higher voltage is smaller and higher, roughly an octave per volt |
| CV In 2 | Shake amount. A positive voltage keeps the grains rattling |
| Pulse In 1 | Trigger a hard shake (accent) |
| Pulse In 2 | Trigger a soft shake (ghost note) |
| CV Out 1 | Rattle energy, 0V upwards: an envelope follower for your hand |
| CV Out 2 | Where the grains are, left to right, about -6V to +6V |
| Pulse Out 1 | A short pulse for every grain collision: random triggers, as dense as Grains |
| Pulse Out 2 | A pulse each time the grains hit a wall: the rhythm you are shaking |

**Panel LEDs**

| LED | Meaning |
|---|---|
| 0 | Lit while an 8mu is connected |
| 1 | Flickers with grain collisions |
| 2, 3 | Loudness of the left and right outputs |
| 4 | Rattle energy |
| 5 | Flashes when the grains hit a wall |

## Some starting points

Fader positions from 0 (bottom) to 10 (top).

| Sound | Size | Material | Grains | Hardness | Decay | Travel | Scatter |
|---|---|---|---|---|---|---|---|
| Maraca | 5 | 2 | 6 | 7 | 3 | 3 | 0 |
| Sleigh bells | 2 | 9 | 3 | 9 | 5 | 3 | 5 |
| Box of sand | 9 | 0 | 10 | 1 | 3 | 3 | 0 |
| Rainstick (tilt slowly) | 4 | 4 | 5 | 7 | 7 | 8 | 2 |
| One pea in a tin | 4 | 8 | 1 | 10 | 5 | 3 | 0 |

Set Sensitivity to taste. With Travel high, the Decay fader has less to do, because the grains are making sound for as long as they are sliding.

## Without an 8mu

The card still works as a percussion voice. It keeps its default maraca settings, the switch and both pulse inputs trigger it, CV In 2 shakes it, and X retunes it. Unplug an 8mu part way through and the faders stay where they were.

## How the model works

The model has three layers, each feeding the next. `Shaker.h` explains them at length.

1. **The mass of grains.** A thousand times a second, all the grains together are treated as one lump sliding in a box. The accelerometer says which way is downhill; the lump accelerates, slides with some friction, and thumps into a wall. Wall impacts and fast sliding create *rattle energy*, which then fades at a rate set by Decay. Sensitivity scales the acceleration and Travel is the length of the box.
2. **The individual grains.** 48,000 times a second a dice is rolled. With a probability set by Grains, a collision happens and makes a tiny click of noise, as loud as the current rattle energy. As the energy dies away the collisions become rarer as well as quieter, until the grains settle and stop. Hardness sets how short and bright each click is. This is based on Perry Cook's PhISM method (Physically Informed Stochastic Event Modelling).
3. **The shell.** The clicks excite three resonators, each a ring at one frequency. Size sets their pitch and Material how long they ring. Scatter retunes one of them at random on every collision, so the clicks seem to come from many separate objects. The resonators are built so that integer rounding can only ever shorten a ring, never sustain it, which guarantees they fall fully silent.

Grains are panned towards wherever the lump currently is, so a slow tilt moves the sound across the stereo field.

## Tuning it

All of the numbers that decide how the shaker feels are in `Shaker.h`, and each is commented:

- The ranges of every fader are set in `Init()`, as `Sweep(from, to, t)` lines. To make the longest Decay 10 seconds rather than 3, change one number.
- `restThreshold` is how far the 8mu must tilt before the grains move at all.
- Friction, and how much the grains bounce off a wall, are in `ControlTick()`.
- `modePitchOffset` sets the musical interval between the shell's three resonances.

You can hear a change without any hardware. `test/render_wav.cpp` runs the model on an ordinary computer, with a pretend hand doing the shaking, and writes a WAV file:

```bash
g++ -O2 -I. -o render_wav test/render_wav.cpp
./render_wav shaker.wav
```

## Ideas for more controls

Things that would fit the model but are not built:

- **Shaker shape.** Different Travel for the two axes, so side-to-side and front-to-back shakes sound different, like a long tube against a round ball.
- **Fill level.** A nearly full shaker is quiet and tight because the grains have no room to move. This could reduce Travel and energy together.
- **Number of resonances.** One for a simple tube, up to five or six for a bell.
- **Upside down.** The 8mu reports which way up it is (`Flip()`), which is currently unused. Turning it over could switch between two saved shakers.
- **Saved presets** on the buttons, with the faders picking up from the stored values.
- **Vertical shaking.** Only two of the accelerometer's three axes are used.

## Requirements

The 8mu is a USB device, so the Computer has to act as a USB host. This needs a Rev 1.1 or later board, with the 8mu plugged into the Computer's front USB-C socket. On connecting, the card resets the 8mu to its default fader and button assignments.

## Building

With the [Pico SDK](https://github.com/raspberrypi/pico-sdk) installed and `PICO_SDK_PATH` set:

```bash
cmake -B build -G Ninja
cmake --build build
```

Drag `build/8mu_shaker.uf2` onto the Pico in bootloader mode, or use the binary committed at `UF2/8mu_shaker.uf2`.

## Files

| File | What it is |
|---|---|
| `Shaker.h` | The physical model. Plain C++, no hardware code |
| `main.cpp` | Connects the model to the 8mu, the knobs, the jacks and the LEDs |
| `test/render_wav.cpp` | Runs the model on a desktop computer and writes a WAV file |
| `ComputerCard.h` | Chris Johnson's hardware library for the Workshop Computer |
| `EightMU.h` | Chris Johnson's 8mu library, including the USB MIDI host driver |

## Credits

- **Chris Johnson**: the ComputerCard library and `EightMU.h`.
- **rppicomidi**: the `usb_midi_host` driver, MIT, vendored inside `EightMU.h`.
- **Perry Cook**: the PhISM shaker model that layer 2 is based on.
- **Tom Whitwell / Music Thing Modular**: the Workshop System and the 8mu.

## License

MIT. `ComputerCard.h` and `EightMU.h` are vendored unmodified under their own MIT terms, and their copyright headers are intact in those files.
