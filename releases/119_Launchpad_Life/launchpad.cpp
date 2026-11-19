// =============================================================================
//  launchpad.cpp  -  Launchpad X driver (see launchpad.h for the overview)
// =============================================================================
//
//  Everything in this file runs on CORE 1, the core that owns the USB host.
// =============================================================================

#include "launchpad.h"

#include "tusb.h"            // TinyUSB, the USB library in the Pico SDK
#include "usb_midi_host.h"   // the USB MIDI host driver (by rppicomidi)

namespace lplife {

// -----------------------------------------------------------------------------
//  Constants from the Launchpad X Programmer's Reference
// -----------------------------------------------------------------------------
//  Every Launchpad X SysEx message starts with these bytes after the F0:
//     00 20 29  = Novation/Focusrite's manufacturer ID
//     02 0C     = "Launchpad X"
static const uint8_t kSysExHeader[] = {0x00, 0x20, 0x29, 0x02, 0x0C};

// Command numbers that come straight after the header:
static constexpr uint8_t kCmdLedLighting   = 0x03;  // set LED colours
static constexpr uint8_t kCmdBrightness    = 0x08;  // overall brightness 0..127
static constexpr uint8_t kCmdProgrammerMode = 0x0E; // 0 = Live mode, 1 = Programmer mode

// Inside an LED-lighting message, each LED is described by a "colour spec".
// Type 3 means "RGB": followed by LED number, red, green, blue.
static constexpr uint8_t kLightingTypeRgb = 3;

// How bright the whole Launchpad should be, 0..127.
// The Workshop Computer powers the Launchpad through its USB socket. Full
// brightness with every pad lit draws a lot of current, so we turn it down
// a little. Raise this if your Launchpad looks dim and everything still
// behaves; lower it if the Launchpad resets or flickers.
static constexpr uint8_t kBrightness = 96;

// How many LEDs we describe in one SysEx message. Smaller messages fit
// comfortably in the USB output buffer; 16 LEDs = 7 + 16*5 + 1 = 88 bytes.
static constexpr int kLedsPerMessage = 16;

// -----------------------------------------------------------------------------
//  Connection
// -----------------------------------------------------------------------------
void Launchpad::Attach(uint8_t devAddr, uint8_t numTxCables) {
  devAddr_ = devAddr;
  numTxCables_ = numTxCables;

  // Until the Launchpad tells us otherwise (by sending us a button press),
  // guess cable 1 - the "LPX MIDI" port that Programmer mode uses. A device
  // with only one cable must use cable 0.
  outCable_ = (numTxCables_ >= 2) ? 1 : 0;

  ForgetDisplay();
}

void Launchpad::Detach() {
  devAddr_ = 0;
  numTxCables_ = 0;
}

void Launchpad::SetOutputCable(uint8_t cable) {
  // Only accept cables that actually exist on this device.
  if (cable < numTxCables_) outCable_ = cable;
}

void Launchpad::Initialise() {
  if (!IsConnected()) return;

  // Switch to Programmer mode. We send it on EVERY cable, because we can't be
  // sure which port the Launchpad listens to for this command - sending it
  // twice does no harm.
  const uint8_t programmerMode[] = {kCmdProgrammerMode, 1};
  uint8_t savedCable = outCable_;
  for (uint8_t cable = 0; cable < numTxCables_; cable++) {
    outCable_ = cable;
    SendSysEx(programmerMode, sizeof(programmerMode));
  }
  outCable_ = savedCable;

  // Set the overall LED brightness.
  const uint8_t brightness[] = {kCmdBrightness, kBrightness};
  SendSysEx(brightness, sizeof(brightness));

  // Entering Programmer mode clears the LEDs, so make sure the next Show()
  // repaints everything.
  ForgetDisplay();
}

// -----------------------------------------------------------------------------
//  Drawing
// -----------------------------------------------------------------------------
void Launchpad::SetLed(int index, Colour c) {
  // Guard against a bad index - writing outside an array is one of the most
  // common (and nastiest) bugs in C++, because nothing stops you!
  if (index < 0 || index >= kNumLedSlots) return;
  wanted_[index] = c;
}

void Launchpad::ForgetDisplay() {
  for (int i = 0; i < kNumLedSlots; i++) shownValid_[i] = false;
}

void Launchpad::Show() {
  if (!IsConnected()) return;

  // We build up a lighting message in this buffer:
  //   [0x03] [type idx r g b] [type idx r g b] ...
  // (SendSysEx adds the F0, the header and the F7 around it.)
  uint8_t message[1 + kLedsPerMessage * 5];
  int length = 0;
  int ledsInMessage = 0;

  message[length++] = kCmdLedLighting;   // "length++" uses length, THEN adds 1

  // Visit every LED number that exists on the Launchpad X (11..99).
  for (int index = 11; index <= 99; index++) {
    // Skip numbers that aren't real LEDs: units digit 0 (10, 20, ...),
    // which don't exist in the layout.
    if (index % 10 == 0) continue;

    // Has this LED changed since we last sent it? If not, skip it.
    if (shownValid_[index] && shown_[index] == wanted_[index]) continue;

    // Add this LED to the message.
    message[length++] = kLightingTypeRgb;
    message[length++] = static_cast<uint8_t>(index);
    message[length++] = wanted_[index].r & 0x7F;  // "& 0x7F" keeps it 0..127,
    message[length++] = wanted_[index].g & 0x7F;  // because a MIDI data byte
    message[length++] = wanted_[index].b & 0x7F;  // must never exceed 127
    ledsInMessage++;

    // Remember that the Launchpad now shows this colour.
    shown_[index] = wanted_[index];
    shownValid_[index] = true;

    // Message full? Send it and start a new one.
    if (ledsInMessage == kLedsPerMessage) {
      SendSysEx(message, length);
      length = 1;          // keep the 0x03 command byte at the start
      ledsInMessage = 0;
      if (!IsConnected()) return;   // unplugged while we were sending
    }
  }

  // Send whatever is left over.
  if (ledsInMessage > 0) SendSysEx(message, length);
}

// -----------------------------------------------------------------------------
//  Low-level sending
// -----------------------------------------------------------------------------
void Launchpad::SendSysEx(const uint8_t* payload, uint32_t length) {
  const uint8_t start = 0xF0;   // "SysEx starts here"
  const uint8_t end   = 0xF7;   // "SysEx ends here"
  SendBytes(&start, 1);
  SendBytes(kSysExHeader, sizeof(kSysExHeader));
  SendBytes(payload, length);
  SendBytes(&end, 1);
}

void Launchpad::SendBytes(const uint8_t* data, uint32_t length) {
  uint32_t sent = 0;

  // A safety limit, so that a misbehaving device can never trap us in this
  // loop forever.
  int attempts = 0;

  while (sent < length && IsConnected() && attempts < 1000) {
    // tuh_midi_stream_write() copies as many bytes as will fit into the
    // driver's output buffer and tells us how many it took.
    //  "data + sent" is "pointer arithmetic": the address 'sent' bytes
    //  further along the array, i.e. the first byte not yet sent.
    sent += tuh_midi_stream_write(devAddr_, outCable_, data + sent, length - sent);

    if (sent < length) {
      // The buffer is full. Ask the driver to start sending it over USB,
      // then let TinyUSB do its work (tuh_task) so space frees up.
      tuh_midi_stream_flush(devAddr_);
      tuh_task();
      attempts++;
    }
  }
}

}  // namespace lplife
