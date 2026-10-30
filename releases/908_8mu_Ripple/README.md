# 8mu Ripple

*8mu Ripple* is a noise synth built from divided noise. You play it from the faders of a [Music Thing Modular 8mu](https://www.musicthing.co.uk/8mu_page/).

A square-wave clock steps a shift-register (LFSR) noise source. Every rising edge of the noise bit advances a binary ripple counter, so its outputs are noise divided by 2, 4, 8, 16 and 32. Faders mix the raw bit (/1) with the five counter outputs. The mix then goes two ways:

* **Back to the clock pitch** (*noise mod*). The clock speeds up and slows down with its own divided output, which gives rhythmic, chattering, chaotic patterns.
* **Into a resonant filter**, whose cutoff is modulated by the clock square wave (*filter mod*).

Slow clocks give stuttering rhythms and clicks. Fast clocks give squeals, chiptune-style buzzes and rich, gritty noise.

This is the classic divided-noise topology, built around a clock-driven noise register, a ripple counter, feedback and a filter. Other pages add features that the 8mu's extra faders, buttons and motion sensors make room for: LFSR length, noise colour, a hiss bed, stereo panning, non-binary count lengths, a loop lock and tilt control.

```
                    +--------- noise mod (feedback, with slew) ----------+
                    v                                                    |
 pitch --> [clock osc] --tick--> [LFSR] --> [colour] --bit--> [ripple counter]
                |                                         |       /2 /4 /8 /16 /32
                |                                        /1         |
                |                                         +--> [mix + pan] <-- hiss
                |                                                   |
                +---- filter mod (square/triangle) ---> [drive] --> [LP/BP filter] --> L/R
```

### Hardware requirements

* Workshop System Computer Rev 1.1 or later, which can act as a USB host.
* A Music Thing Modular 8mu, plugged into the Computer's USB socket. A USB hub between the two also works.

The card also works without an 8mu. The fader parameters then stay at their default settings, a mid-level mix with some noise mod and filter mod, and you play it from the knobs and jacks.

### Controls

* **Main knob:** clock pitch, from about 0.5Hz to about 32kHz. CV in 1 adds to it at about 1V/octave.
* **Knob X:** filter cutoff, 20Hz to 16kHz. CV in 2 adds to it at about 1V/octave.
* **Knob Y:** filter resonance, up to the edge of self-oscillation.
* **Switch:**
    * Up: lowpass filter
    * Middle: bandpass filter
    * Down (momentary): reset. This reseeds the noise register and clears the counter, so the same pattern starts again.

### 8mu faders

The faders have four pages. Hold an 8mu button to reach a page, and release it to return to *Play*. Faders use soft takeover: when you change page, a fader does nothing until it reaches or crosses its stored value. While you hold a page button, the 8mu's LEDs show which faders are live (lit) and which are waiting for pickup (blinking).

| Fader | Play (no button) | Noise (hold button 1) | Divide (hold button 2) | Motion (hold button 3) |
|---|---|---|---|---|
| 1 | /1 level (raw noise bit) | LFSR length | /1 pan | Tilt → clock pitch |
| 2 | /2 level | Noise colour | /2 pan | Roll → filter cutoff |
| 3 | /4 level | Hiss level | /4 pan | Rotation → noise mod |
| 4 | /8 level | Hiss colour | /8 pan | Pitch glide |
| 5 | /16 level | Drive | /16 pan | – |
| 6 | /32 level | Filter mod shape | /32 pan | – |
| 7 | Noise mod | Feedback slew | Count length | – |
| 8 | Filter mod | Key tracking | Edge mode | Volume |

**Button 4:** tap to toggle **loop lock**. While loop lock is on, the bit leaving the noise register is fed straight back in, so the current pattern repeats exactly, like a locked Turing Machine. With a short LFSR length the loop is a few steps long. With a long one it is up to 31 steps. 8mu LED 8 is lit while loop lock is on.

#### Play
* **/1 to /32 levels:** each tap is a ±1 square wave, scaled by its fader.
* **Noise mod:** how far the tap mix bends the clock pitch, up to about ±2 octaves per tap. This is the feedback that makes the patterns chaotic.
* **Filter mod:** how far the clock oscillator moves the filter cutoff, up to ±3 octaves.

#### Noise
* **LFSR length:** 15 register lengths, from 3 bits at the bottom to 31 bits at the top. Short registers loop quickly, so at audio rates they give pitched, metallic, 8-bit noise. A 31-bit register is effectively endless.
* **Noise colour:** at the bottom, the noise bit comes straight from the register (white). Raising it low-passes the register's value, once per clock tick, before the bit is taken from its sign. The bit then comes in longer runs of ones and zeros, so the divider rhythms clump, stutter and stretch out.
* **Hiss level / hiss colour:** a free-running noise bed mixed in before the filter. The colour fades from white through pink (Paul Kellet's filter, −3dB/octave) to brown (−6dB/octave).
* **Drive:** gain into the filter, ×0.25 to ×4, through a soft clipper.
* **Filter mod shape:** the filter modulation fades from the clock's square wave to its triangle.
* **Feedback slew:** smooths the noise-mod signal. Clock pitch steps turn into swoops and glides.
* **Key tracking:** how far the filter cutoff follows the clock pitch, including its feedback wobble.

#### Divide
* **Pans:** stereo position of each tap. Centre is the default, and either end sends that tap to one output only.
* **Count length:** the counter resets after 2 to 64 edges. At the top it is a plain binary counter. Lower settings give non-binary ratios, and the higher taps fall silent or turn irregular.
* **Edge mode:** bottom half counts only rising edges of the noise bit. Top half counts both edges, which doubles every division rate.

#### Motion
These faders set how much the 8mu's motion sensors play the synth. All three are off by default.
* **Tilt → clock pitch:** up to ±1.5 octaves.
* **Roll → filter cutoff:** up to ±2 octaves.
* **Rotation → noise mod:** twisting the 8mu adds feedback.
* **Pitch glide:** portamento on the clock pitch from the knob, CV and tilt.
* **Volume:** output level.

### Inputs and outputs

* **Audio in 1:** when patched, a comparator on this input replaces the noise bit, sampled at each clock tick. The ripple counter then divides any audio you feed it.
* **Audio in 2:** exponential FM of the clock, about ±2 octaves.
* **CV in 1:** clock pitch, about 1V/octave.
* **CV in 2:** filter cutoff, about 1V/octave.
* **Pulse in 1:** when patched, rising edges step the noise register instead of the internal clock. The internal clock still runs, for filter mod.
* **Pulse in 2:** reset, the same as switch down.
* **Audio out 1 / 2:** left and right.
* **CV out 1:** a random stepped voltage, taken from the register each time the counter completes a cycle.
* **CV out 2:** the noise-mod (feedback) signal, after slew.
* **Pulse out 1:** a 5ms trigger each time the counter completes a cycle.
* **Pulse out 2:** the raw noise bit (/1).

### LEDs

* **Computer LEDs 1 to 6:** the /1, /2, /4, /8, /16 and /32 taps.
* **8mu LEDs (Play page):** LEDs 1 to 6 show the taps, brighter for taps that are turned up. LED 7 shows the clock and LED 8 shows loop lock.
* **8mu LEDs (other pages):** live faders lit, faders waiting for pickup blinking.

### Tips

* Start with only /32 up, noise mod at zero, and a slow clock. You get a slow random square wave. Add /16 and /8 to build up rhythms, then bring up noise mod to make them lurch.
* A short LFSR length plus loop lock and a slow-to-mid clock gives repeating, sequencer-like patterns. Use reset (switch down or pulse in 2) to restart them on the beat.
* For C64-style chases, use a fast clock, LFSR length around 5 to 7 bits, high resonance and plenty of filter mod.
* Noise colour near the top with a fast clock gives bubbling, granular textures, because the counter outputs come in bursts.
* Patch a drum loop or an oscillator into audio in 1 to divide it down into sub-octaves and broken rhythms.

### Ideas for further expansion

Some directions this card could still go, roughly from easiest to most involved:

* **Logic between taps:** XOR, AND or OR of pairs of taps, as extra mix sources. This gives the bitwise character of 4000-series logic.
* **Programmable dividers:** replace the binary taps with arbitrary divide ratios such as /3, /5 and /7, or with Euclidean patterns clocked by the noise edges.
* **Velvet and crackle noise:** sparse random impulses (Dust-style) as an alternative bit source, for vinyl crackle and rain textures.
* **Second register, cross-coupled:** a second LFSR clocked by a tap of the first, with each one's output modulating the other's clock. This moves towards Benjolin and Rungler territory.
* **Filter models:** a 4-pole ladder alongside the SVF, a highpass mode, or a comb filter tuned to the clock frequency, which gives Karplus-style pitched noise.
* **Envelope or LPG per cycle:** a decaying envelope retriggered by each counter cycle, on the filter or a VCA, for plucked and percussive patterns.
* **Bit crushing and sample-rate reduction** on the output, in time with the clock.
* **Quantised CV out:** quantise CV out 1 to a scale, so the random steps play melodies.
* **Presets:** save and recall all four fader pages to flash with 8mu button combinations.
* **Tap-to-tempo:** set the clock from button taps, with musical-interval feedback depths.

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
