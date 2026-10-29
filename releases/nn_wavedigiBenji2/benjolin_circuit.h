// WDF Benjolin 2 - component-level model of the Benjolin Gorga
// (https://github.com/XponentOneAudio/benjolin_gorga, a v1.6 Benjolin after
// Rob Hordijk and Forestcaver, CC BY-NC 4.0).
//
// Every constant below comes from the Gorga netlist and BOM; reference
// designators are given in the comments. Voltages are real circuit volts
// (+/-9V rails).
//
// Modelling approach
// ------------------
// * Op-amps are ideal (virtual-ground inputs) with outputs that clip at the
//   TL07x swing, +/-VSAT.
// * Every capacitor is a WDF one-port (trapezoidal: a voltage source equal
//   to its stored wave in series with R = T/2C). The resistive/controlled
//   source network around them is solved as an R-type adaptor in Kirchhoff
//   form, which is exact for that network. Capacitor values never change, so
//   the stored waves stay valid when the VCA gains are modulated.
// * SSM2164 VCAs are ideal current-mode exponential gain cells,
//   -33 mV/dB at the control port, unity gain at 0V.
// * Precision rectifiers driving the SSM2164 control ports are ideal:
//   CTRL = max(0, -R_f * sum of input currents).
// * CD4021 outputs swing to the +/-9V rails and its inputs switch at 0V.
// * The 510R + 560pF compensation networks on the SSM2164 inputs (corners
//   above 500 kHz) are left out.
//
// The DSP core has no Pico SDK dependencies (see test/host_sim.cpp).
//
// Fixed point: voltages Q24 (1V = 1 << 24), gains Q30, network
// coefficients Q26.

#pragma once

#include <cstdint>
#include <cmath>

namespace benji
{

static constexpr int32_t V1 = 1 << 24; // one volt, Q24
static constexpr int32_t G1 = 1 << 30; // unity gain, Q30
static constexpr int32_t K1 = 1 << 26; // unity coefficient, Q26
static constexpr float FS = 48000.0f;
static constexpr float TS = 1.0f / FS;

static inline int32_t Volts(float v) { return (int32_t)(v * (float)V1); }
static inline int32_t Coef(float c) { return (int32_t)(c * (float)K1); }

static inline int32_t Mul(int32_t a, int32_t b, int shift)
{
	return (int32_t)(((int64_t)a * b) >> shift);
}

// volts (Q24, |v| < 32V) x small coefficient (Q30, |c| < 2) with a single
// 32-bit multiply; coefficient resolution 1/16384, used for CV mixing
static inline int32_t MulVC(int32_t v, int32_t c)
{
	return ((v >> 10) * (c >> 16)) >> 4;
}

static inline int64_t Clamp64(int64_t x, int32_t lim)
{
	return x < -lim ? -lim : (x > lim ? lim : x);
}

static inline int32_t Clamp(int32_t x, int32_t lo, int32_t hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

////////////////////////////////////////////////////////////////////////////////
// Component values (Benjolin Gorga BOM)

namespace part
{
// Supplies: 78L09 / 79L09 (IC7, IC8)
static constexpr float VCC = 9.0f, VEE = -9.0f;
static constexpr float VSAT = 7.5f; // TL072/TL074 output swing on +/-9V

// SSM2164 (IC3) control law
static constexpr float SSM_V_PER_DB = 0.033f;
static constexpr float SSM_V_PER_OCT = SSM_V_PER_DB * 6.0206f;

// Oscillators: integrators IC2.4/IC2.2 with C3/C4, SSM inputs R11/R12,
// Schmitt triggers IC4.2/IC4.1 with R13/R15 and R14/R16, control summers
// IC2.3/IC2.1 with feedback R7/R8 and precision rectifier D1/D3, D2/D4.
static constexpr float C_OSC = 1e-9f;		// C3, C4
static constexpr float R_OSC_IN = 100e3f;	// R11, R12
static constexpr float R_HYST_A = 15e3f;	// R13, R14 (from triangle)
static constexpr float R_HYST_B = 30e3f;	// R15, R16 (positive feedback)
static constexpr float R_CTRL_FB = 10e3f;	// R7, R8
static constexpr float R_FREQ = 30e3f;		// R3, R5 from the rate pots R54, R56
static constexpr float R_CVIN = 51e3f;		// R1, R2 from the CV pots R51, R52
static constexpr float R_RUNIN = 51e3f;		// R4, R6 from the rungler pots R55, R57

// Rungler: CD4021 (IC1), R-2R DAC into IC5.4
static constexpr float R23 = 30e3f, R24 = 30e3f, R25 = 30e3f, R26 = 15e3f;
static constexpr float R27 = 15e3f, R28 = 30e3f, R29 = 10e3f;
// XOR: IC5.3 with Q1 (2N3904)
static constexpr float R17 = 100e3f, R18 = 47e3f, R19 = 30e3f, R20 = 30e3f;
static constexpr float R21 = 62e3f, R22 = 15e3f;
static constexpr float VBE_ON = 0.6f;

// Filter: SSM2164 channels 1, 2 into integrators IC5.2 (C10) and IC6.2 (C8)
static constexpr float C10 = 220e-12f, C8 = 220e-12f, C7 = 220e-12f;
static constexpr float R47 = 30e3f, R50 = 470e3f;	// SSM ch1 inputs (from N, from OUT)
static constexpr float R34 = 30e3f, R39 = 15e3f;	// SSM ch2 inputs (from U, from resonance)
static constexpr float R30 = 51e3f, R31 = 100e3f, R32 = 30e3f, R33 = 30e3f; // summer IC6.1
static constexpr float C5 = 100e-9f;				// input coupling
static constexpr float R35 = 51e3f, R36 = 150e3f, R38 = 10e3f, R44 = 100e3f; // mix node X
static constexpr float R48 = 30e3f;					// IC5.1 feedback
static constexpr float R40 = 1.5e3f;				// resonance pot load
static constexpr float R41 = 51e3f, R42 = 51e3f, R43 = 51e3f, R45 = 100e3f, R46 = 10e3f; // F_CV
static constexpr float R63 = 100e3f, R64 = 100e3f, R65 = 100e3f, R66 = 20e3f; // output IC9

static constexpr float R_POT = 10e3f; // R51 - R61, assumed linear
} // namespace part

// Thevenin resistance of a pot wiper at fraction a from one end
static inline float PotRth(float a)
{
	return a * (1.0f - a) * part::R_POT;
}

////////////////////////////////////////////////////////////////////////////////
// 2^(-x) for the SSM2164 gain, Q30

struct ExpTable
{
	int32_t t[257];
	void Init()
	{
		for (int i = 0; i <= 256; i++)
			t[i] = (int32_t)(exp2f(-(float)i / 256.0f) * (float)G1);
	}
	// gain for a control voltage (Q24, >= 0)
	int32_t Gain(int32_t ctrl) const
	{
		// octaves in Q16: ctrl / (V per octave)
		static const int32_t kOct = (int32_t)(4096.0f / part::SSM_V_PER_OCT);
		int32_t oct = ((ctrl >> 12) * kOct) >> 8; // Q16
		int32_t io = oct >> 16;
		if (io > 29) return 0;
		int32_t f = (oct >> 8) & 0xFF;
		int32_t lo = t[f], hi = t[f + 1];
		int32_t m = lo + (((hi - lo) >> 8) * (oct & 0xFF));
		return m >> io;
	}
};

////////////////////////////////////////////////////////////////////////////////
// Oscillator: SSM2164 current-controlled integrator and Schmitt trigger.
//
// The pulse output P sits at +/-VSAT; the capacitor (a WDF one-port fed by
// the VCA's output current, g P / R_OSC_IN) ramps the triangle T at
// -g P T / (R_OSC_IN C) volts per sample. The Schmitt trigger flips P when
// T crosses -P R_HYST_A / R_HYST_B. Within a sample the current is constant,
// so trapezoidal integration is exact; at a flip the overshoot is reflected
// with the new current, keeping the period exact at audio rates.

struct Oscillator
{
	int32_t T = 0;		  // triangle (integrator output), Q24
	bool high = false;	  // pulse output P > 0
	int32_t incFull;	  // |dT| per sample at unity gain, Q24
	int32_t vth;		  // Schmitt threshold, Q24

	void Init()
	{
		incFull = Volts(part::VSAT * TS / (part::R_OSC_IN * part::C_OSC));
		vth = Volts(part::VSAT * part::R_HYST_A / part::R_HYST_B);
	}

	int32_t P() const { return high ? Volts(part::VSAT) : Volts(-part::VSAT); }

	// returns true on a rising edge of P
	bool Step(int32_t gain)
	{
		int32_t inc = Mul(incFull, gain, 30);
		if (high)
		{
			T -= inc;
			if (T < -vth)
			{
				T = -2 * vth - T;
				high = false;
			}
		}
		else
		{
			T += inc;
			if (T > vth)
			{
				T = 2 * vth - T;
				high = true;
				return true;
			}
		}
		return false;
	}
};

////////////////////////////////////////////////////////////////////////////////
// PWM comparator IC6.3 (+ = T2, - = T1), open loop at +/-VSAT, with a
// two-sample polyBLEP at the interpolated crossing so the edges fed to the
// filter don't alias. One sample of latency.

struct BlepComparator
{
	int32_t H;
	int32_t dPrev = 0;
	int32_t naivePrev;
	int32_t pending;

	void Init()
	{
		H = Volts(part::VSAT);
		naivePrev = pending = -H;
	}

	int32_t Step(int32_t d)
	{
		int32_t naive = d > 0 ? H : -H;
		int32_t now = naive;
		int32_t out = pending;
		if (naive != naivePrev)
		{
			int32_t h = naive - naivePrev;
			int32_t t = (int32_t)(((int64_t)dPrev << 15) / (dPrev - d)); // crossing, Q15
			int32_t dl = (1 << 15) - t;
			int32_t before = (dl * dl) >> 1;					   // Q30
			int32_t after = (((1 << 15) - dl) * ((1 << 15) - dl)) >> 1; // Q30
			out += Mul(h, before, 30);
			now -= Mul(h, after, 30);
		}
		dPrev = d;
		naivePrev = naive;
		pending = now;
		return out;
	}
};

////////////////////////////////////////////////////////////////////////////////
// Rungler: CD4021 clocked by P2, serial input from the XOR (IC5.3, Q1),
// DAC from Q6, Q7, Q8 through the R-2R network into IC5.4.

struct Rungler
{
	uint8_t reg = 0x5A;		// bit 0 = Q1 ... bit 7 = Q8
	int32_t dacTable[8];	// RUN_CV for each (Q6, Q7, Q8), Q24
	int32_t cv = 0;			// RUN_CV, Q24

	void Init()
	{
		using namespace part;
		for (int s = 0; s < 8; s++)
		{
			float q6 = (s & 1) ? VCC : VEE;
			float q7 = (s & 2) ? VCC : VEE;
			float q8 = (s & 4) ? VCC : VEE;
			// nodal analysis: node A (R24 from Q7, R26 to virtual ground, R27 to B),
			// node B (R25 from Q8, R28 to ground, R27 to A)
			float gA = 1 / R24 + 1 / R26 + 1 / R27, gB = 1 / R25 + 1 / R28 + 1 / R27;
			float iA = q7 / R24, iB = q8 / R25, g = 1 / R27;
			float vA = (iA * gB + iB * g) / (gA * gB - g * g);
			float run = -R29 * (q6 / R23 + vA / R26);
			if (run > VSAT) run = VSAT;
			if (run < -VSAT) run = -VSAT;
			dacTable[s] = Volts(run);
		}
		Update();
	}

	void Update() { cv = dacTable[reg >> 5]; }

	// q1On: Q1 conducting (grounds IC5.3's + input)
	static bool XorData(bool q8, bool q1On)
	{
		using namespace part;
		float vq8 = q8 ? VCC : VEE;
		float out = q1On ? -(R22 / R19) * vq8
						 : vq8 * R21 / (R20 + R21) * (1 + R22 / R19) - (R22 / R19) * vq8;
		return out > 0; // 4021 input threshold at mid-supply
	}

	void Clock(bool q1On)
	{
		bool q8 = reg & 0x80;
		reg = (uint8_t)((reg << 1) | (XorData(q8, q1On) ? 1 : 0));
		Update();
	}
};

////////////////////////////////////////////////////////////////////////////////
// Filter
//
// Two SSM2164 current-mode integrators in a loop:
//
//   OUT = integrator IC5.2 / C10, input current  g (N/R47 + OUT/R50)
//   N   = integrator IC6.2 / C8,  input current  g (U/R34 + Y/R39 + i_C7)
//
// with C7 from N back to the SSM channel 2 input, the summer
//   U = -R33 (i_ext + X/R31 + OUT/R32)
// the mix node
//   X = (RUN/R35 + PWM/R36 + N/R44) / (1/R35 + 1/R36 + 1/R38 + 1/R44 + 1/R31)
// the resonance path
//   W = N (1 + R48/R44) - X R48/R44,   Y = beta(R59) W
// and external input i_ext through C5 and R30.
//
// Capacitors C10, C8, C7 (and C5) are WDF one-ports with port resistance
// T/2C. Writing each as v = alpha + R i (alpha = stored wave) and
// substituting into the network gives a 2x2 system in OUT and N, solved
// exactly each sample (an R-type adaptor in Kirchhoff form).
//
// Cutoff CV (F_CV) is driven by the precision rectifier IC6.4/D5/D6 from the
// cutoff pot, the rungler, the filter CV jack and N itself through R45. That
// self-modulation is a delay-free loop through a nonlinearity; it uses the
// previous sample's N.

struct Filter
{
	// constant network coefficients, Q26
	int32_t a1, e1, e2, e3, r87, cxP2; // see Init
	int32_t pX0;					   // R8p p2 contribution independent of beta, Q26
	int32_t pXbeta;					   // per unit beta, Q26
	int32_t xinRun, xinPwm;			   // x_in = RUN xinRun + PWM xinPwm, Q30
	int32_t cxinBeta;				   // c_xin per unit beta, Q26
	int32_t cxin0;
	int32_t extGain, extState;		   // C5 / R30 input network, Q30
	int32_t outGain;				   // IC9 gain, Q8

	// set by SetResonance
	int32_t P2 = 0, cxin = 0;

	// WDF capacitor states (Q24, C5 state Q28)
	int32_t s10 = 0, s8 = 0, s7 = 0, s5 = 0;
	int32_t out = 0, n = 0; // integrator outputs
	int32_t outAmp = 0;		// J9 output

	void Init()
	{
		using namespace part;
		float R10p = TS / (2 * C10), R8p = TS / (2 * C8), R7p = TS / (2 * C7);
		float Gx = 1 / R35 + 1 / R36 + 1 / R38 + 1 / R44 + 1 / R31;
		float cx = (1 / R44) / Gx;
		float rho = R48 / R44;
		a1 = Coef(R10p / R50);
		e1 = Coef(R10p / R47);
		e2 = Coef(R8p * R33 / (R34 * R32));
		e3 = Coef(R8p / R34);
		r87 = Coef(R8p / R7p);
		// R8p p2 = -R8p (R33/R34) cx / R31 + beta R8p ((1 + rho) - rho cx) / R39
		pX0 = Coef(-R8p * (R33 / R34) * cx / R31);
		pXbeta = Coef(R8p * ((1 + rho) - rho * cx) / R39);
		// c_xin = R33/R31 + beta rho R34/R39
		cxin0 = Coef(R33 / R31);
		cxinBeta = Coef(rho * R34 / R39);
		xinRun = (int32_t)((1 / R35) / Gx * (float)G1);
		xinPwm = (int32_t)((1 / R36) / Gx * (float)G1);
		float R5p = TS / (2 * C5);
		extGain = (int32_t)(R33 / (R30 + R5p) * (float)G1);
		extState = (int32_t)(2 * R5p / (R30 + R5p) * (float)G1);
		outGain = (int32_t)(R65 / R66 * R64 / R63 * 256.0f);
		SetResonance(0.2f);
	}

	// beta: fraction of W reaching the resonance node (0 = most resonant)
	void SetResonance(float beta)
	{
		P2 = Coef(beta * (float)pXbeta / (float)K1) + pX0 + r87;
		cxin = cxin0 + Coef(beta * (float)cxinBeta / (float)K1);
	}

	// g: SSM2164 gain (Q30), run/pwm/ext: volts (Q24)
	void Step(int32_t g, int32_t run, int32_t pwm, int32_t ext)
	{
		const int32_t vsat = Volts(part::VSAT);

		// External input through C5 (WDF capacitor) and R30 into IC6.1
		int32_t vdiff = ext - (s5 >> 4);
		int32_t uExt = MulVC(vdiff, extGain);		// R33 i_ext, volts
		s5 += Mul(vdiff, extState, 26);				// Q28 state

		int32_t xin = MulVC(run, xinRun) + MulVC(pwm, xinPwm);

		int32_t A = K1 + Mul(g, a1, 30);
		int32_t E1 = Mul(g, e1, 30);
		int32_t E2 = Mul(g, e2, 30);
		int32_t D = K1 + Mul(g, P2, 30);
		int32_t E3 = Mul(g, e3, 30);
		// The wave states can reach a few times VSAT, so the right-hand side
		// and numerators are accumulated in 64 bits
		int32_t s7r = r87 == K1 ? s7 : Mul(r87, s7, 26); // C8/C7 = 1 on the Gorga
		int32_t drive = uExt + MulVC(xin, cxin << 4);
		int64_t q = (int64_t)s8 + Mul(g, s7r, 30) + Mul(E3, drive, 26); // Q24

		int64_t det = (int64_t)A * D + (int64_t)E1 * E2; // Q52
		int32_t det26 = (int32_t)(det >> 26);
		// 1/det: hardware divide for a Q17 estimate, one Newton step to Q29
		int32_t r = (0x7FFFFFFF / (det26 >> 12)) << 12; // Q29
		r = Mul(r, (2 << 29) - Mul(det26, r, 26), 29);

		// numerators kept at full Q24 precision (small increments at low
		// cutoff would otherwise fall into a rounding dead-band)
		int64_t n2 = ((int64_t)A * q + (int64_t)E2 * s10) >> 26;
		int64_t n1 = ((int64_t)D * s10 - (int64_t)E1 * q) >> 26;
		int32_t v2 = (int32_t)Clamp64((n2 * r) >> 29, vsat);
		int32_t v1 = (int32_t)Clamp64((n1 * r) >> 29, vsat);

		s10 = 2 * v1 - s10;
		s8 = 2 * v2 - s8;
		s7 = 2 * v2 - s7;
		out = v1;
		n = v2;
		outAmp = Clamp((v1 >> 8) * outGain, -vsat, vsat);
	}
};

////////////////////////////////////////////////////////////////////////////////
// Panel: pot positions and the circuit coefficients derived from them.
// Knob fractions are 0..1, increasing clockwise.

enum Pot
{
	POT_RATE1,	  // R54
	POT_RUN1,	  // R55
	POT_CV1,	  // R51 (osc 1 CV / cross mod from T2)
	POT_RATE2,	  // R56
	POT_RUN2,	  // R57
	POT_CV2,	  // R52 (osc 2 CV / cross mod from T1)
	POT_CUTOFF,	  // R58
	POT_RUNF,	  // R61
	POT_RES,	  // R59
	POT_CHAOS,	  // R60
	POT_FCV,	  // R53 (filter CV, normalled to T2)
	NUM_POTS
};

struct Benjolin
{
	ExpTable exp;
	Oscillator osc1, osc2;
	BlepComparator cmp;
	Rungler rungler;
	Filter filter;

	// CTRL = max(0, base - cv * Vcv - run * RUN), base Q24, cv/run Q30
	int32_t oscBase[2] = {0, 0}, oscCv[2] = {0, 0}, oscRun[2] = {0, 0};
	int32_t fBase = 0, fCv = 0, fRun = 0, fN = 0;
	bool q1OnHigh = true, q1OnLow = false; // Q1 state for P1 high / low
	float chaosV = 0.0f, chaosG = 0.0f;
	bool sw1 = false; // SW1: connects the chaos pot to Q1
	float resWiper = 0, resBeta = 0; // resonance pot state, for inspection

	int32_t pwm = 0;
	int32_t fctrl = 0;

	void Init()
	{
		exp.Init();
		osc1.Init();
		osc2.Init();
		cmp.Init();
		rungler.Init();
		filter.Init();
		osc2.T = osc2.vth / 3;
		fN = (int32_t)(part::R46 / part::R45 * (float)G1);
		for (int p = 0; p < NUM_POTS; p++) SetPot(p, 0.5f);
	}

	static int32_t Gn(float x) { return (int32_t)(x * (float)G1); }

	void SetPot(int pot, float k)
	{
		using namespace part;
		k = k < 0 ? 0 : (k > 1 ? 1 : k);
		switch (pot)
		{
		case POT_RATE1:
		case POT_RATE2:
		{
			// wiper between GND and V-; clockwise towards GND = faster
			float a = 1 - k;
			oscBase[pot == POT_RATE2] = Volts(R_CTRL_FB * -VEE * a / (R_FREQ + PotRth(a)));
			break;
		}
		case POT_RUN1:
		case POT_RUN2:
			oscRun[pot == POT_RUN2] = Gn(R_CTRL_FB * k / (R_RUNIN + PotRth(k)));
			break;
		case POT_CV1:
		case POT_CV2:
			oscCv[pot == POT_CV2] = Gn(R_CTRL_FB * k / (R_CVIN + PotRth(k)));
			break;
		case POT_CUTOFF:
		{
			float a = 1 - k;
			fBase = Volts(R46 * -VEE * a / (R43 + PotRth(a)));
			break;
		}
		case POT_RUNF:
			fRun = Gn(R46 * k / (R42 + PotRth(k)));
			break;
		case POT_FCV:
			fCv = Gn(R46 * k / (R41 + PotRth(k)));
			break;
		case POT_RES:
			resWiper = ResonanceTaper(k);
			resBeta = ResBeta(resWiper);
			filter.SetResonance(resBeta);
			break;
		case POT_CHAOS:
			chaosV = VEE + (VCC - VEE) * k;
			chaosG = 1 / (R18 + PotRth(k));
			UpdateQ1();
			break;
		}
	}

	// R59 from W (top) to GND, wiper loaded by R40 to ground and R39 into
	// the SSM input. a = wiper position measured from the GND end; a = 0
	// gives beta = 0, the least damping.
	static float ResBeta(float a)
	{
		using namespace part;
		float rTop = (1 - a) * R_POT + 1.0f, rBot = a * R_POT + 1.0f;
		float gT = 1 / rTop, gB = 1 / rBot;
		return gT / (gT + gB + 1 / R40 + 1 / R39);
	}

	// Filter damping (1/Q, low-cutoff limit) for a given beta, from the loop
	// equations: d = R47/R50 - m, m = c_xin c_x - (R34/R39)(1 + rho) beta
	static float ResDamping(float beta)
	{
		using namespace part;
		float Gx = 1 / R35 + 1 / R36 + 1 / R38 + 1 / R44 + 1 / R31;
		float cx = (1 / R44) / Gx, rho = R48 / R44;
		float cxin = R33 / R31 + beta * rho * R34 / R39;
		return R47 / R50 - (cxin * cx - (R34 / R39) * (1 + rho) * beta);
	}

	// Virtual taper for the resonance pot. With a linear R59, Q only rises
	// steeply in the last quarter turn. Instead the knob sets a target
	// damping that moves evenly on a log scale (so Q rises at a steady
	// rate in dB) between the two ends of the real pot, and the wiper
	// position that gives it is found by bisection. The circuit is
	// unchanged: only the knob-to-wiper mapping differs from a linear pot.
	static float ResonanceTaper(float k)
	{
		float dMax = ResDamping(ResBeta(1.0f)), dMin = ResDamping(ResBeta(0.0f));
		float target = dMax * powf(dMin / dMax, k);
		float lo = 0.0f, hi = 1.0f; // damping rises with a
		for (int i = 0; i < 24; i++)
		{
			float mid = 0.5f * (lo + hi);
			if (ResDamping(ResBeta(mid)) > target) hi = mid;
			else lo = mid;
		}
		return 0.5f * (lo + hi);
	}

	void SetSW1(bool on)
	{
		if (on != sw1)
		{
			sw1 = on;
			UpdateQ1();
		}
	}

	void UpdateQ1()
	{
		using namespace part;
		// Q1 base, Thevenin from P1 through R17 and (SW1) the chaos pot through R18
		float g17 = 1 / R17, g60 = sw1 ? chaosG : 0.0f;
		auto on = [&](float p1) { return (p1 * g17 + chaosV * g60) / (g17 + g60) > VBE_ON; };
		q1OnHigh = on(VSAT);
		q1OnLow = on(-VSAT);
	}

	int32_t OscGain(int i, int32_t vcv)
	{
		int32_t ctrl = oscBase[i] - MulVC(vcv, oscCv[i]) - MulVC(rungler.cv, oscRun[i]);
		return exp.Gain(ctrl > 0 ? ctrl : 0);
	}

	// cv1, cv2: voltages at the osc CV jacks (J1, J2); pass the other
	// triangle for the normalled cross modulation.
	// fcvIn: voltage at the filter CV jack (J4), normalled to T2.
	// ext: voltage at the filter input jack (J3).
	// extClock: if >= 0, replaces P2 as the rungler clock
	void Step(int32_t cv1, int32_t cv2, int32_t fcvIn, int32_t ext, int extClock = -1)
	{
		int32_t g1 = OscGain(0, cv1);
		int32_t g2 = OscGain(1, cv2);
		osc1.Step(g1);
		bool clk = osc2.Step(g2);

		if (extClock >= 0) clk = extClock > 0;
		if (clk) rungler.Clock(osc1.high ? q1OnHigh : q1OnLow);

		pwm = cmp.Step(osc2.T - osc1.T);

		int32_t c = fBase - MulVC(fcvIn, fCv) - MulVC(rungler.cv, fRun) - MulVC(filter.n, fN);
		fctrl = c > 0 ? c : 0;
		filter.Step(exp.Gain(fctrl), rungler.cv, pwm, ext);
	}
};

} // namespace benji
