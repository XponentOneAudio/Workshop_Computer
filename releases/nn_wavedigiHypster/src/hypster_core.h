// hypster_core.h - the Hypster's dynamic core, in fixed point, for 48kHz on
// an RP2040 (no FPU).
//
// Four LM13700 integrators (U2/A3 -> u, U4/A9 -> z, U5/A12 -> y, U6/A15 -> x)
// in a ring, each one a WDF:
//
//     capacitor C (port resistance T/2C, reflected wave = last incident wave)
//        closed by a nonlinear root: the OTA, a current source
//        I = Iabc tanh((V- - V+) / 2VT), whose V+ includes the stage's own
//        damping (a fraction of its output, and for y and x the Shape
//        injection).
//
// The root is solved each sample with one warm-started Newton step on
//     v = a + K tanh(s_in - kd v),     K = Iabc T / 2C
// which makes each integrator a trapezoidal (bilinear) integrator of the
// real OTA law, tanh compression included. The ring is evaluated in signal
// order u, z, y, x so that only the x -> u link (and the Shape cross
// coupling, taken from x) carries a one-sample delay.
//
// The memoryless shapers between stages were solved as WDFs in float
// (hypster_circuit.h) and are read here from tables.
//
// Units: volts and dimensionless quantities in Q24 (int32), tables in Q16.

#pragma once
#include <stdint.h>
#include "hypster_circuit.h"

namespace hyp
{

constexpr int32_t kOne24 = 1 << 24;
constexpr float kFs = 48000.0f;

inline int32_t ToQ24(float x) { return (int32_t)lrintf(x * 16777216.0f); }
constexpr int32_t ToQ24c(float x) { return (int32_t)(x * 16777216.0f + (x < 0 ? -0.5f : 0.5f)); }
constexpr int32_t Q20(float x) { return (int32_t)(x * 1048576.0f + 0.5f); }
inline float FromQ24(int32_t x) { return (float)x * (1.0f / 16777216.0f); }

static inline int32_t Mul24(int32_t a, int32_t b)
{
	return (int32_t)(((int64_t)a * b) >> 24);
}

// ------------------------------------------------------------ tables
// Voltage tables span -16V..+16V in 1/16V steps (513 points).
constexpr int kVTabBits = 9;
constexpr int kVTabSize = (1 << kVTabBits) + 1;
constexpr float kVTabMin = -16.0f, kVTabStep = 1.0f / 16.0f;

// Interpolate a voltage-indexed table; x in Q24 volts.
static inline int32_t LookupV(const int32_t *t, int32_t x)
{
	uint32_t u = (uint32_t)(x + (16 << 24));
	if ((int32_t)u < 0) u = 0;                       // below -16V
	if (u > (32u << 24) - 1) u = (32u << 24) - 1;    // above +16V
	const uint32_t i = u >> 20;
	const int32_t frac = (int32_t)((u >> 4) & 0xFFFF);
	const int32_t y0 = t[i];
	return y0 + (int32_t)(((int32_t)(t[i + 1] - y0) * frac) >> 16);
}

struct ShaperTables
{
	int32_t u[kVTabSize]; // A1 output (unclipped) -> A2 output, Q16 volts
	int32_t z[kVTabSize]; // u -> A8 output, Q16 volts
	int32_t y[kVTabSize]; // z -> A11 output, Q16 volts
	int32_t x[kVTabSize]; // y -> A14 output, Q16 volts
	int32_t tanh[513];    // tanh on -4..4 in 1/64 steps, Q16
	int32_t exp2[257];    // 2^(k/256), Q16
	int32_t limiter[257]; // D2-D4 current (nA) vs Ic - Ib, 0..524uA in 2.048uA steps
};

struct ShapeTables
{
	int32_t a[kVTabSize]; // x -> V_A / 2VT, Q24
	int32_t b[kVTabSize]; // x -> V_B / 2VT, Q24
};

inline float TableV(int k) { return kVTabMin + kVTabStep * (float)k; }

inline void BuildShaperTables(ShaperTables &t)
{
	ZenerPairShaper a2(2.2e3f, 33e3f, 100e3f, 220e3f, 100e3f);
	ZenerPairShaper a8(2.2e3f, 33e3f, 100e3f, 330e3f, 100e3f);
	BreakpointShaperY a11;
	BreakpointShaperX a14;
	for (int k = 0; k < kVTabSize; k++)
	{
		const float v = TableV(k);
		t.u[k] = (int32_t)lrintf(a2(OpAmpSat(v)) * 65536.0f); // A1 rails, then ZD1/2 + A2
		t.z[k] = (int32_t)lrintf(a8(v) * 65536.0f);
		t.y[k] = (int32_t)lrintf(a11(v) * 65536.0f);
		t.x[k] = (int32_t)lrintf(a14(v) * 65536.0f);
	}
	for (int k = 0; k < 513; k++)
		t.tanh[k] = (int32_t)lrintf(tanhf(-4.0f + (float)k / 64.0f) * 65536.0f);
	for (int k = 0; k < 257; k++)
		t.exp2[k] = (int32_t)lrintf(exp2f((float)k / 256.0f) * 65536.0f);
	Limiter lim;
	for (int k = 0; k < 257; k++)
		t.limiter[k] = (int32_t)lrintf(lim((float)k * 2.048e-6f) * 1e9f);
}

inline void BuildShapeTables(ShapeTables &t, ShapeMode mode, float pot)
{
	ShapeNetwork net;
	net.Configure(mode, pot);
	for (int k = 0; k < kVTabSize; k++)
	{
		const ShapeOut o = net(TableV(k));
		t.a[k] = ToQ24(o.vA / kVT2);
		t.b[k] = ToQ24(o.vB / kVT2);
	}
}

// ------------------------------------------------------------ controls
// Everything the panel sets, already in circuit terms.
struct Controls
{
	int32_t rateOct;   // rate in octaves above the minimum, Q16 (0..16 nominal)
	int32_t gainV;     // voltage at A5's summing input from Gain (+ mod), Q16 volts, 0..12 nominal
	int32_t trimV;     // A5 trim offset, Q16 volts, +/-1.2
	int32_t dampV;     // voltage at A6's input from Damping (+ mod), Q16 volts, 0..12 nominal
	bool limiter;      // D2-D4 fitted
	bool hold;         // freeze the integrators
};

// ------------------------------------------------------------ the core
// The RP2040 (Cortex-M0+) has a single-cycle 32x32->32 multiply but no long
// multiply, so every per-sample product below is arranged to fit in 32 bits:
// the products that set the signal's precision split one operand into high
// and low parts; the Newton slope corrections only need ~15-bit precision.
class Core
{
public:
	// Signal outputs, Q24 volts (op-amp outputs A3, A9, A12, A15)
	int32_t U() const { return st[0].v; }
	int32_t Z() const { return st[1].v; }
	int32_t Y() const { return st[2].v; }
	int32_t X() const { return st[3].v; }

	// diagnostics, last control update
	int32_t icNA = 0, ibNA = 0, kRate = 0;

	void Init(const ShaperTables *s, const ShapeTables *sh)
	{
		tab = s;
		shape = sh;
		Kick(0x1234567u);
	}

	void SetShape(const ShapeTables *sh) { shape = sh; }

	// Host-test helpers (Lyapunov estimates)
	void Nudge(int i, int32_t dv)
	{
		st[i].v += dv;
		st[i].a += dv;
	}
	void CopyScaledDelta(const Core &r, float k)
	{
		for (int i = 0; i < 4; i++)
		{
			st[i].v = r.st[i].v + (int32_t)((float)(st[i].v - r.st[i].v) * k);
			st[i].a = r.st[i].a + (int32_t)((float)(st[i].a - r.st[i].a) * k);
		}
	}

	// Start from a fresh small random state (the panel 'Kick').
	void Kick(uint32_t seed)
	{
		rng ^= seed | 1u;
		for (auto &s : st)
		{
			s.v = (int32_t)(Rand() >> 8) - (1 << 23); // +/-0.5V
			s.a = s.v;
		}
	}

	// Gain, Damping, limiter: A5/Q1 and A6/Q2 converters. Call at control
	// rate (not per sample: uses 64-bit arithmetic).
	void UpdateCurrents(const Controls &c)
	{
		// Ic = (0.4545 Vgain + trim) / 18k ; Ib = (0.37 Vdamp + 2.14) / 18k   (nA)
		constexpr int32_t kNAperV = (int32_t)(1e9f / kRemit); // 55555
		int32_t vc = (int32_t)(((int64_t)c.gainV * (int32_t)(kGainPerV * 65536.0f)) >> 16) + c.trimV;
		int32_t vb = (int32_t)(((int64_t)c.dampV * (int32_t)(kDampPerV * 65536.0f)) >> 16) +
		             (int32_t)(kDampOffsetV * 65536.0f);
		int32_t ic = vc > 0 ? (int32_t)(((int64_t)vc * kNAperV) >> 16) : 0;
		int32_t ib = vb > 0 ? (int32_t)(((int64_t)vb * kNAperV) >> 16) : 0;
		if (c.limiter && ic > ib)
		{
			// D2-D4 table in 2.048uA steps
			int32_t d = ic - ib;
			if (d > (256 << 11) - 1) d = (256 << 11) - 1;
			const int32_t i = d >> 11, f = d & 2047;
			const int32_t id = tab->limiter[i] + (((tab->limiter[i + 1] - tab->limiter[i]) * f) >> 11);
			ic -= id;
			ib += id;
		}
		icNA = ic;
		ibNA = ib;
		// A1 transimpedance: Va1 = Ic R5 tanh(..)  -> Ic R5 in Q8 volts
		gainQ8 = (int32_t)(((int64_t)ic * (int32_t)(kR5 * 1e-9f * 256.0f * 65536.0f)) >> 16);
		// U3 damping into R13: V+ = 220 Ib kAdU u / 2VT ; per volt of u, /2VT again, Q20
		kd20[0] = (int32_t)(((int64_t)ib * (int32_t)(kRdiv * 1e-9f * kAdU / (kVT2 * kVT2) * 1048576.0f * 65536.0f)) >> 16);
		// Newton slopes K kd (Q15), refreshed here rather than every sample
		for (int i = 0; i < 4; i++)
			kkd15[i] = (int32_t)(((int64_t)kRate * kd20[i]) >> 29);
		hold = c.hold;
	}

	// Rate: exponential converter A7/Q3/Q4. Call every sample (audio-rate FM).
	void UpdateRate(int32_t rateOct)
	{
		if (rateOct < 0) rateOct = 0;
		if (rateOct > (17 << 16) - 1) rateOct = (17 << 16) - 1;
		const int32_t io = rateOct >> 16;
		const int32_t fr = (rateOct >> 8) & 0xFF;
		// K = Ia T / 2C, Q24 ; at the minimum current K = 699.05 = 11185 / 16
		static_assert(kIaMin / (2.0f * kCint * kFs) * 16777216.0f > 699.0f &&
		              kIaMin / (2.0f * kCint * kFs) * 16777216.0f < 699.1f, "K scaling");
		kRate = (tab->exp2[fr] * 11185) >> (20 - io);
	}

	void __attribute__((always_inline)) Process()
	{
		if (hold) return;
		const int32_t xPrev = st[3].v;

		// x -> U1 (gain c) -> A1 -> ZD1/2 -> A2
		const int32_t t16 = Tanh16(MulKV(kInG20, xPrev));
		const int32_t va1 = ((gainQ8 * t16) >> 8) << 8;  // Q24 volts (Q16 precision)
		const int32_t nlU = LookupV(tab->u, va1);         // Q16
		// u: U2 / A3, damped by U3
		const int32_t noise = (int32_t)(Rand() >> 22) - 512; // op-amp noise, keeps it off equilibria
		Step(0, MulKN(kInU20, nlU) + noise);

		// u -> ZD3/4 -> A8 -> z: U4 / A9
		Step(1, MulKN(kInZ20, LookupV(tab->z, st[0].v)));

		// z -> D6/ZD5 -> A11 -> y: U5 / A12, Shape into node B
		Step(2, MulKN(kInY20, LookupV(tab->y, st[1].v)) - LookupS(shape->b, xPrev));

		// y -> D7/ZD6 -> A14 -> x: U6 / A15, Shape into node A
		Step(3, MulKN(kInX20, LookupV(tab->x, st[2].v)) - LookupS(shape->a, xPrev));
	}

private:
	struct Stage
	{
		int32_t v = 0; // capacitor (= integrator output) voltage, Q24
		int32_t a = 0; // wave incident on the root (reflected by the capacitor), Q24
	};
	Stage st[4];
	const ShaperTables *tab = nullptr;
	const ShapeTables *shape = nullptr;
	uint32_t rng = 0x9E3779B9u;
	bool hold = false;
	int32_t gainQ8 = 0;

	// tanh arguments per volt (attenuator / 2VT), Q20
	static constexpr int32_t kInG20 = Q20(kAinG / kVT2); // MulKV needs these < 0.06
	static_assert(kAinG / kVT2 < 0.06f && kAinZ / kVT2 < 0.06f && kAdZ / kVT2 < 0.06f, "MulKV range");
	static constexpr int32_t kInU20 = Q20(kAinU / kVT2);
	static constexpr int32_t kInZ20 = Q20(kAinZ / kVT2);
	static constexpr int32_t kInY20 = Q20(kAinY / kVT2);
	static constexpr int32_t kInX20 = Q20(kAinX / kVT2);
	// damping per volt of own output, Q20 ; [0] (u, from U3) is set by Damping
	int32_t kd20[4] = {0, Q20(kAdZ / kVT2), Q20(kAdY / kVT2), Q20(kAdX / kVT2)};
	int32_t kkd15[4] = {0, 0, 0, 0};
	static constexpr int32_t kRail24 = ToQ24c(kRail);

	uint32_t Rand()
	{
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		return rng;
	}

	// k (Q20, < 0.06) * v (Q24 volts, within the 10.8V rails) -> Q24, full
	// precision: v = vh 2^13 + vl ;  |k vh| < 2^31 , k vl < 2^30
	static int32_t MulKV(int32_t k, int32_t v)
	{
		const int32_t vh = v >> 13, vl = v & 8191;
		return (k * vh + ((k * vl) >> 13)) >> 7;
	}
	// k (Q20, < 0.1) * n (Q16 volts, < 16V) -> Q24
	static int32_t MulKN(int32_t k, int32_t n)
	{
		const int32_t nh = n >> 8, nl = n & 255;
		return ((k * nh) >> 4) + ((k * nl) >> 12);
	}

	// tanh of a Q24 argument, Q16 result
	int32_t Tanh16(int32_t s) const
	{
		uint32_t u = (uint32_t)(s + (4 << 24));
		if ((int32_t)u < 0) return -65536;
		if (u >= (8u << 24)) return 65536;
		const uint32_t i = u >> 18;
		const int32_t frac = (int32_t)((u >> 2) & 0xFFFF);
		const int32_t y0 = tab->tanh[i];
		return y0 + (((tab->tanh[i + 1] - y0) * frac) >> 16);
	}

	// Shape tables are Q24 and gentle; same indexing as LookupV
	static int32_t LookupS(const int32_t *t, int32_t x)
	{
		uint32_t u = (uint32_t)(x + (16 << 24));
		if ((int32_t)u < 0) u = 0;
		if (u > (32u << 24) - 1) u = (32u << 24) - 1;
		const uint32_t i = u >> 20;
		const int32_t frac = (int32_t)((u >> 6) & 0x3FFF);
		const int32_t y0 = t[i];
		return y0 + (((t[i + 1] - y0) * frac) >> 14);
	}

	// One sample of one integrator: WDF capacitor + OTA root, one Newton step
	// warm-started from the last sample:
	//   v = a + K tanh(sIn - kd v) ;  f(v) = v - a - K tanh(..) ; f' = 1 + K kd sech^2
	void __attribute__((always_inline)) Step(int i, int32_t sIn)
	{
		Stage &s = st[i];
		const int32_t v0 = s.v, a = s.a;
		const int32_t t = Tanh16(sIn - MulKV(kd20[i], v0));  // Q16
		// K t : K (Q24, < 2^28) split at 2^12
		const int32_t K = kRate;
		const int32_t Kt = ((K >> 12) * t >> 4) + (((K & 4095) * t) >> 16);
		const int32_t f = v0 - a - Kt;
		// eps = K kd sech^2 (Q15, up to ~0.3 at the top rate)
		// f / (1 + eps) ~ f - f q,  q = eps - eps^2 + eps^3
		const int32_t t15 = t >> 1;
		const int32_t sech2 = 32768 - ((t15 * t15) >> 15);
		const int32_t eps = (kkd15[i] * sech2) >> 15;
		const int32_t q = eps - ((eps * (eps - ((eps * eps) >> 15))) >> 15);
		const int32_t fq = ((f >> 10) * q) >> 5;
		int32_t v = v0 - (f - fq);
		// integrator op-amp rails
		if (v > kRail24) v = kRail24;
		if (v < -kRail24) v = -kRail24;
		s.v = v;
		s.a = 2 * v - a; // reflected by the root -> stored in the capacitor
	}
};

} // namespace hyp
