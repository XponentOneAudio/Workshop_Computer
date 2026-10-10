"""End-to-end checks on synthetic recordings with known answers.

Run from tools/budgie_analysis:  python3 -m unittest discover tests
"""

import csv
import os
import shutil
import subprocess
import tempfile
import unittest

import numpy as np

from budgie_analysis import analyze, audio, config, corpus, pitch, sequence, synth

SR = 48000


class TestPitch(unittest.TestCase):
    def track(self, x, cfg):
        p = cfg["pitch"]
        _, f0, _ = pitch.yin(x, SR, p["fmin_hz"], p["fmax_hz"], int(SR * p["win_ms"] / 1000), 48, p["threshold"])
        return f0

    def test_pure_tone(self):
        cfg = config.load()
        for f in (600.0, 2900.0, 4800.0):
            t = np.arange(int(0.1 * SR)) / SR
            f0 = self.track(np.sin(2 * np.pi * f * t), cfg)
            self.assertLess(abs(np.nanmedian(f0) / f - 1), 0.01, f)

    def test_harmonic_tone_no_octave_error(self):
        cfg = config.load()
        t = np.arange(int(0.1 * SR)) / SR
        x = sum(a * np.sin(2 * np.pi * 700 * k * t) for k, a in [(1, 1), (2, 0.9), (3, 0.7), (4, 0.5)])
        self.assertLess(abs(np.nanmedian(self.track(x, cfg)) / 700 - 1), 0.01)

    def test_noise_is_unvoiced(self):
        cfg = config.load()
        f0 = self.track(np.random.default_rng(0).standard_normal(SR // 10), cfg)
        self.assertGreater(np.mean(np.isnan(f0)), 0.8)


class TestSequence(unittest.TestCase):
    def test_entropy_falls_with_structure(self):
        rng = np.random.default_rng(0)
        seqs = [corpus.label_sequence(rng, 40) for _ in range(50)]
        h = [r["entropy_bits"] for r in sequence.conditional_entropy(seqs, 3)]
        self.assertGreater(h[0], h[1])
        self.assertGreater(h[1], h[2])

    def test_model_ends_with_fallback(self):
        cfg = config.load()
        model = sequence.variable_order_model(["ABAB" * 10, "ABAC" * 10], cfg)
        self.assertEqual(model[-1]["context"], "")
        self.assertTrue(all(abs(sum(r["dist"]) - 1) < 1e-9 for r in model))


class TestQuantise(unittest.TestCase):
    def test_roundtrip_renders(self):
        rng = np.random.default_rng(0)
        for c in "ABCDEFG":
            m = corpus.element(rng, c)
            y = synth.render(synth.quantise(m), SR, rng, gain=1.0)
            self.assertEqual(len(y), int(round(synth.quantise(m)["dur_ms"] * SR / 1000)))
            self.assertTrue(np.all(np.isfinite(y)))


class TestPipeline(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        x, cls.truth = corpus.make(90, SR, seed=3)
        cls.wav = os.path.join(cls.tmp, "synthetic.wav")
        audio.save(cls.wav, x, SR)
        truth_csv = os.path.join(cls.tmp, "truth.csv")
        with open(truth_csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["file", "onset_s", "label"])
            for on, _, lab in cls.truth:
                w.writerow(["synthetic.wav", on, lab])
        cls.out = os.path.join(cls.tmp, "out")
        cls.summary = analyze.run([cls.wav], cls.out, config.load(), labels=truth_csv, resynth=True)
        with open(os.path.join(cls.out, "elements.csv")) as f:
            cls.rows = list(csv.DictReader(f))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def test_segmentation_recall(self):
        onsets = np.array([float(r["onset_s"]) for r in self.rows])
        hits = sum(np.min(np.abs(onsets - on)) < 0.005 for on, _, _ in self.truth)
        self.assertGreater(hits / len(self.truth), 0.95)
        self.assertLess(abs(len(self.rows) - len(self.truth)), 0.05 * len(self.truth))

    def test_rule_labels_match_truth(self):
        lab = [r for r in self.rows if r["hand_label"]]
        acc = np.mean([r["hand_label"] == r["rule_label"] for r in lab])
        self.assertGreater(acc, 0.9)

    def test_contact_call_measurements(self):
        b = [r for r in self.rows if r["label"] == "B"]
        self.assertTrue(b)
        f0 = np.median([float(r["f0_median"]) for r in b])
        self.assertTrue(2200 <= f0 <= 3600)
        am = [float(r["am_rate_hz"]) for r in b]
        self.assertTrue(80 * 0.9 <= np.median(am) <= 300 * 1.1)

    def test_resynthesis_close(self):
        d = [float(r["resynth_dist_db"]) for r in self.rows if r["label"] in "BCDG"]
        self.assertLess(np.median(d), 3.0)

    def test_outputs_exist(self):
        for f in ("report.md", "model.json", "budgie_data.h", "labels/synthetic.txt",
                  "resynth/synthetic_AB.wav"):
            self.assertTrue(os.path.exists(os.path.join(self.out, f)), f)

    @unittest.skipUnless(shutil.which("cc"), "no C compiler")
    def test_header_compiles(self):
        src = os.path.join(self.tmp, "t.c")
        with open(src, "w") as f:
            f.write('#include "budgie_data.h"\n'
                    "int main(void) {\n"
                    "  unsigned n = 0;\n"
                    "  for (int i = 0; i < BUDGIE_N_ELEMENTS; i++) n += budgie_elements[i].dur_ms;\n"
                    "  if (budgie_contexts[BUDGIE_N_CONTEXTS - 1].order != 0) return 1;\n"
                    "  if (budgie_category_start[BUDGIE_N_CATEGORIES] != BUDGIE_N_ELEMENTS) return 2;\n"
                    "  return n == 0;\n}\n")
        exe = os.path.join(self.tmp, "t")
        subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", self.out, src, "-o", exe],
                       check=True)
        self.assertEqual(subprocess.run([exe]).returncode, 0)


if __name__ == "__main__":
    unittest.main()
