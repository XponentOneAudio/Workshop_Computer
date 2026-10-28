// KRAUTROCK PHASER - a wave digital recreation of the JH Krautrock Phaser
// (Juergen Haible, 2008) for the Music Thing Modular Workshop System Computer.
//
// Eight LM741 all-pass stages swept by eight LDRs, lit by two incandescent
// lamps, driven by a Schmitt/integrator triangle LFO - and a resonance loop
// from stage 2 back into the input amplifier. See phaser.h for the circuit
// model and tools/phaser_check.cpp for what has been verified on the host.
//
// CORE SPLIT
//   core 0  the audio ISR: the whole audio path in fixed point, every sample.
//   core 1  the control model in float, once per 16-sample block: LFO, lamp
//           driver, filament heat balance, LDR response, and solving the
//           feedback network when the FEEDBACK knob moves.
//
// The control model needs eight float divisions per block, and the network
// solve a dozen more. Inline in the ISR, that block would land on one sample
// and blow its 20.8 us budget; on core 1 it has the whole block (333 us).
// Results come back through a double buffer: core 1 fills the slot core 0 is
// not reading, then publishes its index. Core 0 picks up the newest slot at
// each block boundary, so the audio path sees one block (0.33 ms) of control
// latency - far below anything a lamp can do.

#include "ComputerCard.h"

#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"

#include "phaser.h"

// REQUIRED for the Workshop Computer's crystal: without it the card can fail
// to boot from cold. Also passed by CMakeLists.txt; the registry's validator
// looks for this form in the source.
#ifndef PICO_XOSC_STARTUP_DELAY_MULTIPLIER
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64
#endif

using namespace kraut;

// --- Shared between cores ---------------------------------------------------

// core 0 -> core 1: a snapshot of the panel, taken at each block boundary.
struct Panel {
  int32_t rate, depth, feedback;    // knobs, 0..4095
  int32_t cv1, cv2;                 // -2048..2047
  bool hold;
};
static volatile uint32_t gTick = 0;
static volatile bool gReset = false;
static Panel gPanel;

// core 1 -> core 0: everything the audio path and the panel outputs need.
struct Result {
  int32_t q[kStages];
  NetCoeffs net;
  int16_t cvLfo, cvPole;            // CV out codes
  uint16_t ledLamp, ledUp, ledDown; // LED brightness, 0..4095
  bool rising;
};
static Result gResult[2];
static volatile int gReady = -1;

// --- Core 1: the lamp phaser's control circuitry -----------------------------

static void Core1Entry() {
  static Control ctrl;
  ctrl.Init();
  int32_t lastFeedback = -1000;
  NetCoeffs net = ComputeNet(0.0f);
  int w = 0;
  uint32_t seen = gTick;

  while (true) {
    const uint32_t t = gTick;
    if (t == seen) continue;
    seen = t;
    __dmb();
    const Panel p = gPanel;

    // RATE knob, plus CV 1 at roughly one octave per volt.
    ctrl.SetRate((float)p.rate * (1.0f / 4095.0f), (float)p.cv1 * (1.0f / 341.0f));
    if (gReset) { gReset = false; ctrl.Reset(); }

    Result& r = gResult[w];
    // CV 2 is the AMOUNT jack: volts added at Q1's base, so it sweeps the
    // lamps by hand - through their lag - with or without the LFO.
    ctrl.Step((float)p.depth * (1.0f / 4095.0f), (float)p.cv2 * (6.0f / 2048.0f),
              p.hold, r.q);

    // The network is re-solved only when FEEDBACK actually moves, with enough
    // hysteresis that ADC noise does not trigger it.
    if (p.feedback - lastFeedback > 6 || lastFeedback - p.feedback > 6) {
      lastFeedback = p.feedback;
      net = ComputeNet((float)p.feedback * (1.0f / 4095.0f));
    }
    r.net = net;

    // CV 1: the triangle as U4 makes it, +/-5 V. CV 2: where the notches are,
    // as the log of the pole frequency, 0 V at 25 Hz and +1 V per octave.
    r.cvLfo = (int16_t)(ctrl.Lfo() * 1700.0f);
    float pole = ctrl.PoleHz() * (1.0f / 25.0f);
    float oct = pole > 1.0f ? log2f(pole) : 0.0f;
    if (oct > 6.0f) oct = 6.0f;
    r.cvPole = (int16_t)(oct * 341.0f);
    // The lamp LEDs glow like the lamps: by light, not by drive.
    float l = sqrtf(ctrl.Light());
    if (l > 1.0f) l = 1.0f;
    r.ledLamp = (uint16_t)(l * 4095.0f);
    float tri = 0.5f + 0.5f * ctrl.Lfo();
    r.ledUp = (uint16_t)(tri * 4095.0f);
    r.ledDown = (uint16_t)((1.0f - tri) * 4095.0f);
    r.rising = ctrl.Rising();

    __dmb();
    gReady = w;
    w ^= 1;
  }
}

// --- Core 0: audio ------------------------------------------------------------

class KrautrockPhaser : public ComputerCard {
 public:
  KrautrockPhaser() { engine_.Init(); }

 protected:
  void __not_in_flash_func(ProcessSample)() override {
    if (phase_ == 0) Block();
    phase_ = (phase_ + 1) & (kCtrlDiv - 1);

    // Switch: Up is VIBRATO (wet only), Middle is PHASER (the MODULATION pot
    // at noon: equal dry and wet). Down is momentary and toggles BYPASS, as
    // the original's footswitch toggles its relay.
    // ComputerCard's switch filter starts at Down, so ignore the first half
    // second rather than toggling BYPASS on every power-up.
    if (boot_ < 24000) boot_++;
    else if (SwitchChanged() && SwitchVal() == Down) bypass_ = !bypass_;
    const int32_t mixTarget = SwitchVal() == Up ? kOneQ30 : kOneQ30 / 2;
    mix_ += (mixTarget - mix_) >> 9;
    const int32_t bypTarget = bypass_ ? 4096 : 0;
    if (byp_ < bypTarget) byp_++;               // ~85 ms crossfade, no click
    if (byp_ > bypTarget) byp_--;

    if (PulseIn1RisingEdge()) gReset = true;

    int32_t in = AudioIn1() + AudioIn2();
    if (in > 2047) in = 2047;
    if (in < -2048) in = -2048;

    int32_t dry;
    const int32_t wet = engine_.Process(in * kInScale, &dry);
    const int32_t mixed = dry + Mul30(mix_, wet - dry);
    if (dry >= 13 * kOneQ20 || dry <= -13 * kOneQ20) clipHold_ = 480;
    else if (clipHold_) clipHold_--;

    // Out 1 is the original's L_Out (the effect), Out 2 its R_Out (dry,
    // through U6 and the same divider). Pan them apart for Haible's spread.
    const int32_t l = Clip(Mul30(mixed, kOutScale));
    const int32_t r = Clip(Mul30(dry, kOutScale));
    AudioOut1((int16_t)((l * (4096 - byp_) + in * byp_) >> 12));
    AudioOut2((int16_t)((r * (4096 - byp_) + in * byp_) >> 12));
    PulseOut2(clipHold_ > 0);
  }

 private:
  // Once per 16 samples: hand the panel to core 1, take its newest result.
  void __not_in_flash_func(Block)() {
    gPanel.rate = KnobVal(Main);
    gPanel.depth = KnobVal(X);
    gPanel.feedback = KnobVal(Y);
    gPanel.cv1 = CVIn1();
    gPanel.cv2 = CVIn2();
    gPanel.hold = PulseIn2();
    __dmb();
    gTick = gTick + 1;

    const int ready = gReady;
    if (ready < 0) return;
    const Result& r = gResult[ready];
    if (!primed_) { engine_.SetQ(r.q); primed_ = true; }
    else engine_.SetTargets(r.q);
    engine_.SetNet(r.net);

    CVOut1(r.cvLfo);
    CVOut2(r.cvPole);
    PulseOut1(r.rising);
    LedBrightness(0, r.ledLamp);
    LedBrightness(1, r.ledLamp);
    LedBrightness(2, r.ledUp);
    LedBrightness(3, r.ledDown);
    LedOn(4, !bypass_);
    LedOn(5, clipHold_ > 0);
  }

  static int32_t Clip(int32_t v) { return v > 2047 ? 2047 : v < -2048 ? -2048 : v; }

  Phaser engine_;
  int phase_ = 0;
  int32_t boot_ = 0;
  bool primed_ = false;
  bool bypass_ = false;
  int32_t byp_ = 0;
  int32_t mix_ = kOneQ30 / 2;
  int32_t clipHold_ = 0;
};

int main() {
  // 192 MHz, as used by several cards on this hardware.
  set_sys_clock_khz(192000, true);
  multicore_launch_core1(Core1Entry);
  static KrautrockPhaser card;
  card.Run();
}
