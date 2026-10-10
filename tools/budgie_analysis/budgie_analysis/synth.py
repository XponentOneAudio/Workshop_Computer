"""Reference model of the card's syrinx voice, and element templates.

This is the Python twin of the planned firmware voice: a pitch contour
driving a sine plus a few harmonics, amplitude-modulated, mixed with
band-pass noise, under a breakpoint envelope. Templates go through the same
quantisation as the generated C header, so what you hear from `--resynth` is
what the card would get.
"""

import numpy as np
from scipy.signal import lfilter

N_F0 = 5
N_AMP = 4
N_HARM = 4


def _u8(v):
    return int(np.clip(round(v * 255), 0, 255))


def _u16(v):
    return int(np.clip(round(v), 0, 65535))


def quantise(m):
    """Element measurements -> integer template (the C struct's fields)."""
    nf = len(m["f0_t"])
    q = {
        "category": m["label"],
        "dur_ms": _u16(m["dur_ms"]),
        "n_f0": nf,
        "f0_t": [_u8(t) for t in m["f0_t"]] + [0] * (N_F0 - nf),
        "f0_hz": [_u16(f) for f in m["f0_hz"]] + [0] * (N_F0 - nf),
        "amp_t": [_u8(t) for t in m["amp_t"]] + [255] * (N_AMP - len(m["amp_t"])),
        "amp": [_u8(a) for a in m["amp"]] + [0] * (N_AMP - len(m["amp"])),
        "am_rate_hz": _u16(m["am_rate_hz"]),
        "am_depth": _u8(m["am_depth"]),
        "harm": [_u8(h) for h in m["harm"][:N_HARM]] + [0] * max(0, N_HARM - len(m["harm"])),
        "noise_mix": _u8(m["noise_mix"] if nf else 1.0),
        "noise_centre_hz": _u16(m["centroid_hz"]),
        "noise_bw_hz": _u16(m["bandwidth_hz"]),
        "level": _u8(10 ** (min(0.0, m["level_db"]) / 20)),
    }
    return q


NOISE_MIN_HZ, NOISE_MAX_HZ = 200, 16000
NOISE_MIN_BW = 200
NOISE_Q_RANGE = (0.3, 20.0)


def noise_filter(centre, bw, sr):
    """RBJ band-pass (0 dB peak) at `centre` with bandwidth `bw`, plus the gain
    that brings uniform [-1, 1] noise through it to the RMS of a unit sine.
    The firmware uses exactly this design."""
    c = float(np.clip(centre, NOISE_MIN_HZ, NOISE_MAX_HZ))
    bw = float(max(NOISE_MIN_BW, bw))
    Q = float(np.clip(c / bw, *NOISE_Q_RANGE))
    w0 = 2 * np.pi * c / sr
    alpha = np.sin(w0) / (2 * Q)
    a0 = 1 + alpha
    b = np.array([alpha, 0.0, -alpha]) / a0
    a = np.array([1.0, -2 * np.cos(w0) / a0, (1 - alpha) / a0])
    # noise bandwidth of this filter is (pi/2)(c/Q); uniform noise has variance 1/3
    g = np.sqrt(1.5 * sr / (np.pi * c / Q))
    return b, a, g


def render(q, sr, rng, gain=None):
    """Render one quantised template. gain overrides the template's level."""
    n = max(1, int(round(q["dur_ms"] * sr / 1000)))
    tf = np.arange(n) / n
    nf = q["n_f0"]
    tonal = np.zeros(n)
    if nf:
        ft = np.array(q["f0_t"][:nf]) / 255
        fl = np.log2(np.maximum(1, np.array(q["f0_hz"][:nf], float)))
        f0 = 2 ** np.interp(tf, ft, fl)
        phase = 2 * np.pi * np.cumsum(f0) / sr
        harm = np.array(q["harm"], float) / 255
        for k, h in enumerate(harm):
            if h > 0:
                tonal += h * np.sin((k + 1) * phase) * ((k + 1) * f0 < 0.45 * sr)
        if harm.sum() > 0:
            tonal /= harm.sum()
        d = q["am_depth"] / 255
        if d > 0 and q["am_rate_hz"] > 0:
            t = np.arange(n) / sr
            tonal *= (1 + d * np.sin(2 * np.pi * q["am_rate_hz"] * t)) / (1 + d)
    mix = q["noise_mix"] / 255 if nf else 1.0
    noise = np.zeros(n)
    if mix > 0:
        b, a, g = noise_filter(q["noise_centre_hz"], q["noise_bw_hz"], sr)
        noise = g * lfilter(b, a, rng.uniform(-1, 1, n))
    sig = (1 - mix) * tonal + mix * noise
    env = np.interp(tf, np.array(q["amp_t"]) / 255, np.array(q["amp"], float) / 255)
    fade = min(n // 2, int(0.0005 * sr))
    if fade:
        ramp = np.linspace(0, 1, fade)
        env[:fade] *= ramp
        env[-fade:] *= ramp[::-1]
    g = q["level"] / 255 if gain is None else gain
    return g * env * sig


def resynthesise(elements, length, sr, seed=0):
    """Place each element's rendered template at its original onset and loudness."""
    rng = np.random.default_rng(seed)
    out = np.zeros(length)
    for m in elements:
        q = quantise(m)
        # the envelope peaks at 1; scale so the RMS peak matches the original
        y = render(q, sr, rng, gain=np.sqrt(2) * 10 ** (m["peak_db"] / 20))
        s = int(round(m["onset_s"] * sr))
        e = min(length, s + len(y))
        out[s:e] += y[: e - s]
    return out


def band_energies(x, sr, n_bands=24, lo=400.0, hi=16000.0):
    hi = min(hi, 0.45 * sr)
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1 / sr)
    edges = np.geomspace(lo, hi, n_bands + 1)
    e = np.array([spec[(f >= a) & (f < b)].sum() for a, b in zip(edges, edges[1:])])
    return np.maximum(10 * np.log10(e / (e.sum() + 1e-20) + 1e-12), -60.0)


def spectral_distance_db(orig, resyn, sr):
    """RMS difference (dB) between level-normalised band spectra: 0 = identical shape."""
    if len(orig) < 64:
        return float("nan")
    a, b = band_energies(orig, sr), band_energies(resyn, sr)
    # ignore bands more than 30 dB below the strongest, where neither has much energy
    top = max(a.max(), b.max())
    a, b = np.maximum(a, top - 30), np.maximum(b, top - 30)
    return float(np.sqrt(np.mean((a - b) ** 2)))
