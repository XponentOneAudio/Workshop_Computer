# 8mu Ripple

*8mu Ripple* is a noise synth built from divided noise. You play it from the faders of a [Music Thing Modular 8mu](https://www.musicthing.co.uk/8mu_page/).

A square-wave clock steps a shift-register (LFSR) noise source. Every rising edge of the noise bit advances five dividers. By default they divide by 2, 4, 8, 16 and 32, exactly like a binary ripple counter, but each can be set to any ratio from 2 to 37, including the primes. Faders mix the raw bit (/1) with the five divider outputs and with four logic combinations of them (XOR, AND, OR, AND NOT). The mix then goes two ways:

* **Back to the clock pitch** (*noise mod*). The clock speeds up and slows down with its own divided output, which gives rhythmic, chattering, chaotic patterns.
* **Into a filter**, whose cutoff is modulated by the clock square wave (*filter mod*). There are six filter types: lowpass, bandpass, highpass, notch, a 4-pole ladder and a tuned comb.

Slow clocks give stuttering rhythms and clicks. Fast clocks give squeals, chiptune-style buzzes and rich, gritty noise.

This is the classic divided-noise topology, built around a clock-driven noise register, a divider chain, feedback and a filter. Further pages add features that the 8mu's extra faders, buttons and motion sensors make room for.

```
                    +--------- noise mod (feedback, with slew) ----------+
                    v                                                    |
 pitch --> [clock osc] --tick--> [LFSR] --> [colour] --bit--> [dividers, 2..37]
                |                                         |          |
                |                                        /1     [logic A-D]
                |                                         |          |
                |                                         +--> [mix + pan] <-- hiss
                |                                                    |
                +---- filter mod (square/triangle) ---> [drive] --> [filter A/B] --> L/R
```

### Hardware requirements

* Workshop System Computer Rev 1.1 or later, which can act as a USB host.
* A Music Thing Modular 8mu, plugged into the Computer's USB socket. A USB hub between the two also works.

The card also works without an 8mu. The fader parameters then stay at their default settings, a mid-level mix with some noise mod and filter mod, and you play it from the knobs and jacks.

### Controls

* **Main knob:** clock pitch, from about 0.5Hz to about 32kHz. CV in 1 adds to it at about 1V/octave.
* **Knob X:** filter cutoff, 20Hz to 16kHz, or the comb's pitch. With *clock tune* on, it sets an interval from the clock instead, ±4 octaves with unison at noon. CV in 2 adds to it at about 1V/octave.
* **Knob Y:** filter resonance. For the comb filter, this is its feedback.
* **Switch:**
    * Up: filter type A (default lowpass)
    * Middle: filter type B (default bandpass)
    * Down (momentary): reset. This reseeds the noise register and clears the dividers, so the same pattern starts again.

### 8mu pages

The 8mu's faders have seven pages, in three groups. **Tap** button 1, 2 or 3 to go to the first page of that button's group. Tap the same button again to step on to the group's next page, and around to its first page again.

| Button | Pages, in order |
|---|---|
| 1 | Play → Logic |
| 2 | Noise → Filter |
| 3 | Divide → Ratios → Motion |
| 4 | tap to toggle loop lock |

Tap button 1 at any time to get back to *Play*.

**Page indicator.** For half a second after you change page, a single 8mu LED shows which page you are on: LED 1 for Play, 2 Logic, 3 Noise, 4 Filter, 5 Divide, 6 Ratios, 7 Motion.

**Soft takeover.** When you change page, a fader does nothing until it reaches or crosses its stored value, so parameters never jump. On every page except Play, the 8mu's LEDs show which faders are live (lit) and which are waiting for pickup (blinking). Faders that do nothing on that page are dark.

| Fader | Play | Logic | Noise | Filter | Divide | Ratios | Motion |
|---|---|---|---|---|---|---|---|
| 1 | /1 level (raw noise bit) | Logic A level | LFSR length | Type A | /1 pan | Divider 1 ratio | Tilt → clock pitch |
| 2 | Divider 1 level (/2) | Logic B level | Noise colour | Type B | Divider 1 pan | Divider 2 ratio | Roll → filter cutoff |
| 3 | Divider 2 level (/4) | Logic C level | Hiss level | Comb damping | Divider 2 pan | Divider 3 ratio | Rotation → noise mod |
| 4 | Divider 3 level (/8) | Logic D level | Hiss colour | Filter mod shape | Divider 3 pan | Divider 4 ratio | Pitch glide |
| 5 | Divider 4 level (/16) | Logic A function | Feedback slew | Key tracking | Divider 4 pan | Divider 5 ratio | – |
| 6 | Divider 5 level (/32) | Logic B function | – | Drive | Divider 5 pan | – | – |
| 7 | Noise mod | Logic C function | – | Clock tune | Logic spread | Count length | – |
| 8 | Filter mod | Logic D function | – | – | – | Edge mode | Volume |

**Button 4:** tap to toggle **loop lock**. While loop lock is on, the bit leaving the noise register is fed straight back in, so the current pattern repeats exactly, like a locked Turing Machine. With a short LFSR length the loop is a few steps long. With a long one it is up to 31 steps. On the Play page, 8mu LED 8 is lit while loop lock is on.

#### Play
* **/1 and divider levels:** each source is a ±1 square wave, scaled by its fader.
* **Noise mod:** how far the source mix bends the clock pitch, up to about ±2 octaves per source. This is the feedback that makes the patterns chaotic. The logic outputs feed into it too.
* **Filter mod:** how far the clock oscillator moves the filter cutoff, up to ±3 octaves.

#### Logic
Four logic outputs, each combining a pair of neighbouring dividers:

| Output | Inputs |
|---|---|
| A | divider 1 and divider 2 |
| B | divider 2 and divider 3 |
| C | divider 3 and divider 4 |
| D | divider 4 and divider 5 |

Faders 1 to 4 set their levels, which are all zero by default. Faders 5 to 8 choose each output's function, in four zones from bottom to top:

| Zone | Function | Result |
|---|---|---|
| 1 | XOR | High when the two inputs differ. This gives the classic ring-mod-like, sum-and-difference rhythms. |
| 2 | AND | High only when both inputs are high. Gives short, sparse pulses. |
| 3 | OR | High when either input is high. Gives long, dense pulses. |
| 4 | AND NOT | First input high and second low. Gives syncopated gaps. |

Because the inputs follow the Ratios page, the logic outputs change with the divide ratios. For example, /3 XOR /4 gives a 12-step pattern that no binary divider can make.

#### Noise
* **LFSR length:** 15 register lengths, from 3 bits at the bottom to 31 bits at the top. Short registers loop quickly, so at audio rates they give pitched, metallic, 8-bit noise. A 31-bit register is effectively endless.
* **Noise colour:** at the bottom, the noise bit comes straight from the register (white). Raising it low-passes the register's value, once per clock tick, before the bit is taken from its sign. The bit then comes in longer runs of ones and zeros, so the divider rhythms clump, stutter and stretch out.
* **Hiss level / hiss colour:** a free-running noise bed mixed in before the filter. The colour fades from white through pink (Paul Kellet's filter, −3dB/octave) to brown (−6dB/octave).
* **Feedback slew:** smooths the noise-mod signal. Clock pitch steps turn into swoops and glides.

#### Filter
* **Type A / Type B:** the filter types selected by switch up and switch middle. Each fader has six zones, from bottom to top:

  | Zone | Type | What it does |
  |---|---|---|
  | 1 | Lowpass | 2-pole state variable filter. This is the type A default. |
  | 2 | Bandpass | 2-pole state variable filter. This is the type B default. |
  | 3 | Highpass | 2-pole state variable filter. |
  | 4 | Notch | 2-pole state variable filter. |
  | 5 | Ladder | 4-pole lowpass with saturating feedback. Darker and squelchier than the SVF lowpass, and it self-oscillates at the top of the resonance range. |
  | 6 | Comb | Feedback comb pitched at the cutoff frequency, 23Hz to 16kHz, with knob Y as feedback. It turns noise into plucked, Karplus-Strong-style pitched tones and metallic resonances. |

* **Comb damping:** a lowpass in the comb's feedback loop. At the bottom it is bright and ringing, and at the top it gives soft, string-like decays.
* **Filter mod shape:** the filter modulation fades from the clock's square wave to its triangle.
* **Key tracking:** how far the cutoff follows the clock pitch, including its feedback wobble.
* **Drive:** gain into the filter, ×0.25 to ×4, through a soft clipper.
* **Clock tune:** in the top half, the cutoff (or comb pitch) is tuned to the clock frequency, and knob X becomes an interval from it. With the comb filter this gives pitched noise that follows the clock, and its feedback swoops, exactly.

#### Divide
* **Pans:** stereo position of /1 and each divider. Centre is the default, and either end sends that source to one output only.
* **Logic spread:** spreads the logic outputs across the stereo field, with A and C to the left and B and D to the right.

#### Ratios
* **Divider 1 to 5 ratio:** each fader picks from 2, 3, 4, 5, 7, 8, 11, 13, 16, 17, 19, 23, 29, 31, 32 and 37. These are the powers of two and the primes. The defaults are 2, 4, 8, 16 and 32. With odd ratios the output is high for slightly less than half of each cycle.
* **Count length:** at the top, the dividers run freely, so prime ratios drift against each other in long polyrhythms. Lower settings reset every divider together after 2 to 63 edges, which turns the drifting patterns into repeating bars.
* **Edge mode:** bottom half counts only rising edges of the noise bit. Top half counts both edges, which doubles every division rate.

The cycle outputs (CV out 1 and pulse out 1) fire every count-length edges, or every 64 edges when free running.

#### Motion
These faders set how much the 8mu's motion sensors play the synth. All three are off by default.
* **Tilt → clock pitch:** up to ±1.5 octaves.
* **Roll → filter cutoff:** up to ±2 octaves.
* **Rotation → noise mod:** twisting the 8mu adds feedback.
* **Pitch glide:** portamento on the clock pitch from the knob, CV and tilt.
* **Volume:** output level.

### Inputs and outputs

* **Audio in 1:** when patched, a comparator on this input replaces the noise bit, sampled at each clock tick. The dividers then divide any audio you feed them.
* **Audio in 2:** exponential FM of the clock, about ±2 octaves.
* **CV in 1:** clock pitch, about 1V/octave.
* **CV in 2:** filter cutoff, about 1V/octave.
* **Pulse in 1:** when patched, rising edges step the noise register instead of the internal clock. The internal clock still runs, for filter mod.
* **Pulse in 2:** reset, the same as switch down.
* **Audio out 1 / 2:** left and right.
* **CV out 1:** a random stepped voltage, taken from the register on each counter cycle.
* **CV out 2:** the noise-mod (feedback) signal, after slew.
* **Pulse out 1:** a 5ms trigger on each counter cycle.
* **Pulse out 2:** the raw noise bit (/1).

### LEDs

* **Computer LEDs 1 to 6:** /1 and dividers 1 to 5.
* **8mu LEDs, Play page:** LEDs 1 to 6 show /1 and the dividers, brighter for louder ones. LED 7 shows the clock and LED 8 shows loop lock.
* **8mu LEDs, other pages:** live faders lit, faders waiting for pickup blinking.
* **8mu LEDs, after a page change:** for half a second, one LED shows the page number.

### Tips

* Start with only /32 up, noise mod at zero, and a slow clock. You get a slow random square wave. Add /16 and /8 to build up rhythms, then bring up noise mod to make them lurch.
* A short LFSR length plus loop lock and a slow-to-mid clock gives repeating, sequencer-like patterns. Use reset (switch down or pulse in 2) to restart them on the beat.
* For C64-style chases, use a fast clock, LFSR length around 5 to 7 bits, high resonance and plenty of filter mod.
* Noise colour near the top with a fast clock gives bubbling, granular textures, because the divider outputs come in bursts.
* Set the ratios to 3, 5, 7, 11 and 13 with count length at the top for slowly evolving polyrhythms. Then pull count length down until the pattern locks into a bar length you like.
* For plucked tones that follow the clock, set filter type A to comb and clock tune on. Turn knob Y up and use comb damping for tone.
* Set type A to ladder and type B to comb, then flip the switch for two very different voices from the same pattern.
* Patch a drum loop or an oscillator into audio in 1 to divide it down into sub-octaves and broken rhythms.

### Ideas for further expansion

* **Euclidean dividers:** spread k pulses evenly across n noise edges, as another divider mode.
* **Velvet and crackle noise:** sparse random impulses (Dust-style) as an alternative bit source, for vinyl crackle and rain textures.
* **Second register, cross-coupled:** a second LFSR clocked by a divider of the first, with each one's output modulating the other's clock. This moves towards Benjolin and Rungler territory.
* **Envelope or LPG per cycle:** a decaying envelope retriggered on each counter cycle, on the filter or a VCA, for plucked and percussive patterns.
* **Bit crushing and sample-rate reduction** on the output, in time with the clock.
* **Quantised CV out:** quantise CV out 1 to a scale, so the random steps play melodies.
* **Presets:** save and recall all seven fader pages to flash with 8mu button combinations.

### Building

The card is built with the Pico SDK (2.1 or later) and [ComputerCard](https://github.com/TomWhitwell/Workshop_Computer/tree/main/Demonstrations%2BHelloWorlds/PicoSDK/ComputerCard). `EightMU.h` provides the 8mu USB host support.

```
mkdir build
cd build
cmake ..
make
```

Then copy `build/8mu_ripple.uf2` to the Computer.

All the sound generation is in `RippleEngine.h`, which has no hardware dependencies. You can include it in a desktop program to render audio and try out changes.
