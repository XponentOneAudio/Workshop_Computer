// SysEx protocol between the Nested Circles card and its web app
// (web/index.html), used when a computer is plugged into the Computer's USB
// socket and the card is acting as a USB MIDI device.
//
// Every message is  F0 7D 4E <cmd> <payload...> F7
// (7D is the MIDI 'non-commercial' manufacturer ID, 4E is 'N').
// All values are 7-bit; wider values are sent as two bytes, high 7 bits first.
//
// Fader values are in 8mu fader units, 0-127, sent page by page (4 pages
// of 8): the additive pages LEVEL, PHASE, FREQUENCY, then the FM faders
// (carrier ratio, modulator ratio, modulator fine, depth, feedback, decay,
// env > depth, env > level).
//
// Web -> card
//   HELLO    01                       card replies with STATE
//   SET      03 page fader value      one fader value (page 0-3)
//   SET_ALL  04 version values[32] type   every fader value, and the FM type
//   PAGE     05 page                  additive page (0-2) shown on the LEDs
//   SHAPE    06 shape                 load an additive shape (0-5, as kShapes
//                                     in main.cpp); card replies with STATE
//   PING     09                       sent every second; STATUS flows while
//                                     pings keep arriving
//   SYNC     0A                       additive: every circle to its start;
//                                     FM: trigger the envelope
//   MODE     0B mode                  0 additive, 1 FM; card replies with STATE
//   FM_TYPE  0C type                  0 linear, 1 exponential, 2 through-zero,
//                                     3 phase modulation
//   EXAMPLE  0D example               load an FM example (0-5, as kExamples
//                                     in main.cpp); card replies with STATE
//
// Card -> web
//   STATE    02 version mode page values[32] type
//            sent in reply, and whenever the card's shape, example, FM type
//            or mode changes from its own panel or 8mu
//   STATUS   07 note(2) partials stretch flags mode type depth(2) steps env
//            note        base pitch in 1/8 semitones (MIDI note * 8)
//            partials    additive: how many partials sound, 0-127 for 1 to 8
//            stretch     additive: stretch, 0-127 for 0 to 0.5
//            flags       bit 0 switch up (harmonic lock / drone),
//                        bit 1 8mu on card
//            mode, type  as MODE and FM_TYPE
//            depth       FM: depth from X and CV In 2, in thousandths
//            steps       FM: modulator ratio steps added by Y, 0-12
//            env         FM: envelope, 0-127

#ifndef NESTED_SYSEX_H
#define NESTED_SYSEX_H

#include <stdint.h>

namespace sysex
{

static constexpr uint8_t kMfr = 0x7D;
static constexpr uint8_t kProduct = 0x4E;
static constexpr uint8_t kVersion = 2;
static constexpr int kNumValues = 32;

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
	FMType = 0x0C,
	Example = 0x0D,
};

// Collects one SysEx message from a byte stream.  Feed() returns true when
// a complete message for this card has arrived; its command and payload are
// then in cmd, payload and length.
struct Parser
{
	static constexpr int kMax = 48;
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

static constexpr int kStateLen = 4 + 3 + kNumValues + 1 + 1;
static constexpr int kStatusLen = 4 + 2 + 5 + 2 + 2 + 1;

} // namespace sysex

#endif
