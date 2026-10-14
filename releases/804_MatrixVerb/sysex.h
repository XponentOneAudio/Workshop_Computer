// SysEx protocol between the MatrixVerb card and its web editor
// (web/index.html), used when a computer is plugged into the Computer's USB
// socket and the card is acting as a USB MIDI device.
//
// Every message is  F0 7D 4D <cmd> <payload...> F7
// (7D is the MIDI 'non-commercial' manufacturer ID, 4D is 'M').
// All values are 7-bit; wider values are sent as two bytes, high 7 bits first.
//
// Settings are in 8mu fader units, 0-127, sent page by page (14 pages of 8,
// one value per fader), numbered
//   0-7  MATRIX row 1-8: value c of row r is how much line r takes from
//        line c, two-sided (64 is 0, 0 is -1, 127 is +1)
//   8    LENGTH     each line's delay, before the size knob and the
//                   coprime snap
//   9    REVERB     0 predelay  1 decay  2 damping  3 low cut  4 tone
//                   5 mod depth  6 width  7 mix
//   10   LAB        0 normalise  1 blend  2 drive  3 diffuse  4 mod rate
//                   5 input level  6 output level  7 ceiling
//   11   INPUT      each line's share of the input, two-sided
//   12   OUTPUT     each line's share of the output, two-sided
//   13   PAN        each line's place, two-sided (64 the centre)
// On the 8mu, A is pages 0-7, B 8, C 9 then 10, and D 11, 12 then 13.
// A preset is the 112 values, page 0 first.  design.h has what they mean.
//
// Web -> card
//   HELLO      01                         card replies with STATE
//   SET        03 page index value        one value (index 0-7)
//   SET_ALL    04 version values[112]     every value
//   PAGE       05 page                    page shown on the Computer's LEDs
//   RESET      06                         default settings; card replies STATE
//   MOTION     08 pitch(2) roll(2)        8mu tilt, each 0-4095, 2048 level
//   PING       09                         sent every second; STATUS flows
//                                         while pings keep arriving
//   FREEZE     0A on                      0 or 1, ORed with the switch
//   CLEAR      0B                         empty the network
//   IMPULSE    0C line                    a click into line 0-7, or through
//                                         the input vector (8)
//   SAVE       0D slot                    store the settings in slot 0-7 of
//                                         the card's flash
//   LOAD       0E slot                    load slot 0-7; card replies STATE
//   GET_SLOTS  0F                         card replies SLOTS
//   STORE      11 slot values[112]        write a preset into slot 0-7
//                                         without loading it
//   ERASE      12 slot                    empty slot 0-7
//   TEMPLATE   13 n                       matrix template 0-7 (design.h);
//                                         card replies STATE
//
// Card -> web
//   STATE      02 version page slot values[112]
//              slot   last slot saved or loaded, 0-7, or 127 for none
//   STATUS     07 flags page slot load lengths[8](2 each) decay(2) sigma(2)
//                 scale(2) decayOct(2) sizeOct(2) damp guard limit
//                 in out wet tank
//              flags    bit 0 frozen, bit 1 clearing, bit 2 the guard is
//                       holding the feedback down, bit 3 the limiter is
//                       acting, bit 4 8mu on the card, bit 5 switch down
//              load     the longest the audio took over a sample lately, as
//                       a percentage of the time a sample has (over 100
//                       means samples are being missed)
//              lengths  each line's delay right now, in samples at 24kHz
//              decay    RT60 in hundredths of a second, 16383 for infinite
//              sigma    the matrix's spectral norm (after blend, before
//                       normalising), x1000
//              scale    what normalising multiplied the matrix by, x1000
//              decayOct Main + CV In 1 + tilt: decay multiplier, 1/256
//                       octave offset by 8192
//              sizeOct  X + CV In 2: size multiplier, the same
//              damp     Y + tilt: added to damping, 0-127 (64 none)
//              guard    the guard's feedback gain, 0-127 (127 = untouched)
//              limit    the limiter's least gain lately, 0-127 (127 none)
//              in...    peak levels, 0-127
//   SLOTS      10 valid[8] values[8][112]

#ifndef MATRIXVERB_SYSEX_H
#define MATRIXVERB_SYSEX_H

#include <stdint.h>

namespace sysex
{

static constexpr uint8_t kMfr = 0x7D;
static constexpr uint8_t kProduct = 0x4D;
static constexpr uint8_t kVersion = 1;
static constexpr int kPages = 14;
static constexpr int kNumValues = kPages * 8;
static constexpr int kSlots = 8;
static constexpr uint8_t kNoSlot = 127;

enum Cmd : uint8_t
{
	Hello = 0x01,
	State = 0x02,
	Set = 0x03,
	SetAll = 0x04,
	Page = 0x05,
	Reset = 0x06,
	Status = 0x07,
	Motion = 0x08,
	Ping = 0x09,
	Freeze = 0x0A,
	Clear = 0x0B,
	Impulse = 0x0C,
	Save = 0x0D,
	Load = 0x0E,
	GetSlots = 0x0F,
	Slots = 0x10,
	Store = 0x11,
	Erase = 0x12,
	Template = 0x13,
};

// Collects one SysEx message from a byte stream.  Feed() returns true when
// a complete message for this card has arrived; its command and payload are
// then in cmd, payload and length.
struct Parser
{
	static constexpr int kMax = 160;
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

static constexpr int kStateLen = 4 + 3 + kNumValues + 1;
static constexpr int kStatusLen = 4 + 4 + 16 + 10 + 3 + 4 + 1;
static constexpr int kSlotsLen = 4 + kSlots + kSlots * kNumValues + 1;

} // namespace sysex

#endif
