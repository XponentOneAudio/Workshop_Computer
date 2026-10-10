"""YIN fundamental-frequency tracker (de Cheveigne & Kawahara 2002), vectorised."""

import numpy as np


def yin(x, sr, fmin, fmax, win, hop, threshold):
    """Track f0 over x.

    Returns (times_s, f0_hz, aperiodicity). Frame i analyses
    x[i*hop : i*hop + win + tau_max]; its time is the centre of the first
    `win` samples. f0 is NaN where no period was found below `threshold`.
    """
    tau_min = max(2, int(sr / fmax))
    tau_max = int(np.ceil(sr / fmin))
    L = win + tau_max
    if len(x) < L:
        x = np.pad(x, (0, L - len(x)))
    n = 1 + (len(x) - L) // hop
    idx = np.arange(L)[None, :] + hop * np.arange(n)[:, None]
    fr = x[idx]

    nfft = 1 << int(np.ceil(np.log2(L + win)))
    A = np.fft.rfft(fr[:, :win], nfft)
    B = np.fft.rfft(fr, nfft)
    corr = np.fft.irfft(np.conj(A) * B, nfft)[:, : tau_max + 1]

    e = np.concatenate([np.zeros((n, 1)), np.cumsum(fr * fr, axis=1)], axis=1)
    taus = np.arange(tau_max + 1)
    d = (e[:, win] - e[:, 0])[:, None] + (e[:, win + taus] - e[:, taus]) - 2 * corr
    d = np.maximum(d, 0.0)

    cum = np.cumsum(d[:, 1:], axis=1)
    cmndf = np.ones_like(d)
    with np.errstate(divide="ignore", invalid="ignore"):
        cmndf[:, 1:] = np.where(cum > 0, d[:, 1:] * taus[1:] / cum, 1.0)

    f0 = np.full(n, np.nan)
    aper = np.ones(n)
    for i in range(n):
        c = cmndf[i]
        below = np.nonzero(c[tau_min:tau_max] < threshold)[0]
        if len(below):
            t = tau_min + below[0]
            while t + 1 < tau_max and c[t + 1] < c[t]:
                t += 1
        else:
            t = tau_min + int(np.argmin(c[tau_min:tau_max]))
        aper[i] = c[t]
        if c[t] >= threshold:
            continue
        # parabolic interpolation on d for sub-sample period
        if 1 <= t < tau_max:
            a, b, g = d[i, t - 1], d[i, t], d[i, t + 1]
            den = a - 2 * b + g
            shift = 0.5 * (a - g) / den if den > 0 else 0.0
        else:
            shift = 0.0
        f0[i] = sr / (t + np.clip(shift, -0.5, 0.5))

    times = (np.arange(n) * hop + win / 2) / sr
    return times, f0, aper
