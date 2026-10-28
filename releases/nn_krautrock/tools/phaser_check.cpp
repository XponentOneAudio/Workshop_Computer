// phaser_check.cpp - host checks for the Krautrock Phaser engine.
//
//   g++ -O2 -std=c++17 -DKRAUT_HOST_BUILD -I.. phaser_check.cpp -o phaser_check
//   ./phaser_check [out.wav]
//
// 1. Fixed point against a double-precision model of the same circuit.
// 2. Resonance: peak gain at each FEEDBACK setting, and no self-oscillation
//    at the end stop (the R60 trim claim in phaser.h).
// 3. Sweep range of the lamp/LDR model against RATE and AMOUNT, which shows
//    the lamps' thermal lag eating the depth at fast rates.
// 4. Stability under the full model: 20 s of loud noise at maximum feedback.
// 5. The panel: LFO stop, scenes, soft takeover, glide and mix.
// 6. Optionally renders a demo WAV through the whole card model.
//
// Exit status is non-zero if any check fails.

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "panel.h"
#include "phaser.h"

using namespace kraut;

static int failures = 0;
static void Check(bool ok, const char* what) {
  printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

// --- Double-precision reference: same equations, no quantisation ----------

struct RefPhaser {
  double s[kStages] = {}, s17 = 0, b19 = 0, b21 = 0, q[kStages] = {};
  double A, B, C, alpha, beta, w1, w2, w3, m1n, k19, q17, div17;
  explicit RefPhaser(double amount) {
    using namespace part;
    const double T = 1.0 / 48000.0;
    double a = 1.0 - amount, ra = a * (double)R66, rb = (1.0 - a) * (double)R66;
    if (ra < 100) ra = 100;
    if (rb < 100) rb = 100;
    double rc19 = T / (2 * (double)C19), rg = (double)R61 + rc19;
    double rc21 = T / (2 * (double)C21), rth = 1 / (1 / (double)R63 + 1 / rc21);
    double rf = (double)R62 + rth;
    double g60 = 1 / (double)R60, ga = 1 / ra, gb = 1 / rb, gf = 1 / rf, gg = 1 / rg;
    double d = ga + gb + gf, d0 = g60 + ga * gb / d;
    A = (g60 + ga + gg - ga * ga / d) / d0;
    B = gg / d0;
    C = ga * gf / d / d0;
    alpha = rth / (double)R63; beta = rth / rc21;
    w1 = gb / d; w2 = ga / d; w3 = gf / d;
    m1n = 1 - (1 / rth) / (1 / rth + 1 / (double)R62);
    k19 = 2 * rc19 / rg;
    double rp17 = T / (2 * (double)C17);
    q17 = rp17 / (rp17 + (double)R49 + (double)R48);
    div17 = (double)R48 / ((double)R49 + (double)R48);
  }
  double Process(double x, double* dry) {
    double e = x - s17, dd = q17 * e;
    s17 += 2 * dd;
    double X = (e - dd) * div17;
    double g1 = 1 - 2 * q[0], g2 = 1 - 2 * q[1];
    double h1 = -2 * (1 - q[0]) * s[0], h2 = -2 * (1 - q[1]) * s[1];
    double G = g2 * g1, H = g2 * h1 + h2;
    double Gp = alpha * G, Hp = alpha * H + beta * b21;
    double vo = (A * X - B * b19 - C * Hp) / (1 + C * Gp);
    if (vo > 13) vo = 13;
    if (vo < -13) vo = -13;
    *dry = vo;
    double v = vo, vs = 0;
    for (int i = 0; i < kStages; i++) {
      e = v - s[i];
      v = v - 2 * s[i] - 2 * q[i] * e;
      s[i] += 2 * q[i] * e;
      if (i == 1) vs = v;
    }
    double vth = alpha * vs + beta * b21;
    double w = w1 * vo + w2 * X + w3 * vth;
    double vm = vth + m1n * (w - vth);
    b21 = 2 * vm - b21;
    b19 += k19 * (X - b19);
    return v;
  }
};

static void QFor(double rldr, double qd[kStages], int32_t qi[kStages]) {
  const double rp = 1.0 / (48000.0 * 2 * (double)part::Cx);
  for (int i = 0; i < kStages; i++) {
    qd[i] = rp / (rp + rldr);
    qi[i] = (int32_t)llround(qd[i] * 1073741824.0);
    qd[i] = qi[i] / 1073741824.0;          // compare like with like
  }
}

// Magnitude response of the 50/50 mix, by driving a sine and measuring.
static double MixGainAt(double amount, double rldr, double hz, bool fixed) {
  double qd[kStages]; int32_t qi[kStages];
  QFor(rldr, qd, qi);
  Phaser p; p.Init(); p.SetNet(ComputeNet((float)amount)); p.SetQ(qi);
  RefPhaser r(amount);
  for (int i = 0; i < kStages; i++) r.q[i] = qd[i];
  const double amp = 0.01;                       // small: stay linear
  const int settle = 48000, n = 48000;
  double acc = 0;
  for (int t = 0; t < settle + n; t++) {
    double x = amp * sin(2 * M_PI * hz * t / 48000.0), y;
    if (fixed) {
      int32_t dry, wet = p.Process((int32_t)lround(x * kOneQ20), &dry);
      y = 0.5 * (dry + wet) / kOneQ20;
    } else {
      double dry, wet = r.Process(x, &dry);
      y = 0.5 * (dry + wet);
    }
    if (t >= settle) acc += y * y;
  }
  return sqrt(2 * acc / n) / amp;
}

static void CheckAgainstReference() {
  printf("\n1. Fixed point vs double reference (noise in, 2 s)\n");
  double worst = -999;
  for (double amount : {0.0, 0.5, 0.9, 1.0}) {
    for (double rldr : {1.5e3, 20e3, 300e3}) {
      double qd[kStages]; int32_t qi[kStages];
      QFor(rldr, qd, qi);
      Phaser p; p.Init(); p.SetNet(ComputeNet((float)amount)); p.SetQ(qi);
      RefPhaser r(amount);
      for (int i = 0; i < kStages; i++) r.q[i] = qd[i];
      srand(1);
      double err = 0, sig = 0;
      for (int t = 0; t < 96000; t++) {
        double x = 0.2 * ((rand() / (double)RAND_MAX) * 2 - 1);
        int32_t xi = (int32_t)lround(x * kOneQ20);
        int32_t dryi; int32_t wi = p.Process(xi, &dryi);
        double dryd, wd = r.Process(xi / (double)kOneQ20, &dryd);
        double a = (wi + dryi) / (double)kOneQ20, b = wd + dryd;
        err += (a - b) * (a - b); sig += b * b;
      }
      double db = 10 * log10(err / sig);
      if (db > worst) worst = db;
      printf("  feedback %.1f  R_ldr %6.0f   error %6.1f dB\n", amount, rldr, db);
    }
  }
  Check(worst < -60, "fixed-point error below -60 dB everywhere");
}

static void CheckResonance() {
  printf("\n2. Resonance (50/50 mix, R_ldr = 20k, pole at %.0f Hz)\n",
         1 / (2 * M_PI * 20e3 * (double)part::Cx));
  const double pole = 1 / (2 * M_PI * 20e3 * (double)part::Cx);
  double lastPeak = 0;
  bool monotonic = true;
  for (double amount : {0.0, 0.25, 0.5, 0.75, 1.0}) {
    double peak = 0, peakHz = 0;
    for (double hz = 0.6 * pole; hz <= 1.6 * pole; hz *= 1.02) {
      double g = MixGainAt(amount, 20e3, hz, true);
      if (g > peak) { peak = g; peakHz = hz; }
    }
    double base = MixGainAt(amount, 20e3, 8000, true);
    printf("  feedback %.2f  peak %5.1f dB at %4.0f Hz  (vs %5.1f dB at 8 kHz)\n",
           amount, 20 * log10(peak), peakHz, 20 * log10(base));
    if (peak < lastPeak) monotonic = false;
    lastPeak = peak;
  }
  Check(monotonic, "resonance grows monotonically with FEEDBACK");

  // End stop: an impulse must die away, not ring forever.
  for (double rldr : {1e3, 5e3, 20e3, 100e3, 1e6}) {
    double qd[kStages]; int32_t qi[kStages];
    QFor(rldr, qd, qi);
    Phaser p; p.Init(); p.SetNet(ComputeNet(1.0f)); p.SetQ(qi);
    double tail = 0;
    for (int t = 0; t < 5 * 48000; t++) {
      int32_t dry, wet = p.Process(t == 0 ? kOneQ20 / 10 : 0, &dry);
      if (t > 4 * 48000) tail = fmax(tail, fabs((double)wet) / kOneQ20);
    }
    char msg[96];
    snprintf(msg, sizeof msg, "full FEEDBACK, R_ldr %7.0f: impulse decays (tail %.1e V)",
             rldr, tail);
    Check(tail < 1e-4, msg);
  }
}

static void CheckSweep() {
  printf("\n3. Sweep range (all-pass pole frequency, after the lamps settle)\n");
  printf("   rate knob  depth   LFO Hz     pole min   pole max\n");
  for (float rate : {0.2f, 0.5f, 0.7f, 0.85f, 1.0f}) {
    for (float depth : {0.0f, 0.5f, 1.0f}) {
      Control c; c.Init(); c.SetRate(rate, 0);
      int32_t q[kStages];
      double lo = 1e9, hi = 0;
      int flips = 0; bool lastRising = c.Rising();
      const int blocks = 3000 * 30;            // 30 s
      for (int b = 0; b < blocks; b++) {
        c.Step(depth, 0, false, q);
        if (b > blocks / 2) {
          lo = fmin(lo, (double)c.PoleHz()); hi = fmax(hi, (double)c.PoleHz());
          if (c.Rising() != lastRising) flips++;
        }
        lastRising = c.Rising();
      }
      printf("   %5.2f     %4.2f  %7.3f   %7.0f Hz %7.0f Hz\n", rate, depth,
             flips / 2.0 / 15.0, lo, hi);
    }
  }
  Control c; c.Init(); c.SetRate(0.2f, 0);
  int32_t q[kStages];
  double lo = 1e9, hi = 0;
  for (int b = 0; b < 3000 * 60; b++) {
    c.Step(1.0f, 0, false, q);
    if (b > 3000 * 30) { lo = fmin(lo, (double)c.PoleHz()); hi = fmax(hi, (double)c.PoleHz()); }
  }
  Check(lo < 100 && hi > 3000, "full depth at slow rate sweeps below 100 Hz to above 3 kHz");
  Control d; d.Init(); d.SetRate(1.0f, 0);
  double lo2 = 1e9, hi2 = 0;
  for (int b = 0; b < 3000 * 10; b++) {
    d.Step(1.0f, 0, false, q);
    if (b > 3000 * 5) { lo2 = fmin(lo2, (double)d.PoleHz()); hi2 = fmax(hi2, (double)d.PoleHz()); }
  }
  Check(hi2 / lo2 < (hi / lo) / 4, "lamp lag shrinks the sweep at the fastest rate");
}

struct Card {
  Phaser p; Control c;
  int32_t q[kStages];
  void Init(float rate, float depth, float fb) {
    p.Init(); c.Init(); c.SetRate(rate, 0); p.SetNet(ComputeNet(fb));
    c.Step(depth, 0, false, q); p.SetQ(q);
    this->depth = depth;
  }
  float depth;
  int t = 0;
  // in/out: Computer audio codes, +/-2048. Mirrors main.cpp's scaling.
  void Process(int16_t in, int16_t* l, int16_t* r, bool vibrato = false) {
    if ((t++ & (kCtrlDiv - 1)) == 0) { c.Step(depth, 0, false, q); p.SetTargets(q); }
    int32_t dry, wet = p.Process(in * kInScale, &dry);
    int32_t mix = vibrato ? wet : (dry + wet) >> 1;
    *l = Clip(Mul30(mix, kOutScale));
    *r = Clip(Mul30(dry, kOutScale));
  }
  static int16_t Clip(int32_t v) { return v > 2047 ? 2047 : v < -2048 ? -2048 : (int16_t)v; }
};

static void CheckStability() {
  printf("\n4. Full model, max feedback, loud noise for 20 s\n");
  Card k; k.Init(0.6f, 1.0f, 1.0f);
  srand(7);
  int16_t l, r; int maxl = 0; double dc = 0;
  for (int t = 0; t < 48000 * 20; t++) {
    int16_t in = (int16_t)((rand() % 4096) - 2048);
    k.Process(in, &l, &r);
    maxl = abs(l) > maxl ? abs(l) : maxl;
    dc += l;
  }
  int tailmax = 0;
  for (int t = 0; t < 48000 * 3; t++) {
    k.Process(0, &l, &r);
    if (t > 48000 * 2) tailmax = abs(l) > tailmax ? abs(l) : tailmax;
  }
  printf("  peak output %d, mean %.2f, residual after 2 s silence %d\n",
         maxl, dc / (48000 * 20), tailmax);
  Check(tailmax <= 2, "returns to silence after the input stops");
}

static void WriteWav(const char* path, const std::vector<int16_t>& lr) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  uint32_t data = (uint32_t)lr.size() * 2, rate = 48000, br = rate * 4;
  uint32_t riff = 36 + data, fmt = 16; uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
  fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
  fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f);
  fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
  for (int16_t s : lr) { int16_t v = (int16_t)(s * 16); fwrite(&v, 2, 1, f); }
  fclose(f);
}

// Demo: a sawtooth drone through four settings, 8 s each. Left = the card's
// Out 1 (effect), right = Out 1 again so it auditions on any speaker.
static void Render(const char* path) {
  struct Scene { float rate, depth, fb; bool vib; } scenes[] = {
      {0.30f, 1.0f, 0.0f, false},     // slow, deep, no feedback
      {0.30f, 1.0f, 0.85f, false},    // same, resonant
      {0.62f, 0.7f, 0.6f, false},     // medium rate
      {0.55f, 1.0f, 0.3f, true},      // vibrato
  };
  std::vector<int16_t> out;
  double ph[3] = {0, 0, 0};
  const double hz[3] = {55.0, 82.41, 110.0};
  for (const Scene& s : scenes) {
    Card k; k.Init(s.rate, s.depth, s.fb);
    for (int t = 0; t < 48000 * 8; t++) {
      double x = 0;
      for (int v = 0; v < 3; v++) {
        ph[v] += hz[v] / 48000.0; if (ph[v] >= 1) ph[v] -= 1;
        x += (2 * ph[v] - 1) / 3;
      }
      int16_t l, r;
      k.Process((int16_t)(x * 1400), &l, &r, s.vib);
      out.push_back(l); out.push_back(l);
    }
  }
  WriteWav(path, out);
  printf("\nrendered %s\n", path);
}

// --- 5. Panel ----------------------------------------------------------------

static void Steps(PanelLogic& p, int n, float m, float x, float y,
                  PanelSwitch sw = kSwMiddle, bool gate = false) {
  for (int i = 0; i < n; i++) p.Step(m, x, y, sw, gate);
}

static void CheckPanel() {
  printf("\n5. Panel\n");
  const int sec = 3000;                       // control blocks per second

  // RATE fully anticlockwise stops the LFO; just above the stop it crawls.
  {
    Control c; c.Init(); c.SetRate(0.4f, 0);
    int32_t q[kStages];
    for (int b = 0; b < sec / 3; b++) c.Step(1, 0, false, q);
    c.SetRate(0.0f, 0);
    const float before = c.Lfo();
    for (int b = 0; b < 10 * sec; b++) c.Step(1, 0, false, q);
    Check(c.Stopped() && c.Lfo() == before, "RATE at zero holds the LFO still");
    c.SetRate(0.0f, 2.0f);
    Check(c.Stopped(), "CV cannot restart a stopped LFO (it scales the rate)");
    c.SetRate(0.025f, 0);
    const float a = c.Lfo();
    for (int b = 0; b < sec; b++) c.Step(1, 0, false, q);
    const float moved = fabsf(c.Lfo() - a);
    Check(moved > 0 && moved < 0.05f, "just above the stop, the LFO crawls (minutes per cycle)");
  }

  // Scenes, glide and soft takeover.
  {
    PanelLogic p; p.Init(0.2f, 0.3f, 0.4f);
    Steps(p, sec, 0.2f, 0.3f, 0.4f);           // let the boot window pass
    Steps(p, sec / 2, 0.8f, 0.7f, 0.6f);       // scene A: turn everything
    Check(fabsf(p.Value(kRate) - 0.8f) < 0.002f, "live knobs drive scene A");

    // Switch to B, which still holds the boot values. Knobs stay where they are.
    float prev = p.Value(kRate);
    bool monotonic = true, overshoot = false;
    float at50ms = 0;
    int settled = -1;
    for (int b = 0; b < sec * 2; b++) {
      p.Step(0.8f, 0.7f, 0.6f, kSwUp, false);
      const float v = p.Value(kRate);
      if (v > prev + 1e-6f) monotonic = false;
      if (v < 0.2f - 1e-4f) overshoot = true;
      if (b == sec / 20) at50ms = v;
      if (settled < 0 && fabsf(v - 0.2f) < 0.006f) settled = b;
      prev = v;
    }
    printf("  glide 0.8 -> 0.2: %.2f at 50 ms, within 1%% after %d ms\n",
           (double)at50ms, settled * 1000 / sec);
    Check(p.Scene() == 1, "switch Up selects scene B");
    Check(at50ms > 0.7f, "scene change glides rather than jumps");
    Check(settled > 0 && settled < sec * 7 / 10, "glide arrives within 0.7 s");
    Check(monotonic && !overshoot, "glide neither reverses nor overshoots");
    Check(!p.Live(kRate) && fabsf(p.Value(kRate) - 0.2f) < 0.002f,
          "knob away from B's value does not take over");
    Steps(p, 100, 0.5f, 0.7f, 0.6f, kSwUp);
    Check(fabsf(p.Value(kRate) - 0.2f) < 0.002f, "turning towards it does nothing yet");
    Steps(p, 1, 0.1f, 0.7f, 0.6f, kSwUp);     // fast turn that jumps past 0.2
    Steps(p, 300, 0.35f, 0.7f, 0.6f, kSwUp);
    Check(p.Live(kRate) && fabsf(p.Value(kRate) - 0.35f) < 0.002f,
          "crossing the stored value takes over");

    // Back to A: its values were kept.
    Steps(p, sec * 2, 0.35f, 0.7f, 0.6f, kSwMiddle);
    Check(fabsf(p.Value(kRate) - 0.8f) < 0.002f && fabsf(p.Value(kAmount) - 0.7f) < 0.002f,
          "scene A kept its settings");

    // The gate selects the other scene.
    Steps(p, 10, 0.35f, 0.7f, 0.6f, kSwMiddle, true);
    Check(p.Scene() == 1, "Pulse In 2 high flips Middle to scene B");
    Steps(p, 10, 0.35f, 0.7f, 0.6f, kSwUp, true);
    Check(p.Scene() == 0, "Pulse In 2 high flips Up to scene A");
  }

  // Hold Down for MIX.
  {
    PanelLogic p; p.Init(0.5f, 0.5f, 0.5f);
    // ComputerCard reports Down while its switch filter settles at power-up.
    Steps(p, 100, 0.5f, 0.5f, 0.5f, kSwDown);
    Steps(p, 100, 0.9f, 0.5f, 0.5f, kSwDown);
    Check(fabsf(p.Mix() - 0.5f) < 1e-6f, "the boot-time Down reading does not edit MIX");
    Steps(p, sec, 0.6f, 0.5f, 0.5f);

    const float rateBefore = p.Value(kRate);
    Steps(p, 100, 0.6f, 0.5f, 0.5f, kSwDown);   // Main at 0.6, MIX at 0.5
    Check(fabsf(p.Mix() - 0.5f) < 1e-6f, "holding Down does not jump MIX to Main");
    for (int i = 0; i <= 100; i++) p.Step(0.6f - 0.6f * (float)i / 100.0f, 0.5f, 0.5f, kSwDown, false);
    Check(fabsf(p.Mix()) < 1e-6f, "Down + Main turns MIX after pickup");
    Steps(p, 100, 0.0f, 0.5f, 0.5f, kSwDown);
    Steps(p, 300, 0.0f, 0.5f, 0.5f);
    Check(fabsf(p.Value(kRate) - rateBefore) < 0.002f,
          "after release Main does not drag RATE until picked up");
  }
}

int main(int argc, char** argv) {
  CheckAgainstReference();
  CheckResonance();
  CheckSweep();
  CheckStability();
  CheckPanel();
  if (argc > 1) Render(argv[1]);
  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
