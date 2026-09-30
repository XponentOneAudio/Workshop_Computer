// what.three.wiggles: the six output engines and the clock.
//
// Each engine reads its settings from the Patch, keeps its own state and
// random stream, and has a Reset() that puts it back where the seeds start it.
// Plain C++ with no Pico SDK dependencies, so it also compiles on a desktop.

#ifndef W3W_ENGINES_H
#define W3W_ENGINES_H

#include <cstdint>
#include "seed.h"
#include "dsp.h"
#include "patch.h"

namespace w3w
{

////////////////////////////////////////////////////////////////////////////////
// Clock: internal phase accumulator, or rising edges on Pulse In 1

struct Clock
{
	static constexpr uint32_t kMinPeriod = 48;           // 1 ms
	static constexpr uint32_t kMaxPeriod = 48000 * 8;    // 8 s

	uint32_t phase = 0;
	uint32_t period = 24000;      // samples between the last two ticks
	uint32_t sinceTick = 0;
	bool tickNow = false;

	void Reset()
	{
		phase = 0;
		tickNow = true; // internal clock ticks straight away after a reset
	}

	// Returns true on a tick. inc is the internal clock rate.
	bool Process(bool external, bool externalEdge, uint32_t inc)
	{
		if (sinceTick < kMaxPeriod) sinceTick++;
		bool tick;
		if (external)
		{
			tick = externalEdge;
			tickNow = false;
		}
		else
		{
			uint32_t last = phase;
			phase += inc;
			tick = tickNow || phase < last;
			tickNow = false;
		}
		if (tick)
		{
			if (external)
			{
				period = sinceTick < kMinPeriod ? kMinPeriod : sinceTick;
			}
			else
			{
				uint32_t p = inc ? 0xFFFFFFFFu / inc : kMaxPeriod;
				period = p < kMinPeriod ? kMinPeriod : (p > kMaxPeriod ? kMaxPeriod : p);
			}
			sinceTick = 0;
		}
		return tick;
	}
};

////////////////////////////////////////////////////////////////////////////////
// Pulse 1 and Pulse 2: random triggers, and a flip-flop they toggle

struct Triggers
{
	static constexpr int32_t kTriggerSamples = 480; // 10 ms

	Rng rng;
	int32_t remaining = 0;
	bool flipFlop = false;

	void Reset(const Patch &p)
	{
		rng.state = p.triggerState;
		remaining = 0;
		flipFlop = false;
	}

	// density: Q16 chance of a trigger on this tick. One random number is used
	// per tick whatever the density, so turning the knob adds or removes hits
	// without reshuffling the rest of the pattern.
	bool Tick(uint32_t density, uint32_t period)
	{
		uint32_t r = rng.Unit16();
		if (r >= density) return false;
		int32_t len = int32_t(period / 2);
		remaining = len < kTriggerSamples ? len : kTriggerSamples;
		flipFlop = !flipFlop;
		return true;
	}

	bool Process()
	{
		if (remaining > 0)
		{
			remaining--;
			return true;
		}
		return false;
	}
};

// Mix the patch's density (the knob at noon) with a 0..4095 knob:
// fully anticlockwise is silent, fully clockwise fires every tick.
inline uint32_t TriggerDensity(uint32_t patchDensity, int32_t knob)
{
	if (knob < 2048) return (patchDensity * uint32_t(knob)) >> 11;
	return patchDensity + (((65536 - patchDensity) * uint32_t(knob - 2048)) >> 11);
}

////////////////////////////////////////////////////////////////////////////////
// CV 1: a random walk through scale degrees, with random holds and glides

struct Walk
{
	Rng rng;
	int32_t degree = 0;
	int32_t holdLeft = 0;
	int32_t mvQ8 = 0;       // current output, millivolts x 256
	int32_t stepQ8 = 0;     // glide increment per sample
	int32_t slewLeft = 0;   // samples of glide remaining
	int32_t targetQ8 = 0;

	void Reset(const Patch &p, int32_t maxDegree, const Scale &s)
	{
		rng.state = p.walkState;
		degree = int32_t((uint32_t(p.walkStart) * uint32_t(maxDegree + 1)) >> 16);
		holdLeft = 2; // the tick that comes with a reset plays the start note
		slewLeft = 0;
		targetQ8 = mvQ8 = SemitonesToMillivolts(DegreeToSemitone(degree, s)) * 256;
	}

	// Called on each clock tick. period: samples per tick.
	void Tick(const Patch &p, int32_t maxDegree, const Scale &s, uint32_t period)
	{
		if (--holdLeft > 0) return;

		// Always make the same six draws per step, whatever the knobs are
		// doing, so the walk's random sequence never shifts.
		bool leap = rng.Unit16() < p.walkLeap;
		int32_t size = leap ? rng.Range(3, 7) : rng.Range(1, 2);
		bool down = rng.Below(2) != 0;
		int32_t hold = rng.Range(1, p.walkMaxHold);
		bool glide = rng.Unit16() < p.walkGlide;
		uint32_t glideFrac = rng.Unit16();

		degree += down ? -size : size;
		// Reflect off the ends of the range
		if (degree > maxDegree) degree = 2 * maxDegree - degree;
		if (degree < 0) degree = -degree;
		degree = Clamp(degree, 0, maxDegree);
		holdLeft = hold;

		targetQ8 = SemitonesToMillivolts(DegreeToSemitone(degree, s)) * 256;
		uint32_t slew = glide ? uint32_t((uint64_t(period) * uint32_t(hold) * glideFrac) >> 16) : 0;
		if (slew > 1)
		{
			slewLeft = int32_t(slew);
			stepQ8 = (targetQ8 - mvQ8) / slewLeft;
		}
		else
		{
			slewLeft = 0;
			mvQ8 = targetQ8;
		}
	}

	// Returns the output in millivolts.
	int32_t Process()
	{
		if (slewLeft > 0)
		{
			mvQ8 += stepQ8;
			if (--slewLeft == 0) mvQ8 = targetQ8;
		}
		return mvQ8 >> 8;
	}
};

// Highest scale degree for a span of 1..48 semitones above C4.
inline int32_t WalkMaxDegree(int32_t spanSemis, const Scale &s)
{
	int32_t d = (spanSemis * s.count) / 12;
	return d < 1 ? 1 : d;
}

////////////////////////////////////////////////////////////////////////////////
// CV 2: three sines at seeded ratios, cross-modulated and wavefolded

struct ComplexLfo
{
	uint32_t phase[3] = {0, 0, 0};
	int32_t last3 = 0;

	void Reset(const Patch &p)
	{
		for (int n = 0; n < 3; n++) phase[n] = p.lfoPhase0[n];
		last3 = 0;
	}

	// baseInc: phase increment of the base rate. Returns Q15.
	int32_t Process(const Patch &p, uint32_t baseInc)
	{
		int32_t pm = last3 * p.lfoCrossMod; // up to about a quarter cycle
		int32_t s0 = Sine(phase[0] + uint32_t(pm));
		int32_t s1 = Sine(phase[1]);
		int32_t s2 = Sine(phase[2]);
		last3 = s2;
		for (int n = 0; n < 3; n++) phase[n] += (baseInc * p.lfoRatio[n]) >> 8;

		int32_t sum = (s0 * p.lfoLevel[0] + s1 * p.lfoLevel[1] + s2 * p.lfoLevel[2]) >> 15;
		int32_t x = (sum * p.lfoFold) >> 8;
		// Triangle wavefolder: reflect back into range (gain is at most 3x)
		while (x > 32767 || x < -32767)
		{
			if (x > 32767) x = 65534 - x;
			if (x < -32767) x = -65534 - x;
		}
		return x;
	}
};

////////////////////////////////////////////////////////////////////////////////
// Audio 2: additive oscillator of 16 harmonic partials

struct Additive
{
	// Partials above about 18 kHz are dropped, so nothing aliases
	static constexpr uint32_t kMaxInc = uint32_t(4294967296.0 * 18000.0 / 48000.0);

	uint32_t phase = 0;

	int32_t Process(const Patch &p, uint32_t inc)
	{
		phase += inc;
		uint32_t kmax = inc ? kMaxInc / inc : kNumPartials;
		if (kmax > uint32_t(kNumPartials)) kmax = kNumPartials;
		int32_t acc = 0;
		uint32_t ph = 0;
		for (uint32_t k = 0; k < kmax; k++)
		{
			ph += phase; // (k+1) x phase, wrapping
			acc += p.partialLevel[k] * Sine(ph + p.partialPhase[k]);
		}
		return acc >> 15;
	}
};

////////////////////////////////////////////////////////////////////////////////
// Audio 1: three operators in a feedback FM loop (1 -> 2 -> 3 -> 1),
// or ring modulated

struct FmLoop
{
	uint32_t phase[3] = {0, 0, 0};
	int32_t out[3] = {0, 0, 0};
	int32_t fbLast = 0;
	int32_t dcIn = 0, dcOut = 0;

	void Reset(const Patch &p)
	{
		for (int n = 0; n < 3; n++)
		{
			phase[n] = p.fmPhase0[n];
			out[n] = 0;
		}
		fbLast = dcIn = dcOut = 0;
	}

	// depth: 0..4095 global FM depth (or ring mod amount). Returns Q15.
	int32_t Process(const Patch &p, int32_t depth, bool ringMod)
	{
		int32_t mix;
		if (!ringMod)
		{
			// Average the feedback over two samples, which tames the
			// high-frequency hunting that raw feedback FM falls into.
			int32_t fb = (out[2] + fbLast) >> 1;
			fbLast = out[2];
			// Each link's index is at most about pi radians
			int32_t d0 = (depth * p.fmDepth[0]) >> 12;
			int32_t d1 = (depth * p.fmDepth[1]) >> 12;
			int32_t d2 = (depth * p.fmDepth[2]) >> 12;
			out[0] = Sine(phase[0] + uint32_t(fb * d0));
			out[1] = Sine(phase[1] + uint32_t(out[0] * d1));
			out[2] = Sine(phase[2] + uint32_t(out[1] * d2));
			mix = (out[0] * p.fmLevel[0] + out[1] * p.fmLevel[1] + out[2] * p.fmLevel[2]) >> 15;
		}
		else
		{
			// Depth crossfades from operator 1 alone to 1 x 2 x 3
			for (int n = 0; n < 3; n++) out[n] = Sine(phase[n]);
			fbLast = out[2];
			int32_t rm = (((out[0] * out[1]) >> 15) * out[2]) >> 15;
			mix = out[0] + (((rm - out[0]) * depth) >> 12);
		}
		for (int n = 0; n < 3; n++) phase[n] += p.fmInc[n];

		// DC blocker: y = x - x' + 0.998 y' (about 15 Hz)
		int32_t y = mix - dcIn + dcOut - (dcOut >> 9);
		dcIn = mix;
		dcOut = y;
		return Clamp(y, -32767, 32767);
	}
};

} // namespace w3w

#endif
