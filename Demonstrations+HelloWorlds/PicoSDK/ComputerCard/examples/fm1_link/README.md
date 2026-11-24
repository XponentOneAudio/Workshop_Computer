# FM1 Link: Workshop Computer ↔ M-VAVE FM1 over USB

This example card, and the small framework behind it, connects a Workshop Computer to an M-VAVE FM1 with a single USB cable. The Computer acts as the USB host.

- **MIDI both ways.** This works with any FM1 firmware, because the FM1 is a class-compliant USB-MIDI device.
- **A live readout from the Computer on the FM1's 240×240 screen.** You get labelled bars, numbers, indicators, text and an oscilloscope trace, and the FM1's encoders and buttons are sent back to the card. This part needs FM1 firmware built from `fm1_open` with the Workshop Link page (branch `ccr-c74800d3-7ogch8` of `XponentOneAudio/fm1_open`).

Status: the card builds for the Workshop Computer against the repo's pinned toolchain (Pico SDK 2.2.0, TinyUSB 0.20.0). The FM1 side compiles under the same `-Werror` flags as the FM1 web simulator. The two sides were tested together in simulation (see [Testing without hardware](#testing-without-hardware)). **They have not yet been run on a real Computer and FM1.** See [Before trusting it on hardware](#before-trusting-it-on-hardware).

## What the example card does

| Direction | Source | Destination |
|---|---|---|
| FM1 → Computer | notes on MIDI channel 1 | CV Out 1 (1V/oct), Pulse Out 1 (gate) |
| | velocity | CV Out 2 |
| | MIDI clock | Pulse Out 2 (16th-note triggers) |
| Computer → FM1 | Pulse In 1 + CV In 1 (pitch) | notes on MIDI channel 2 (FM1 track 2) |
| | Knob X | CC 1 on channel 2 (whether this does anything depends on the FM1 firmware) |
| FM1 screen | knobs, CV ins, pulse ins, switch, last note received, scope of Audio In 1 | Knob Y sets the scope timebase |
| FM1 encoder 1 | (while the link page is open) | transposes the notes the Computer sends |

To get notes out of the FM1, go to **SEQ → 6/8 MIDI TRACK** and set track 1 to send MIDI over USB.

On the FM1, **HOME + ARP** opens the link page and **HOME** leaves it. The card asks for the page to open when it connects. The FM1 only obeys that request while it is on its HOME overview, so it never interrupts an editor.

LEDs on the Computer: top-left means a MIDI device is attached, top-right means a Workshop Link session is running, the middle pair shows gate out and clock out, and the bottom pair shows the pulse inputs.

## Power: read this first

When the Computer is the USB host, it supplies 5 V to the device, with a budget of about **150 mA**. The FM1 has a battery and will probably try to charge from USB, which can draw more than that. Until this has been measured, use one of these setups:

- **a powered USB hub** between the Computer and the FM1 (the host stack supports hubs), or
- an FM1 running on a charged battery, with current measured on its first connection.

The Computer's USB-C port only acts as a host on hardware revision 1.1 and later (`USBPowerState()` reports `DFP`).

## Using the framework in your own card

The framework is two headers in `ComputerCard/`:

- `WorkshopLink.h`: the protocol. It is plain C99, header-only, MIT licensed, and identical to `fm1_open/app/WorkshopLink.h`.
- `WorkshopLinkHost.h`: the Computer side. It is a class that runs the TinyUSB host and the link session on core 1. The calls you make from `ProcessSample()` are lock-free.

```cpp
#include "ComputerCard.h"
#include "WorkshopLinkHost.h"

class MyCard : public ComputerCard {
    WorkshopLinkHost link;
    int level, scope;
public:
    MyCard() {
        link.SetName("MY CARD");            // shown in the FM1 status line
        link.SetTitle("MY CARD");           // page title
        level = link.AddWidget(WL_W_BAR, 8, 40, 224, 24, WL_C_GREEN, "LEVEL", 0, 2047);
        scope = link.AddWidget(WL_W_SCOPE, 8, 80, 224, 80, WL_C_ACCENT, "OUT");
        link.Start();                       // claims core 1
    }
    void ProcessSample() override {
        link.SetValue(level, abs(AudioIn1()));
        if (++n % 64 == 0) link.PushScope(scope, AudioIn1());
        uint8_t m[3];
        while (int len = link.ReadMIDI(m)) { /* notes, CCs, clock from the FM1 */ }
        transpose += link.EncoderDelta(0);  // FM1 encoder 1
    }
    int n = 0, transpose = 0;
};
```

Build: copy `tusb_config.h` from this folder next to your card, and link with `pico_multicore tinyusb_host tinyusb_board`. The framework uses TinyUSB's built-in MIDI host class, so you don't need any third-party driver sources.

### Widgets

Coordinates are absolute screen pixels. On the FM1, the usable area is x 0–239 and y 32–223. The title and status line sit outside that area.

| Type | Shows | Updated by |
|---|---|---|
| `WL_W_LABEL` | its label (large if h ≥ 14) | n/a |
| `WL_W_VALUE` | label + number (large if h ≥ 24) | `SetValue` |
| `WL_W_BAR` | label, number and a bar from min to max | `SetValue` |
| `WL_W_LED` | an indicator and its label | `SetValue` (non-zero = lit) |
| `WL_W_TEXT` | label + a 23-character string | `SetText` |
| `WL_W_SCOPE` | a rolling trace of the last 120 points (two scopes at most) | `PushScope` |

There are up to 16 widgets. The colours are roles (`WL_C_TEXT`, `ACCENT`, `DIM`, `GREEN`, `AMBER`, `RED`, `BLUE`, `VIOLET`), and each device maps them to its own palette.

### Other calls

`NoteOn/NoteOff/ControlChange/Realtime/SendMIDI`, `ReadMIDI`, `EncoderDelta(i)`, `Buttons()`, `PageVisible()`, `GetState()` (`NoDevice`, `Connecting`, `MidiOnly`, `Linked`), `RequestShow(bool)`. For your own messages, `SendUser`/`SetUserHandler` carry card-specific frames (commands `0x20–0x3F` from the host and `0x60–0x7F` from the device) without changing the protocol.

### Threading

- Call layout functions before `Start()`.
- `SetValue` is safe from either core.
- Call each other runtime function from one core only (normally core 0).
- The user handler runs on core 1.

## Protocol summary

The full description is at the top of `WorkshopLink.h`.

```
F0 7D 'W' 'L' <version=1> <command> <sequence> <payload, 8→7 bit packed> <checksum> F7
```

- Frames are at most 119 bytes, which fits the FM1 SDK's 128-byte outgoing limit.
- The host sends `HELLO`. The device replies `HELLO_ACK` with its capabilities. The host then sends `LAYOUT` plus one `WIDGET` per widget, followed by `VALUES` (about 30 Hz, only changed values), `TEXT`, `SCOPE` (about 20 Hz) and `PING` (once a second when idle).
- The device sends `EVENT` (page visible, buttons, encoder deltas). It sends `NEED_LAYOUT` when it gets updates without a session (for example after it restarts), and the host then starts again.
- The device drops the session after 3 s without a frame. Its screen keeps the last values and shows "LINK LOST".
- A device that never answers (stock FM1 firmware) leaves the card in `MidiOnly`, and plain MIDI keeps working.

Other firmwares can implement the device side by porting `fm1_open/app/workshop_link.c` (about 260 lines, no allocation) and drawing the widgets with their own graphics.

## Testing without hardware

`host_test/run.sh /path/to/fm1_open` builds the FM1 web simulator natively: the real FM1 APP code, including the link page. It links that with the real `WorkshopLinkHost.h` (Pico SDK and TinyUSB are replaced by small stubs) and runs a session byte for byte through a simulated USB-MIDI cable. The test covers:

- protocol fuzzing;
- the MIDI parser;
- HELLO and layout;
- screen repaint;
- encoders and buttons;
- HOME and HOME + ARP;
- an FM1 restart;
- a lost link;
- the MIDI-only fallback.

It writes the simulated FM1 screen to `build/link_page.ppm`.

## Before trusting it on hardware

These are the things simulation can't check. Do them in roughly this order:

1. **Enumeration.** Plug the FM1 into a computer and save `lsusb -v` (or the macOS/Windows equivalent). Confirm that the MIDI streaming interface has one bulk IN and one bulk OUT endpoint (TinyUSB's MIDI host requirement), and that the device works at full speed (the RP2040 host is full-speed only).
2. **Power.** Measure the FM1's current from the Computer's port with and without a charged battery. If it goes over budget, use a powered hub.
3. **Stock-firmware MIDI.** Run this card with the stock FM1 firmware. Notes, clock and CV/gate should all work, and the card should settle in `MidiOnly` (the top-right LED stays off).
4. **The link.** Flash fm1_open with the Workshop Link page, then check the page, the update rate, and that the FM1's audio doesn't glitch while the screen updates. Link work runs on the FM1's control task, not the audio path.

## Audio over USB: feasibility

Both ends have USB audio, but nothing connects them yet:

- **FM1:** it already offers USB audio in both directions, 44.1 kHz stereo 24-bit (`fm1_usb_audio_read/write`). The open APP does the mixing, so firmware changes are possible.
- **Computer:** it would have to be a USB *audio host*. TinyUSB has no audio host class. Its RP2040 host driver doesn't yet support the isochronous transfers that audio needs (it is a TODO in `hcd_rp2040.c`). On top of that, the Computer runs at 48 kHz against the FM1's 44.1 kHz, so the card would need sample-rate conversion and drift correction.

That makes USB audio an R&D project rather than a framework feature. The options are:

| Option | Quality | Effort | Notes |
|---|---|---|---|
| Patch cables (FM1 out → Computer audio in) | full | none | Recommended for now. Covers FM1 → Computer. |
| UAC host on the RP2040 | full | high | Isochronous IN/OUT on the RP2040 host controller plus a minimal UAC1/2 host. Both are new code, and the result would also be reusable for other USB audio devices. |
| Audio-in-SysEx over the existing link | lo-fi (roughly 8-bit, ≤ 8–16 kHz mono, to be measured) | medium | Needs no isochronous support. Uses `WL_*_USER` commands and an FM1 firmware mix-in. Throughput across the RP2040 host and FM1 SysEx path is not yet measured. |
