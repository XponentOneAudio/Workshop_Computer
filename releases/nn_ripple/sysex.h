// SysEx protocol between the 8mu Ripple card and its web editor
// (web/index.html), used when a computer is plugged into the Computer's USB
// socket and the card is acting as a USB MIDI device.
//
// Every message is  F0 7D 52 <cmd> <payload...> F7
// (7D is the MIDI 'non-commercial' manufacturer ID, 52 is 'R').
// All values are 7-bit; wider values are sent as two bytes, high 7 bits first.
//
// Fader values are in 8mu fader units, 0-127, sent page by page (7 pages of
// 8 faders), the pages numbered as PageId in main.cpp:
//   0 PLAY   1 LOGIC   2 NOISE   3 FILTER   4 DIVIDE   5 RATIOS   6 MOTION
//
// Web -> card
//   HELLO     01                        card replies with STATE
//   SET       03 page fader value       one fader value
//   SET_ALL   04 version freeze values[56]
//   RESEED    06                        restart the pattern, as Pulse In 2
//   PING      09                        sent every second; STATUS flows while
//                                       pings keep arriving
//   FREEZE    0A on                     loop lock
//   MOTION    0B pitch(2) roll(2) yaw(2)  an 8mu's motion, in EightMU units
//                                       (-2032 to 2032) offset by 8192
//
// Card -> web
//   STATE     02 version freeze values[56]
//   STATUS    07 flags type pitch(2) pitchMin(2) pitchMax(2)
//                cutoff(2) cutoffMin(2) cutoffMax(2) main x y length reg[5]
//                cycles peakL peakR busy parts[5] scopeCount scope[scopeCount]
//                bitCount bits[2 * bitCount]
//             flags      bit 0 loop lock, bit 1 switch middle (filter type
//                        B), bit 2 a jack in Pulse In 1 (external clock),
//                        bit 3 in Audio In 1 (bit source), bit 4 in Audio
//                        In 2 (clock FM)
//             type       the filter type playing, 0-5: LP BP HP notch
//                        ladder comb
//             pitch      clock pitch now, and its lowest and highest since
//                        the last STATUS: (Q12 octaves above 1Hz + 32768) / 8
//             cutoff     the same for the filter cutoff: Q12 octaves above
//                        20Hz, / 4
//             main x y   the knobs, 0-127
//             length     LFSR length in bits; reg its contents, 35 bits
//             cycles     counter cycles since the last STATUS (to 127)
//             peakL/R    output peak levels, 0-127
//             busy       the longest the audio interrupt took over the last
//                        second, in microseconds (it has about 20)
//             parts      the same, for the samples doing each part of the
//                        work: the engine alone, then with the knobs and
//                        pages, the engine's coefficients, the LEDs, and
//                        this status
//             scope      Audio Out 1, every second sample from a rising zero
//                        crossing, 0-127; sent when a new capture is ready
//             bits       the sources sampled once a millisecond since the
//                        last STATUS, oldest first: bit 0 /1, bits 1-5
//                        dividers 1-5, bits 6-9 logic A-D

#ifndef RIPPLE_SYSEX_H
#define RIPPLE_SYSEX_H

#include <stdint.h>

namespace sysex
{

static constexpr uint8_t kMfr = 0x7D;
static constexpr uint8_t kProduct = 0x52;
static constexpr uint8_t kVersion = 2;
static constexpr int kPages = 7;
static constexpr int kNumValues = kPages * 8;
static constexpr int kScopeLen = 120; // fits in one 7-bit count byte
static constexpr int kMaxBits = 63;

enum Cmd : uint8_t
{
	Hello = 0x01,
	State = 0x02,
	Set = 0x03,
	SetAll = 0x04,
	Reseed = 0x06,
	Status = 0x07,
	Ping = 0x09,
	Freeze = 0x0A,
	Motion = 0x0B,
};

// Collects one SysEx message from a byte stream.  Feed() returns true when
// a complete message for this card has arrived; its command and payload are
// then in cmd, payload and length.
struct Parser
{
	static constexpr int kMax = 80;
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

inline int32_t Get14(const uint8_t *in)
{
	return (int32_t(in[0] & 0x7F) << 7) | (in[1] & 0x7F);
}

// Fader units (0-127) to and from the card's parameter units (0-4095)
inline int32_t FromFader(int32_t v) {return (v * 4095 + 63) / 127;}
inline int32_t ToFader(int32_t v) {return (v * 127 + 2047) / 4095;}

static constexpr int kStateLen = 4 + 2 + kNumValues + 1;
static constexpr int kStatusFixed = 32;
static constexpr int kStatusMaxPayload = kStatusFixed + 1 + kScopeLen + 1 + 2 * kMaxBits;
static constexpr int kStatusMaxLen = 4 + kStatusMaxPayload + 1;

} // namespace sysex

#endif
