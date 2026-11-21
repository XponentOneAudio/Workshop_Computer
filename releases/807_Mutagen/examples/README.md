# Example sounds

Rendered by `render.cpp` with the card's own engine (`../mutagen.h`, the
same code the firmware runs), at 48 kHz and the card's 12-bit resolution.
Stereo: **Audio Out 1 left, Audio Out 2 right** (Out 2 is the dry wave
unless noted). The card itself drones; here each note gets a short fade in
and out, as a VCA after it would give. Most use the web editor's built-in
presets.

| File | What it shows |
|---|---|
| 01_default_saw_pulse | The default: saw through PULSE, its width swept by LFO 1 |
| 02_classic_pwm_square | Square-wave PWM bass |
| 03_pwm_from_two_saws | PHASEDIFF: a pulse made from two saws, the cleanest PWM |
| 04_sync_sweep_knob | SYNC depth swept up and back by the knob: the classic sync sweep |
| 05_windowed_sync_formants | SYNC with full WINDOW on a sine: vocal formants |
| 06_pulse_sync_ratio2 | SYNC at ratio 2 into PULSE |
| 07_mirror_and_squeeze | MIRROR into SQUEEZE on a triangle |
| 08_warp_bells_ratio3 | WARP at ratio 3 with a jagged drawn curve: bells |
| 09_squeeze_bass_with_sub_R | SQUEEZE bass with drive; Out 2 (right) is the sub an octave down |
| 10_feedback_grit | PULSE and PULSE with feedback and the bit crusher |
| 11_subharmonic_ratio_2_3 | PULSE at ratio ⅔: sub-harmonics |
| 12_ratio_sweep_knob | The ratio knob swept from 1 to 16 and back, settling on whole numbers |
| 13_mode_tour | Each mode in turn (2 s each), depth swept up and down |

To render them: `g++ -std=c++17 -O2 render.cpp -o render && ./render`
