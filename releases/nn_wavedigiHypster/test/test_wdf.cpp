// Host test: every WDF network in hypster_circuit.h against an independent
// brute-force nodal solution (bisection on the string current, in double).
//
//   g++ -O2 -std=c++17 -I../src test_wdf.cpp -o test_wdf && ./test_wdf

#include <cmath>
#include <cstdio>
#include <functional>
#include "hypster_circuit.h"

using namespace hyp;

static double Bisect(const std::function<double(double)> &g, double lo, double hi)
{
	// g decreasing in its argument
	for (int k = 0; k < 200; k++)
	{
		const double m = 0.5 * (lo + hi);
		if (g(m) > 0) lo = m; else hi = m;
	}
	return 0.5 * (lo + hi);
}

static const double Is = StringIs(), Vt = kStringVt;
static double StringV(double i) { return i > 0 ? Vt * std::log1p(i / Is) : -1e3; } // one direction
static double PairV(double i) { return Vt * std::asinh(i / (2 * Is)); }
static double DiodeV(double i, double is, double vt) { return i > -is ? vt * std::log1p(i / is) : -1e3; }

static double SatD(double v) { return OpAmpSat((float)v); }

// ---- reference solutions
static double RefZenerPair(double vin, double rL, double rM, double rF, double kPlus)
{
	auto vN = [&](double i) { return (i + kPlus * vin / rM) / (1 / rL + 1 / rM); };
	double i = Bisect([&](double i) { return vin - i * kRout - vN(i) - PairV(i); }, -0.1, 0.1);
	return SatD(kPlus * vin * (1 + rF / rM) - rF / rM * vN(i));
}

static double RefY(double z)
{
	// current i flows N -> M through the string
	const double vth = z / 2, rth = 500, rN = 2.2e3;
	double i = Bisect([&](double i) {
		// V_N = -i rN ; V_M = vth + i rth ; string voltage = V_N - V_M
		return -i * rN - (vth + i * rth) - StringV(i);
	}, 0, 0.1);
	const double vN = -i * rN, vM = vth + i * rth;
	return SatD(vN * (1 + 100.0 / 180 + 100.0 / 39) - 100.0 / 180 * vM);
}

static double RefX(double y)
{
	const double rb = 1e3 * 220e3 / 221e3;
	const double vth = y * rb / (1e3 + rb), rth = 1 / (1 / 1e3 + 1 / 1e3 + 1 / 220e3);
	const double rN = 2.2e3 * 100e3 / 102.2e3;
	double i = Bisect([&](double i) { return (vth - i * rth) - i * rN - StringV(i); }, 0, 0.1);
	const double vN = i * rN, vM = vth - i * rth;
	return SatD(-(vN + 100.0 / 220 * vM));
}

static double RefLimiter(double dI)
{
	return Bisect([&](double i) { return 10e3 * dI - 20e3 * i - DiodeV(i, kDiodeIs, 3 * kDiodeVt); }, -1e-3, 1e-3);
}

static int failures = 0;
static void Check(const char *what, double x, double got, double ref, double tol)
{
	if (!(std::fabs(got - ref) <= tol))
	{
		printf("FAIL %-28s x=%8.3f got=%.6f ref=%.6f\n", what, x, got, ref);
		failures++;
	}
}

int main()
{
	ZenerPairShaper a2(2.2e3f, 33e3f, 100e3f, 220e3f, 100e3f);
	ZenerPairShaper a8(2.2e3f, 33e3f, 100e3f, 330e3f, 100e3f);
	BreakpointShaperY a11;
	BreakpointShaperX a14;
	Limiter lim;

	double worst[5] = {0};
	for (double v = -12; v <= 12; v += 0.01)
	{
		double r, g;
		r = RefZenerPair(v, 2.2e3, 33e3, 100e3, 100.0 / 320); g = a2((float)v);
		Check("A2 (ZD1/2)", v, g, r, 2e-3); worst[0] = std::fmax(worst[0], std::fabs(g - r));
		r = RefZenerPair(v, 2.2e3, 33e3, 100e3, 100.0 / 430); g = a8((float)v);
		Check("A8 (ZD3/4)", v, g, r, 2e-3); worst[1] = std::fmax(worst[1], std::fabs(g - r));
		r = RefY(v); g = a11((float)v);
		Check("A11 (D6/ZD5)", v, g, r, 2e-3); worst[2] = std::fmax(worst[2], std::fabs(g - r));
		r = RefX(v); g = a14((float)v);
		Check("A14 (D7/ZD6)", v, g, r, 2e-3); worst[3] = std::fmax(worst[3], std::fabs(g - r));
	}
	for (double dI = -0.4e-3; dI <= 0.4e-3; dI += 1e-6)
	{
		const double r = RefLimiter(dI), g = lim((float)dI);
		Check("Limiter", dI * 1e3, g * 1e6, r * 1e6, 0.05); // microamps
		worst[4] = std::fmax(worst[4], std::fabs(g - r) * 1e6);
	}

	// Shape network: option 2 asymmetric conducts only for X > ~0.6V and
	// pulls the damping nodes negative; symmetric is odd; option 1 mild is
	// linear; 'fat' pushes more into A than B.
	ShapeNetwork sh;
	sh.Configure(ShapeMode::Opt2Asym, 0.0f);
	ShapeOut lo = sh(-5.0f), hi = sh(5.0f);
	Check("Opt2 asym off for X<0", -5, lo.vA, 0, 1e-6);
	if (!(hi.vA < -1e-3 && hi.vB < 0 && hi.vA < hi.vB)) { printf("FAIL Opt2 asym fat: vA=%g vB=%g\n", hi.vA, hi.vB); failures++; }
	// reference: series loop  -X - 22k - D8 - W - Rpar
	{
		const double rA = 100e3 + 220, rB = 200e3 + 220, rP = rA * rB / (rA + rB);
		for (double x = -10; x <= 10; x += 0.05)
		{
			// current i from W through D8 into -X' ; V_W = -i rP
			double i = Bisect([&](double i) { return (-i * rP) - (-x + i * (22e3 + kRout)) - DiodeV(i, kDiodeIs, kDiodeVt); }, -1e-6, 1e-3);
			const double vW = -i * rP;
			Check("Opt2 asym vA", x, sh((float)x).vA, vW * 220 / rA, 2e-6);
		}
	}
	sh.Configure(ShapeMode::Opt2Symm, 0.5f);
	ShapeOut p = sh(4.0f), n = sh(-4.0f);
	Check("Opt2 symm odd", 4, p.vA, -n.vA, 1e-6);
	sh.Configure(ShapeMode::Opt1CurlyMild, 0.0f);
	Check("Opt1 mild linear", 6, sh(6.0f).vB, 2.0 * sh(3.0f).vB, 1e-7);
	sh.Configure(ShapeMode::Opt1SquirrelyWild, 0.0f);
	{
		// reference: -X - 82k - J ; J-P: 470k || diode pair ; P - Rnet
		const double toA = 220e3 + 220, toB = 182e3 + 220, rN = toA * toB / (toA + toB);
		for (double x = -10; x <= 10; x += 0.05)
		{
			// unknown V_P; current J->P = V_P / rN
			double vP = Bisect([&](double vP) {
				const double i = vP / rN;            // current into P from J
				const double vJ = -x - i * (82e3 + kRout);
				// current J->P through 470k || pair must equal i
				const double iJP = (vJ - vP) / 470e3 + 2 * kDiodeIs * std::sinh((vJ - vP) / kDiodeVt);
				return iJP - i;
			}, -12, 12);
			Check("Opt1 wild vA", x, sh((float)x).vA, vP * 220 / toA, 2e-6);
		}
	}

	printf("worst |err|  A2 %.2e V   A8 %.2e V   A11 %.2e V   A14 %.2e V   limiter %.3f uA\n",
	       worst[0], worst[1], worst[2], worst[3], worst[4]);
	printf("string Is_eff = %.3e A, Vt_eff = %.3f V\n", (double)StringIs(), (double)kStringVt);
	printf("%s\n", failures ? "FAILED" : "all WDF networks match the nodal reference");
	return failures ? 1 : 0;
}
