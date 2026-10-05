// SysEx protocol between the Sample Lab card and its web app (web/index.html),
// used when a computer is plugged into the Computer's USB socket and the card
// is acting as a USB MIDI device.
//
// Every message is  F0 7D 50 <cmd> <payload...> F7
// (7D is the MIDI 'non-commercial' manufacturer ID, 50 is 'P', for samPle).
// All values are 7-bit; wider values are sent as several bytes, high 7 bits
// first: two bytes for 14 bits, three for 21.
//
// Fader values are in 8mu fader units, 0-127: position, overlap / density,
// spray, window, search, pitch spray, bits, rate.
//
// Samples are sent as 14 bits each, the sample's top 14 bits offset by 8192
// (so 8192 is silence), in chunks of up to kChunk samples.
//
// Web -> card
//   HELLO        01                    card replies with STATE
//   SET          03 fader value        one fader value
//   SET_ALL      04 version values[8] engine
//   ENGINE       05 engine             0 varispeed, 1 overlap-add, 2 WSOLA,
//                                      3 cloud
//   PING         09                    sent every second; STATUS flows while
//                                      pings keep arriving
//   RESTART      0A                    playhead back to the start
//   SAMPLE_BEGIN 0B length(3)          a new sample follows; output mutes
//   SAMPLE_DATA  0C offset(3) samples(2 each)
//   SAMPLE_END   0D                    the new sample is complete; card
//                                      replies with STATE
//   GET_SAMPLE   0E                    card sends its sample as SAMPLE_DATA
//                                      chunks, then SAMPLE_END
//   RECORD       0F on                 1 start recording Audio In 1, 0 stop
//   DEMO         10                    put the built-in demo loop back
//
// Card -> web
//   STATE        02 version engine values[8] length(3) capacity(3)
//                sent in reply, and whenever the sample or engine changes on
//                the card itself (a recording, a tap on the switch, the 8mu)
//   STATUS       07 speed(2) pitch(2) size(2) head(3) flags
//                speed       playback speed, thousandths, offset by 4000
//                            (so 4000 is stopped, 5000 is normal speed)
//                pitch       pitch shift, hundredths of a semitone, offset
//                            by 4800
//                size        grain size, tenths of a millisecond
//                head        playhead, in samples
//                flags       bit 0 recording, bit 1 8mu on card
//   SAMPLE_DATA  0C offset(3) samples(2 each)
//   SAMPLE_END   0D

#ifndef SAMPLELAB_SYSEX_H
#define SAMPLELAB_SYSEX_H

#include <stdint.h>

namespace sysex
{

static constexpr uint8_t kMfr = 0x7D;
static constexpr uint8_t kProduct = 0x50;
static constexpr uint8_t kVersion = 1;
static constexpr int kNumValues = 8;
static constexpr int kChunk = 48;

enum Cmd : uint8_t
{
	Hello = 0x01,
	State = 0x02,
	Set = 0x03,
	SetAll = 0x04,
	Engine = 0x05,
	Status = 0x07,
	Ping = 0x09,
	Restart = 0x0A,
	SampleBegin = 0x0B,
	SampleData = 0x0C,
	SampleEnd = 0x0D,
	GetSample = 0x0E,
	Record = 0x0F,
	Demo = 0x10,
};

// Collects one SysEx message from a byte stream.  Feed() returns true when
// a complete message for this card has arrived; its command and payload are
// then in cmd, payload and length.
struct Parser
{
	static constexpr int kMax = 3 + 3 + 2 * kChunk + 8;
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

inline int Put21(uint8_t *out, int32_t v)
{
	if (v < 0) v = 0;
	if (v > 0x1FFFFF) v = 0x1FFFFF;
	out[0] = uint8_t(v >> 14);
	out[1] = uint8_t((v >> 7) & 0x7F);
	out[2] = uint8_t(v & 0x7F);
	return 3;
}

inline int32_t Get14(const uint8_t *in)
{
	return (int32_t(in[0] & 0x7F) << 7) | (in[1] & 0x7F);
}

inline int32_t Get21(const uint8_t *in)
{
	return (int32_t(in[0] & 0x7F) << 14) | (int32_t(in[1] & 0x7F) << 7) | (in[2] & 0x7F);
}

static constexpr int kStateLen = 4 + 2 + kNumValues + 6 + 1;
static constexpr int kStatusLen = 4 + 2 + 2 + 2 + 3 + 1 + 1;
static constexpr int kDataLen = 4 + 3 + 2 * kChunk + 1;

} // namespace sysex

#endif
