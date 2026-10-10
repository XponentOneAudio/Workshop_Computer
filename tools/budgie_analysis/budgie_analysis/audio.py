"""WAV loading and writing."""

from math import gcd

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly


def load(path, sr):
    """Load a WAV as mono float64 in [-1, 1] at sample rate `sr`."""
    file_sr, x = wavfile.read(path)
    if x.dtype.kind == "i":
        x = x.astype(np.float64) / float(np.iinfo(x.dtype).max)
    elif x.dtype.kind == "u":
        x = (x.astype(np.float64) - 128.0) / 128.0
    else:
        x = x.astype(np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    if file_sr != sr:
        g = gcd(int(file_sr), int(sr))
        x = resample_poly(x, sr // g, file_sr // g)
    return x


def save(path, x, sr):
    peak = np.max(np.abs(x)) if len(x) else 0.0
    if peak > 1.0:
        x = x / peak
    wavfile.write(path, sr, (np.clip(x, -1, 1) * 32767).astype(np.int16))
