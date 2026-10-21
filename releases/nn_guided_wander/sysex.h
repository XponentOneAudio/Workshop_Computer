// SysEx protocol between the Guided Wander card and its web editor
// (web/index.html), used when a computer is plugged into the Computer's USB
// socket and the card is acting as a USB MIDI device.
//
// Every message is  F0 7D 47 <cmd> <payload...> F7
// (7D is the MIDI 'non-commercial' manufacturer ID, 47 is 'G').
// All values are 7-bit; wider values are sent as several bytes, high bits first.
//
// Slider values are in 8mu fader units, 0-127: relative probabilities.  There
// are eight pages of eight sliders, numbered
//   0 NOTES 1     1 OCTAVES 1     2 GATES 1     3 RATCHETS 1    (voice 1)
//   4 NOTES 2     5 OCTAVES 2     6 GATES 2     7 RATCHETS 2    (voice 2)
// On the 8mu, button A is pages 0 and 4, B 1 and 5, C 2 and 6, D 3 and 7.
// A second page whose sliders are all zero is not used: voice 2 then draws
// from the first page of the pair, as voice 1 does.
//
// Web -> card
//   HELLO   01                    card replies with STATE
//   SET     03 page slider value  one slider
//   SET_ALL 04 version values[64] every slider
//   PAGE    05 page               page shown on the Computer's LEDs
//   RESET   06                    default probabilities; card replies with STATE
//   PING    09                    sent every second; STATUS flows while pings
//                                 keep arriving
//
// Card -> web
//   STATE   02 version page values[64]
//   STATUS  07 flags flags2 page scale clock slew attack decay pluck
//              progress stepMs(3) voice1(7) voice2(7)
//     flags     bit 0 clocked, bit 1 8mu on card, bit 2 switch up (knobs are
//               scale, clock and slew, not decay, attack and pluck), bit 3 a
//               knob is waiting to pick up its setting
//     flags2    bits 0-2: the X, Y and Main knob is waiting
//     page      0-7, the page the faders edit
//     scale     0-31, see kScaleNames in wander.h and the web editor
//     clock     0-10: /8 /6 /4 /3 /2 x1 x2 x3 x4 x6 x8
//     slew, attack, decay, pluck   the knob settings, 0-127
//     progress  position through the current step, 0-127
//     stepMs    length of a step in milliseconds, 21 bits
//     voice     note | octave << 3     (each 0-7: the slider chosen)
//               gate | ratchet << 3    (gate 0-7: 0% to tied; ratchet 0-7)
//               hits                   counts up (mod 128) at every hit
//               bit 0 gate high, bit 1 tied into the next hit
//               envelope               0-127
//               pitch(2)               millivolts + 8192, 14 bits

#ifndef WANDER_SYSEX_H
#define WANDER_SYSEX_H

#include <stdint.h>

namespace sysex
{

static constexpr uint8_t kMfr = 0x7D;
static constexpr uint8_t kProduct = 0x47;
static constexpr uint8_t kVersion = 1;
static constexpr int kPages = 8;
static constexpr int kSliders = 8;
static constexpr int kNumValues = kPages * kSliders;

enum Cmd : uint8_t
{
	Hello = 0x01,
	State = 0x02,
	Set = 0x03,
	SetAll = 0x04,
	Page = 0x05,
	Reset = 0x06,
	Status = 0x07,
	Ping = 0x09,
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

inline int Put21(uint8_t *out, int32_t v)
{
	if (v < 0) v = 0;
	if (v > 2097151) v = 2097151;
	out[0] = uint8_t(v >> 14);
	out[1] = uint8_t((v >> 7) & 0x7F);
	out[2] = uint8_t(v & 0x7F);
	return 3;
}

static constexpr int kStateLen = 4 + 2 + kNumValues + 1;
static constexpr int kVoiceLen = 7;
static constexpr int kStatusLen = 4 + 13 + 2 * kVoiceLen + 1;

} // namespace sysex

#endif
