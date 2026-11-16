// ============================================================================
//  zplane.h  —  the maths of the "Zeds Dead" morphing Z-plane filter
// ============================================================================
//
//  This file contains ALL of the signal-processing maths for the card, and
//  deliberately does NOT include anything Workshop-Computer specific. That
//  means you can compile it on a normal PC (see tools/host_test.cpp) to plot
//  the filter responses and check it is stable, before ever flashing a card.
//
//  If you are new to digital filters, read README.md first — it contains a
//  longer walkthrough with pictures-in-words. The comments here assume you have
//  skimmed it, but try to be self-contained too.
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
//  describe each filter shape — a "frame" — as a list of pole/zero positions,
//  and to morph we literally slide the poles and zeros from one frame's
//  positions to the next. Every in-between point is itself a real, single
//  filter. That is what E-mu called "Z-plane" filtering in the 1990s.
//
// ----------------------------------------------------------------------------
//  STRUCTURE
// ----------------------------------------------------------------------------
//
//  * The filter is a CASCADE of kSections "biquads" (2nd-order sections).
//    Each biquad owns one pair of poles and one pair of zeros, so 3 sections
//    = a 6-pole filter. Audio goes through section 0, then 1, then 2.
//
//  * A FRAME says, for each section, where its poles and zeros go, relative
//    to the cutoff frequency set by the Main knob.
//
//  * A BANK is an ordered list of kFramesPerBank frames. The morph control
//    (knob X) moves smoothly through the list: 0.0 = frame 0, 1.0 = frame 1,
//    2.5 = halfway between frames 2 and 3, and so on.
//
//  * The DESIGNER (float maths, slow, runs on the RP2040's second core a few
//    thousand times per second) turns knob positions into biquad coefficients.
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
constexpr int   kSections       = 3;         // 3 biquads = 6 poles + 6 zeros
constexpr int   kFramesPerBank  = 5;         // shapes per bank to morph through
constexpr int   kNumBanks       = 2;

// Highest pole/zero frequency we allow, as a fraction of the sample rate.
// Nyquist (the highest representable frequency) is 0.5; we stay a bit below
// it because poles right at Nyquist behave badly.
constexpr float kMaxFreqFraction = 0.45f;    // = 21.6 kHz at 48 kHz
constexpr float kMinFreqHz       = 10.0f;

// ----------------------------------------------------------------------------
//  Describing filter shapes ("frames")
// ----------------------------------------------------------------------------

// What kind of ZEROS a section has. A biquad's numerator (the "zeros" part) is
//
//          N(z) = n0 + n1·z⁻¹ + n2·z⁻²
//
// and each kind below is just a particular choice of (n0, n1, n2):
//
//   kAllPole : (1,  0,  0)   no zeros at all → a pure resonator ("formant")
//   kLow     : (1,  2,  1)   double zero at z = -1 (= Nyquist) → lowpass
//   kHigh    : (1, -2,  1)   double zero at z = +1 (= DC, 0 Hz) → highpass
//   kBand    : (1,  0, -1)   one zero at DC, one at Nyquist → bandpass
//   kNotch   : (1, -2cosθ, 1) zero pair ON the unit circle at angle θ → notch
//
// You can check these by factoring: (1 + z⁻¹)² = 1 + 2z⁻¹ + z⁻², which is
// zero when z = -1. Likewise (1 - z⁻¹)² is zero when z = +1, and
// (1 - z⁻¹)(1 + z⁻¹) = 1 - z⁻² is zero at both.
enum ZeroKind : uint8_t { kAllPole, kLow, kHigh, kBand, kNotch };

// Recipe for ONE biquad section inside ONE frame.
struct SectionSpec
{
	float    poleRatio;  // pole frequency = cutoff × poleRatio
	float    baseQ;      // pole "Q" (sharpness) with the Resonance knob at 0
	float    resQ;       // extra Q added with the Resonance knob at maximum
	ZeroKind zeros;      // which numerator shape (see above)
	float    zeroRatio;  // only used by kNotch: notch frequency = cutoff × this
};

// One complete filter shape = one recipe per section.
struct Frame
{
	const char* name;               // for the host test / documentation only
	SectionSpec sec[kSections];
};

// A Q of 0 is our way of saying "this section does nothing": as you will see
// in placePole(), Q = 0 puts the poles at the very centre of the z-plane
// (radius 0), and with kAllPole zeros the section becomes H(z) = 1.
// Morphing a section from "bypass" to "something" therefore smoothly grows a
// resonance out of nothing — no clicks, no crossfades.

// ----------------------------------------------------------------------------
//  THE BANKS
// ----------------------------------------------------------------------------
//
//  The ORDER of frames matters, because we morph between neighbours. The
//  classic bank was arranged so that each step is a "natural" movement:
//
//    LP → BP : the zeros at Nyquist (1,2,1) slide to (1,0,-1). Halfway, the
//              numerator is (1,1,0) = a single zero at Nyquist: still a gentle
//              lowpass. So the sound goes lowpass → "less low" → bandpass.
//    BP → HP : (1,0,-1) slides to (1,-2,1); halfway is (1,-1,0) = a single zero
//              at DC: a gentle highpass. Bandpass → highpass, smoothly.
//    HP → Notch : the zero pair sits at DC (angle 0) for highpass and at the
//              cutoff angle for notch, so morphing literally sweeps the
//              zeros round the circle from 0 Hz up to the cutoff.
//    Notch → Twin Peaks : the outer notches dissolve while two resonances
//              grow either side of the remaining centre notch. The upper
//              resonance uses highpass zeros: an all-pole resonance at 500 Hz
//              falls 12 dB/octave above itself, and highpass zeros on the 2 kHz
//              resonance rise 12 dB/octave below it, so the two cancel out and
//              both peaks come out roughly equally loud.
//
//  Try reordering them later and listen to what changes!

// Bank 0: "Classic" multimode (switch UP)
//
// For the 6-pole low/high-pass we use the Q values of a 6th-order Butterworth
// filter (0.518, 0.707, 1.932) — the "maximally flat" textbook choice — and
// put the user's resonance on the last (sharpest) section only, which is how
// many analogue multi-pole filters behave: one pole pair does the "singing".
static const Frame kClassicBank[kFramesPerBank] = {
	{ "Lowpass 6-pole", {
		{ 1.00f, 0.518f,  0.0f, kLow,     1.0f },
		{ 1.00f, 0.707f,  0.0f, kLow,     1.0f },
		{ 1.00f, 1.932f, 22.0f, kLow,     1.0f } } },
	{ "Bandpass", {
		{ 0.80f, 0.900f,  0.0f, kBand,    1.0f },
		{ 1.25f, 0.900f,  0.0f, kBand,    1.0f },
		{ 1.00f, 1.500f, 22.0f, kBand,    1.0f } } },
	{ "Highpass 6-pole", {
		{ 1.00f, 0.518f,  0.0f, kHigh,    1.0f },
		{ 1.00f, 0.707f,  0.0f, kHigh,    1.0f },
		{ 1.00f, 1.932f, 22.0f, kHigh,    1.0f } } },
	{ "Triple notch", {
		{ 0.70f, 3.000f,  0.0f, kNotch,   0.70f },
		{ 1.00f, 1.500f, 12.0f, kNotch,   1.00f },
		{ 1.40f, 3.000f,  0.0f, kNotch,   1.40f } } },
	{ "Twin peaks", {
		{ 0.50f, 4.000f, 18.0f, kAllPole, 1.0f },
		{ 1.00f, 2.000f,  6.0f, kNotch,   1.00f },
		{ 2.00f, 4.000f, 18.0f, kHigh,    1.0f } } },
};

// Bank 1: "Vowels" (switch MIDDLE)
//
// Human vowels are recognised mostly by their first three FORMANTS —
// resonant peaks of the vocal tract. Classic speech synthesisers (e.g. the
// Klatt cascade synthesiser) model a vowel as three all-pole resonators in
// series, exactly what we build here. The frequencies are the well-known
// Peterson & Barney (1952) averages for adult male voices, divided by 800 Hz,
// so with the Main knob near 800 Hz you get "natural" vowels; turn it up or
// down and the whole vocal tract shrinks or grows (chipmunk ↔ giant).
constexpr float kVowelRef = 800.0f;
static const Frame kVowelBank[kFramesPerBank] = {
	{ "A (father)", {
		{ 730.0f / kVowelRef, 4.0f, 14.0f, kAllPole, 1.0f },
		{ 1090.0f / kVowelRef, 6.0f, 18.0f, kAllPole, 1.0f },
		{ 2440.0f / kVowelRef, 8.0f, 24.0f, kAllPole, 1.0f } } },
	{ "E (bed)", {
		{ 530.0f / kVowelRef, 4.0f, 14.0f, kAllPole, 1.0f },
		{ 1840.0f / kVowelRef, 6.0f, 18.0f, kAllPole, 1.0f },
		{ 2480.0f / kVowelRef, 8.0f, 24.0f, kAllPole, 1.0f } } },
	{ "I (beet)", {
		{ 270.0f / kVowelRef, 4.0f, 14.0f, kAllPole, 1.0f },
		{ 2290.0f / kVowelRef, 6.0f, 18.0f, kAllPole, 1.0f },
		{ 3010.0f / kVowelRef, 8.0f, 24.0f, kAllPole, 1.0f } } },
	{ "O (bought)", {
		{ 570.0f / kVowelRef, 4.0f, 14.0f, kAllPole, 1.0f },
		{ 840.0f / kVowelRef, 6.0f, 18.0f, kAllPole, 1.0f },
		{ 2410.0f / kVowelRef, 8.0f, 24.0f, kAllPole, 1.0f } } },
	{ "U (boot)", {
		{ 300.0f / kVowelRef, 4.0f, 14.0f, kAllPole, 1.0f },
		{ 870.0f / kVowelRef, 6.0f, 18.0f, kAllPole, 1.0f },
		{ 2240.0f / kVowelRef, 8.0f, 24.0f, kAllPole, 1.0f } } },
};

static const Frame* const kBanks[kNumBanks] = { kClassicBank, kVowelBank };

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

// ---- Where do the poles go? ------------------------------------------------
//
// We borrow the classic analogue description of a resonance: a frequency f
// and a quality factor Q. An analogue 2-pole filter with these has its poles
// (in the s-plane, the analogue cousin of the z-plane) at
//
//      s = −ω/(2Q)  ±  j·ω·√(1 − 1/(4Q²))        where ω = 2πf
//
// The real part says how fast a "ping" dies away; the imaginary part is the
// frequency it rings at. To move to the digital z-plane we use the mapping
// z = e^{sT} (T = 1/fs, one sample period). This is the "impulse invariant" or
// "matched-z" mapping: the digital filter's impulse response is a sampled
// copy of the analogue one. Because e^{a + jb} = e^a · e^{jb}:
//
//      radius  r = e^{−ωT/(2Q)}  = exp(−π f / (Q fs))
//      angle   θ = ωT·√(1 − 1/(4Q²))
//
// Nice properties:
//   * r is ALWAYS < 1 for any positive Q, so the pole is always inside the
//     unit circle, which is exactly the condition for a stable filter.
//   * Q → ∞ makes r → 1 (a pole right on the circle = infinite ringing).
//   * Q → 0 makes r → 0: the poles collapse to the centre and do nothing.
//     That is how a frame can "switch off" a section without a discontinuity.
//
// (For Q < 0.5 a real analogue filter would have two separate real poles; we
// approximate that with a double pole at angle 0. It only happens briefly
// while morphing towards a bypassed section, so the approximation is fine.)
inline Pole placePole(const SectionSpec& s, float cutoffHz, float resonance)
{
	// Resonance is squared so the knob has more travel in the useful
	// low-to-mid range and only the last part of its turn goes "screaming".
	const float q = s.baseQ + s.resQ * resonance * resonance;
	if (q < 1.0e-3f) return Pole{ 0.0f, 0.0f };

	const float w = hzToAngle(cutoffHz * s.poleRatio);  // ωT, in radians
	Pole p;
	p.r = expf(-w / (2.0f * q));
	const float k = 1.0f - 1.0f / (4.0f * q * q);
	p.theta = (k > 0.0f) ? w * sqrtf(k) : 0.0f;
	return p;
}

// ---- Where do the zeros go? ------------------------------------------------
// Returns the numerator (n0, n1, n2) for a section; see ZeroKind above.
inline void placeZeros(const SectionSpec& s, float cutoffHz, float n[3])
{
	switch (s.zeros)
	{
	case kLow:   n[0] = 1.0f; n[1] =  2.0f; n[2] =  1.0f; break;
	case kHigh:  n[0] = 1.0f; n[1] = -2.0f; n[2] =  1.0f; break;
	case kBand:  n[0] = 1.0f; n[1] =  0.0f; n[2] = -1.0f; break;
	case kNotch:
		// A zero pair exactly ON the unit circle (radius 1) at angle θ:
		//   (1 − e^{jθ}z⁻¹)(1 − e^{−jθ}z⁻¹) = 1 − 2cosθ·z⁻¹ + z⁻²
		n[0] = 1.0f; n[1] = -2.0f * cosf(hzToAngle(cutoffHz * s.zeroRatio)); n[2] = 1.0f;
		break;
	case kAllPole:
	default:     n[0] = 1.0f; n[1] =  0.0f; n[2] =  0.0f; break;
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
	float morph;      // 0 .. kFramesPerBank-1 (knob X + CV2)
	float resonance;  // 0 .. 1 (knob Y)
	int   bank;       // 0 = classic, 1 = vowels
};

struct Design
{
	Biquad sec[kSections];
	float  makeup;          // overall output gain applied after the cascade
	float  poleTheta[kSections];  // remembered for plotting / LEDs
};

// The heart of the card. Read this one slowly!
inline void designCascade(const DesignParams& in, Design& out)
{
	// --- 1. Find the two neighbouring frames and how far between them we are.
	float m = in.morph;
	if (m < 0.0f) m = 0.0f;
	if (m > float(kFramesPerBank - 1)) m = float(kFramesPerBank - 1);
	int i = int(m);                          // frame "A" (floor)
	if (i > kFramesPerBank - 2) i = kFramesPerBank - 2;
	const float t = m - float(i);            // 0 = all A, 1 = all B

	int bank = in.bank;
	if (bank < 0) bank = 0;
	if (bank >= kNumBanks) bank = kNumBanks - 1;
	const Frame& A = kBanks[bank][i];
	const Frame& B = kBanks[bank][i + 1];

	for (int s = 0; s < kSections; ++s)
	{
		// --- 2. Morph the POLES in polar form.
		//
		// We compute where the pole pair sits in frame A and in frame B, and
		// then slide between them:
		//   * radius: straight-line (linear) interpolation. Both radii are < 1,
		//     so anything in between is also < 1  ⇒  STABLE at every point of
		//     the morph. This is the key safety property of morphing in the
		//     pole domain. (If you instead interpolated the a1/a2 coefficients
		//     of two arbitrary higher-order filters you could wander outside
		//     the unit circle and the filter would explode.)
		//   * angle: geometric interpolation, θ = θA^(1−t) · θB^t, i.e. linear
		//     in LOG frequency. Our ears hear pitch logarithmically, so a
		//     formant moving from 300 Hz to 2400 Hz passes 850 Hz at halfway
		//     (one "octave-wise" midpoint) instead of 1350 Hz.
		const Pole pa = placePole(A.sec[s], in.cutoffHz, in.resonance);
		const Pole pb = placePole(B.sec[s], in.cutoffHz, in.resonance);
		const float r = pa.r + (pb.r - pa.r) * t;
		const float ta = pa.theta > 1.0e-4f ? pa.theta : 1.0e-4f;
		const float tb = pb.theta > 1.0e-4f ? pb.theta : 1.0e-4f;
		const float theta = ta * powf(tb / ta, t);

		// Expand the pole pair back into denominator coefficients:
		//   (1 − r·e^{jθ}z⁻¹)(1 − r·e^{−jθ}z⁻¹) = 1 − 2r·cosθ·z⁻¹ + r²·z⁻²
		Biquad q;
		q.a1 = -2.0f * r * cosf(theta);
		q.a2 = r * r;

		// --- 3. Morph the ZEROS by interpolating numerator coefficients.
		//
		// Zeros can never make a filter unstable (they only sit in the
		// numerator; the "feedback" is all in the denominator), so we are free
		// to interpolate them the simplest way. Interpolating the coefficients
		// is also the only sensible option when a zero is real-valued and not
		// part of a mirror pair (e.g. bandpass has one at +1 and one at −1).
		float na[3], nb[3];
		placeZeros(A.sec[s], in.cutoffHz, na);
		placeZeros(B.sec[s], in.cutoffHz, nb);
		q.b0 = na[0] + (nb[0] - na[0]) * t;
		q.b1 = na[1] + (nb[1] - na[1]) * t;
		q.b2 = na[2] + (nb[2] - na[2]) * t;

		// --- 4. Normalise this section so its loudest point is gain 1.
		//
		// A resonant pole pair can have a gain of hundreds at its peak (and a
		// lowpass with zeros at Nyquist starts with a DC gain of ~4 / |D(1)|).
		// If we let that through untamed, the integer audio path would
		// overflow. So we find (approximately) the loudest point of this
		// section's response and divide by it. The response of a single
		// biquad can only peak at DC, at Nyquist, or near the pole angle, so
		// those three probes are enough.
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
	for (int k = 0; k < kSections + 2; ++k)
	{
		const float w = (k < kSections) ? out.poleTheta[k] : (k == kSections ? 0.0f : kPi);
		const Probe p(w);
		float mag = 1.0f;
		for (int s = 0; s < kSections; ++s) mag *= magSq(out.sec[s], p);
		if (mag > totalPeak) totalPeak = mag;
		if (k >= kSections && mag > body) body = mag;   // the DC & Nyquist probes
	}
	totalPeak = sqrtf(totalPeak);
	body = sqrtf(body);
	const float safePeak = totalPeak > 1.0e-4f ? totalPeak : 1.0e-4f;
	const float peakiness = 1.0f - body / safePeak;    // 0..1
	float makeup = (1.0f + 3.0f * in.resonance * peakiness) / safePeak;
	if (makeup > 200.0f) makeup = 200.0f;   // don't amplify near-silence into noise
	out.makeup = makeup;
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
	FixedBiquad sec[2][kSections];   // [channel][section]
	int32_t     makeup[2];           // Q16
};

inline int32_t floatToQ28(float x)
{
	if (x >  7.99f) x =  7.99f;
	if (x < -7.99f) x = -7.99f;
	return int32_t(x * float(kCoeffOne));
}

inline void toFixed(const Design& d, FixedBiquad sec[kSections], int32_t& makeup)
{
	for (int s = 0; s < kSections; ++s)
	{
		sec[s].c[0] = floatToQ28(d.sec[s].b0);
		sec[s].c[1] = floatToQ28(d.sec[s].b1);
		sec[s].c[2] = floatToQ28(d.sec[s].b2);
		sec[s].c[3] = floatToQ28(d.sec[s].a1);
		sec[s].c[4] = floatToQ28(d.sec[s].a2);
	}
	makeup = int32_t(d.makeup * float(1 << kMakeupFracBits));
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
	acc += int64_t(q.c[0]) * x;
	acc += int64_t(q.c[1]) * st.x1;
	acc += int64_t(q.c[2]) * st.x2;
	acc -= int64_t(q.c[3]) * st.y1;
	acc -= int64_t(q.c[4]) * st.y2;

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
	// v³ would overflow 64 bits, so drop 8 bits first:
	//   w = v/2⁸  →  v³/F² = w³·2²⁴ / 2⁴⁶ = w³ / 2²²
	const int64_t w = v >> 8;
	const int64_t cube = (w * w * w) >> 22;
	// Multiply by 4/27 WITHOUT dividing: division is very slow on the M0+
	// (no divide instruction for 64-bit numbers), so instead we multiply by
	// round(4/27 × 2¹⁶) = 9709 and shift right by 16. Same answer to within
	// one part in ~20000, at a fraction of the cost.
	return int32_t(v - ((cube * 9709) >> 16));
}

} // namespace zp

#endif
