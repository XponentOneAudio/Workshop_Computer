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
//                |                                  dividers (/2 /4 /8 /16 /32,
//                |                                  or any of 2..37, primes too)
//                |                                                      |
//                |                                  logic: XOR/AND/OR/ANDNOT
//                |                                  of adjacent divider pairs
//                |                                                      |
//                +---- slew <---- mono mix of taps and logic outputs <--+
//                                         |
//                              pan, + hiss, drive, soft clip
//                                         |
//                    stereo filter (SVF LP/BP/HP/notch, ladder, comb)
//                                         |
//                                 DC block --> volume --> out
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

static constexpr int numTaps = 6;     // /1 (raw bit) and five dividers
static constexpr int numDividers = numTaps - 1;
static constexpr int numLogic = 4;
static constexpr int numSources = numTaps + numLogic;
static constexpr int numLengths = 15;

// Register lengths with two-tap maximal-length feedback (1-indexed taps).
// Short lengths loop quickly, giving pitched, metallic noise.
static constexpr uint8_t lfsrTaps[numLengths][2] = {
	{3, 2}, {4, 3}, {5, 3}, {6, 5}, {7, 6}, {9, 5}, {10, 7}, {11, 9},
	{15, 14}, {17, 14}, {20, 17}, {23, 18}, {25, 22}, {28, 25}, {31, 28}};

// Divide ratios selectable for each divider: the powers of two, so the
// classic binary ripple divider is always available, and the primes.
static constexpr int numRatios = 16;
static constexpr uint8_t ratios[numRatios] = {
	2, 3, 4, 5, 7, 8, 11, 13, 16, 17, 19, 23, 29, 31, 32, 37};

// Fader value that selects entry i of a list of n
static constexpr int32_t ListValue(int i, int n)
{
	return (i * 4096 + 2048) / n;
}

// Ratio list entries for /2, /4, /8, /16, /32
static constexpr int binaryRatioIndex[numDividers] = {0, 2, 5, 8, 14};

enum LogicFunc
{
	LogicXor,
	LogicAnd,
	LogicOr,
	LogicAndNot,
	numLogicFuncs
};

enum FilterType
{
	FilterLP,
	FilterBP,
	FilterHP,
	FilterNotch,
	FilterLadder,
	FilterComb,
	numFilterTypes
};

// Clock pitch range, in Q12 octaves above 1Hz
static constexpr int32_t minPitch = -8 * 4096;
static constexpr int32_t maxPitch = 15 * 4096 + 2400; // ~48kHz, one tick per sample

// Filter cutoff range, in Q12 octaves above 20Hz (20Hz to ~16kHz)
static constexpr int32_t maxCutoff = 39500;
static constexpr int cutoffTableShift = 5;
static constexpr int cutoffTableSize = (maxCutoff >> cutoffTableShift) + 2;
static constexpr int32_t log2Of20 = 17702; // Q12

static constexpr int combSize = 2048;   // power of two; lowest comb pitch ~23Hz
static constexpr int32_t combMinPeriod = 2 << 8;
static constexpr int32_t combMaxPeriod = (combSize - 8) << 8;


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
// Topology-preserving state variable filter, with lowpass, bandpass, highpass
// and notch outputs.
//
// Same fixed-point scheme as the bandpass in 8mu EQ: internal state clipped
// to +-32767, and the coefficients recomputed (one divide) whenever the
// cutoff moves, which here is every sample.

struct SVFCoeffs
{
	int32_t a1v = 0, a2v = 0, a3v = 0;
	int32_t k = 0;

	void Set(int32_t gFixed, int32_t kFixed)
	{
		k = kFixed;
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
	int32_t __not_in_flash_func(Process)(int32_t x, const SVFCoeffs &c, int type)
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

		switch (type)
		{
		case FilterBP:
			return bp;
		case FilterHP:
			return Clip(x - lp - (((c.k >> 4) * bp) >> 12), -32767, 32767);
		case FilterNotch:
			return Clip(x - (((c.k >> 4) * bp) >> 12), -32767, 32767);
		default:
			return lp;
		}
	}

private:
	int32_t ic1eq = 0, ic2eq = 0, v1rem = 0, v2rem = 0;
};


////////////////////////////////////////////////////////////////////////////////
// Four-pole transistor-ladder-style lowpass: four one-pole TPT stages, with
// global feedback taken from the previous sample and saturated at the input.
// Self-oscillates at the top of the resonance range.

class Ladder
{
public:
	// G = g/(1+g) in Q15, res is the feedback gain in Q12 (0 to ~4.2)
	int32_t __not_in_flash_func(Process)(int32_t x, int32_t G, int32_t res)
	{
		int32_t u = SoftClip(x - ((res * last) >> 12));
		for (int s = 0; s < 4; s++)
		{
			int32_t v = (Clip(u - z[s], -65535, 65535) * G) >> 15;
			int32_t y = v + z[s];
			z[s] = Clip(y + v, -32767, 32767);
			u = y;
		}
		last = u;
		// Partly make up the passband loss as resonance rises
		return Clip((u * (4096 + res / 3)) >> 12, -32767, 32767);
	}

private:
	int32_t z[4] = {}, last = 0;
};


////////////////////////////////////////////////////////////////////////////////
// Feedback comb filter, with a one-pole lowpass in the loop for
// Karplus-Strong-style damping. Its fundamental is the filter cutoff.

class Comb
{
public:
	// periodQ8 in samples, fb and damp in Q15
	int32_t __not_in_flash_func(Process)(int32_t x, int32_t periodQ8, int32_t fb, int32_t damp)
	{
		int32_t ip = periodQ8 >> 8, fr = periodQ8 & 255;
		int r0 = (w - ip) & (combSize - 1);
		int r1 = (r0 - 1) & (combSize - 1);
		int32_t d = buf[r0] + (((buf[r1] - buf[r0]) * fr) >> 8);
		lp += ((d - lp) * damp) >> 15;
		int32_t y = Clip(x + ((lp * fb) >> 15), -32767, 32767);
		buf[w] = (int16_t)y;
		w = (w + 1) & (combSize - 1);
		return y;
	}

private:
	int16_t buf[combSize] = {};
	int w = 0;
	int32_t lp = 0;
};


////////////////////////////////////////////////////////////////////////////////

struct Params
{
	// Play page
	int32_t level[numTaps] = {0, 2048, 2458, 1638, 1229, 2048};
	int32_t noiseMod = 1229;  // source mix --> clock pitch
	int32_t filterMod = 1229; // clock --> filter cutoff

	// Logic page: outputs A-D combine dividers (1,2), (2,3), (3,4), (4,5)
	int32_t logicLevel[numLogic] = {0, 0, 0, 0};
	int32_t logicFunc[numLogic] = {
		ListValue(LogicXor, numLogicFuncs), ListValue(LogicXor, numLogicFuncs),
		ListValue(LogicXor, numLogicFuncs), ListValue(LogicXor, numLogicFuncs)};

	// Noise page
	int32_t length = 4095;     // LFSR length, 3 to 31 bits
	int32_t colour = 0;        // clock-noise colour, white to deep red
	int32_t hiss = 0;          // level of free-running noise bed
	int32_t hissColour = 2048; // white, pink, brown
	int32_t fbSlew = 0;        // smoothing of the feedback signal

	// Filter page
	int32_t combDamp = 1024;   // lowpass in the comb loop
	int32_t modShape = 0;      // filter mod, square to triangle
	int32_t keyTrack = 0;      // clock pitch --> filter cutoff
	int32_t drive = 2048;      // gain into the filter, x0.25 to x4
	int32_t clockTune = 0;     // top half: cutoff follows the clock

	// Divide page
	int32_t pan[numTaps] = {2048, 2048, 2048, 2048, 2048, 2048};
	int32_t logicSpread = 0;   // A and C to the left, B and D to the right

	// Ratios page
	int32_t ratio[numDividers] = {
		ListValue(binaryRatioIndex[0], numRatios), ListValue(binaryRatioIndex[1], numRatios),
		ListValue(binaryRatioIndex[2], numRatios), ListValue(binaryRatioIndex[3], numRatios),
		ListValue(binaryRatioIndex[4], numRatios)};
	int32_t countLength = 4095; // reset all dividers after 2 to 63 edges, or free run
	int32_t edges = 0;          // count rising edges, or both edges

	// Motion page (the tilt amounts are applied in main.cpp)
	int32_t glide = 0;
	int32_t volume = 3072;

	// Knobs, CV and switch, already combined
	int32_t pitch = 6 * 4096;   // Q12 octaves above 1Hz
	int32_t cutoff = 7 * 4096;  // Q12 octaves above 20Hz
	int32_t cutoffOffset = 0;   // Q12 octaves from the clock, with clockTune
	int32_t resonance = 1024;
	int filterType = FilterLP;
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
	bool cyclePulse = false;     // high for 5ms when the counter cycles
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
		for (int i = 0; i < numSources; i++)
		{
			levelS[i] = level[i] << 4;
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

		for (int i = 0; i < numDividers; i++)
		{
			divN[i] = ratios[Clip((p.ratio[i] * numRatios) >> 12, 0, numRatios - 1)];
		}
		for (int i = 0; i < numLogic; i++)
		{
			logicFunc[i] = Clip((p.logicFunc[i] * numLogicFuncs) >> 12, 0, numLogicFuncs - 1);
		}

		colourK = (int32_t)(Exp2Q16(-p.colour * 10) >> 1);       // Q15, 32768 to 32
		countN = 2 + ((p.countLength * 63) >> 12);               // 2 to 64
		freeRun = countN >= 64;
		bothEdges = p.edges >= 2048;
		driveGain = (int32_t)(Exp2Q16((p.drive - 2048) * 2) >> 4); // Q12, x0.25 to x4
		fbAlpha = (int32_t)Exp2Q16(-p.fbSlew * 14);              // Q16
		glideAlpha = (int32_t)Exp2Q16(-p.glide * 14);            // Q16
		noiseModAmt = (p.noiseMod * p.noiseMod) >> 12;
		filterModAmt = (p.filterMod * p.filterMod) >> 12;
		hissAmt = (p.hiss * p.hiss) >> 12;
		volumeAmt = (p.volume * p.volume) >> 12;
		filterType = Clip(p.filterType, 0, numFilterTypes - 1);

		// Resonance, per filter type
		int32_t res = Clip(p.resonance, 0, 4095);
		int32_t r = 4096 - res;
		kFixed = ((r * r) >> 7) + 1300;            // SVF damping, Q16: 2 down to ~0.02
		ladderRes = (res * 17200) >> 12;           // ladder feedback, Q12: 0 to 4.2
		combFb = (res * 32440) >> 12;              // comb feedback, Q15: 0 to 0.99
		combDampCoeff = 32768 - Clip(p.combDamp, 0, 4095) * 7; // Q15

		// Levels and pans of all ten sources. Pans use a balance law:
		// centre is full level in both channels.
		for (int i = 0; i < numTaps; i++)
		{
			level[i] = p.level[i];
			SetPan(i, p.pan[i]);
		}
		int32_t spread = Clip(p.logicSpread, 0, 4095) >> 1;
		for (int i = 0; i < numLogic; i++)
		{
			level[numTaps + i] = p.logicLevel[i];
			SetPan(numTaps + i, (i & 1) ? 2048 + spread : 2048 - spread);
		}
	}

	Outputs __not_in_flash_func(Process)(const Inputs &in)
	{
		Outputs out;

		// Smooth source levels (8mu faders move in steps of 32)
		for (int i = 0; i < numSources; i++)
		{
			levelS[i] += ((level[i] << 4) - levelS[i]) >> 6;
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

		// Sources: /1 is the noise bit itself, then the dividers, then the
		// logic combinations of adjacent divider pairs
		bool bits[numSources];
		bits[0] = srcBit;
		for (int i = 0; i < numDividers; i++)
		{
			// Low for the first half of each cycle, so power-of-two ratios
			// match the bits of a binary ripple counter
			bits[i + 1] = divCount[i] * 2 >= divN[i];
		}
		for (int i = 0; i < numLogic; i++)
		{
			bool a = bits[i + 1], b = bits[i + 2];
			bool v;
			switch (logicFunc[i])
			{
			case LogicAnd:
				v = a && b;
				break;
			case LogicOr:
				v = a || b;
				break;
			case LogicAndNot:
				v = a && !b;
				break;
			default:
				v = a != b;
				break;
			}
			bits[numTaps + i] = v;
		}

		int32_t mono = 0, sumL = 0, sumR = 0;
		for (int i = 0; i < numSources; i++)
		{
			int32_t l = levelS[i] >> 4;
			int32_t v = bits[i] ? l : -l;
			mono += v;
			sumL += (v * panL[i]) >> 12;
			sumR += (v * panR[i]) >> 12;
		}
		for (int i = 0; i < numTaps; i++)
		{
			out.tap[i] = bits[i];
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

		// Cutoff: absolute from knob X, or an interval from the clock pitch
		int32_t c = (target.clockTune >= 2048) ? p - log2Of20 + target.cutoffOffset : target.cutoff;
		c += ((mod * filterModAmt) >> 12) * 3;                        // +-3 octaves
		c += ((p - 8 * 4096) * target.keyTrack) >> 12;                // relative to 256Hz
		c += in.cutoffMod;
		c = Clip(c, 0, maxCutoff);

		int32_t yL, yR;
		int32_t g = tables.g[c >> cutoffTableShift];
		if (filterType == FilterLadder)
		{
			int32_t G = (int32_t)(((uint32_t)g << 15) / (uint32_t)(65536 + g));
			yL = ladder[0].Process(xL, G, ladderRes);
			yR = ladder[1].Process(xR, G, ladderRes);
		}
		else if (filterType == FilterComb)
		{
			// Period in samples = 48000 / (20Hz * 2^(c/4096))
			int64_t period = ((int64_t)(2400 << 8) * (int64_t)Exp2Q16(-c)) >> 16;
			int32_t pq = (int32_t)std::min<int64_t>(std::max<int64_t>(period, combMinPeriod), combMaxPeriod);
			yL = comb[0].Process(xL, pq, combFb, combDampCoeff) >> 1;
			yR = comb[1].Process(xR, pq, combFb, combDampCoeff) >> 1;
		}
		else
		{
			coeffs.Set(g, kFixed);
			yL = svf[0].Process(xL, coeffs, filterType);
			yR = svf[1].Process(xR, coeffs, filterType);
		}

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
		cycleCount = 0;
		for (int i = 0; i < numDividers; i++)
		{
			divCount[i] = 0;
		}
		colourState = 0;
	}

private:
	void SetPan(int i, int32_t pan)
	{
		pan = Clip(pan, 0, 4096);
		panL[i] = std::min<int32_t>(4096, 2 * (4096 - pan));
		panR[i] = std::min<int32_t>(4096, 2 * pan);
	}

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
		if (!edge)
		{
			return;
		}

		for (int i = 0; i < numDividers; i++)
		{
			if (++divCount[i] >= divN[i])
			{
				divCount[i] = 0;
			}
		}

		// The cycle counter drives the cycle outputs, and unless free running,
		// resets every divider together
		if (++cycleCount >= countN)
		{
			cycleCount = 0;
			pulseTimer = 240;
			randomCV = (int32_t)(reg << (32 - len)) >> 20;
			if (!freeRun)
			{
				for (int i = 0; i < numDividers; i++)
				{
					divCount[i] = 0;
				}
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
	int32_t divN[numDividers] = {2, 4, 8, 16, 32};
	int logicFunc[numLogic] = {};
	int32_t colourK = 32768, countN = 64;
	bool freeRun = true;
	bool bothEdges = false;
	int32_t driveGain = 4096, fbAlpha = 65536, glideAlpha = 65536;
	int32_t noiseModAmt = 0, filterModAmt = 0, hissAmt = 0, volumeAmt = 0;
	int filterType = FilterLP;
	int32_t kFixed = 131072, ladderRes = 0, combFb = 0, combDampCoeff = 32768;
	int32_t level[numSources] = {};
	int32_t panL[numSources] = {}, panR[numSources] = {};

	// State
	uint32_t phase = 0;
	uint32_t reg = 0xACE1;
	int32_t colourState = 0;
	bool srcBit = false;
	int32_t divCount[numDividers] = {};
	int32_t cycleCount = 0;
	int32_t pulseTimer = 0;
	int32_t randomCV = 0;
	int32_t levelS[numSources] = {};
	int64_t pitchS = 0, fbS = 0;
	uint32_t lcg = 1;
	int32_t pink0 = 0, pink1 = 0, pink2 = 0, brown = 0;
	int32_t dcL = 0, dcR = 0;
	SVFCoeffs coeffs;
	SVF svf[2];
	Ladder ladder[2];
	Comb comb[2];
};

} // namespace ripple
