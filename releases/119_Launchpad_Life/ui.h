// =============================================================================
//  ui.h  -  The paged Launchpad interface
// =============================================================================
//
//  LAYOUT
//  ------
//  * The 8 x 8 grid always shows the Game of Life. Press a pad to bring a
//    cell to life or kill it.
//
//  * The 8 TOP buttons choose a PAGE (the lit-up one is the current page):
//
//      1 PLAY   2 RULE   3 EVOLVE   4 TEMPO   5 LENGTH   6 SCALE   7 ROOT   8 OPTIONS
//
//  * The 8 SIDE buttons are the controls for the current page. Their lights
//    show the current settings. Most pages are "pick one of eight" (the
//    bright button is the chosen one); PLAY holds actions and OPTIONS holds
//    eight on/off switches.
//
//  The full list of what each button does is in README.md, and also in the
//  comments of ui.cpp next to the code that does it.
//
//  This class runs entirely on CORE 1.
// =============================================================================

#pragma once

#include <stdint.h>
#include "launchpad.h"
#include "life.h"
#include "shared.h"

namespace lplife {

class UI {
 public:
  // Set up the starting grid and settings. Call once at power-on.
  void Init(uint32_t randomSeed);

  // ---- input from the Launchpad ------------------------------------------
  // "pressed" is true when a button goes down and false when it comes up.
  void OnPad(int x, int y, bool pressed);
  void OnTopButton(int index, bool pressed);
  void OnSideButton(int index, bool pressed);

  // ---- regular jobs ------------------------------------------------------
  // Check for requests from core 0 (new generation, switch pushed down) and
  // carry them out. Call this as often as possible.
  void Update();

  // Paint the whole Launchpad into the frame buffer (then call lp.Show()).
  void Draw(Launchpad& lp, uint32_t nowMicros);

  // Set by the switch-down request, so the caller knows to re-initialise
  // the Launchpad (e.g. if it was knocked out of Programmer mode).
  bool TakeLaunchpadRefreshRequest();

 private:
  // ---- actions ---------------------------------------------------------
  void NextGeneration();         // run one step of the cellular automaton
  void RandomiseGrid();
  void Publish();                // copy the grid to core 0 (see shared.h)

  // ---- per-page handlers ----------------------------------------------
  void PressOnPlayPage(int index);
  void PressOnRootPage(int index);

  // ---- drawing helpers ------------------------------------------------
  void DrawGrid(Launchpad& lp);
  void DrawTopRow(Launchpad& lp);
  void DrawSideButtons(Launchpad& lp);
  void DrawLogo(Launchpad& lp, uint32_t nowMicros);
  Colour CellColour(int x, int y) const;

  // ---- state -------------------------------------------------------------
  Life life_;              // the live grid
  Life snapshot_;          // a saved copy (PLAY page: save / recall)
  bool haveSnapshot_ = false;

  // The last two generations, used to notice when the pattern has got
  // stuck (died, or frozen into a still life or a two-step blinker).
  Life previous_;
  Life previousButOne_;
  int  stuckGenerations_ = 0;

  uint8_t page_ = PagePlay;        // which page is showing
  bool sideHeld_[kNumSideButtons] = {};  // which side buttons are held down

  // ROOT page: which natural note (C, D, E, F, G, A, B = 0..6) and whether
  // it is sharpened.
  uint8_t rootNatural_ = 0;
  bool rootSharp_ = false;

  uint32_t rng_ = 1;               // random number generator state

  // "How many requests from core 0 have we already dealt with?"
  // (see the explanation of request counters in shared.h)
  uint32_t handledGenerations_ = 0;
  uint32_t handledSwitchDowns_ = 0;
  uint32_t lastStepCount_ = 0;
  uint32_t logoFlashUntil_ = 0;    // time (microseconds) the logo stops flashing

  bool launchpadRefreshRequested_ = false;
};

}  // namespace lplife
