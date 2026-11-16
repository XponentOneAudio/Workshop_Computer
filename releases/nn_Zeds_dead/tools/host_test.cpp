// ============================================================================
//  host_test.cpp — run the Zeds Dead filter maths on a normal computer
// ============================================================================
//
//  Build & run (from this tools/ directory):
//
//      g++ -O2 -std=c++17 -I.. host_test.cpp -o host_test && ./host_test
//
//  What it does:
//    1. STABILITY: designs the filter at thousands of knob combinations and
//       checks every pole radius is < 1 (inside the unit circle).
//    2. ACCURACY: feeds sine waves through the INTEGER (fixed-point) audio path
//       and compares the measured gain with what the FLOAT design predicts.
//       This catches mistakes in the fixed-point conversion.
//    3. TORTURE: sweeps the morph/cutoff/resonance wildly while feeding noise
//       through the integer path, the way a fast LFO on the CV inputs would,
//       and checks the output never blows up.
//    4. PICTURES: writes SVG plots of the frequency response as the morph
//       control sweeps through each bank (open them in a web browser).
//
//  This is a learning tool as much as a test: try editing the frames in
//  zplane.h, re-run, and look at how the pictures change.
// ============================================================================

#include "zplane.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

using namespace zp;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

// Magnitude (in dB) of the whole designed cascade at frequency hz, including
// the makeup gain — i.e. what the float maths says the card should do.
static float designDb(const Design& d, float hz)
{
	Probe p(2.0f * kPi * hz / kSampleRate);
	float m = d.makeup * d.makeup;
	for (int s = 0; s < kSections; ++s) m *= magSq(d.sec[s], p);
	return 10.0f * log10f(m + 1e-20f);
}

// ---------------------------------------------------------------------------
// 1. Stability sweep
// ---------------------------------------------------------------------------
static void testStability()
{
	int designs = 0;
	float worstR = 0.0f;
	for (int bank = 0; bank < kNumBanks; ++bank)
		for (float morph = 0.0f; morph <= 4.0f; morph += 0.05f)
			for (float res = 0.0f; res <= 1.0f; res += 0.1f)
				for (float hz = 20.0f; hz < 20000.0f; hz *= 1.25f)
				{
					Design d;
					designCascade({ hz, morph, res, bank }, d);
					++designs;
					for (int s = 0; s < kSections; ++s)
					{
						// a2 = r², so r = sqrt(a2)
						float r = sqrtf(d.sec[s].a2);
						if (r > worstR) worstR = r;
						CHECK(r < 1.0f, "unstable pole r=%f bank=%d morph=%f res=%f hz=%f", r, bank, morph, res, hz);
						CHECK(std::isfinite(d.sec[s].b0) && std::isfinite(d.makeup), "non-finite coefficient");
					}
				}
	std::printf("[stability] %d designs checked, largest pole radius %.6f\n", designs, worstR);
}

// ---------------------------------------------------------------------------
// The integer audio path, exactly as main.cpp runs it (one channel).
// ---------------------------------------------------------------------------
struct FixedChannel
{
	FixedBiquad cur[kSections];
	int32_t makeup = 1 << kMakeupFracBits;
	BiquadState st[kSections];

	void snapTo(const FixedBiquad tgt[kSections], int32_t mk)
	{
		for (int s = 0; s < kSections; ++s) cur[s] = tgt[s];
		makeup = mk;
	}
	void smoothTo(const FixedBiquad tgt[kSections], int32_t mk)
	{
		for (int s = 0; s < kSections; ++s)
			for (int k = 0; k < 5; ++k)
				cur[s].c[k] += (tgt[s].c[k] - cur[s].c[k]) >> 5;
		makeup += (mk - makeup) >> 5;
	}
	int16_t process(int16_t in)
	{
		int32_t x = int32_t(in) << kSampleShift;
		for (int s = 0; s < kSections; ++s) x = runBiquad(cur[s], st[s], x);
		int64_t y = (int64_t(x) * makeup) >> kMakeupFracBits;
		if (y >  (int64_t(1) << 30)) y =  (int64_t(1) << 30);
		if (y < -(int64_t(1) << 30)) y = -(int64_t(1) << 30);
		int32_t out = softClip(int32_t(y)) >> kSampleShift;
		if (out > 2047) out = 2047;
		if (out < -2048) out = -2048;
		return int16_t(out);
	}
};

// ---------------------------------------------------------------------------
// 2. Fixed-point vs float accuracy
// ---------------------------------------------------------------------------
static void testAccuracy()
{
	const DesignParams cases[] = {
		{ 1000.0f, 0.0f, 0.0f, 0 },   // lowpass
		{ 1000.0f, 1.0f, 0.3f, 0 },   // bandpass
		{ 1000.0f, 2.0f, 0.0f, 0 },   // highpass
		{ 1000.0f, 3.0f, 0.0f, 0 },   // notch
		{ 1000.0f, 3.5f, 0.5f, 0 },   // between notch & twin peaks
		{ 800.0f,  0.0f, 0.2f, 1 },   // vowel A
		{ 800.0f,  2.5f, 0.2f, 1 },   // between I and O
		{ 60.0f,   0.0f, 0.8f, 0 },   // low, resonant lowpass (hardest case)
	};
	const float testHz[] = { 100.0f, 300.0f, 700.0f, 1000.0f, 1500.0f, 3000.0f, 8000.0f };
	float worst = 0.0f;
	for (const auto& c : cases)
	{
		Design d; designCascade(c, d);
		FixedBiquad fq[kSections]; int32_t mk; toFixed(d, fq, mk);
		for (float hz : testHz)
		{
			const float predDb = designDb(d, hz);
			// Choose an amplitude that should give a ~-12 dBFS output so the soft
			// clipper stays out of the way, but not below ~-60 dBFS in.
			float amp = 2047.0f * powf(10.0f, (-12.0f - predDb) / 20.0f);
			if (amp > 1500.0f) amp = 1500.0f;
			if (amp < 2.0f) continue;   // response too far down to measure with 12-bit input
			FixedChannel ch; ch.snapTo(fq, mk);
			const int N = 48000, settle = 24000;
			double sumIn = 0, sumOut = 0;
			for (int n = 0; n < N; ++n)
			{
				float s = amp * sinf(2.0f * kPi * hz * float(n) / kSampleRate);
				int16_t in = int16_t(lrintf(s));
				int16_t out = ch.process(in);
				if (n >= settle) { sumIn += double(in) * in; sumOut += double(out) * out; }
			}
			float measDb = 10.0f * float(log10((sumOut + 1e-9) / sumIn));
			float outDbfs = 10.0f * float(log10(sumOut / (N - settle) / (2047.0 * 2047.0 / 2.0) + 1e-12));
			if (outDbfs < -55.0f) continue;   // too close to 12-bit output noise floor to judge
			float err = fabsf(measDb - predDb);
			if (err > worst) worst = err;
			CHECK(err < 1.0f, "fixed vs float mismatch %.2f dB (pred %.2f meas %.2f) hz=%g morph=%g bank=%d cutoff=%g",
			      err, predDb, measDb, hz, c.morph, c.bank, c.cutoffHz);
		}
	}
	std::printf("[accuracy] worst fixed-vs-float gain error %.3f dB\n", worst);
}

// ---------------------------------------------------------------------------
// 3. Torture test: fast modulation + noise
// ---------------------------------------------------------------------------
static void testTorture()
{
	FixedChannel ch;
	uint32_t rng = 12345;
	int peak = 0;
	long clipped = 0;
	const int N = 48000 * 20;  // 20 seconds
	Design d; FixedBiquad tgt[kSections]; int32_t mk = 1 << kMakeupFracBits;
	for (int n = 0; n < N; ++n)
	{
		// Redesign every 16 samples, like core 1 might (it is probably faster).
		if ((n & 15) == 0)
		{
			float tsec = float(n) / kSampleRate;
			DesignParams p;
			p.cutoffHz  = 20.0f * powf(2.0f, 10.0f * (0.5f + 0.5f * sinf(2.0f * kPi * 3.1f * tsec)));
			p.morph     = 2.0f + 2.0f * sinf(2.0f * kPi * 7.3f * tsec);
			p.resonance = 0.5f + 0.5f * sinf(2.0f * kPi * 0.37f * tsec);
			p.bank      = (n / (48000 * 5)) & 1;   // switch banks every 5 s
			designCascade(p, d);
			toFixed(d, tgt, mk);
		}
		ch.smoothTo(tgt, mk);
		rng = rng * 1664525u + 1013904223u;
		int16_t in = int16_t(int32_t(rng >> 20) - 2048);   // full-scale white noise
		int16_t out = ch.process(in);
		int a = abs(out);
		if (a > peak) peak = a;
		if (a >= 2047) ++clipped;
	}
	// Output is bounded by the soft clipper anyway; the interesting check is
	// that the internal states settle again afterwards (no stuck oscillation).
	for (int n = 0; n < 48000; ++n) ch.process(0);
	int tail = 0;
	for (int n = 0; n < 4800; ++n) { int a = abs(ch.process(0)); if (a > tail) tail = a; }
	CHECK(tail <= 1, "filter still ringing %d after 1 s of silence (limit cycle?)", tail);
	std::printf("[torture] 20 s fast modulation: peak %d, %.3f%% samples at full scale, silent tail %d\n",
	            peak, 100.0 * double(clipped) / N, tail);
}

// ---------------------------------------------------------------------------
// 4. SVG plots
// ---------------------------------------------------------------------------
static void plotBank(int bank, float cutoff, float res, const char* filename)
{
	const int W = 900, H = 500, L = 60, R = 20, T = 40, B = 50;
	const float fLo = 20.0f, fHi = 20000.0f, dbLo = -60.0f, dbHi = 18.0f;
	auto X = [&](float hz) { return L + (W - L - R) * log10f(hz / fLo) / log10f(fHi / fLo); };
	auto Y = [&](float db) { if (db < dbLo) db = dbLo; if (db > dbHi) db = dbHi;
	                         return T + (H - T - B) * (dbHi - db) / (dbHi - dbLo); };

	FILE* f = std::fopen(filename, "w");
	if (!f) return;
	std::fprintf(f, "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' font-family='sans-serif' font-size='12'>\n", W, H);
	std::fprintf(f, "<rect width='100%%' height='100%%' fill='white'/>\n");
	std::fprintf(f, "<text x='%d' y='22' font-size='15'>Bank %d (%s): morph sweep 0→4, cutoff %.0f Hz, resonance %.1f</text>\n",
	             L, bank, bank == 0 ? "classic" : "vowels", cutoff, res);
	// grid
	for (float hz : { 20.f, 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f, 20000.f })
		std::fprintf(f, "<line x1='%.1f' x2='%.1f' y1='%d' y2='%d' stroke='#ddd'/><text x='%.1f' y='%d' text-anchor='middle'>%g</text>\n",
		             X(hz), X(hz), T, H - B, X(hz), H - B + 16, hz);
	for (float db = dbLo; db <= dbHi; db += 12.0f)
		std::fprintf(f, "<line x1='%d' x2='%d' y1='%.1f' y2='%.1f' stroke='#ddd'/><text x='%d' y='%.1f' text-anchor='end'>%+.0f dB</text>\n",
		             L, W - R, Y(db), Y(db), L - 4, Y(db) + 4, db);
	// curves: morph from 0 to 4 in 0.25 steps; whole-number frames drawn thicker
	for (int k = 0; k <= 16; ++k)
	{
		float morph = float(k) * 0.25f;
		Design d; designCascade({ cutoff, morph, res, bank }, d);
		// colour slides from blue (frame 0) through to red (frame 4)
		int hue = int(240.0f - 240.0f * morph / 4.0f);
		std::fprintf(f, "<polyline fill='none' stroke='hsl(%d,75%%,45%%)' stroke-width='%s' stroke-opacity='%s' points='",
		             hue, (k % 4 == 0) ? "2.5" : "1", (k % 4 == 0) ? "1" : "0.55");
		for (float hz = fLo; hz <= fHi; hz *= 1.02f) std::fprintf(f, "%.1f,%.1f ", X(hz), Y(designDb(d, hz)));
		std::fprintf(f, "'/>\n");
		if (k % 4 == 0)
			std::fprintf(f, "<text x='%d' y='%d' fill='hsl(%d,75%%,40%%)'>%d: %s</text>\n",
			             W - R - 150, T + 16 + 16 * (k / 4), hue, k / 4, kBanks[bank][k / 4].name);
	}
	std::fprintf(f, "</svg>\n");
	std::fclose(f);
	std::printf("[plot] wrote %s\n", filename);
}

int main()
{
	testStability();
	testAccuracy();
	testTorture();
	plotBank(0, 1000.0f, 0.0f, "response_classic_res0.svg");
	plotBank(0, 1000.0f, 0.6f, "response_classic_res06.svg");
	plotBank(1, 800.0f, 0.3f, "response_vowels.svg");
	std::printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", failures);
	return failures ? 1 : 0;
}
