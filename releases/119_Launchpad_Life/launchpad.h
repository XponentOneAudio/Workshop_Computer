// =============================================================================
//  launchpad.h  -  Talking to a Novation Launchpad X over USB MIDI
// =============================================================================
//
//  HOW THE LAUNCHPAD X IS ORGANISED
//  --------------------------------
//  We put the Launchpad into "Programmer mode". In that mode every pad and
//  button has a fixed number, laid out like this (looking at the Launchpad):
//
//        91  92  93  94  95  96  97  98     99  <- logo LED (light only)
//      +---+---+---+---+---+---+---+---+
//      | 81| 82| 83| 84| 85| 86| 87| 88|    89  <- side button 1 (top)
//      | 71| 72| 73| 74| 75| 76| 77| 78|    79
//      | 61| 62| 63| 64| 65| 66| 67| 68|    69
//      | 51| 52| 53| 54| 55| 56| 57| 58|    59
//      | 41| 42| 43| 44| 45| 46| 47| 48|    49
//      | 31| 32| 33| 34| 35| 36| 37| 38|    39
//      | 21| 22| 23| 24| 25| 26| 27| 28|    29
//      | 11| 12| 13| 14| 15| 16| 17| 18|    19  <- side button 8 (bottom)
//      +---+---+---+---+---+---+---+---+
//
//  The tens digit is the ROW (1 = bottom) and the units digit is the COLUMN
//  (1 = left). So in our code, where x and y start at 0:
//        number = (y + 1) * 10 + (x + 1)
//
//  MESSAGES THE LAUNCHPAD SENDS US
//    pads (the 8x8 grid):    Note On,  note = pad number, velocity 0 = released
//    top + side buttons:     Control Change, controller = button number,
//                            value 127 = pressed, 0 = released
//
//  MESSAGES WE SEND THE LAUNCHPAD
//    We light LEDs with a "SysEx" (System Exclusive) message. SysEx is MIDI's
//    way of sending manufacturer-specific data. Ours starts with Novation's
//    ID and the Launchpad X's model number, then lists LEDs and their
//    colours as Red, Green, Blue amounts (each 0..127).
//
//  Everything about this protocol comes from Novation's "Launchpad X
//  Programmer's Reference Manual", which is free to download.
// =============================================================================

#pragma once

#include <stdint.h>

namespace lplife {

// -----------------------------------------------------------------------------
//  A colour: how much red, green and blue (each 0..127, because MIDI data
//  bytes can only go up to 127).
// -----------------------------------------------------------------------------
struct Colour {
  uint8_t r, g, b;

  // "operator==" teaches C++ what "colourA == colourB" means: all three
  // parts must match. We use it to spot LEDs that have changed.
  bool operator==(const Colour& other) const {
    return r == other.r && g == other.g && b == other.b;
  }
  bool operator!=(const Colour& other) const { return !(*this == other); }

  // Return a darker copy of this colour. 'shift' = 1 halves, 2 quarters...
  Colour Dim(int shift) const {
    return Colour{static_cast<uint8_t>(r >> shift),
                  static_cast<uint8_t>(g >> shift),
                  static_cast<uint8_t>(b >> shift)};
  }
};

// Some named colours, so the UI code can say kGreen instead of {0,127,0}.
// "inline constexpr" lets a constant live in a header that many files include.
inline constexpr Colour kOff     {0,   0,   0};
inline constexpr Colour kWhite   {127, 127, 127};
inline constexpr Colour kRed     {127, 0,   0};
inline constexpr Colour kOrange  {127, 40,  0};
inline constexpr Colour kYellow  {127, 110, 0};
inline constexpr Colour kGreen   {0,   127, 0};
inline constexpr Colour kCyan    {0,   110, 110};
inline constexpr Colour kBlue    {0,   20,  127};
inline constexpr Colour kPurple  {70,  0,   127};
inline constexpr Colour kPink    {127, 0,   60};

class Launchpad {
 public:
  // ---- LED numbering helpers (see the diagram above) ----------------------
  // "static" methods belong to the class itself, not to one object, so they
  // can be called as Launchpad::PadIndex(x, y) without a Launchpad object.
  static int PadIndex(int x, int y) { return (y + 1) * 10 + (x + 1); }
  static int TopIndex(int i)        { return 91 + i; }        // i = 0..7, left to right
  static int SideIndex(int i)       { return (8 - i) * 10 + 9; } // i = 0..7, top to bottom
  static constexpr int kLogoIndex = 99;

  // ---- connection --------------------------------------------------------
  // Called when the USB driver tells us a MIDI device has appeared/gone.
  void Attach(uint8_t devAddr, uint8_t numTxCables);
  void Detach();
  bool IsConnected() const { return devAddr_ != 0; }
  uint8_t DeviceAddress() const { return devAddr_; }

  // The Launchpad X shows up as TWO MIDI ports on one USB cable. USB MIDI
  // calls these "cables": cable 0 is "LPX DAW", cable 1 is "LPX MIDI".
  // We send our lights to whichever cable the Launchpad sends its button
  // presses on, so this is called whenever a message arrives.
  void SetOutputCable(uint8_t cable);

  // Put the Launchpad into Programmer mode and set its brightness.
  // Must be called once after it is plugged in.
  void Initialise();

  // ---- drawing -----------------------------------------------------------
  // We keep a "frame buffer": an array holding the colour we WANT every LED
  // to be. The UI paints into it with SetLed(), then calls Show(), which
  // sends ONLY the LEDs that changed since last time. Sending the whole
  // Launchpad every frame would waste lots of USB time.
  void SetLed(int index, Colour c);
  void Show();

  // Forget what we think the Launchpad is showing, so the next Show()
  // re-sends every LED. Used after (re)connecting.
  void ForgetDisplay();

 private:
  // Send raw MIDI bytes to the Launchpad. Waits (politely) if the USB
  // output buffer is full.
  void SendBytes(const uint8_t* data, uint32_t length);

  // Send a SysEx message: F0, Novation header, the payload, F7.
  void SendSysEx(const uint8_t* payload, uint32_t length);

  uint8_t devAddr_ = 0;       // USB address of the Launchpad (0 = not connected)
  uint8_t numTxCables_ = 0;   // how many MIDI "cables" we can send on
  uint8_t outCable_ = 0;      // which cable we send lights on

  // LED numbers go from 11 to 99, so an array of 100 lets us use the LED
  // number directly as the index (a few entries are simply never used).
  static constexpr int kNumLedSlots = 100;
  Colour wanted_[kNumLedSlots];   // what the UI wants to show
  Colour shown_[kNumLedSlots];    // what we last sent to the Launchpad
  bool   shownValid_[kNumLedSlots] = {};  // false = "we don't know", so send it
};

}  // namespace lplife
