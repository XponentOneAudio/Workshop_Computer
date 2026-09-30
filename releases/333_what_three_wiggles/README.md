# what.three.wiggles

*Work in progress. See [TASKS.md](TASKS.md) for the design and the build plan.*

A program card for the Music Thing Workshop Computer that takes its name from what3words.

A companion webapp finds your what3words address from your location, or lets you type any three
words. It turns each word into a seed and sends the seeds to the card over USB. Every oscillator,
LFO, random walk and gate pattern on the card is generated from those seeds, so a place always
gives the same sounds and patterns. Stand somewhere else and you get a different patch.

| Output    | What it does                                                                  |
|-----------|-------------------------------------------------------------------------------|
| Audio 1   | Three oscillators in a feedback FM loop. Each word sets one oscillator's pitch, phase and FM depth. A knob and CV set the global FM depth. |
| Audio 2   | Additive oscillator whose partial levels and phases come from the words. V/oct comes in on CV In 1 and is sampled and held by triggers into CV In 2, then quantised to a scale chosen with a knob. |
| CV 1      | A wandering V/oct line: a random walk through notes, with random slew times.   |
| CV 2      | A complex LFO built from the three seeds.                                     |
| Pulse 1   | Random triggers.                                                              |
| Pulse 2   | Flip-flop gates.                                                              |

*what3words is a trademark of what3words Ltd. This project is not affiliated with or endorsed by
what3words.*
