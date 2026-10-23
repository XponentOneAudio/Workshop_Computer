// SysEx protocol between the Epicycles card and its web app (web/index.html),
// used when a computer is plugged into the Computer's USB socket and the
// card is acting as a USB MIDI device.
//
// Every message is  F0 7D 45 <cmd> <payload...> F7
// (7D is the MIDI 'non-commercial' manufacturer ID, 45 is 'E').
// All values are 7-bit; wider values are sent as two bytes, high 7 bits first.
//
// Fader values are in 8mu fader units, 0-127, sent page by page (4 pages
// of 8): the partials pages LEVEL, PHASE, FREQUENCY, then the drawing's
// eight BANDS.
//
// A circle of a drawing is six bytes: k + 128 (2), amp (2), phase (2)
//   k      turns per turn of the drawing, -127 to 127; below 0 it turns
//          clockwise
//   amp    radius, 0-16383 for 0 to 1 (full scale)
//   phase  where it starts, 0-16383 for one turn
// A drawing has up to 64 circles, biggest first.
//
// Web -> card
//   HELLO    01                       card replies with STATE, then, if its
//                                     drawing is one sent to it, the drawing
//                                     (CIRCLES and COMMIT)
//   SET      03 page fader value      one fader value (page 0-3)
//   SET_ALL  04 version values[32]    every fader value
//   PAGE     05 page                  partials page (0-2) shown on the LEDs
//   SHAPE    06 shape                 load a partials shape (0-5, as kShapes
//                                     in main.cpp); card replies with STATE
//   PING     09                       sent every second; STATUS flows while
//                                     pings keep arriving
//   SYNC     0A                       every circle back to its start
//   MODE     0B mode                  0 partials, 1 drawing; card replies
//                                     with STATE
//   DRAWING  0C drawing               load a built-in drawing (0-7, as
//                                     drawings.h); card replies with STATE
//   CIRCLES  0D first count circles[count]
//                                     up to 8 circles of a new drawing,
//                                     from circle 'first'
//   COMMIT   0E count                 the new drawing is complete, with
//                                     'count' circles: the card starts
//                                     drawing it and replies with STATE
//
// Card -> web
//   STATE    02 version mode page values[32] shape drawing
//            sent in reply, and whenever the card's shape, drawing or mode
//            changes from its own panel or 8mu.  drawing is 0-7, or 127
//            for one sent to the card
//   STATUS   07 note(2) partials stretch flags mode circles(2) rotate(2)
//            note        base pitch in 1/8 semitones (MIDI note * 8)
//            partials    partials mode: how many sound, 0-127 for 1 to 8
//            stretch     partials mode: stretch, 0-127 for 0 to 0.5
//            flags       bit 0 switch up (harmonic lock / slow),
//                        bit 1 8mu on card
//            mode        as MODE
//            circles     drawing mode: X + CV In 2, 0-4095
//            rotate      drawing mode: Y, 0-4095 for a full turn
//   CIRCLES, COMMIT      as above, after STATE in reply to HELLO

#ifndef EPICYCLES_SYSEX_H
#define EPICYCLES_SYSEX_H

#include <stdint.h>

namespace sysex
{

static constexpr uint8_t kMfr = 0x7D;
static constexpr uint8_t kProduct = 0x45;
static constexpr uint8_t kVersion = 1;
static constexpr int kNumValues = 32;
static constexpr int kCirclesPerMessage = 8;
static constexpr uint8_t kCustomDrawing = 127;

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
	Mode = 0x0B,
	Drawing = 0x0C,
	Circles = 0x0D,
	Commit = 0x0E,
};

// Collects one SysEx message from a byte stream.  Feed() returns true when
// a complete message for this card has arrived; its command and payload are
// then in cmd, payload and length.
struct Parser
{
	static constexpr int kMax = 64;
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

inline int32_t Get14(const uint8_t *p)
{
	return (int32_t(p[0] & 0x7F) << 7) | (p[1] & 0x7F);
}

static constexpr int kStateLen = 4 + 3 + kNumValues + 2 + 1;
static constexpr int kStatusLen = 4 + 2 + 4 + 2 + 2 + 1;
static constexpr int kCirclesLen = 4 + 2 + 6 * kCirclesPerMessage + 1;

} // namespace sysex

#endif
