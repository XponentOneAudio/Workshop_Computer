// =============================================================================
//  life.cpp  -  The Game of Life engine (see life.h for the explanation)
// =============================================================================

#include "life.h"

namespace lplife {

// -----------------------------------------------------------------------------
//  The eight rules on the RULE page, top side-button first.
// -----------------------------------------------------------------------------
//  A small helper to turn a list of neighbour counts into a bit mask, so the
//  table below can be written in the familiar "B3/S23" style.
//
//  This is a "variadic template": it accepts any number of arguments.
//  Don't worry if that is unfamiliar - all it does is
//      Bits(2, 3)  ->  (1 << 2) | (1 << 3)
//  and the compiler works it out before the program ever runs.
template <typename... Counts>
constexpr uint16_t Bits(Counts... counts) {
  return static_cast<uint16_t>((0 | ... | (1u << counts)));
}

const Rule kRules[kNumRules] = {
    // {birth,                 survive}
    {Bits(3),                  Bits(2, 3)},                    // 1 Conway's Life   B3/S23
    {Bits(3, 6),               Bits(2, 3)},                    // 2 HighLife        B36/S23
    {Bits(2),                  Bits()},                        // 3 Seeds           B2/S   (explosive!)
    {Bits(3, 6, 7, 8),         Bits(3, 4, 6, 7, 8)},           // 4 Day & Night     B3678/S34678
    {Bits(3),                  Bits(0, 1, 2, 3, 4, 5, 6, 7, 8)},// 5 Life w/o Death  B3/S012345678 (only grows)
    {Bits(3),                  Bits(1, 2, 3, 4, 5)},           // 6 Maze            B3/S12345
    {Bits(3, 6),               Bits(1, 2, 5)},                 // 7 2x2             B36/S125
    {Bits(3, 6, 8),            Bits(2, 4, 5)},                 // 8 Move (Morley)   B368/S245
};

// -----------------------------------------------------------------------------
//  Constructor
// -----------------------------------------------------------------------------
//  "Life::Life()" means "the Life() function that belongs to the Life class".
Life::Life() { Clear(); }

void Life::Clear() {
  // Two nested loops visit every cell: the outer loop goes up the rows,
  // the inner loop goes along each row.
  for (int y = 0; y < kGridHeight; y++) {
    for (int x = 0; x < kGridWidth; x++) {
      age_[y][x] = 0;
    }
  }
}

void Life::Randomise(uint32_t& rng, int percentAlive) {
  for (int y = 0; y < kGridHeight; y++) {
    for (int x = 0; x < kGridWidth; x++) {
      // NextRandom() gives a big number; "% 100" (remainder after dividing
      // by 100) turns it into 0..99. If that is below percentAlive, the
      // cell starts alive. So percentAlive = 30 makes about 30% alive.
      bool alive = (NextRandom(rng) % 100) < static_cast<uint32_t>(percentAlive);
      age_[y][x] = alive ? 1 : 0;   // "condition ? a : b" picks a if true, else b
    }
  }
}

void Life::Toggle(int x, int y) {
  // A cell drawn by hand counts as newborn (age 1), so it shows the newborn
  // colour and can fire the "births" output (Pulse Out 2).
  age_[y][x] = IsAlive(x, y) ? 0 : 1;
}

int Life::CountNeighbours(int x, int y, bool wrap) const {
  int count = 0;

  // Look at the 3 x 3 block centred on (x, y). dx and dy go -1, 0, +1.
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      if (dx == 0 && dy == 0) continue;   // skip the cell itself

      int nx = x + dx;   // neighbour's column
      int ny = y + dy;   // neighbour's row

      if (wrap) {
        // Wrap around the edges. Adding the width before taking the
        // remainder keeps the answer positive: (-1 + 8) % 8 = 7, so the
        // neighbour to the left of column 0 is column 7.
        nx = (nx + kGridWidth) % kGridWidth;
        ny = (ny + kGridHeight) % kGridHeight;
      } else {
        // No wrapping: anything outside the grid counts as dead.
        if (nx < 0 || nx >= kGridWidth || ny < 0 || ny >= kGridHeight) continue;
      }

      if (age_[ny][nx] != 0) count++;
    }
  }
  return count;
}

void Life::Step(const Rule& rule, bool wrap) {
  // IMPORTANT: every cell must decide its future based on the CURRENT
  // generation. If we updated cells in place, cells later in the loop would
  // see a mixture of old and new neighbours and the rule would break.
  //
  // So we first write the new generation into a temporary grid 'next', and
  // copy it back only when every cell has been decided.
  uint8_t next[kGridHeight][kGridWidth];

  for (int y = 0; y < kGridHeight; y++) {
    for (int x = 0; x < kGridWidth; x++) {
      int n = CountNeighbours(x, y, wrap);
      uint8_t age = age_[y][x];

      // "(rule.survive >> n) & 1" picks out bit number n of the mask:
      // shift it down to position 0, then keep only that bit.
      if (age != 0) {
        bool survives = (rule.survive >> n) & 1;
        // A survivor gets one generation older (but we stop at 255,
        // the biggest number a uint8_t can hold).
        next[y][x] = survives ? (age < 255 ? age + 1 : 255) : 0;
      } else {
        bool born = (rule.birth >> n) & 1;
        next[y][x] = born ? 1 : 0;
      }
    }
  }

  // Copy the new generation into place.
  for (int y = 0; y < kGridHeight; y++) {
    for (int x = 0; x < kGridWidth; x++) {
      age_[y][x] = next[y][x];
    }
  }
}

void Life::Mutate(uint32_t& rng, int amount) {
  if (amount <= 0) return;   // nothing to do - leave quickly

  for (int y = 0; y < kGridHeight; y++) {
    for (int x = 0; x < kGridWidth; x++) {
      // "& 32767" keeps the lowest 15 bits: a random number 0..32767.
      // With amount = 4095 the chance of a flip is 4095/32768 = 1 in 8.
      if (static_cast<int>(NextRandom(rng) & 32767) < amount) {
        Toggle(x, y);
      }
    }
  }
}

int Life::Population() const {
  int total = 0;
  for (int y = 0; y < kGridHeight; y++) {
    for (int x = 0; x < kGridWidth; x++) {
      if (IsAlive(x, y)) total++;
    }
  }
  return total;
}

uint8_t Life::RowBits(int y) const {
  uint8_t bits = 0;
  for (int x = 0; x < kGridWidth; x++) {
    // "|=" means "OR this bit in". (1 << x) is a byte with only bit x set.
    if (IsAlive(x, y)) bits |= static_cast<uint8_t>(1 << x);
  }
  return bits;
}

uint8_t Life::BirthBits(int y) const {
  uint8_t bits = 0;
  for (int x = 0; x < kGridWidth; x++) {
    if (WasJustBorn(x, y)) bits |= static_cast<uint8_t>(1 << x);
  }
  return bits;
}

bool Life::SamePatternAs(const Life& other) const {
  for (int y = 0; y < kGridHeight; y++) {
    if (RowBits(y) != other.RowBits(y)) return false;  // found a difference
  }
  return true;
}

}  // namespace lplife
