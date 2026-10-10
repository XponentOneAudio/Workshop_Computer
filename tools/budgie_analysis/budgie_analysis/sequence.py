"""Sequence statistics: what follows what, how far back it matters, and rhythm.

Input is a list of bouts; each bout is a list of elements (dicts with
'label', 'onset_s', 'offset_s') in time order.
"""

from collections import Counter, defaultdict

import numpy as np

from .config import CATEGORIES


def ngram_counts(seqs, order):
    """Counts of (context tuple of length order-1, next symbol)."""
    c = defaultdict(Counter)
    for s in seqs:
        for i in range(order - 1, len(s)):
            c[tuple(s[i - order + 1: i])][s[i]] += 1
    return c


def conditional_entropy(seqs, max_order):
    """H(next | previous n-1 symbols) for n = 1..max_order, in bits.

    If H keeps falling as n grows, longer contexts carry information - the
    'higher-order Markov' structure reported for real warble. Estimates are
    biased low when samples are few relative to contexts, so each row also
    reports the counts.
    """
    rows = []
    for n in range(1, max_order + 1):
        c = ngram_counts(seqs, n)
        total = sum(sum(v.values()) for v in c.values())
        if total == 0:
            break
        h = 0.0
        for nxt in c.values():
            t = sum(nxt.values())
            p = np.array(list(nxt.values()), float) / t
            h -= (t / total) * (p * np.log2(p)).sum()
        rows.append({"order": n, "entropy_bits": float(h), "contexts": len(c), "samples": total,
                     "reliable": total >= 20 * len(c)})
    return rows


def _dist(counter):
    v = np.array([counter.get(s, 0) for s in CATEGORIES], float) + 0.1  # light smoothing
    return v / v.sum()


def _kl(p, q):
    return float((p * np.log2(p / q)).sum())


def variable_order_model(seqs, cfg):
    """Contexts worth keeping, longest first.

    A context of length k is kept when it has been seen min_context_count
    times and its next-symbol distribution differs from that of its own
    (k-1)-length suffix by at least min_kl_bits. The firmware looks up the
    longest matching context and falls back to shorter ones.
    """
    sc = cfg["sequence"]
    uni = Counter(x for s in seqs for x in s)
    model = [{"context": "", "dist": _dist(uni).tolist(), "count": sum(uni.values())}]
    dists = {(): _dist(uni)}
    for k in range(1, sc["max_order"] + 1):
        for ctx, nxt in ngram_counts(seqs, k + 1).items():
            n = sum(nxt.values())
            if n < sc["min_context_count"]:
                continue
            d = _dist(nxt)
            parent = dists.get(ctx[1:])
            if parent is None:
                continue
            dists[ctx] = d
            if k == 1 or _kl(d, parent) >= sc["min_kl_bits"]:
                model.append({"context": "".join(ctx), "dist": d.tolist(), "count": n})
    # longest contexts first, most-seen first; keep the unigram fallback always
    model.sort(key=lambda r: (-len(r["context"]), -r["count"]))
    if len(model) > sc["max_contexts"]:
        model = model[: sc["max_contexts"] - 1] + [r for r in model if r["context"] == ""]
    return model


def transition_matrix(seqs):
    m = np.zeros((len(CATEGORIES), len(CATEGORIES)))
    for s in seqs:
        for a, b in zip(s, s[1:]):
            m[CATEGORIES.index(a), CATEGORIES.index(b)] += 1
    return m


def rhythm(bouts, cfg):
    """Inter-onset intervals, IOI ratios r = IOI_k / (IOI_k + IOI_k+1), and
    median silent gap (offset to next onset) for each category pair."""
    iois, ratios = [], []
    gaps = defaultdict(list)
    for b in bouts:
        on = np.array([e["onset_s"] for e in b])
        d = np.diff(on)
        iois.extend(d.tolist())
        if len(d) >= 2:
            ratios.extend((d[:-1] / (d[:-1] + d[1:])).tolist())
        for e1, e2 in zip(b, b[1:]):
            gaps[(e1["label"], e2["label"])].append(e2["onset_s"] - e1["offset_s"])
    nb = cfg["sequence"]["ioi_ratio_bins"]
    hist, edges = np.histogram(ratios, bins=nb, range=(0, 1))
    gap_ms = [[float(1000 * np.median(gaps[(a, b)])) if gaps[(a, b)] else None
               for b in CATEGORIES] for a in CATEGORIES]
    all_gaps = [g for v in gaps.values() for g in v]
    return {
        "ioi_s": iois,
        "ioi_ratio_hist": hist.tolist(),
        "ioi_ratio_edges": edges.tolist(),
        "gap_ms_median": gap_ms,
        "gap_ms_overall": float(1000 * np.median(all_gaps)) if all_gaps else 0.0,
        "elements_per_s": float(1 / np.median(iois)) if iois else 0.0,
    }
