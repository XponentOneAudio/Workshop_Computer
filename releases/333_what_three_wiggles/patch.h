// what.three.wiggles: everything the three words decide, in one struct.
//
// BuildPatch() turns three seeds into the parameters used by every engine.
// It runs at boot and on the USB core when new words arrive, never in the
// audio callback, so it is free to use floats.

#ifndef W3W_PATCH_H
#define W3W_PATCH_H

#include <cstdint>
#include "seed.h"
#include "dsp.h"

namespace w3w
{

constexpr int kNumPartials = 16;

struct Patch
{
	uint32_t seed[3];
	uint32_t combined;

	// Audio 1: three operators, operator n set by word n
	uint32_t fmInc[3];      // drone pitch x operator ratio
	uint32_t fmPhase0[3];
	uint16_t fmDepth[3];    // Q16 share of the global FM depth for each link
	int16_t fmLevel[3];     // Q15 output mix, sums to at most 32767

	// Audio 2: harmonic partials, grouped low / mid / high by word 1 / 2 / 3
	int16_t partialLevel[kNumPartials]; // Q15, sums to at most 32767
	uint32_t partialPhase[kNumPartials];

	// CV 2: three sine components, component n set by word n
	uint16_t lfoRatio[3];   // Q8 rate multiplier, 0.25x to 4x
	uint32_t lfoPhase0[3];
	int16_t lfoLevel[3];    // Q15, sums to at most 32767
	uint16_t lfoFold;       // Q8 wavefolder gain, 1x to 3x
	uint16_t lfoCrossMod;   // Q15 amount that component 3 phase-modulates component 1

	// CV 1: random walk
	uint32_t walkState;     // starting state of the walk's random stream
	uint16_t walkLeap;      // Q16 chance of a leap (3-7 degrees) instead of a step (1-2)
	uint16_t walkGlide;     // Q16 chance that a note glides rather than jumps
	uint16_t walkStart;     // Q16 start position within the range
	uint8_t walkMaxHold;    // each note lasts 1..walkMaxHold clock ticks

	// Pulse 1: random triggers
	uint32_t triggerState;  // starting state of the trigger random stream
	uint16_t triggerDensity; // Q16 chance of a trigger per tick (knob at noon)
};

// Frequency ratios for the FM operators: some harmonic, some not.
inline constexpr float kFmRatios[16] = {
	0.5f, 1.0f, 1.0f, 1.5f, 2.0f, 2.0f, 3.0f, 4.0f,
	5.0f, 7.0f, 1.41421356f, 2.61803399f, 0.70710678f, 3.14159265f, 1.00350000f, 6.0f,
};

// Scale n raw weights so they sum to at most 32767.
inline void NormaliseQ15(const uint32_t *raw, int16_t *out, int n)
{
	uint64_t sum = 0;
	for (int i = 0; i < n; i++) sum += raw[i];
	for (int i = 0; i < n; i++)
	{
		out[i] = sum ? int16_t((uint64_t(raw[i]) * 32767u) / sum) : 0;
	}
}

inline void BuildPatch(const uint32_t seeds[3], Patch &p)
{
	for (int n = 0; n < 3; n++) p.seed[n] = seeds[n];
	p.combined = CombineSeeds(seeds[0], seeds[1], seeds[2]);

	// Audio 1: drone somewhere between C2 and G3, then one operator per word
	Rng drone(DeriveStream(p.combined, STREAM_DRONE));
	float droneHz = kC4Hz * exp2f(float(drone.Range(-24, -5)) / 12.0f);
	uint32_t fmRaw[3];
	for (int n = 0; n < 3; n++)
	{
		Rng r(DeriveStream(seeds[n], STREAM_FM));
		float ratio = kFmRatios[r.Below(16)];
		float cents = float(r.Range(-8, 8));
		p.fmInc[n] = IncFromHz(droneHz * ratio * exp2f(cents / 1200.0f));
		p.fmPhase0[n] = r.Next();
		p.fmDepth[n] = uint16_t(16384 + r.Below(49152)); // 0.25 to 1.0
		fmRaw[n] = 13107 + r.Below(52429);               // 0.2 to 1.0
	}
	NormaliseQ15(fmRaw, p.fmLevel, 3);

	// Audio 2: partials 1-5 from word 1, 6-11 from word 2, 12-16 from word 3.
	// Higher partials are quieter on average, and some are silenced so each
	// word gives the spectrum a distinct gap-toothed shape.
	uint32_t partialRaw[kNumPartials];
	for (int n = 0; n < 3; n++)
	{
		Rng r(DeriveStream(seeds[n], STREAM_ADDITIVE));
		int first = (n == 0) ? 0 : (n == 1) ? 5 : 11;
		int last = (n == 0) ? 5 : (n == 1) ? 11 : kNumPartials;
		for (int k = first; k < last; k++)
		{
			uint32_t level = r.Unit16();
			bool silent = r.Below(100) < 30;
			p.partialPhase[k] = r.Next();
			if (k == 0)
			{
				level = 32768 + (level >> 1); // fundamental always present
			}
			else if (silent)
			{
				level = 0;
			}
			float tilt = 1.0f / powf(float(k + 1), 0.7f);
			partialRaw[k] = uint32_t(float(level) * tilt);
		}
	}
	NormaliseQ15(partialRaw, p.partialLevel, kNumPartials);

	// CV 2: one sine component per word, then a wavefolder and cross-mod
	uint32_t lfoRaw[3];
	for (int n = 0; n < 3; n++)
	{
		Rng r(DeriveStream(seeds[n], STREAM_LFO));
		float ratio = exp2f(float(r.Range(-2000, 2000)) / 1000.0f);
		p.lfoRatio[n] = uint16_t(lrintf(256.0f * ratio));
		p.lfoPhase0[n] = r.Next();
		lfoRaw[n] = 19661 + r.Below(45875); // 0.3 to 1.0
	}
	NormaliseQ15(lfoRaw, p.lfoLevel, 3);
	Rng fold(DeriveStream(p.combined, STREAM_LFO_FOLD));
	p.lfoFold = uint16_t(256 + fold.Below(513));
	p.lfoCrossMod = uint16_t(fold.Below(32768));

	// CV 1 and Pulse 1: character of the walk and trigger density
	Rng cfg(DeriveStream(p.combined, STREAM_CONFIG));
	p.walkLeap = uint16_t(cfg.Below(16384));          // 0 to 25%
	p.walkGlide = uint16_t(16384 + cfg.Below(42598)); // 25% to 90%
	p.walkStart = uint16_t(cfg.Unit16());
	p.walkMaxHold = uint8_t(cfg.Range(1, 4));
	p.triggerDensity = uint16_t(13107 + cfg.Below(39322)); // 20% to 80%

	p.walkState = DeriveStream(p.combined, STREAM_WALK);
	p.triggerState = DeriveStream(p.combined, STREAM_TRIGGERS);
}

} // namespace w3w

#endif
