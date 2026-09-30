// what.three.wiggles: SysEx messages between the webapp and the card.
//
// Every message is F0 7D <card id> <type> <payload> F7. 7D is the MIDI
// manufacturer ID for private use. SysEx bytes are 7-bit, so the card sends
// seeds, and both sides send UTF-8 text, as 4-bit nibbles, high nibble first.
// Functions here see the bytes between 7D and F7.
//
//   HELLO      webapp -> card   33 01 <interface version: 3 bytes>
//   SET_WORDS  webapp -> card   33 03 <length: 2 nibbles> <text: 2 nibbles per byte>
//   STATE      card -> webapp   33 02 <firmware version: 3 bytes>
//                                     <3 seeds: 8 nibbles each>
//                                     <length: 2 nibbles> <text: 2 nibbles per byte>
//   ERROR      card -> webapp   33 04 <code>
//   GET_STATS  webapp -> card   33 06
//   STATS      card -> webapp   33 05 <peak cycles: 6 nibbles> <budget cycles: 6 nibbles>
//
// STATS reports the longest the audio callback has taken since the last
// GET_STATS, against the CPU cycles available per sample.
//
// The card replies to HELLO and SET_WORDS with STATE. It hashes the words
// itself, so if the seeds in STATE match the webapp's, both agree.
//
// Plain C++ with no Pico SDK dependencies, so it also compiles on a desktop.

#ifndef W3W_PROTOCOL_H
#define W3W_PROTOCOL_H

#include <cstdint>
#include <cstddef>
#include <initializer_list>
#include "seed.h"

namespace w3w
{

constexpr uint8_t kCardId = 0x33;
constexpr uint8_t kMsgHello = 0x01;
constexpr uint8_t kMsgState = 0x02;
constexpr uint8_t kMsgSetWords = 0x03;
constexpr uint8_t kMsgError = 0x04;
constexpr uint8_t kMsgStats = 0x05;
constexpr uint8_t kMsgGetStats = 0x06;
constexpr uint8_t kErrBadMessage = 1;
constexpr uint8_t kErrBadWords = 2;

constexpr uint32_t kMaxWordsLen = 95;
constexpr uint32_t kMaxStateLen = 2 + 3 + 24 + 2 + 2 * kMaxWordsLen;

// Decode the text of a SET_WORDS message into `text` (kMaxWordsLen bytes).
// Returns false, leaving text and len alone, if the message is malformed.
// Doesn't check that the text is three words: use SeedsFromWords for that.
inline bool DecodeSetWords(const uint8_t *data, uint32_t size, uint8_t *text, uint32_t &len)
{
	if (size < 4 || data[0] != kCardId || data[1] != kMsgSetWords) return false;
	uint32_t n = (uint32_t(data[2] & 0x0F) << 4) | (data[3] & 0x0F);
	if (n > kMaxWordsLen || size != 4 + 2 * n) return false;
	for (uint32_t i = 0; i < n; i++)
	{
		text[i] = uint8_t(((data[4 + 2 * i] & 0x0F) << 4) | (data[5 + 2 * i] & 0x0F));
	}
	len = n;
	return true;
}

// Build a STATE message for these words. `out` needs kMaxStateLen bytes.
// Returns the message length.
inline uint32_t EncodeState(const uint8_t *text, uint32_t len, const uint8_t version[3], uint8_t *out)
{
	uint32_t seeds[3] = {0, 0, 0};
	SeedsFromWords(text, len, seeds);
	uint32_t n = 0;
	out[n++] = kCardId;
	out[n++] = kMsgState;
	for (int i = 0; i < 3; i++) out[n++] = version[i];
	for (int w = 0; w < 3; w++)
	{
		for (int shift = 28; shift >= 0; shift -= 4) out[n++] = uint8_t((seeds[w] >> shift) & 0x0F);
	}
	out[n++] = uint8_t(len >> 4);
	out[n++] = uint8_t(len & 0x0F);
	for (uint32_t i = 0; i < len; i++)
	{
		out[n++] = text[i] >> 4;
		out[n++] = text[i] & 0x0F;
	}
	return n;
}

// Build a STATS message. `out` needs 14 bytes. Returns the message length.
inline uint32_t EncodeStats(uint32_t peakCycles, uint32_t budgetCycles, uint8_t *out)
{
	uint32_t n = 0;
	out[n++] = kCardId;
	out[n++] = kMsgStats;
	for (uint32_t v : {peakCycles, budgetCycles})
	{
		for (int shift = 20; shift >= 0; shift -= 4) out[n++] = uint8_t((v >> shift) & 0x0F);
	}
	return n;
}

} // namespace w3w

#endif
