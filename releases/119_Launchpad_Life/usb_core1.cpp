// =============================================================================
//  usb_core1.cpp  -  Everything that runs on CORE 1
// =============================================================================
//
//  Core 1 runs one endless loop (core1_entry, at the bottom of this file):
//
//     forever:
//        1. let TinyUSB do its work           (tuh_task)
//        2. read any MIDI bytes from the Launchpad and act on them
//        3. if a Launchpad has just been plugged in, set it up
//        4. carry out requests from core 0    (ui.Update)
//        5. every 20 ms, redraw the Launchpad (ui.Draw + launchpad.Show)
//
//  USB HOST
//  --------
//  Normally a computer is the USB "host" and gadgets like a Launchpad are
//  "devices". Here the Workshop Computer is the host: it powers the Launchpad
//  through its USB-C socket and talks to it directly, no laptop required.
//
//  We use TinyUSB (built into the Pico SDK) plus a small USB-MIDI host driver
//  written by rppicomidi (usb_midi_host.c). That driver calls the four
//  "callback" functions below when things happen - a device is plugged in,
//  unplugged, or sends data.
// =============================================================================

#include "pico/time.h"
#include "bsp/board_api.h"   // board_init()
#include "tusb.h"            // TinyUSB
#include "usb_midi_host.h"   // the USB MIDI host driver

#include "launchpad.h"
#include "shared.h"
#include "ui.h"

using namespace lplife;   // lets us write "UI" instead of "lplife::UI" in this file

// -----------------------------------------------------------------------------
//  The objects core 1 works with.
// -----------------------------------------------------------------------------
//  "static" at file level means "private to this file" - no other .cpp file
//  can see these two names.
static Launchpad s_launchpad;
static UI s_ui;

// Set by the "mount" callback, handled in the main loop. We don't set up the
// Launchpad inside the callback itself because callbacks should be quick
// (see the note on the receive buffer below).
static volatile bool s_needsInitialise = false;

// =============================================================================
//  Receive buffer ("ring buffer")
// =============================================================================
//  When the Launchpad sends us bytes, the driver calls tuh_midi_rx_cb().
//  That callback must be FAST: the USB driver is waiting for it to return
//  before it can listen for the next USB packet. So the callback only copies
//  the bytes into this ring buffer, and the main loop reads them out later.
//
//  A RING BUFFER is an array used in a circle. 'head' is where the next byte
//  is written; 'tail' is where the next byte is read. When either index
//  reaches the end it wraps back to 0. If head == tail the buffer is empty.
//
//      [ . . . T x x x x H . . . ]    x = bytes waiting to be read
//
//  The size is a power of two (256), so "& kRxMask" wraps an index round to 0
//  - a fast way of writing "% 256".
static constexpr uint32_t kRxSize = 256;
static constexpr uint32_t kRxMask = kRxSize - 1;
static uint8_t s_rxBuffer[kRxSize];
static volatile uint32_t s_rxHead = 0;
static volatile uint32_t s_rxTail = 0;

// The USB-MIDI "cable" (virtual port) the last bytes arrived on, and
// whether anything has arrived yet at all.
static volatile uint8_t s_rxCable = 0;
static volatile bool s_rxCableKnown = false;

// =============================================================================
//  Callbacks from the USB MIDI driver
// =============================================================================
//  These four functions are called BY the driver. Their names and parameters
//  must match exactly what usb_midi_host.h declares, or they won't be found.

// A MIDI device has been plugged in and is ready.
void tuh_midi_mount_cb(uint8_t dev_addr, uint8_t in_ep, uint8_t out_ep,
                       uint8_t num_cables_rx, uint16_t num_cables_tx) {
  // "(void)x" tells the compiler "I know I'm not using x, don't warn me".
  (void)in_ep;
  (void)out_ep;
  (void)num_cables_rx;

  s_launchpad.Attach(dev_addr, static_cast<uint8_t>(num_cables_tx));
  s_rxCableKnown = false;     // a new device: forget the old one's cable
  s_needsInitialise = true;   // finish setting it up in the main loop
}

// A MIDI device has been unplugged.
void tuh_midi_umount_cb(uint8_t dev_addr, uint8_t instance) {
  (void)instance;

  if (dev_addr == s_launchpad.DeviceAddress()) {
    s_launchpad.Detach();
    g_status.launchpadConnected = false;
  }
}

// The device has sent us some MIDI. Copy it into the ring buffer and return.
void tuh_midi_rx_cb(uint8_t dev_addr, uint32_t num_packets) {
  if (num_packets == 0) return;
  if (dev_addr != s_launchpad.DeviceAddress()) return;   // not our Launchpad

  uint8_t cable;
  uint8_t bytes[64];
  uint32_t total = 0;

  // Read what has arrived, but never more than one USB packet's worth
  // (64 bytes) - see the note above about keeping this callback short.
  while (total < sizeof(bytes)) {
    uint32_t count = tuh_midi_stream_read(dev_addr, &cable, bytes, sizeof(bytes));
    if (count == 0) break;   // nothing left to read
    total += count;
    s_rxCable = cable;
    s_rxCableKnown = true;

    uint32_t head = s_rxHead;
    for (uint32_t i = 0; i < count; i++) {
      uint32_t next = (head + 1) & kRxMask;
      if (next == s_rxTail) break;   // buffer full: drop the rest rather than wait
      s_rxBuffer[head] = bytes[i];
      head = next;
    }
    s_rxHead = head;
  }
}

// The driver has finished sending something. We don't need to know.
void tuh_midi_tx_cb(uint8_t dev_addr) { (void)dev_addr; }

// =============================================================================
//  MIDI parser
// =============================================================================
//  MIDI arrives as a stream of bytes. A message is a STATUS byte (top bit
//  set, 0x80..0xFF) followed by DATA bytes (top bit clear, 0x00..0x7F):
//
//     0x90 note velocity     Note On  (channel 1)
//     0x80 note velocity     Note Off (channel 1)
//     0xB0 controller value  Control Change (channel 1)
//
//  "Running status": a device may send the status byte once and then several
//  pairs of data bytes, which all reuse that status. So we remember the last
//  status byte and count data bytes as they arrive.

static uint8_t s_status = 0;      // the current status byte (0 = none)
static uint8_t s_data[2];         // the data bytes collected so far
static int     s_dataCount = 0;

// Called once a complete three-byte message has arrived.
static void HandleMessage(uint8_t status, uint8_t d1, uint8_t d2) {
  // The top four bits are the message type; the bottom four the channel.
  // We ignore the channel - the Launchpad sends everything on channel 1.
  const uint8_t type = status & 0xF0;

  if (type == 0x90 || type == 0x80) {
    // A grid pad. Note number = (row + 1) * 10 + (column + 1).
    // A Note On with velocity 0 means "released", as does a Note Off.
    bool pressed = (type == 0x90) && (d2 > 0);
    int x = (d1 % 10) - 1;   // units digit -> column
    int y = (d1 / 10) - 1;   // tens digit  -> row
    if (x >= 0 && x < kGridWidth && y >= 0 && y < kGridHeight) {
      s_ui.OnPad(x, y, pressed);
    }
  } else if (type == 0xB0) {
    // A top or side button. Value > 0 means pressed.
    bool pressed = d2 > 0;
    if (d1 >= 91 && d1 <= 98) {
      s_ui.OnTopButton(d1 - 91, pressed);               // 91..98 -> 0..7
    } else if (d1 % 10 == 9 && d1 >= 19 && d1 <= 89) {
      s_ui.OnSideButton(8 - d1 / 10, pressed);          // 89 -> 0 ... 19 -> 7
    }
  }
  // Everything else (aftertouch, SysEx replies...) is ignored.
}

static void ParseByte(uint8_t b) {
  if (b >= 0xF8) return;   // real-time messages (clock etc): ignore, keep status

  if (b & 0x80) {          // a status byte (top bit set)
    // 0xF0..0xF7 are SysEx/system messages, which we don't use. Setting
    // s_status to 0 makes us ignore their data bytes.
    s_status = (b >= 0xF0) ? 0 : b;
    s_dataCount = 0;
    return;
  }

  if (s_status == 0) return;   // a data byte with no status: ignore it

  s_data[s_dataCount++] = b;

  // Program Change (0xC0) and Channel Pressure (0xD0) have ONE data byte;
  // all the others we might see have TWO.
  const uint8_t type = s_status & 0xF0;
  const int needed = (type == 0xC0 || type == 0xD0) ? 1 : 2;

  if (s_dataCount == needed) {
    if (needed == 2) HandleMessage(s_status, s_data[0], s_data[1]);
    s_dataCount = 0;   // ready for the next message (running status)
  }
}

// =============================================================================
//  Core 1 entry point
// =============================================================================
//  main() (in main.cpp) starts core 1 by calling this function. It never
//  returns. 'extern "C"' gives it a plain C name, the style the Pico SDK uses.
extern "C" void core1_entry() {
  // Give the power supply and USB hardware a moment to settle after
  // switch-on. Other USB-host cards for the Workshop Computer do the same.
  sleep_ms(150);

  board_init();   // basic board setup that TinyUSB expects
  tusb_init();    // start TinyUSB (in host mode, see tusb_config.h)

  // Use the current time (in microseconds) as a random seed, so every
  // power-on starts from a different random grid.
  s_ui.Init(time_us_32());

  uint32_t nextDrawTime = 0;
  const uint32_t kDrawIntervalMicros = 20000;   // 20 ms = 50 frames a second

  while (true) {
    // 1. Let TinyUSB handle plugging, unplugging and data transfers.
    tuh_task();

    // 2. Read and act on any bytes the Launchpad has sent.
    while (s_rxTail != s_rxHead) {
      ParseByte(s_rxBuffer[s_rxTail]);
      s_rxTail = (s_rxTail + 1) & kRxMask;
    }
    // Send our lights back on the same virtual cable the Launchpad uses
    // (once it has sent us something, so we know which one that is).
    if (s_rxCableKnown) s_launchpad.SetOutputCable(s_rxCable);

    // 3. A Launchpad was just plugged in (or the panel switch asked us to
    //    refresh it): put it in Programmer mode.
    if (s_ui.TakeLaunchpadRefreshRequest()) s_needsInitialise = true;

    if (s_needsInitialise && s_launchpad.IsConnected() &&
        tuh_midi_configured(s_launchpad.DeviceAddress())) {
      s_needsInitialise = false;
      s_launchpad.Initialise();
      g_status.launchpadConnected = true;
    }

    // 4. Do whatever core 0 has asked for (new generations, etc).
    s_ui.Update();

    // 5. Redraw the Launchpad 50 times a second.
    uint32_t now = time_us_32();
    if (static_cast<int32_t>(now - nextDrawTime) >= 0) {
      nextDrawTime = now + kDrawIntervalMicros;
      s_ui.Draw(s_launchpad, now);
      s_launchpad.Show();
    }

    // Push any MIDI we've queued out over USB now, rather than waiting for
    // the driver's buffer to fill up.
    if (s_launchpad.IsConnected()) tuh_midi_stream_flush(s_launchpad.DeviceAddress());
  }
}
