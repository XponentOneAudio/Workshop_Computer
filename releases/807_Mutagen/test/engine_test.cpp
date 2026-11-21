// Tests of the Mutagen engine (mutagen.h) and the 8mu handling (ui.h) on a
// PC: pitch, each mutant against a floating-point model of what it should
// do, the band-limiting (an FFT of the output, measuring what isn't a
// harmonic), and that nothing overflows anywhere in the controls' ranges.
// Built and run by test/run_tests.sh.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <complex>
#include <algorithm>

#include "../mutagen.h"
#include "../ui.h"

using namespace mutagen;

static int checks = 0, failures = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Tables gTables;

// A fresh engine with everything at its defaults, then mutants off
struct Rig
{
	Engine e;
	Inputs in;
	Rig()
	{
		e.Begin(&gTables);
	}
	void Set(int pg, int f, int32_t v) {e.values[pg][f] = uint16_t(v);}
	void Plain()
	{
		// No modulation, both mutants all dry
		for (int s = 0; s < 2; s++)
		{
			int pg = s ? PageMut2 : PageMut1;
			Set(pg, MLfo1, 2048);
			Set(pg, MLfo2, 2048);
			Set(pg, MDepth, 0);
			Set(pg, MMix, 0);
			Set(pg, MWindow, 0);
			Set(pg, MFeedback, 0);
			Set(pg, MRatio, ValueForRatio(1.0f));
		}
		for (int f = 0; f < kFaders; f++) Set(PageMod, f, 2048);
		Set(PageOsc, OLevel, 4095);
		Set(PageOsc, OOut2, ValueForZone(Out2Dry, kOut2Modes));
	}
	void Mutant(int s, int mode, float depth, float ratio = 1.0f, float mix = 1.0f)
	{
		int pg = s ? PageMut2 : PageMut1;
		Set(pg, MMode, ValueForZone(mode, kModes));
		Set(pg, MDepth, int32_t(lroundf(depth * 4095.0f)));
		Set(pg, MRatio, ValueForRatio(ratio));
		Set(pg, MMix, int32_t(lroundf(mix * 4095.0f)));
	}
	void Wave(int w) {Set(PageOsc, OWave, (w * 4095) / 3);}
	// Settle (smoothing, glide), then record
	std::vector<double> Run(int n, int settle = 9600, int which = 1)
	{
		for (int i = 0; i < settle; i++) e.Tick(in);
		std::vector<double> v(n);
		for (int i = 0; i < n; i++)
		{
			e.Tick(in);
			v[i] = which == 1 ? e.out1 : e.out2;
		}
		return v;
	}
};

// The Main knob position for a pitch in Hz (C1 to C7 round the knob)
static int32_t KnobForHz(double hz)
{
	double oct = log2(hz / 16.3516) - 1.0;
	return int32_t(lround(oct / 6.0 * 4095.0));
}

//----------------------------------------------------------------------------
// FFT, for measuring harmonics and what isn't
//----------------------------------------------------------------------------

static void Fft(std::vector<std::complex<double>> &a)
{
	size_t n = a.size();
	for (size_t i = 1, j = 0; i < n; i++)
	{
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) std::swap(a[i], a[j]);
	}
	for (size_t len = 2; len <= n; len <<= 1)
	{
		double ang = -2 * M_PI / double(len);
		std::complex<double> wl(cos(ang), sin(ang));
		for (size_t i = 0; i < n; i += len)
		{
			std::complex<double> w(1);
			for (size_t j = 0; j < len / 2; j++)
			{
				std::complex<double> u = a[i + j], v = a[i + j + len / 2] * w;
				a[i + j] = u + v;
				a[i + j + len / 2] = u - v;
				w *= wl;
			}
		}
	}
}

// Power spectrum of 65536 samples, Blackman-Harris windowed
static std::vector<double> Spectrum(const std::vector<double> &x)
{
	const size_t n = 65536;
	std::vector<std::complex<double>> a(n);
	for (size_t i = 0; i < n; i++)
	{
		double t = 2 * M_PI * double(i) / double(n);
		double w = 0.35875 - 0.48829 * cos(t) + 0.14128 * cos(2 * t) - 0.01168 * cos(3 * t);
		a[i] = x[i] * w;
	}
	Fft(a);
	std::vector<double> p(n / 2);
	for (size_t i = 0; i < n / 2; i++) p[i] = std::norm(a[i]);
	return p;
}

// The strongest component below 20kHz that is not a multiple of f (beyond
// a few bins either side, which the window spreads each harmonic over), in
// dB relative to the strongest harmonic.  With sub = 4, multiples of f/4
// count as harmonics too (the mutants' ratios below 1 make those).
static double WorstAlias(const std::vector<double> &p, double f, int sub = 1, double fmax = 18000.0)
{
	const double binHz = 48000.0 / 65536.0;
	double strongest = 1e-30, worst = 1e-30;
	double fundamental = f / sub;
	for (size_t i = 3; i < p.size(); i++)
	{
		double hz = double(i) * binHz;
		if (hz > fmax) break;
		double h = hz / fundamental;
		double near = fabs(h - round(h)) * fundamental / binHz;   // bins from a harmonic
		if (near <= 6) strongest = std::max(strongest, p[i]);
		else worst = std::max(worst, p[i]);
	}
	return 10 * log10(worst / strongest);
}

// The pitch the engine is playing, in Hz
static double EngineHz(const Engine &e) {return 16.3516 * pow(2.0, double(e.pitch) / 65536.0);}

// The pitch, from the spacing of rising zero crossings
static double MeasureHz(const std::vector<double> &x)
{
	int first = -1, count = 0;
	double firstT = 0, lastT = 0;
	for (size_t i = 1; i < x.size(); i++)
	{
		if (x[i - 1] < 0 && x[i] >= 0)
		{
			double t = double(i - 1) + (-x[i - 1]) / (x[i] - x[i - 1]);
			if (first < 0) {first = int(i); firstT = t;}
			lastT = t;
			count++;
		}
	}
	if (count < 2) return 0;
	return 48000.0 * (count - 1) / (lastT - firstT);
}

//----------------------------------------------------------------------------
// Tests
//----------------------------------------------------------------------------

static void TestTables()
{
	int32_t peak = 0;
	for (int i = 0; i < kTableSize; i++) peak = std::max(peak, int32_t(abs(gTables.sine[i])));
	CHECK(peak == 32767, "sine peak %d", peak);
	// Every level of every wave within range, and the top level of each a
	// pure sine (one harmonic) apart from sign and level
	for (int w = 1; w < kWaves; w++)
		for (int L = 0; L < kLevels; L++)
		{
			const int16_t *t = gTables.Get(w, L);
			int32_t mx = 0;
			for (int i = 0; i <= kTableSize; i++) mx = std::max(mx, int32_t(abs(t[i])));
			CHECK(mx <= 32000 && mx > 8000, "wave %d level %d peak %d", w, L, mx);
			CHECK(t[kTableSize] == t[0], "wave %d level %d guard", w, L);
		}
	// The saw's jump is at mid-cycle, like the square's falling edge
	const int16_t *saw = gTables.Get(WSaw, 0), *sq = gTables.Get(WSquare, 0);
	CHECK(saw[kTableSize / 2 - 8] > 25000 && saw[kTableSize / 2 + 8] < -25000, "saw jump at mid-cycle (%d, %d)", saw[kTableSize / 2 - 8], saw[kTableSize / 2 + 8]);
	CHECK(sq[kTableSize / 4] > 25000 && sq[3 * kTableSize / 4] < -25000, "square high then low");
}

static void TestHelpers()
{
	// Bipolar: dead centre, ends
	CHECK(Bipolar(2048) == 0 && Bipolar(2048 + 40) == 0 && Bipolar(2048 - 40) == 0, "bipolar centre");
	CHECK(Bipolar(4095) == 32767 && Bipolar(0) == -32767, "bipolar ends %d %d", Bipolar(4095), Bipolar(0));
	for (int p = -100; p <= 100; p += 25)
	{
		int32_t b = Bipolar(BipolarValue(p));
		CHECK(abs(b - p * 32767 / 100) < 400, "BipolarValue(%d) -> %d", p, b);
	}
	// Zones
	for (int n : {int(kModes), int(kShapes), int(kOut2Modes), kSemiZones})
		for (int z = 0; z < n; z++) CHECK(ZoneOf(ValueForZone(z, n), n) == z, "zone %d of %d", z, n);
	// Ratios: the detents are exact, the ends are 1/4 and 16
	CHECK(RatioFromLog(RatioLogFromValue(ValueForRatio(1.0f))) == 256, "ratio 1");
	CHECK(RatioFromLog(RatioLogFromValue(ValueForRatio(2.0f))) == 512, "ratio 2");
	CHECK(RatioFromLog(RatioLogFromValue(ValueForRatio(3.0f))) == 768, "ratio 3");
	CHECK(RatioFromLog(RatioLogFromValue(0)) == 64, "ratio 1/4");
	CHECK(RatioFromLog(RatioLogFromValue(4095)) == 4096, "ratio 16 (%u)", RatioFromLog(RatioLogFromValue(4095)));
	// Near a whole number snaps to it
	int32_t v3 = ValueForRatio(3.0f);
	CHECK(RatioFromLog(RatioLogFromValue(v3 + 5)) == 768 && RatioFromLog(RatioLogFromValue(v3 - 5)) == 768, "ratio 3 detent");
	// Pitch: C0 and octaves above it
	for (int oct = 0; oct <= 10; oct++)
	{
		double hz = double(IncFromPitch(oct * kOct)) * 96000.0 / 1073741824.0;
		double want = 16.3516 * pow(2.0, oct);
		CHECK(fabs(hz / want - 1) < 1e-4, "pitch oct %d: %f Hz, want %f", oct, hz, want);
	}
	// LFO rates
	double lo = double(LfoIncFromValue(0)) * 3000.0 / 4294967296.0, hi = double(LfoIncFromValue(4095)) * 3000.0 / 4294967296.0;
	CHECK(fabs(lo - 0.02) < 0.001 && fabs(hi - 40) < 1, "LFO range %f-%f Hz", lo, hi);
	double at = double(LfoIncFromValue(ValueForLfoHz(0.5f))) * 3000.0 / 4294967296.0;
	CHECK(fabs(at - 0.5) < 0.01, "LFO 0.5Hz: %f", at);
	for (int z = 0; z < kSyncs; z++) CHECK(LfoSyncOf(ValueForSync(z)) == z, "sync choice %d", z);
	CHECK(LfoSyncOf(0) == 0 && LfoSyncOf(4095) == -1, "sync at the bottom, free at the top");
}

static void TestPitch()
{
	// The Main knob alone, and with CV In 1
	for (double hz : {65.41, 261.63, 1046.5})
	{
		Rig r;
		r.Plain();
		r.Wave(WSine);
		r.e.pitchKnob = KnobForHz(hz);
		double got = MeasureHz(r.Run(48000));
		CHECK(fabs(got / hz - 1) < 0.002, "Main %.2f Hz: measured %.3f", hz, got);
	}
	{
		Rig r;
		r.Plain();
		r.Wave(WSine);
		r.e.pitchKnob = KnobForHz(130.81);
		r.in.cv1 = 341;   // +1V (341.3 a volt)
		double got = MeasureHz(r.Run(48000));
		CHECK(fabs(got / 261.63 - 1) < 0.003, "CV In 1 +1V from C3: %.3f Hz", got);
		r.in.cv1 = -341;
		got = MeasureHz(r.Run(48000));
		CHECK(fabs(got / 65.41 - 1) < 0.003, "CV In 1 -1V from C3: %.3f Hz", got);
	}
	{
		// SEMI +12 and FINE +50 cents
		Rig r;
		r.Plain();
		r.Wave(WSine);
		r.e.pitchKnob = KnobForHz(130.81);
		r.Set(PageOsc, OSemi, ValueForZone(36, kSemiZones));
		double got = MeasureHz(r.Run(48000));
		CHECK(fabs(got / 261.63 - 1) < 0.002, "SEMI +12: %.3f Hz", got);
		r.Set(PageOsc, OSemi, ValueForZone(24, kSemiZones));
		r.Set(PageOsc, OFine, 4095);
		got = MeasureHz(r.Run(48000));
		CHECK(fabs(got / (130.81 * pow(2.0, 50.0 / 1200.0)) - 1) < 0.002, "FINE +50c: %.3f Hz", got);
	}
}

// Every mutant at depth 0 (and every mutant with its mix at 0) leaves the
// wave alone, apart from SQUEEZE ... no: every one but PHASEDIFF, which at
// depth 0 is the wave minus itself half a cycle on
static void TestIdentity()
{
	for (int w = 0; w < kWaves; w++)
	{
		Rig ref;
		ref.Plain();
		ref.Wave(w);
		ref.e.pitchKnob = KnobForHz(200);
		std::vector<double> dry = ref.Run(4800);
		for (int mode = 0; mode < kModes; mode++)
		{
			if (mode == PhaseDiff) continue;
			for (int s = 0; s < 2; s++)
			{
				Rig r;
				r.Plain();
				r.Wave(w);
				r.e.pitchKnob = KnobForHz(200);
				r.Mutant(s, mode, 0.0f);
				std::vector<double> y = r.Run(4800);
				double err = 0;
				for (size_t i = 0; i < y.size(); i++) err = std::max(err, fabs(y[i] - dry[i]));
				CHECK(err <= 2, "wave %d mutant %d mode %d at depth 0 differs from dry by %.0f", w, s, mode, err);
			}
		}
	}
}

// PULSE on a square: the share of the cycle that is high is the width,
// from 50% at depth 0 to 1.5% at full
static void TestPulseWidth()
{
	for (float d : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
	{
		Rig r;
		r.Plain();
		r.Wave(WSquare);
		r.e.pitchKnob = KnobForHz(100);
		r.Mutant(0, Pulse, d);
		std::vector<double> y = r.Run(48000);
		// The middle of the two levels
		double mx = *std::max_element(y.begin(), y.end()), mn = *std::min_element(y.begin(), y.end());
		double mid = (mx + mn) / 2;
		int high = 0;
		for (double v : y) if (v > mid) high++;
		double width = double(high) / double(y.size());
		double want = 0.5 * (1 - 0.97 * double(d));
		CHECK(fabs(width - want) < 0.012, "PULSE depth %.2f: width %.3f, want %.3f", double(d), width, want);
		double hz = MeasureHz(r.Run(48000));
		(void)hz;
	}
}

// The pitch stays put under every mutant at whole-number ratios
static void TestPitchKept()
{
	for (int mode = 0; mode < kModes; mode++)
		for (float ratio : {1.0f, 2.0f, 3.0f})
		{
			Rig r;
			r.Plain();
			r.Wave(WSaw);
			r.e.pitchKnob = KnobForHz(110);
			r.Mutant(0, mode, 0.6f, ratio);
			std::vector<double> y = r.Run(65536);
			std::vector<double> p = Spectrum(y);
			// Everything should be a harmonic of the pitch
			double a = WorstAlias(p, EngineHz(r.e));
			CHECK(a < -40, "mode %d ratio %.0f: strongest non-harmonic %.1f dB", mode, double(ratio), a);
		}
}

// SYNC: depth sweeps the second copy from 1x to 16x; WINDOW fades it at the
// restart, so a full window starts every cycle at zero
static void TestSync()
{
	Rig r;
	r.Plain();
	r.Wave(WSine);
	r.e.pitchKnob = KnobForHz(100);
	r.Mutant(0, Sync, 0.25f);   // 2x
	r.Run(100);
	CHECK(r.e.stage[0].ks == 512, "sync at depth 1/4: %d", r.e.stage[0].ks);
	r.Mutant(0, Sync, 1.0f);
	r.Run(9600);
	CHECK(r.e.stage[0].ks > 4000 && r.e.stage[0].ks <= 4096, "sync at full depth: %d", r.e.stage[0].ks);
	// The map itself: 2x
	Stage st;
	st.mode = Sync;
	st.depthT = 8192;
	st.ratioT = 2 << 16;   // ratio 1
	st.depthS = st.depthT << 6;
	st.ratioS = st.ratioT << 6;
	st.sine = gTables.sine;
	st.Prepare(0);
	int32_t slope, gain;
	CHECK(st.Map(16384, slope, gain) == 32768, "sync map 1/4 cycle -> 1/2");
	CHECK(st.Map(40000, slope, gain) == 80000, "sync map past half (%u)", st.Map(40000, slope, gain));
	CHECK(slope == 256, "sync runs an octave faster: %d", slope);
}

// The phase maps against a floating-point model of each mutant
static double ModelMap(int mode, double s, double depth, double ratio, const double *pts)
{
	double x = s * ratio, u = x - floor(x), w2 = u;
	switch (mode)
	{
	case Pulse:
	{
		double w = 0.5 * (1 - 0.97 * depth), vh = 0.5 / w, u1 = 1.25 * w, u2 = 1 - w / 4;
		w2 = u < u1 ? u * vh : (u < u2 ? 0.625 + (u - u1) * 0.25 / (u2 - u1) : 1 - (1 - u) * vh);
		break;
	}
	case Squeeze:
	{
		double t = depth * 3;
		int j = std::min(2, int(t));
		double f = t - j;
		double lo = pow(u, 1 << j), hi = pow(u, 2 << j);
		w2 = lo + (hi - lo) * f;
		break;
	}
	case Mirror:
	{
		double w = 1 - 0.98 * depth;
		w2 = u < w ? u / w : (1 - u) / (1 - w);
		break;
	}
	case Warp:
	{
		double p = u * 8;
		int j = int(p);
		double c = pts[j] + (pts[(j + 1) % 8] - pts[j]) * (p - j);
		w2 = u + 0.5 * depth * c;
		break;
	}
	case PhaseDiff:
		return x + 0.5 * (1 - 0.97 * depth);
	default:
		return u * pow(2.0, 4 * depth) / ratio;
	}
	return s + (w2 - u) / ratio;
}

static void TestMaps()
{
	double pts[8];
	for (int i = 0; i < 8; i++) pts[i] = sin(2 * M_PI * i / 8) * 0.9;
	int worstMode = -1;
	double worst = 0;
	for (int mode = 0; mode < kModes; mode++)
		for (float ratio : {0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 5.0f, 16.0f})
			for (float depth : {0.0f, 0.1f, 0.33f, 0.5f, 0.8f, 0.99f})
			{
				Stage st;
				st.mode = mode;
				st.depthT = int32_t(lroundf(depth * 32767.0f));
				st.ratioT = RatioLogFromValue(ValueForRatio(ratio));
				st.depthS = st.depthT << 6;
				st.ratioS = st.ratioT << 6;
				for (int i = 0; i < 8; i++) st.points[i] = int32_t(lround(pts[i] * 32767));
				st.points[8] = st.points[0];
				st.sine = gTables.sine;
				st.Prepare(0);
				double dq = double(st.depth) / 32767.0;
				double rq = double(st.r) / 256.0;
				for (uint32_t S = 0; S < (4u << 16); S += 997)
				{
					int32_t slope, gain;
					double got = double(st.Map(S, slope, gain)) / 65536.0;
					double want = ModelMap(mode, double(S) / 65536.0, dq, rq, pts);
					// compare as phases within a cycle (that's all the wave sees)
					double e = fabs(fmod(got - want + 100.5, 1.0) - 0.5);
					if (e > worst) {worst = e; worstMode = mode;}
					// (SYNC's copy can run up to 64 cycles in one run, so its
					// small speed error adds up)
					if (e > 0.01)
					{
						CHECK(false, "mode %d ratio %.2f depth %.2f S %u: %f want %f", mode, double(ratio), double(depth), S, got, want);
						goto next;
					}
				}
			next:;
			}
	printf("  phase maps match the model to within %.5f of a cycle (worst: mode %d)\n", worst, worstMode);
}

// Every setting of every fader, with the mutants on, at low and high pitch:
// the outputs stay in range, nothing gets stuck, nothing wraps
static void TestFuzz()
{
	srand(1);
	int silent = 0;
	for (int trial = 0; trial < 300; trial++)
	{
		Rig r;
		for (int p = 0; p < kPages; p++)
			for (int f = 0; f < kFaders; f++) r.Set(p, f, rand() % 4096);
		// sometimes the ends exactly
		for (int k = 0; k < 6; k++) r.Set(rand() % kPages, rand() % kFaders, (rand() & 1) ? 4095 : 0);
		r.Set(PageOsc, OLevel, 4095);
		r.e.pitchKnob = rand() % 4096;
		r.in.cv1 = (rand() % 4096) - 2048;
		r.in.cv2 = (rand() % 4096) - 2048;
		r.in.audio2 = (rand() % 4096) - 2048;
		r.e.tiltRoll = (rand() % 4065) - 2032;
		r.e.tiltPitch = (rand() % 4065) - 2032;
		double mx = 0;
		int clipped = 0;
		for (int i = 0; i < 6000; i++)
		{
			if (i % 50 == 0) r.in.audio2 = (rand() % 4096) - 2048;
			r.e.Tick(r.in);
			mx = std::max(mx, fabs(double(r.e.out1)));
			if (r.e.out1 >= 2047 || r.e.out1 <= -2048) clipped++;
			CHECK(r.e.out1 >= -2048 && r.e.out1 <= 2047 && r.e.out2 >= -2048 && r.e.out2 <= 2047, "range");
		}
		if (mx < 20) silent++;
		// A full-level output should only clip with drive on or a lot of feedback
		if (r.e.Value(PageOsc, ODrive) == 0 && r.e.Value(PageOsc, OBits) < 64)
			CHECK(clipped < 600, "trial %d: clipped %d samples without drive", trial, clipped);
	}
	CHECK(silent < 15, "%d of 300 random settings were nearly silent", silent);
}

// Band-limiting: measure, at a range of pitches, the worst thing below
// 20kHz that isn't a harmonic, for the plain waves and each mutant
static void TestAliasing()
{
	// (Harmonics between 24 and 29kHz, in the filter's transition band, fold
	// to 19-24kHz: inaudible, so the measurement stops at 18kHz)
	printf("  aliasing, the strongest non-harmonic below 18kHz (dB below the strongest harmonic):\n");
	printf("  %-22s", "");
	const double pitches[] = {110, 440, 1760, 3520};
	for (double hz : pitches) printf("%8.0fHz", hz);
	printf("\n");
	struct Case {const char *name; int wave; int mode; float depth; float window; float ratio;};
	const Case cases[] = {
		{"saw, dry", WSaw, -1, 0, 0, 1},
		{"square, dry", WSquare, -1, 0, 0, 1},
		{"saw PULSE 50%", WSaw, Pulse, 0.5f, 0, 1},
		{"square PULSE 95%", WSquare, Pulse, 0.95f, 0, 1},
		{"saw SQUEEZE 70%", WSaw, Squeeze, 0.7f, 0, 1},
		{"tri MIRROR 80%", WTriangle, Mirror, 0.8f, 0, 1},
		{"saw WARP 70%", WSaw, Warp, 0.7f, 0, 1},
		{"saw PHASEDIFF 60%", WSaw, PhaseDiff, 0.6f, 0, 1},
		{"saw PULSE x3 50%", WSaw, Pulse, 0.5f, 0, 3},
		{"sine SYNC 60%", WSine, Sync, 0.6f, 0, 1},
		{"saw SYNC 60%", WSaw, Sync, 0.6f, 0, 1},
		{"saw SYNC 60% window", WSaw, Sync, 0.6f, 1, 1},
	};
	for (const Case &c : cases)
	{
		printf("  %-22s", c.name);
		for (double hz : pitches)
		{
			Rig r;
			r.Plain();
			r.Wave(c.wave);
			// (above the Main knob's range, an octave up on SEMI)
			r.e.pitchKnob = KnobForHz(hz > 2000 ? hz / 2 : hz);
			if (hz > 2000) r.Set(PageOsc, OSemi, ValueForZone(36, kSemiZones));
			if (c.mode >= 0)
			{
				r.Mutant(0, c.mode, c.depth, c.ratio);
				r.Set(PageMut1, MWindow, int32_t(c.window * 4095));
			}
			std::vector<double> y = r.Run(65536);
			double a = WorstAlias(Spectrum(y), EngineHz(r.e));
			printf("%10.1f", a);
			// Plain waves and the phase-bending mutants are clean at
			// ordinary pitches; sync's restart is not band-limited, so only
			// a loose bound there, and up high everything gets rougher
			double limit = c.mode == Sync && c.window < 1 ? -30 : -50;
			if (hz >= 1760) limit += 12;
			if (hz <= 1760) CHECK(a < limit, "%s at %.0f Hz: %.1f dB", c.name, hz, a);
		}
		printf("\n");
	}
}

// Out 2's choices
static void TestOut2()
{
	Rig r;
	r.Plain();
	r.Wave(WSaw);
	r.e.pitchKnob = KnobForHz(200);
	r.Mutant(0, Pulse, 0.8f);
	r.Set(PageOsc, OOut2, ValueForZone(Out2Sub1, kOut2Modes));
	double hz = MeasureHz(r.Run(48000, 9600, 2));
	CHECK(fabs(hz / 100.0 - 1) < 0.01, "sub -1 oct: %.2f Hz", hz);
	r.Set(PageOsc, OOut2, ValueForZone(Out2Sub2, kOut2Modes));
	hz = MeasureHz(r.Run(48000, 9600, 2));
	CHECK(fabs(hz / 50.0 - 1) < 0.01, "sub -2 oct: %.2f Hz", hz);
	r.Set(PageOsc, OOut2, ValueForZone(Out2Same, kOut2Modes));
	r.Run(10);
	CHECK(r.e.out1 == r.e.out2, "Out 2 same as Out 1");
}

// LFOs reach the depth, and Pulse In 2 restarts them
static void TestModulation()
{
	Rig r;
	r.Plain();
	r.Set(PageMut1, MMode, ValueForZone(Pulse, kModes));
	r.Set(PageMut1, MMix, 4095);
	r.Set(PageMut1, MLfo1, 4095);                  // LFO 1 -> depth, +100%
	r.Set(PageLfo1, LRate, 4095);                  // 40Hz
	r.Set(PageLfo1, LShape, ValueForZone(LSquare, kShapes));
	int32_t lo = 99999, hi = -1;
	for (int i = 0; i < 48000; i++)
	{
		r.e.Tick(r.in);
		lo = std::min(lo, r.e.stage[0].depth);
		hi = std::max(hi, r.e.stage[0].depth);
	}
	CHECK(lo < 100 && hi > 32600, "LFO square sweeps depth 0..full: %d..%d", lo, hi);
	// CV In 2 at +5V -> full depth on mutant 1 (the default MOD page)
	Rig c;
	c.in.cv2 = 1707;
	c.Set(PageMut1, MLfo1, 2048);
	c.Run(100, 4800);
	CHECK(c.e.stage[0].depth > 32000, "CV In 2 +5V -> depth %d", c.e.stage[0].depth);
	// Tilt, past the dead zone only
	Rig t;
	t.Set(PageMut1, MLfo1, 2048);
	t.Set(PageMod, CvDepth1, 2048);
	t.Set(PageMod, TiltDepth1, 4095);
	t.e.tiltRoll = 400;
	t.Run(10, 4800);
	CHECK(t.e.stage[0].depth == 0, "tilt within the dead zone: %d", t.e.stage[0].depth);
	t.e.tiltRoll = 2032;
	t.Run(10, 4800);
	CHECK(t.e.stage[0].depth >= 32760, "tilt right up: %d", t.e.stage[0].depth);
	// LFO reset
	r.in.lfoReset = true;
	r.e.Tick(r.in);
	r.in.lfoReset = false;
	CHECK(r.e.lfo[0].phase < 100000000u && r.e.lfo[1].phase < 100000000u, "LFO reset");
}

// Pulse In 1 restarts the wave, Audio In 1 syncs it
static void TestResetAndSync()
{
	Rig r;
	r.Plain();
	r.Wave(WSine);
	r.e.pitchKnob = KnobForHz(97);
	// A 150Hz square into Audio In 1: the oscillator now repeats at 150Hz
	r.in.syncPatched = true;
	const int per = 320;   // 150Hz
	std::vector<double> y;
	for (int i = 0; i < 9600 + 65536; i++)
	{
		r.in.audio1 = (i % per) < per / 2 ? 1000 : -1000;
		r.e.Tick(r.in);
		if (i >= 9600) y.push_back(r.e.out1);
	}
	std::vector<double> p = Spectrum(y);
	double a = WorstAlias(p, 150.0, 1, 5000.0);
	CHECK(a < -25, "synced to 150Hz: strongest non-harmonic below 5kHz %.1f dB", a);
	CHECK(r.e.syncing, "syncing flag");
}

// The 8mu: pages, pickup, and handing over between knob and fader
static void TestUi()
{
	Engine e;
	e.Begin(&gTables);
	EightMuUi ui;
	int32_t f[8] = {};
	bool b[4] = {};
	ui.Update(e, f, b);
	CHECK(ui.page == PageMut1, "starts on MUTANT 1");
	// Buttons: A, A again (WARP 1), A again (back), then C, C (MOD)
	b[0] = true; ui.Update(e, f, b); b[0] = false; ui.Update(e, f, b);
	CHECK(ui.page == PageWarp1, "A twice: WARP 1 (%d)", ui.page);
	b[0] = true; ui.Update(e, f, b); b[0] = false; ui.Update(e, f, b);
	CHECK(ui.page == PageMut1, "A again: MUTANT 1");
	b[2] = true; ui.Update(e, f, b); b[2] = false; ui.Update(e, f, b);
	b[2] = true; ui.Update(e, f, b); b[2] = false; ui.Update(e, f, b);
	CHECK(ui.page == PageMod, "C twice: MOD (%d)", ui.page);
	// Pickup: MOD fader 1 is at full (4095, 127); a fader at 0 doesn't move it
	CHECK(e.values[PageMod][0] == 4095, "MOD default");
	f[0] = 10; ui.Update(e, f, b);
	CHECK(e.values[PageMod][0] == 4095, "not picked up yet");
	f[0] = 126; ui.Update(e, f, b);
	f[0] = 100; ui.Update(e, f, b);
	CHECK(e.values[PageMod][0] == (100 * 4095) / 127, "picked up: %d", e.values[PageMod][0]);
	// Changed elsewhere (a knob or the web editor): the fader lets go
	e.values[PageMod][0] = 2048;
	f[0] = 101; ui.Update(e, f, b);
	CHECK(e.values[PageMod][0] == 2048, "let go after another change");
	f[0] = 64; ui.Update(e, f, b);
	CHECK(e.values[PageMod][0] == (64 * 4095) / 127, "and picks up again: %d", e.values[PageMod][0]);
	// LEDs in range
	int32_t led[8];
	for (int pg = 0; pg < kPages; pg++)
	{
		ui.SetPage(pg);
		ui.Leds(e, led);
		for (int i = 0; i < 8; i++) CHECK(led[i] >= 0 && led[i] <= 4095, "LED %d page %d: %d", i, pg, led[i]);
	}
}

int main()
{
	gTables.Build();
	TestTables();
	TestHelpers();
	TestPitch();
	TestIdentity();
	TestPulseWidth();
	TestMaps();
	TestSync();
	TestPitchKept();
	TestOut2();
	TestModulation();
	TestResetAndSync();
	TestUi();
	TestFuzz();
	TestAliasing();
	printf("engine_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
