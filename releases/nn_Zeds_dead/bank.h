// ============================================================================
//  bank.h — factory presets, and turning slots into bytes (for USB and flash)
// ============================================================================
//
//  Like zplane.h this has no hardware code, so tools/host_test.cpp can check
//  it on a PC. The web editor (web/index.html) has a JavaScript mirror of the
//  presets and the byte encoding; the host test and tools/check_web.js make
//  sure the two sides agree byte for byte.
// ============================================================================

#ifndef ZEDS_DEAD_BANK_H
#define ZEDS_DEAD_BANK_H

#include "zplane.h"
#include <cstring>
#include <cstddef>   // offsetof

namespace zp
{

// ----------------------------------------------------------------------------
//  Building sections from musician-friendly numbers
// ----------------------------------------------------------------------------
//
//  The presets are written as "a resonance at ratio × cutoff with quality Q",
//  using the analogue pole positions
//
//      s = −ω/(2Q) ± j·ω·√(1 − 1/(4Q²))
//
//  mapped to the z-plane with z = e^{sT}. That gives a pole ANGLE of
//  ratio·θc·k and RADIUS exp(−ratio·θc/(2Q)), where k = √(1 − 1/(4Q²)).
//  In our stored form (angle = poleRatio·θc, radius = exp(−angle·poleDamp)):
//
//      poleRatio = ratio · k          poleDamp = 1 / (2Q·k)
//
//  (k shrinks the angle a little for low Q. That is real analogue
//  behaviour: a heavily damped resonance rings slightly below its nominal
//  frequency. Q must be above 0.5 for this to make sense.)

inline SectionData poleQ(float ratio, float q, float resWeight)
{
	const float k = sqrtf(1.0f - 1.0f / (4.0f * q * q));
	SectionData s;
	s.poleRatio = ratio * k;
	s.poleDamp  = 1.0f / (2.0f * q * k);
	s.resWeight = resWeight;
	s.zeroMode  = kZeroReal;   // "no zeros" until a helper below says otherwise
	s.zeroA = 0.0f;
	s.zeroB = 0.0f;
	return s;
}

// Zero helpers: return the section with its zeros set.
inline SectionData lowZeros(SectionData s)  { s.zeroMode = kZeroReal; s.zeroA = -1.0f; s.zeroB = -1.0f; return s; }
inline SectionData highZeros(SectionData s) { s.zeroMode = kZeroReal; s.zeroA =  1.0f; s.zeroB =  1.0f; return s; }
inline SectionData bandZeros(SectionData s) { s.zeroMode = kZeroReal; s.zeroA =  1.0f; s.zeroB = -1.0f; return s; }
inline SectionData notchAt(SectionData s, float ratio)
{
	s.zeroMode = kZeroPair; s.zeroA = ratio; s.zeroB = 0.0f;   // on the unit circle
	return s;
}

// ----------------------------------------------------------------------------
//  Factory banks
// ----------------------------------------------------------------------------
//
//  Bank A ("Classic") — the order matters because we morph between
//  neighbours, and each step is a "natural" movement:
//
//    LP → BP : the real zeros at −1,−1 slide to +1,−1, one passing through
//              the middle of the circle: lowpass → "less low" → bandpass.
//    BP → HP : the other zero slides from −1 to +1: bandpass → highpass.
//    HP → Triple notch : real zeros at DC become pairs ON the circle at the
//              notch frequencies (numerator interpolation sweeps them).
//    Triple notch → Twin peaks : side notches dissolve while two resonances
//              grow either side of the centre notch.
//    Twin peaks → Quad notch : a fourth section appears out of nothing (its
//              pole grows from the centre of the circle) — a phaser-like comb.
//
//  For the 6-pole low/high-pass we use the Q values of a 6th-order
//  Butterworth filter (0.518, 0.707, 1.932) — the "maximally flat" textbook
//  choice — and let only the last (sharpest) pole pair respond to the
//  Resonance knob, like many analogue multi-pole filters.
//
//  Bank B ("Vowels") — human vowels are recognised mostly by their first
//  three FORMANTS: resonant peaks of the vocal tract. Classic speech
//  synthesisers (e.g. the Klatt cascade synthesiser) model a vowel as three
//  all-pole resonators in series, exactly what we build here. Frequencies are
//  the well-known Peterson & Barney (1952) averages for adult male voices,
//  divided by 800 Hz, so with the cutoff near 800 Hz you get natural vowels.

constexpr float kVowelRef = 800.0f;

inline SlotData makeSlot(int n, const SectionData* secs)
{
	SlotData s;
	std::memset(&s, 0, sizeof(s));
	s.numSections = uint8_t(n);
	for (int i = 0; i < kMaxSections; ++i)
		s.sec[i] = (i < n) ? secs[i] : poleQ(1.0f, 0.707f, 0.0f);
	return s;
}

inline SlotData vowel(float f1, float f2, float f3)
{
	const SectionData v[3] = {
		poleQ(f1 / kVowelRef, 4.0f, 0.25f),
		poleQ(f2 / kVowelRef, 6.0f, 0.25f),
		poleQ(f3 / kVowelRef, 8.0f, 0.25f) };
	return makeSlot(3, v);
}

inline void factoryBank(int bank, BankData& b)
{
	std::memset(&b, 0, sizeof(b));
	b.numSlots = kNumSlots;
	if (bank == 0)
	{
		const SectionData lp[3] = { lowZeros(poleQ(1.0f, 0.518f, 0.0f)),
		                            lowZeros(poleQ(1.0f, 0.707f, 0.0f)),
		                            lowZeros(poleQ(1.0f, 1.932f, 0.8f)) };
		const SectionData bp[3] = { bandZeros(poleQ(0.8f, 0.9f, 0.0f)),
		                            bandZeros(poleQ(1.25f, 0.9f, 0.0f)),
		                            bandZeros(poleQ(1.0f, 1.5f, 1.0f)) };
		const SectionData hp[3] = { highZeros(poleQ(1.0f, 0.518f, 0.0f)),
		                            highZeros(poleQ(1.0f, 0.707f, 0.0f)),
		                            highZeros(poleQ(1.0f, 1.932f, 0.8f)) };
		const SectionData tn[3] = { notchAt(poleQ(0.7f, 3.0f, 0.0f), 0.7f),
		                            notchAt(poleQ(1.0f, 1.5f, 0.55f), 1.0f),
		                            notchAt(poleQ(1.4f, 3.0f, 0.0f), 1.4f) };
		const SectionData tp[3] = { poleQ(0.5f, 4.0f, 0.3f),
		                            notchAt(poleQ(1.0f, 2.0f, 0.2f), 1.0f),
		                            highZeros(poleQ(2.0f, 4.0f, 0.3f)) };
		const SectionData qn[4] = { notchAt(poleQ(0.5f, 1.2f, 0.3f), 0.5f),
		                            notchAt(poleQ(0.8f, 1.2f, 0.3f), 0.8f),
		                            notchAt(poleQ(1.3f, 1.2f, 0.3f), 1.3f),
		                            notchAt(poleQ(2.0f, 1.2f, 0.3f), 2.0f) };
		b.slot[0] = makeSlot(3, lp);
		b.slot[1] = makeSlot(3, bp);
		b.slot[2] = makeSlot(3, hp);
		b.slot[3] = makeSlot(3, tn);
		b.slot[4] = makeSlot(3, tp);
		b.slot[5] = makeSlot(4, qn);
	}
	else
	{
		b.slot[0] = vowel(730.0f, 1090.0f, 2440.0f);   // A (father)
		b.slot[1] = vowel(530.0f, 1840.0f, 2480.0f);   // E (bed)
		b.slot[2] = vowel(270.0f, 2290.0f, 3010.0f);   // I (beet)
		b.slot[3] = vowel(570.0f,  840.0f, 2410.0f);   // O (bought)
		b.slot[4] = vowel(300.0f,  870.0f, 2240.0f);   // U (boot)
		b.slot[5] = vowel(490.0f, 1350.0f, 1690.0f);   // ER (bird)
	}
}

// ----------------------------------------------------------------------------
//  Bytes for USB-MIDI SysEx
// ----------------------------------------------------------------------------
//
//  MIDI SysEx messages may only contain bytes 0..127 (7 bits), because a byte
//  with its top bit set means "a new MIDI message starts here". So each 32-bit
//  float is chopped into five 7-bit pieces, least significant first:
//
//      bits  0– 6 | 7–13 | 14–20 | 21–27 | 28–31
//
//  We send the float's raw IEEE-754 bit pattern, so the value arrives exactly,
//  with no rounding. JavaScript does the same with a DataView.

constexpr int kFloatBytes   = 5;
constexpr int kSectionBytes = 5 * kFloatBytes + 1;                 // 5 floats + zero mode
constexpr int kSlotBytes    = 1 + kMaxSections * kSectionBytes;    // = 209

inline void putFloat(float f, uint8_t* out)
{
	uint32_t u; std::memcpy(&u, &f, 4);
	for (int i = 0; i < kFloatBytes; ++i) out[i] = uint8_t((u >> (7 * i)) & 0x7F);
}

inline float getFloat(const uint8_t* in)
{
	uint32_t u = 0;
	for (int i = 0; i < kFloatBytes; ++i) u |= uint32_t(in[i] & 0x7F) << (7 * i);
	float f; std::memcpy(&f, &u, 4);
	return f;
}

// Slot → kSlotBytes bytes. Layout: numSections, then for each of the
// kMaxSections sections: poleRatio, poleDamp, resWeight, zeroA, zeroB
// (5 bytes each), then zeroMode (1 byte).
inline void encodeSlot(const SlotData& s, uint8_t* out)
{
	out[0] = s.numSections & 0x7F;
	uint8_t* p = out + 1;
	for (int i = 0; i < kMaxSections; ++i)
	{
		const SectionData& d = s.sec[i];
		putFloat(d.poleRatio, p);      p += kFloatBytes;
		putFloat(d.poleDamp, p);       p += kFloatBytes;
		putFloat(d.resWeight, p);      p += kFloatBytes;
		putFloat(d.zeroA, p);          p += kFloatBytes;
		putFloat(d.zeroB, p);          p += kFloatBytes;
		*p++ = d.zeroMode & 0x7F;
	}
}

// Bytes → slot. Always sanitizes, so even garbage gives a safe filter.
inline bool decodeSlot(const uint8_t* in, int len, SlotData& s)
{
	if (len < kSlotBytes) return false;
	std::memset(&s, 0, sizeof(s));
	s.numSections = in[0];
	const uint8_t* p = in + 1;
	for (int i = 0; i < kMaxSections; ++i)
	{
		SectionData& d = s.sec[i];
		d.poleRatio = getFloat(p); p += kFloatBytes;
		d.poleDamp  = getFloat(p); p += kFloatBytes;
		d.resWeight = getFloat(p); p += kFloatBytes;
		d.zeroA     = getFloat(p); p += kFloatBytes;
		d.zeroB     = getFloat(p); p += kFloatBytes;
		d.zeroMode  = *p++;
	}
	sanitize(s);
	return true;
}

// ----------------------------------------------------------------------------
//  Flash image
// ----------------------------------------------------------------------------
//
//  Both banks are saved as one block in the last 4 KB sector of flash. A
//  "magic number" and a version number say "this really is Zeds Dead data, in
//  this layout", and a CRC (a checksum) catches corruption, e.g. from a power
//  cut half-way through saving. If any check fails we use the factory banks.

constexpr uint32_t kFlashMagic   = 0x5A454453u;   // "ZEDS" in ASCII
constexpr uint32_t kFlashVersion = 1;

struct FlashImage
{
	uint32_t magic;
	uint32_t version;
	BankData banks[kNumBanks];
	uint32_t crc;
};

// CRC-32 (the same one used by zip files), computed a bit at a time: slow but
// tiny, and we only use it when loading or saving.
inline uint32_t crc32(const uint8_t* data, uint32_t len)
{
	uint32_t crc = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < len; ++i)
	{
		crc ^= data[i];
		for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

inline uint32_t imageCrc(const FlashImage& img)
{
	return crc32(reinterpret_cast<const uint8_t*>(&img), uint32_t(offsetof(FlashImage, crc)));
}

inline bool imageValid(const FlashImage& img)
{
	return img.magic == kFlashMagic && img.version == kFlashVersion && img.crc == imageCrc(img);
}

} // namespace zp

#endif
