// WDF Benjolin - DSP core
//
// A Benjolin (after Rob Hordijk) where the analogue parts that shape the sound
// are modelled as Wave Digital Filters:
//
//   * Two triangle-core oscillators (current source charging a capacitor,
//     Schmitt trigger reversing the current)
//   * Rungler: 8-bit shift register clocked by oscillator 2, data from
//     oscillator 1 XOR the register output (or recirculating in "loop")
//   * Rungler DAC: WDF of the 4R/2R/R resistor network summing into a
//     smoothing capacitor
//   * Cross FM: each oscillator's rate is modulated by the other's triangle
//   * PWM comparator (tri1 > tri2), band-limited with polyBLEP, feeding
//   * Resonant filter: WDF of a voltage-controlled series RLC with a
//     piecewise-linear "negative resistance + current limiter" in the loop
//     as the (non-adapted) root, giving resonance up to and beyond
//     self-oscillation with a limited amplitude.
//
// This header has no Pico SDK dependencies so it can be built and tested on a
// desktop machine (see test/host_sim.cpp).
//
// Fixed point conventions:
//   signals / wave variables : Q24  (1.0 = 1 << 24)
//   adaptor coefficients     : Q30  (1.0 = 1 << 30)
//   pitches                  : Q16 octaves

#pragma once

#include <cstdint>
#include <cmath>

#ifndef __not_in_flash_func
#define __not_in_flash_func(f) f
#endif

namespace wdfb
{

static constexpr int32_t ONE = 1 << 24;	  // Q24 unit
static constexpr int32_t CONE = 1 << 30;  // Q30 unit
static constexpr int32_t OCT = 1 << 16;	  // one octave, Q16
static constexpr float SAMPLE_RATE = 48000.0f;

static inline int32_t mulq30(int32_t a, int32_t c)
{
	return (int32_t)(((int64_t)a * c) >> 30);
}

static inline int32_t clampi(int32_t x, int32_t lo, int32_t hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

////////////////////////////////////////////////////////////////////////////////
// Triangle-core oscillator
//
// An ideal current source charging a capacitor between Schmitt thresholds +/-1.
// Per sample the capacitor voltage changes by I*T/C = 4 f / fs; at each
// threshold the overshoot is reflected so the period is exact.

struct TriangleCore
{
	int32_t v = 0;	 // capacitor voltage, Q24, -ONE..ONE
	int32_t dir = 1; // Schmitt output: +1 charging, -1 discharging

	void Step(int32_t inc)
	{
		v += dir > 0 ? inc : -inc;
		if (v > ONE)
		{
			v = 2 * ONE - v;
			dir = -1;
		}
		else if (v < -ONE)
		{
			v = -2 * ONE - v;
			dir = 1;
		}
	}
	bool Square() const { return dir > 0; }
};

////////////////////////////////////////////////////////////////////////////////
// Rungler DAC: three resistive voltage sources (bits, through 4R, 2R, R) and a
// capacitor on one parallel junction. All ports are adapted, so the junction
// is evaluated directly as the root:
//   v = sum(G_k b_k) / sum(G_k),  capacitor state <- 2v - b_C

struct RunglerDAC
{
	int32_t srcSum[8]; // sum(G_k b_k) / sum(G) for each bit pattern, Q24
	int32_t gCap;	   // G_C / sum(G), Q30
	int32_t sC = 0;	   // capacitor wave state, Q24

	void Init(float tau_seconds)
	{
		const float G[3] = {0.25f, 0.5f, 1.0f}; // 4R, 2R, R  (bit 5, 6, 7)
		const float Gsrc = G[0] + G[1] + G[2];
		// Capacitor port resistance R_C = T / 2C, with C set so that
		// tau = C * (R || 2R || 4R) = C / Gsrc
		const float C = tau_seconds * Gsrc;
		const float GC = 2.0f * C * SAMPLE_RATE;
		const float sum = Gsrc + GC;
		// The voltage sources are adapted and reflect their source voltage
		// (0 or 1), so their contribution only depends on the bit pattern
		for (int bits = 0; bits < 8; bits++)
		{
			float v = 0;
			for (int k = 0; k < 3; k++)
				if (bits & (1 << k)) v += G[k] / sum;
			srcSum[bits] = (int32_t)(v * ONE);
		}
		gCap = (int32_t)(GC / sum * CONE);
	}

	// bits: bit 0 = register bit 5, bit 1 = bit 6, bit 2 = bit 7
	// returns 0 .. ONE
	int32_t Step(uint32_t bits)
	{
		int32_t v = mulq30(sC, gCap) + srcSum[bits & 7];
		sC = 2 * v - sC;
		return v;
	}
};

////////////////////////////////////////////////////////////////////////////////
// Resonant filter
//
//   Vin --[R_s]--[L]--[NL]--+-- LP out (across C)
//                           |
//                          [C]
//                           |
//                          GND
//
// WDF tree:  root NL  <-  series adaptor { Vin(R_s), L, C }
//
// Units are normalised so the tank impedance sqrt(L/C) = 1 and R_s = 1
// (passive Q of 1). Cutoff is set by scaling L and C together with the
// bilinear-transform prewarped k = tan(pi fc / fs):  R_L = 1/k, R_C = k.
//
// NL is a piecewise-linear current-controlled one-port in the series loop:
//   v = -r_n i                          for |i| <= I_d
//   v = -r_n I_d sgn(i) + r_d (i - I_d sgn(i))   beyond
// The negative resistance r_n (the resonance control) cancels R_s, so Q
// rises without moving the cutoff and the loop self-oscillates once
// r_n > R_s. Beyond I_d the element becomes a stiff resistance r_d which
// limits the loop current, and therefore the oscillation amplitude. Being
// piecewise linear the root is solved explicitly, with no iteration.
//
// Inductor waves scale like R_L i ~ i/k, which is large at low cutoff, so the
// filter works internally in Q20 (range +/-2048) rather than Q24.

static constexpr float FILT_FMIN = 30.0f;	// Hz at 0 octaves
static constexpr int FILT_OCTAVES = 9;		// 30 Hz .. 15.36 kHz
static constexpr int FILT_STEPS_PER_OCT = 32;
static constexpr int FILT_TABLE = FILT_OCTAVES * FILT_STEPS_PER_OCT + 2;
static constexpr int FQ = 20;				 // filter signal format
static constexpr float FILT_RS = 1.0f;		 // series damping resistor
static constexpr float FILT_RD = 24.0f;		 // limiter resistance beyond I_d
static constexpr float FILT_ID = 1.0f;		 // limiter knee current
static constexpr float FILT_RN_MAX = 1.12f;	 // resonance control range (oscillates above 1.0)

struct ResonantFilter
{
	// Coefficient tables, indexed in 1/32 octave steps
	int32_t tRL[FILT_TABLE];	// R_L = 1/k                Q20
	int32_t tRC[FILT_TABLE];	// R_C = k                  Q20
	int32_t tInvRP[FILT_TABLE]; // 1 / (R_s + R_L + R_C)    Q30
	int32_t tCo[FILT_TABLE];	// R_P / (R_P + r_d)        Q30

	// State is kept as Kirchhoff variables rather than waves, and the wave
	// states are rebuilt from the current port resistances every sample.
	// Storing waves would make a cutoff change re-interpret the stored
	// energy (R_L changes by up to 500x over the range) and blow up under
	// fast modulation; storing v and i makes the cutoff behave like a VCF
	// whose integrator states are continuous.
	int32_t i = 0;	// loop current (x tank impedance), Q20
	int32_t vC = 0; // capacitor voltage, Q20
	int32_t vL = 0; // inductor voltage, Q20

	int32_t lp = 0, bp = 0, hp = 0; // Q24

	void Init()
	{
		for (int n = 0; n < FILT_TABLE; n++)
		{
			float fc = FILT_FMIN * exp2f((float)n / FILT_STEPS_PER_OCT);
			if (fc > 0.45f * SAMPLE_RATE) fc = 0.45f * SAMPLE_RATE;
			float k = tanf(3.14159265f * fc / SAMPLE_RATE);
			float RC = k, RL = 1.0f / k;
			float RP = FILT_RS + RL + RC;
			tRL[n] = (int32_t)(RL * (1 << FQ));
			tRC[n] = (int32_t)(RC * (1 << FQ));
			tInvRP[n] = (int32_t)(1.0f / RP * CONE);
			tCo[n] = (int32_t)(RP / (RP + FILT_RD) * CONE);
		}
	}

	void Reset() { i = vC = vL = 0; }

	static int32_t Interp(const int32_t *t, int idx, int32_t frac8)
	{
		return t[idx] + ((t[idx + 1] - t[idx]) >> 8) * frac8;
	}

	static int32_t MulQ20(int32_t a, int32_t b)
	{
		return (int32_t)(((int64_t)a * b) >> FQ);
	}

	// vin: Q24 input voltage
	// cutoffOct: Q16 octaves above FILT_FMIN, 0 .. FILT_OCTAVES
	// res: Q30, 0 .. CONE
	void Step(int32_t vin, int32_t cutoffOct, int32_t res)
	{
		cutoffOct = clampi(cutoffOct, 0, FILT_OCTAVES * OCT - 1);
		int32_t pos = cutoffOct >> 3; // 1/8192 octave units
		int idx = pos >> 8;
		int32_t frac = pos & 0xFF;
		int32_t RL = Interp(tRL, idx, frac);
		int32_t RC = Interp(tRC, idx, frac);
		int32_t invRP = Interp(tInvRP, idx, frac);
		int32_t co = Interp(tCo, idx, frac);
		int32_t RP = (int32_t)(FILT_RS * (1 << FQ)) + RL + RC;

		// Root coefficients, working with the scaled current i' = R_P i
		int32_t rn = mulq30(res, (int32_t)(FILT_RN_MAX * (1 << FQ)));	// Q20
		int32_t rho = mulq30(rn, invRP) << (30 - FQ);					// r_n / R_P, Q30, < 0.6
		int32_t cin = 0x7FFFFFFF / ((CONE - rho) >> 15);				// 1/(1-rho), Q16
		int32_t Ath = mulq30(RP - rn, (int32_t)(FILT_ID * CONE));		// I_d (R_P - r_n)
		int32_t knee = mulq30(rn + (int32_t)(FILT_RD * (1 << FQ)), (int32_t)(FILT_ID * CONE));

		// Up-sweep: leaves reflect.
		// Trapezoidal capacitor: b = a[n-1] = v[n-1] + R_C i[n-1]
		// Trapezoidal inductor:  b = -a[n-1] = -(v[n-1] + R_L i[n-1])
		int32_t xV = vin >> (24 - FQ); // adapted resistive voltage source
		int32_t xL = -(vL + MulQ20(RL, i));
		int32_t xC = vC + MulQ20(RC, i);
		int32_t a = -(xV + xL + xC); // series adaptor, reflected to root

		// Root: solve a = v(i) + R_P i on the PWL characteristic
		int32_t ip;
		if (a <= Ath && a >= -Ath)
			ip = (int32_t)(((int64_t)a * cin) >> 16);
		else if (a > 0)
			ip = mulq30(a + knee, co);
		else
			ip = mulq30(a - knee, co);

		// Down-sweep: every series port carries the same current, so the
		// wave returned to port k is y_k = x_k + 2 R_k i and its voltage
		// (x_k + y_k)/2 = x_k + R_k i
		i = mulq30(ip, invRP);
		vL = xL + MulQ20(RL, i);
		vC = xC + MulQ20(RC, i);

		// Outputs, back to Q24. The loop orientation makes v_C = -Vin at DC,
		// so flip signs for positive gain. With R_s = 1, v_Rs = i.
		lp = -(vC << (24 - FQ));
		bp = -(i << (24 - FQ));
		hp = -(vL << (24 - FQ));
	}
};

////////////////////////////////////////////////////////////////////////////////
// Exponential pitch -> per-sample triangle increment

struct Exp2Table
{
	int32_t t[257]; // 2^(i/256), Q16
	void Init()
	{
		for (int i = 0; i <= 256; i++)
			t[i] = (int32_t)(exp2f(i / 256.0f) * 65536.0f + 0.5f);
	}
	// returns base * 2^(oct), oct in Q16, base*2 must fit in 15 bits
	int32_t Scale(int32_t base, int32_t oct) const
	{
		int32_t ioct = oct >> 16;
		int32_t f = (oct >> 8) & 0xFF;
		int32_t m = t[f] + (((t[f + 1] - t[f]) * (oct & 0xFF)) >> 8); // Q16, 1..2
		return ((base * m) >> 16) << ioct;
	}
};

////////////////////////////////////////////////////////////////////////////////
// Band-limited comparator (the Benjolin "PWM" output)
//
// The naive comparator output steps between -H and +H at arbitrary times
// between samples, which aliases badly once the oscillators are high or
// being cross-modulated. Each step is corrected with a two-sample polyBLEP:
// the crossing time is found by linear interpolation of d = tri1 - tri2, and
// the band-limited step residual is added to the samples either side of it.
// This costs one sample of latency.

struct BlepComparator
{
	static constexpr int32_t H = ONE >> 1; // output level

	int32_t dPrev = 0;
	int32_t naivePrev = -H;
	int32_t pending = -H; // previous sample, naive value + corrections so far

	int32_t Step(int32_t d)
	{
		int32_t naive = d > 0 ? H : -H;
		int32_t now = naive;
		int32_t out = pending;
		if (naive != naivePrev)
		{
			int32_t h = naive - naivePrev; // +/- 2H
			// fraction of the sample interval at which the crossing happens
			int32_t t = (int32_t)(((int64_t)dPrev << 15) / (dPrev - d)); // Q15, 0..1
			int32_t dl = (1 << 15) - t;									  // time from crossing to this sample
			int32_t before = (dl * dl) >> 1;							  // Q30, dl^2/2
			int32_t after = (((1 << 15) - dl) * ((1 << 15) - dl)) >> 1;	  // Q30, (1-dl)^2/2
			out += mulq30(h, before);
			now -= mulq30(h, after);
		}
		dPrev = d;
		naivePrev = naive;
		pending = now;
		return out;
	}
};

////////////////////////////////////////////////////////////////////////////////
// Complete instrument

static constexpr float OSC_FMIN = 0.5f;	   // Hz at 0 octaves
static constexpr int OSC_MAX_OCTAVES = 14; // 0.5 Hz .. 8.2 kHz

struct Controls
{
	int32_t osc1Oct;   // Q16 octaves above OSC_FMIN, including all modulation except rungler
	int32_t osc2Oct;
	int32_t run1;	   // rungler -> osc1 depth, Q16 octaves at full rungler
	int32_t run2;	   // rungler -> osc2 depth
	int32_t cutoffOct; // Q16 octaves above FILT_FMIN, excluding rungler
	int32_t runFilt;   // rungler -> filter depth, Q16 octaves
	int32_t fm1;	   // osc2 triangle -> osc1 pitch, Q16 octaves at full swing
	int32_t fm2;	   // osc1 triangle -> osc2 pitch
	int32_t res;	   // Q30, 0..CONE
	bool loop;		   // rungler recirculates instead of taking data from osc1
	bool useExtClock;  // rungler clocked by extClock instead of osc2
	bool extClock;
	bool useExtIn;	   // filter input from extIn instead of the PWM comparator
	int32_t extIn;	   // Q24
};

struct Benjolin
{
	TriangleCore osc1, osc2;
	RunglerDAC dac;
	BlepComparator comparator;
	ResonantFilter filter;
	Exp2Table exp2;
	int32_t incBase; // triangle increment per sample at OSC_FMIN, Q24

	uint8_t reg = 0xA5;
	int32_t rungler = 0; // 0..ONE
	int32_t pwm = 0; // band-limited comparator output, Q24
	bool lastClock = false;

	void Init()
	{
		dac.Init(0.0003f);
		filter.Init();
		exp2.Init();
		// triangle swings 4 units (-1..1..-1) per cycle: inc = 4 f / fs
		incBase = (int32_t)(4.0f * OSC_FMIN / SAMPLE_RATE * ONE);
	}

	int32_t Increment(int32_t oct) const
	{
		oct = clampi(oct, 0, OSC_MAX_OCTAVES * OCT - 1);
		return exp2.Scale(incBase, oct);
	}

	// rungler (Q24, 0..1) x depth (Q16 octaves, < 128) -> Q16 octaves
	static int32_t Mod(int32_t rung, int32_t depth)
	{
		return ((rung >> 8) * (depth >> 8)) >> 8;
	}

	// triangle (Q24, -1..1) x depth (Q16 octaves, < 128) -> Q16 octaves
	static int32_t FM(int32_t tri, int32_t depth)
	{
		return ((tri >> 9) * (depth >> 8)) >> 7;
	}

	void Step(const Controls &c)
	{
		// Cross FM: each oscillator's exponential rate is modulated by the
		// other's triangle (both taken from the previous sample)
		int32_t tri1 = osc1.v, tri2 = osc2.v;
		osc1.Step(Increment(c.osc1Oct + Mod(rungler, c.run1) + FM(tri2, c.fm1)));
		osc2.Step(Increment(c.osc2Oct + Mod(rungler, c.run2) + FM(tri1, c.fm2)));

		bool clk = c.useExtClock ? c.extClock : osc2.Square();
		if (clk && !lastClock)
		{
			uint32_t msb = reg >> 7;
			uint32_t data = c.loop ? msb : (msb ^ (osc1.Square() ? 1u : 0u));
			reg = (uint8_t)((reg << 1) | data);
		}
		lastClock = clk;

		rungler = dac.Step(reg >> 5);

		pwm = comparator.Step(osc1.v - osc2.v);
		int32_t vin = c.useExtIn ? c.extIn : pwm;
		filter.Step(vin, c.cutoffOct + Mod(rungler, c.runFilt), c.res);
	}
};

} // namespace wdfb
