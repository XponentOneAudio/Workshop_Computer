import argparse
import csv
import glob
import os

from . import analyze, audio, config, corpus


def _wavs(inputs):
    out = []
    for p in inputs:
        if os.path.isdir(p):
            out += sorted(glob.glob(os.path.join(p, "*.wav")) + glob.glob(os.path.join(p, "*.WAV")))
        else:
            out.append(p)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(prog="budgie_analysis",
                                 description="Analyse budgerigar recordings into data for the Budgie card.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("analyze", help="analyse WAV recordings")
    a.add_argument("inputs", nargs="+", help="WAV files or directories of WAVs")
    a.add_argument("-o", "--out", default="out", help="output directory (default: out)")
    a.add_argument("--config", help="JSON file overriding settings in config.py")
    a.add_argument("--labels", help="hand labels: a directory of Audacity label tracks "
                                    "(<recording>.txt) or a CSV with file,onset_s,label")
    a.add_argument("--clusters", type=int, default=0, help="also run k-means with this many clusters")
    a.add_argument("--resynth", action="store_true", help="write resynthesised and A/B comparison WAVs")

    d = sub.add_parser("demo", help="make a synthetic test recording with known labels")
    d.add_argument("-o", "--out", default="demo", help="output directory (default: demo)")
    d.add_argument("--seconds", type=float, default=60.0)
    d.add_argument("--seed", type=int, default=1)

    args = ap.parse_args(argv)
    if args.cmd == "analyze":
        paths = _wavs(args.inputs)
        if not paths:
            raise SystemExit("No WAV files found.")
        s = analyze.run(paths, args.out, config.load(args.config), args.labels, args.clusters, args.resynth)
        print(f"{s['n_elements']} elements, {s['n_bouts']} bouts -> {args.out}/ "
              "(report.md, elements.csv, model.json, budgie_data.h, labels/)")
    else:
        cfg = config.load()
        os.makedirs(args.out, exist_ok=True)
        x, truth = corpus.make(args.seconds, cfg["sample_rate"], args.seed)
        audio.save(os.path.join(args.out, "synthetic.wav"), x, cfg["sample_rate"])
        with open(os.path.join(args.out, "synthetic_truth.csv"), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["file", "onset_s", "offset_s", "label"])
            for on, off, lab in truth:
                w.writerow(["synthetic.wav", f"{on:.4f}", f"{off:.4f}", lab])
        print(f"{len(truth)} elements -> {args.out}/synthetic.wav, synthetic_truth.csv")
