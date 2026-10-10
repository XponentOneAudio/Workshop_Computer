"""Analysis settings.

Every threshold here is a starting guess, to be calibrated against
hand-labelled recordings (see README, "Calibrating"). Override any of them
with --config my.json, which is merged over these defaults.
"""

import copy
import json

CATEGORIES = "ABCDEFG"
CATEGORY_NAMES = {
    "A": "alarm-like",
    "B": "contact-like",
    "C": "long harmonic",
    "D": "short harmonic",
    "E": "noisy",
    "F": "click",
    "G": "soft call",
}

DEFAULTS = {
    "sample_rate": 48000,
    "segment": {
        "highpass_hz": 400.0,       # cuts handling noise and room rumble
        "lowpass_hz": 16000.0,
        "env_win_ms": 2.0,          # RMS window
        "hop_ms": 1.0,              # analysis hop for envelope and pitch
        "floor_percentile": 10.0,   # noise floor estimate
        "on_db": 12.0,              # start an element this far above floor
        "off_db": 6.0,              # end it when it falls below this
        "merge_gap_ms": 8.0,        # gaps shorter than this don't split
        "min_dur_ms": 2.0,
        "pad_ms": 1.0,              # widen each element a little at both ends
        "bout_gap_s": 1.0,          # silences longer than this start a new bout
    },
    "pitch": {
        "fmin_hz": 400.0,
        "fmax_hz": 6000.0,
        "win_ms": 8.0,
        "threshold": 0.15,          # YIN CMNDF threshold
        "voiced_max_aperiodicity": 0.25,
    },
    "features": {
        "f0_points": 5,
        "amp_points": 4,
        "harmonics": 4,
        "am_min_hz": 15.0,
        "am_max_hz": 1000.0,
        "am_min_dur_ms": 30.0,
    },
    "classify": {
        "click_max_ms": 12.0,
        "noisy_max_voiced_frac": 0.35,
        "noisy_min_flatness": 0.30,
        "alarm_min_ms": 60.0,
        "alarm_min_level_db": -10.0,
        "soft_max_level_db": -15.0,
        "contact_f0_hz": [1800.0, 5000.0],
        "contact_dur_ms": [50.0, 300.0],
        "contact_max_h2": 0.5,      # contact calls are close to a single tone
        "long_harmonic_min_ms": 100.0,
    },
    "sequence": {
        "max_order": 5,
        "min_context_count": 5,
        "min_kl_bits": 0.05,        # keep a longer context only if it says something new
        "max_contexts": 512,
        "ioi_ratio_bins": 20,
    },
    "export": {
        "max_templates_per_category": 32,
    },
}


def load(path=None):
    cfg = copy.deepcopy(DEFAULTS)
    if path:
        with open(path) as f:
            _merge(cfg, json.load(f))
    return cfg


def _merge(base, over):
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(base.get(k), dict):
            _merge(base[k], v)
        else:
            base[k] = v
