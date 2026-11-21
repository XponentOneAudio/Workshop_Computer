// Mutagen: the oscillator, its two mutants, the LFOs and the modulation,
// with no hardware in it, so it can be tested on a PC (test/engine_test.cpp).
// main.cpp connects it to the Computer's knobs, jacks and USB; ui.h is the
// 8mu's pages.  The web editor (web/index.html) has a copy of this in
// JavaScript, for its picture of the wave and its preview in the browser.
//
// The sound
//   One oscillator.  Its wave (sine, triangle, saw, square, and every blend
//   between neighbours) goes through two mutants in series, each one of six
//   kinds that bend the wave's timing - its phase - rather than filter it:
//
//     PULSE    the first half of the wave is squeezed into a width w and the
//              second half stretched over the rest: pulse width for any wave
//     SQUEEZE  the wave starts slowly and speeds up through the cycle
//     MIRROR   the wave plays forwards up to a turning point, then back
//     WARP     the phase is pushed by a curve drawn with eight faders
//     PHASEDIFF  the wave minus a copy shifted along: two saws make a pulse,
//              and Depth sets its width
//     SYNC     a second, faster copy of the wave restarts with the first:
//              hard sync, Depth sweeps its speed from 1 to 16 times
//
//   Each mutant has a RATIO: how many times its process happens in one
//   cycle (0.25 to 16; whole numbers keep the pitch, anything else adds
//   sub-harmonics and sync-like edges), a DEPTH, a WINDOW (a raised-cosine
//   fade of the mutated wave at each restart, which softens sync into
//   formants), FEEDBACK (its own output pushes its phase) and a dry/wet MIX.
//
// How it's made
//   Everything is integer arithmetic: the RP2040 has no floating point.
//   The oscillator runs at twice the sample rate (96kHz) and a 39-tap
//   half-band filter brings it down to 48kHz.  The waves come from
//   band-limited tables, nine per wave, each with half the harmonics of the
//   last.  Every mutant reports how fast it is running through the wave at
//   each moment (PULSE at width 5% runs through the first half ten times as
//   fast), and the table is picked for the pitch times that speed, so the
//   mutated wave has no harmonics above about 56kHz before the filter (it
//   removes 29-67kHz).  The jumps a mutant makes (sync's restart, a ratio
//   that isn't whole) are not band-limited, so those still alias a little,
//   as on most digital synths; WINDOW smooths them away.
//
// Phases
//   A "long phase" spans four cycles of the oscillator, 16 bits a cycle
//   (18 bits in all), so a ratio below 1 can stretch a mutant's process over
//   up to four cycles.  The oscillator's own phase is 32 bits for those four
//   cycles, for fine pitch.

#ifndef MUTAGEN_H
#define MUTAGEN_H

#include <stdint.h>
#include <math.h>

namespace mutagen
{

//----------------------------------------------------------------------------
// Pages and their faders.  Every value is 0-4095 (the 8mu's 0-127 faders are
// scaled up; the knobs and the web editor have the full range).
//----------------------------------------------------------------------------

static constexpr int kFaders = 8;
static constexpr int kPages = 8;
static constexpr int kButtons = 4;
static constexpr int kLayers = 2;

// Page numbers: button A-D is page 0-3, pressed again 4-7
enum Page {PageMut1, PageMut2, PageOsc, PageLfo1, PageWarp1, PageWarp2, PageMod, PageLfo2};
inline int ButtonOf(int pg) {return pg & 3;}
inline int LayerOf(int pg) {return pg >> 2;}

// The faders of MUTANT 1 and 2
enum MutParam {MMode, MRatio, MDepth, MWindow, MFeedback, MMix, MLfo1, MLfo2};
// OSC
enum OscParam {OWave, OSemi, OFine, OBits, ODrive, OOut2, OGlide, OLevel};
// LFO 1 and 2
enum LfoParam {LRate, LShape, LRatio1, LRatio2, LWave, LPitch, LMix1, LMix2};
// MOD: what CV In 2, Audio In 2 and the 8mu's tilt do
enum ModParam {CvDepth1, CvDepth2, CvRatio1, CvRatio2, AudioDepth1, AudioDepth2, TiltDepth1, TiltDepth2};

enum Mode {Pulse, Squeeze, Mirror, Warp, PhaseDiff, Sync, kModes};
enum Shape {LSine, LTriangle, LRampUp, LRampDown, LSquare, LSampleHold, LDrift, kShapes};
enum Out2Mode {Out2Same, Out2Dry, Out2Mutant1, Out2Sub1, Out2Sub2, kOut2Modes};
static constexpr int kSemiZones = 49;   // -24 ... +24 semitones

// Which faders are bipolar (centre is zero), and which choose from a list
inline bool IsBipolar(int pg, int f)
{
	switch (pg)
	{
	case PageMut1: case PageMut2: return f == MLfo1 || f == MLfo2;
	case PageOsc: return f == OSemi || f == OFine;
	case PageLfo1: case PageLfo2: return f >= LRatio1;
	default: return true;   // WARP and MOD pages
	}
}
// The number of choices of a fader that picks from a list, or 0
inline int ZonesOf(int pg, int f)
{
	if ((pg == PageMut1 || pg == PageMut2) && f == MMode) return kModes;
	if (pg == PageOsc && f == OOut2) return kOut2Modes;
	if (pg == PageOsc && f == OSemi) return kSemiZones;
	if ((pg == PageLfo1 || pg == PageLfo2) && f == LShape) return kShapes;
	return 0;
}

inline int ZoneOf(int32_t v, int n)
{
	int z = int((v * n) >> 12);
	return z < 0 ? 0 : (z >= n ? n - 1 : z);
}
inline int32_t ValueForZone(int z, int n) {return int32_t(((2 * z + 1) * 4096) / (2 * n));}

// A bipolar value as -32767 ... +32767, with a small dead zone round the
// centre so that a fader left in the middle really is zero
static constexpr int32_t kCentreZone = 48;
inline int32_t Bipolar(int32_t v)
{
	int32_t d = v - 2048;
	if (d > -kCentreZone && d < kCentreZone) return 0;
	// (d - 48) * 32767 / 1999 and (d + 48) * 32767 / 2000, as multiplies
	int32_t r = d > 0 ? ((d - kCentreZone) * 67142) >> 12
	                  : -((((-d) - kCentreZone) * 67108) >> 12);
	return r > 32767 ? 32767 : (r < -32767 ? -32767 : r);
}
// The value that gives a bipolar amount (-1 ... +1), for defaults
inline int32_t BipolarValue(int32_t percent)
{
	if (percent == 0) return 2048;
	int32_t span = percent > 0 ? 2047 - kCentreZone : 2048 - kCentreZone;
	int32_t c = percent > 0 ? kCentreZone : -kCentreZone;
	return 2048 + c + (percent * span) / 100;
}

//----------------------------------------------------------------------------
// Fixed-point helpers
//----------------------------------------------------------------------------

static constexpr uint32_t kPhaseMask = (1u << 18) - 1;   // a long phase

// 2^x for x 0 ... 1 (Q16), as Q16 (65536 ... 131072)
struct Exp2Table
{
	uint32_t t[257];
	Exp2Table()
	{
		for (int i = 0; i <= 256; i++) t[i] = uint32_t(lroundf(65536.0f * exp2f(float(i) / 256.0f)));
	}
};
inline const Exp2Table gExp2;
inline uint32_t Exp2Frac(uint32_t frac16)
{
	const uint32_t *t = gExp2.t;
	uint32_t i = (frac16 >> 8) & 255, f = frac16 & 255;
	return t[i] + (((t[i + 1] - t[i]) * f) >> 8);
}

// log2(x) in 1/256 octaves, with a straight line between octaves (at most
// about 0.09 octaves out).  Used only to choose a table, so that's plenty.
inline int32_t Log2Q8(uint32_t x)
{
	if (x == 0) return 0;
	int n = 31 - __builtin_clz(x);
	uint32_t m = n >= 8 ? (x >> (n - 8)) : (x << (8 - n));
	return (n << 8) + int32_t(m & 255);
}

//----------------------------------------------------------------------------
// The wave tables
//----------------------------------------------------------------------------

static constexpr int kTableBits = 10;
static constexpr int kTableSize = 1 << kTableBits;
static constexpr int kLevels = 9;        // level L has 256 >> L harmonics
enum Wave {WSine, WTriangle, WSaw, WSquare, kWaves};

struct Tables
{
	int16_t sine[kTableSize + 1];
	int16_t wave[kWaves - 1][kLevels][kTableSize + 1];

	const int16_t *Get(int w, int level) const {return w == WSine ? sine : wave[w - 1][level];}

	// Built at power-up, by adding up harmonics in integers (fast enough
	// without floating point: about a tenth of a second)
	void Build()
	{
		for (int i = 0; i <= kTableSize; i++)
			sine[i] = int16_t(lroundf(32767.0f * sinf(6.28318531f * float(i) / float(kTableSize))));
		static int32_t acc[kTableSize];
		for (int w = 1; w < kWaves; w++)
		{
			// Twice: the first time to find the loudest peak over all the
			// levels, so they can all share one scale and not jump in level
			// from one table to the next
			int32_t peak = 1;
			for (int pass = 0; pass < 2; pass++)
			{
				for (int L = 0; L < kLevels; L++)
				{
					Synth(w, 256 >> L, acc);
					for (int i = 0; i < kTableSize; i++)
					{
						int32_t a = acc[i] < 0 ? -acc[i] : acc[i];
						if (pass == 0 && a > peak) peak = a;
						if (pass == 1)
						{
							int32_t v = int32_t((int64_t(acc[i]) * 32000) / peak);
							wave[w - 1][L][i] = int16_t(v);
						}
					}
					if (pass == 1) wave[w - 1][L][kTableSize] = wave[w - 1][L][0];
				}
			}
		}
	}

	// Harmonics 1 ... n of a wave, in Q14 times Q15
	void Synth(int w, int n, int32_t *out)
	{
		int32_t amp[257] = {};
		for (int h = 1; h <= n; h++)
		{
			float a = 0.0f;
			if (w == WTriangle && (h & 1))
				a = (((h >> 1) & 1) ? -1.0f : 1.0f) * 0.81057f / float(h * h);  // 8/pi^2
			else if (w == WSaw)
				// a ramp from 0 rising to +1 at mid-cycle, a jump to -1, and a
				// ramp back up to 0: the jump at mid-cycle, where the square's
				// falling edge is, so PULSE moves both alike
				a = ((h & 1) ? 0.63662f : -0.63662f) / float(h);              // 2/pi
			else if (w == WSquare && (h & 1))
				a = 1.27324f / float(h);                                       // 4/pi
			amp[h] = int32_t(lroundf(a * 16384.0f));
		}
		for (int i = 0; i < kTableSize; i++)
		{
			int32_t s = 0;
			for (int h = 1; h <= n; h++)
				if (amp[h]) s += amp[h] * sine[(h * i) & (kTableSize - 1)];
			out[i] = s >> 14;
		}
	}
};

// Interpolated lookup: p is a phase, 16 bits a cycle (higher bits ignored)
inline int32_t Lookup(const int16_t *t, uint32_t p)
{
	p &= 0xFFFF;
	uint32_t i = p >> 6;
	int32_t f = int32_t(p & 63);
	int32_t a = t[i];
	return a + (((t[i + 1] - a) * f) >> 6);
}

//----------------------------------------------------------------------------
// The half-band filter, 96kHz to 48kHz: 23 taps, flat to 18kHz (-0.4dB at
// 20kHz), down 50dB from 29.5kHz.  Every other tap is zero, and the rest are
// symmetric, so it costs six multiplies.  (Above 29kHz the waves' harmonics
// are already weak, so 50dB puts what folds back well under the 12-bit
// output's noise.)
//----------------------------------------------------------------------------

struct HalfBand
{
	static constexpr int kTaps = 6;
	static constexpr int32_t kCoef[kTaps] = {10302, -3106, 1515, -777, 371, -161};
	// The last 12 of the second sample of each pair, and the last 6 of the
	// first, each written twice so that the newest is always at [n] and the
	// older ones follow it without wrapping round
	int32_t b[24] = {};
	int32_t a[12] = {};
	int n = 0, m = 0;

	// Takes the pair of 96kHz samples for this 48kHz one
	int32_t Process(int32_t x0, int32_t x1)
	{
		n = n == 0 ? 11 : n - 1;
		m = m == 0 ? 5 : m - 1;
		b[n] = b[n + 12] = x1;
		a[m] = a[m + 6] = x0;
		const int32_t *p = b + n;
		// In Q15 (the centre tap is a half), which leaves room for the
		// largest sum of full-scale samples
		int32_t y = a[m + 5] * 16384
			+ kCoef[0] * (p[5] + p[6]) + kCoef[1] * (p[4] + p[7])
			+ kCoef[2] * (p[3] + p[8]) + kCoef[3] * (p[2] + p[9])
			+ kCoef[4] * (p[1] + p[10]) + kCoef[5] * (p[0] + p[11]);
		return y >> 15;
	}
};

//----------------------------------------------------------------------------
// Ratio: 0.25 to 16, exponential, settling on whole numbers and a few simple
// fractions when near them
//----------------------------------------------------------------------------

static constexpr int32_t kRatioOctaves = 6;           // 0.25 ... 16
// Where the ratio settles, as octaves above 0.25 (Q16)
inline int32_t RatioLogFromValue(int32_t v)
{
	int32_t lg = (v * 393313) >> 12;          // v * 6 octaves / 4095
	// 1/4, 1/3, 1/2, 2/3, 3/4, 1, 1.5, 2, 3, 4 ... 16, as octaves above 1/4
	static constexpr int32_t kDetentLog[] = {0, 27200, 65536, 92736, 103872, 131072, 169408, 196608, 234944,
		262144, 283242, 300480, 315055, 327680, 338816, 348778, 357789, 366016, 373584, 380591, 387114, 393216};
	// Within 0.025 octaves of one: that one
	for (int32_t d0 : kDetentLog)
	{
		int32_t d = lg - d0;
		if (d > -1638 && d < 1638) return d0;
	}
	return lg;
}
// The ratio itself, Q8 (64 ... 4096), from octaves above 0.25 (Q16)
inline uint32_t RatioFromLog(int32_t lg)
{
	if (lg < 0) lg = 0;
	if (lg > (kRatioOctaves << 16)) lg = kRatioOctaves << 16;
	uint32_t e = Exp2Frac(uint32_t(lg) & 0xFFFF);
	return ((e << (lg >> 16)) + 512) >> 10;
}
// The fader value for a ratio, for defaults
inline int32_t ValueForRatio(float r)
{
	return int32_t(lroundf(log2f(r / 0.25f) / float(kRatioOctaves) * 4095.0f));
}

// Pitch: octaves above C0 (16.35Hz), Q16
static constexpr int32_t kOct = 65536;
static constexpr int32_t kMaxPitch = int32_t(10.2 * 65536);       // about 19kHz
// The oscillator's phase increment for a pitch, per 96kHz sample (2^32 is
// four cycles)
inline uint32_t IncFromPitch(int32_t pitch)
{
	if (pitch < 0) pitch = 0;
	if (pitch > kMaxPitch) pitch = kMaxPitch;
	// C0 at 96kHz: 16.3516 * 2^30 / 96000
	static constexpr uint32_t kC0 = 182889;
	uint32_t inc = (kC0 * (Exp2Frac(uint32_t(pitch) & 0xFFFF) >> 3)) >> 13;
	return inc << (pitch >> 16);
}

//----------------------------------------------------------------------------
// LFOs, run at control rate
//----------------------------------------------------------------------------

struct Lfo
{
	uint32_t phase = 0, inc = 0;
	int shape = LTriangle;
	int32_t out = 0;           // -32767 ... 32767
	int32_t held = 0, from = 0, to = 0;
	uint32_t rng = 0x12345678;

	int32_t Random()
	{
		rng = rng * 1664525u + 1013904223u;
		return int32_t(rng >> 16) - 32768;
	}

	void Reset() {phase = 0;}

	// Tempo sync: put the phase where the beat says it should be.  Moving
	// it back past the start of a cycle counts as a new cycle (a new random
	// value for S&H and drift).
	void Align(uint32_t to)
	{
		if (to < phase && phase - to > 0x80000000u)
		{
			held = Random();
			from = this->to;
			this->to = Random();
		}
		phase = to;
	}

	void Step(const int16_t *sine)
	{
		uint32_t before = phase;
		phase += inc;
		bool wrapped = phase < before;
		if (wrapped)
		{
			held = Random();
			from = this->to;
			to = Random();
		}
		uint32_t p16 = phase >> 16;
		int32_t v;
		switch (shape)
		{
		case LSine: v = Lookup(sine, p16); break;
		case LTriangle:
			// up from -1 to +1 over the first half, back down over the second
			v = p16 < 32768 ? int32_t(p16 * 2) - 32767 : 32767 - int32_t((p16 - 32768) * 2);
			break;
		case LRampUp: v = int32_t(p16) - 32768; break;
		case LRampDown: v = 32767 - int32_t(p16); break;
		case LSquare: v = p16 < 32768 ? 32767 : -32767; break;
		case LSampleHold: v = held; break;
		default:
			// Drift: from one random value to the next, eased
			{
				int32_t e = (32768 - Lookup(sine, (p16 >> 1) + 16384)) >> 1;  // 0 ... 32767, a half cosine
				v = from + (((to - from) * e) >> 15);
			}
			break;
		}
		out = v > 32767 ? 32767 : (v < -32767 ? -32767 : v);
	}
};
// The RATE fader: its first third is tempo sync, to the clock at Pulse In 1
// (or 120 BPM without one), twelve choices of how many beats a cycle lasts,
// slowest first; the rest is free, 0.02Hz to 40Hz
static constexpr int32_t kSyncTop = 1365;
static constexpr int kSyncs = 12;
// Beats a cycle, as n / d
static constexpr uint8_t kSyncN[kSyncs] = {16, 8, 4, 3, 2, 3, 1, 2, 1, 1, 1, 1};
static constexpr uint8_t kSyncD[kSyncs] = {1, 1, 1, 1, 1, 2, 1, 3, 2, 3, 4, 8};
// The choice for a RATE value, or -1 for a free rate
inline int LfoSyncOf(int32_t v) {return v < kSyncTop ? (v * kSyncs) / kSyncTop : -1;}
inline int32_t ValueForSync(int z) {return ((2 * z + 1) * kSyncTop) / (2 * kSyncs);}
// A free rate, for a control rate of 3kHz
inline uint32_t LfoIncFromValue(int32_t v)
{
	if (v < kSyncTop) v = kSyncTop;
	// 0.02Hz * 2^32 / 3000
	static constexpr uint32_t kSlowest = 28634;
	int32_t lg = int32_t((uint32_t(v - kSyncTop) * 1078272u) >> 12);   // 10.966 octaves over the rest
	return ((kSlowest * (Exp2Frac(uint32_t(lg) & 0xFFFF) >> 1)) >> 15) << (lg >> 16);
}
// The RATE value for a free rate in Hz, for defaults
inline int32_t ValueForLfoHz(float hz)
{
	return kSyncTop + int32_t(lroundf(log2f(hz / 0.02f) / 10.966f * float(4095 - kSyncTop)));
}

//----------------------------------------------------------------------------
// One mutant
//----------------------------------------------------------------------------

struct Stage
{
	// Set at control rate
	int mode = Pulse;
	int32_t window = 0;           // Q15
	int32_t fb = 0;               // Q8, 0 ... 384 (150%)
	int32_t points[kFaders + 1] = {};  // WARP's curve, Q15, the last the first again

	// Targets from control rate, smoothed each sample
	int32_t depthT = 0, mixT = 32768, ratioT = 0;
	int32_t depthS = 0, mixS = 32768 << 6, ratioS = 0;   // all << 6
	int32_t audioDepth = 0;        // Audio In 2's amount, Q15

	// Each sample
	uint32_t r = 256, invR = 4096;
	uint32_t qi = 1, qf = 0;      // SYNC: the copy's speed over r, whole and Q16 parts
	int32_t ca = 0, cb = 32768;
	int32_t depth = 0;
	int32_t w = 32768, u1 = 40960, u2 = 57344;
	uint32_t rc1 = 0, rc2 = 0;
	int32_t slope1 = 256, slope2 = 256;
	int32_t lg1 = 0, lg2 = 0;      // the slopes as 1/256 octaves faster than the wave itself
	const int16_t *sine = nullptr;
	int32_t sqJ = 0, sqF = 0, sqMax = 256;
	int32_t phi = 32768;
	int32_t ks = 256;
	bool active = true;

	// Feedback: this mutant's last output
	int32_t prev = 0;

	// Effective values, for the LEDs and the web editor
	int32_t ShownDepth() const {return depth;}
	int32_t ShownMix() const {return mixS >> 6;}
	int32_t ShownRatioLog() const {return ratioS >> 6;}

	// A one-pole lag of about 1.3ms that arrives exactly, run every sample
	// (shift 6) or every fourth (shift 4)
	static void Smooth(int32_t &s, int32_t target, int shift)
	{
		int32_t d = (target << 6) - s;
		if (d > -64 && d < 64) s = target << 6;
		else s += d >> shift;
	}

	// The per-sample part: smoothing and everything that needs dividing.
	// audio is Audio In 2, Q15; shift as Smooth.
	void Prepare(int32_t audio, int shift = 6)
	{
		Smooth(depthS, depthT, shift);
		Smooth(mixS, mixT, shift);
		Smooth(ratioS, ratioT, shift);
		int32_t d = (depthS >> 6) + ((audio * audioDepth) >> 15);
		depth = d < 0 ? 0 : (d > 32767 ? 32767 : d);
		int32_t mix = mixS >> 6;
		if (mix < 0) mix = 0;
		if (mix > 32768) mix = 32768;
		active = mix > 0;

		r = RatioFromLog(ratioS >> 6);
		invR = (1u << 20) / r;

		if (mode == PhaseDiff)
		{
			ca = 32768 - (mix >> 1);
			cb = -(mix >> 1);
		}
		else
		{
			ca = 32768 - mix;
			cb = mix;
		}

		slope1 = slope2 = 256;
		switch (mode)
		{
		case Pulse:
		{
			// The first half of the wave (the square's high half) takes w
			// of the cycle, 50% to 1.5%, so it plays fast (vH = 0.5 / w)
			// and the rest slowly.  The change of speed is not where the
			// halves meet - the wave's edges are there, and bending an edge
			// can't be band-limited - but a quarter of the way into the
			// slow half, and a quarter from its end: on a square, where the
			// wave is flat.  So the speeds are
			//   wave 0 ... 5/8     at vH, from the start to u1 = 5/4 w
			//   wave 5/8 ... 7/8   slower, to u2 = 1 - w/4
			//   wave 7/8 ... 1     at vH again
			// and the whole wave is played from the table for vH, so the
			// change of table doesn't fall on an edge either.
			w = 32768 - ((depth * 31785) >> 15);
			u1 = (w * 5) >> 2;
			u2 = 65536 - (w >> 2);
			rc1 = (1u << 29) / uint32_t(w);                 // vH, Q14
			rc2 = (16384u << 14) / uint32_t(u2 - u1);       // the slow speed, Q14
			slope1 = slope2 = int32_t(rc1 >> 6);
			break;
		}
		case Mirror:
			// Forwards up to w, then back: w from the whole cycle (depth 0,
			// no change) to 2% (a quick run up, a slow one back).  The wave
			// never jumps, so each part can have its own table.
			w = 65535 - ((depth * 64223) >> 15);
			rc1 = (1u << 30) / uint32_t(w);
			rc2 = (1u << 30) / uint32_t(65536 - w);
			slope1 = int32_t(rc1 >> 6);
			slope2 = int32_t(rc2 >> 6);
			if (slope2 > 65535) slope2 = 65535;
			break;
		case Squeeze:
		{
			// From the phase itself to its 8th power, in three blends
			int32_t t = depth * 3;
			sqJ = t >> 15;
			if (sqJ > 2) sqJ = 2;
			sqF = t - (sqJ << 15);
			int32_t n0 = 1 << sqJ;
			sqMax = (n0 * 256 * (32768 - sqF) + n0 * 512 * sqF) >> 15;
			slope1 = slope2 = sqMax;
			break;
		}
		case Warp:
			// The fastest of the eight segments (as PULSE, for all of them)
			for (int j = 0; j < kFaders; j++)
			{
				int32_t sl = 256 + ((((points[j + 1] - points[j]) * depth) >> 15) >> 5);
				if (sl < 0) sl = -sl;
				if (sl > slope1) slope1 = sl;
			}
			slope2 = slope1;
			break;
		case PhaseDiff:
			// The copy half a cycle along (depth 0) to 1.5% along, running
			// r times as fast
			phi = 32768 - ((depth * 31785) >> 15);
			slope1 = slope2 = int32_t(r);
			break;
		default: // Sync
		{
			// The second copy runs 1 to 16 times as fast
			uint32_t ks16 = Exp2Frac(uint32_t(depth * 8) & 0xFFFF) << ((depth * 8) >> 16);   // Q16, 1 ... 16
			ks = int32_t(ks16 >> 8);
			uint32_t q = (ks16 << 8) / r;                                               // Q16, up to 64
			qi = q >> 16;
			qf = q & 0xFFFF;
			slope1 = slope2 = ks;
			break;
		}
		}
		lg1 = Log2Q8(uint32_t(slope1)) - 2048;
		lg2 = mode == Mirror ? Log2Q8(uint32_t(slope2)) - 2048 : lg1;
	}

	// The mutated phase for long phase S, with how much faster than the wave
	// it runs (lg, in 1/256 octaves) and WINDOW's gain (Q15).
	//
	// The process runs r times a cycle: x is S times r, and u how far
	// through this run of it.  The mutants that bend the phase map u to w2,
	// and the result is S moved by (w2 - u) / r: exact wherever the mutant
	// leaves the phase alone (at the start and end of every run), so
	// whole-number ratios repeat exactly every cycle.
	inline uint32_t Map(uint32_t S, int32_t &lg, int32_t &gain) const
	{
		uint32_t x = (S * r) >> 8;
		uint32_t u = x & 0xFFFF;
		int32_t b;
		lg = lg1;
		switch (mode)
		{
		case Pulse:
		{
			uint32_t w2;
			if (int32_t(u) < u1) w2 = (u * rc1) >> 14;
			else if (int32_t(u) < u2) w2 = 40960 + (((u - uint32_t(u1)) * rc2) >> 14);
			else w2 = 65536 - (((65536 - u) * rc1) >> 14);
			b = int32_t(S) + ((int32_t(w2 - u) * int32_t(invR)) >> 12);
			break;
		}
		case Squeeze:
		{
			uint32_t p1 = (u * u) >> 16, p2 = (p1 * p1) >> 16;
			uint32_t lo, hi;
			if (sqJ == 0) {lo = u; hi = p1;}
			else if (sqJ == 1) {lo = p1; hi = p2;}
			else {lo = p2; hi = (p2 * p2) >> 16;}
			uint32_t w2 = lo - (((lo - hi) * uint32_t(sqF)) >> 15);
			b = int32_t(S) + ((int32_t(w2 - u) * int32_t(invR)) >> 12);
			break;
		}
		case Mirror:
		{
			uint32_t w2;
			if (int32_t(u) < w) w2 = (u * rc1) >> 14;
			else {w2 = ((65536 - u) * rc2) >> 14; lg = lg2;}
			b = int32_t(S) + ((int32_t(w2 - u) * int32_t(invR)) >> 12);
			break;
		}
		case Warp:
		{
			int j = int(u >> 13);
			int32_t fr = int32_t(u & 8191);
			int32_t d = points[j + 1] - points[j];
			int32_t c = points[j] + ((d * fr) >> 13);
			int32_t off = (c * depth) >> 15;                 // up to half a cycle
			b = int32_t(S) + ((off * int32_t(invR)) >> 12);
			break;
		}
		case PhaseDiff:
			b = int32_t(x) + phi;
			break;
		default: // Sync
			b = int32_t(u * qi + ((u * qf) >> 16));
			break;
		}
		if (fb) b += (prev * fb) >> 8;
		if (window > 0)
		{
			// A raised cosine over each restart: 0 at the restart itself
			// (the nearest table entry is plenty for a gain)
			int32_t c = sine[((u >> 6) + 256) & (kTableSize - 1)];
			int32_t hann = (32767 - c) >> 1;
			gain = 32768 - ((window * (32768 - hann)) >> 15);
		}
		else gain = 32768;
		return uint32_t(b) & kPhaseMask;
	}
};

//----------------------------------------------------------------------------
// The engine
//----------------------------------------------------------------------------

// What the jacks and the 8mu bring, each sample or control tick
struct Inputs
{
	int32_t cv1 = 0, cv2 = 0;          // -2048 ... 2047
	int32_t audio1 = 0, audio2 = 0;    // -2048 ... 2047
	bool syncPatched = false;          // a jack in Audio In 1
	bool clock = false;                // Pulse In 1, high or low
	bool lfoReset = false;             // Pulse In 2 rising edge
};

class Engine
{
public:
	static constexpr int kControlEvery = 16;   // samples: a 3kHz control rate

	// The faders, 0-4095
	volatile uint16_t values[kPages][kFaders];
	// The Main knob's pitch, 0-4095: C1 to C7
	int32_t pitchKnob = 2048;
	// The 8mu's tilt, -2032 ... 2032 (0 without one)
	int32_t tiltRoll = 0, tiltPitch = 0;

	// Outputs, each sample
	int16_t out1 = 0, out2 = 0;        // -2048 ... 2047
	int16_t cv1 = 0, cv2 = 0;          // LFO 1, 2 as -2048 ... 2047
	bool pulse1 = false, pulse2 = false;

	// For the LEDs and the web editor
	int32_t pitch = 0;                 // octaves above C0, Q16, as playing
	int32_t wavePos = 0;               // 0 ... 4095 after modulation
	Stage stage[2];
	Lfo lfo[2];
	bool syncing = false;              // following Audio In 1
	int32_t clockFlash = 0;            // samples left of showing a clock
	bool clocked = false;              // following a clock at Pulse In 1
	uint32_t clockPeriod = 24000;      // samples a beat: the clock's, or 120 BPM
	int32_t tiltQ[2] = {};             // tilt after the dead zone, Q15

	Tables *tables = nullptr;

	// Lean: Out 1 made once a sample instead of twice.  The card sets this
	// if a patch takes too long to make (both mutants busy, everything on),
	// so the processor keeps up; a little rougher at high pitch, otherwise
	// the same.
	bool lean = false;

	Engine()
	{
		DefaultValues();
	}

	void Begin(Tables *t)
	{
		tables = t;
		stage[0].sine = stage[1].sine = t->sine;
		Control(Inputs());
		for (int s = 0; s < 2; s++)
		{
			stage[s].depthS = stage[s].depthT << 6;
			stage[s].mixS = stage[s].mixT << 6;
			stage[s].ratioS = stage[s].ratioT << 6;
		}
		pitchS = pitchT << 8;
	}

	void DefaultValues()
	{
		static const int16_t kDefaults[kPages][kFaders] = {
			// MUTANT 1: PULSE, x1, no depth (the X knob), no window or
			// feedback, all wet, LFO 1 sweeping the width a little
			{int16_t(ValueForZone(Pulse, kModes)), int16_t(ValueForRatio(1.0f)), 0, 0, 0, 4095, int16_t(BipolarValue(35)), 2048},
			// MUTANT 2: SYNC, x1, no depth (the Y knob)
			{int16_t(ValueForZone(Sync, kModes)), int16_t(ValueForRatio(1.0f)), 0, 0, 0, 4095, 2048, 2048},
			// OSC: saw, no transposition, no crush or drive, Out 2 dry, no
			// glide, 80% level
			{2730, int16_t(ValueForZone(24, kSemiZones)), 2048, 0, 0, int16_t(ValueForZone(Out2Dry, kOut2Modes)), 0, 3276},
			// LFO 1: 0.5Hz triangle, nothing else
			{int16_t(ValueForLfoHz(0.5f)), int16_t(ValueForZone(LTriangle, kShapes)), 2048, 2048, 2048, 2048, 2048, 2048},
			// WARP 1, 2: a sine's worth of push
			{2048, 3496, 4095, 3496, 2048, 600, 0, 600},
			{2048, 4095, 2048, 0, 2048, 4095, 2048, 0},
			// MOD: CV In 2 sweeps mutant 1's depth, nothing else
			{4095, 2048, 2048, 2048, 2048, 2048, 2048, 2048},
			// LFO 2: 0.15Hz sine
			{int16_t(ValueForLfoHz(0.15f)), int16_t(ValueForZone(LSine, kShapes)), 2048, 2048, 2048, 2048, 2048, 2048},
		};
		for (int p = 0; p < kPages; p++)
			for (int f = 0; f < kFaders; f++) values[p][f] = uint16_t(kDefaults[p][f]);
	}

	int32_t Value(int pg, int f) const {return values[pg][f];}

	//------------------------------------------------------------------------
	// Control rate: the LFOs and every modulation target
	//------------------------------------------------------------------------
	// In four parts, one a sample, so no one sample has all of it to do:
	// 0 the LFOs, 1 and 2 the two mutants, 3 the wave, pitch and output
	void Control(const Inputs &in)
	{
		for (int k = 0; k < 4; k++) ControlPart(k, in);
	}

	void ControlPart(int part, const Inputs &in)
	{
		if (part == 0)
		{
			const int16_t *sine = tables ? tables->sine : nullptr;
			for (int l = 0; l < 2; l++)
			{
				const int pg = l ? PageLfo2 : PageLfo1;
				const int32_t rate = Value(pg, LRate);
				const int sync = LfoSyncOf(rate);
				if (sync < 0) lfo[l].inc = LfoIncFromValue(rate);
				else
				{
					// A beat of clockPeriod samples, n/d beats a cycle: 2^32
					// a cycle, a step every 16 samples
					const uint32_t n = kSyncN[sync], d = kSyncD[sync];
					if (sync != syncMade[l] || clockPeriod != periodMade[l])
					{
						syncMade[l] = sync;
						periodMade[l] = clockPeriod;
						lfo[l].inc = uint32_t(((uint64_t(1) << 36) * d) / (uint64_t(clockPeriod) * n));
					}
					// At each clock, where this beat falls in the cycle
					if (beatPending) lfo[l].Align(uint32_t(((uint64_t((beat * d) % n)) << 32) / n));
				}
				if (sync < 0) syncMade[l] = -1;
				lfo[l].shape = ZoneOf(Value(pg, LShape), kShapes);
				if (lfoResetPending) lfo[l].Reset();
				if (sine) lfo[l].Step(sine);
			}
			beatPending = false;
			lfoResetPending = false;
			tiltQ[0] = TiltQ(tiltRoll);
			tiltQ[1] = TiltQ(tiltPitch);
			pulse1 = lfo[0].out > 0;
			cv1 = int16_t(lfo[0].out / 19);   // +/- 5V
			cv2 = int16_t(lfo[1].out / 19);
			return;
		}
		const int32_t l1 = lfo[0].out, l2 = lfo[1].out;

		// CV In 2: +5V is +100%.  Audio In 2 likewise, at audio rate.
		int32_t cvq = in.cv2 * 19;
		cvq = cvq > 40000 ? 40000 : (cvq < -40000 ? -40000 : cvq);

		if (part <= 2)
		{
			const int s = part - 1;
			Stage &st = stage[s];
			const int pg = s ? PageMut2 : PageMut1;
			st.mode = ZoneOf(Value(pg, MMode), kModes);
			st.window = Value(pg, MWindow) * 8;
			st.fb = (Value(pg, MFeedback) * 384) / 4095;
			int32_t d = Value(pg, MDepth) * 8
				+ Mod(l1, Bipolar(Value(pg, MLfo1))) + Mod(l2, Bipolar(Value(pg, MLfo2)))
				+ Mod(cvq, Bipolar(Value(PageMod, s ? CvDepth2 : CvDepth1)))
				+ Mod(tiltQ[s], Bipolar(Value(PageMod, s ? TiltDepth2 : TiltDepth1)));
			st.depthT = d < 0 ? 0 : (d > 32767 ? 32767 : d);
			st.audioDepth = Bipolar(Value(PageMod, s ? AudioDepth2 : AudioDepth1));
			// Ratio: LFOs and CV In 2 add up to two octaves either way
			int32_t lg = RatioLogFromValue(Value(pg, MRatio))
				+ 4 * (Mod(l1, Bipolar(Value(PageLfo1, s ? LRatio2 : LRatio1)))
				     + Mod(l2, Bipolar(Value(PageLfo2, s ? LRatio2 : LRatio1)))
				     + Mod(cvq, Bipolar(Value(PageMod, s ? CvRatio2 : CvRatio1))));
			st.ratioT = lg < 0 ? 0 : (lg > (kRatioOctaves << 16) ? (kRatioOctaves << 16) : lg);
			int32_t mix = (Value(pg, MMix) * 32768 + 2047) / 4095
				+ Mod(l1, Bipolar(Value(PageLfo1, s ? LMix2 : LMix1)))
				+ Mod(l2, Bipolar(Value(PageLfo2, s ? LMix2 : LMix1)));
			st.mixT = mix < 0 ? 0 : (mix > 32768 ? 32768 : mix);
			const int pw = s ? PageWarp2 : PageWarp1;
			for (int f = 0; f < kFaders; f++) st.points[f] = Bipolar(Value(pw, f));
			st.points[kFaders] = st.points[0];
			return;
		}

		// The wave, 0-4095 across sine, triangle, saw and square
		int32_t wv = Value(PageOsc, OWave)
			+ ((Mod(l1, Bipolar(Value(PageLfo1, LWave))) + Mod(l2, Bipolar(Value(PageLfo2, LWave)))) >> 3);
		wavePos = wv < 0 ? 0 : (wv > 4095 ? 4095 : wv);
		int32_t t = wavePos * 3;
		waveA = t >> 12;
		if (waveA > 2) waveA = 2;
		waveF = (t - (waveA << 12)) << 3;
		// Close to one wave: that wave alone, which is half the work
		if (waveF < 256) waveF = 0;
		if (waveF > 32512 && waveA < 2) {waveA++; waveF = 0;}
		if (waveF > 32512) waveF = 32767;
		if (tables)
			for (int L = 0; L < kLevels; L++)
			{
				tabA[L] = tables->Get(waveA, L);
				tabB[L] = tables->Get(waveA < 3 ? waveA + 1 : waveA, L);
			}

		// Pitch: Main (C1 to C7), SEMI and FINE, with glide; CV In 1 and the
		// LFOs are added each sample
		int32_t semi = ZoneOf(Value(PageOsc, OSemi), kSemiZones) - 24;
		int32_t fine = Bipolar(Value(PageOsc, OFine));     // +/- 50 cents
		pitchT = kOct + ((pitchKnob * 393313) >> 12) + (semi * kOct) / 12 + (fine * (kOct / 24)) / 32767;
		// Vibrato: up to an octave, the fader's amount squared, for fine
		// settings near the centre
		int32_t a1 = Bipolar(Value(PageLfo1, LPitch)), a2 = Bipolar(Value(PageLfo2, LPitch));
		pitchMod = 2 * (Mod(l1, Mod(a1, a1 < 0 ? -a1 : a1)) + Mod(l2, Mod(a2, a2 < 0 ? -a2 : a2)));
		// Glide: 2ms to 2s.  As a one-pole coefficient at control rate.
		int32_t g = Value(PageOsc, OGlide);
		uint32_t gl = (Exp2Frac(uint32_t(g * 160) & 0xFFFF) << ((g * 160) >> 16)) >> 16;  // 1 ... 1024
		glideK = int32_t(65536 / (6 * gl));
		if (glideK < 1) glideK = 1;
		if (glideK > 65536) glideK = 65536;

		level = Value(PageOsc, OLevel) * 8;
		drive = Value(PageOsc, ODrive);
		int32_t bits = Value(PageOsc, OBits);
		crushShift = bits < 64 ? 0 : 4 + (bits * 11) / 4096;   // 12 bits down to 2
		out2Mode = ZoneOf(Value(PageOsc, OOut2), kOut2Modes);
	}

	//------------------------------------------------------------------------
	// Each sample
	//------------------------------------------------------------------------
	void Tick(const Inputs &in)
	{
		// The periodic work, each on its own sample of every 16:
		//   0, 4, 8, 12   the four parts of the control work
		//   1, 5, 9, 13   mutant 1's settings (every sample instead while
		//   3, 7, 11, 15  mutant 2's        Audio In 2 is moving a depth)
		//   2, 6, 10, 14  free, for the card's knobs, 8mu, LEDs and status
		if (in.lfoReset) lfoResetPending = true;
		controlCount = (controlCount + 1) & (kControlEvery - 1);
		if ((controlCount & 3) == 0) ControlPart(controlCount >> 2, in);
		if (controlCount == 12)
		{
			// (glideK is at most 10922, so this stays inside 32 bits for a
			// jump across the whole range)
			pitchS += ((((pitchT << 8) - pitchS) >> 10) * glideK) >> 6;
		}

		// CV In 1: about 341 a volt; smoothed a little
		cvPitch += ((in.cv1 * 192 * 16) - cvPitch) >> 3;
		int32_t p = (pitchS >> 8) + (cvPitch >> 4) + pitchMod;
		p = p < 0 ? 0 : (p > kMaxPitch ? kMaxPitch : p);
		if (p != pitch || inc == 0)
		{
			pitch = p;
			inc = IncFromPitch(pitch);
			lgBase = Log2Q8(inc) - kLgRef;
		}

		// The mutants' settings: every sample while Audio In 2 is moving a
		// depth, otherwise every fourth (12kHz: smooth enough, and a third
		// of the work)
		int32_t audio2 = in.audio2 * 16;
		bool audioRate = in.audio2 != 0 && (stage[0].audioDepth != 0 || stage[1].audioDepth != 0);
		if (audioRate)
		{
			stage[0].Prepare(audio2, 6);
			stage[1].Prepare(audio2, 6);
		}
		else if ((controlCount & 3) == 1) stage[0].Prepare(audio2, 4);
		else if ((controlCount & 3) == 3) stage[1].Prepare(audio2, 4);

		// Pulse In 1: the clock for the LFOs' tempo sync.  A rising edge
		// counts only after 2ms low, and at most every 4ms, so noise and slow
		// edges aren't taken for extra clocks.  Followed from its second
		// edge; with none for four beats (and at least 2s), back to 120 BPM.
		if (sinceClock < 0x7FFFFFFF) sinceClock++;
		if (!in.clock) {if (clockLow < 96) clockLow++;}
		else if (!clockWas && clockLow >= 96 && sinceClock >= 192)
		{
			if (clockSeen && sinceClock <= 48000u * 60)
			{
				clockPeriod = sinceClock;
				clocked = true;
			}
			clockSeen = true;
			sinceClock = 0;
			beat = (beat + 1) % 48;     // 48: every choice's cycle lines up again
			beatPending = true;
			clockFlash = 2400;
		}
		if (in.clock) clockLow = 0;
		clockWas = in.clock;
		if (clocked && sinceClock > (clockPeriod * 4 > 96000 ? clockPeriod * 4 : 96000))
		{
			clocked = false;
			clockSeen = false;
			clockPeriod = 24000;
		}
		if (in.lfoReset) beat = 0;
		if (clockFlash > 0) clockFlash--;

		// Audio In 1: hard sync from another oscillator, at each rising
		// zero crossing, placed between the samples where it happened
		syncing = in.syncPatched;
		bool synced = false;
		uint32_t syncAt = 0;
		if (syncing)
		{
			if (in.audio1 < -40) armed = true;
			if (armed && in.audio1 >= 0)
			{
				armed = false;
				int32_t span = in.audio1 - lastAudio1;
				// how far into the sample since the crossing, 0 ... 1 (Q8)
				int32_t past = span > 0 ? (in.audio1 * 256) / span : 0;
				if (past > 256) past = 256;
				synced = true;
				syncAt = (uint32_t(past) * (inc >> 7));   // two 96kHz samples' worth
			}
		}
		lastAudio1 = in.audio1;

		// Out 1 at 96kHz, filtered down.  Out 2 (when it isn't the same) at
		// 48kHz from tables with fewer harmonics, to save the time: it is
		// clean for the dry wave and the subs, a little rougher than Out 1
		// for mutant 1.
		uint32_t S0 = acc >> 14;
		int32_t y;
		if (!lean)
		{
			int32_t y0 = Eval2(S0, lgBase);
			acc += inc;
			int32_t y1 = Eval2(acc >> 14, lgBase);
			acc += inc;
			y = hb.Process(y0, y1);
		}
		else
		{
			// Lean (see the variable): once a sample, at 48kHz, from tables
			// with few enough harmonics for that
			y = Eval2(S0, lgBase - kLg48);
			acc += inc << 1;
		}
		int32_t z = out2Mode != Out2Same ? Out2(S0) : 0;
		if (synced) acc = syncAt;

		out1 = int16_t(Finish(y));
		out2 = out2Mode == Out2Same ? out1 : int16_t(Finish(z));

		pulse2 = (acc >> 29) & 1 ? false : true;   // the first half of each cycle
	}

	// Evaluate the chain: mutant 2 of mutant 1 of the wave
	int32_t Eval2(uint32_t S, int32_t lg)
	{
		Stage &st = stage[1];
		if (!st.active) return Eval1(S, lg);
		int32_t lgs, gain;
		uint32_t b = st.Map(S, lgs, gain);
		int32_t cb = (st.cb * gain) >> 15;
		int32_t v = cb * Eval1(b, lg + lgs);
		if (st.ca) v += st.ca * Eval1(S, lg);
		v >>= 15;
		st.prev = v;
		return v;
	}

	int32_t Eval1(uint32_t S, int32_t lg)
	{
		Stage &st = stage[0];
		if (!st.active) return Base(S, lg);
		int32_t lgs, gain;
		uint32_t b = st.Map(S, lgs, gain);
		int32_t cb = (st.cb * gain) >> 15;
		int32_t v = cb * Base(b, lg + lgs);
		if (st.ca) v += st.ca * Base(S, lg);
		v >>= 15;
		st.prev = v;
		return v;
	}

	// The wave itself, from the table with few enough harmonics for lg (how
	// fast it is being played, in 1/256 octaves past the first table's limit)
	int32_t Base(uint32_t p, int32_t lg) const
	{
		int L = (lg + 255) >> 8;
		L = L < 0 ? 0 : (L >= kLevels ? kLevels - 1 : L);
		int32_t a = Lookup(tabA[L], p);
		if (waveF == 0) return a;
		int32_t b = Lookup(tabB[L], p);
		return a + (((b - a) * waveF) >> 15);
	}

private:
	uint32_t acc = 0, inc = 0;
	int controlCount = kControlEvery - 1;
	bool lfoResetPending = false;
	// The clock at Pulse In 1
	uint32_t sinceClock = 0, beat = 0;
	int32_t clockLow = 0;
	bool clockWas = false, clockSeen = false, beatPending = false;
	int syncMade[2] = {-1, -1};
	uint32_t periodMade[2] = {0, 0};
	const int16_t *tabA[kLevels] = {}, *tabB[kLevels] = {};
	int32_t pitchT = 0, pitchS = 0, pitchMod = 0, glideK = 65536;
	int32_t cvPitch = 0;
	int32_t lgBase = 0;
	int waveA = 0;
	int32_t waveF = 0;
	int32_t level = 26208, drive = 0;
	int crushShift = 0;
	int out2Mode = Out2Dry;
	bool armed = false;
	int32_t lastAudio1 = 0;
	HalfBand hb;
	uint32_t dither = 1;

	// log2 of the phase increment at which the first table (256
	// harmonics) reaches 56kHz, past which the filter can't remove it:
	// 56000 / 96000 * 2^30 / 256
	static constexpr uint32_t kRefInc = 2446677;
	const int32_t kLgRef = Log2Q8(kRefInc);
	// The same for Out 2, at 48kHz: 22kHz, and the increment is per 96kHz
	// sample, so 22000 / 96000 * 2^30 / 256
	const int32_t kLg48 = Log2Q8(961194) - kLgRef;

	static int32_t Mod(int32_t source, int32_t amount) {return (source * amount) >> 15;}

	// 8mu tilt to Q15, ignoring about 15 degrees either side of flat so
	// holding it doesn't nudge anything
	static int32_t TiltQ(int32_t t)
	{
		static constexpr int32_t kDead = 512;
		if (t > -kDead && t < kDead) return 0;
		int32_t d = t > 0 ? t - kDead : t + kDead;
		int32_t m = (d * 32767) / (2032 - kDead);
		return m > 32767 ? 32767 : (m < -32767 ? -32767 : m);
	}

	int32_t Out2(uint32_t S)
	{
		int32_t lg = lgBase - kLg48;
		switch (out2Mode)
		{
		case Out2Dry: return Base(S, lg);
		case Out2Mutant1:
		{
			// (without upsetting mutant 1's feedback)
			int32_t keep = stage[0].prev;
			int32_t v = Eval1(S, lg);
			stage[0].prev = keep;
			return v;
		}
		case Out2Sub1: return Lookup(tables->Get(WSquare, Level(lg - 256)), S >> 1);
		default: return Lookup(tables->Get(WSquare, Level(lg - 512)), S >> 2);
		}
	}
	static int Level(int32_t lg)
	{
		int L = (lg + 255) >> 8;
		return L < 0 ? 0 : (L >= kLevels ? kLevels - 1 : L);
	}

	// Drive, level and bit crush, to the output's range
	int32_t Finish(int32_t x)
	{
		if (drive > 0)
		{
			// Up to 8x into a soft clip, y = (3n - n^3) / 2 for n = 2/3 of
			// the driven level: full scale at 1.5, and flat there
			int32_t n = (x * (4096 + drive * 7)) >> 12;
			n = (n * 21845) >> 15;
			if (n > 32767) n = 32767;
			if (n < -32767) n = -32767;
			int32_t n3 = (((n * n) >> 15) * n) >> 15;
			x = (3 * n - n3) >> 1;
		}
		// Level after the drive, so it still sets how loud a driven wave is
		x = (x * level) >> 15;
		// No DC blocker: like an analogue pulse wave, a narrow pulse keeps
		// its two levels and sits mostly at the low one.  (Taking the DC
		// out would push its short peak far past full scale.)
		if (crushShift) x = (x >> crushShift) << crushShift;
		else
		{
			// Dither (triangular, one step of the 12-bit output either
			// way): without it, rounding a quiet wave - a narrow pulse - to
			// 12 bits makes whistles tied to the sample rate; with it, a
			// smooth hiss 70dB down
			dither = dither * 1664525u + 1013904223u;
			x += int32_t((dither >> 28) + ((dither >> 24) & 15)) - 15;
		}
		x >>= 4;
		return x > 2047 ? 2047 : (x < -2048 ? -2048 : x);
	}
};

} // namespace mutagen

#endif
