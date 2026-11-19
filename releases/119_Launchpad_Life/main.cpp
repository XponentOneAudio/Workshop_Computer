// =============================================================================
//  Launchpad Life  -  a Game of Life sequencer for the Workshop Computer
// =============================================================================
//
//  A cellular automaton (Conway's Game of Life, and seven other rules) lives
//  on an 8 x 8 grid shown on a Novation Launchpad X. A playhead sweeps across
//  the grid, column by column, turning the live cells it finds into pitches
//  and gates. Every few steps the grid evolves into its next generation, so
//  the melody slowly mutates by itself.
//
//  FILES
//  -----
//    main.cpp        <- you are here. CORE 0: clock, playhead, CV/pulse outputs
//    shared.h        the variables the two cores use to talk to each other
//    usb_core1.cpp   CORE 1: USB host, MIDI parsing, the main loop
//    ui.h / ui.cpp   CORE 1: the paged Launchpad interface
//    launchpad.h/.cpp  CORE 1: Launchpad X protocol (lights + buttons)
//    life.h / life.cpp the Game of Life engine
//    ComputerCard.h  Chris Johnson's library for the Workshop Computer hardware
//    usb_midi_host.* rppicomidi's USB MIDI host driver (unmodified)
//    tusb_config.h   settings for TinyUSB, the USB library
//
//  JACKS
//  -----
//    Pulse In 1   Clock. Each rising edge is one step. With nothing patched
//                 the card runs from its own clock (TEMPO page).
//    Pulse In 2   Reset: the next step plays the first column again.
//    CV In 1      Transpose, roughly 1V/octave (added after the scale).
//    CV In 2      Chaos, added to the Main knob.
//    CV Out 1     Pitch (1V/oct) of the HIGHEST live cell in the column.
//    CV Out 2     Pitch of the LOWEST live cell (or, with the OPTIONS
//                 "density" switch, how many cells are alive: 0-5V).
//    Pulse Out 1  Fires when the column has any live cell.
//    Pulse Out 2  Fires when the column has a cell born this generation.
//    Audio Out 1  Total population of the grid as a voltage (0 to ~6V).
//    Audio Out 2  Number of live cells in the current column (0 to ~6V).
//
//  PANEL
//  -----
//    Main knob    Chaos: randomly flips cells each generation. Fully
//                 anticlockwise = none.
//    X knob       Skip: chance that a step is silent. Anticlockwise = never.
//    Y knob       Glide: slides CV Out 1 between notes.
//    Switch up    Run.   Switch middle: Stop.
//    Switch down  (momentary) Randomise the grid, and re-send Programmer mode
//                 to the Launchpad if it has got confused.
//
//  Everything else is on the Launchpad - see ui.h and README.md.
// =============================================================================

#include "ComputerCard.h"

#include "pico/multicore.h"     // multicore_launch_core1()
#include "hardware/clocks.h"    // set_sys_clock_khz()

#include "life.h"     // for NextRandom()
#include "shared.h"

using namespace lplife;

// core1_entry() is in usb_core1.cpp. Declaring it here lets main() start it.
extern "C" void core1_entry();

// =============================================================================
//  The shared variables (declared "extern" in shared.h) - created here,
//  with their power-on values.
// =============================================================================
namespace lplife {

Settings g_settings = {
    0,                          // ruleIndex:   Conway's Life
    1,                          // evolveIndex: a new generation every 2 steps
    4,                          // tempoIndex:  120 BPM
    8,                          // length:      all 8 columns
    0,                          // scaleIndex:  minor pentatonic
    0,                          // rootNote:    C
    OptWrap | OptAutoReseed,    // options:     wrap edges, auto-reseed
    true,                       // running
    false,                      // freeze
};

// "{}" sets every member to zero.
PublishedGrid g_grid = {};
Status g_status = {};

}  // namespace lplife

// =============================================================================
//  Lookup tables for the side-button choices on each page
// =============================================================================
// EVOLVE page: how many steps between generations.
static const uint8_t kEvolveSteps[8] = {1, 2, 3, 4, 6, 8, 12, 16};

// TEMPO page: beats per minute. One step is an eighth note (two steps per
// beat), so one pass of all 8 columns is one bar of 4/4.
static const uint16_t kTempoBpm[8] = {40, 60, 80, 100, 120, 140, 160, 200};

// SCALE page. Each scale lists its notes as semitones above the root.
struct Scale {
  uint8_t length;      // how many notes in one octave of the scale
  uint8_t notes[12];   // the notes themselves (unused slots are ignored)
};

static const Scale kScales[8] = {
    {5,  {0, 3, 5, 7, 10}},                          // 1 minor pentatonic
    {5,  {0, 2, 4, 7, 9}},                           // 2 major pentatonic
    {7,  {0, 2, 4, 5, 7, 9, 11}},                    // 3 major
    {7,  {0, 2, 3, 5, 7, 8, 10}},                    // 4 natural minor
    {7,  {0, 2, 3, 5, 7, 9, 10}},                    // 5 dorian
    {7,  {0, 2, 4, 5, 7, 9, 10}},                    // 6 mixolydian
    {6,  {0, 2, 4, 6, 8, 10}},                       // 7 whole tone
    {12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},    // 8 chromatic
};

// MIDI note number for the bottom row with root C. 48 is the C an octave
// below middle C, which ComputerCard outputs as -1 volt (note 60 = 0V).
static constexpr int kBaseNote = 48;

// Timing, in samples. ProcessSample() runs 48,000 times a second, so
// 48 samples = 1 millisecond.
static constexpr int32_t kSampleRate      = 48000;
static constexpr int32_t kTriggerSamples  = 480;              // 10 ms trigger
static constexpr int32_t kClockTimeout    = 2 * kSampleRate;  // 2 s without a clock -> internal
static constexpr int32_t kLedFlashSamples = 2400;             // 50 ms

// =============================================================================
//  The card itself
// =============================================================================
//  "class LifeSequencer : public ComputerCard" means our class is built ON
//  TOP of ComputerCard ("inherits" from it). We get all of ComputerCard's
//  abilities - reading knobs, setting outputs - and add our own.
class LifeSequencer : public ComputerCard {
 protected:
  // ComputerCard calls this 48,000 times every second, on core 0.
  // "override" tells the compiler we are replacing ComputerCard's version;
  // it will complain if we get the name or parameters wrong.
  //
  // Each call has only about 20 microseconds before the next one is due,
  // so everything in here must be quick - no waiting, no USB.
  void ProcessSample() override {
    ReadPanelAndRequests();

    if (ClockTick()) DoStep();

    UpdateOutputs();
  }

 private:
  // ---------------------------------------------------------------------------
  //  Knobs, switch, inputs and requests from core 1
  // ---------------------------------------------------------------------------
  void ReadPanelAndRequests() {
    // Chaos = Main knob + CV In 2, kept in the range 0..4095.
    // We subtract a little from the knob so that "fully anticlockwise"
    // really is zero even if the knob doesn't quite reach the end.
    int32_t chaos = KnobVal(Knob::Main) - 64 + CVIn2();
    if (chaos < 0) chaos = 0;
    if (chaos > 4095) chaos = 4095;
    g_status.chaos = static_cast<uint16_t>(chaos);

    // The three-way switch. SwitchChanged() is true for just one sample
    // after it moves, so each movement is handled once.
    if (SwitchChanged()) {
      const Switch now = SwitchVal();
      switch (now) {
        case Switch::Up:
          g_settings.running = true;
          break;
        case Switch::Middle:
          // Only STOP when coming down from Up. Down is a spring-loaded
          // (momentary) position, so after every randomise the switch
          // springs back to Middle by itself - that must not stop us!
          if (previousSwitch_ == Switch::Up) g_settings.running = false;
          break;
        case Switch::Down:   // momentary: ask core 1 to randomise
          g_status.switchDownCount = g_status.switchDownCount + 1;
          break;
      }
      previousSwitch_ = now;
    }

    // A reset pulse, or a reset request from the Launchpad, means the next
    // step will play the first column.
    if (PulseIn2RisingEdge()) resetPending_ = true;

    uint32_t resets = g_status.resetRequests;
    if (resets != handledResets_) {
      handledResets_ = resets;
      resetPending_ = true;
    }
  }

  // ---------------------------------------------------------------------------
  //  The clock. Returns true on the sample where a new step should happen.
  // ---------------------------------------------------------------------------
  bool ClockTick() {
    // Count samples since the last external clock pulse (stop counting at
    // a big number so the counter can never overflow).
    if (samplesSinceClock_ < kClockTimeout) samplesSinceClock_++;

    // ---- external clock on Pulse In 1 ----
    if (PulseIn1RisingEdge()) {
      // Measure the time between pulses, so gates can be half a step long.
      if (samplesSinceClock_ < kClockTimeout) stepPeriod_ = samplesSinceClock_;
      samplesSinceClock_ = 0;
      g_status.externalClock = true;
      return g_settings.running;
    }

    // If a clock pulse arrived in the last 2 seconds, we're following the
    // external clock, so the internal clock stays quiet.
    if (samplesSinceClock_ < kClockTimeout) return false;
    g_status.externalClock = false;

    // ---- internal clock ----
    // Samples per step = samples per minute / steps per minute
    //                  = (48000 * 60) / (BPM * 2)
    const int32_t bpm = kTempoBpm[g_settings.tempoIndex & 7];
    const int32_t samplesPerStep = (kSampleRate * 60) / (bpm * 2);
    stepPeriod_ = samplesPerStep;

    if (!g_settings.running) {
      // While stopped, keep the counter "just about to fire", so pressing
      // Run plays the next step immediately.
      internalCounter_ = samplesPerStep;
      return false;
    }

    internalCounter_++;
    if (internalCounter_ >= samplesPerStep) {
      internalCounter_ = 0;
      return true;
    }
    return false;
  }

  // ---------------------------------------------------------------------------
  //  One step of the sequencer
  // ---------------------------------------------------------------------------
  void DoStep() {
    // Read the loop length, making sure it's in the range 1..8.
    int length = g_settings.length;
    if (length < 1) length = 1;
    if (length > kGridWidth) length = kGridWidth;

    const bool reverse = (g_settings.options & OptReverse) != 0;

    // ---- move the playhead ----
    if (resetPending_) {
      playhead_ = reverse ? length - 1 : 0;
      resetPending_ = false;
    } else if (reverse) {
      // Going left: from column 0 (or outside a shortened loop) wrap to the end.
      playhead_ = (playhead_ <= 0 || playhead_ >= length) ? length - 1 : playhead_ - 1;
    } else {
      // Going right: past the end of the loop, wrap to column 0.
      playhead_ = (playhead_ + 1 >= length) ? 0 : playhead_ + 1;
    }

    // Tell core 1, so the Launchpad can show it.
    g_status.playhead = static_cast<uint8_t>(playhead_);
    g_status.stepCount = g_status.stepCount + 1;
    stepLedTimer_ = kLedFlashSamples;

    PlayColumn(playhead_);

    // ---- is it time for a new generation? ----
    // (Freeze stops evolution but the playhead keeps going.)
    if (!g_settings.freeze) {
      stepsSinceGeneration_++;
      if (stepsSinceGeneration_ >= kEvolveSteps[g_settings.evolveIndex & 7]) {
        stepsSinceGeneration_ = 0;
        // Core 1 does the actual work (it has more time to spare).
        // It will be finished long before the next step.
        g_status.generationRequests = g_status.generationRequests + 1;
      }
    }
  }

  // Turn the live cells in one column into outputs.
  void PlayColumn(int column) {
    // Read from the copy of the grid that core 1 has finished with.
    const uint8_t f = g_grid.front;

    int count = 0;        // how many cells are alive in this column
    int lowest = -1;      // lowest live row (-1 = none yet)
    int highest = -1;     // highest live row
    bool anyBirth = false;

    for (int y = 0; y < kGridHeight; y++) {
      // Shift the row's bits right by 'column' places, then look at bit 0.
      bool alive = (g_grid.rows[f][y] >> column) & 1;
      if (!alive) continue;

      count++;
      if (lowest < 0) lowest = y;   // the first one we find is the lowest
      highest = y;                  // the last one we find is the highest
      if ((g_grid.births[f][y] >> column) & 1) anyBirth = true;
    }

    columnCount_ = count;
    population_ = g_grid.population[f];

    // The X knob sets the chance of skipping a step. Compare a random
    // number 0..4095 with the knob: with the knob at 0 we never skip.
    const bool skip = static_cast<int32_t>(NextRandom(rng_) & 4095) < KnobVal(Knob::X);

    // An empty (or skipped) column is a rest: no pulses, and the CVs hold
    // their previous notes.
    if (count == 0 || skip) return;

    // ---- pulses ----
    const bool gates = (g_settings.options & OptGates) != 0;
    int32_t gateLength = gates ? stepPeriod_ / 2 : kTriggerSamples;
    if (gateLength < kTriggerSamples) gateLength = kTriggerSamples;

    pulse1Timer_ = gateLength;
    if (anyBirth) pulse2Timer_ = kTriggerSamples;

    // ---- pitches ----
    // CV Out 1: the highest live cell. Gliding happens in UpdateOutputs(),
    // so here we only set where it is heading.
    glideTarget_ = NoteToMillivolts(RowToNote(highest)) * 65536;

    // CV Out 2: the lowest live cell, or the column density.
    if (g_settings.options & OptCv2Density) {
      CVOut2Millivolts(count * 625);   // 8 cells * 625 mV = 5 V
    } else {
      CVOut2MIDINote(static_cast<uint8_t>(RowToNote(lowest)));
    }
  }

  // Which MIDI note does a row (0 = bottom) play?
  int RowToNote(int row) {
    const Scale& scale = kScales[g_settings.scaleIndex & 7];
    const uint8_t options = g_settings.options;

    // Each row is one step up the scale ("degree"), or two in wide mode.
    int degree = (options & OptWideRange) ? row * 2 : row;

    // Split the degree into whole octaves plus a position in the scale.
    // e.g. pentatonic (5 notes), degree 7 -> octave 1, note index 2.
    int octave = degree / scale.length;
    int index  = degree % scale.length;

    int note = kBaseNote + g_settings.rootNote + octave * 12 + scale.notes[index];

    if (options & OptOctaveDown) note -= 12;
    if (options & OptOctaveUp)   note += 12;

    // CV In 1 transposes. The input reads about 341 per volt (2048 = 6V),
    // so 1 semitone = 341 / 12 = about 28.4. Multiplying by 36/1024 is the
    // same as dividing by 28.4, without slow floating-point maths. Adding
    // half (512) before dividing rounds to the NEAREST semitone.
    const int32_t cv = CVIn1();
    note += (cv * 36 + (cv >= 0 ? 512 : -512)) / 1024;

    // Keep within the range of MIDI notes.
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    return note;
  }

  // 1V per octave, with MIDI note 60 at 0V: every semitone is 1000/12 mV.
  static int32_t NoteToMillivolts(int note) { return (note - 60) * 1000 / 12; }

  // ---------------------------------------------------------------------------
  //  Things that happen every sample: pulse timing, glide, LEDs
  // ---------------------------------------------------------------------------
  void UpdateOutputs() {
    // ---- pulse outputs: on while their timers are counting down ----
    if (pulse1Timer_ > 0) pulse1Timer_--;
    if (pulse2Timer_ > 0) pulse2Timer_--;
    PulseOut1(pulse1Timer_ > 0);
    PulseOut2(pulse2Timer_ > 0);

    // ---- glide on CV Out 1 ----
    // We store the voltage in millivolts * 65536 ("fixed point") so there
    // are plenty of fractional bits for slow, smooth slides.
    //
    // Each sample we move a fraction of the remaining distance towards the
    // target: 1/1 (instant), 1/2, 1/4 ... 1/32768 (very slow). Dividing by
    // a power of two is just a right-shift (>>), which is very fast.
    const int shift = KnobVal(Knob::Y) >> 8;   // 0..4095 -> 0..15
    const int32_t difference = glideTarget_ - glideNow_;
    const int32_t move = difference >> shift;
    if (move == 0) glideNow_ = glideTarget_;   // close enough: arrive
    else           glideNow_ += move;
    CVOut1Millivolts(glideNow_ / 65536);

    // ---- audio outputs used as extra CVs ----
    // Audio outputs take -2048..2047. Scale the counts to fill 0..2047.
    AudioOut1(static_cast<int16_t>(population_ * 2047 / 64));
    AudioOut2(static_cast<int16_t>(columnCount_ * 2047 / 8));

    // ---- the six LEDs on the Computer ----
    //   0 Pulse Out 1      1 Pulse Out 2
    //   2 column density   3 grid population
    //   4 Launchpad connected   5 flashes on each step
    if (stepLedTimer_ > 0) stepLedTimer_--;
    LedOn(0, pulse1Timer_ > 0);
    LedOn(1, pulse2Timer_ > 0);
    LedBrightness(2, static_cast<uint16_t>(columnCount_ * 4095 / 8));
    LedBrightness(3, static_cast<uint16_t>(population_ * 4095 / 64));
    LedOn(4, g_status.launchpadConnected);
    LedOn(5, stepLedTimer_ > 0);
  }

  // ---------------------------------------------------------------------------
  //  Member variables (only ever touched by core 0)
  // ---------------------------------------------------------------------------
  //  Giving each one a value here ("= 0") means it starts with that value
  //  when the LifeSequencer object is created.
  Switch previousSwitch_ = Switch::Middle;   // where the switch was last
  int playhead_ = 0;
  bool resetPending_ = true;          // so the very first step plays column 0
  uint32_t handledResets_ = 0;
  int stepsSinceGeneration_ = 0;

  int32_t samplesSinceClock_ = kClockTimeout;   // "no clock seen yet"
  int32_t internalCounter_ = 0;
  int32_t stepPeriod_ = kSampleRate / 4;        // length of a step, in samples

  int32_t pulse1Timer_ = 0;
  int32_t pulse2Timer_ = 0;
  int32_t stepLedTimer_ = 0;

  int32_t glideTarget_ = 0;   // millivolts * 65536
  int32_t glideNow_ = 0;      // millivolts * 65536

  int columnCount_ = 0;
  int population_ = 0;

  uint32_t rng_ = 0x12345678;   // core 0's own random number generator
};

// =============================================================================
//  main() - where every C++ program starts
// =============================================================================
//  The card object is created as a global (outside main) rather than inside
//  main, because globals don't use the small stack memory.
static LifeSequencer card;

int main() {
  // Run the processor at 144 MHz, as the official USB-host example does.
  set_sys_clock_khz(144000, true);

  // Start core 1 running core1_entry() (USB + Launchpad + Game of Life).
  multicore_launch_core1(core1_entry);

  // Start the 48 kHz sample loop on core 0. Run() never returns: from now on
  // core 0 spends its life calling ProcessSample().
  card.Run();
}
