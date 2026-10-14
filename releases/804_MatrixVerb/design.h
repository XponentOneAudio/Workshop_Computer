// MatrixVerb: the reverb's design maths.
//
// Turns the 112 fader values (and the knobs) into everything the audio
// needs: the feedback matrix, normalised so that it can never gain energy,
// the eight delay lengths, snapped so that every pair is coprime, and each
// line's decay and damping filter.  It runs on core1, next to the USB, as it
// uses floating point and takes a few milliseconds when the matrix changes;
// the audio on core0 only ever sees the finished, safe numbers.
//
// Nothing here touches the hardware, so test/host_test.cpp builds it on a
// computer too.  The web editor (web/index.html) mirrors these mappings.

#ifndef MATRIXVERB_DESIGN_H
#define MATRIXVERB_DESIGN_H

#include <stdint.h>
#include <math.h>
#include <string.h>

namespace mv
{

static constexpr int kLines = 8;
static constexpr int kRate = 24000;          // the network runs at half the sample rate
static constexpr int kLineSize = 8192;       // samples a delay line holds
static constexpr int kMaxLen = 8150;         // longest delay, leaving room for modulation
static constexpr int kMinLen = 16;
static constexpr int kPredelaySize = 4096;

// Pages of eight values, one value per 8mu fader.  The numbers are the
// protocol's and the presets'.
enum Page
{
	PageRow1 = 0,           // 0-7: matrix rows 1-8 (what line r takes from each line)
	PageLength = 8,         // delay lengths
	PageReverb = 9,         // predelay, decay, damping...
	PageLab = 10,           // normalise, blend, drive...
	PageInput = 11,         // how much input each line takes
	PageOutput = 12,        // how much of each line is heard
	PagePan = 13,           // each line's place in the stereo field
	kPages = 14
};
enum Reverb {RvPredelay, RvDecay, RvDamping, RvLowCut, RvTone, RvModDepth, RvWidth, RvMix};
enum Lab {LbNormalise, LbBlend, LbDrive, LbDiffuse, LbModRate, LbInput, LbOutput, LbCeiling};
static constexpr int kNumValues = kPages * kLines;
static constexpr int kNumTemplates = 8;

// Delay length for a fader value at size x1, in samples at 24kHz: 1ms to
// 340ms, evenly spaced in log, 24 * (8150/24)^(v/127) rounded
static const uint16_t kLenTable[128] = {
	24, 25, 26, 28, 29, 30, 32, 33, 35, 36, 38, 40, 42, 44, 46, 48,
	50, 52, 55, 57, 60, 63, 66, 69, 72, 76, 79, 83, 87, 91, 95, 100,
	104, 109, 114, 120, 125, 131, 137, 144, 150, 158, 165, 173, 181, 189, 198, 207,
	217, 227, 238, 249, 261, 273, 286, 299, 313, 328, 344, 360, 377, 394, 413, 432,
	453, 474, 496, 519, 544, 569, 596, 624, 653, 684, 716, 750, 785, 822, 860, 901,
	943, 987, 1034, 1082, 1133, 1186, 1242, 1300, 1361, 1425, 1492, 1562, 1636, 1712, 1793, 1877,
	1965, 2057, 2154, 2255, 2361, 2472, 2588, 2709, 2837, 2970, 3109, 3255, 3408, 3568, 3736, 3911,
	4095, 4287, 4488, 4699, 4920, 5151, 5393, 5646, 5911, 6188, 6479, 6783, 7102, 7435, 7784, 8150,
};

// A random orthogonal matrix (QR of a seeded Gaussian), as fader values
static const uint8_t kRandomOrthogonal[64] = {
	21, 59, 49, 44, 58, 75, 40, 95,
	88, 79, 70, 57, 59, 98, 88, 103,
	62, 91, 34, 45, 80, 94, 62, 32,
	60, 60, 61, 75, 6, 84, 60, 46,
	61, 92, 43, 114, 69, 60, 49, 77,
	94, 35, 18, 57, 58, 54, 56, 74,
	40, 73, 42, 62, 54, 41, 113, 67,
	80, 102, 67, 37, 44, 32, 47, 72,
};

//----------------------------------------------------------------------------
// What a fader value (0-127) means
//----------------------------------------------------------------------------

// Two-sided: -1 (down) to +1 (up), with 63-65 all reading exactly 0
inline float Bipolar(int v)
{
	if (v > 65) return float(v - 65) / 62.0f;
	if (v < 63) return -float(63 - v) / 63.0f;
	return 0.0f;
}

// The fader value nearest a two-sided value
inline int FaderOf(float x)
{
	int v = x > 0 ? int(lroundf(65.0f + x * 62.0f)) : (x < 0 ? int(lroundf(63.0f + x * 63.0f)) : 64);
	return v < 0 ? 0 : (v > 127 ? 127 : v);
}

// Decay time (RT60) in seconds, 0.1s to 30s; the top two positions are
// infinite
inline float DecaySeconds(int v)
{
	if (v >= 126) return INFINITY;
	return 0.1f * powf(300.0f, float(v) / 125.0f);
}
inline float LowCutHz(int v) {return 10.0f * powf(100.0f, float(v) / 127.0f);}
// Input low-pass; off at the top
inline float ToneHz(int v) {return v >= 127 ? 0.0f : 200.0f * exp2f(float(v) * 6.0f / 127.0f);}
inline float PredelayMs(int v) {float u = float(v) / 127.0f; return 170.0f * u * u;}
inline float ModDepthSamples(int v) {float u = float(v) / 127.0f; return 36.0f * u * u;}
inline float ModRateHz(int v) {return 0.05f * powf(100.0f, float(v) / 127.0f);}
inline float DriveOf(int v) {return exp2f(float(v) * 3.0f / 127.0f);}
inline float DiffuseOf(int v) {return 0.7f * float(v) / 127.0f;}
// Input and output level: 0dB at 95, +12dB at the top, off at the bottom
inline float GainDb(int v) {return float(v - 95) * 0.375f;}
inline float CeilingDb(int v) {return -30.0f + float(v) * 29.0f / 127.0f;}

//----------------------------------------------------------------------------
// Defaults and matrix templates
//----------------------------------------------------------------------------

// One of the eight matrix templates as 64 fader values, row by row
//   0 Hadamard     every line feeds every other, equally (densest)
//   1 Householder  every line feeds every other, mostly itself
//   2 Identity     each line feeds only itself: eight separate combs
//   3 Loop         each line feeds the next: one long loop through all eight
//   4 Pairs        four pairs, each a 45 degree rotation
//   5 Random       a random orthogonal matrix
//   6 Negative     each line feeds itself inverted: hollow, odd harmonics
//   7 Zero         no feedback at all: eight single echoes
inline void Template(int n, uint8_t m[64])
{
	for (int r = 0; r < kLines; r++)
	{
		for (int c = 0; c < kLines; c++)
		{
			float x = 0.0f;
			switch (n)
			{
			case 0: x = (__builtin_popcount(unsigned(r & c)) & 1) ? -0.35355339f : 0.35355339f; break;
			case 1: x = (r == c ? 1.0f : 0.0f) - 0.25f; break;
			case 2: x = r == c ? 1.0f : 0.0f; break;
			case 3: x = c == (r + kLines - 1) % kLines ? 1.0f : 0.0f; break;
			case 4:
				if ((r >> 1) == (c >> 1)) x = (r & 1) == 0 && (c & 1) == 1 ? -0.70710678f : 0.70710678f;
				break;
			case 5: m[r * kLines + c] = kRandomOrthogonal[r * kLines + c]; continue;
			case 6: x = r == c ? -1.0f : 0.0f; break;
			default: break;
			}
			m[r * kLines + c] = uint8_t(FaderOf(x));
		}
	}
}

// The settings at power-up (before a preset), and after Reset
inline void Defaults(uint8_t v[kNumValues])
{
	Template(0, v);
	static const uint8_t rest[6][kLines] = {
		{82, 85, 88, 91, 94, 97, 100, 103},       // lengths: 43 to 113ms
		{44, 66, 40, 20, 112, 24, 127, 64},       // reverb
		{127, 127, 0, 48, 40, 87, 95, 118},       // lab
		{112, 16, 112, 112, 16, 112, 16, 16},     // input
		{112, 112, 16, 112, 16, 16, 112, 16},     // output
		{8, 120, 24, 104, 40, 88, 56, 72},        // pan
	};
	memcpy(v + PageLength * kLines, rest, sizeof(rest));
}

//----------------------------------------------------------------------------
// Coprime delay lengths
//----------------------------------------------------------------------------

inline int Gcd(int a, int b)
{
	while (b) {int t = a % b; a = b; b = t;}
	return a;
}

// Snap eight target lengths so that every pair is coprime (no common
// factor), which spreads the network's echoes as evenly as possible.  Lines
// are placed in order, line 1 first: each takes the nearest length to its
// target, trying above before below, that shares no factor with any line
// already placed.  So line 1 always gets its target, and a later line moves
// by a sample or two when it must.
inline void SnapLengths(const int target[kLines], uint16_t out[kLines])
{
	for (int i = 0; i < kLines; i++)
	{
		int t = target[i];
		if (t < kMinLen) t = kMinLen;
		if (t > kMaxLen) t = kMaxLen;
		int best = t;
		for (int k = 0; k < 4 * kMaxLen; k++)
		{
			// t, t+1, t-1, t+2, t-2...
			int n = t + ((k & 1) ? (k + 1) / 2 : -(k / 2));
			if (n < kMinLen || n > kMaxLen) continue;
			bool ok = true;
			for (int j = 0; j < i && ok; j++) ok = Gcd(n, out[j]) == 1;
			if (ok) {best = n; break;}
		}
		out[i] = uint16_t(best);
	}
}

//----------------------------------------------------------------------------
// Matrix normalisation
//----------------------------------------------------------------------------

// The spectral norm of an 8x8 matrix: its largest singular value, the most
// it can amplify any vector.  Found exactly (to float precision) as the
// square root of the largest eigenvalue of A'A, by Jacobi rotations.
inline float SpectralNorm(const float a[64])
{
	float b[64];
	for (int i = 0; i < kLines; i++)
		for (int j = 0; j < kLines; j++)
		{
			float s = 0.0f;
			for (int k = 0; k < kLines; k++) s += a[k * kLines + i] * a[k * kLines + j];
			b[i * kLines + j] = s;
		}
	for (int sweep = 0; sweep < 16; sweep++)
	{
		float off = 0.0f, diag = 0.0f;
		for (int i = 0; i < kLines; i++)
		{
			diag += b[i * kLines + i] * b[i * kLines + i];
			for (int j = i + 1; j < kLines; j++) off += b[i * kLines + j] * b[i * kLines + j];
		}
		if (off <= 1e-14f * diag || off < 1e-30f) break;
		for (int p = 0; p < kLines - 1; p++)
		{
			for (int q = p + 1; q < kLines; q++)
			{
				float bpq = b[p * kLines + q];
				if (fabsf(bpq) < 1e-30f) continue;
				float theta = (b[q * kLines + q] - b[p * kLines + p]) / (2.0f * bpq);
				float t = (theta >= 0 ? 1.0f : -1.0f) / (fabsf(theta) + sqrtf(theta * theta + 1.0f));
				float c = 1.0f / sqrtf(t * t + 1.0f), s = t * c;
				for (int k = 0; k < kLines; k++)
				{
					float bkp = b[k * kLines + p], bkq = b[k * kLines + q];
					b[k * kLines + p] = c * bkp - s * bkq;
					b[k * kLines + q] = s * bkp + c * bkq;
				}
				for (int k = 0; k < kLines; k++)
				{
					float bpk = b[p * kLines + k], bqk = b[q * kLines + k];
					b[p * kLines + k] = c * bpk - s * bqk;
					b[q * kLines + k] = s * bpk + c * bqk;
				}
			}
		}
	}
	float mx = 0.0f;
	for (int i = 0; i < kLines; i++) if (b[i * kLines + i] > mx) mx = b[i * kLines + i];
	return sqrtf(mx);
}

//----------------------------------------------------------------------------
// The design: everything the audio needs, ready to use
//----------------------------------------------------------------------------

struct Controls
{
	uint8_t v[kNumValues];
	int32_t decayOct;    // Main, CV In 1 and tilt: decay multiplier, Q12 octaves
	int32_t sizeOct;     // X and CV In 2: size multiplier, Q12 octaves
	int32_t dampOffset;  // Y and tilt: added to damping, Q12 of its range
};

struct Design
{
	int16_t mat[64];          // Q14: the matrix, normalised so it never gains
	int16_t matFreeze[64];    // Q14: the same, normalised to lose (almost) nothing
	uint16_t len[kLines];     // samples at 24kHz, pairwise coprime
	int32_t g[kLines];        // Q15 decay gain at low frequencies, 32768 = 1
	int32_t damp[kLines];     // Q10 damping one-pole coefficient, 1024 = none
	int16_t inL[kLines], inR[kLines];     // Q14 input to each line
	int16_t outL[kLines], outR[kLines];   // Q14 each line to the output
	int32_t hpA;              // Q16 low cut one-pole coefficient
	int32_t toneA;            // Q16 input low-pass coefficient, 65536 = off
	int32_t predelay;         // Q8 samples
	int32_t modDepth;         // Q8 samples
	uint32_t modInc[kLines];  // modulation phase step a control tick
	int32_t drive;            // Q8, 256 = x1
	int32_t invDrive;         // Q16
	int32_t diffuse;          // Q15 allpass coefficient
	int32_t inGain;           // Q12
	int32_t outGain;          // Q12
	int32_t ceiling;          // Q15 of full scale
	int32_t dryGain, wetGain; // Q12
	// For the web editor
	int32_t sigmaMilli;       // spectral norm before normalising, x1000
	int32_t scaleMilli;       // what the matrix was multiplied by, x1000
	int32_t decayCs;          // RT60 in hundredths of a second, -1 for infinite
};

// Control ticks a second, at which the modulation phases are stepped
static constexpr float kControlRate = 48000.0f / 32.0f;

class Designer
{
public:
	// Work out the design for these controls.  The matrix (the slow part) is
	// only worked out again when its faders change.
	void Compute(const Controls &c, Design &d)
	{
		const uint8_t *rv = c.v + PageReverb * kLines;
		const uint8_t *lb = c.v + PageLab * kLines;

		// Matrix: blended with the identity, then scaled so its spectral
		// norm is at most 1 (never gains), or exactly 1 with Normalise up
		// (loses nothing, so the decay is all down to the Decay fader).
		// Freeze always uses the lossless version.  0.999 leaves room for
		// rounding to Q14 (at most 8 x 0.5/16384 = 0.00025).
		bool matChanged = !haveMat || memcmp(c.v, lastMat, 64) != 0
			|| lb[LbBlend] != lastBlend || lb[LbNormalise] != lastNorm;
		if (matChanged)
		{
			haveMat = true;
			memcpy(lastMat, c.v, 64);
			lastBlend = lb[LbBlend];
			lastNorm = lb[LbNormalise];
			float blend = float(lastBlend) / 127.0f;
			float a[64];
			for (int i = 0; i < 64; i++)
			{
				a[i] = blend * Bipolar(c.v[i]) + ((i / kLines == i % kLines) ? 1.0f - blend : 0.0f);
			}
			float sigma = SpectralNorm(a);
			float n = float(lastNorm) / 127.0f;
			float k = 1.0f, kf = 1.0f;
			if (sigma > 1e-6f)
			{
				float down = sigma > 1.0f ? 1.0f / sigma : 1.0f;
				k = (1.0f - n) * down + n / sigma;
				kf = 1.0f / sigma;
			}
			k *= 0.999f;
			kf *= 0.999f;
			for (int i = 0; i < 64; i++)
			{
				mat[i] = int16_t(lroundf(a[i] * k * 16384.0f));
				matFreeze[i] = int16_t(lroundf(a[i] * kf * 16384.0f));
			}
			sigmaMilli = int32_t(lroundf(sigma * 1000.0f));
			scaleMilli = int32_t(lroundf(k * 1000.0f));
		}
		memcpy(d.mat, mat, sizeof(mat));
		memcpy(d.matFreeze, matFreeze, sizeof(matFreeze));
		d.sigmaMilli = sigmaMilli;
		d.scaleMilli = scaleMilli;

		// Lengths, times the size, then made coprime
		float size = exp2f(float(c.sizeOct) / 4096.0f);
		int target[kLines];
		for (int i = 0; i < kLines; i++)
		{
			target[i] = int(lroundf(float(kLenTable[c.v[PageLength * kLines + i] & 127]) * size));
		}
		SnapLengths(target, d.len);

		// Decay and damping, per line (Jot's method): each line's gain is
		// set by its length, so that every line dies away at the same rate
		// whatever its length, and a one-pole low-pass gives the high
		// frequencies a shorter decay than the lows.
		float rt = DecaySeconds(rv[RvDecay]);
		if (rt < INFINITY)
		{
			rt *= exp2f(float(c.decayOct) / 4096.0f);
			if (rt < 0.05f) rt = 0.05f;
			if (rt > 100.0f) rt = 100.0f;
			d.decayCs = int32_t(lroundf(rt * 100.0f));
		}
		else d.decayCs = -1;
		float dm = float(rv[RvDamping]) / 127.0f + float(c.dampOffset) / 4096.0f;
		dm = dm < 0.0f ? 0.0f : (dm > 1.0f ? 1.0f : dm);
		float ratio = exp2f(-5.0f * dm);  // high-frequency decay time as a share of the low
		for (int i = 0; i < kLines; i++)
		{
			float len = float(d.len[i]);
			float gdc = rt < INFINITY ? expf(-6.9077553f * len / (float(kRate) * rt)) : 1.0f;
			d.g[i] = int32_t(lroundf(gdc * 32768.0f));
			if (dm <= 0.0f)
			{
				d.damp[i] = 1024;
			}
			else
			{
				float rthf = (rt < INFINITY ? rt : 100.0f) * ratio;
				float gny = expf(-6.9077553f * len / (float(kRate) * rthf));
				float p = (gdc - gny) / (gdc + gny);
				int32_t a = int32_t(lroundf((1.0f - p) * 1024.0f));
				d.damp[i] = a < 1 ? 1 : (a > 1024 ? 1024 : a);
			}
		}

		// Each line's input, output and place: a line takes its input from
		// where it's panned, and plays back there
		float width = float(rv[RvWidth]) / 127.0f;
		for (int i = 0; i < kLines; i++)
		{
			float pan = Bipolar(c.v[PagePan * kLines + i]) * width;
			float b = Bipolar(c.v[PageInput * kLines + i]);
			float o = Bipolar(c.v[PageOutput * kLines + i]) * 0.35f;
			d.inL[i] = int16_t(lroundf(b * (1.0f - pan) * 0.5f * 16384.0f));
			d.inR[i] = int16_t(lroundf(b * (1.0f + pan) * 0.5f * 16384.0f));
			d.outL[i] = int16_t(lroundf(o * (pan > 0 ? 1.0f - pan : 1.0f) * 16384.0f));
			d.outR[i] = int16_t(lroundf(o * (pan < 0 ? 1.0f + pan : 1.0f) * 16384.0f));
		}

		d.hpA = CoefQ16(LowCutHz(rv[RvLowCut]));
		float tone = ToneHz(rv[RvTone]);
		d.toneA = tone > 0.0f ? CoefQ16(tone) : 65536;
		d.predelay = int32_t(lroundf(PredelayMs(rv[RvPredelay]) * 24.0f * 256.0f));
		d.modDepth = int32_t(lroundf(ModDepthSamples(rv[RvModDepth]) * 256.0f));
		// Each line's modulation at its own rate, so they never line up
		static const float kSpread[kLines] = {1.0f, 1.17f, 0.83f, 1.31f, 0.71f, 1.09f, 0.93f, 1.23f};
		float hz = ModRateHz(lb[LbModRate]);
		for (int i = 0; i < kLines; i++)
		{
			d.modInc[i] = uint32_t(hz * kSpread[i] / kControlRate * 4294967296.0f);
		}
		float drive = DriveOf(lb[LbDrive]);
		d.drive = int32_t(lroundf(drive * 256.0f));
		d.invDrive = int32_t(lroundf(65536.0f / drive));
		d.diffuse = int32_t(lroundf(DiffuseOf(lb[LbDiffuse]) * 32768.0f));
		d.inGain = lb[LbInput] == 0 ? 0 : int32_t(lroundf(4096.0f * powf(10.0f, GainDb(lb[LbInput]) / 20.0f)));
		d.outGain = lb[LbOutput] == 0 ? 0 : int32_t(lroundf(4096.0f * powf(10.0f, GainDb(lb[LbOutput]) / 20.0f)));
		d.ceiling = int32_t(lroundf(32767.0f * powf(10.0f, CeilingDb(lb[LbCeiling]) / 20.0f)));

		// Mix: dry full below the centre, wet full above
		int32_t m = rv[RvMix] >= 127 ? 4096 : (rv[RvMix] * 4096) / 127;
		d.dryGain = m <= 2048 ? 4096 : (4096 - m) * 2;
		d.wetGain = m >= 2048 ? 4096 : m * 2;
	}

	// One-pole coefficient (Q16) for a cutoff in Hz at 24kHz
	static int32_t CoefQ16(float hz)
	{
		float a = 1.0f - expf(-6.2831853f * hz / float(kRate));
		return int32_t(lroundf(a * 65536.0f));
	}

private:
	bool haveMat = false;
	uint8_t lastMat[64] = {};
	uint8_t lastBlend = 0, lastNorm = 0;
	int16_t mat[64] = {}, matFreeze[64] = {};
	int32_t sigmaMilli = 0, scaleMilli = 0;
};

} // namespace mv

#endif
