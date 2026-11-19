// =============================================================================
//  shared.h  -  Data shared between the two processor cores
// =============================================================================
//
//  WHY THIS FILE EXISTS
//  --------------------
//  The Workshop Computer is built around an RP2040 chip, which has TWO
//  processor "cores". Each core runs its own program at the same time as the
//  other - like two people working in the same kitchen.
//
//    Core 0  runs ComputerCard's ProcessSample() 48,000 times a second.
//            It reads the clock input, decides when a step happens, and sets
//            the CV / pulse outputs. Timing matters here, so it must never
//            wait for anything slow.
//
//    Core 1  runs the USB host (talking to the Launchpad), works out each new
//            Game of Life generation, and handles the buttons and lights.
//            USB is slow and unpredictable, so we keep it away from core 0.
//
//  The two cores need to talk to each other. They do that through the global
//  variables declared in this file. Both cores can read and write the same
//  memory, so we have to be a little careful (see "volatile" below).
//
//  HEADER FILES IN C++
//  -------------------
//  A ".h" file is a "header". It DECLARES things (says "this exists, and this
//  is its type") so that several ".cpp" files can use them. Exactly one .cpp
//  file then DEFINES each global (actually creates the memory for it). For
//  these globals that is main.cpp.
//
//  "#pragma once" tells the compiler to only read this file once, even if
//  several other files #include it.
// =============================================================================

#pragma once

#include <stdint.h>   // gives us fixed-size integer types such as uint8_t

// A "namespace" is a named box for our code. Everything inside is referred to
// as lplife::Something from outside, which stops our names clashing with
// names used by the Pico SDK or TinyUSB. (lplife = "Launchpad Life".)
namespace lplife {

// -----------------------------------------------------------------------------
//  Basic sizes
// -----------------------------------------------------------------------------
// "constexpr" means "a constant the compiler knows while compiling". It is the
// modern C++ way to name a fixed number, safer than a #define.
constexpr int kGridWidth  = 8;   // the Launchpad X has an 8 x 8 grid of pads
constexpr int kGridHeight = 8;
constexpr int kNumSideButtons = 8;   // the column of buttons on the right
constexpr int kNumPages       = 8;   // one page per top-row button

// -----------------------------------------------------------------------------
//  Pages
// -----------------------------------------------------------------------------
// An "enum" gives names to a list of numbers. PagePlay is 0, PageRule is 1,
// and so on. Using names makes the code read like English.
//
// The ORDER here is the order of the top-row buttons, left to right.
enum Page : uint8_t {
  PagePlay = 0,  // transport and actions: run/stop, randomise, clear, ...
  PageRule,      // which cellular-automaton rule to use
  PageEvolve,    // how many steps between each new generation
  PageTempo,     // internal clock speed
  PageLength,    // how many columns the playhead loops over
  PageScale,     // musical scale for the CV outputs
  PageRoot,      // root note (key) of the scale
  PageOptions,   // assorted on/off switches
};

// -----------------------------------------------------------------------------
//  Option flags (the OPTIONS page)
// -----------------------------------------------------------------------------
// We pack eight on/off options into ONE byte. Each option owns one "bit".
// "1 << 3" means "the number 1 shifted left 3 places" = binary 00001000 = 8.
//
//   To test an option:   if (options & OptGates) { ... }
//   To turn it on:       options |= OptGates;
//   To flip it:          options ^= OptGates;
//
// The order matches the side buttons on the OPTIONS page, top to bottom.
enum OptionBit : uint8_t {
  OptWrap        = 1 << 0,  // grid edges wrap around (a "torus")
  OptReverse     = 1 << 1,  // playhead moves right-to-left
  OptCv2Density  = 1 << 2,  // CV Out 2 = how many cells are alive in the column
                            //   (off: CV Out 2 = pitch of the LOWEST live cell)
  OptGates       = 1 << 3,  // Pulse Out 1 gives long gates (off: short triggers)
  OptAutoReseed  = 1 << 4,  // re-randomise when the grid dies or stops moving
  OptWideRange   = 1 << 5,  // each row jumps two scale steps instead of one
  OptOctaveDown  = 1 << 6,  // shift all pitches down one octave
  OptOctaveUp    = 1 << 7,  // shift all pitches up one octave
};

// -----------------------------------------------------------------------------
//  Settings - written by core 1 (the Launchpad UI), read by core 0
// -----------------------------------------------------------------------------
//  "volatile" tells the compiler: "this value can change at any moment,
//  behind your back (because the OTHER core may write it). Always really read
//  it from memory; never keep a cached copy in a register."
//
//  Without volatile, core 0 might read 'running' once, keep it in a register,
//  and never notice core 1 changing it.
//
//  Every member is one byte. A one-byte read or write on the RP2040 always
//  happens in one go ("atomically"), so a core can never see half a value.
struct Settings {
  volatile uint8_t ruleIndex;    // 0..7, index into kRules (life.cpp)
  volatile uint8_t evolveIndex;  // 0..7, index into kEvolveSteps (main.cpp)
  volatile uint8_t tempoIndex;   // 0..7, index into kTempoBpm (main.cpp)
  volatile uint8_t length;       // 1..8 columns in the loop
  volatile uint8_t scaleIndex;   // 0..7, index into kScales (main.cpp)
  volatile uint8_t rootNote;     // 0..11 semitones above C
  volatile uint8_t options;      // OptionBit flags, see above
  volatile bool    running;      // is the sequencer playing?
  volatile bool    freeze;       // true = keep playing but stop evolving
};

// -----------------------------------------------------------------------------
//  The published grid - written by core 1, read by core 0
// -----------------------------------------------------------------------------
//  Core 1 owns the "real" Game of Life grid (see life.h). Whenever it changes,
//  core 1 PUBLISHES a compact copy here, which core 0 reads at every step.
//
//  Each row of the grid is stored in one byte: bit x is 1 if the cell in
//  column x is alive.  e.g.  rows[y] = 0b00000101  means columns 0 and 2.
//
//  DOUBLE BUFFERING
//  We keep TWO copies ([2]). Core 1 always writes into the copy core 0 is NOT
//  using, and only when it has finished does it change 'front' to point at
//  the new copy. Core 0 therefore never sees a half-updated grid.
struct PublishedGrid {
  volatile uint8_t rows[2][kGridHeight];    // alive cells
  volatile uint8_t births[2][kGridHeight];  // cells born in the latest generation
  volatile uint8_t population[2];           // total number of live cells
  volatile uint8_t front;                   // which copy (0 or 1) is current
};

// -----------------------------------------------------------------------------
//  Status and requests - flowing in both directions
// -----------------------------------------------------------------------------
//  REQUEST COUNTERS
//  When one core wants the other to "do something once" it increments a
//  counter. The other core remembers the last value it handled; whenever the
//  counter differs, it does the job and catches up. This is safer than a
//  true/false flag, because a flag that is set twice before anybody looks at
//  it would only be noticed once - and counters never need "clearing", so
//  only ONE core ever writes each counter.
struct Status {
  // ---- written by core 0, read by core 1 ----
  volatile uint32_t generationRequests;  // "please compute the next generation"
  volatile uint32_t switchDownCount;     // panel switch pushed down (randomise)
  volatile uint32_t stepCount;           // +1 on every sequencer step
  volatile uint8_t  playhead;            // column that is currently playing
  volatile bool     externalClock;       // true while a clock is arriving at Pulse In 1
  volatile uint16_t chaos;               // 0..4095, from Main knob + CV In 2

  // ---- written by core 1, read by core 0 ----
  volatile uint32_t resetRequests;       // "please send the playhead home"
  volatile bool     launchpadConnected;  // is a Launchpad plugged in & ready?
};

// -----------------------------------------------------------------------------
//  The global instances
// -----------------------------------------------------------------------------
//  "extern" means "this variable exists, but it is created in another file".
//  The real definitions (with their starting values) are in main.cpp.
extern Settings      g_settings;
extern PublishedGrid g_grid;
extern Status        g_status;

}  // namespace lplife
