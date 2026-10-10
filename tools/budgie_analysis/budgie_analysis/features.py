"""Per-element measurements, in the same terms the synth engine uses.

Each element becomes a dict with:
  timing      onset_s, offset_s, dur_ms
  loudness    peak_db (absolute), level_db (relative to the file's loud elements)
  pitch       voiced_frac, f0_median/min/max, fm_extent_st, and an f0 contour
              of up to N breakpoints (f0_t as fraction of duration, f0_hz)
  envelope    amp_t / amp breakpoints (linear, normalised to the element peak)
  modulation  am_rate_hz, am_depth (0..1)
  timbre      harm (relative amplitudes of harmonics 1..N), noise_mix (0..1)
  spectrum    centroid_hz, bandwidth_hz, flatness
"""

import numpy as np
from scipy.signal import hilbert, medfilt, welch

from . import pitch as pitch_mod


def simplify(t, y, k):
    """Choose k points (always including both ends) that best describe y(t)
    as a piecewise-linear curve, adding the worst-fitting point each time."""
    n = len(t)
    if n <= k:
        return list(t), list(y)
    keep = [0, n - 1]
    while len(keep) < k:
        keep.sort()
        approx = np.interp(t, t[keep], y[keep])
        err = np.abs(y - approx)
        err[keep] = -1
        keep.append(int(np.argmax(err)))
    keep.sort()
    return list(t[keep]), list(y[keep])


def measure(x, sr, start, end, cfg):
    pc, fc = cfg["pitch"], cfg["features"]
    hop = max(1, int(round(sr * cfg["segment"]["hop_ms"] / 1000)))
    seg = x[start:end]
    dur = (end - start) / sr
    out = {
        "onset_s": start / sr,
        "offset_s": end / sr,
        "dur_ms": 1000 * dur,
    }

    # Loudness envelope (1 ms hop, 2 ms window)
    win = max(hop, int(round(sr * cfg["segment"]["env_win_ms"] / 1000)))
    c = np.concatenate([[0.0], np.cumsum(seg * seg)])
    starts = np.arange(0, max(1, len(seg) - win + 1), hop)
    rms = np.sqrt(np.maximum(c[np.minimum(starts + win, len(seg))] - c[starts], 0) / win)
    peak = float(rms.max()) if len(rms) else 1e-12
    out["peak_db"] = 20 * np.log10(peak + 1e-12)
    env_t = (starts + win / 2) / sr / dur
    amp_t, amp = simplify(np.clip(env_t, 0, 1), rms / (peak + 1e-12), fc["amp_points"])
    out["amp_t"] = [float(v) for v in amp_t]
    out["amp"] = [float(v) for v in amp]

    # Pitch: analyse with some context either side, keep frames inside the element
    pw = int(round(sr * pc["win_ms"] / 1000))
    tau_max = int(np.ceil(sr / pc["fmin_hz"]))
    a = max(0, start - pw // 2)
    b = min(len(x), end + tau_max)
    times, f0, aper = pitch_mod.yin(x[a:b], sr, pc["fmin_hz"], pc["fmax_hz"],
                                    pw, hop, pc["threshold"])
    times = times + a / sr - start / sr
    inside = (times >= 0) & (times <= dur)
    times, f0, aper = times[inside], f0[inside], aper[inside]
    # frame loudness, to ignore near-silent frames at the edges
    if len(times):
        fl = np.interp(times / dur, np.clip(env_t, 0, 1), rms / (peak + 1e-12))
        loud = fl > 10 ** (-30 / 20)
    else:
        loud = np.zeros(0, bool)
    voiced = loud & np.isfinite(f0) & (aper < pc["voiced_max_aperiodicity"])
    out["voiced_frac"] = float(voiced.sum() / max(1, loud.sum()))
    out["noise_mix"] = float(np.clip((np.median(aper[loud]) - 0.05) / 0.5, 0, 1)) if loud.any() else 1.0

    if voiced.sum() >= 3:
        lf = np.log2(f0[voiced])
        if len(lf) >= 3:
            lf = medfilt(lf, 3)
            lf[0], lf[-1] = lf[1], lf[-2]
        vt = times[voiced] / dur
        ft, fv = simplify(vt, lf, fc["f0_points"])
        out["f0_t"] = [float(v) for v in ft]
        out["f0_hz"] = [float(2 ** v) for v in fv]
        out["f0_median"] = float(2 ** np.median(lf))
        out["f0_min"] = float(2 ** lf.min())
        out["f0_max"] = float(2 ** lf.max())
        out["fm_extent_st"] = float(12 * (lf.max() - lf.min()))
        out["harm"] = _harmonics(seg, sr, times[voiced], f0[voiced], fc["harmonics"])
    else:
        out["f0_t"], out["f0_hz"] = [], []
        out["f0_median"] = out["f0_min"] = out["f0_max"] = 0.0
        out["fm_extent_st"] = 0.0
        out["harm"] = [0.0] * fc["harmonics"]

    out["am_rate_hz"], out["am_depth"] = _am(seg, sr, cfg, out)
    out.update(_spectrum(seg, sr))
    return out


def _harmonics(seg, sr, times, f0, n):
    """Mean relative amplitude of harmonics 1..n over voiced frames."""
    w = int(0.008 * sr)
    nfft = 2048
    win = np.hanning(w)
    acc = np.zeros(n)
    for t, f in zip(times, f0):
        c = int(t * sr)
        fr = seg[max(0, c - w // 2): c + w // 2]
        if len(fr) < w:
            fr = np.pad(fr, (0, w - len(fr)))
        mag = np.abs(np.fft.rfft(fr * win, nfft))
        for k in range(n):
            fk = (k + 1) * f
            if fk >= sr / 2:
                break
            b = int(round(fk * nfft / sr))
            acc[k] += mag[max(0, b - 1): b + 2].max()
    m = acc.max()
    return [float(v / m) if m > 0 else 0.0 for v in acc]


def _am(seg, sr, cfg, m):
    fc = cfg["features"]
    if m["dur_ms"] < fc["am_min_dur_ms"] or m["voiced_frac"] < 0.3:
        return 0.0, 0.0
    env = np.abs(hilbert(seg))
    k = max(1, int(0.02 * sr))
    trend = np.convolve(env, np.ones(k) / k, mode="same")
    lo, hi = int(0.1 * len(env)), int(0.9 * len(env))
    r = env[lo:hi] / (trend[lo:hi] + 1e-12) - 1.0
    if len(r) < 32:
        return 0.0, 0.0
    w = np.hanning(len(r))
    nfft = 1 << int(np.ceil(np.log2(max(len(r), sr // 2))))
    spec = np.abs(np.fft.rfft((r - r.mean()) * w, nfft))
    freqs = np.fft.rfftfreq(nfft, 1 / sr)
    top = fc["am_max_hz"]
    if m["f0_median"] > 0:
        top = min(top, 0.5 * m["f0_median"])
    band = (freqs >= fc["am_min_hz"]) & (freqs <= top)
    if not band.any():
        return 0.0, 0.0
    i = np.argmax(np.where(band, spec, 0))
    depth = 2 * spec[i] / w.sum()
    return float(freqs[i]), float(np.clip(depth, 0, 1))


def _spectrum(seg, sr):
    f, p = welch(seg, sr, nperseg=min(512, len(seg)))
    band = (f >= 300) & (f <= 16000)
    f, p = f[band], p[band] + 1e-20
    tot = p.sum()
    centroid = float((f * p).sum() / tot)
    bw = float(np.sqrt(((f - centroid) ** 2 * p).sum() / tot))
    flat = float(np.exp(np.mean(np.log(p))) / np.mean(p))
    return {"centroid_hz": centroid, "bandwidth_hz": bw, "flatness": flat}
