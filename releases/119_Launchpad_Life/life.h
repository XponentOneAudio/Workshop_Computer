// =============================================================================
//  life.h  -  An 8 x 8 cellular automaton ("Game of Life" and friends)
// =============================================================================
//
//  HOW THE GAME OF LIFE WORKS
//  --------------------------
//  The grid is made of "cells". Each cell is either ALIVE or DEAD.
//  Every cell has 8 neighbours (up, down, left, right and the four diagonals).
//
//  To get the NEXT generation, every cell looks at how many of its neighbours
//  are alive right now, and the RULE decides what happens:
//
//    - a DEAD cell becomes alive ("is born") if the count is in the BIRTH list
//    - a LIVE cell stays alive ("survives") if the count is in the SURVIVE list
//    - otherwise the cell is dead in the next generation
//
//  John Conway's original rule is written "B3/S23":
//    Born with exactly 3 neighbours, Survives with 2 or 3.
//  Changing those numbers gives completely different behaviours - this card
//  lets you pick from eight rules on the RULE page.
//
//  CLASSES IN C++
//  --------------
//  A "class" bundles some data together with the functions ("methods") that
//  work on that data. Here the data is the grid, and the methods are things
//  like Step(), Randomise() and Toggle().
//
//  "public:" members can be used by any code. "private:" members can only be
//  used by the class's own methods - this protects the data from being
//  changed in ways the class doesn't expect.
// =============================================================================

#pragma once

#include <stdint.h>
#include "shared.h"

namespace lplife {

// -----------------------------------------------------------------------------
//  A rule, stored as two "bit masks".
// -----------------------------------------------------------------------------
//  Bit n of 'birth' is 1 if a dead cell with n live neighbours is born.
//  Bit n of 'survive' is 1 if a live cell with n live neighbours survives.
//  n can be 0..8, so we need 9 bits - a uint16_t (16 bits) is big enough.
//
//  Example, Conway B3/S23:
//     birth   = (1 << 3)            = binary 0000 0000 1000
//     survive = (1 << 2) | (1 << 3) = binary 0000 0000 1100
struct Rule {
  uint16_t birth;
  uint16_t survive;
};

// The eight rules offered on the RULE page. Defined in life.cpp.
// "extern const" = "a read-only table that lives in another file".
constexpr int kNumRules = 8;
extern const Rule kRules[kNumRules];

class Life {
 public:
  // A "constructor" has the same name as the class and runs automatically
  // when a Life object is created. Ours just clears the grid.
  Life();

  // ---- changing the grid -----------------------------------------------------
  void Clear();                                   // kill every cell
  void Randomise(uint32_t& rng, int percentAlive);// fill with random cells
  void Toggle(int x, int y);                      // flip one cell (pad press)

  // Work out the next generation using 'rule'.
  // If 'wrap' is true the grid is a torus: the left edge touches the right
  // edge and the top touches the bottom, so gliders fly off one side and
  // reappear on the other.
  void Step(const Rule& rule, bool wrap);

  // Randomly flip cells. 'amount' is 0..4095; at 4095 roughly one cell in
  // eight is flipped. Used for the "chaos" control (Main knob + CV In 2).
  void Mutate(uint32_t& rng, int amount);

  // ---- reading the grid ------------------------------------------------------
  // "const" after a method means "this method only LOOKS at the grid, it
  // never changes it". The compiler checks that promise for us.
  bool    IsAlive(int x, int y) const { return age_[y][x] != 0; }
  uint8_t Age(int x, int y) const     { return age_[y][x]; }
  bool    WasJustBorn(int x, int y) const { return age_[y][x] == 1; }
  int     Population() const;

  // Pack one row into a byte: bit x = cell (x, y) is alive.
  uint8_t RowBits(int y) const;
  // Same, but only cells born in the latest generation.
  uint8_t BirthBits(int y) const;

  // True if the same cells are alive in both grids (ages are ignored).
  bool SamePatternAs(const Life& other) const;

 private:
  // Count the live neighbours around (x, y).
  int CountNeighbours(int x, int y, bool wrap) const;

  // THE GRID ITSELF.
  // Instead of storing just alive/dead we store each cell's AGE:
  //    0      = dead
  //    1      = born in the latest generation (a "newborn")
  //    2, 3.. = has survived that many generations (stops counting at 255)
  // That gives us alive/dead for free (age != 0) AND lets the Launchpad
  // colour cells by how old they are.
  //
  // The trailing underscore in "age_" is a common naming habit that marks
  // a private member variable.
  //
  // Indexed [y][x]: row first, then column. Row 0 is the BOTTOM of the
  // Launchpad (lowest pitch) and column 0 is the LEFT.
  uint8_t age_[kGridHeight][kGridWidth];
};

// -----------------------------------------------------------------------------
//  A tiny random number generator ("xorshift").
// -----------------------------------------------------------------------------
//  Microcontrollers have no dice! This function scrambles the bits of 'state'
//  with shifts and XORs and returns the result. The sequence it makes looks
//  random enough for music. 'state' is passed by REFERENCE (the &), so the
//  function updates the caller's variable ready for next time.
//
//  "inline" lets us put the function body in a header without the linker
//  complaining that it is defined in several .cpp files.
inline uint32_t NextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

}  // namespace lplife
