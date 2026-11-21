// SysEx protocol between the Mutagen card and its web editor
// (web/index.html), used when a computer is plugged into the Computer's USB
// socket and the card is acting as a USB MIDI device.
//
// Every message is  F0 7D 4D <cmd> <payload...> F7
// (7D is the MIDI 'non-commercial' manufacturer ID, 4D is 'M').
// All values are 7-bit; wider values are sent as two bytes, high 7 bits first.
//
// Fader values are 0-4095, sent as two bytes each, page by page (8 pages of
// 8 faders), the pages numbered
//   0 MUTANT 1   1 MUTANT 2   2 OSC   3 LFO 1      (buttons A-D)
//   4 WARP 1     5 WARP 2     6 MOD   7 LFO 2      (A-D pressed again)
//
// Web -> card
//   HELLO     01                        card replies with STATE
//   SET       03 page fader value(2)    one fader value
//   SET_ALL   04 version values(2)[64]
//   PAGE      05 page                   page shown on the Computer's LEDs
//   RESET     06                        default values; card replies with STATE
//   PING      09                        sent every second; STATUS flows while
//                                       pings keep arriving
//   MOTION    0B pitch(2) roll(2)       an 8mu's tilt on the computer, each
//                                       offset by 2048 (2048 is level)
//
// Card -> web
//   STATE     02 version page values(2)[64]
//   STATUS    07 flags flags2 pitch(2) wave(2) mutant1[6] mutant2[6] lfo1 lfo2
//                knobs(2)[7] tilt1 tilt2 busy parts[4]
//             flags      bit 0 8mu on the card, bit 1 switch up (X and Y are
//                        the depths, not the ratios), bit 2 switch held down
//                        (Main the wave, X and Y the modes), bit 3 a knob is
//                        waiting to pick up its setting, bits 4 and 5 a
//                        jack in Audio In 1 (sync) and 2, bit 6 lean: the
//                        patch is too much for the processor to make
//                        twice a sample, so Out 1 is made once
//             flags2     bits 0-2 the X, Y and Main knob is waiting
//             pitch      as playing, 1/1024 octaves above C0 (16.35Hz)
//             wave       0-4095 across sine, triangle, saw, square, after
//                        modulation
//             mutant     mode (0-5), depth(2) 0-8191, ratio(2) as 1/2048
//                        octaves above 1/4, mix 0-127; all after modulation
//             lfo        -1 ... +1 as 0-127
//             knobs      the values the knobs set (they may have changed
//                        them): MUTANT 1 and 2 depth, ratio and mode, and
//                        the wave
//             tilt       the tilt after its dead zone, -1 ... +1 as 0-127
//             busy       the longest the card's audio interrupt took over the
//                        last second, in microseconds (it has about 20)
//             parts      the same for each part of the work: the sound
//                        alone, and with the knobs, the 8mu, and the LEDs
//                        and this status

#ifndef MUTAGEN_SYSEX_H
#define MUTAGEN_SYSEX_H

#include <stdint.h>

namespace sysex
{

static constexpr uint8_t kMfr = 0x7D;
static constexpr uint8_t kProduct = 0x4D;
static constexpr uint8_t kVersion = 1;
static constexpr int kNumValues = 8 * 8;

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
	Motion = 0x0B,
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

static constexpr int kStatePayload = 2 + 2 * kNumValues;
static constexpr int kStateLen = 4 + kStatePayload + 1;
static constexpr int kStatusPayload = 2 + 2 + 2 + 6 + 6 + 2 + 14 + 2 + 1 + 4;
static constexpr int kStatusLen = 4 + kStatusPayload + 1;

} // namespace sysex

#endif
