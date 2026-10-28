// Host-side tests and renders for the WDF Benjolin DSP core.
//
//   g++ -O2 -std=c++17 -I.. host_sim.cpp -o host_sim && ./host_sim [outdir]
//
// Checks the fixed-point WDF filter against the analogue prototype it models,
// checks self-oscillation and amplitude limiting, runs a stress test over
// extreme settings, and writes a few example WAV files.

#include "benjolin_wdf.h"

#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace wdfb;

static int failures = 0;
static void check(bool ok, const char *what)
{
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok) failures++;
}

static void writeWav(const std::string &path, const std::vector<float> &l, const std::vector<float> &r)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	uint32_t n = l.size(), dataBytes = n * 4, sr = 48000, byteRate = sr * 4, chunk = 36 + dataBytes;
	uint16_t fmt = 1, ch = 2, align = 4, bits = 16;
	uint32_t sixteen = 16;
	fwrite("RIFF", 1, 4, f); fwrite(&chunk, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&sixteen, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
	fwrite(&byteRate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
	for (uint32_t i = 0; i < n; i++)
	{
		int16_t s[2] = {(int16_t)(std::max(-1.f, std::min(1.f, l[i])) * 32000),
						(int16_t)(std::max(-1.f, std::min(1.f, r[i])) * 32000)};
		fwrite(s, 2, 2, f);
	}
	fclose(f);
}

// Analogue prototype, normalised: H_LP(s) = 1 / (s^2/w^2 + (Rs - rn) s/w + 1)
// evaluated through the bilinear transform with prewarping at fc.
static double analogueLP(double f, double fc, double rn)
{
	double k = tan(M_PI * fc / 48000.0);
	double wd = tan(M_PI * f / 48000.0) / k; // warped, normalised frequency
	std::complex<double> s(0, wd);
	return std::abs(1.0 / (s * s + (FILT_RS - rn) * s + 1.0));
}

static double measureGain(ResonantFilter &flt, int32_t cutOct, int32_t res, double f, int which)
{
	flt.Reset();
	const int N = 48000 / 2;
	double amp = 0.05; // small, stays in the linear region
	double acc = 0, accRef = 0;
	for (int n = 0; n < N; n++)
	{
		double x = amp * sin(2 * M_PI * f * n / 48000.0);
		flt.Step((int32_t)(x * ONE), cutOct, res);
		if (n > N / 2)
		{
			double y = (which == 0 ? flt.lp : which == 1 ? flt.bp : flt.hp) / (double)ONE;
			acc += y * y;
			accRef += x * x;
		}
	}
	return sqrt(acc / accRef);
}

int main(int argc, char **argv)
{
	std::string out = argc > 1 ? argv[1] : ".";
	static ResonantFilter flt;
	flt.Init();

	printf("Filter vs analogue prototype (linear region)\n");
	double worst = 0;
	for (double fcOct : {1.0, 3.0, 5.0, 7.0, 8.5})
		for (double resv : {0.0, 0.5, 0.8})
		{
			double fc = FILT_FMIN * exp2(fcOct);
			int32_t cut = (int32_t)(fcOct * OCT);
			int32_t res = (int32_t)(resv * CONE);
			double rn = resv * FILT_RN_MAX;
			for (double fr : {0.25, 0.5, 1.0, 2.0, 4.0})
			{
				double f = fc * fr;
				if (f > 20000 || f < 10) continue;
				double g = measureGain(flt, cut, res, f, 0);
				double ref = analogueLP(f, fc, rn);
				double errDb = 20 * log10(g / ref);
				if (fabs(errDb) > fabs(worst)) worst = errDb;
				if (fabs(errDb) > 0.5)
					printf("    fc=%7.1f res=%.1f f=%8.1f  wdf=%.4f ref=%.4f  (%.2f dB)\n", fc, resv, f, g, ref, errDb);
			}
		}
	printf("  worst LP deviation %.3f dB\n", worst);
	check(fabs(worst) < 0.5, "LP magnitude within 0.5 dB of the prewarped analogue prototype");

	{
		double fc = FILT_FMIN * exp2(5.0);
		double gbp = measureGain(flt, 5 * OCT, 0, fc, 1);
		double ghp = measureGain(flt, 5 * OCT, 0, fc * 8, 2);
		double ghpLow = measureGain(flt, 5 * OCT, 0, fc / 8, 2);
		printf("  BP at fc %.3f, HP at 8fc %.3f, HP at fc/8 %.4f\n", gbp, ghp, ghpLow);
		check(fabs(gbp - 1.0) < 0.02, "BP has unity gain at fc (R_s = 1)");
		check(fabs(ghp - 1.0) < 0.05 && ghpLow < 0.03, "HP passes highs and rejects lows");
	}

	printf("Self-oscillation\n");
	for (double fcOct : {2.0, 5.0, 8.0})
	{
		for (double resv : {0.85, 1.0})
		{
			flt.Reset();
			int32_t cut = (int32_t)(fcOct * OCT), res = (int32_t)(resv * CONE);
			flt.Step(ONE / 10, cut, res); // kick
			double peak = 0;
			int crossings = 0;
			int32_t prev = 0;
			for (int n = 0; n < 48000 * 2; n++)
			{
				flt.Step(0, cut, res);
				if (n > 48000)
				{
					peak = std::max(peak, fabs(flt.lp / (double)ONE));
					if (prev < 0 && flt.lp >= 0) crossings++;
				}
				prev = flt.lp;
			}
			double fc = FILT_FMIN * exp2(fcOct);
			printf("  fc=%7.1f res=%.2f: peak %.3f, osc freq %.1f Hz\n", fc, resv, peak, (double)crossings);
			if (resv < 0.89)
				check(peak < 1e-3, "  decays below the oscillation threshold");
			else
			{
				check(peak > 0.5 && peak < 2.0, "  oscillates with bounded amplitude");
				check(fabs(crossings / fc - 1.0) < 0.05, "  oscillates at the cutoff frequency");
			}
		}
	}

	printf("Band-limited comparator\n");
	{
		// Square wave from comparing a triangle with 0. Fit the true harmonics
		// (odd multiples of f below Nyquist) by least squares; what is left is
		// aliasing. Compare against the naive comparator.
		auto aliasDb = [](bool blep, double f) {
			const int N = 48000;
			TriangleCore osc;
			BlepComparator cmp;
			int32_t inc = (int32_t)(4.0 * f / 48000.0 * ONE);
			std::vector<double> y(N);
			for (int n = 0; n < N; n++)
			{
				osc.Step(inc);
				int32_t o = blep ? cmp.Step(osc.v) : (osc.v > 0 ? BlepComparator::H : -BlepComparator::H);
				y[n] = o / (double)ONE;
			}
			// harmonic fit, with frequency estimated from the integer increment
			double fe = inc / (double)ONE * 48000.0 / 4.0;
			std::vector<double> r = y;
			double total = 0;
			for (double v : y) total += v * v;
			for (int k = 1; k * fe < 24000; k += 2)
			{
				double w = 2 * M_PI * k * fe / 48000.0, cs = 0, sn = 0;
				for (int n = 0; n < N; n++) { cs += y[n] * cos(w * n); sn += y[n] * sin(w * n); }
				cs *= 2.0 / N; sn *= 2.0 / N;
				for (int n = 0; n < N; n++) r[n] -= cs * cos(w * n) + sn * sin(w * n);
			}
			double res = 0;
			for (int n = N / 10; n < N - N / 10; n++) res += r[n] * r[n];
			return 10 * log10(res / (total * 0.8));
		};
		for (double f : {1234.5, 3001.7})
		{
			double naive = aliasDb(false, f), blep = aliasDb(true, f);
			printf("  %.1f Hz: aliasing naive %.1f dB, polyBLEP %.1f dB\n", f, naive, blep);
			check(blep < naive - 10, "  polyBLEP reduces aliasing by more than 10 dB");
		}
	}

	printf("Cross FM\n");
	{
		auto freqOf = [](int32_t fm1) {
			static Benjolin b;
			b = Benjolin();
			b.Init();
			Controls c{};
			c.osc1Oct = 8 * OCT; // 128 Hz
			c.osc2Oct = 4 * OCT; // 8 Hz
			c.fm1 = fm1;
			int up = 0;
			bool last = false;
			for (int n = 0; n < 48000 * 4; n++)
			{
				b.Step(c);
				if (b.osc1.Square() && !last) up++;
				last = b.osc1.Square();
			}
			return up / 4.0;
		};
		double f0 = freqOf(0), f2 = freqOf(2 * OCT);
		printf("  osc1 mean rate: no FM %.1f Hz, 2 oct FM %.1f Hz\n", f0, f2);
		// exponential FM by a triangle over +/-2 octaves raises the mean rate
		// by the mean of 2^(2x), x uniform in -1..1: (4 - 1/4) / (4 ln 2) = 1.35
		check(fabs(f0 - 128) < 1.5, "  unmodulated rate is correct");
		check(fabs(f2 / f0 - 1.352) < 0.05, "  FM raises mean rate as expected for exponential FM");
	}

	printf("Stress test (extreme settings, external clipping input)\n");
	{
		static Benjolin b;
		b.Init();
		uint32_t rng = 1;
		int32_t maxLp = 0;
		bool bad = false;
		for (int n = 0; n < 48000 * 20; n++)
		{
			if ((n & 1023) == 0) rng = rng * 1664525 + 1013904223;
			Controls c{};
			c.osc1Oct = (int32_t)((rng >> 8) % (16 * OCT)) - OCT;
			c.osc2Oct = (int32_t)((rng >> 4) % (16 * OCT)) - OCT;
			c.run1 = 6 * OCT;
			c.run2 = 6 * OCT;
			c.cutoffOct = (int32_t)((rng >> 12) % (11 * OCT)) - OCT;
			c.runFilt = 4 * OCT;
			c.fm1 = (int32_t)((rng >> 6) % (6 * OCT));
			c.fm2 = (int32_t)((rng >> 10) % (6 * OCT));
			c.res = (rng & 0x10000) ? CONE : (int32_t)((rng >> 2) % CONE);
			c.useExtIn = (rng & 0x20000);
			c.extIn = (rng & 0x40000) ? 2 * ONE : -2 * ONE;
			c.loop = rng & 0x80000;
			b.Step(c);
			int32_t m = abs(b.filter.lp);
			if (m > maxLp) maxLp = m;
			if (abs(b.filter.vL) > (1 << 30) || abs(b.filter.vC) > (1 << 30) || abs(b.filter.i) > (1 << 30)) bad = true;
		}
		printf("  max |LP| %.3f\n", maxLp / (double)ONE);
		check(!bad && maxLp < 8 * ONE, "no overflow or runaway");
	}

	printf("Renders\n");
	{
		struct Patch
		{
			const char *name;
			float o1, o2, r1, r2, cut, rf, res;
			bool loop;
			float fm1 = 0, fm2 = 0;
		};
		Patch patches[] = {
			{"classic", 8.5f, 4.5f, 2.0f, 1.5f, 4.5f, 2.5f, 0.7f, false},
			{"loop", 9.0f, 5.5f, 3.0f, 0.5f, 4.0f, 3.0f, 0.8f, true},
			{"scream", 10.0f, 7.0f, 4.0f, 3.0f, 5.5f, 2.0f, 0.97f, false},
			{"slow_rungler", 7.5f, 2.0f, 4.0f, 0.0f, 3.5f, 3.5f, 0.6f, false},
			{"cross_fm", 9.5f, 7.0f, 1.5f, 1.0f, 5.0f, 2.0f, 0.75f, false, 2.5f, 1.5f},
		};
		for (auto &p : patches)
		{
			static Benjolin b;
			b = Benjolin();
			b.Init();
			std::vector<float> L, R;
			for (int n = 0; n < 48000 * 8; n++)
			{
				Controls c{};
				// slow sweep of filter cutoff to hear the WDF filter move
				float sweep = 1.5f * sinf(2 * M_PI * n / (48000.0f * 8));
				c.osc1Oct = (int32_t)(p.o1 * OCT);
				c.osc2Oct = (int32_t)(p.o2 * OCT);
				c.run1 = (int32_t)(p.r1 * OCT);
				c.run2 = (int32_t)(p.r2 * OCT);
				c.cutoffOct = (int32_t)((p.cut + sweep) * OCT);
				c.runFilt = (int32_t)(p.rf * OCT);
				c.res = (int32_t)(p.res * CONE);
				c.loop = p.loop;
				c.fm1 = (int32_t)(p.fm1 * OCT);
				c.fm2 = (int32_t)(p.fm2 * OCT);
				b.Step(c);
				L.push_back(0.6f * b.filter.lp / ONE);
				R.push_back(0.6f * b.filter.bp / ONE);
			}
			std::string path = out + "/" + p.name + ".wav";
			writeWav(path, L, R);
			printf("  wrote %s\n", path.c_str());
		}
	}

	printf(failures ? "\n%d FAILURES\n" : "\nAll checks passed\n", failures);
	return failures ? 1 : 0;
}
