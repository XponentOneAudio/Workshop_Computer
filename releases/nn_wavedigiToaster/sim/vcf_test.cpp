// vcf_test.cpp - checks the fixed-point VCF WDF against the analytic transfer
// function of the circuit (bilinear-mapped), for several pot settings.
//
//   Vo / (R35 I_in) = -(R35 || Z_T) / R35,
//   Z_T = 1/sC9 + R29 + 1/sC10 + (1/sC9)(R29 + 1/sC10) / Rg
//
// Each test frequency is measured with a steady-state sine (small enough to
// stay off the op-amp rails), so the result includes the fixed-point noise.
//
// Build & run:  g++ -O2 -std=c++17 vcf_test.cpp -o vcf_test && ./vcf_test

#include <cstdio>
#include <cmath>
#include <complex>
#include "../src/toaster_model.h"

using cd = std::complex<double>;

int main()
{
	const double fs = 48000.0;
	struct Case { float cutoff, reso; } cases[] = {
		{0.0f, 0.0f}, {0.5f, 0.3f}, {0.8f, 0.6f}, {1.0f, 0.8f}, {0.9f, 1.0f}, {0.3f, 1.0f}};
	int fails = 0;
	for (auto c : cases)
	{
		wdt::Controls ctl;
		ctl.cutoff = c.cutoff; ctl.reso = c.reso; ctl.vcfMod = 0.0f;
		double rg, r29 = 100e3 * (1.0 - c.reso) * (1.0 - c.reso) + 100.0;
		{
			wdt::NoiseToaster m;
			m.ControlAll(ctl, 0, 0);
			rg = m.VcfRg();
		}
		const double C = 1e-9, R35 = 4.7e6;
		double f0 = 1.0 / (2 * M_PI * C * sqrt(rg * (R35 + r29)));
		double Q = sqrt(rg * (R35 + r29)) / (2 * rg + r29);
		double worstPass = 0, worstStop = 0;
		for (double f = 30; f < 16000; f *= 1.15)
		{
			cd z = std::polar(1.0, 2 * M_PI * f / fs);
			cd s = 2 * fs * (z - 1.0) / (z + 1.0);
			cd zt = 1.0 / (s * C) + r29 + 1.0 / (s * C) + (1.0 / (s * C)) * (r29 + 1.0 / (s * C)) / rg;
			double ga = std::abs(-(R35 * zt / (R35 + zt)) / R35);

			// keep the output around 1 V peak
			double amp = std::min(0.5, 1.0 / ga);
			wdt::NoiseToaster m;
			m.ControlAll(ctl, 0, 0);
			int settle = 24000, meas = 48000;
			cd acc = 0;
			for (int n = 0; n < settle + meas; n++)
			{
				double x = amp * sin(2 * M_PI * f / fs * n);
				double y = wdt::toVolts(m.VcfTick(wdt::V((float)x)));
				if (n >= settle) acc += y * std::polar(1.0, -2 * M_PI * f / fs * n);
			}
			double gw = 2.0 * std::abs(acc) / meas / amp;
			double err = fabs(20 * log10(gw / ga));
			if (20 * log10(ga) > -30) worstPass = std::max(worstPass, err);
			else worstStop = std::max(worstStop, err);
		}
		bool ok = worstPass < 0.05 && worstStop < 0.5;
		if (!ok) fails++;
		printf("cutoff %.2f reso %.2f: Rg %7.0f R29 %7.0f  f0 %6.0f Hz Q %5.1f | max err %.4f dB (> -30 dB), %.3f dB (below) %s\n",
		       c.cutoff, c.reso, rg, r29, f0, Q, worstPass, worstStop, ok ? "OK" : "FAIL");
	}
	return fails;
}
