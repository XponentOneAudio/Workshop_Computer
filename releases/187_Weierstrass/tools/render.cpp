// Desktop test bench for weierstrass.h.
//
//   g++ -O2 -std=c++17 -I.. render.cpp -o render && ./render [outdir]
//
// Renders a few zooms to WAV and checks the claims the engine makes:
//   - the zoom loops: one full step later the spectrum is the same,
//   - the relabel is seamless: no jump in the waveform where z wraps,
//   - the level holds steady through the zoom,
//   - coherent mode really is W(x): with no zoom, the output matches the
//     Weierstrass sum evaluated in double precision.

#include "weierstrass.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>

static const double SR = 48000.0;
static int failures = 0;

static void Check(bool ok, const char *what, double v)
{
	printf("  %-58s %10.4f  %s\n", what, v, ok ? "ok" : "FAIL");
	if (!ok) failures++;
}

static void WriteWav(const std::string &path, const std::vector<int16_t> &l, const std::vector<int16_t> &r)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	uint32_t n = (uint32_t)l.size(), bytes = n * 4;
	auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
	auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
	fwrite("RIFF", 1, 4, f); w32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
	w32(16); w16(1); w16(2); w32(48000); w32(48000 * 4); w16(4); w16(16);
	fwrite("data", 1, 4, f); w32(bytes);
	for (uint32_t i = 0; i < n; i++) { w16(uint16_t(l[i] * 16)); w16(uint16_t(r[i] * 16)); }
	fclose(f);
}

// Zoom rate in Q32 steps per sample from steps per second.
static int32_t Rate(double stepsPerSec) { return (int32_t)lrint(stepsPerSec * 4294967296.0 / SR); }

// Magnitude spectrum on a log-frequency grid (Goertzel at fixed frequencies).
static std::vector<double> LogSpectrum(const std::vector<int16_t> &x, size_t start, size_t len)
{
	std::vector<double> out;
	for (double f = 30.0; f < 16000.0; f *= 1.02)
	{
		double c = 2.0 * cos(2.0 * M_PI * f / SR), s1 = 0, s2 = 0;
		for (size_t i = 0; i < len; i++)
		{
			double win = 0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)len);
			double s = win * x[start + i] + c * s1 - s2;
			s2 = s1; s1 = s;
		}
		out.push_back(sqrt(s1 * s1 + s2 * s2 - c * s1 * s2));
	}
	return out;
}

static double SpectralDistance(const std::vector<double> &a, const std::vector<double> &b)
{
	double num = 0, den = 0;
	for (size_t i = 0; i < a.size(); i++) { num += (a[i] - b[i]) * (a[i] - b[i]); den += a[i] * a[i]; }
	return sqrt(num / den);
}

static void ZoomTest(const std::string &dir, int ratio, int rough, double stepsPerSec, bool scatter)
{
	printf("ratio %d, roughness %d, %+.2f steps/s, %s\n", ratio, rough, stepsPerSec, scatter ? "scatter" : "coherent");
	weier::Engine e;
	e.SetRatio(ratio);
	e.SetRoughness(rough);
	e.SetScatter(scatter);
	int16_t c, s;
	for (int i = 0; i < 4800; i++) e.Process(0, c, s);  // let the declick settle
	e.Reset();
	e.SetZoomRate(Rate(stepsPerSec));

	const size_t n = (size_t)(SR * 3.0 / fabs(stepsPerSec));  // three zoom steps
	std::vector<int16_t> L, R;
	std::vector<size_t> wraps;
	for (size_t i = 0; i < n; i++)
	{
		if (e.Process(0, c, s)) wraps.push_back(i);
		L.push_back(c); R.push_back(s);
	}

	// Seam: the largest sample-to-sample jump just after each wrap, against
	// the largest jump in the 50 ms either side. A click would make the wrap
	// the outlier. (A zoom-in wraps on sample 0, straight after Reset; that
	// one is the reset, not a seam, so it is skipped.)
	double worst = 0;
	for (size_t w : wraps)
	{
		if (w < 2400 || w + 2400 >= n) continue;
		double atWrap = 0, around = 0;
		for (size_t i = w; i < w + 3; i++) atWrap = fmax(atWrap, fabs((double)L[i] - L[i - 1]));
		for (size_t i = w - 2400; i < w + 2400; i++)
			if (i < w || i >= w + 3) around = fmax(around, fabs((double)L[i] - L[i - 1]));
		worst = fmax(worst, around > 0 ? atWrap / around : 0);
	}
	Check(wraps.size() >= 2, "zoom steps completed", (double)wraps.size());
	Check(worst <= 1.0, "jump at wrap vs nearby waveform (ratio, <= 1)", worst);

	// Loop: spectrum a quarter of the way into step 1 vs the same point in step 2.
	size_t stepLen = (size_t)(SR / fabs(stepsPerSec));
	size_t win = 8192;
	auto a = LogSpectrum(L, stepLen / 4, win);
	auto b = LogSpectrum(L, stepLen + stepLen / 4, win);
	auto mid = LogSpectrum(L, stepLen / 4 + stepLen / 2, win);
	double dSame = SpectralDistance(a, b), dHalf = SpectralDistance(a, mid);
	Check(dSame < 0.2, "spectral distance, one step apart (small)", dSame);
	Check(dHalf > dSame, "spectral distance, half a step apart (larger)", dHalf);

	// Level through the zoom: RMS in 50 ms blocks.
	double lo = 1e9, hi = 0;
	size_t blk = 2400;
	for (size_t st = 0; st + blk <= n; st += blk)
	{
		double acc = 0;
		for (size_t i = st; i < st + blk; i++) acc += (double)L[i] * L[i];
		double rms = sqrt(acc / blk);
		lo = fmin(lo, rms); hi = fmax(hi, rms);
	}
	Check(20 * log10(hi / lo) < 6.0, "RMS swing through the zoom, dB", 20 * log10(hi / lo));

	char name[128];
	snprintf(name, sizeof name, "/zoom_b%d_r%d_%s_%s.wav", ratio, rough, stepsPerSec < 0 ? "in" : "out", scatter ? "scatter" : "true");
	WriteWav(dir + name, L, R);
}

// With no zoom and no scatter the engine plays sum_k w_k cos(2pi b^k X).
static void CoherenceTest()
{
	printf("coherence: output vs double-precision Weierstrass sum\n");
	weier::Engine e;
	e.SetRatio(3);
	e.SetRoughness(3000);
	int16_t c, s;
	for (int i = 0; i < 4800; i++) e.Process(0, c, s);
	e.Reset();
	e.SetZoomRate(0);
	for (int i = 0; i < 64; i++) e.Process(0, c, s);  // let amplitudes settle

	double inc = e.BaseInc() / 4294967296.0;
	double X = 0;
	{
		// Reconstruct X: Reset put P at 0 and every sample added BaseInc.
		X = 64 * inc;
	}
	double err = 0, peak = 0;
	for (int i = 0; i < 48000; i++)
	{
		e.Process(0, c, s);
		X += inc;
		double ref = 0;
		double bk = 1;
		for (int k = 0; k < e.Partials(); k++)
		{
			ref += e.PartialLevel(k) / 32767.0 * cos(2.0 * M_PI * fmod(bk * X, 1.0));
			bk *= 3;
		}
		ref *= 2047.0;
		err = fmax(err, fabs(ref - c));
		peak = fmax(peak, fabs(ref));
	}
	Check(err / peak < 0.01, "max error / peak", err / peak);
}

int main(int argc, char **argv)
{
	std::string dir = argc > 1 ? argv[1] : ".";
	CoherenceTest();
	ZoomTest(dir, 2, 1500, -0.25, false);
	ZoomTest(dir, 2, 1500, +0.25, true);
	ZoomTest(dir, 3, 2500, -0.2, false);
	ZoomTest(dir, 3, 2500, +0.5, false);
	ZoomTest(dir, 5, 3500, -0.2, false);
	ZoomTest(dir, 7, 3000, +0.2, true);
	ZoomTest(dir, 3, 4095, -1.0, false);
	ZoomTest(dir, 2, 0, -0.5, false);
	printf("%s (%d failures)\n", failures ? "FAILED" : "all passed", failures);
	return failures ? 1 : 0;
}
