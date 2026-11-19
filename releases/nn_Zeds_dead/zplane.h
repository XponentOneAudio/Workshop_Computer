// ============================================================================
//  zplane.h  —  the maths of the "Zeds Dead" morphing Z-plane filter
// ============================================================================
//
//  This file contains ALL of the signal-processing maths for the card, and
//  deliberately does NOT include anything Workshop-Computer specific. That
//  means you can compile it on a normal PC (see tools/host_test.cpp) to plot
//  the filter responses and check it is stable, before ever flashing a card.
//  The web editor (web/index.html) contains a line-for-line JavaScript copy of
//  the designer part, so what you see in the browser is what the card does.
//
//  If you are new to digital filters, read README.md first — it contains a
//  longer walkthrough. The comments here try to be self-contained too.
//
// ----------------------------------------------------------------------------
//  THE BIG IDEA IN ONE PARAGRAPH
// ----------------------------------------------------------------------------
//
//  A digital filter can be completely described by where its POLES and ZEROS
//  sit on the "z-plane" (a 2D map of complex numbers). Poles make the
//  response bulge up (resonances); zeros pin the response down (notches, or the
//  rolled-off end of a low/high-pass). The angle of a pole/zero around the
//  circle is its FREQUENCY; the distance from the centre is how SHARP it is.
//
//  So instead of designing "a lowpass" and "a bandpass" as separate circuits
//  and crossfading their outputs (which sounds like two filters, not one), we
//  describe each filter shape — a "slot" — as a list of pole/zero positions,
//  and to morph we literally slide the poles and zeros from one slot's
//  positions to the next. Every in-between point is itself a real, single
//  filter. That is what E-mu called "Z-plane" filtering in the 1990s.
//
// ----------------------------------------------------------------------------
//  STRUCTURE
// ----------------------------------------------------------------------------
//
//  * The filter is a CASCADE of up to kMaxSections "biquads" (2nd-order
//    sections). Each biquad owns one pair of poles and one pair of zeros, so
//    3 sections = a 6-pole filter. Audio goes through section 0, then 1, ...
//
//  * A SLOT (SlotData) says, for each section, where its poles and zeros go,
//    RELATIVE to the cutoff frequency set by the Main knob. Slots are designed
//    in the web editor by dragging poles and zeros around.
//
//  * A BANK (BankData) is an ordered list of up to kNumSlots slots. The morph
//    control (knob X) moves smoothly through the list: 0.0 = slot 1,
//    1.0 = slot 2, 2.5 = halfway between slots 3 and 4, and so on.
//
//  * The DESIGNER (float maths, slow, runs on the RP2040's second core a
//    thousand or more times per second) turns knob positions into biquad
//    coefficients.
//
//  * The RUNNER (fixed-point integer maths, fast, runs 48000 times per second
//    on the first core) pushes audio samples through the biquads.
//
// ============================================================================

#ifndef ZEDS_DEAD_ZPLANE_H
#define ZEDS_DEAD_ZPLANE_H

#include <cstdint>
#include <cmath>

// On the Pico we want time-critical functions placed in RAM rather than flash
// (flash is read through a small cache and can stall). On a PC that macro does
// not exist, so we define it as "do nothing".
#ifndef __not_in_flash_func
#define __not_in_flash_func(f) f
#endif

namespace zp
{

// ----------------------------------------------------------------------------
//  Global constants
// ----------------------------------------------------------------------------

constexpr float kSampleRate     = 48000.0f;  // Workshop Computer runs at 48 kHz
constexpr float kPi             = 3.14159265358979f;
constexpr int   kMaxSections    = 8;         // up to 8 biquads = 16 poles + 16 zeros
constexpr int   kNumSlots       = 6;         // slots per bank to morph through
constexpr int   kNumBanks       = 2;         // bank A (switch up), bank B (middle)

// Highest pole/zero frequency we allow, as a fraction of the sample rate.
// Nyquist (the highest representable frequency) is 0.5; we stay a bit below
// it because poles right at Nyquist behave badly.
constexpr float kMaxFreqFraction = 0.45f;    // = 21.6 kHz at 48 kHz
constexpr float kMinFreqHz       = 10.0f;

// Poles are never allowed closer to the unit circle than this. 0.9995 means a
// "ping" takes about 2000 samples (~40 ms) to die away by a factor of e.
constexpr float kMaxPoleRadius   = 0.9995f;

// How much the Resonance knob (at full) multiplies a pole's Q, for a pole whose
// resonance weight is 1. Q is multiplied by (1 + kResonanceGain·weight·res²).
constexpr float kResonanceGain   = 15.0f;

// ----------------------------------------------------------------------------
//  Describing filter shapes ("slots")
// ----------------------------------------------------------------------------
//
//  POLES. Every section has one pole pair: a point p = r·e^{jθ} and its mirror
//  image r·e^{−jθ}. We store it in a form that SCALES with the cutoff knob:
//
//     poleRatio  :  θ = poleRatio × θc, where θc = 2π·cutoff/fs is the
//                   cutoff's own angle on the circle.
//     poleDamp   :  r = exp(−θ · poleDamp)
//
//  Why exp(−θ·d)? Think of the analogue world: a resonance at angular
//  frequency ω with quality factor Q decays like e^{−ω t/(2Q)}. Sampling that
//  every T seconds gives a pole radius of e^{−ωT/(2Q)} = e^{−θ/(2Q)}. So our
//  "damping" d is (about) 1/(2Q), and keeping d fixed while the cutoff moves
//  keeps the Q fixed: the filter keeps its character as you sweep it, just
//  like an analogue filter does. d > 0 always means r < 1: stable.
//
//  ZEROS. Every section has one zero pair, in one of two forms:
//
//     kZeroPair : a mirror pair z = r·e^{±jθ}, scaled with cutoff just like
//                 the pole:  θ = zeroA × θc,  r = exp(−θ · zeroB).
//                 zeroB = 0 puts the zeros exactly ON the unit circle — a
//                 perfect notch. (Zeros on or inside the circle only; they
//                 can't affect stability, but that keeps the maths simple.)
//     kZeroReal : two zeros on the real axis at zeroA and zeroB (each −1..+1).
//                 These do NOT move with the cutoff. The useful ones:
//                     (−1, −1) : double zero at Nyquist → lowpass slopes
//                     (+1, +1) : double zero at DC      → highpass slopes
//                     (+1, −1) : one at each end        → bandpass slopes
//                     ( 0,  0) : zeros at the centre    → "no zeros"
//
//  Expanding the zeros gives the biquad's numerator n0 + n1·z⁻¹ + n2·z⁻²:
//     pair : (1 − z·z⁻¹)(1 − z̄·z⁻¹) = 1 − 2r·cosθ·z⁻¹ + r²·z⁻²
//     real : (1 − a·z⁻¹)(1 − b·z⁻¹) = 1 − (a+b)·z⁻¹ + ab·z⁻²
//
//  resWeight (0..1) says how strongly the Resonance knob sharpens this pole.
//  A 6-pole lowpass typically has only its last pole pair "sing".

enum ZeroMode : uint8_t { kZeroPair = 0, kZeroReal = 1 };

struct SectionData
{
	float   poleRatio;   // pole angle  = poleRatio × cutoff angle
	float   poleDamp;    // pole radius = exp(−angle × poleDamp)   (≈ 1/(2Q))
	float   resWeight;   // 0..1: how much the Resonance knob affects this pole
	uint8_t zeroMode;    // kZeroPair or kZeroReal
	float   zeroA;       // pair: angle ratio   | real: first zero (−1..1)
	float   zeroB;       // pair: damping (≥ 0) | real: second zero (−1..1)
};

struct SlotData
{
	uint8_t     numSections;        // 0 .. kMaxSections (0 = flat, no filtering)
	SectionData sec[kMaxSections];
};

struct BankData
{
	uint8_t  numSlots;              // 1 .. kNumSlots: how many slots the morph covers
	SlotData slot[kNumSlots];
};

// Number of biquads the audio core must run for a whole bank: the largest
// section count of any slot in use. Morphing from a 2-section slot to a
// 5-section one needs all 5 sections running throughout.
inline int sectionsNeeded(const BankData& b)
{
	int n = 0;
	for (int i = 0; i < b.numSlots && i < kNumSlots; ++i)
		if (b.slot[i].numSections > n) n = b.slot[i].numSections;
	return n;
}

// Data can arrive from the web editor or from flash, so never trust it: force
// every number into a sensible range, and replace NaNs ("not a number") with
// safe defaults. A NaN coefficient would silence the card until power-off.
inline float clampf(float x, float lo, float hi, float fallback)
{
	if (!(x == x)) return fallback;    // NaN is the only value not equal to itself
	return x < lo ? lo : (x > hi ? hi : x);
}

inline void sanitize(SlotData& s)
{
	if (s.numSections > kMaxSections) s.numSections = kMaxSections;
	for (int i = 0; i < kMaxSections; ++i)
	{
		SectionData& d = s.sec[i];
		d.poleRatio = clampf(d.poleRatio, 0.001f, 1000.0f, 1.0f);
		d.poleDamp  = clampf(d.poleDamp, 0.0001f, 20.0f, 0.7f);
		d.resWeight = clampf(d.resWeight, 0.0f, 1.0f, 0.0f);
		if (d.zeroMode != kZeroPair) d.zeroMode = kZeroReal;
		if (d.zeroMode == kZeroPair)
		{
			d.zeroA = clampf(d.zeroA, 0.001f, 1000.0f, 1.0f);
			d.zeroB = clampf(d.zeroB, 0.0f, 50.0f, 0.0f);
		}
		else
		{
			d.zeroA = clampf(d.zeroA, -1.0f, 1.0f, 0.0f);
			d.zeroB = clampf(d.zeroB, -1.0f, 1.0f, 0.0f);
		}
	}
}

inline void sanitize(BankData& b)
{
	if (b.numSlots < 1) b.numSlots = 1;
	if (b.numSlots > kNumSlots) b.numSlots = kNumSlots;
	for (int i = 0; i < kNumSlots; ++i) sanitize(b.slot[i]);
}

// ----------------------------------------------------------------------------
//  Float biquad + helpers (used by the designer)
// ----------------------------------------------------------------------------

// A biquad's transfer function, in the sign convention we use throughout:
//
//            b0 + b1·z⁻¹ + b2·z⁻²
//   H(z) = ------------------------
//             1 + a1·z⁻¹ + a2·z⁻²
//
// which as a recipe for computing output sample y[n] from input x[n] is
//
//   y[n] = b0·x[n] + b1·x[n-1] + b2·x[n-2]  −  a1·y[n-1] − a2·y[n-2]
//
// (z⁻¹ just means "one sample ago".)
struct Biquad
{
	float b0, b1, b2, a1, a2;
};

// A pole pair in polar form: radius r (0 ≤ r < 1) and angle theta (radians,
// 0..π). The pair is r·e^{+jθ} and its mirror image r·e^{-jθ}.
struct Pole
{
	float r, theta;
};

// Turn a frequency in Hz into an angle on the z-plane, clamped to a safe range.
// One trip round the full circle (2π) equals the sample rate, so
//   θ = 2π · f / fs
inline float hzToAngle(float hz)
{
	if (hz < kMinFreqHz) hz = kMinFreqHz;
	const float maxHz = kMaxFreqFraction * kSampleRate;
	if (hz > maxHz) hz = maxHz;
	return 2.0f * kPi * hz / kSampleRate;
}

// Where does this section's pole pair sit, for the current cutoff and
// resonance? (See "POLES" above for the reasoning.)
inline Pole placePole(const SectionData& s, float cutoffHz, float resonance)
{
	Pole p;
	p.theta = hzToAngle(cutoffHz * s.poleRatio);
	// Resonance divides the damping, i.e. multiplies Q. It is squared so the
	// knob has more travel in the useful low-to-mid range and only the last
	// part of its turn goes "screaming".
	const float d = s.poleDamp / (1.0f + kResonanceGain * s.resWeight * resonance * resonance);
	p.r = expf(-p.theta * d);
	if (p.r > kMaxPoleRadius) p.r = kMaxPoleRadius;
	return p;
}

// Numerator (n0, n1, n2) for this section's zeros. (See "ZEROS" above.)
inline void placeZeros(const SectionData& s, float cutoffHz, float n[3])
{
	n[0] = 1.0f;
	if (s.zeroMode == kZeroPair)
	{
		const float th = hzToAngle(cutoffHz * s.zeroA);
		const float r  = expf(-th * s.zeroB);
		n[1] = -2.0f * r * cosf(th);
		n[2] = r * r;
	}
	else
	{
		n[1] = -(s.zeroA + s.zeroB);
		n[2] = s.zeroA * s.zeroB;
	}
}

// ---- Measuring a biquad --------------------------------------------------
// To know how loud a filter is at some frequency, evaluate H(z) at the point
// z = e^{jω} on the unit circle (ω = 2πf/fs). With z⁻¹ = e^{−jω} = cosω − j·sinω
// and z⁻² = cos2ω − j·sin2ω, each polynomial c0 + c1·z⁻¹ + c2·z⁻² becomes
//
//    real part = c0 + c1·cosω + c2·cos2ω
//    imag part =    − c1·sinω − c2·sin2ω
//
// and |H|² = |numerator|² / |denominator|². We pass in the cos/sin values so
// that the (slow) trig only needs computing once per probe frequency, then can
// be reused for every section.
struct Probe
{
	float c1, s1, c2, s2;   // cos ω, sin ω, cos 2ω, sin 2ω
	explicit Probe(float w = 0.0f)
	{
		c1 = cosf(w); s1 = sinf(w);
		c2 = 2.0f * c1 * c1 - 1.0f;   // double-angle identities: cheaper than
		s2 = 2.0f * s1 * c1;          // calling cosf/sinf again
	}
};

inline float polyMagSq(float c0, float c1, float c2, const Probe& p)
{
	const float re = c0 + c1 * p.c1 + c2 * p.c2;
	const float im = -(c1 * p.s1 + c2 * p.s2);
	return re * re + im * im;
}

inline float magSq(const Biquad& q, const Probe& p)
{
	const float num = polyMagSq(q.b0, q.b1, q.b2, p);
	const float den = polyMagSq(1.0f, q.a1, q.a2, p);
	return num / (den > 1.0e-30f ? den : 1.0e-30f);
}

// ----------------------------------------------------------------------------
//  THE DESIGNER: knobs → coefficients
// ----------------------------------------------------------------------------

struct DesignParams
{
	float cutoffHz;   // where the filter "is" (Main knob + CV1)
	float morph;      // 0 .. numSlots−1 (knob X + CV2)
	float resonance;  // 0 .. 1 (knob Y)
};

struct Design
{
	int    numSections;              // how many of sec[] are in use
	Biquad sec[kMaxSections];
	float  makeup;                   // overall output gain applied after the cascade
	float  poleTheta[kMaxSections];  // remembered for the loudness probes
};

// Design the filter that lies a fraction t of the way from slot A to slot B.
// numRun is how many sections to produce (normally sectionsNeeded(bank)).
//
// This is the heart of the card. Read it slowly!
inline void designBetween(const SlotData& A, const SlotData& B, float t,
                          float cutoffHz, float resonance, int numRun, Design& out)
{
	if (numRun > kMaxSections) numRun = kMaxSections;
	if (numRun < 0) numRun = 0;
	out.numSections = numRun;

	for (int s = 0; s < numRun; ++s)
	{
		const bool hasA = s < A.numSections;
		const bool hasB = s < B.numSections;
		Biquad q;

		if (!hasA && !hasB)
		{
			// Neither slot uses this section: make it a "wire", H(z) = 1.
			q.b0 = 1.0f; q.b1 = q.b2 = q.a1 = q.a2 = 0.0f;
			out.sec[s] = q;
			out.poleTheta[s] = -1.0f;   // marker: no pole to probe
			continue;
		}

		// --- 1. Where are the poles in slot A and in slot B?
		//
		// If one slot doesn't use this section, we pretend its pole sits at
		// radius 0 (the centre of the circle, where a pole has no effect), at
		// the SAME angle as the other slot's pole. Then morphing just grows
		// the resonance out of nothing, rather than also sweeping it in from
		// some arbitrary frequency.
		Pole pa = hasA ? placePole(A.sec[s], cutoffHz, resonance) : Pole{ 0.0f, 0.0f };
		Pole pb = hasB ? placePole(B.sec[s], cutoffHz, resonance) : Pole{ 0.0f, 0.0f };
		if (!hasA) pa.theta = pb.theta;
		if (!hasB) pb.theta = pa.theta;

		// --- 2. Morph the POLES in polar form.
		//   * radius: straight-line (linear) interpolation. Both radii are < 1,
		//     so anything in between is also < 1  ⇒  STABLE at every point of
		//     the morph. This is the key safety property of morphing in the
		//     pole domain. (If you instead interpolated the a1/a2 coefficients
		//     of two arbitrary higher-order filters you could wander outside
		//     the unit circle and the filter would explode.)
		//   * angle: geometric interpolation, θ = θA^(1−t) · θB^t, i.e. linear
		//     in LOG frequency. Our ears hear pitch logarithmically, so a
		//     formant moving from 300 Hz to 2400 Hz passes 850 Hz at halfway
		//     instead of 1350 Hz.
		const float r = pa.r + (pb.r - pa.r) * t;
		const float theta = pa.theta * powf(pb.theta / pa.theta, t);   // angles are ≥ hzToAngle(10 Hz) > 0

		// Expand the pole pair back into denominator coefficients:
		//   (1 − r·e^{jθ}z⁻¹)(1 − r·e^{−jθ}z⁻¹) = 1 − 2r·cosθ·z⁻¹ + r²·z⁻²
		q.a1 = -2.0f * r * cosf(theta);
		q.a2 = r * r;

		// --- 3. Morph the ZEROS by interpolating numerator coefficients.
		//
		// Zeros can never make a filter unstable (they only sit in the
		// numerator; the "feedback" is all in the denominator), so we are free
		// to interpolate them the simplest way. That also copes with real
		// zeros morphing into a mirror pair, which polar interpolation can't.
		// A slot that doesn't use this section counts as "no zeros" (1, 0, 0).
		float na[3] = { 1.0f, 0.0f, 0.0f }, nb[3] = { 1.0f, 0.0f, 0.0f };
		if (hasA) placeZeros(A.sec[s], cutoffHz, na);
		if (hasB) placeZeros(B.sec[s], cutoffHz, nb);
		q.b0 = na[0] + (nb[0] - na[0]) * t;
		q.b1 = na[1] + (nb[1] - na[1]) * t;
		q.b2 = na[2] + (nb[2] - na[2]) * t;

		// --- 4. Normalise this section so its loudest point is gain 1.
		//
		// A resonant pole pair can have a gain of hundreds at its peak. If we
		// let that through untamed, the integer audio path would overflow. So
		// we find (approximately) the loudest point of this section's response
		// and divide by it. A biquad's response nearly always peaks at DC, at
		// Nyquist, or near the pole angle, so those three probes are enough.
		const Probe pDC(0.0f), pNyq(kPi), pPole(theta);
		float peak = magSq(q, pDC);
		const float mNyq = magSq(q, pNyq);   if (mNyq > peak) peak = mNyq;
		const float mPol = magSq(q, pPole);  if (mPol > peak) peak = mPol;
		const float g = 1.0f / sqrtf(peak > 1.0e-12f ? peak : 1.0e-12f);
		q.b0 *= g; q.b1 *= g; q.b2 *= g;

		out.sec[s] = q;
		out.poleTheta[s] = theta;
	}

	// --- 5. Overall loudness ("makeup") gain.
	//
	// Each section now peaks at 1, but the CASCADE's peak can be much lower
	// (e.g. three bandpasses at slightly different frequencies each knock the
	// others down). We probe the whole cascade at DC, Nyquist and every pole
	// angle, find its loudest point, and choose a makeup gain to bring that
	// back to 1 ("constant peak" normalisation).
	//
	// Pure constant-peak normalisation has a downside: with lots of resonance
	// the peak is held at 1 so the REST of the spectrum (the "body") gets
	// quieter and the sound thins out. So we let the peak rise by up to ×4
	// (+12 dB) as resonance goes up — but ONLY in proportion to how "peaky"
	// the response actually is. We estimate the body as the louder of the DC
	// and Nyquist gains (the two ends of the spectrum, where a lowpass or
	// highpass has its flat passband):
	//
	//     peakiness = 1 − body/peak      (0 = flat like a notch, 1 = all peak)
	//     boost     = 1 + 3 · resonance · peakiness
	//
	// So a resonant lowpass gets its full boost, a notch (whose "peak" is just
	// its flat passband) gets none, and in-between shapes get something
	// in-between. Those extra-hot peaks are tamed by a gentle soft clipper in
	// the runner, which also adds a pleasing analogue-ish grit.
	float totalPeak = 0.0f, body = 0.0f;
	for (int k = -2; k < numRun; ++k)
	{
		float w;
		if (k == -2) w = 0.0f;              // DC
		else if (k == -1) w = kPi;          // Nyquist
		else if (out.poleTheta[k] < 0.0f) continue;   // unused section
		else w = out.poleTheta[k];
		const Probe p(w);
		float mag = 1.0f;
		for (int s = 0; s < numRun; ++s) mag *= magSq(out.sec[s], p);
		if (mag > totalPeak) totalPeak = mag;
		if (k < 0 && mag > body) body = mag;   // the DC & Nyquist probes
	}
	totalPeak = sqrtf(totalPeak);
	body = sqrtf(body);
	const float safePeak = totalPeak > 1.0e-4f ? totalPeak : 1.0e-4f;
	float peakiness = 1.0f - body / safePeak;    // 0..1
	if (peakiness < 0.0f) peakiness = 0.0f;
	float makeup = (1.0f + 3.0f * resonance * peakiness) / safePeak;
	if (makeup > 200.0f) makeup = 200.0f;   // don't amplify near-silence into noise
	out.makeup = makeup;
}

// Design for a morph position within a bank.
inline void designMorph(const BankData& bank, const DesignParams& in, Design& out)
{
	int n = bank.numSlots;
	if (n < 1) n = 1;
	if (n > kNumSlots) n = kNumSlots;
	float m = in.morph;
	if (m < 0.0f) m = 0.0f;
	if (m > float(n - 1)) m = float(n - 1);
	int i = int(m);                     // slot "A" (floor)
	if (i > n - 2) i = n - 2;
	if (i < 0) i = 0;
	const int j = (n > 1) ? i + 1 : i;  // slot "B"
	const float t = (n > 1) ? m - float(i) : 0.0f;
	designBetween(bank.slot[i], bank.slot[j], t, in.cutoffHz, in.resonance, sectionsNeeded(bank), out);
}

// ----------------------------------------------------------------------------
//  THE RUNNER: fixed-point biquads for the 48 kHz audio path
// ----------------------------------------------------------------------------
//
//  The RP2040 has no floating-point hardware, so float maths is done in
//  software and costs tens of clock cycles per operation. At 48 kHz we have
//  only ~4000 cycles per sample (at 192 MHz) for BOTH channels and everything
//  else. So the audio path uses integers in "fixed point": we pretend there is
//  a binary point at a fixed position inside a 32-bit integer.
//
//  COEFFICIENTS are stored as Q3.28 — 28 bits after the binary point, so the
//  integer 1<<28 (= 268435456) means 1.0, and the range is about ±8. Biquad
//  coefficients live within ±2 (a1) or so, so this is plenty of range while
//  keeping very fine resolution.
//
//  SAMPLES: the Computer's audio inputs give 12-bit numbers (−2048..2047).
//  We shift them up by 12 bits (×4096) so the filter keeps 12 extra bits of
//  precision below the input's smallest step. Full scale is therefore ±2²³,
//  leaving 8 bits (48 dB) of headroom inside the 32-bit integer for
//  resonant peaks and transients.
//
//  The products sample × coefficient are up to 2²³ × 2³⁰ ≈ 2⁵³, so we
//  multiply into a 64-bit accumulator and shift right by 28 at the end.

constexpr int     kCoeffFracBits  = 28;
constexpr int32_t kCoeffOne       = int32_t(1) << kCoeffFracBits;
constexpr int     kSampleShift    = 12;                  // 12-bit in → 24-bit internal
constexpr int32_t kFullScale      = int32_t(2048) << kSampleShift;  // = 2²³
constexpr int     kMakeupFracBits = 16;                  // makeup gain stored as Q16

// Integer biquad coefficients, ordered so they can be treated as an array of
// 5 for the smoothing loop.
struct FixedBiquad
{
	int32_t c[5];   // b0, b1, b2, a1, a2 in Q3.28
};

// Everything core 0 needs for one stereo update.
struct FixedFrame
{
	int32_t     numSections;                 // how many biquads to run
	FixedBiquad sec[2][kMaxSections];        // [channel][section]
	int32_t     makeup[2];                   // Q16
};

inline int32_t floatToQ28(float x)
{
	if (x >  7.99f) x =  7.99f;
	if (x < -7.99f) x = -7.99f;
	return int32_t(x * float(kCoeffOne));
}

// "A wire": the biquad that passes audio through untouched.
inline FixedBiquad fixedPassthrough()
{
	FixedBiquad q;
	q.c[0] = kCoeffOne; q.c[1] = q.c[2] = q.c[3] = q.c[4] = 0;
	return q;
}

inline void toFixed(const Design& d, FixedBiquad sec[kMaxSections], int32_t& makeup)
{
	for (int s = 0; s < kMaxSections; ++s)
	{
		if (s >= d.numSections) { sec[s] = fixedPassthrough(); continue; }
		sec[s].c[0] = floatToQ28(d.sec[s].b0);
		sec[s].c[1] = floatToQ28(d.sec[s].b1);
		sec[s].c[2] = floatToQ28(d.sec[s].b2);
		sec[s].c[3] = floatToQ28(d.sec[s].a1);
		sec[s].c[4] = floatToQ28(d.sec[s].a2);
	}
	makeup = int32_t(d.makeup * float(1 << kMakeupFracBits));
}

// ---- A fast 32 × 32 → 64-bit signed multiply --------------------------------
//
// The Cortex-M0+ has a single-cycle MULS, but it only gives the LOW 32 bits of
// a product. Writing  int64_t(a) * b  in C makes the compiler call a library
// routine that multiplies two full 64-bit numbers (6 multiplies, ~25
// instructions, plus the call) — wasteful, since our numbers are only 32-bit.
//
// This does the schoolbook method with 16-bit "digits" instead:
//
//     a = ah·2¹⁶ + al        (ah signed, al unsigned 0..65535)
//     b = bh·2¹⁶ + bl
//     a·b = ah·bh·2³² + (ah·bl + al·bh)·2¹⁶ + al·bl
//
// Each of the four partial products fits in 32 bits, so each is one MULS:
//   al·bl  < 2³²            (unsigned)
//   ah·bl, al·bh  within ±2³¹  (e.g. −32768 × 65535 = −2147450880)
//   ah·bh  ≤ 2³⁰
//
// On the RP2040 it is written in Thumb assembly: 17 instructions, no call.
// (Written in C, the compiler handles the 64-bit additions so clumsily that
// it ends up SLOWER than the library routine — we measured.) Each middle
// product is added in two halves: its low 16 bits shifted up into the low
// word, and its sign-extended top 16 bits into the high word, with ADCS
// carrying between them. On a PC the plain C version below is used, and
// tools/emulate_m0.py runs the real ARM code in an emulator to check the two
// agree.
static inline int64_t __not_in_flash_func(smull32)(int32_t a, int32_t b)
{
#if defined(__ARM_ARCH_6M__)
	uint32_t lo, hi, t;
	__asm__ (
		".syntax unified         \n\t"   // same as the Pico SDK's pico_default_asm
		"uxth %[lo], %[a]        \n\t"   // lo = al
		"asrs %[a], %[a], #16    \n\t"   // a  = ah
		"uxth %[t], %[b]         \n\t"   // t  = bl
		"asrs %[b], %[b], #16    \n\t"   // b  = bh
		"movs %[hi], %[a]        \n\t"
		"muls %[hi], %[b]        \n\t"   // hi = ah·bh
		"muls %[a], %[t]         \n\t"   // a  = ah·bl   (middle product 1)
		"muls %[t], %[lo]        \n\t"   // t  = al·bl   (low word so far)
		"muls %[lo], %[b]        \n\t"   // lo = al·bh   (middle product 2)
		"lsls %[b], %[a], #16    \n\t"   // add middle product 1 × 2¹⁶
		"asrs %[a], %[a], #16    \n\t"
		"adds %[t], %[b]         \n\t"
		"adcs %[hi], %[a]        \n\t"
		"lsls %[b], %[lo], #16   \n\t"   // add middle product 2 × 2¹⁶
		"asrs %[lo], %[lo], #16  \n\t"
		"adds %[t], %[b]         \n\t"
		"adcs %[hi], %[lo]       \n\t"
		: [lo] "=&l"(lo), [hi] "=&l"(hi), [t] "=&l"(t), [a] "+l"(a), [b] "+l"(b)
		:
		: "cc");
	return int64_t((uint64_t(hi) << 32) | t);
#else
	const uint32_t al = uint32_t(a) & 0xFFFFu, bl = uint32_t(b) & 0xFFFFu;
	const int32_t  ah = a >> 16, bh = b >> 16;
	uint64_t r = (uint64_t(uint32_t(ah * bh)) << 32) + uint64_t(al * bl);
	r += uint64_t(int64_t(ah * int32_t(bl))) << 16;
	r += uint64_t(int64_t(int32_t(al) * bh)) << 16;
	return int64_t(r);
#endif
}

// Memory of one biquad between samples: the last two inputs and outputs, plus
// the rounding error "left over" from the previous output (see below).
struct BiquadState
{
	int32_t x1 = 0, x2 = 0, y1 = 0, y2 = 0;
	int64_t err = 0;
};

// One sample through one biquad, "Direct Form I":
//
//   y[n] = b0·x[n] + b1·x[n-1] + b2·x[n-2] − a1·y[n-1] − a2·y[n-2]
//
// Direct Form I keeps separate memories for past inputs and past outputs. It
// costs a couple more variables than the popular "Direct Form II" but has no
// internal node that can overflow on its own, and it copes gracefully with
// coefficients that change every sample (as ours do while morphing).
//
// ERROR FEEDBACK: shifting the 64-bit accumulator right by 28 bits throws away
// the low 28 bits — a tiny rounding error every sample. For a pole very close
// to the unit circle at a low frequency (say 30 Hz with high Q), the feedback
// loop amplifies those errors enormously and you hear hiss or "limit cycles".
// A classic cheap cure is to remember the discarded bits and add them back in
// on the next sample, so the errors cancel out over time instead of
// accumulating. That's the `err` variable.
static inline int32_t __not_in_flash_func(runBiquad)(const FixedBiquad& q, BiquadState& st, int32_t x)
{
	int64_t acc = st.err;
	acc += smull32(q.c[0], x);
	acc += smull32(q.c[1], st.x1);
	acc += smull32(q.c[2], st.x2);
	acc -= smull32(q.c[3], st.y1);
	acc -= smull32(q.c[4], st.y2);

	int64_t y = acc >> kCoeffFracBits;           // back to sample scale
	st.err = acc - (y << kCoeffFracBits);        // the bits we just dropped

	// Safety net: clamp to ±2³⁰ so a wild transient can't wrap round the
	// 32-bit integer (which would sound like a loud crack).
	const int64_t lim = int64_t(1) << 30;
	if (y >  lim) y =  lim;
	if (y < -lim) y = -lim;

	st.x2 = st.x1; st.x1 = x;
	st.y2 = st.y1; st.y1 = int32_t(y);
	return int32_t(y);
}

// Gentle cubic soft clipper.  f(v) = v − (4/27)·v³/F²  for |v| ≤ 1.5F,
// where F is full scale. Check: f'(0) = 1 (quiet signals untouched),
// f'(1.5F) = 0 and f(1.5F) = F, so the curve flattens out exactly at full
// scale and joins a hard limit with no corner — no harsh edge.
static inline int32_t __not_in_flash_func(softClip)(int32_t v)
{
	const int32_t knee = kFullScale + (kFullScale >> 1);   // 1.5 × full scale
	if (v >=  knee) return  kFullScale;
	if (v <= -knee) return -kFullScale;
	// v³ would overflow 64 bits, so drop 9 bits first:
	//   w = v/2⁹  →  v³/F² = w³·2²⁷ / 2⁴⁶ = w³ / 2¹⁹
	// |v| < 1.5·2²³ so |w| < 24576 and w² < 2³⁰: fits a 32-bit int.
	const int32_t w = v >> 9;
	const int64_t cube = smull32(w * w, w) >> 19;
	// Multiply by 4/27 WITHOUT dividing: division is very slow on the M0+
	// (no divide instruction for 64-bit numbers), so instead we multiply by
	// round(4/27 × 2¹⁶) = 9709 and shift right by 16. Same answer to within
	// one part in ~20000, at a fraction of the cost.
	return int32_t(v - (smull32(int32_t(cube), 9709) >> 16));   // |cube| < 2²⁵
}

} // namespace zp

#endif
