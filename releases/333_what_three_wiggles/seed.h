// what.three.wiggles: turning three words into random streams.
//
// This file is the C++ half of the seeding spec in SEEDING.md. web/seed.js is
// the JavaScript half. Both must produce identical numbers, which
// test/host_test.cpp and web/seed.test.mjs check against the same vectors.
//
// Plain C++ with no Pico SDK dependencies, so it also compiles on a desktop.

#ifndef W3W_SEED_H
#define W3W_SEED_H

#include <cstdint>
#include <cstddef>

namespace w3w
{

// Stream IDs. Each output derives its own random stream from a word seed and
// one of these IDs, so changing one engine never shifts the others.
// Append new IDs; never renumber, or existing locations will change sound.
enum StreamId : uint32_t
{
	STREAM_FM       = 1, // Audio 1: per-word operator settings
	STREAM_ADDITIVE = 2, // Audio 2: per-word partial levels and phases
	STREAM_LFO      = 3, // CV 2: per-word LFO components
	STREAM_WALK     = 4, // CV 1: random walk (from the combined seed)
	STREAM_TRIGGERS = 5, // Pulse 1: random triggers (from the combined seed)
	STREAM_DRONE    = 6, // Audio 1: drone pitch (from the combined seed)
	STREAM_LFO_FOLD = 7, // CV 2: wavefolder amount (from the combined seed)
	STREAM_CONFIG   = 8, // CV 1 and Pulse 1: walk character and trigger density (combined seed)
};

// murmur3 32-bit finaliser: a cheap, well-mixed bijection on 32 bits.
constexpr uint32_t Fmix32(uint32_t h)
{
	h ^= h >> 16;
	h *= 0x85EBCA6Bu;
	h ^= h >> 13;
	h *= 0xC2B2AE35u;
	h ^= h >> 16;
	return h;
}

constexpr uint32_t Rotl32(uint32_t x, int r)
{
	return (x << r) | (x >> (32 - r));
}

// FNV-1a over the UTF-8 bytes of one normalised word, then Fmix32.
inline uint32_t HashWord(const uint8_t *bytes, size_t len)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < len; i++)
	{
		h ^= bytes[i];
		h *= 16777619u;
	}
	return Fmix32(h);
}

// One seed built from all three words. Order matters: a.b.c != c.b.a
constexpr uint32_t CombineSeeds(uint32_t s0, uint32_t s1, uint32_t s2)
{
	return Fmix32(s0 ^ Rotl32(s1, 10) ^ Rotl32(s2, 21));
}

// Starting state for the random stream `id` of seed `seed`.
constexpr uint32_t DeriveStream(uint32_t seed, uint32_t id)
{
	return Fmix32(seed + 0x9E3779B9u * (id + 1u));
}

// Small counter-based generator (a 32-bit splitmix variant).
// Deterministic, cheap on a Cortex-M0+, and easy to mirror in JS with Math.imul.
struct Rng
{
	uint32_t state;

	explicit Rng(uint32_t s = 0) : state(s) {}

	uint32_t Next()
	{
		state += 0x9E3779B9u;
		return Fmix32(state);
	}

	// Uniform integer in [0, n), for 1 <= n <= 65536.
	// JS computes the same thing as Math.floor(next * n / 2**32).
	uint32_t Below(uint32_t n)
	{
		return uint32_t((uint64_t(Next()) * n) >> 32);
	}

	// Uniform integer in [lo, hi], inclusive.
	int32_t Range(int32_t lo, int32_t hi)
	{
		return lo + int32_t(Below(uint32_t(hi - lo + 1)));
	}

	// Uniform 16-bit fraction, 0..65535 (Q16).
	uint32_t Unit16()
	{
		return Next() >> 16;
	}
};

// Split a normalised "word.word.word" string (UTF-8, as sent by the webapp)
// into three seeds. Returns false unless there are exactly three non-empty
// words separated by single dots.
inline bool SeedsFromWords(const uint8_t *text, size_t len, uint32_t seeds[3])
{
	size_t start = 0;
	int word = 0;
	for (size_t i = 0; i <= len; i++)
	{
		if (i == len || text[i] == '.')
		{
			if (i == start || word >= 3) return false;
			seeds[word++] = HashWord(text + start, i - start);
			start = i + 1;
		}
	}
	return word == 3;
}

} // namespace w3w

#endif
