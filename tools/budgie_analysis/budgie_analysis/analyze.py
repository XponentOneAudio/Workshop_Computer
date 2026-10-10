"""The full pipeline: recordings in, element table + model + C header + report out."""

import csv
import json
import os

import numpy as np

from . import audio, classify, export, features, segment, sequence, synth
from .config import CATEGORIES, CATEGORY_NAMES


def analyse_file(path, cfg, hand=None):
    sr = cfg["sample_rate"]
    raw = audio.load(path, sr)
    x = segment.bandpass(raw, sr, cfg["segment"])
    spans = segment.find_elements(x, sr, cfg["segment"])
    els = [features.measure(x, sr, s, e, cfg) for s, e in spans]
    if els:
        ref = np.percentile([m["peak_db"] for m in els], 95)
        for m in els:
            m["level_db"] = m["peak_db"] - ref
    stem = os.path.splitext(os.path.basename(path))[0]
    hand_labels = classify.match_labels(els, (hand or {}).get(stem))
    for m, h in zip(els, hand_labels):
        m["file"] = stem
        m["rule_label"] = classify.rule_label(m, cfg)
        m["hand_label"] = h or ""
        m["label"] = h if h and h in CATEGORIES else m["rule_label"]
    groups = segment.bouts(spans, sr, cfg["segment"]["bout_gap_s"])
    for b, g in enumerate(groups):
        for i in g:
            els[i]["bout"] = f"{stem}#{b}"
    return x, els, [[els[i] for i in g] for g in groups]


def run(paths, out_dir, cfg, labels=None, clusters=0, resynth=False):
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(os.path.join(out_dir, "labels"), exist_ok=True)
    sr = cfg["sample_rate"]
    hand = classify.read_hand_labels(labels, paths) if labels else None

    all_els, all_bouts = [], []
    for p in paths:
        x, els, bouts = analyse_file(p, cfg, hand)
        all_els.extend(els)
        all_bouts.extend(bouts)
        classify.write_audacity_labels(os.path.join(out_dir, "labels", els[0]["file"] + ".txt") if els
                                       else os.path.join(out_dir, "labels", "empty.txt"), els)
        if resynth and els:
            os.makedirs(os.path.join(out_dir, "resynth"), exist_ok=True)
            y = synth.resynthesise(els, len(x), sr)
            for m in els:
                s, e = int(m["onset_s"] * sr), int(m["offset_s"] * sr)
                m["resynth_dist_db"] = synth.spectral_distance_db(x[s:e], y[s:e], sr)
            gap = np.zeros(int(0.5 * sr))
            norm = np.max(np.abs(x)) + 1e-12
            audio.save(os.path.join(out_dir, "resynth", els[0]["file"] + "_resynth.wav"), y / norm, sr)
            audio.save(os.path.join(out_dir, "resynth", els[0]["file"] + "_AB.wav"),
                       np.concatenate([x, gap, y]) / norm, sr)
    if not all_els:
        raise SystemExit("No elements found. Check the recordings, or lower segment.on_db.")

    X = classify.feature_matrix(all_els)
    if clusters:
        for m, c in zip(all_els, classify.kmeans(X, clusters)):
            m["cluster"] = int(c)

    seqs = ["".join(e["label"] for e in b) for b in all_bouts]
    model = sequence.variable_order_model(seqs, cfg)
    ent = sequence.conditional_entropy(seqs, cfg["sequence"]["max_order"] + 1)
    rhy = sequence.rhythm(all_bouts, cfg)
    trans = sequence.transition_matrix(seqs)

    chosen = export.choose_templates(all_els, X, cfg["export"]["max_templates_per_category"])
    templates = [synth.quantise(all_els[i]) for i in chosen]
    for i in chosen:
        all_els[i]["template"] = True
    sources = [os.path.basename(p) for p in paths]
    with open(os.path.join(out_dir, "budgie_data.h"), "w") as f:
        f.write(export.header(templates, model, rhy["gap_ms_median"], rhy["ioi_ratio_hist"],
                              rhy["elements_per_s"], sources))

    _write_csv(os.path.join(out_dir, "elements.csv"), all_els)
    summary = {
        "sources": sources,
        "config": cfg,
        "n_elements": len(all_els),
        "n_bouts": len(all_bouts),
        "categories": _category_stats(all_els),
        "transitions": trans.tolist(),
        "conditional_entropy": ent,
        "contexts": model,
        "rhythm": {k: v for k, v in rhy.items() if k != "ioi_s"},
        "templates": templates,
    }
    with open(os.path.join(out_dir, "model.json"), "w") as f:
        json.dump(summary, f, indent=1)
    with open(os.path.join(out_dir, "report.md"), "w") as f:
        f.write(report(summary, all_els, rhy, clusters))
    return summary


SCALAR_COLUMNS = [
    "file", "bout", "onset_s", "offset_s", "dur_ms", "label", "rule_label", "hand_label",
    "cluster", "template", "peak_db", "level_db", "voiced_frac", "f0_median", "f0_min",
    "f0_max", "fm_extent_st", "am_rate_hz", "am_depth", "noise_mix", "centroid_hz",
    "bandwidth_hz", "flatness", "resynth_dist_db",
]
LIST_COLUMNS = ["f0_t", "f0_hz", "amp_t", "amp", "harm"]


def _write_csv(path, els):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(SCALAR_COLUMNS + LIST_COLUMNS)
        for m in els:
            row = []
            for k in SCALAR_COLUMNS:
                v = m.get(k, "")
                row.append(f"{v:.7g}" if isinstance(v, float) else v)
            row += [" ".join(f"{v:.4g}" for v in m[k]) for k in LIST_COLUMNS]
            w.writerow(row)


def _q(v, p):
    return float(np.percentile(v, p)) if len(v) else 0.0


def _category_stats(els):
    out = {}
    for c in CATEGORIES:
        sel = [m for m in els if m["label"] == c]
        dur = [m["dur_ms"] for m in sel]
        f0 = [m["f0_median"] for m in sel if m["f0_median"] > 0]
        am = [m["am_rate_hz"] for m in sel if m["am_depth"] > 0.1]
        out[c] = {
            "name": CATEGORY_NAMES[c],
            "count": len(sel),
            "share": len(sel) / len(els),
            "dur_ms": [_q(dur, 25), _q(dur, 50), _q(dur, 75)],
            "f0_hz": [_q(f0, 25), _q(f0, 50), _q(f0, 75)],
            "am_rate_hz_median": _q(am, 50),
            "level_db_median": _q([m["level_db"] for m in sel], 50),
            "resynth_dist_db_median": _q([m["resynth_dist_db"] for m in sel
                                          if np.isfinite(m.get("resynth_dist_db", np.nan))], 50),
        }
    return out


def report(s, els, rhy, clusters):
    L = []
    w = L.append
    w("# Budgie analysis report\n")
    w(f"{s['n_elements']} elements in {s['n_bouts']} bouts from {len(s['sources'])} recording(s): "
      + ", ".join(s["sources"]) + "\n")
    w(f"Median rate within bouts: **{rhy['elements_per_s']:.2f} elements/s** "
      f"(literature: about 3-4/s for warble). Median silent gap: {rhy['gap_ms_overall']:.0f} ms.\n")

    w("## Categories\n")
    w("| | Category | n | share | duration ms (25/50/75%) | f0 Hz (25/50/75%) | AM Hz | level dB | resynth dist dB |")
    w("|---|---|---|---|---|---|---|---|---|")
    for c, v in s["categories"].items():
        d, f = v["dur_ms"], v["f0_hz"]
        w(f"| {c} | {v['name']} | {v['count']} | {100 * v['share']:.0f}% | "
          f"{d[0]:.0f} / {d[1]:.0f} / {d[2]:.0f} | {f[0]:.0f} / {f[1]:.0f} / {f[2]:.0f} | "
          f"{v['am_rate_hz_median']:.0f} | {v['level_db_median']:.1f} | {v['resynth_dist_db_median']:.1f} |")
    w("\nResynth distance: RMS dB difference between band spectra of original and "
      "resynthesised element (lower is better; only with `--resynth`).\n")

    hand = [m for m in els if m["hand_label"] and m["hand_label"] in CATEGORIES]
    if hand:
        agree = sum(m["hand_label"] == m["rule_label"] for m in hand)
        w("## Rules vs hand labels\n")
        w(f"{agree}/{len(hand)} hand-labelled elements ({100 * agree / len(hand):.0f}%) match the rule label. "
          "Rows: hand label; columns: rule label.\n")
        w(_crosstab(hand, "hand_label", "rule_label", CATEGORIES, CATEGORIES))

    if clusters:
        w("## Unsupervised clusters vs labels\n")
        w("Do the recordings fall into groups that match the categories? "
          "Rows: label; columns: k-means cluster.\n")
        w(_crosstab(els, "label", "cluster", CATEGORIES, list(range(clusters))))

    w("## Transitions (first order)\n")
    w("Row: current element; column: next element; values are percentages of the row.\n")
    t = np.array(s["transitions"])
    w("| | " + " | ".join(CATEGORIES) + " |")
    w("|---" * (len(CATEGORIES) + 1) + "|")
    for i, c in enumerate(CATEGORIES):
        tot = t[i].sum()
        w(f"| **{c}** | " + " | ".join(f"{100 * v / tot:.0f}" if tot else "-" for v in t[i]) + " |")

    w("\n## How far back does order matter?\n")
    w("Conditional entropy of the next element given the previous n-1. A drop from "
      "one row to the next means longer contexts help predict what comes next. "
      "Rows marked * have too few samples per context to trust.\n")
    w("| n | H (bits) | contexts | samples |")
    w("|---|---|---|---|")
    for r in s["conditional_entropy"]:
        w(f"| {r['order']} | {r['entropy_bits']:.3f}{'' if r['reliable'] else ' *'} | {r['contexts']} | {r['samples']} |")
    w(f"\nThe exported model keeps {len(s['contexts'])} contexts "
      f"(longest {max(len(r['context']) for r in s['contexts'])}).\n")

    w("## Rhythm\n")
    w("IOI ratio r = IOI_k / (IOI_k + IOI_k+1). Peaks at 0.5 mean even pulses; "
      "peaks at 0.33/0.67 mean 1:2 or 2:1 patterns.\n")
    w("```")
    h = rhy["ioi_ratio_hist"]
    e = rhy["ioi_ratio_edges"]
    top = max(h) or 1
    for i, v in enumerate(h):
        w(f"{e[i]:.2f}-{e[i + 1]:.2f} {'#' * int(round(40 * v / top))} {v}")
    w("```\n")
    return "\n".join(L) + "\n"


def _crosstab(els, row_key, col_key, rows, cols):
    L = ["| | " + " | ".join(str(c) for c in cols) + " |", "|---" * (len(cols) + 1) + "|"]
    for r in rows:
        counts = [sum(1 for m in els if m[row_key] == r and m.get(col_key) == c) for c in cols]
        if sum(counts):
            L.append(f"| **{r}** | " + " | ".join(str(v) for v in counts) + " |")
    return "\n".join(L) + "\n"
