// SysEx protocol between the Nested Circles card and its web app
// (web/index.html), used when a computer is plugged into the Computer's USB
// socket and the card is acting as a USB MIDI device.
//
// Every message is  F0 7D 4E <cmd> <payload...> F7
// (7D is the MIDI 'non-commercial' manufacturer ID, 4E is 'N').
// All values are 7-bit; wider values are sent as two bytes, high 7 bits first.
//
// Partial values are in 8mu fader units, 0-127, sent page by page (3 pages
// of 8 partials): LEVEL, PHASE, FREQUENCY.
//
// Web -> card
//   HELLO    01                       card replies with STATE
//   SET      03 page partial value    one partial value
//   SET_ALL  04 version values[24]    every partial value
//   PAGE     05 page                  page shown on the Computer's LEDs
//   SHAPE    06 shape                 load a built-in shape (0-5, as kShapes
//                                     in main.cpp); card replies with STATE
//   PING     09                       sent every second; STATUS flows while
//                                     pings keep arriving
//   SYNC     0A                       reset every circle to its start phase
//
// Card -> web
//   STATE    02 version page values[24]
//   STATUS   07 note(2) partials stretch flags
//            note        base pitch in 1/8 semitones (MIDI note * 8)
//            partials    how many partials sound, 0-127 for 1 to 8
//            stretch     stretch setting, 0-127 for 0 to 0.5
//            flags       bit 0 switch up (harmonic lock), bit 1 8mu on card

#ifndef NESTED_SYSEX_H
#define NESTED_SYSEX_H

#include <stdint.h>

namespace sysex
{

static constexpr uint8_t kMfr = 0x7D;
static constexpr uint8_t kProduct = 0x4E;
static constexpr uint8_t kVersion = 1;
static constexpr int kNumValues = 24;

enum Cmd : uint8_t
{
	Hello = 0x01,
	State = 0x02,
	Set = 0x03,
	SetAll = 0x04,
	Page = 0x05,
	Shape = 0x06,
	Status = 0x07,
	Ping = 0x09,
	Sync = 0x0A,
};

// Collects one SysEx message from a byte stream.  Feed() returns true when
// a complete message for this card has arrived; its command and payload are
// then in cmd, payload and length.
struct Parser
{
	static constexpr int kMax = 40;
	uint8_t buf[kMax];
	int n = 0;
	bool in = false;

	uint8_t cmd = 0;
	const uint8_t *payload = nullptr;
	int length = 0;

	bool Feed(uint8_t b)
	{
		if (b == 0xF0)
		{
			in = true;
			n = 0;
			return false;
		}
		if (!in) return false;
		if (b == 0xF7)
		{
			in = false;
			if (n < 3 || buf[0] != kMfr || buf[1] != kProduct) return false;
			cmd = buf[2];
			payload = buf + 3;
			length = n - 3;
			return true;
		}
		if (b & 0x80)
		{
			// Any other status byte aborts the message, except real-time
			// bytes, which may legally appear inside SysEx
			if (b < 0xF8) in = false;
			return false;
		}
		if (n < kMax) buf[n++] = b;
		else in = false;
		return false;
	}
};

inline int Header(uint8_t *out, uint8_t cmd)
{
	out[0] = 0xF0;
	out[1] = kMfr;
	out[2] = kProduct;
	out[3] = cmd;
	return 4;
}

inline int Put14(uint8_t *out, int32_t v)
{
	if (v < 0) v = 0;
	if (v > 16383) v = 16383;
	out[0] = uint8_t(v >> 7);
	out[1] = uint8_t(v & 0x7F);
	return 2;
}

static constexpr int kStateLen = 4 + 2 + kNumValues + 1;
static constexpr int kStatusLen = 4 + 2 + 3 + 1;

} // namespace sysex

#endif
