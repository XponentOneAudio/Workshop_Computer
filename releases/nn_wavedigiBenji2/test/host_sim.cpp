// Host-side tests and renders for the Benjolin Gorga circuit model.
//
//   g++ -O2 -std=c++17 -I.. host_sim.cpp -o host_sim && ./host_sim [outdir]
//
// The filter is checked against an independent frequency-domain solve of the
// circuit equations written directly from the netlist (not the reduced
// coefficients the model uses). Oscillator rates are checked against the
// analytic integrator/Schmitt period, and the rungler DAC and XOR against
// hand-derived values.

#include "benjolin_circuit.h"

#include <algorithm>
#include <complex>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

using namespace benji;
using cd = std::complex<double>;

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
	uint32_t n = l.size(), dataBytes = n * 4, sr = 48000, byteRate = sr * 4, chunk = 36 + dataBytes, sixteen = 16;
	uint16_t fmt = 1, ch = 2, align = 4, bits = 16;
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

// Filter transfer function Vext -> OUT, straight from the circuit equations
static cd circuitLP(double w, double g, double beta)
{
	using namespace part;
	cd s(0, w);
	double Gx = 1 / R35 + 1 / R36 + 1 / R38 + 1 / R44 + 1 / R31;
	double rho = R48 / R44;
	// unknowns OUT, N with Vext = 1. Linear in (OUT, N): write each equation
	// as a0 + a1 OUT + a2 N = 0
	// X = (N/R44)/Gx
	cd xN = (1 / R44) / Gx;
	cd iExt = 1.0 / ((double)R30 + 1.0 / (s * (double)C5));
	// U = -R33 (iExt + X/R31 + OUT/R32)
	cd u0 = -(double)R33 * iExt, uO = -(double)R33 / R32, uN = -(double)R33 * xN / (double)R31;
	// Y = beta (N (1 + rho) - rho X)
	cd yN = beta * ((1 + rho) - rho * xN);
	// eq1: s C10 OUT + g (N/R47 + OUT/R50) = 0
	cd a11 = s * (double)C10 + g / R50, a12 = g / R47, b1 = 0;
	// eq2: s C8 N + g (U/R34 + Y/R39 + s C7 N) = 0
	cd a21 = g * uO / (double)R34;
	cd a22 = s * (double)C8 + g * (uN / (double)R34 + yN / (double)R39 + s * (double)C7);
	cd b2 = -g * u0 / (double)R34;
	cd det = a11 * a22 - a12 * a21;
	return (b1 * a22 - a12 * b2) / det; // OUT
}

static double measureLP(int32_t g, float beta, double f)
{
	static Filter flt;
	flt = Filter();
	flt.Init();
	flt.SetResonance(beta);
	const int N = 48000;
	double amp = 0.05;
	// correlate with the input frequency, so a static rounding offset or
	// residual ringing at other frequencies isn't counted as response
	double c = 0, sn = 0;
	int cnt = 0;
	for (int n = 0; n < N; n++)
	{
		double x = amp * sin(2 * M_PI * f * n / 48000.0);
		flt.Step(g, 0, 0, Volts((float)x));
		if (n > N / 2)
		{
			double y = flt.out / (double)V1;
			c += y * cos(2 * M_PI * f * n / 48000.0);
			sn += y * sin(2 * M_PI * f * n / 48000.0);
			cnt++;
		}
	}
	return 2 * sqrt(c * c + sn * sn) / cnt / amp;
}

// Rate of oscillator 1 alone (cross mod and rungler off)
static double oscRate(float rate)
{
	static Benjolin b;
	b = Benjolin();
	b.Init();
	b.SetPot(POT_RATE1, rate);
	b.SetPot(POT_RUN1, 0);
	b.SetPot(POT_CV1, 0);
	int edges = 0;
	bool last = false;
	const int N = 48000 * 4;
	for (int n = 0; n < N; n++)
	{
		b.Step(0, 0, 0, 0);
		if (b.osc1.high && !last) edges++;
		last = b.osc1.high;
	}
	return edges / 4.0;
}

int main(int argc, char **argv)
{
	std::string out = argc > 1 ? argv[1] : ".";
	using namespace part;

	printf("Oscillator rate vs integrator/Schmitt analysis\n");
	{
		double worst = 0;
		for (float k : {1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f})
		{
			double a = 1 - k;
			double ctrl = R_CTRL_FB * 9.0 * a / (R_FREQ + a * (1 - a) * R_POT);
			double g = pow(2.0, -ctrl / SSM_V_PER_OCT);
			// half period = 2 Vth / (g VSAT / (R C)), Vth = VSAT R_HYST_A / R_HYST_B
			double f = g / (4.0 * (R_HYST_A / R_HYST_B) * R_OSC_IN * C_OSC);
			double m = oscRate(k);
			double err = fabs(m / f - 1);
			worst = std::max(worst, f > 20 ? err : 0.0);
			printf("  knob %.1f: CTRL %.3f V, predicted %8.2f Hz, model %8.2f Hz\n", k, ctrl, f, m);
		}
		check(worst < 0.01, "rates within 1% of the analytic value (above 20 Hz)");
	}

	printf("Rungler DAC and XOR\n");
	{
		Rungler r;
		r.Init();
		// closed form for the R-2R ladder: RUN = -(Q6 + Q7/2 + Q8/4) R29 / R23
		bool ok = true;
		for (int s = 0; s < 8; s++)
		{
			double q6 = (s & 1) ? 9 : -9, q7 = (s & 2) ? 9 : -9, q8 = (s & 4) ? 9 : -9;
			double ref = -(q6 + q7 / 2 + q8 / 4) * R29 / R23;
			if (fabs(r.dacTable[s] / (double)V1 - ref) > 1e-3) ok = false;
		}
		printf("  RUN_CV range %.3f .. %.3f V\n", r.dacTable[7] / (double)V1, r.dacTable[0] / (double)V1);
		check(ok, "DAC levels match the R-2R closed form");
		check(Rungler::XorData(true, false) && !Rungler::XorData(false, false) &&
				  !Rungler::XorData(true, true) && Rungler::XorData(false, true),
			  "XOR: data = Q8 with Q1 off, NOT Q8 with Q1 on");

		auto period = [](float chaos) {
			static Benjolin b;
			b = Benjolin();
			b.Init();
			b.SetSW1(true);
			b.SetPot(POT_CHAOS, chaos);
			std::vector<uint8_t> seq;
			for (int n = 0; n < 48000 * 4; n++)
			{
				uint8_t before = b.rungler.reg;
				b.Step(0, 0, 0, 0);
				if (b.rungler.reg != before || seq.empty()) seq.push_back(b.rungler.reg);
			}
			// smallest p such that the tail repeats with period p clocks
			for (int p = 1; p < 64; p++)
			{
				bool rep = true;
				for (size_t i = seq.size() - 100; i < seq.size(); i++)
					if (seq[i] != seq[i - p]) rep = false;
				if (rep) return p;
			}
			return -1;
		};
		int pLoop = period(0.0f), pInv = period(1.0f);
		printf("  SW1 on, chaos CCW: pattern period %d register changes; CW: %d\n", pLoop, pInv);
		check(pLoop > 0 && 8 % pLoop == 0, "chaos fully CCW locks into an 8-step loop");
		check(pInv > 0 && 16 % pInv == 0, "chaos fully CW locks into a 16-step inverted loop");
	}

	printf("Filter vs circuit equations (linear region)\n");
	{
		double worst = 0;
		for (double g : {0.002, 0.02, 0.2, 0.7})
			for (float beta : {0.05f, 0.2f, 0.6f})
				for (double f : {50.0, 200.0, 1000.0, 4000.0, 12000.0})
				{
					// trapezoidal = bilinear transform: evaluate the analogue
					// response at the prewarped frequency
					double wa = 2 * FS * tan(M_PI * f / FS);
					double ref = std::abs(circuitLP(wa, g, beta));
					if (ref < 1e-3) continue; // below what the test signal resolves
					double m = measureLP((int32_t)(g * G1), beta, f);
					double err = 20 * log10(m / ref);
					if (fabs(err) > fabs(worst)) worst = err;
					if (fabs(err) > 0.5)
						printf("    g=%.3f beta=%.2f f=%6.0f model %.4f circuit %.4f (%.2f dB)\n", g, beta, f, m, ref, err);
				}
		printf("  worst deviation %.3f dB\n", worst);
		check(fabs(worst) < 0.5, "LP response within 0.5 dB of the circuit (bilinear-warped)");

		// peak frequency and Q at maximum resonance
		for (double g : {0.01, 0.1})
		{
			double best = 0, fpk = 0;
			for (double f = 20; f < 20000; f *= 1.002)
			{
				double m = std::abs(circuitLP(2 * M_PI * f, g, 0.0));
				if (m > best) best = m, fpk = f;
			}
			double dc = std::abs(circuitLP(2 * M_PI * fpk / 8, g, 0.0)); // passband, above the C5 high-pass
			printf("  g=%.2f, resonance max: peak %.0f Hz, %.1f dB above passband\n", g, fpk, 20 * log10(best / dc));
		}
	}

	printf("Stress test (random pots, cross mod, external input)\n");
	{
		static Benjolin b;
		b = Benjolin();
		b.Init();
		uint32_t rng = 7;
		bool bad = false;
		int32_t maxOut = 0, maxN = 0;
		for (int n = 0; n < 48000 * 20; n++)
		{
			if ((n % 2400) == 0)
			{
				for (int p = 0; p < NUM_POTS; p++)
				{
					rng = rng * 1664525 + 1013904223;
					b.SetPot(p, (rng >> 8) / 16777216.0f);
				}
				b.SetSW1(rng & 1);
			}
			int32_t ext = (n & 64) ? Volts(6) : Volts(-6);
			b.Step(b.osc2.T, b.osc1.T, b.osc2.T, (rng & 2) ? ext : 0);
			maxOut = std::max(maxOut, abs(b.filter.outAmp));
			maxN = std::max(maxN, abs(b.filter.n));
			if (abs(b.filter.s10) > 60 * V1 || abs(b.filter.s8) > 60 * V1 || abs(b.filter.s7) > 60 * V1) bad = true;
		}
		printf("  max |OUT_AMP| %.2f V, max |N| %.2f V\n", maxOut / (double)V1, maxN / (double)V1);
		check(!bad, "filter states stay bounded");
	}

	printf("Renders\n");
	{
		struct Patch
		{
			const char *name;
			float rate1, run1, cv1, rate2, run2, cv2, cut, runf, res, chaos, fcv;
			bool sw1;
		};
		Patch patches[] = {
			{"gorga_classic", 0.62f, 0.35f, 0.0f, 0.45f, 0.25f, 0.0f, 0.55f, 0.4f, 0.8f, 0.5f, 0.0f, false},
			{"gorga_crossmod", 0.7f, 0.2f, 0.4f, 0.55f, 0.3f, 0.3f, 0.6f, 0.3f, 0.9f, 0.5f, 0.0f, false},
			{"gorga_loop", 0.65f, 0.5f, 0.0f, 0.5f, 0.1f, 0.0f, 0.5f, 0.5f, 0.85f, 0.0f, 0.0f, true},
			{"gorga_scream", 0.75f, 0.5f, 0.2f, 0.6f, 0.4f, 0.1f, 0.65f, 0.3f, 1.0f, 0.5f, 0.3f, false},
		};
		for (auto &p : patches)
		{
			static Benjolin b;
			b = Benjolin();
			b.Init();
			float v[NUM_POTS] = {p.rate1, p.run1, p.cv1, p.rate2, p.run2, p.cv2, p.cut, p.runf, p.res, p.chaos, p.fcv};
			for (int i = 0; i < NUM_POTS; i++) b.SetPot(i, v[i]);
			b.SetSW1(p.sw1);
			std::vector<float> L, R;
			for (int n = 0; n < 48000 * 8; n++)
			{
				// slow cutoff sweep
				if ((n & 255) == 0) b.SetPot(POT_CUTOFF, p.cut + 0.15f * sinf(2 * (float)M_PI * n / (48000.0f * 8)));
				b.Step(b.osc2.T, b.osc1.T, b.osc2.T, 0);
				L.push_back(0.12f * b.filter.outAmp / V1);
				R.push_back(0.06f * b.pwm / V1);
			}
			std::string path = out + "/" + p.name + ".wav";
			writeWav(path, L, R);
			printf("  wrote %s\n", path.c_str());
		}
	}

	printf(failures ? "\n%d FAILURES\n" : "\nAll checks passed\n", failures);
	return failures ? 1 : 0;
}
