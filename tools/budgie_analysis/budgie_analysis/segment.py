"""Split a recording into elements (sounds separated by silence) and bouts."""

import numpy as np
from scipy.signal import butter, sosfiltfilt


def bandpass(x, sr, cfg):
    hi = min(cfg["lowpass_hz"], 0.45 * sr)
    sos = butter(4, [cfg["highpass_hz"], hi], btype="bandpass", fs=sr, output="sos")
    return sosfiltfilt(sos, x)


def envelope_db(x, sr, cfg):
    """RMS envelope in dB, one value per hop. Returns (env_db, hop_samples)."""
    hop = max(1, int(round(sr * cfg["hop_ms"] / 1000)))
    win = max(hop, int(round(sr * cfg["env_win_ms"] / 1000)))
    n = 1 + max(0, len(x) - win) // hop
    c = np.concatenate([[0.0], np.cumsum(x * x)])
    starts = np.arange(n) * hop
    rms = np.sqrt((c[starts + win] - c[starts]) / win)
    return 20 * np.log10(rms + 1e-12), hop


def find_elements(x, sr, cfg):
    """Return a list of (start_sample, end_sample) for each element.

    Hysteresis thresholds relative to an estimated noise floor, then gaps
    shorter than merge_gap_ms are bridged.
    """
    env, hop = envelope_db(x, sr, cfg)
    if len(env) == 0:
        return []
    floor = np.percentile(env, cfg["floor_percentile"])
    on, off = floor + cfg["on_db"], floor + cfg["off_db"]

    runs = []
    active = False
    for i, v in enumerate(env):
        if not active and v > on:
            # walk back down the rising edge, stopping at the off threshold or
            # where the level stops falling (so noise doesn't drag the onset early)
            j = i
            while (j > 0 and off < env[j - 1] < env[j]
                   and (not runs or j - 1 > runs[-1][1])):
                j -= 1
            start, active = j, True
        elif active and v < off:
            runs.append([start, i])
            active = False
    if active:
        runs.append([start, len(env)])

    merge = cfg["merge_gap_ms"] / cfg["hop_ms"]
    merged = []
    for r in runs:
        if merged and r[0] - merged[-1][1] < merge:
            merged[-1][1] = r[1]
        else:
            merged.append(r)

    win = int(round(sr * cfg["env_win_ms"] / 1000))
    pad = int(round(sr * cfg["pad_ms"] / 1000))
    min_len = sr * cfg["min_dur_ms"] / 1000
    out = []
    for a, b in merged:
        # frame i covers [i*hop, i*hop + win); use frame centres as the edges
        s = max(0, a * hop + win // 2 - pad)
        e = min(len(x), b * hop + win // 2 + pad)
        if e - s >= min_len:
            out.append((s, e))
    return out


def bouts(elements, sr, bout_gap_s):
    """Group element indices into bouts separated by long silences."""
    groups, cur = [], []
    for i, (s, e) in enumerate(elements):
        if cur and (s - elements[cur[-1]][1]) / sr > bout_gap_s:
            groups.append(cur)
            cur = []
        cur.append(i)
    if cur:
        groups.append(cur)
    return groups
