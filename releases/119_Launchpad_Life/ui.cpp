// =============================================================================
//  ui.cpp  -  The paged Launchpad interface (see ui.h for the overview)
// =============================================================================

#include "ui.h"

#include "hardware/sync.h"   // for __dmb(), the "memory barrier" used in Publish()

namespace lplife {

// -----------------------------------------------------------------------------
//  Colours for each page. The top-row button for a page lights in this
//  colour, and so do that page's side buttons, so you can always tell
//  which page you're on at a glance.
// -----------------------------------------------------------------------------
static const Colour kPageColours[kNumPages] = {
    kGreen,    // PLAY
    kRed,      // RULE
    kOrange,   // EVOLVE
    kYellow,   // TEMPO
    kCyan,     // LENGTH
    kBlue,     // SCALE
    kPurple,   // ROOT
    kPink,     // OPTIONS
};

// The semitone (counting up from C) for each natural note C D E F G A B.
static const uint8_t kNaturalSemitones[7] = {0, 2, 4, 5, 7, 9, 11};

// Colours of live cells, by age. Young cells are green, then they cool to
// blue and finally purple as they get older.
static constexpr Colour kNewbornColour  {30, 127, 10};
static constexpr Colour kYoungColour    {0,  70, 127};
static constexpr Colour kOldColour      {60, 0,  110};
static constexpr Colour kPlayheadEmpty  {10, 10, 10};   // faint column marker

// How long the logo LED flashes on each step, in microseconds (40 ms).
static constexpr uint32_t kLogoFlashMicros = 40000;

// =============================================================================
//  Setting up
// =============================================================================
void UI::Init(uint32_t randomSeed) {
  // The random generator must never start at 0 (xorshift would then only
  // ever produce 0). "| 1" makes sure the lowest bit is set.
  rng_ = randomSeed | 1;

  // Start with a random grid so there's something to hear straight away.
  RandomiseGrid();
}

bool UI::TakeLaunchpadRefreshRequest() {
  bool requested = launchpadRefreshRequested_;
  launchpadRefreshRequested_ = false;
  return requested;
}

// =============================================================================
//  Input
// =============================================================================
void UI::OnPad(int x, int y, bool pressed) {
  // We only care about the moment a pad is pressed, not when it is released.
  if (!pressed) return;

  life_.Toggle(x, y);
  Publish();   // let core 0 hear the change straight away
}

void UI::OnTopButton(int index, bool pressed) {
  // A top button simply chooses the page.
  if (pressed) page_ = static_cast<uint8_t>(index);
}

void UI::OnSideButton(int index, bool pressed) {
  // Remember held buttons so they can light up white while pressed -
  // it's nice to see that the Launchpad noticed you.
  sideHeld_[index] = pressed;
  if (!pressed) return;

  // A "switch" statement picks one block of code depending on a value.
  // Each "case" is one possible value; "break" jumps to the end.
  switch (page_) {
    case PagePlay:
      PressOnPlayPage(index);
      break;

    // On these five pages the side buttons just choose one of eight values.
    // The tables those values index into live in main.cpp (core 0).
    case PageRule:
      g_settings.ruleIndex = static_cast<uint8_t>(index);
      break;
    case PageEvolve:
      g_settings.evolveIndex = static_cast<uint8_t>(index);
      break;
    case PageTempo:
      g_settings.tempoIndex = static_cast<uint8_t>(index);
      break;
    case PageLength:
      g_settings.length = static_cast<uint8_t>(index + 1);  // 1..8, not 0..7
      break;
    case PageScale:
      g_settings.scaleIndex = static_cast<uint8_t>(index);
      break;

    case PageRoot:
      PressOnRootPage(index);
      break;

    case PageOptions:
      // Flip the option's bit with XOR (^): 0 becomes 1 and 1 becomes 0.
      g_settings.options = static_cast<uint8_t>(g_settings.options ^ (1 << index));
      break;
  }
}

// -----------------------------------------------------------------------------
//  PLAY page - side buttons, top to bottom
// -----------------------------------------------------------------------------
void UI::PressOnPlayPage(int index) {
  switch (index) {
    case 0:  // RUN / STOP
      g_settings.running = !g_settings.running;
      break;

    case 1:  // NEXT GENERATION - evolve once, by hand
      NextGeneration();
      break;

    case 2:  // RESET - send the playhead back to the start of the loop.
      // Core 0 owns the playhead, so we ASK it to do this (see shared.h).
      g_status.resetRequests = g_status.resetRequests + 1;
      break;

    case 3:  // RANDOMISE the grid
      RandomiseGrid();
      break;

    case 4:  // CLEAR the grid
      life_.Clear();
      Publish();
      break;

    case 5:  // SAVE a snapshot of the grid
      // Assigning one object to another copies all of its data.
      snapshot_ = life_;
      haveSnapshot_ = true;
      break;

    case 6:  // RECALL the snapshot
      if (haveSnapshot_) {
        life_ = snapshot_;
        Publish();
      }
      break;

    case 7:  // FREEZE - keep playing, but stop evolving
      g_settings.freeze = !g_settings.freeze;
      break;
  }
}

// -----------------------------------------------------------------------------
//  ROOT page - buttons 1..7 pick C D E F G A B, button 8 toggles sharp (#)
// -----------------------------------------------------------------------------
void UI::PressOnRootPage(int index) {
  if (index < 7) {
    rootNatural_ = static_cast<uint8_t>(index);
  } else {
    rootSharp_ = !rootSharp_;
  }

  // Work out the semitone. "% 12" wraps B# (11 + 1 = 12) round to C (0).
  int semitone = kNaturalSemitones[rootNatural_] + (rootSharp_ ? 1 : 0);
  g_settings.rootNote = static_cast<uint8_t>(semitone % 12);
}

// =============================================================================
//  Regular jobs
// =============================================================================
void UI::Update() {
  // ---- has core 0 asked for new generations? ----
  uint32_t requested = g_status.generationRequests;

  // If we have fallen a long way behind (it shouldn't happen, but USB can
  // occasionally keep core 1 busy), skip ahead rather than computing a
  // burst of generations all at once.
  //
  // Note the unsigned subtraction: even when the counter wraps round from
  // 4,294,967,295 back to 0, "requested - handled" still gives the right
  // difference. That is a handy property of unsigned numbers in C++.
  if (requested - handledGenerations_ > 4) handledGenerations_ = requested - 1;

  while (handledGenerations_ != requested) {
    handledGenerations_++;
    NextGeneration();
  }

  // ---- has the panel switch been pushed down? ----
  uint32_t switchDowns = g_status.switchDownCount;
  if (switchDowns != handledSwitchDowns_) {
    handledSwitchDowns_ = switchDowns;
    RandomiseGrid();
    // Also re-send Programmer mode to the Launchpad, in case it was knocked
    // out of it (for example by opening its Settings menu).
    launchpadRefreshRequested_ = true;
  }
}

void UI::NextGeneration() {
  // Remember the last two generations so we can spot a stuck pattern.
  previousButOne_ = previous_;
  previous_ = life_;

  const uint8_t options = g_settings.options;
  const Rule& rule = kRules[g_settings.ruleIndex];   // "&" = refer to it, don't copy it

  life_.Step(rule, (options & OptWrap) != 0);

  // Chaos: randomly flip a few cells (Main knob + CV In 2 on the panel).
  life_.Mutate(rng_, g_status.chaos);

  // Auto-reseed: if the grid has died, or is repeating itself every one or
  // two generations, for 4 generations in a row, throw in a fresh random grid.
  if (options & OptAutoReseed) {
    bool stuck = life_.Population() == 0 ||
                 life_.SamePatternAs(previous_) ||
                 life_.SamePatternAs(previousButOne_);

    stuckGenerations_ = stuck ? stuckGenerations_ + 1 : 0;

    if (stuckGenerations_ >= 4) {
      stuckGenerations_ = 0;
      RandomiseGrid();
      return;   // RandomiseGrid() has already published
    }
  }

  Publish();
}

void UI::RandomiseGrid() {
  life_.Randomise(rng_, 30);   // about 30% of cells alive
  stuckGenerations_ = 0;
  Publish();
}

void UI::Publish() {
  // Copy the grid into the buffer core 0 is NOT reading ("1 - front" turns
  // 0 into 1 and 1 into 0)...
  const uint8_t back = static_cast<uint8_t>(1 - g_grid.front);

  for (int y = 0; y < kGridHeight; y++) {
    g_grid.rows[back][y]   = life_.RowBits(y);
    g_grid.births[back][y] = life_.BirthBits(y);
  }
  g_grid.population[back] = static_cast<uint8_t>(life_.Population());

  // ...then a "memory barrier". This guarantees that all the writes above
  // have really reached memory BEFORE the write below, so core 0 can never
  // see the new 'front' while the data is still on its way.
  __dmb();

  // ...and finally flip 'front' so core 0 starts using the new copy.
  g_grid.front = back;
}

// =============================================================================
//  Drawing
// =============================================================================
void UI::Draw(Launchpad& lp, uint32_t nowMicros) {
  DrawGrid(lp);
  DrawTopRow(lp);
  DrawSideButtons(lp);
  DrawLogo(lp, nowMicros);
}

Colour UI::CellColour(int x, int y) const {
  const int playhead = g_status.playhead;
  const int length = g_settings.length;

  if (!life_.IsAlive(x, y)) {
    // Dead cells are dark, except for a faint stripe showing the playhead.
    return (x == playhead) ? kPlayheadEmpty : kOff;
  }

  // Live cells in the column being played flash white - this is the
  // "now playing" highlight.
  if (x == playhead) return kWhite;

  // Otherwise colour by age.
  int age = life_.Age(x, y);
  Colour c = (age == 1) ? kNewbornColour
           : (age <= 3) ? kYoungColour
                        : kOldColour;

  // Columns outside the loop are never played, so show them dimmer.
  if (x >= length) c = c.Dim(2);
  return c;
}

void UI::DrawGrid(Launchpad& lp) {
  for (int y = 0; y < kGridHeight; y++) {
    for (int x = 0; x < kGridWidth; x++) {
      lp.SetLed(Launchpad::PadIndex(x, y), CellColour(x, y));
    }
  }
}

void UI::DrawTopRow(Launchpad& lp) {
  for (int i = 0; i < kNumPages; i++) {
    // The current page is bright; the others glow dimly in their colour.
    Colour c = (i == page_) ? kPageColours[i] : kPageColours[i].Dim(3);
    lp.SetLed(Launchpad::TopIndex(i), c);
  }
}

void UI::DrawSideButtons(Launchpad& lp) {
  const Colour pageColour = kPageColours[page_];
  const Colour dim = pageColour.Dim(3);

  for (int i = 0; i < kNumSideButtons; i++) {
    Colour c = kOff;

    switch (page_) {
      case PagePlay: {
        // Each action has its own colour. Toggles are bright when on.
        // (The curly braces give this "case" its own little scope.)
        static const Colour kActionColours[8] = {
            kGreen, kYellow, kCyan, kOrange, kRed, kBlue, kPurple, kWhite};
        c = kActionColours[i].Dim(1);
        if (i == 0) c = g_settings.running ? kGreen : kRed;
        if (i == 6 && !haveSnapshot_) c = kPurple.Dim(4);
        if (i == 7) c = g_settings.freeze ? kWhite : kWhite.Dim(4);
        break;
      }

      // "Pick one" pages: the chosen button bright, the rest dim.
      case PageRule:   c = (i == g_settings.ruleIndex)   ? pageColour : dim; break;
      case PageEvolve: c = (i == g_settings.evolveIndex) ? pageColour : dim; break;
      case PageTempo:  c = (i == g_settings.tempoIndex)  ? pageColour : dim; break;
      case PageScale:  c = (i == g_settings.scaleIndex)  ? pageColour : dim; break;

      // LENGTH shows a bar: every button up to the chosen length is lit.
      case PageLength: c = (i < g_settings.length) ? pageColour : dim; break;

      case PageRoot:
        if (i < 7) c = (i == rootNatural_) ? pageColour : dim;
        else       c = rootSharp_ ? kWhite : kWhite.Dim(4);
        break;

      case PageOptions:
        c = (g_settings.options & (1 << i)) ? pageColour : dim;
        break;
    }

    if (sideHeld_[i]) c = kWhite;   // held buttons light white
    lp.SetLed(Launchpad::SideIndex(i), c);
  }
}

void UI::DrawLogo(Launchpad& lp, uint32_t nowMicros) {
  // Start a flash whenever core 0 reports a new step.
  uint32_t steps = g_status.stepCount;
  if (steps != lastStepCount_) {
    lastStepCount_ = steps;
    logoFlashUntil_ = nowMicros + kLogoFlashMicros;
  }

  // "(int32_t)(a - b) > 0" is a wrap-safe way of asking "is a later than b?"
  // for a timer that counts up and eventually wraps round to zero.
  bool flashing = static_cast<int32_t>(logoFlashUntil_ - nowMicros) > 0;

  Colour c;
  if (!g_settings.running)       c = kRed.Dim(3);    // stopped: dull red
  else if (!flashing)            c = kOff;
  else if (g_status.externalClock) c = kOrange;      // following Pulse In 1
  else                           c = kWhite;         // internal clock
  lp.SetLed(Launchpad::kLogoIndex, c);
}

}  // namespace lplife
