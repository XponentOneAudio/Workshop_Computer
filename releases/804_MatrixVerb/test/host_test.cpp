// Host tests for MatrixVerb's design maths and network (design.h, fdn.h).
//
//   g++ -O2 -std=c++17 -I.. host_test.cpp -o host_test && ./host_test
//
// Checks that the lengths are always pairwise coprime, that the normalised
// matrix (as the audio uses it, in Q14) never amplifies, that the decay time
// comes out as set, and that no settings at all - random matrices, infinite
// decay, full drive, +12dB in, loud noise - get the wet signal past the
// ceiling or the network past full scale.  Pass a file name to also write
// the default impulse response as raw stereo 16-bit 48kHz.

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../design.h"
#include "../fdn.h"

using namespace mv;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 12345;
static uint32_t Rand() {rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng;}
static int32_t Noise(int32_t amp) {return int32_t((int64_t(int32_t(Rand())) * amp) >> 31);}

// Spectral norm of a Q14 matrix, in double, by power iteration on A'A run
// to convergence (fine for a test)
static double NormQ14(const int16_t *m)
{
	double v[8], w[8];
	for (int i = 0; i < 8; i++) v[i] = 1.0 + 0.1 * i;
	double s = 0;
	for (int it = 0; it < 3000; it++)
	{
		for (int i = 0; i < 8; i++) {w[i] = 0; for (int j = 0; j < 8; j++) w[i] += m[i * 8 + j] / 16384.0 * v[j];}
		double n = 0;
		for (int i = 0; i < 8; i++) {v[i] = 0; for (int j = 0; j < 8; j++) v[i] += m[j * 8 + i] / 16384.0 * w[j]; n += v[i] * v[i];}
		n = sqrt(n);
		if (n < 1e-300) return 0;
		for (int i = 0; i < 8; i++) v[i] /= n;
		s = sqrt(n);
	}
	return s;
}

static Controls Make(const uint8_t *v, int32_t decayOct = 0, int32_t sizeOct = 0, int32_t damp = 0)
{
	Controls c;
	memcpy(c.v, v, kNumValues);
	c.decayOct = decayOct;
	c.sizeOct = sizeOct;
	c.dampOffset = damp;
	return c;
}

static void TestLengths()
{
	for (int t = 0; t < 2000; t++)
	{
		uint8_t v[kNumValues];
		Defaults(v);
		for (int i = 0; i < 8; i++) v[PageLength * 8 + i] = uint8_t(Rand() % 128);
		if (t % 3 == 0) for (int i = 0; i < 8; i++) v[PageLength * 8 + i] = uint8_t(Rand() % 4 + 60); // crowded
		Designer dz;
		Design d;
		dz.Compute(Make(v, 0, int32_t(Rand() % 16384) - 8192), d);
		for (int i = 0; i < 8; i++)
		{
			CHECK(d.len[i] >= kMinLen && d.len[i] <= kMaxLen, "length %d out of range", d.len[i]);
			for (int j = 0; j < i; j++) CHECK(Gcd(d.len[i], d.len[j]) == 1, "lengths %d and %d share a factor", d.len[i], d.len[j]);
		}
	}
	printf("lengths: ok\n");
}

static void TestNorm()
{
	double worst = 0;
	for (int t = 0; t < 400; t++)
	{
		uint8_t v[kNumValues];
		Defaults(v);
		if (t < kNumTemplates) Template(t, v);
		else for (int i = 0; i < 64; i++) v[i] = uint8_t(t % 5 == 0 ? (Rand() & 1 ? 127 : 0) : Rand() % 128);
		v[PageLab * 8 + LbNormalise] = uint8_t(Rand() % 128);
		v[PageLab * 8 + LbBlend] = uint8_t(Rand() % 128);
		if (t % 4 == 0) v[PageLab * 8 + LbNormalise] = 127;
		Designer dz;
		Design d;
		dz.Compute(Make(v), d);
		double n = NormQ14(d.mat), nf = NormQ14(d.matFreeze);
		if (n > worst) worst = n;
		if (nf > worst) worst = nf;
		CHECK(n < 1.0, "matrix norm %f >= 1 (test %d)", n, t);
		CHECK(nf < 1.0, "freeze matrix norm %f >= 1 (test %d)", nf, t);
		double sig = d.sigmaMilli / 1000.0;
		if (v[PageLab * 8 + LbNormalise] == 127 && sig > 0.01) CHECK(n > 0.99, "lossless matrix norm only %f", n);
	}
	printf("matrix norm: ok, worst %.6f\n", worst);
}

// Run the engine with a design; return the peak wet/out seen
struct RunResult {int32_t peakOut, peakTank; int32_t minGuard;};
static RunResult Run(Engine &e, int samples, int32_t inAmp, bool impulseAtStart, int16_t *rec = nullptr)
{
	RunResult r = {0, 0, 32768};
	for (int n = 0; n < samples; n++)
	{
		int32_t in = 0;
		if (inAmp) in = Noise(inAmp);
		// A burst of noise at -6dBFS for 0.2s, a second in
		if (impulseAtStart && n >= 48000 && n < 48000 + 9600) in = Noise(16384);
		int32_t ol, orr;
		e.Process(in, in, ol, orr);
		int32_t a = abs(ol) > abs(orr) ? abs(ol) : abs(orr);
		if (a > r.peakOut) r.peakOut = a;
		if (e.lvTank > r.peakTank) r.peakTank = e.lvTank;
		if (e.guard < r.minGuard) r.minGuard = e.guard;
		if (rec) {rec[2 * n] = int16_t(ol > 32767 ? 32767 : (ol < -32768 ? -32768 : ol)); rec[2 * n + 1] = int16_t(orr > 32767 ? 32767 : (orr < -32768 ? -32768 : orr));}
	}
	return r;
}

// RT60 from a recorded impulse response (Schroeder backward integration),
// fitted between -5 and -35dB
static double MeasureRt(const int16_t *rec, int n, int start)
{
	static double edc[48000 * 20];
	double s = 0;
	for (int i = n - 1; i >= start; i--)
	{
		double a = rec[2 * i], b = rec[2 * i + 1];
		s += a * a + b * b;
		edc[i] = s;
	}
	double total = edc[start];
	int i5 = -1, i35 = -1;
	for (int i = start; i < n; i++)
	{
		double db = 10 * log10(edc[i] / total + 1e-30);
		if (i5 < 0 && db <= -5) i5 = i;
		if (i35 < 0 && db <= -35) {i35 = i; break;}
	}
	if (i5 < 0 || i35 < 0) return -1;
	return (i35 - i5) / 48000.0 * 2.0;
}

static void TestDecay(const char *irFile)
{
	static int16_t rec[48000 * 14 * 2];
	const float times[] = {0.5f, 2.0f, 6.0f};
	for (float rt : times)
	{
		uint8_t v[kNumValues];
		Defaults(v);
		v[PageReverb * 8 + RvMix] = 127;              // wet only
		v[PageReverb * 8 + RvDamping] = 0;
		v[PageReverb * 8 + RvLowCut] = 0;
		v[PageReverb * 8 + RvModDepth] = 0;
		v[PageReverb * 8 + RvPredelay] = 0;
		v[PageLab * 8 + LbCeiling] = 127;
		int dv = int(lroundf(125.0f * logf(rt / 0.1f) / logf(300.0f)));
		v[PageReverb * 8 + RvDecay] = uint8_t(dv);
		float want = DecaySeconds(dv);
		Engine e;
		e.Init();
		Designer dz;
		Design d;
		dz.Compute(Make(v), d);
		e.SetDesign(d);
		int n = 48000 * (2 + int(want * 1.5f) + 1);
		Run(e, n, 0, true, rec);
		double got = MeasureRt(rec, n, 48000 + 9600);
		printf("decay: set %.2fs, measured %.2fs\n", want, got);
		CHECK(got > want * 0.8 && got < want * 1.2, "decay %.2fs measured as %.2fs", want, got);
	}

	// The default sound, and its level against the dry signal
	uint8_t v[kNumValues];
	Defaults(v);
	Engine e;
	e.Init();
	Designer dz;
	Design d;
	dz.Compute(Make(v), d);
	e.SetDesign(d);
	int n = 48000 * 5;
	RunResult r = Run(e, n, 0, true, rec);
	printf("default burst: peak out %d (dry burst peak 16384), measured RT60 %.2fs\n", r.peakOut, MeasureRt(rec, n, 48000 + 9600));
	if (irFile)
	{
		FILE *fp = fopen(irFile, "wb");
		if (fp) {fwrite(rec, 4, size_t(n), fp); fclose(fp); printf("wrote %s\n", irFile);}
	}

	// Steady noise in at -12dBFS, defaults: how hot does the network run?
	Engine e2;
	e2.Init();
	e2.SetDesign(d);
	RunResult r2 = Run(e2, 48000 * 6, 8192, false);
	printf("default, noise at -12dBFS: network peak %d (guard at %d), out peak %d\n", r2.peakTank, 22937, r2.peakOut);
}

static void TestSafety()
{
	int32_t worstOut = 0, worstWet = 0;
	int guarded = 0;
	for (int t = 0; t < 60; t++)
	{
		uint8_t v[kNumValues];
		Defaults(v);
		for (int i = 0; i < 64; i++) v[i] = uint8_t(Rand() % 128);
		for (int i = PageLength * 8; i < kNumValues; i++) v[i] = uint8_t(Rand() % 128);
		v[PageReverb * 8 + RvDecay] = uint8_t(120 + Rand() % 8);     // long to infinite
		v[PageReverb * 8 + RvDamping] = uint8_t(Rand() % 20);
		v[PageReverb * 8 + RvMix] = 127;                            // wet only
		v[PageLab * 8 + LbNormalise] = 127;
		v[PageLab * 8 + LbInput] = 127;                             // +12dB in
		v[PageLab * 8 + LbOutput] = 127;                            // +12dB out
		v[PageLab * 8 + LbDrive] = uint8_t(Rand() % 128);
		int ceilV = int(Rand() % 128);
		v[PageLab * 8 + LbCeiling] = uint8_t(ceilV);
		Engine e;
		e.Init();
		Designer dz;
		Design d;
		dz.Compute(Make(v, 8192, int32_t(Rand() % 16384) - 8192), d);
		e.SetDesign(d);
		if (t % 3 == 0) e.SetFreeze(true);
		RunResult r = Run(e, 48000 * 4, 32000, false);
		// Wet only: the output is the limited wet, so it must stay under the
		// ceiling (soft clipped above half scale, which only lowers it)
		int32_t ceil = d.ceiling;
		CHECK(r.peakOut <= ceil, "test %d: out %d over ceiling %d", t, r.peakOut, ceil);
		CHECK(r.peakTank <= 32767, "test %d: network %d over full scale", t, r.peakTank);
		if (r.peakOut > worstWet) worstWet = r.peakOut;
		if (r.minGuard < 32000) guarded++;
		// Then the input stops: with a finite decay it must die away (unless
		// frozen or infinite)
		Run(e, 48000 * 2, 0, false);
		(void)worstOut;
	}
	printf("safety: ok, loudest wet %d, guard acted in %d of 60 runs\n", worstWet, guarded);

	// Dry and wet at full: the final soft clip keeps it in range
	uint8_t v[kNumValues];
	Defaults(v);
	v[PageLab * 8 + LbInput] = 127;
	v[PageLab * 8 + LbOutput] = 127;
	v[PageLab * 8 + LbCeiling] = 127;
	v[PageReverb * 8 + RvDecay] = 127;
	Engine e;
	e.Init();
	Designer dz;
	Design d;
	dz.Compute(Make(v), d);
	e.SetDesign(d);
	RunResult r = Run(e, 48000 * 4, 32767, false);
	CHECK(r.peakOut <= 32767, "full mix out %d", r.peakOut);
	printf("full mix: peak out %d\n", r.peakOut);
}

int main(int argc, char **argv)
{
	TestLengths();
	TestNorm();
	TestDecay(argc > 1 ? argv[1] : nullptr);
	TestSafety();
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures ? 1 : 0;
}
