"""Sort elements into the seven warble categories (A-G).

Two routes:
  rule_label()  explicit thresholds from the published category descriptions;
                transparent and easy to tune, used by default.
  kmeans()      unsupervised clusters over z-scored features, for exploring
                whether the recordings group the way the rules assume.
Hand labels (Audacity label tracks or a CSV) override both, and are compared
against the rules so the thresholds can be calibrated.
"""

import csv
import os

import numpy as np


def rule_label(m, cfg):
    c = cfg["classify"]
    if m["dur_ms"] <= c["click_max_ms"]:
        return "F"
    if m["voiced_frac"] < c["noisy_max_voiced_frac"] or m["flatness"] > c["noisy_min_flatness"]:
        if m["dur_ms"] >= c["alarm_min_ms"] and m["level_db"] >= c["alarm_min_level_db"]:
            return "A"
        return "E"
    if m["level_db"] < c["soft_max_level_db"]:
        return "G"
    lo, hi = c["contact_f0_hz"]
    dlo, dhi = c["contact_dur_ms"]
    h1, h2 = m["harm"][0], m["harm"][1]
    pure = h1 > 0 and h2 / h1 <= c["contact_max_h2"]
    if lo <= m["f0_median"] <= hi and dlo <= m["dur_ms"] <= dhi and pure:
        return "B"
    return "C" if m["dur_ms"] >= c["long_harmonic_min_ms"] else "D"


FEATURES_FOR_CLUSTERING = [
    "log_dur", "log_f0", "voiced_frac", "flatness", "level_db",
    "fm_extent_st", "am_depth", "log_centroid", "h2",
]


def feature_matrix(elements):
    rows = []
    for m in elements:
        rows.append([
            np.log(m["dur_ms"]),
            np.log(m["f0_median"]) if m["f0_median"] > 0 else np.log(300.0),
            m["voiced_frac"], m["flatness"], m["level_db"],
            m["fm_extent_st"], m["am_depth"], np.log(m["centroid_hz"]),
            m["harm"][1] / m["harm"][0] if m["harm"][0] > 0 else 0.0,
        ])
    X = np.array(rows, dtype=float)
    sd = X.std(axis=0)
    sd[sd == 0] = 1
    return (X - X.mean(axis=0)) / sd


def kmeans(X, k, seed=0, iters=100):
    rng = np.random.default_rng(seed)
    centres = [X[rng.integers(len(X))]]
    for _ in range(1, k):  # k-means++ seeding
        d = np.min([((X - c) ** 2).sum(1) for c in centres], axis=0)
        centres.append(X[rng.choice(len(X), p=d / d.sum())] if d.sum() > 0 else X[rng.integers(len(X))])
    C = np.array(centres)
    for _ in range(iters):
        lab = np.argmin(((X[:, None, :] - C[None]) ** 2).sum(2), axis=1)
        newC = np.array([X[lab == j].mean(0) if (lab == j).any() else C[j] for j in range(k)])
        if np.allclose(newC, C):
            break
        C = newC
    return lab


def read_hand_labels(path, wav_paths):
    """Return {wav_stem: [(onset_s, label), ...]}.

    `path` is either a directory of Audacity label tracks named <stem>.txt
    (tab-separated start, end, label) or a CSV with columns file,onset_s,label.
    """
    out = {}
    stems = [os.path.splitext(os.path.basename(p))[0] for p in wav_paths]
    if os.path.isdir(path):
        for s in stems:
            f = os.path.join(path, s + ".txt")
            if not os.path.exists(f):
                continue
            rows = []
            with open(f) as fh:
                for line in fh:
                    parts = line.rstrip("\n").split("\t")
                    if len(parts) >= 3 and parts[0] and parts[0][0] not in "\\#":
                        rows.append((float(parts[0]), parts[2].strip().upper()[:1]))
            out[s] = rows
    else:
        with open(path) as fh:
            for r in csv.DictReader(fh):
                s = os.path.splitext(os.path.basename(r["file"]))[0]
                out.setdefault(s, []).append((float(r["onset_s"]), r["label"].strip().upper()[:1]))
    return out


def match_labels(elements, labels, tol_s=0.02):
    """For each element, the hand label whose onset is nearest (within tol_s), else None."""
    if not labels:
        return [None] * len(elements)
    onsets = np.array([t for t, _ in labels])
    out = []
    for m in elements:
        i = int(np.argmin(np.abs(onsets - m["onset_s"])))
        out.append(labels[i][1] if abs(onsets[i] - m["onset_s"]) <= tol_s else None)
    return out


def write_audacity_labels(path, elements):
    with open(path, "w") as f:
        for m in elements:
            f.write(f"{m['onset_s']:.6f}\t{m['offset_s']:.6f}\t{m['label']}\n")
