#!/usr/bin/env python3
"""Convert Neural Amp Modeler (.nam) files into a C header for the NAM Amp card.

Usage:
    python3 tools/nam2header.py model_a.nam [model_b.nam] -o model_data.h

Up to two models can be built into the card: the first plays with the switch
Up, the second with the switch in the Middle. NAM models are shared on
https://www.tone3000.com. Small LSTM models (hidden_size up to about 8) are
the best fit for the RP2040; WaveNet models, even "Nano" ones, are too slow
(see README.md).

Each model is converted to fixed point (see nam_fixed.h). This script reports
how much CPU each model needs and refuses models the RP2040 can't run in real
time. With --verify, it also builds tools/host_check.cpp with the computer's
C++ compiler and compares the fixed-point engine to a floating point
implementation of NAM, reporting the signal-to-error ratio.

Only numpy is needed.
"""

import argparse
import json
import math
import os
import re
import subprocess
import sys
import tempfile

import numpy as np

Q = 12
ONE = 1 << Q
Q_OUT = 16  # format of the model's final output
ACC_LIMIT = (1 << 31) - 1
MAX_SHIFT = 24

# Cortex-M0+ cycle costs, fitted to cycle counts of the compiled engine
# (measured by emulating the RP2040 build of nam_fixed.h instruction by
# instruction). Used to check a model will run in real time and to split it
# between the two cores.
CYC_WN_LAYER = 370      # per WaveNet layer
CYC_WN_MAC = 5.0        # per multiply-accumulate
CYC_WN_ACT = 86         # per activation function
CYC_WN_ARRAY_IN = 350   # extra for the first layer of a layer array
CYC_WN_ARRAY_OUT = 200  # extra for the last layer of a layer array
CYC_LSTM_LAYER = 400
CYC_LSTM_MAC = 4.7
CYC_LSTM_CELL = 272
CYC_LSTM_HEAD = 60

# Cycles per sample used on each core by everything else: on core 0,
# ComputerCard and the card's own input/output processing (an estimate, not
# measured); on core 1, passing samples between the cores
CORE0_OVERHEAD = 1200
CORE1_OVERHEAD = 100

SUPPORTED_ACTIVATIONS = {"Tanh": 0, "Hardtanh": 1, "ReLU": 2, "Sigmoid": 3}


class ConversionError(Exception):
    pass


################################################################################
# Floating point reference (follows NeuralAmpModelerCore)


def act_float(name, x):
    if name == "Tanh":
        return np.tanh(x)
    if name == "Hardtanh":
        return np.clip(x, -1.0, 1.0)
    if name == "ReLU":
        return np.maximum(x, 0.0)
    if name == "Sigmoid":
        return 1.0 / (1.0 + np.exp(-x))
    raise ConversionError(f"unsupported activation {name}")


def wavenet_float(model, x, peaks=None):
    """x: 1D array. Returns 1D output.

    If peaks is a list, the largest magnitude of the residual stream at the
    input to each layer, and at the output of each layer array, is appended."""
    n = len(x)
    cond = x[None, :]
    stream = cond
    head = None
    for arr in model["arrays"]:
        stream = arr["rechannel"] @ stream
        if peaks is not None:
            peaks.append(np.abs(stream).max())
        acc = np.zeros((arr["channels"], n)) if head is None else head
        for L in arr["layers"]:
            d = L["dilation"]
            z = np.zeros((L["conv"].shape[0], n))
            K = L["conv"].shape[2]
            for k in range(K):
                shift = d * (K - 1 - k)
                xs = np.zeros_like(stream)
                xs[:, shift:] = stream[:, : n - shift] if shift else stream
                z += L["conv"][:, :, k] @ xs
            z += L["conv_bias"][:, None]
            z += L["mixin"] @ cond
            C = arr["channels"]
            if L["gated"]:
                a = act_float(L["activation"], z[:C]) * act_float("Sigmoid", z[C:])
            else:
                a = act_float(L["activation"], z)
            acc = acc + a
            stream = stream + L["w1"] @ a + L["b1"][:, None]
            if peaks is not None:
                peaks.append(np.abs(stream).max())
        head = arr["head_w"] @ acc + (arr["head_b"][:, None] if arr["head_b"] is not None else 0)
    return model["head_scale"] * head[0]


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))


def lstm_float(model, x):
    layers = model["layers"]
    state = [(L["h0"].copy(), L["c0"].copy()) for L in layers]
    out = np.zeros(len(x))
    for t, xt in enumerate(x):
        inp = np.array([xt])
        for li, L in enumerate(layers):
            h, c = state[li]
            H = L["hidden"]
            g = L["w"] @ np.concatenate([inp, h]) + L["b"]
            c = sigmoid(g[H : 2 * H]) * c + sigmoid(g[:H]) * np.tanh(g[2 * H : 3 * H])
            h = sigmoid(g[3 * H :]) * np.tanh(c)
            state[li] = (h, c)
            inp = h
        out[t] = model["head_w"] @ inp + model["head_b"]
    return out


################################################################################
# Parsing .nam files


def activation_name(a):
    if isinstance(a, dict):
        a = a.get("type", a.get("name"))
    if a not in SUPPORTED_ACTIVATIONS:
        raise ConversionError(f"activation {a!r} is not supported")
    return a


def parse_wavenet(cfg, weights):
    it = iter(weights)

    def take(*shape):
        n = int(np.prod(shape))
        vals = [next(it) for _ in range(n)]
        return np.array(vals, dtype=np.float64).reshape(shape)

    if cfg.get("head") is not None:
        raise ConversionError("WaveNet post-stack head is not supported")
    if cfg.get("condition_dsp") is not None:
        raise ConversionError("condition_dsp is not supported")
    if cfg.get("in_channels", 1) != 1 or cfg.get("out_channels", 1) != 1:
        raise ConversionError("only mono models are supported")

    arrays = []
    prev_channels = None
    for li, lc in enumerate(cfg["layers"]):
        for key in ("head1x1", "layer1x1"):
            if key in lc and lc[key] is not None:
                sub = lc[key]
                active = sub.get("active", key == "layer1x1")
                if (key == "head1x1" and active) or (key == "layer1x1" and not active):
                    raise ConversionError(f"{key} setting is not supported (newer 'A2' WaveNet)")
                if sub.get("groups", 1) != 1:
                    raise ConversionError("grouped convolutions are not supported")
        for key in lc:
            if key.endswith("_film") and lc[key] not in (None, False):
                raise ConversionError("FiLM layers are not supported (newer 'A2' WaveNet)")
        if lc.get("groups_input", 1) != 1 or lc.get("groups_input_mixin", 1) != 1:
            raise ConversionError("grouped convolutions are not supported")
        C = lc["channels"]
        if lc.get("bottleneck", C) != C:
            raise ConversionError("bottleneck layers are not supported")
        if "kernel_sizes" in lc or lc.get("kernel_size") != 3:
            raise ConversionError("only kernel_size 3 is supported")
        if lc["condition_size"] != 1:
            raise ConversionError("condition_size must be 1")
        # Newer exports describe each array's head as an object; older ones
        # use head_size and head_bias
        if lc.get("head") is not None:
            hj = lc["head"]
            if hj.get("kernel_size", 1) != 1 or hj.get("head_dilation", 1) != 1:
                raise ConversionError("heads with kernel_size > 1 are not supported")
            head_size, head_bias = hj["out_channels"], hj.get("bias", False)
        else:
            head_size, head_bias = lc["head_size"], lc.get("head_bias", False)
        if isinstance(lc["activation"], list):
            raise ConversionError("per-layer activations are not supported")
        act = activation_name(lc["activation"])
        gated = lc.get("gated", False)
        if "gating_mode" in lc:
            gm = lc["gating_mode"]
            if isinstance(gm, list) or gm not in ("none", "gated", "NONE", "GATED"):
                raise ConversionError(f"gating_mode {gm!r} is not supported")
            gated = gm.lower() == "gated"
            sec = lc.get("secondary_activation", "Sigmoid")
            if gated and activation_name(sec) != "Sigmoid":
                raise ConversionError("only Sigmoid gates are supported")
        if C not in (1, 2, 3, 4, 6, 8, 12, 16) or (gated and C > 8):
            raise ConversionError(f"{C} channels{' (gated)' if gated else ''} is not supported")
        if li > 0 and lc["input_size"] != prev_channels:
            raise ConversionError("layer array input_size doesn't match previous channels")
        if li == 0 and lc["input_size"] != 1:
            raise ConversionError("first layer array must have input_size 1")
        if li + 1 < len(cfg["layers"]):
            if head_size != cfg["layers"][li + 1]["channels"]:
                raise ConversionError("head_size must match the next layer array's channels")
        elif head_size != 1:
            raise ConversionError("last head_size must be 1")

        R = 2 * C if gated else C
        arr = {"channels": C, "layers": [], "input_size": lc["input_size"]}
        arr["rechannel"] = take(C, lc["input_size"])
        for d in lc["dilations"]:
            L = {"dilation": d, "activation": act, "gated": gated}
            # Conv1D weights are flattened (out, in, kernel)
            L["conv"] = take(R, C, 3)
            L["conv_bias"] = take(R)
            L["mixin"] = take(R, 1)
            L["w1"] = take(C, C)
            L["b1"] = take(C)
            arr["layers"].append(L)
        arr["head_w"] = take(head_size, C)
        arr["head_b"] = take(head_size) if head_bias else None
        arrays.append(arr)
        prev_channels = C

    head_scale = float(next(it))
    leftover = sum(1 for _ in it)
    if leftover:
        raise ConversionError(f"{leftover} unexpected extra weights - unsupported WaveNet variant")
    return {"type": "WaveNet", "arrays": arrays, "head_scale": head_scale}


def parse_lstm(cfg, weights):
    if cfg.get("in_channels", 1) != 1 or cfg.get("out_channels", 1) != 1:
        raise ConversionError("only mono models are supported")
    if cfg["input_size"] != 1:
        raise ConversionError("LSTM input_size must be 1")
    H = cfg["hidden_size"]
    if H > 16:
        raise ConversionError(f"LSTM hidden_size {H} is too large (max 16)")
    if cfg["num_layers"] > 4:
        raise ConversionError("at most 4 LSTM layers are supported")
    w = np.array(weights, dtype=np.float64)
    pos = 0

    def take(*shape):
        nonlocal pos
        n = int(np.prod(shape))
        out = w[pos : pos + n].reshape(shape)
        pos += n
        return out

    layers = []
    for i in range(cfg["num_layers"]):
        ins = 1 if i == 0 else H
        L = {"input_size": ins, "hidden": H}
        L["w"] = take(4 * H, ins + H)
        L["b"] = take(4 * H)
        L["h0"] = take(H)
        L["c0"] = take(H)
        layers.append(L)
    head_w = take(H)
    head_b = float(take(1)[0])
    if pos != len(w):
        raise ConversionError("unexpected number of LSTM weights")
    return {"type": "LSTM", "layers": layers, "head_w": head_w, "head_b": head_b}


def load_nam(path):
    with open(path) as f:
        d = json.load(f)
    arch = d.get("architecture")
    sr = d.get("sample_rate")
    if sr not in (None, 48000, 48000.0):
        raise ConversionError(f"model sample rate is {sr}Hz; the Computer runs at 48kHz")
    if arch == "WaveNet":
        model = parse_wavenet(d["config"], d["weights"])
    elif arch == "LSTM":
        model = parse_lstm(d["config"], d["weights"])
    else:
        raise ConversionError(f"architecture {arch!r} is not supported (WaveNet and LSTM only)")
    model["metadata"] = d.get("metadata") or {}
    model["path"] = path
    return model


################################################################################
# Quantisation


def quantise_block(w, col_bounds, bias=None, col_q=None, out_q=Q):
    """Quantise weights w (rows x cols) and optional bias (rows) with a shared shift.

    col_bounds[c] is the largest integer magnitude that input column c can
    take, and col_q[c] its fixed point format (default Q12). The output, after
    the shift, is in format out_q. Picks the largest shift for which no
    accumulator can overflow. Returns (w_int, b_int (with rounding offset), shift)."""
    w = np.atleast_2d(w)
    col_bounds = np.asarray(col_bounds, dtype=np.float64)
    rows, cols = w.shape
    col_q = np.full(cols, Q) if col_q is None else np.asarray(col_q)
    w = w * 2.0 ** (out_q - col_q)[None, :]
    b = np.zeros(rows) if bias is None else np.asarray(bias, dtype=np.float64) * 2.0 ** (out_q - Q)
    for s in range(MAX_SHIFT, -1, -1):
        wi = np.round(w * (1 << s)).astype(np.int64)
        bi = np.round(b * (1 << (s + Q))).astype(np.int64)
        if s > 0:
            bi = bi + (1 << (s - 1))
        worst = (np.abs(wi) * col_bounds).sum(axis=1) + np.abs(bi)
        if worst.max() <= ACC_LIMIT and np.abs(bi).max() <= ACC_LIMIT:
            return wi, bi, s
    raise ConversionError("weights are too large to represent in fixed point")


def act_bound(act):
    return 32767 if act == "ReLU" else ONE


def stream_formats(model):
    """Fixed point format of the residual stream at the input to each layer
    and the output of each layer array, chosen from its peak level when the
    model runs on a test signal peaking at 2.0, with 2x headroom"""
    peaks = []
    x = test_signal(48000 * 3)
    wavenet_float(model, 2.0 * x / np.abs(x).max(), peaks)
    return [int(np.clip(np.floor(np.log2(32767 / (2.0 * max(p, 1e-3)))), 6, 14)) for p in peaks]


def quantise_wavenet(model):
    arrays = []
    fmt = iter(stream_formats(model))
    q_prev = Q  # format of this array's input
    for ai, arr in enumerate(model["arrays"]):
        C = arr["channels"]
        qa = {"channels": C, "layers": []}
        last = ai == len(model["arrays"]) - 1
        q_in = next(fmt)
        cols = arr["rechannel"].shape[1]
        w, b, s = quantise_block(arr["rechannel"], [32768] * cols, None, [q_prev] * cols, q_in)
        qa["rechannel"] = (w, None, s)
        for L in arr["layers"]:
            q_out = next(fmt)
            R = L["conv"].shape[0]
            # Row layout: [tap t-2d][tap t-d][tap t][mixin]
            wz = np.concatenate([L["conv"][:, :, 0], L["conv"][:, :, 1], L["conv"][:, :, 2], L["mixin"]], axis=1)
            assert wz.shape == (R, 3 * C + 1)
            wzi, bzi, sz = quantise_block(wz, [32768] * (3 * C + 1), L["conv_bias"], [q_in] * (3 * C) + [Q])
            bound = ONE if L["gated"] and L["activation"] != "ReLU" else act_bound(L["activation"])
            w1i, b1i, s1 = quantise_block(L["w1"], [bound] * C, L["b1"], None, q_out)
            qa["layers"].append(
                {
                    "dilation": L["dilation"],
                    "gated": L["gated"],
                    "activation": L["activation"],
                    "wz": wzi, "bz": bzi, "sz": sz,
                    "w1": w1i, "b1": b1i, "s1": s1,
                    "res_shift": q_out - q_in, "q_in": q_in,
                }
            )
            q_in = q_out
        q_prev = q_in
        hw, hb = arr["head_w"], arr["head_b"]
        if last:
            # Fold head_scale into the final head
            hw = hw * model["head_scale"]
            hb = None if hb is None else hb * model["head_scale"]
        w, b, s = quantise_block(hw, [1 << 17] * C, hb, None, Q_OUT if last else Q)
        qa["head"] = (w, b if hb is not None else None, s)
        arrays.append(qa)
    return arrays


def quantise_lstm(model):
    out = []
    for i, L in enumerate(model["layers"]):
        H = L["hidden"]
        # Input is x (Q12) or the previous layer's hidden state (Q14), then this
        # layer's hidden state (Q14). The gates, cell and hidden state are Q14.
        ins = L["input_size"]
        bounds = [32768 if i == 0 else 16384] * ins + [16384] * H
        col_q = [Q if i == 0 else 14] * ins + [14] * H
        w, b, s = quantise_block(L["w"], bounds, L["b"], col_q, 14)
        out.append(
            {
                "input_size": ins, "hidden": H, "w": w, "b": b, "s": s,
                "h0": np.round(L["h0"] * 16384).astype(np.int64),
                "c0": np.round(L["c0"] * 16384).astype(np.int64),
            }
        )
    H = len(model["head_w"])
    hw, hb, hs = quantise_block(model["head_w"][None, :], [16384] * H, [model["head_b"]], [14] * H, Q_OUT)
    return out, (hw, hb, hs)


################################################################################
# CPU cost


def unit_costs(model):
    """Estimated Cortex-M0+ cycles for each processing unit"""
    costs = []
    if model["type"] == "WaveNet":
        for arr in model["arrays"]:
            C = arr["channels"]
            for li, L in enumerate(arr["layers"]):
                R = L["conv"].shape[0]
                macs = R * (3 * C + 1) + C * C
                acts = C * (2 if L["gated"] else 1)
                cyc = CYC_WN_LAYER + CYC_WN_MAC * macs + CYC_WN_ACT * acts
                if li == 0:
                    cyc += CYC_WN_ARRAY_IN
                if li == len(arr["layers"]) - 1:
                    cyc += CYC_WN_ARRAY_OUT
                costs.append(cyc)
    else:
        for L in model["layers"]:
            H = L["hidden"]
            macs = 4 * H * (L["input_size"] + H)
            costs.append(CYC_LSTM_LAYER + CYC_LSTM_MAC * macs + CYC_LSTM_CELL * H)
        costs[-1] += CYC_LSTM_HEAD
    return costs


def choose_split(costs, clock_mhz):
    """Split units between core 1 ([0, split)) and core 0 ([split, n)).

    Returns (split, core1_load, core0_load) with loads as a fraction of the
    available time."""
    budget = clock_mhz * 1e6 / 48000
    avail1 = budget - CORE1_OVERHEAD
    avail0 = budget - CORE0_OVERHEAD
    best = None
    for split in range(1, len(costs) + 1):
        l1 = sum(costs[:split]) / avail1
        l0 = sum(costs[split:]) / avail0
        worst = max(l1, l0)
        if best is None or worst < best[0]:
            best = (worst, split, l1, l0)
    return best[1], best[2], best[3]


################################################################################
# Output


def c_ident(s):
    s = re.sub(r"[^0-9a-zA-Z_]", "_", s)
    if not s or s[0].isdigit():
        s = "m_" + s
    return s


def c_array(ctype, name, values, per_line=12):
    values = [int(v) for v in np.asarray(values).ravel()]
    lines = []
    for i in range(0, len(values), per_line):
        lines.append("\t" + ", ".join(str(v) for v in values[i : i + per_line]) + ",")
    return f"static const {ctype} {name}[{len(values)}] = {{\n" + "\n".join(lines) + "\n};\n"


def pad_rows(m):
    """Pad to an even number of rows with zeros"""
    if len(m) % 2:
        m = np.concatenate([m, np.zeros((1,) + m.shape[1:], dtype=m.dtype)])
    return m


def mac_layout(w):
    """Order a weight matrix for nam_fixed.h's MacMatrix: blocks of 4 rows
    (then a block of 2), each block stored column by column"""
    w = pad_rows(w)
    blocks = []
    r = 0
    while r < len(w):
        n = 4 if r + 4 <= len(w) else 2
        blocks.append(w[r : r + n].T.ravel())
        r += n
    return np.concatenate(blocks)


def dense_init(prefix, q, rows, cols):
    w, b, s = q
    return f"{{{prefix}_w, {prefix + '_b' if b is not None else 'nullptr'}, {rows}, {cols}, {s}}}"


def calibration(meta):
    """Q16 input and output gains, from the model's metadata"""
    # The Computer's audio input reads +-2048 for about +-6V.
    # NAM's 1.0 full scale corresponds to input_level_dbu, if given.
    dbu = meta.get("input_level_dbu")
    vpk = 0.7746 * math.sqrt(2) * 10 ** (dbu / 20) if dbu else 5.0
    in_gain = (6.0 / vpk) * ONE / 2048
    # Aim for a loudness of -12dB (relative to model full scale) for a
    # similar level to the input, and scale so 1.0 -> 2048 (~6V)
    loud = meta.get("loudness")
    out_db = 0.0 if loud is None else max(-12.0, min(24.0, -12.0 - loud))
    out_gain = 10 ** (out_db / 20) * 2048 / (1 << Q_OUT)
    return int(round(in_gain * 65536)), int(round(out_gain * 65536)), vpk, out_db


def emit_model(idx, model, clock_mhz):
    p = f"m{idx}"
    lines = []
    meta = model["metadata"]
    name = meta.get("name") or os.path.splitext(os.path.basename(model["path"]))[0]
    costs = unit_costs(model)
    split, l1, l0 = choose_split(costs, clock_mhz)
    in_g, out_g, vpk, out_db = calibration(meta)
    model["input_gain_q16"] = in_g
    ring_words = 0

    lines.append(f"// Model {idx}: {name}")
    lines.append(f"// from {os.path.basename(model['path'])}")
    for k in ("modeled_by", "gear_make", "gear_model", "gear_type", "tone_type"):
        if meta.get(k):
            lines.append(f"//   {k}: {meta[k]}")
    lines.append(f"// Estimated CPU at {clock_mhz}MHz: core 1 {l1*100:.0f}%, core 0 {l0*100:.0f}% (split at unit {split})")
    lines.append(f"// Input: 1.0 = {vpk:.2f}V peak. Output gain {out_db:+.1f}dB")
    lines.append("")

    if model["type"] == "WaveNet":
        qarrays = quantise_wavenet(model)
        layer_inits = []
        array_inits = []
        n = 0
        for ai, (arr, qa) in enumerate(zip(model["arrays"], qarrays)):
            C = qa["channels"]
            pr = f"{p}_a{ai}_re"
            lines.append(c_array("int32_t", f"{pr}_w", qa["rechannel"][0]))
            ph = f"{p}_a{ai}_head"
            lines.append(c_array("int32_t", f"{ph}_w", qa["head"][0]))
            if qa["head"][1] is not None:
                lines.append(c_array("int32_t", f"{ph}_b", qa["head"][1]))
            first = n
            for li, ql in enumerate(qa["layers"]):
                pl = f"{p}_l{n}"
                lines.append(c_array("int32_t", f"{pl}_wz", mac_layout(ql["wz"])))
                lines.append(c_array("int32_t", f"{pl}_bz", pad_rows(ql["bz"])))
                lines.append(c_array("int32_t", f"{pl}_w1", mac_layout(ql["w1"])))
                lines.append(c_array("int32_t", f"{pl}_b1", pad_rows(ql["b1"])))
                act = SUPPORTED_ACTIVATIONS[ql["activation"]]
                layer_inits.append(
                    f"\t{{{ql['dilation']}, {C}, {int(ql['gated'])}, {act}, {ql['sz']}, {ql['s1']}, {ql['res_shift']}, "
                    f"{pl}_wz, {pl}_bz, {pl}_w1, {pl}_b1}},"
                )
                ring_words += (2 * ql["dilation"] + 1) * C
                n += 1
            in_size = arr["rechannel"].shape[1]
            head_rows = arr["head_w"].shape[0]
            array_inits.append(
                f"\t{{{first}, {len(qa['layers'])}, {dense_init(pr, qa['rechannel'], C, in_size)}, "
                f"{dense_init(ph, qa['head'], head_rows, C)}}},"
            )
        if n > 32:
            raise ConversionError(f"{n} layers is too many (max 32)")
        lines.append(f"static const nam_fx::WNLayer {p}_layers[] = {{\n" + "\n".join(layer_inits) + "\n};\n")
        lines.append(f"static const nam_fx::WNArray {p}_arrays[] = {{\n" + "\n".join(array_inits) + "\n};\n")
        lines.append(
            f"static const nam_fx::Model {p} = {{\"{c_str(name)}\", nam_fx::kWaveNet, {n}, {split}, "
            f"{len(qarrays)}, {p}_arrays, {p}_layers, nullptr, {{nullptr, nullptr, 0, 0, 0}}, "
            f"{in_g}, {out_g}, {ring_words}}};\n"
        )
    else:
        qlayers, qhead = quantise_lstm(model)
        inits = []
        for li, ql in enumerate(qlayers):
            pl = f"{p}_lstm{li}"
            lines.append(c_array("int32_t", f"{pl}_w", mac_layout(ql["w"])))
            lines.append(c_array("int32_t", f"{pl}_b", ql["b"]))
            lines.append(c_array("int16_t", f"{pl}_h0", np.clip(ql["h0"], -32768, 32767)))
            lines.append(c_array("int32_t", f"{pl}_c0", ql["c0"]))
            inits.append(f"\t{{{ql['input_size']}, {ql['hidden']}, {ql['s']}, {pl}_w, {pl}_b, {pl}_h0, {pl}_c0}},")
        H = qlayers[-1]["hidden"]
        lines.append(c_array("int32_t", f"{p}_head_w", qhead[0]))
        lines.append(c_array("int32_t", f"{p}_head_b", qhead[1]))
        lines.append(f"static const nam_fx::LSTMLayer {p}_lstm[] = {{\n" + "\n".join(inits) + "\n};\n")
        lines.append(
            f"static const nam_fx::Model {p} = {{\"{c_str(name)}\", nam_fx::kLSTM, {len(qlayers)}, {split}, "
            f"0, nullptr, nullptr, {p}_lstm, {{{p}_head_w, {p}_head_b, 1, {H}, {qhead[2]}}}, "
            f"{in_g}, {out_g}, 0}};\n"
        )
    info = {"name": name, "split": split, "load1": l1, "load0": l0, "ring_words": ring_words, "costs": costs}
    return "\n".join(lines), info


def c_str(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


################################################################################
# Verification against the floating point reference


def test_signal(n, seed=1):
    """Guitar-ish test signal: decaying plucked tones at several levels"""
    rng = np.random.default_rng(seed)
    t = np.arange(n) / 48000.0
    x = np.zeros(n)
    pos = 0
    level = 0.05
    while pos < n:
        length = int(48000 * rng.uniform(0.15, 0.4))
        f0 = rng.uniform(80, 660)
        tt = t[: min(length, n - pos)]
        tone = sum(np.sin(2 * np.pi * f0 * k * tt + rng.uniform(0, 6)) / k**1.5 for k in range(1, 8))
        x[pos : pos + len(tt)] += level * tone * np.exp(-tt * rng.uniform(3, 10))
        pos += len(tt)
        level = min(1.0, level * 1.6) if level < 0.9 else 0.05
    return x


def snr_db(ref, err):
    return 10 * np.log10(np.mean(ref**2) / max(np.mean(err**2), 1e-20))


def verify(header, models, here, seconds):
    cxx = os.environ.get("CXX", "g++")
    n = int(48000 * seconds)
    x = test_signal(n)
    xq = np.round(x * ONE).astype(np.int32)
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "host_check")
        cmd = [cxx, "-O2", "-std=c++17", f"-I{here}", f"-I{os.path.dirname(os.path.abspath(header))}",
               f"-DMODEL_HEADER=\"{os.path.abspath(header)}\"",
               os.path.join(here, "tools", "host_check.cpp"), "-o", exe]
        subprocess.run(cmd, check=True)
        xin = os.path.join(tmp, "in.raw")
        xq.tofile(xin)
        for i, m in enumerate(models):
            yout = os.path.join(tmp, f"out{i}.raw")
            subprocess.run([exe, str(i), xin, yout], check=True)
            yq = np.fromfile(yout, dtype=np.int32) / (1 << Q_OUT)
            yf = wavenet_float(m, xq / ONE) if m["type"] == "WaveNet" else lstm_float(m, xq / ONE)
            snr = snr_db(yf, yq - yf)
            # For comparison: the error caused by a tiny amount of noise (one
            # step of the Computer's ADC) at the input. High gain models can
            # amplify this a lot.
            rng = np.random.default_rng(0)
            lsb = m["input_gain_q16"] / 65536.0 / ONE
            xn = xq / ONE + rng.uniform(-lsb / 2, lsb / 2, len(xq))
            yn = wavenet_float(m, xn) if m["type"] == "WaveNet" else lstm_float(m, xn)
            snr_noise = snr_db(yf, yn - yf)
            print(f"  model {i}: fixed point vs float: SNR {snr:.1f}dB, max error {np.abs(yq - yf).max():.4f}"
                  f" (output rms {np.sqrt(np.mean(yf**2)):.3f})")
            err_dbfs = 10 * np.log10(max(np.mean((yq - yf) ** 2), 1e-20))
            print(f"    error level {err_dbfs:.1f}dB relative to full scale; for comparison,"
                  f" 1 ADC step of input noise gives SNR {snr_noise:.1f}dB")
            # Warn if the error is louder than the Computer's own 12 bit resolution
            # and well above the error from input noise
            if err_dbfs > -66 and snr < min(40.0, snr_noise - 6.0):
                print("  WARNING: large fixed point error")
                ok = False
    return ok


################################################################################


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("models", nargs="+", help=".nam files (one or two)")
    ap.add_argument("-o", "--output", default="model_data.h")
    ap.add_argument("--clock", type=int, default=240, help="RP2040 clock in MHz (must match main.cpp)")
    ap.add_argument("--force", action="store_true", help="write the header even if a model is too slow")
    ap.add_argument("--verify", action="store_true", help="compare fixed point and float output (needs g++)")
    ap.add_argument("--seconds", type=float, default=2.0, help="length of the --verify test signal")
    args = ap.parse_args()
    if len(args.models) > 2:
        ap.error("at most two models")

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    models, parts, infos = [], [], []
    too_slow = False
    for i, path in enumerate(args.models):
        try:
            m = load_nam(path)
            part, info = emit_model(i, m, args.clock)
        except ConversionError as e:
            sys.exit(f"{path}: {e}")
        models.append(m)
        parts.append(part)
        infos.append(info)
        load = max(info["load1"], info["load0"])
        print(f"{path}: {m['type']} '{info['name']}'")
        print(f"  estimated CPU at {args.clock}MHz: core 1 {info['load1']*100:.0f}%, "
              f"core 0 {info['load0']*100:.0f}% ({sum(info['costs']):.0f} cycles/sample)")
        print(f"  history buffers: {info['ring_words'] * 2 / 1024:.1f}kB")
        if load > 1.0:
            too_slow = True
            print("  TOO SLOW: this model won't run in real time on the Computer. Small LSTM"
                  " models (hidden_size up to about 8) are the best fit; see README.md.")
    ring = sum(info["ring_words"] for info in infos)
    if ring * 2 > 160 * 1024:
        sys.exit(f"models need {ring * 2 // 1024}kB of history buffers, more than the 160kB available")
    if too_slow and not args.force:
        sys.exit("not writing header (use --force to override)")

    out = [
        "// Generated by tools/nam2header.py - do not edit",
        "#pragma once",
        '#include "nam_fixed.h"',
        "",
        f"#define NAM_MODEL_CLOCK_MHZ {args.clock}",
        f"#define NAM_RING_WORDS {max(ring, 1)}",
        "",
    ]
    out += parts
    out.append("static const nam_fx::Model *const kModels[] = {" + ", ".join(f"&m{i}" for i in range(len(parts))) + "};")
    out.append(f"static constexpr int kNumModels = {len(parts)};")
    with open(args.output, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"wrote {args.output}")

    if args.verify and not verify(args.output, models, here, args.seconds):
        sys.exit(1)


if __name__ == "__main__":
    main()
