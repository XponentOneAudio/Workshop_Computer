"""Synthetic 'budgie' recordings with known answers, for testing the pipeline.

These are NOT budgie recordings. Each category is generated from the same
descriptions the classifier rules use, so getting the labels back proves the
plumbing (segmentation, pitch, AM, classification, sequence stats, export)
works, not that the categories are biologically right. Real recordings are
needed for that.
"""

import numpy as np

from . import synth

# Hidden "grammar": motifs give the sequences higher-order structure, so
# conditional entropy should fall beyond first order.
MOTIFS = ["FDC", "BB", "EDDC", "FFD", "CGC", "DBD"]
UNIGRAM = {"A": 0.08, "B": 0.2, "C": 0.2, "D": 0.25, "E": 0.12, "F": 0.12, "G": 0.08}


def _contour(rng, f_lo, f_hi, spread_st, n=5):
    base = rng.uniform(f_lo, f_hi)
    st = np.cumsum(rng.uniform(-spread_st, spread_st, n))
    st -= st.mean()
    return list(np.linspace(0, 1, n)), list(base * 2 ** (st / 12))


def element(rng, cat):
    """Measurement-shaped dict for one synthetic element of category `cat`."""
    m = {"label": cat, "am_rate_hz": 0.0, "am_depth": 0.0, "noise_mix": 0.03,
         "f0_t": [], "f0_hz": [], "harm": [0.0] * 4,
         "centroid_hz": 4000.0, "bandwidth_hz": 2000.0,
         "amp_t": [0.0, 0.15, 0.7, 1.0], "amp": [0.2, 1.0, 0.8, 0.1]}
    if cat == "A":
        m.update(dur_ms=rng.uniform(80, 200), noise_mix=0.85, gain=1.0,
                 centroid_hz=rng.uniform(3000, 5000), bandwidth_hz=3500.0)
        m["f0_t"], m["f0_hz"] = _contour(rng, 1500, 2500, 1.0)
        m["harm"] = [1.0, 0.6, 0.4, 0.3]
    elif cat == "B":
        m.update(dur_ms=rng.uniform(90, 160), gain=0.7,
                 am_rate_hz=rng.uniform(80, 300), am_depth=rng.uniform(0.3, 0.6))
        m["f0_t"], m["f0_hz"] = _contour(rng, 2200, 3600, 3.0)
        m["harm"] = [1.0, 0.2, 0.05, 0.0]
    elif cat == "C":
        m.update(dur_ms=rng.uniform(110, 250), gain=0.6)
        m["f0_t"], m["f0_hz"] = _contour(rng, 600, 1400, 1.5)
        m["harm"] = [1.0, 0.8, 0.6, 0.4]
    elif cat == "D":
        m.update(dur_ms=rng.uniform(30, 90), gain=0.6)
        m["f0_t"], m["f0_hz"] = _contour(rng, 600, 1500, 4.0)
        m["harm"] = [1.0, 0.7, 0.5, 0.3]
    elif cat == "E":
        m.update(dur_ms=rng.uniform(20, 55), noise_mix=1.0, gain=0.3,
                 centroid_hz=rng.uniform(3000, 7000))
    elif cat == "F":
        m.update(dur_ms=rng.uniform(3, 8), noise_mix=1.0, gain=0.5,
                 centroid_hz=5000.0, bandwidth_hz=6000.0,
                 amp_t=[0.0, 0.1, 0.5, 1.0], amp=[1.0, 1.0, 0.4, 0.0])
    elif cat == "G":
        m.update(dur_ms=rng.uniform(60, 150), gain=0.07)
        m["f0_t"], m["f0_hz"] = _contour(rng, 2000, 3500, 1.0)
        m["harm"] = [1.0, 0.2, 0.0, 0.0]
    m["level_db"] = 0.0
    return m


def label_sequence(rng, n):
    cats, p = list(UNIGRAM), np.array(list(UNIGRAM.values()))
    out = ""
    while len(out) < n:
        out += MOTIFS[rng.integers(len(MOTIFS))] if rng.random() < 0.6 else rng.choice(cats, p=p / p.sum())
    return out[:n]


def make(seconds, sr, seed=0, noise_db=-60.0):
    """Return (signal, truth) where truth is a list of (onset_s, offset_s, label)."""
    rng = np.random.default_rng(seed)
    x = np.zeros(int(seconds * sr))
    truth = []
    t = 0.5
    while True:
        bout = label_sequence(rng, int(rng.integers(15, 40)))
        for cat in bout:
            m = element(rng, cat)
            y = synth.render(synth.quantise(m), sr, rng, gain=m["gain"])
            s = int(t * sr)
            if s + len(y) >= len(x):
                break
            x[s:s + len(y)] += y
            truth.append((t, t + m["dur_ms"] / 1000, cat))
            t += m["dur_ms"] / 1000 + rng.uniform(0.06, 0.2)
        else:
            t += rng.uniform(1.5, 3.0)
            continue
        break
    x += 10 ** (noise_db / 20) * rng.standard_normal(len(x))
    return x, truth
