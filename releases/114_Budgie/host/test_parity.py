"""Check the firmware voice (budgie_voice.h, built on the host) against the
Python reference voice (tools/budgie_analysis synth.render), template by
template.

Tonal templates should match sample for sample (bar 16-sample pitch
blocks and integer rounding). Noisy ones use different random numbers, so
those are compared on band spectrum and loudness instead.

Run with `make test` from this directory.
"""

import json
import os
import struct
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools", "budgie_analysis"))
from budgie_analysis import synth  # noqa: E402

SR = 48000
OUT_SCALE = 1600  # kOutScale in budgie_voice.h


def soft_clip(x):
    """SoftClip() in budgie_voice.h: the DAC stage, after the voice proper."""
    a = np.abs(x)
    e = a - 1536
    knee = np.where(e >= 1024, 2047, 1536 + e - e * e / 2048)
    return np.sign(x) * np.minimum(np.where(a > 1536, knee, a), 2047)


def firmware_renders():
    path = os.path.join(HERE, "render.bin")
    subprocess.run([os.path.join(HERE, "render"), path], check=True)
    out = []
    with open(path, "rb") as f:
        while True:
            h = f.read(4)
            if not h:
                break
            (n,) = struct.unpack("<i", h)
            out.append(np.frombuffer(f.read(2 * n), dtype="<i2").astype(float))
    os.remove(path)
    return out


def best_corr(a, b, max_shift=2):
    n = min(len(a), len(b)) - 2 * max_shift
    best = -1.0
    for s in range(-max_shift, max_shift + 1):
        x = a[max_shift: max_shift + n]
        y = b[max_shift + s: max_shift + s + n]
        d = np.linalg.norm(x) * np.linalg.norm(y)
        if d > 0:
            best = max(best, float(np.dot(x, y) / d))
    return best


def main():
    with open(os.path.join(HERE, "..", "model.json")) as f:
        templates = json.load(f)["templates"]
    fw = firmware_renders()
    assert len(fw) == len(templates), (len(fw), len(templates))
    rng = np.random.default_rng(0)
    fails = []
    noisy = {}
    worst = {"corr": 1.0, "dist": 0.0, "rms_db": 0.0}
    for i, (q, y) in enumerate(zip(templates, fw)):
        ref = soft_clip(OUT_SCALE * synth.render(q, SR, rng))
        tag = f"#{i} {q['category']}"
        if abs(len(y) - len(ref)) > 2:
            fails.append(f"{tag}: length {len(y)} vs {len(ref)}")
            continue
        n = min(len(y), len(ref))
        y, ref = y[:n], ref[:n]
        rms_y, rms_r = np.sqrt(np.mean(y ** 2)), np.sqrt(np.mean(ref ** 2))
        if rms_r < 2:  # below one DAC count: nothing to compare
            continue
        rms_db = 20 * np.log10((rms_y + 1e-9) / rms_r)
        worst["rms_db"] = max(worst["rms_db"], abs(rms_db))
        tonal = q["n_f0"] > 0 and q["noise_mix"] < 10
        if tonal:
            c = best_corr(y, ref)
            worst["corr"] = min(worst["corr"], c)
            if c < 0.98:
                fails.append(f"{tag}: waveform correlation {c:.3f}")
        if not tonal:
            # different random numbers: judge the noise on category averages below
            noisy.setdefault(q["category"], []).append((y, ref, rms_db))
        if n >= 256:
            d = synth.spectral_distance_db(y, ref, SR)
            worst["dist"] = max(worst["dist"], d)
            if d > (2.0 if tonal else 6.0):
                fails.append(f"{tag}: spectral distance {d:.1f} dB")
        if abs(rms_db) > (1.0 if tonal else 3.0):
            fails.append(f"{tag}: loudness differs by {rms_db:+.1f} dB")
    for cat, items in sorted(noisy.items()):
        # energy spectra summed over every element in the category
        spec = lambda k: sum(np.abs(np.fft.rfft(it[k], 4096)) ** 2 for it in items)
        a, b = spec(0), spec(1)
        f = np.fft.rfftfreq(4096, 1 / SR)
        edges = np.geomspace(400, 16000, 25)
        ba = np.array([a[(f >= lo) & (f < hi)].sum() for lo, hi in zip(edges, edges[1:])])
        bb = np.array([b[(f >= lo) & (f < hi)].sum() for lo, hi in zip(edges, edges[1:])])
        da = 10 * np.log10(ba / ba.sum() + 1e-12)
        db = 10 * np.log10(bb / bb.sum() + 1e-12)
        top = max(da.max(), db.max())
        d = float(np.sqrt(np.mean((np.maximum(da, top - 30) - np.maximum(db, top - 30)) ** 2)))
        loud = float(np.mean([it[2] for it in items]))
        print(f"noise, category {cat} ({len(items)} templates): mean spectrum distance {d:.2f} dB, "
              f"mean loudness error {loud:+.2f} dB")
        if d > 1.5 or abs(loud) > 0.75:
            fails.append(f"category {cat}: noise does not match on average")
    print(f"{len(templates)} templates; worst tonal correlation {worst['corr']:.4f}, "
          f"worst spectral distance {worst['dist']:.2f} dB, worst loudness error {worst['rms_db']:.2f} dB")
    for f in fails:
        print("FAIL", f)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
