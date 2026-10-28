// phaser.h - wave digital model of the JH Krautrock Phaser (J. Haible, 2008).
//
// Two halves, deliberately split by rate and by number type:
//
//   Phaser   the audio path, run every sample in FIXED POINT. Eight LM741
//            first-order all-pass stages, the input amplifier U6, and the
//            resonance loop that runs from the output of stage 2 back into
//            U6's inverting input.
//
//   Control  the LFO, the lamp driver, two incandescent lamps and eight LDRs,
//            run every 16th sample (3 kHz) in FLOAT on core 1. Everything here
//            moves on millisecond timescales, so soft float is affordable there
//            - but not inside the audio interrupt, which is why it is a
//            separate class with a separate core.
//
// Schematic designators (R26, U6, C19 ...) refer to Haible's drawing.
//
// --- How the circuit becomes a wave digital filter -------------------------
//
// Every capacitor is a WDF one-port under the bilinear transform: port
// resistance Rp = T/2C, reflected wave = last sample's incident wave. Every
// resistor is reflection-free. Op-amps are ideal (V+ == V-, no input current),
// which is what lets each part of the circuit be solved locally:
//
//   All-pass stage (U8..U15). Cx and the LDR form a series loop driven by the
//   stage input; the LDR voltage is V+. With e = Vin - s (s = Cx's wave state)
//   and q = Rp/(Rp + R_ldr):
//       V+ = (1-q) e,   Vout = 2 V+ - Vin,   s' = s + 2 q e.
//   q is the only coefficient, it lives in [0,1), and adapting it every sample
//   costs nothing because the LDR is a leaf of the tree.
//
//   U6 and the resonance network. R60, R61+C19, the R66 pot halves, and
//   R62/C21/R63 meet at three nodes and form a bridge, which no series or
//   parallel adaptor can express - this is an R-type adaptor, solved here in
//   closed form by nodal analysis. Its awkward part is that R63 is driven by
//   the output of stage 2, which depends INSTANTANEOUSLY on U6's output (an
//   all-pass passes high frequencies straight through). That is a delay-free
//   loop. It is resolved exactly, not by inserting a unit delay: each stage's
//   output is affine in its input given its state, so the first two compose to
//   Vs = G Vo + H, and the network equation is solved with that substituted.
//   One division per sample buys a loop with no extra delay, so the resonance
//   lands on the same frequency as in the analogue circuit.
//
// Fixed-point formats: signals are volts in Q20 (13 V rails = 13.6M, leaving
// headroom for wave variables), coefficients in Q30 (or Q24 where they exceed
// one). The all-pass q needs Q30 because the low end of the sweep has
// q ~ 2e-4; Q15 would quantise the bottom of the sweep into audible steps.

#ifndef KRAUT_PHASER_H_
#define KRAUT_PHASER_H_

#include <stdint.h>
#include <math.h>

#ifdef KRAUT_HOST_BUILD
  #define __not_in_flash_func(f) f
#else
  #include "pico.h"
#endif

namespace kraut {

constexpr float kFs = 48000.0f;
constexpr float kT = 1.0f / kFs;
constexpr int kStages = 8;
constexpr int kCtrlDiv = 16;            // control block every 16 samples
constexpr int kCtrlShift = 4;           // log2(kCtrlDiv)

constexpr int32_t kOneQ20 = 1 << 20;    // one volt
constexpr int32_t kOneQ30 = 1 << 30;

static inline int32_t __not_in_flash_func(Mul30)(int32_t a, int32_t b) {
  return (int32_t)(((int64_t)a * b) >> 30);
}
static inline int32_t __not_in_flash_func(Mul24)(int32_t a, int32_t b) {
  return (int32_t)(((int64_t)a * b) >> 24);
}
// Q30 multiply with error feedback: the truncated low bits are carried into
// the next sample instead of being thrown away. Used wherever a product is
// INTEGRATED into a state. The bottom of the sweep has q ~ 2e-4, and plain
// truncation there piles up into a low-frequency noise floor about 50 dB
// down - measured, see tools/phaser_check.cpp. With the remainder carried, the
// state tracks to within one LSB forever.
static inline int32_t __not_in_flash_func(Mul30EF)(int32_t a, int32_t b, int32_t* rem) {
  const int64_t p = (int64_t)a * b + *rem;
  *rem = (int32_t)(p & 0x3FFFFFFF);
  return (int32_t)(p >> 30);
}
static inline int32_t ToQ30(float x) { return (int32_t)(x * 1073741824.0f); }
static inline int32_t ToQ24(float x) { return (int32_t)(x * 16777216.0f); }

// --- Component values (Haible's schematic) ---------------------------------

namespace part {
// Input coupling. R50 (1M) sits across an ideal source and does nothing here.
constexpr float C17 = 220e-9f, R49 = 4.7e3f, R48 = 180e3f;
// U6 feedback network.
constexpr float R60 = 100e3f;           // "{reso}" trimmer, 470k max - see below
constexpr float R66 = 50e3f;            // FEEDBACK pot
constexpr float R61 = 3.9e3f;
constexpr float C19 = 2.35e-6f;         // C19 + C20, 4.7u back-to-back
constexpr float R62 = 18e3f, R63 = 18e3f, C21 = 680e-12f;
// All-pass stages. The schematic leaves Cx as a parameter; with 22n the
// lamp/LDR model below sweeps each pole over roughly 25 Hz - 3.8 kHz.
constexpr float Cx = 22e-9f;
// Output dividers into the relay: R59/R58 (wet mix) and R64/R54 (dry).
constexpr float kOutDiv = 2.4e3f / (22e3f + 2.4e3f);
}  // namespace part

// Jack scaling. The circuit wants instrument level, the Computer deals in
// +/-2048 codes. Full scale at the input becomes 0.8 V at C17 - loud enough to
// push U6 into its rails once the resonance peaks, as a hot guitar would. The
// output dividers (R59/R58, R64/R54) are kept, and full scale out is 0.8 V
// after them, so dry gain through the card is about unity.
constexpr int32_t kInScale = 410;                                // code -> Q20
constexpr int32_t kOutScale =
    (int32_t)(part::kOutDiv * (2048.0f / 0.8f) * 1024.0f);       // Q20 -> code, Q30

// R60 is a trimmer that sets how far the FEEDBACK pot can go. At its full
// 470k the loop self-oscillates from about a quarter of the pot's travel; at
// 100k the end stop gives ~+27 dB of resonance and stays just stable across
// the whole sweep. That is how the trimmer would be set on the bench, and it
// is what tools/phaser_check.cpp verifies.

// ===========================================================================
// Audio path
// ===========================================================================

// The R-type adaptor's coefficients. They depend only on the FEEDBACK pot, so
// they are solved in float off the audio path and handed over whole.
struct NetCoeffs {
  int32_t A, B, C;                       // Q24
  int32_t alpha, beta, w1, w2, w3, m1n, k19;   // Q30
};

// amount 0..1: the FEEDBACK pot, anticlockwise to clockwise. Clockwise moves
// R66's wiper towards U6's inverting input.
static inline NetCoeffs ComputeNet(float amount) {
  using namespace part;
  const float a = 1.0f - amount;                // wiper position from node X
  float ra = a * R66, rb = (1.0f - a) * R66;
  if (ra < 100.0f) ra = 100.0f;                 // pot end resistance
  if (rb < 100.0f) rb = 100.0f;
  const float rc19 = kT / (2.0f * C19), rg = R61 + rc19;
  const float rc21 = kT / (2.0f * C21);
  const float rth = 1.0f / (1.0f / R63 + 1.0f / rc21);   // C21 node Thevenin
  const float rf = R62 + rth;
  const float g60 = 1.0f / R60, ga = 1.0f / ra, gb = 1.0f / rb;
  const float gf = 1.0f / rf, gg = 1.0f / rg;
  const float d = ga + gb + gf;
  const float nx = g60 + ga + gg - ga * ga / d;
  const float c = ga * gf / d;
  const float d0 = g60 + ga * gb / d;
  NetCoeffs n;
  n.A = ToQ24(nx / d0);
  n.B = ToQ24(gg / d0);
  n.C = ToQ24(c / d0);
  n.alpha = ToQ30(rth / R63);
  n.beta = ToQ30(rth / rc21);
  n.w1 = ToQ30(gb / d);
  n.w2 = ToQ30(ga / d);
  n.w3 = ToQ30(gf / d);
  n.m1n = ToQ30(1.0f - (1.0f / rth) / (1.0f / rth + 1.0f / R62));
  n.k19 = ToQ30(2.0f * rc19 / rg);
  return n;
}

class Phaser {
 public:
  void Init() {
    for (int i = 0; i < kStages; i++) { s_[i] = 0; q_[i] = 0; dq_[i] = 0; r_[i] = 0; }
    s17_ = b19_ = b21_ = r17_ = r19_ = 0;
    // Input coupling C17 into R49+R48: same series-loop structure as a stage.
    const float rp17 = kT / (2.0f * part::C17);
    q17_ = ToQ30(rp17 / (rp17 + part::R49 + part::R48));
    div17_ = ToQ30(part::R48 / (part::R49 + part::R48));
    n_ = ComputeNet(0.0f);
  }

  void SetNet(const NetCoeffs& n) { n_ = n; }

  // New all-pass coefficients (Q30) for the next kCtrlDiv samples. They are
  // ramped linearly, so the per-sample path sees no 3 kHz zipper.
  void SetTargets(const int32_t q[kStages]) {
    for (int i = 0; i < kStages; i++) dq_[i] = (q[i] - q_[i]) >> kCtrlShift;
  }
  // Jump straight to coefficients (tests, and the very first block).
  void SetQ(const int32_t q[kStages]) {
    for (int i = 0; i < kStages; i++) { q_[i] = q[i]; dq_[i] = 0; }
  }

  // One sample. x: input in Q20 volts at the input jack. Returns U6's output
  // (the dry signal as the circuit sees it) in *dry, and the output of the
  // last all-pass stage. Both Q20 volts.
  int32_t __not_in_flash_func(Process)(int32_t x, int32_t* dry) {
    for (int i = 0; i < kStages; i++) q_[i] += dq_[i];

    // C17 into R49+R48. X is U6's non-inverting input, and with an ideal
    // op-amp also its inverting input: the node the network hangs off.
    int32_t e = x - s17_;
    int32_t d = Mul30EF(e, q17_, &r17_);
    s17_ += 2 * d;
    const int32_t X = Mul30(e - d, div17_);

    // Stages 1 and 2 as an affine map of U6's output: Vs = G Vo + H.
    const int32_t g1 = kOneQ30 - 2 * q_[0], g2 = kOneQ30 - 2 * q_[1];
    const int32_t h1 = -2 * s_[0] + 2 * Mul30(q_[0], s_[0]);
    const int32_t h2 = -2 * s_[1] + 2 * Mul30(q_[1], s_[1]);
    const int32_t G = Mul30(g2, g1);
    const int32_t H = Mul30(g2, h1) + h2;
    // Seen through R63 and the C21 node, as a Thevenin source into R62.
    const int32_t Gp = Mul30(n_.alpha, G);
    const int32_t Hp = Mul30(n_.alpha, H) + Mul30(n_.beta, b21_);

    // The R-type adaptor, solved for U6's output.
    const int32_t num = Mul24(n_.A, X) - Mul24(n_.B, b19_) - Mul24(n_.C, Hp);
    const int32_t den = kOneQ30 + Mul24(n_.C, Gp);       // >= ~0.7, never 0
    const uint32_t recip = 0xFFFFFFFFu / (uint32_t)(den >> 14);   // Q16
    int32_t vo = (int32_t)(((int64_t)num * (int32_t)recip) >> 16);
    // LM741 output swing on +/-15 V. The 741 hits its rails hard.
    constexpr int32_t kRail = 13 * kOneQ20;
    if (vo > kRail) vo = kRail;
    if (vo < -kRail) vo = -kRail;
    *dry = vo;

    // The eight stages, run forward with the actual (possibly clipped) Vo.
    int32_t v = vo, vs = 0;
    for (int i = 0; i < kStages; i++) {
      e = v - s_[i];
      d = Mul30EF(e, q_[i], &r_[i]);
      v = v - 2 * s_[i] - 2 * d;
      s_[i] += 2 * d;
      if (i == 1) vs = v;
    }

    // Close the network: node W (R66 wiper), the C21 node, and C19.
    const int32_t vth = Mul30(n_.alpha, vs) + Mul30(n_.beta, b21_);
    const int32_t w = Mul30(n_.w1, vo) + Mul30(n_.w2, X) + Mul30(n_.w3, vth);
    const int32_t vm = vth + Mul30(n_.m1n, w - vth);
    b21_ = 2 * vm - b21_;
    b19_ += Mul30EF(n_.k19, X - b19_, &r19_);
    return v;
  }

  int32_t Q(int i) const { return q_[i]; }

 private:
  int32_t s_[kStages], q_[kStages], dq_[kStages], r_[kStages];
  int32_t s17_, q17_, div17_, r17_;
  int32_t b19_, b21_, r19_;
  NetCoeffs n_;
};

// ===========================================================================
// Control: LFO -> lamp driver -> lamps -> LDRs, at 3 kHz in float
// ===========================================================================
//
// Volts throughout are the circuit's own (single +15 V supply on this side).

namespace lfo {
// U5A buffers R13/R12 into a 7.86 V reference. U3 is a Schmitt trigger
// (R7/R10) on a 741 that swings 1.5..13.5 V; U4 integrates its output onto C8.
constexpr float kVref = 15.0f * 10.0f / 19.1f;
constexpr float kSqHi = 13.5f, kSqLo = 1.5f;
constexpr float kR7 = 22e3f, kR10 = 13e3f;
constexpr float kVlo = (kVref * (kR7 + kR10) - kSqHi * kR10) / kR7;   // 4.53 V
constexpr float kVhi = (kVref * (kR7 + kR10) - kSqLo * kR10) / kR7;   // 11.6 V
// R8+R9 against R24 (110R) to the reference: a 89:1 attenuator, whose 109R
// Thevenin resistance sits in series with the OSC_PERIOD pot R25.
constexpr float kAtten = 110.0f / (5.1e3f + 4.7e3f + 110.0f);
constexpr float kRth = 1.0f / (1.0f / 110.0f + 1.0f / 9.8e3f);
constexpr float kC8 = 1e-6f;
}  // namespace lfo

namespace lamp {
// PHASING_AMPLITUDE (R26) runs from U4's output down to a node held near
// 3.1 V by R30/R29/R28. LOWLIM (R18, set to its top, via U5B and D12) holds
// Q1's base no lower than 4.1 V, so the lamps never go fully dark. Q1, the
// diodes and Q4 then drop about 1.9 V on the way to the lamps (RANGE trimmer
// R21 shorting D7/D13).
constexpr float kNode = 3.08f;
constexpr float kBaseMin = 4.09f;
constexpr float kBaseMax = 14.0f;
constexpr float kDrop = 1.9f;
// Two 7 V / 100 mA lamps in series. Filament temperature theta is normalised
// to its rated value; resistance tracks it, radiated power goes as theta^4.
constexpr float kRhot = 70.0f;
constexpr float kPrated = 0.7f;
constexpr float kTheta0 = 300.0f / 2700.0f;      // ambient
constexpr float kThetaMax = 1.6f;
// Heat also leaves through the leads, in proportion to the temperature rise.
// Small next to radiation when bright; dominant when dim, which is what lets
// a barely-glowing filament cool in tens of milliseconds rather than seconds.
constexpr float kPcond = 0.12f;  // W per unit theta
constexpr float kCth = 0.035f;  // J per unit theta: ~11 ms at rated, >100 ms dim
// Light seen by the cells: a Wien-law curve, exp(k (1 - 1/theta)). A CdS cell
// sees well into the near infrared, where filament output rises less steeply
// with temperature than in the visible; k = 6 corresponds to about 900 nm.
constexpr float kWien = 6.0f;
// LDR: R = 1k at rated lamp light, gamma 0.6, 2M dark. Rise and decay times
// differ, as CdS cells always do - the decay is the slow one. Together with
// Cx this puts the steady-state pole at ~25 Hz with the lamps at LOWLIM and
// ~3.8 kHz at the top of the drive range.
constexpr float kR1 = 1000.0f, kGamma = 0.6f, kGdark = 1.0f / 2e6f;
constexpr float kTauUp = 0.006f, kTauDown = 0.035f;
constexpr int kLut = 256;
}  // namespace lamp

class Control {
 public:
  void Init() {
    using namespace lamp;
    for (int i = 0; i < kLut + 1; i++) {
      const float th = kTheta0 + (kThetaMax - kTheta0) * (float)i / (float)kLut;
      const float l = expf(kWien * (1.0f - 1.0f / th));
      lightLut_[i] = l;
      gLut_[i] = kGdark + powf(l, kGamma) / kR1;
    }
    v_ = lfo::kVlo;
    rising_ = true;
    theta_ = kTheta0;
    g_ = kGdark;
    rp_ = kT / (2.0f * part::Cx);
    const float dt = kCtrlDiv * kT;
    kUp_ = 1.0f - expf(-dt / kTauUp);
    kDown_ = 1.0f - expf(-dt / kTauDown);
    SetRate(0.5f, 0.0f);
    // Eight LDRs from one batch are never identical. The spread smears the
    // notches slightly - part of why lamp phasers sound soft, not glassy.
    static const float mismatch[kStages] =
        {1.00f, 0.92f, 1.07f, 0.96f, 1.10f, 0.94f, 1.03f, 0.89f};
    for (int i = 0; i < kStages; i++) mis_[i] = mismatch[i];
  }

  // knob 0..1 (OSC_PERIOD, clockwise faster) and octaves of CV offset.
  void SetRate(float knob, float octaves) {
    // R25 is a 50k log pot wired as a rheostat; 50k -> 120R over the travel
    // takes the LFO from ~0.09 Hz to ~19 Hz.
    float r25 = 50e3f * exp2f(-8.7f * knob);
    float rate = 1.0f / ((r25 + lfo::kRth) * lfo::kC8) * exp2f(octaves);
    rate_ = rate * lfo::kAtten * (kCtrlDiv * kT);     // V per block per V drive
  }

  // Restart the sweep from the bottom (Pulse In 1).
  void Reset() { v_ = lfo::kVlo; rising_ = true; }

  // One control block. depth 0..1 (PHASING_AMPLITUDE), manual: volts added at
  // the AMOUNT jack, hold: freeze the LFO. Fills q[] with Q30 targets.
  void Step(float depth, float manual, bool hold, int32_t q[kStages]) {
    using namespace lamp;
    if (!hold) {
      const float drive = (rising_ ? lfo::kSqLo : lfo::kSqHi) - lfo::kVref;
      v_ -= drive * rate_;                  // U4 inverts
      if (rising_ && v_ >= lfo::kVhi) { v_ = lfo::kVhi; rising_ = false; }
      if (!rising_ && v_ <= lfo::kVlo) { v_ = lfo::kVlo; rising_ = true; }
    }

    float base = kNode + depth * (v_ - kNode) + manual;
    if (base < kBaseMin) base = kBaseMin;
    if (base > kBaseMax) base = kBaseMax;
    lampV_ = base - kDrop;

    // Filament heat balance, per lamp (each sees half the drive).
    const float vh = 0.5f * lampV_;
    const float pin = vh * vh / (kRhot * theta_);
    const float t2 = theta_ * theta_, t02 = kTheta0 * kTheta0;
    const float prad = (kPrated - kPcond * (1.0f - kTheta0)) *
                       (t2 * t2 - t02 * t02) / (1.0f - t02 * t02);
    const float pcond = kPcond * (theta_ - kTheta0);
    theta_ += (kCtrlDiv * kT / kCth) * (pin - prad - pcond);
    if (theta_ < kTheta0) theta_ = kTheta0;
    if (theta_ > kThetaMax) theta_ = kThetaMax;

    float pos = (theta_ - kTheta0) * ((float)kLut / (kThetaMax - kTheta0));
    int idx = (int)pos;
    if (idx >= kLut) idx = kLut - 1;
    const float fr = pos - (float)idx;
    const float gt = gLut_[idx] + fr * (gLut_[idx + 1] - gLut_[idx]);
    light_ = lightLut_[idx] + fr * (lightLut_[idx + 1] - lightLut_[idx]);

    g_ += (gt - g_) * (gt > g_ ? kUp_ : kDown_);

    for (int i = 0; i < kStages; i++) {
      const float rg = rp_ * g_ * mis_[i];          // Rp / R_ldr
      q[i] = ToQ30(rg / (1.0f + rg));
    }
  }

  // Monitoring.
  float Lfo() const { return (v_ - 0.5f * (lfo::kVlo + lfo::kVhi)) /
                             (0.5f * (lfo::kVhi - lfo::kVlo)); }      // -1..1
  bool Rising() const { return rising_; }
  float Light() const { return light_; }          // 0..~1.5, 1 = rated
  float PoleHz() const { return g_ / (6.2831853f * part::Cx); }
  float LampVolts() const { return lampV_; }

 private:
  float v_, rate_, theta_, g_, rp_, kUp_, kDown_, lampV_ = 0.0f, light_ = 0.0f;
  bool rising_;
  float mis_[kStages];
  float gLut_[lamp::kLut + 1], lightLut_[lamp::kLut + 1];
};

}  // namespace kraut

#endif  // KRAUT_PHASER_H_
