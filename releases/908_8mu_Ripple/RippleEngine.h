// Ripple engine
//
// Hardware-independent DSP for the 8mu Ripple card, so that it can also be
// built and auditioned on a desktop computer.
//
// Signal flow:
//
//   pitch --> clock oscillator --(tick)--> LFSR noise --> colour --> 1-bit source
//                ^      |                                               |
//                |      +--> filter mod (square/triangle)               v
//                |                                      ripple counter (/2../32)
//                |                                                      |
//                +---- slew <---- mono mix of /1 /2 /4 /8 /16 /32 <-----+
//                                         |
//                              pan, + hiss, drive, soft clip
//                                         |
//                                 stereo LP/BP filter --> volume --> out
//
// All arithmetic is fixed point. Parameters are Q12 (0 to 4095) unless noted.
// Pitches are in Q12 octaves: 4096 per octave.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>

#ifndef __not_in_flash_func
#define __not_in_flash_func(f) f
#endif


namespace ripple
{

static constexpr int numTaps = 6;
static constexpr int numLengths = 15;

// Register lengths with two-tap maximal-length feedback (1-indexed taps).
// Short lengths loop quickly, giving pitched, metallic noise.
static constexpr uint8_t lfsrTaps[numLengths][2] = {
	{3, 2}, {4, 3}, {5, 3}, {6, 5}, {7, 6}, {9, 5}, {10, 7}, {11, 9},
	{15, 14}, {17, 14}, {20, 17}, {23, 18}, {25, 22}, {28, 25}, {31, 28}};

// Clock pitch range, in Q12 octaves above 1Hz
static constexpr int32_t minPitch = -8 * 4096;
static constexpr int32_t maxPitch = 15 * 4096 + 2400; // ~48kHz, one tick per sample

// Filter cutoff range, in Q12 octaves above 20Hz (20Hz to ~16kHz)
static constexpr int32_t maxCutoff = 39500;
static constexpr int cutoffTableShift = 5;
static constexpr int cutoffTableSize = (maxCutoff >> cutoffTableShift) + 2;


static inline int32_t Clip(int32_t x, int32_t lo, int32_t hi)
{
	if (x < lo)
	{
		return lo;
	}
	if (x > hi)
	{
		return hi;
	}
	return x;
}

// Cubic soft clip, +-16384 in and out, unity gain near zero
static inline int32_t SoftClip(int32_t x)
{
	x = Clip(x, -24576, 24576);
	// Rescale so that +-24576 maps to the knee at u = +-1
	int32_t u = (x * 21845) >> 15;
	return (3 * u - (((u * u) >> 14) * u >> 14)) >> 1;
}


////////////////////////////////////////////////////////////////////////////////
// Tables, filled once at startup

struct Tables
{
	uint32_t exp2[257];             // 2^(i/256) in Q16
	int32_t g[cutoffTableSize];      // tan(pi*fc/48000) in Q16

	void Init()
	{
		for (int i = 0; i <= 256; i++)
		{
			exp2[i] = (uint32_t)(exp2f((float)i / 256.0f) * 65536.0f + 0.5f);
		}
		for (int i = 0; i < cutoffTableSize; i++)
		{
			float hz = 20.0f * exp2f((float)(i << cutoffTableShift) / 4096.0f);
			if (hz > 20000.0f)
			{
				hz = 20000.0f;
			}
			g[i] = (int32_t)(tanf((float)M_PI * hz / 48000.0f) * 65536.0f + 0.5f);
		}
	}
};

inline Tables tables;

// 2^(p/4096) in Q16, for p from -32 to +31 octaves
static inline uint64_t Exp2Q16(int32_t p)
{
	int32_t oct = p >> 12; // floor
	uint32_t fr = (uint32_t)p & 4095;
	uint32_t i = fr >> 4, t = fr & 15;
	uint32_t m = tables.exp2[i] + (((tables.exp2[i + 1] - tables.exp2[i]) * t) >> 4);
	if (oct >= 0)
	{
		return (uint64_t)m << oct;
	}
	return (oct > -32) ? (m >> -oct) : 0;
}


////////////////////////////////////////////////////////////////////////////////
// Topology-preserving state variable filter, with low and band outputs.
//
// Same fixed-point scheme as the bandpass in 8mu EQ: internal state clipped
// to +-32767, and the coefficients recomputed (one divide) whenever the
// cutoff moves, which here is every sample.

struct SVFCoeffs
{
	int32_t a1v = 0, a2v = 0, a3v = 0;

	void Set(int32_t gFixed, int32_t kFixed)
	{
		int32_t gpkFp = gFixed + kFixed;
		int32_t gHi = gFixed >> 8, gLo = gFixed & 0xFF;
		int32_t gkHi = gpkFp >> 8, gkLo = gpkFp & 0xFF;
		int32_t pHi = gHi * gkHi;
		int32_t pMid = gHi * gkLo + gLo * gkHi;
		int32_t pLo = gLo * gkLo;
		int32_t denomFp = (1 << 16) + pHi + (((pMid << 8) + pLo + 65535) >> 16);

		a1v = 0x80000000 / denomFp;
		a2v = (a1v * gFixed) >> 16;
		a3v = (a2v * gFixed) >> 16;
	}
};

class SVF
{
public:
	// Returns lowpass when band is false, bandpass otherwise
	int32_t __not_in_flash_func(Process)(int32_t x, const SVFCoeffs &c, bool band)
	{
		int32_t v3 = Clip(x - ic2eq, -32767, 32767);
		ic1eq = Clip(ic1eq, -32767, 32767);

		int32_t v1a = c.a1v * ic1eq + c.a2v * v3 + v1rem;
		v1rem = v1a & 0x3FFF;
		int32_t v1 = v1a >> 14;

		int32_t v2a = c.a2v * ic1eq + c.a3v * v3 + v2rem;
		v2rem = v2a & 0x3FFF;
		int32_t v2 = v2a >> 14;

		int32_t lp = ic2eq + (v2 >> 1);
		int32_t bp = v1 >> 1;

		ic1eq = v1 - ic1eq;
		ic2eq = Clip(v2 + ic2eq, -32767, 32767);

		return band ? bp : lp;
	}

	void Reset()
	{
		ic1eq = ic2eq = v1rem = v2rem = 0;
	}

private:
	int32_t ic1eq = 0, ic2eq = 0, v1rem = 0, v2rem = 0;
};


////////////////////////////////////////////////////////////////////////////////

struct Params
{
	// Play page
	int32_t level[numTaps] = {0, 2048, 2458, 1638, 1229, 2048};
	int32_t noiseMod = 1229;  // divider mix --> clock pitch
	int32_t filterMod = 1229; // clock --> filter cutoff

	// Noise page
	int32_t length = 4095;     // LFSR length, 3 to 31 bits
	int32_t colour = 0;        // clock-noise colour, white to deep red
	int32_t hiss = 0;          // level of free-running noise bed
	int32_t hissColour = 2048; // white, pink, brown
	int32_t drive = 2048;      // gain into the filter, x0.25 to x4
	int32_t modShape = 0;      // filter mod, square to triangle
	int32_t fbSlew = 0;        // smoothing of the feedback signal
	int32_t keyTrack = 0;      // clock pitch --> filter cutoff

	// Divide page
	int32_t pan[numTaps] = {2048, 2048, 2048, 2048, 2048, 2048};
	int32_t countLength = 4095; // counter resets after 2 to 64 edges
	int32_t edges = 0;          // count rising edges, or both edges

	// Motion page (the tilt amounts are applied in main.cpp)
	int32_t glide = 0;
	int32_t volume = 3072;

	// Knobs and CV, already combined
	int32_t pitch = 6 * 4096;   // Q12 octaves above 1Hz
	int32_t cutoff = 7 * 4096;  // Q12 octaves above 20Hz
	int32_t resonance = 1024;
	bool bandpass = false;
	bool freeze = false;
};

struct Inputs
{
	bool useExtClock = false;
	bool extClock = false;       // rising edge on the external clock
	bool reset = false;
	bool useAudioSource = false; // compare an audio input instead of the LFSR
	int32_t audioSource = 0;     // +-2048
	int32_t fm = 0;              // Q12 octaves, added to the clock pitch
	int32_t cutoffMod = 0;       // Q12 octaves, added to the cutoff
};

struct Outputs
{
	int32_t left = 0, right = 0; // +-2047
	bool tap[numTaps] = {};
	bool clockHigh = false;
	bool cyclePulse = false;     // high for 5ms when the counter resets
	int32_t randomCV = 0;        // +-2047, new value each counter cycle
	int32_t modCV = 0;           // +-2047, the feedback signal
};


class Engine
{
public:
	Engine()
	{
		tables.Init();
		Update(Params());
		for (int i = 0; i < numTaps; i++)
		{
			levelS[i] = target.level[i] << 4;
		}
		pitchS = (int64_t)target.pitch * 65536;
	}

	// Called at control rate. Converts parameters to internal coefficients.
	void Update(const Params &p)
	{
		target = p;

		int idx = Clip((p.length * numLengths) >> 12, 0, numLengths - 1);
		len = lfsrTaps[idx][0];
		tap2 = lfsrTaps[idx][1];
		mask = (1u << len) - 1;

		colourK = (int32_t)(Exp2Q16(-p.colour * 10) >> 1);       // Q15, 32768 to 32
		countN = 2 + ((p.countLength * 63) >> 12);               // 2 to 64
		bothEdges = p.edges >= 2048;
		driveGain = (int32_t)(Exp2Q16((p.drive - 2048) * 2) >> 4); // Q12, x0.25 to x4
		fbAlpha = (int32_t)Exp2Q16(-p.fbSlew * 14);              // Q16
		glideAlpha = (int32_t)Exp2Q16(-p.glide * 14);            // Q16
		noiseModAmt = (p.noiseMod * p.noiseMod) >> 12;
		filterModAmt = (p.filterMod * p.filterMod) >> 12;
		hissAmt = (p.hiss * p.hiss) >> 12;
		volumeAmt = (p.volume * p.volume) >> 12;

		// Resonance --> damping k = 2 (none) down to ~0.02, in Q16
		int32_t r = 4096 - Clip(p.resonance, 0, 4095);
		kFixed = ((r * r) >> 7) + 1300;

		for (int i = 0; i < numTaps; i++)
		{
			int32_t pan = Clip(p.pan[i], 0, 4096);
			panL[i] = std::min<int32_t>(4096, 2 * (4096 - pan));
			panR[i] = std::min<int32_t>(4096, 2 * pan);
		}
	}

	Outputs __not_in_flash_func(Process)(const Inputs &in)
	{
		Outputs out;

		// Smooth tap levels (8mu faders move in steps of 32)
		for (int i = 0; i < numTaps; i++)
		{
			levelS[i] += ((target.level[i] << 4) - levelS[i]) >> 6;
		}

		// Clock pitch: glide on the control pitch, then feedback and FM
		pitchS += ((int64_t)target.pitch * 65536 - pitchS) * glideAlpha >> 16;
		int32_t p = (int32_t)(pitchS >> 16);
		p += ((fbS >> 4) * noiseModAmt >> 12) * 2;
		p += in.fm;
		p = Clip(p, minPitch, maxPitch);

		uint64_t inc = (89478ull * Exp2Q16(p)) >> 16; // 2^32 / 48000 per Hz
		if (inc > 0xFFFFFFFFull)
		{
			inc = 0xFFFFFFFFull;
		}
		uint32_t oldPhase = phase;
		phase += (uint32_t)inc;
		bool wrapped = phase < oldPhase;

		if (in.reset)
		{
			Reset();
		}

		bool tick = in.useExtClock ? in.extClock : wrapped;
		if (tick)
		{
			Tick(in);
		}

		// Taps: /1 is the noise bit itself, /2 to /32 the counter
		int32_t mono = 0, sumL = 0, sumR = 0;
		for (int i = 0; i < numTaps; i++)
		{
			bool t = (i == 0) ? srcBit : ((count >> (i - 1)) & 1);
			out.tap[i] = t;
			int32_t v = t ? (levelS[i] >> 4) : -(levelS[i] >> 4);
			mono += v;
			sumL += (v * panL[i]) >> 12;
			sumR += (v * panR[i]) >> 12;
		}

		// Feedback signal, optionally slewed
		fbS += ((int64_t)(mono * 16 - fbS) * fbAlpha) >> 16;

		// Free-running hiss bed, shared by both channels
		int32_t h = Hiss();
		h = ((h * hissAmt) >> 12) * 2;
		sumL += h;
		sumR += h;

		// Drive and soft clip into the filter
		int32_t xL = SoftClip((sumL * driveGain) >> 12);
		int32_t xR = SoftClip((sumR * driveGain) >> 12);

		// Filter modulation from the clock oscillator
		int32_t sq = (phase >> 31) ? 4096 : -4096;
		int32_t t = (int32_t)(phase >> 19); // 0 to 8191
		int32_t tri = ((t < 4096) ? t : 8191 - t) * 2 - 4096;
		int32_t mod = sq + (((tri - sq) * target.modShape) >> 12);

		int32_t c = target.cutoff;
		c += ((mod * filterModAmt) >> 12) * 3;                        // +-3 octaves
		c += ((p - 8 * 4096) * target.keyTrack) >> 12;                // relative to 256Hz
		c += in.cutoffMod;
		c = Clip(c, 0, maxCutoff);

		coeffs.Set(tables.g[c >> cutoffTableShift], kFixed);
		int32_t yL = svf[0].Process(xL, coeffs, target.bandpass);
		int32_t yR = svf[1].Process(xR, coeffs, target.bandpass);

		// Block DC (corner ~4Hz): with a slow clock, the taps sit still for
		// long stretches, which would otherwise hold the outputs off centre
		dcL += (yL * 4096 - dcL) >> 11;
		dcR += (yR * 4096 - dcR) >> 11;
		yL -= dcL >> 12;
		yR -= dcR >> 12;

		out.left = Clip(SoftClip((yL * volumeAmt) >> 11) >> 3, -2047, 2047);
		out.right = Clip(SoftClip((yR * volumeAmt) >> 11) >> 3, -2047, 2047);

		out.clockHigh = phase >> 31;
		if (pulseTimer > 0)
		{
			pulseTimer--;
		}
		out.cyclePulse = pulseTimer > 0;
		out.randomCV = randomCV;
		out.modCV = Clip((int32_t)(fbS >> 4) >> 1, -2047, 2047);
		return out;
	}

	void Reset()
	{
		reg = 0xACE1u & mask;
		if (reg == 0)
		{
			reg = 1;
		}
		count = 0;
		colourState = 0;
	}

private:
	void __not_in_flash_func(Tick)(const Inputs &in)
	{
		// Step the shift register. When frozen, the bit leaving the register
		// is fed straight back in, so the current pattern loops exactly.
		uint32_t bit;
		if (target.freeze)
		{
			bit = (reg >> (len - 1)) & 1;
		}
		else
		{
			bit = ((reg >> (len - 1)) ^ (reg >> (tap2 - 1))) & 1;
		}
		reg = ((reg << 1) | bit) & mask;
		if (reg == 0)
		{
			reg = 1;
		}

		bool newBit;
		if (in.useAudioSource)
		{
			// Comparator with a little hysteresis
			newBit = srcBit;
			if (in.audioSource > 48)
			{
				newBit = true;
			}
			else if (in.audioSource < -48)
			{
				newBit = false;
			}
		}
		else
		{
			// The register read as a signed number is white noise. Low-passing
			// it, once per tick, before taking its sign, gives longer runs of
			// ones and zeros, so the divider rhythms clump and stretch.
			int32_t w = (int32_t)(reg << (32 - len)) >> 16;
			colourState += ((w - colourState) * colourK) >> 15;
			newBit = colourState >= 0;
		}

		bool edge = bothEdges ? (newBit != srcBit) : (newBit && !srcBit);
		srcBit = newBit;
		if (edge)
		{
			if (++count >= countN)
			{
				count = 0;
				pulseTimer = 240;
				randomCV = (int32_t)(reg << (32 - len)) >> 20;
			}
		}
	}

	int32_t __not_in_flash_func(Hiss)()
	{
		lcg = 1664525 * lcg + 1013904223;
		int32_t w = (int32_t)(lcg >> 19) - 4096; // +-4096

		// Paul Kellet's economy pink filter, two stages in Q4
		pink0 += (w * 51920 - pink0 * 77) >> 15;
		pink1 += (w * 155456 - pink1 * 1213) >> 15;
		pink2 += (w * 34494 - pink2 * 14090) >> 15;
		int32_t pink = (((pink0 >> 4) + (pink1 >> 4) + pink2 + ((w * 6056) >> 15)) * 3) >> 3;

		// Leaky integrator, corner ~15Hz
		brown += w - (brown >> 9);
		int32_t br = brown >> 4;

		int32_t c = target.hissColour;
		if (c < 2048)
		{
			return w + (((pink - w) * c) >> 11);
		}
		return pink + (((br - pink) * (c - 2048)) >> 11);
	}

	Params target;

	// Derived coefficients
	int len = 31, tap2 = 28;
	uint32_t mask = 0x7FFFFFFF;
	int32_t colourK = 32768, countN = 64;
	bool bothEdges = false;
	int32_t driveGain = 4096, fbAlpha = 65536, glideAlpha = 65536;
	int32_t noiseModAmt = 0, filterModAmt = 0, hissAmt = 0, volumeAmt = 0;
	int32_t kFixed = 131072;
	int32_t panL[numTaps] = {}, panR[numTaps] = {};

	// State
	uint32_t phase = 0;
	uint32_t reg = 0xACE1;
	int32_t colourState = 0;
	bool srcBit = false;
	int32_t count = 0;
	int32_t pulseTimer = 0;
	int32_t randomCV = 0;
	int32_t levelS[numTaps] = {};
	int64_t pitchS = 0, fbS = 0;
	uint32_t lcg = 1;
	int32_t pink0 = 0, pink1 = 0, pink2 = 0, brown = 0;
	int32_t dcL = 0, dcR = 0;
	SVFCoeffs coeffs;
	SVF svf[2];
};

} // namespace ripple
