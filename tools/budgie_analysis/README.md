# Budgie analysis toolchain

This is Phase 0 of the [Budgie card proposal](../../documentation/proposals/budgie.md).
It turns recordings of budgerigars into data the card's firmware can use: the
element templates, the sequence grammar and the rhythm. It also produces a
report you can use to explore the "language".

```
recordings (WAV) ──► segment ──► measure ──► classify (A–G) ──► sequence stats
                                                  │                      │
                       resynthesise ◄── templates ┘                      │
                            │                                            ▼
                     A/B listening WAVs        budgie_data.h · model.json · report.md
```

It needs only Python 3.9+, numpy and scipy:

```sh
cd tools/budgie_analysis
pip install -r requirements.txt
```

## Quick start

```sh
# Make a synthetic test recording with known labels, then analyse it
python3 -m budgie_analysis demo -o demo
python3 -m budgie_analysis analyze demo -o out --resynth --labels demo/synthetic_truth.csv

# Analyse real recordings (files or folders of WAVs)
python3 -m budgie_analysis analyze recordings/ -o out --resynth --clusters 7
```

The synthetic recording only tests the plumbing. Its categories are generated
from the same descriptions the classifier uses, so it says nothing about real
budgies.

## Outputs

| File | What it is |
|---|---|
| `report.md` | Category table (counts, durations, f0, AM rate, level, resynthesis error), transition matrix, conditional entropy by Markov order, IOI-ratio rhythm histogram, and rule-vs-hand-label and cluster cross-tabs |
| `elements.csv` | One row per element: timing, label, every measurement and the contour breakpoints. Open it in a spreadsheet. |
| `budgie_data.h` | C header for the firmware: element templates grouped by category, the variable-order sequence model, gap and IOI tables. Compiles with `-std=c99 -Wall -Wextra -Werror`. |
| `model.json` | Everything in the report and header, machine-readable |
| `labels/<recording>.txt` | Audacity label track of the detected elements and their labels |
| `resynth/<recording>_AB.wav` | With `--resynth`: the original, half a second of silence, then the resynthesis from the quantised templates (what the card would play) |

## What it measures

Every element (a sound bounded by silence) is described in the same terms as
the proposed synth voice (§3 of the proposal):

- **f0 contour**: YIN pitch tracking, simplified to 5 breakpoints
- **Envelope**: RMS, simplified to 4 breakpoints
- **AM rate and depth**: the budgie "buzz", taken from the spectrum of the
  normalised Hilbert envelope
- **Harmonics 1–4**: relative amplitudes, averaged over voiced frames
- **Noise mix, centre and bandwidth**: from YIN aperiodicity and the spectrum
- **Level**: relative to the loud elements in the same recording (needed for
  soft calls, G)

The categories follow Tu, Smith & Dooling (2011) and Madabhushi et al. (2023):
**A** alarm-like, **B** contact-like, **C** long harmonic (>100 ms),
**D** short harmonic, **E** noisy, **F** click, **G** soft call. The rules are
in `classify.py:rule_label` and the thresholds are in `config.py`.

## Getting recordings

- **xeno-canto**: search for *Melopsittacus undulatus*. Check each
  recording's licence. The header holds measured parameters, not audio, but
  credit the sources anyway. They're listed at the top of `budgie_data.h`.
- **Your own budgies** are best: 48 kHz mono WAV, mic about 30–50 cm from the
  cage, quiet room, no music or TV (budgies imitate it). Get long takes of
  warble. Males warble most when relaxed and in company. Contact calls come
  when a flock-mate is out of sight.
- Split long sessions into files of a few minutes. Each file's loudness is
  normalised separately.

## Calibrating

The default thresholds are educated guesses. To calibrate them:

1. Run `analyze`, then open a recording in Audacity with
   `labels/<recording>.txt` as its label track (File → Import → Labels).
2. Correct the labels by ear and eye. Use spectrogram view with a 0–12 kHz
   range. Export the label track to `hand/<recording>.txt`.
3. Run again with `--labels hand/`. The report's "Rules vs hand labels" table
   shows where the rules disagree. Adjust thresholds in a JSON file, for
   example `{"classify": {"click_max_ms": 15}}`, and pass it with
   `--config my.json`.
4. Hand labels always override the rules in the exported data. So even
   partial hand labelling improves the header immediately.

`--clusters 7` runs k-means alongside the rules. If the clusters line up with
the categories, the rules are describing real structure. If they don't, the
cross-tab shows where.

## Known limits

- Segmentation is silence-based, so compound elements with no gap come out
  as one element. Splitting them is a future step.
- Very quiet elements are lost in noisy recordings: with background noise
  about 35 dB below the loudest calls, some soft calls go undetected.
- YIN's range is set to 400–6000 Hz (`config.py`). Check that this covers
  your birds.
- The conditional-entropy estimates for long contexts need a lot of data. The
  report marks the rows it doesn't trust. Hours of warble, not minutes, are
  needed to test the reported 5th-order structure.

## Tests

```sh
python3 -m unittest discover tests
```

The tests cover pitch tracking on tones and noise, the entropy and sequence
model, template rendering, and end-to-end runs on a synthetic recording:
segmentation recall, label accuracy, contact-call f0 and AM, resynthesis
error, and compiling and running the generated header.
