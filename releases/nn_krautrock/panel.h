// panel.h - knobs, scenes and the switch, for the Krautrock Phaser.
//
// Runs on core 1 once per control block (3 kHz), in float. Host-testable: it
// knows nothing about the hardware, only knob positions (0..1), the switch,
// and the scene gate.
//
//   Switch Middle / Up   two SCENES, each holding its own RATE, AMOUNT and
//                        FEEDBACK. Pulse In 2 high selects the other scene
//                        from the one the switch picks.
//   Switch Down (held)   Main becomes MIX, dry to wet. X and Y carry on as
//                        normal.
//
// SOFT TAKEOVER. Whenever a knob is reassigned - a scene change, or Main
// moving between RATE and MIX - it stops controlling anything until it is
// turned to the stored value (or crosses it). Until then the setting stays
// where it was.
//
// GLIDE. A scene change does not jump. Each setting moves to its new value
// along a critically damped path, which eases in and out like a hand turning
// a pot, and settles in about half a second. It glides the POT POSITION, so
// RATE sweeps through the original's log law rather than linearly in Hz, and
// AMOUNT then reaches the lamps through their own thermal lag.

#ifndef KRAUT_PANEL_H_
#define KRAUT_PANEL_H_

#include <math.h>

namespace kraut {

enum PanelSwitch { kSwDown, kSwMiddle, kSwUp };
enum Param { kRate, kAmount, kFeedback, kParams };

class PanelLogic {
 public:
  // Takes over at the knobs' current positions: both scenes start equal to
  // the panel, and every knob is live.
  void Init(float main, float x, float y) {
    const float k[kParams] = {main, x, y};
    for (int i = 0; i < kParams; i++) {
      scene_[0][i] = scene_[1][i] = k[i];
      pos_[i] = k[i];
      vel_[i] = 0.0f;
      live_[i] = true;
    }
    slow_ = false;
    mix_ = 0.5f;
    mixLive_ = false;
    down_ = false;
    physScene_ = 0;
    active_ = 0;
    boot_ = kBootBlocks;
  }

  void Step(float main, float x, float y, PanelSwitch sw, bool gate) {
    // ComputerCard's switch filter starts at Down; ignore the switch until it
    // has settled, so power-up never briefly hands Main to MIX.
    if (boot_ > 0) { boot_--; sw = kSwMiddle; }

    // --- Down: hold for MIX ---------------------------------------------------
    const bool down = sw == kSwDown;
    if (down && !down_) {
      mixLive_ = false;
      mixLast_ = main - mix_;
    }
    if (!down && down_) Release(kRate, main);   // Main goes back to RATE
    down_ = down;
    if (down && Catch(main, mix_, &mixLast_, &mixLive_)) mix_ = main;

    // --- Scene selection ----------------------------------------------------
    if (sw == kSwMiddle) physScene_ = 0;
    if (sw == kSwUp) physScene_ = 1;          // Down keeps the last position
    const int scene = physScene_ ^ (gate ? 1 : 0);
    if (scene != active_) {
      active_ = scene;
      slow_ = true;
      if (!down) Release(kRate, main);
      Release(kAmount, x);
      Release(kFeedback, y);
    }

    // --- Knobs into the active scene ----------------------------------------
    const float k[kParams] = {main, x, y};
    for (int i = 0; i < kParams; i++) {
      if (i == kRate && down) continue;       // Main is MIX for now
      if (Catch(k[i], scene_[active_][i], &last_[i], &live_[i]))
        scene_[active_][i] = k[i];
    }

    // --- Glide ---------------------------------------------------------------
    // Slow after a scene change, until every setting has arrived. Otherwise
    // fast: knob moves pass straight through, minus ADC jitter.
    const float w = slow_ ? kWSlow : kWFast;
    bool arrived = true;
    for (int i = 0; i < kParams; i++) {
      const float err = scene_[active_][i] - pos_[i];
      vel_[i] += (w * w * err - 2.0f * w * vel_[i]) * kDt;
      pos_[i] += vel_[i] * kDt;
      if (fabsf(err) > 0.002f || fabsf(vel_[i]) > 0.01f) arrived = false;
    }
    if (arrived) slow_ = false;
  }

  float Value(Param p) const {
    const float v = pos_[p];
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
  }
  float Mix() const { return mix_; }
  int Scene() const { return active_; }
  bool Live(Param p) const { return live_[p]; }
  bool Gliding() const { return slow_; }

 private:
  static constexpr float kDt = 16.0f / 48000.0f;
  // Critically damped: ~99% of the way in 6.6/w seconds.
  static constexpr float kWSlow = 13.0f;          // ~0.5 s
  static constexpr float kWFast = 150.0f;         // ~40 ms
  static constexpr float kCatch = 0.012f;         // ~1% of the travel
  static constexpr int kBootBlocks = 1500;        // 0.5 s

  // Reassign a knob: it stops controlling until it reaches the stored value.
  void Release(Param p, float knob) {
    live_[p] = false;
    last_[p] = knob - scene_[active_][p];
  }

  // Soft takeover. A dead knob comes alive when it gets close to the stored
  // value, or crosses it between two blocks (a fast turn can jump the window).
  static bool Catch(float knob, float stored, float* last, bool* live) {
    if (*live) return true;
    const float diff = knob - stored;
    if (fabsf(diff) < kCatch || (diff > 0.0f) != (*last > 0.0f)) *live = true;
    *last = diff;
    return *live;
  }

  float scene_[2][kParams];
  float pos_[kParams], vel_[kParams], last_[kParams] = {0, 0, 0};
  bool live_[kParams];
  bool slow_;
  float mix_, mixLast_ = 0.0f;
  bool mixLive_, down_;
  int physScene_, active_, boot_;
};

}  // namespace kraut

#endif  // KRAUT_PANEL_H_
