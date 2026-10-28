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
//   core 1  the control model in float, once per 16-sample block: the panel
//           (scenes, soft takeover, glide - see panel.h), LFO, lamp driver,
//           filament heat balance, LDR response, and solving the feedback
//           network when FEEDBACK moves.
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

#include "panel.h"
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
  int32_t main, x, y;               // knobs, 0..4095
  int32_t cv1, cv2;                 // -2048..2047
  PanelSwitch sw;
  bool gate;                        // Pulse In 2: the other scene
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
  int32_t mix;                      // Q30, dry 0 .. wet 1
  bool rising, sceneB;
};
static Result gResult[2];
static volatile int gReady = -1;

// --- Core 1: the lamp phaser's control circuitry -----------------------------

static void Core1Entry() {
  static Control ctrl;
  static PanelLogic panel;
  ctrl.Init();
  bool started = false;
  float lastFeedback = -1.0f;
  NetCoeffs net = ComputeNet(0.0f);
  int w = 0;
  uint32_t seen = gTick;

  while (true) {
    const uint32_t t = gTick;
    if (t == seen) continue;
    seen = t;
    __dmb();
    const Panel p = gPanel;

    const float kx = (float)p.x * (1.0f / 4095.0f);
    const float ky = (float)p.y * (1.0f / 4095.0f);
    const float km = (float)p.main * (1.0f / 4095.0f);
    if (!started) { panel.Init(km, kx, ky); started = true; }
    panel.Step(km, kx, ky, p.sw, p.gate);

    // RATE, plus CV 1 at roughly one octave per volt.
    ctrl.SetRate(panel.Value(kRate), (float)p.cv1 * (1.0f / 341.0f));
    if (gReset) { gReset = false; ctrl.Reset(); }

    Result& r = gResult[w];
    // CV 2 is the AMOUNT jack: volts added at Q1's base, so it sweeps the
    // lamps by hand - through their lag - with or without the LFO.
    ctrl.Step(panel.Value(kAmount), (float)p.cv2 * (6.0f / 2048.0f), false, r.q);

    // The network is re-solved only when FEEDBACK actually moves (a glide
    // moves it every block, which core 1 has ample time for).
    const float fb = panel.Value(kFeedback);
    if (fabsf(fb - lastFeedback) > 0.0015f) {
      lastFeedback = fb;
      net = ComputeNet(fb);
    }
    r.net = net;
    r.mix = ToQ30(panel.Mix());
    r.sceneB = panel.Scene() == 1;

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

    // MIX is the original's MODULATION pot (R65), set by holding the switch
    // Down and turning Main. Smoothed here so it cannot step at 3 kHz.
    mix_ += (mixTarget_ - mix_) >> 9;

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
    AudioOut1((int16_t)l);
    AudioOut2((int16_t)r);
    PulseOut2(clipHold_ > 0);
  }

 private:
  // Once per 16 samples: hand the panel to core 1, take its newest result.
  void __not_in_flash_func(Block)() {
    gPanel.main = KnobVal(Main);
    gPanel.x = KnobVal(X);
    gPanel.y = KnobVal(Y);
    gPanel.cv1 = CVIn1();
    gPanel.cv2 = CVIn2();
    const Switch sw = SwitchVal();
    gPanel.sw = sw == Down ? kSwDown : sw == Up ? kSwUp : kSwMiddle;
    gPanel.gate = PulseIn2();
    __dmb();
    gTick = gTick + 1;

    const int ready = gReady;
    if (ready < 0) return;
    const Result& r = gResult[ready];
    if (!primed_) { engine_.SetQ(r.q); primed_ = true; }
    else engine_.SetTargets(r.q);
    engine_.SetNet(r.net);
    mixTarget_ = r.mix;

    CVOut1(r.cvLfo);
    CVOut2(r.cvPole);
    PulseOut1(r.rising);
    LedBrightness(0, r.ledLamp);
    LedBrightness(1, r.ledLamp);
    LedBrightness(2, r.ledUp);
    LedBrightness(3, r.ledDown);
    LedOn(4, r.sceneB);       // the active scene, whether switch or gate chose it
    LedOn(5, clipHold_ > 0);
  }

  static int32_t Clip(int32_t v) { return v > 2047 ? 2047 : v < -2048 ? -2048 : v; }

  Phaser engine_;
  int phase_ = 0;
  bool primed_ = false;
  int32_t mix_ = kOneQ30 / 2, mixTarget_ = kOneQ30 / 2;
  int32_t clipHold_ = 0;
};

int main() {
  // 192 MHz, as used by several cards on this hardware.
  set_sys_clock_khz(192000, true);
  static KrautrockPhaser card;
  // Core 1 must start AFTER the card is constructed. ComputerCard's
  // constructor calls flash_get_unique_id(), which briefly disables execution
  // from flash; core 1 runs its control model from flash, so launching it
  // first crashes it. The audio path then never receives coefficients, every
  // all-pass stage passes straight through, and the effect output is just the
  // dry signal.
  multicore_launch_core1(Core1Entry);
  card.Run();
}
