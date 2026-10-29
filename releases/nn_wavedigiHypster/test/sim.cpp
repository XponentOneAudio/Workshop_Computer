// Host simulator for the Hypster core.
//
//   g++ -O2 -std=c++17 -I../src sim.cpp -o sim
//   ./sim ref                 fixed-point WDF core vs. continuous-time reference
//   ./sim map  <rateOct> <shapeMode> <pot> [limiter]   regime map over gain x damping
//   ./sim dump <rateOct> <gainV> <dampV> <shapeMode> <pot> <seconds> [limiter] > out.csv
//
// The reference model integrates the circuit ODEs with RK4 at 64x the sample
// rate in double precision, using the float WDF networks directly (no
// tables, no fixed point).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "hypster_core.h"

using namespace hyp;

static ShaperTables tabs;
static ShapeTables shapeTab;

static Controls MakeControls(double rateOct, double gainV, double dampV, bool lim)
{
	Controls c{};
	c.rateOct = (int32_t)lrint(rateOct * 65536);
	c.gainV = (int32_t)lrint(gainV * 65536);
	c.dampV = (int32_t)lrint(dampV * 65536);
	c.trimV = 0;
	c.limiter = lim;
	c.hold = false;
	return c;
}

// ------------------------------------------------------------ reference
// Dense float tables (1/512V) of the same WDF networks, so that the
// reference is independent of the fixed-point tables and interpolation.
struct Dense
{
	static constexpr int N = 32 * 512 + 1;
	std::vector<double> t;
	template <class F> void Fill(F f)
	{
		t.resize(N);
		for (int k = 0; k < N; k++) t[k] = f(-16.0 + k / 512.0);
	}
	double operator()(double v) const
	{
		double p = (v + 16.0) * 512.0;
		if (p < 0) p = 0;
		if (p > N - 1.001) p = N - 1.001;
		const int i = (int)p;
		const double f = p - i;
		return t[i] + f * (t[i + 1] - t[i]);
	}
};

struct Ref
{
	Dense nlU, nlZ, nlY, nlX, shA, shB;
	double Ia, Ic, Ib;
	double s[4]; // u z y x

	void Set(double rateOct, double gainV, double dampV, ShapeMode m, double pot, bool lim)
	{
		Ia = kIaMin * std::exp2(rateOct);
		Ic = std::fmax(0.0, kGainPerV * gainV) / kRemit;
		Ib = std::fmax(0.0, kDampPerV * dampV + kDampOffsetV) / kRemit;
		if (lim && Ic > Ib)
		{
			Limiter l;
			const double id = l((float)(Ic - Ib));
			Ic -= id;
			Ib += id;
		}
		ZenerPairShaper a2{2.2e3f, 33e3f, 100e3f, 220e3f, 100e3f};
		ZenerPairShaper a8{2.2e3f, 33e3f, 100e3f, 330e3f, 100e3f};
		BreakpointShaperY a11;
		BreakpointShaperX a14;
		ShapeNetwork sh;
		sh.Configure(m, (float)pot);
		nlU.Fill([&](double v) { return a2(OpAmpSat((float)v)); });
		nlZ.Fill([&](double v) { return a8((float)v); });
		nlY.Fill([&](double v) { return a11((float)v); });
		nlX.Fill([&](double v) { return a14((float)v); });
		shA.Fill([&](double v) { return sh((float)v).vA; });
		shB.Fill([&](double v) { return sh((float)v).vB; });
	}
	void Deriv(const double *v, double *d)
	{
		const double u = v[0], z = v[1], y = v[2], x = v[3];
		const double va1 = Ic * kR5 * std::tanh(kAinG * x / kVT2);
		const double dampU = kRdiv * Ib * std::tanh(kAdU * u / kVT2); // U3 into R13
		const double g = Ia / kCint;
		d[0] = g * std::tanh((kAinU * nlU(va1) - dampU) / kVT2);
		d[1] = g * std::tanh((kAinZ * nlZ(u) - kAdZ * z) / kVT2);
		d[2] = g * std::tanh((kAinY * nlY(z) - kAdY * y - shB(x)) / kVT2);
		d[3] = g * std::tanh((kAinX * nlX(y) - kAdX * x - shA(x)) / kVT2);
	}
	void Step(double T, int sub)
	{
		const double h = T / sub;
		for (int k = 0; k < sub; k++)
		{
			double k1[4], k2[4], k3[4], k4[4], t[4];
			Deriv(s, k1);
			for (int i = 0; i < 4; i++) t[i] = s[i] + 0.5 * h * k1[i];
			Deriv(t, k2);
			for (int i = 0; i < 4; i++) t[i] = s[i] + 0.5 * h * k2[i];
			Deriv(t, k3);
			for (int i = 0; i < 4; i++) t[i] = s[i] + h * k3[i];
			Deriv(t, k4);
			for (int i = 0; i < 4; i++)
			{
				s[i] += h / 6 * (k1[i] + 2 * k2[i] + 2 * k3[i] + k4[i]);
				s[i] = std::fmax(-kRail, std::fmin(kRail, s[i]));
			}
		}
	}
};

// ------------------------------------------------------------ helpers
struct Stats
{
	double mean[4] = {0}, rms[4] = {0}, railFrac = 0, freq = 0;
};

// zero crossings of x (with hysteresis) -> frequency
static Stats Measure(const std::vector<float> *sig, double fs)
{
	Stats st;
	const size_t n = sig[0].size();
	for (int c = 0; c < 4; c++)
	{
		double m = 0, r = 0;
		for (float v : sig[c]) m += v;
		m /= n;
		for (float v : sig[c]) r += (v - m) * (v - m);
		st.mean[c] = m;
		st.rms[c] = std::sqrt(r / n);
	}
	int rail = 0, cross = 0;
	bool pos = sig[3][0] > st.mean[3];
	const double hy = 0.1 * st.rms[3] + 1e-4;
	for (size_t i = 0; i < n; i++)
	{
		for (int c = 0; c < 4; c++)
			if (std::fabs(sig[c][i]) > kRail - 0.05) { rail++; break; }
		const double v = sig[3][i] - st.mean[3];
		if (!pos && v > hy) { pos = true; cross++; }
		else if (pos && v < -hy) pos = false;
	}
	st.railFrac = (double)rail / n;
	st.freq = cross / (n / fs);
	return st;
}

// Largest Lyapunov exponent estimate (per second) from two fixed-point cores
static double Lyapunov(const Controls &c, double seconds)
{
	Core a, b;
	a.Init(&tabs, &shapeTab);
	a.UpdateRate(c.rateOct);
	a.UpdateCurrents(c);
	for (int i = 0; i < (int)(kFs * 1.0); i++) a.Process(); // settle
	b = a;
	const int n = (int)(seconds * kFs);
	double sum = 0;
	int renorm = 0;
	const double d0 = 1e-3;
	// perturb b's x
	auto dist = [&]() {
		const double du = FromQ24(a.U() - b.U()), dz = FromQ24(a.Z() - b.Z());
		const double dy = FromQ24(a.Y() - b.Y()), dx = FromQ24(a.X() - b.X());
		return std::sqrt(du * du + dz * dz + dy * dy + dx * dx);
	};
	b = a;
	b.Nudge(3, ToQ24((float)d0));
	const int every = 64;
	for (int i = 0; i < n; i++)
	{
		a.Process();
		b.Process();
		if (i % every == every - 1)
		{
			const double d = dist();
			if (d > 0)
			{
				sum += std::log(d / d0);
				renorm++;
				b.CopyScaledDelta(a, (float)(d0 / d));
			}
		}
	}
	return renorm ? sum / (renorm * every / kFs) : 0;
}

static ShapeMode ParseMode(const char *s) { return (ShapeMode)atoi(s); }

int main(int argc, char **argv)
{
	BuildShaperTables(tabs);
	if (argc < 2) { fprintf(stderr, "usage: see header\n"); return 1; }

	if (!strcmp(argv[1], "ref"))
	{
		// Periodic and chaotic settings across the rate range; compare the
		// fixed-point WDF core at 48kHz with the RK4 reference.
		struct Case { double rate, gain, damp; int mode; double pot; };
		const Case cases[] = {
			{4, 4.5, 9, 0, 0.5}, {8, 4.5, 9, 0, 0.5}, {11, 4.5, 9, 0, 0.5}, {13, 4.5, 9, 0, 0.5},
			{14, 4.5, 9, 0, 0.5}, {15, 4.5, 9, 0, 0.5}, {16, 4.5, 9, 0, 0.5},
			{10, 3.0, 6, 0, 0.5}, {14, 3.0, 6, 0, 0.5}, {10, 10.5, 12, 0, 0.5}, {14, 10.5, 12, 0, 0.5},
			{12, 6.0, 6, 1, 0.5}, {12, 6.0, 6, 2, 0.5}, {12, 9.0, 9, 3, 0.5}, {12, 6.0, 6, 4, 0.5}, {12, 9.0, 9, 5, 0.5},
		};

		printf("rate  gain damp mode |  f_ref    f_wdf   | rms x ref  wdf  | rms u ref  wdf\n");
		for (const Case &cs : cases)
		{
			BuildShapeTables(shapeTab, (ShapeMode)cs.mode, (float)cs.pot);
			Core core;
			core.Init(&tabs, &shapeTab);
			Controls c = MakeControls(cs.rate, cs.gain, cs.damp, false);
			core.UpdateRate(c.rateOct);
			core.UpdateCurrents(c);
			Ref ref;
			ref.Set(cs.rate, cs.gain, cs.damp, (ShapeMode)cs.mode, cs.pot, false);
			ref.s[0] = FromQ24(core.U()); ref.s[1] = FromQ24(core.Z());
			ref.s[2] = FromQ24(core.Y()); ref.s[3] = FromQ24(core.X());
			const double secs = std::fmin(10.0, std::fmax(2.0, 3000.0 / std::exp2(cs.rate)));
			const int n = (int)(secs * kFs), skip = n / 4;
			std::vector<float> A[4], B[4];
			for (int i = 0; i < n; i++)
			{
				core.Process();
				ref.Step(1.0 / kFs, 16);
				if (i < skip) continue;
				A[0].push_back(FromQ24(core.U())); A[1].push_back(FromQ24(core.Z()));
				A[2].push_back(FromQ24(core.Y())); A[3].push_back(FromQ24(core.X()));
				for (int k = 0; k < 4; k++) B[k].push_back((float)ref.s[k]);
			}
			Stats sa = Measure(A, kFs), sb = Measure(B, kFs);
			printf("%4.1f %5.1f %4.1f %4d | %8.2f %8.2f | %6.2f %6.2f | %6.2f %6.2f\n",
			       cs.rate, cs.gain, cs.damp, cs.mode, sb.freq, sa.freq, sb.rms[3], sa.rms[3], sb.rms[0], sa.rms[0]);
		}
		return 0;
	}

	if (!strcmp(argv[1], "map") && argc >= 5)
	{
		const double rate = atof(argv[2]);
		BuildShapeTables(shapeTab, ParseMode(argv[3]), (float)atof(argv[4]));
		const bool lim = argc > 5 && atoi(argv[5]);
		printf("# rows: damping V (0..12), cols: gain V (0..12). cell: . quiet  o periodic  * chaotic  R rails\n");
		printf("      ");
		for (double g = 0; g <= 12.01; g += 0.75) printf("%5.1f", g);
		printf("\n");
		for (double d = 0; d <= 12.01; d += 0.75)
		{
			printf("%5.2f ", d);
			for (double g = 0; g <= 12.01; g += 0.75)
			{
				Controls c = MakeControls(rate, g, d, lim);
				Core core;
				core.Init(&tabs, &shapeTab);
				core.UpdateRate(c.rateOct);
				core.UpdateCurrents(c);
				const int n = (int)(kFs * 3);
				std::vector<float> s[4];
				for (int i = 0; i < n; i++)
				{
					core.Process();
					if (i > n / 2)
					{
						s[0].push_back(FromQ24(core.U())); s[1].push_back(FromQ24(core.Z()));
						s[2].push_back(FromQ24(core.Y())); s[3].push_back(FromQ24(core.X()));
					}
				}
				Stats st = Measure(s, kFs);
				char ch;
				if (st.railFrac > 0.5) ch = 'R';
				else if (st.rms[3] < 0.05 && st.rms[0] < 0.05) ch = '.';
				else
				{
					const double ly = Lyapunov(c, 1.5);
					ch = ly > 2.0 ? '*' : 'o';
				}
				printf("    %c", ch);
			}
			printf("\n");
		}
		return 0;
	}

	if (!strcmp(argv[1], "dump") && argc >= 8)
	{
		const double rate = atof(argv[2]), gain = atof(argv[3]), damp = atof(argv[4]);
		BuildShapeTables(shapeTab, ParseMode(argv[5]), (float)atof(argv[6]));
		const double secs = atof(argv[7]);
		const bool lim = argc > 8 && atoi(argv[8]);
		Core core;
		core.Init(&tabs, &shapeTab);
		Controls c = MakeControls(rate, gain, damp, lim);
		core.UpdateRate(c.rateOct);
		core.UpdateCurrents(c);
		const int n = (int)(secs * kFs);
		printf("u,z,y,x\n");
		for (int i = 0; i < n; i++)
		{
			core.Process();
			if (i > n / 5)
				printf("%.4f,%.4f,%.4f,%.4f\n", FromQ24(core.U()), FromQ24(core.Z()), FromQ24(core.Y()), FromQ24(core.X()));
		}
		return 0;
	}
	fprintf(stderr, "bad args\n");
	return 1;
}
