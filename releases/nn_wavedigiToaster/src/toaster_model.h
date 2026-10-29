// toaster_model.h - circuit model of the MFOS Noise Toaster (Ray Wilson)
//
// Component references (R33, C9, Q8, ...) are those of the KiCad redraw of the
// MFOS schematic (github.com/XponentOneAudio/MFOS-NoiseToaster, rev 0.6.1).
// See ../docs/circuit-notes.md for the netlist and the derivations.
//
// Everything with a capacitor in it is a wave digital filter (WDF):
//   - VCF (U2A, Q8, C9, C10, R29, R35, R28): 3-port parallel junction at node
//     X9 with the op-amp as a constraint-solving root, incl. rail saturation
//   - input coupling legs C8/R33, C12/R34 (+ an extra leg for Audio In 1)
//   - LFO timing cap C16 and its RC "triangle" C19/R56
//   - AREG cap C18 charged through the D2/R51 + D3/R53 diode network
//     (nonlinear root, solved exactly)
//   - VCO square-wave averager C11/R30, sync differentiator C6/R17/R18
//   - noise coupling C7/R25, output coupling C21
//   - VCO integrator C2 (a constant current into a bilinear capacitor is
//     exactly a ramp, so it reduces to an accumulator)
// The capacitor-free JFET stages (AREG buffer Q9, VCA Q7) are solved exactly
// offline and tabulated (tools/gen_tables.cpp). Comparators are ideal with the
// schematic's hysteresis networks.
//
// Audio-rate code is integer: voltages are Q24 volts, coefficients Q28.
// Coefficients depending on knobs/modulation are recomputed in float at
// control rate (Control(), every few samples).
//
// LM324 single-supply levels: B- = 0 V, B+ = 9 V, virtual ground VG = 4.5 V,
// output swing 0.02 .. 7.5 V.

#pragma once

#include <cstdint>
#include <cmath>
#include "jfet.h"
#include "toaster_tables.h"

// Audio-rate code must run from RAM on the RP2040 (flash is behind an XIP
// cache). The Pico SDK places .time_critical.* sections in RAM.
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define WDT_RAMFUNC(f) __attribute__((noinline, section(".time_critical.wdt_" #f))) f
#else
#define WDT_RAMFUNC(f) f
#endif

namespace wdt
{

// ---------------------------------------------------------------- fixed point
constexpr float FS = 48000.0f;
constexpr float TS = 1.0f / FS;

constexpr int32_t V(float v) { return (int32_t)(v * 16777216.0f); } // Q24 volts
constexpr int32_t Q28(float c) { return (int32_t)(c * 268435456.0f); }
constexpr int32_t Q20(float c) { return (int32_t)(c * 1048576.0f); }
inline float toVolts(int32_t v) { return (float)v * (1.0f / 16777216.0f); }

// (a * b) >> S, rounded (truncation would bias the WDF states).
// The Cortex-M0+ has no 32x32->64 multiply, so on the device the product is
// assembled inline from 16-bit partial products (error < 1 LSB) instead of
// calling __aeabi_lmul. Define WDT_FAST_MUL=1 on the host to test that path.
#ifndef WDT_FAST_MUL
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define WDT_FAST_MUL 1
#else
#define WDT_FAST_MUL 0
#endif
#endif

template <int S>
static inline int32_t __attribute__((always_inline)) mulShift(int32_t a, int32_t b)
{
	static_assert(S > 20 && S < 32, "shift out of range");
#if WDT_FAST_MUL
	int32_t ah = a >> 16, bh = b >> 16;
	uint32_t al = (uint32_t)a & 0xFFFF, bl = (uint32_t)b & 0xFFFF;
	int32_t hh = ah * bh;                 // weight 2^32
	int32_t m1 = ah * (int32_t)bl;        // weight 2^16
	int32_t m2 = (int32_t)al * bh;        // weight 2^16
	uint32_t ll = al * bl;                // weight 1
	int32_t mid = (m1 >> 4) + (m2 >> 4) + (int32_t)(ll >> 20); // weight 2^20
	return (hh << (32 - S)) + ((mid + (1 << (S - 21))) >> (S - 20));
#else
	return (int32_t)(((int64_t)a * b + (1 << (S - 1))) >> S);
#endif
}
static inline int32_t __attribute__((always_inline)) mul28(int32_t a, int32_t b) { return mulShift<28>(a, b); }
static inline int32_t __attribute__((always_inline)) mul24(int32_t a, int32_t b) { return mulShift<24>(a, b); }
static inline int32_t clampV(int32_t x, int32_t lo, int32_t hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

// 2^x for pot tapers at control rate (powf is far too slow on the RP2040).
// Cubic on the fraction, |rel err| < 1e-4.
inline float FastExp2(float x)
{
	float fl = floorf(x);
	float f = x - fl;
	float p = 1.0f + f * (0.6957778f + f * (0.2251150f + f * 0.0791071f));
	union { float f; int32_t i; } u = {p};
	u.i += (int32_t)fl << 23;
	return u.f;
}

// Bilinear-transform capacitor port resistance, R = T / 2C
constexpr float RCap(float c) { return TS / (2.0f * c); }

// ---------------------------------------------------------------- circuit
constexpr float VBP = 9.0f;      // B+
constexpr float VGND = 4.5f;     // virtual ground (R37/R42 divider)
constexpr float VOL = 0.02f;     // LM324 output low
constexpr float VOH = 7.5f;      // LM324 output high (Vcc - 1.5 V)
constexpr float VDIODE = 0.5f;   // 1N914 at the micro-amp currents in the AREG

// --- VCO expo converter (U1C summer, Q3/Q4, U1D reference)
constexpr float R11 = 2e3f, R2 = 75e3f, R3 = 270e3f, R14 = 100e3f, R21 = 36e3f, R16 = 2e6f;
constexpr float IREF = (VBP - VGND) / R16; // Q3 collector current
constexpr float VT = 0.02585f;
constexpr float C2 = 1e-9f;              // integrator cap
// --- VCO ramp comparator U1B: R12 = 10k in, R7 = 300k feedback, R17/R18 threshold
constexpr float R12 = 10e3f, R7 = 300e3f, R17 = 10e3f, R18 = 15e3f, C6 = 0.01e-6f;
constexpr float VTH0 = VBP * R18 / (R17 + R18);  // 5.4 V
constexpr float RTH = R17 * R18 / (R17 + R18);  // 6 k
constexpr float RESET_DEAD_US = 6.0f;           // U1B slew + Q1 discharge time
// --- VCO square: U2B compares ramp with its average (R30 / C11); R31/R32 divider
constexpr float R30 = 2e6f, C11 = 0.1e-6f, R31 = 100e3f, R32 = 10e3f;
// --- VCF input legs
constexpr float R33 = 4.7e6f, C8 = 0.1e-6f, R34 = 3e6f, C12 = 0.1e-6f;
// --- VCF
constexpr float R35 = 4.7e6f, C9 = 1e-9f, C10 = 1e-9f;
constexpr float R39 = 39e3f, R40 = 39e3f, R43 = 27e3f, R44 = 100e3f, R45 = 100e3f, R38 = 100e3f;
constexpr float VCF_OUT_HI = VOH - VGND, VCF_OUT_LO = VOL - VGND;
// --- noise
constexpr float R25 = 100e3f, C7 = 0.1e-6f, R19 = 470e3f;
// --- LFO (U2C)
constexpr float R48 = 3e3f, C16 = 1e-6f, R56 = 20e3f, C19 = 1e-6f, R13 = 100e3f;
constexpr float LFO_TH_HI = (VBP + VOH) / 3.0f;  // R55 = R59 = R60 = 100k
constexpr float LFO_TH_LO = (VBP + VOL) / 3.0f;
// --- AREG (U2D, C18, Q9)
constexpr float R50 = 820.0f, C18 = 4.7e-6f, R57 = 200.0f, R61 = 10e3f;
constexpr float AREG_TH(float vo) { return (VBP / 150e3f + vo / 100e3f) / (2.0f / 150e3f + 1.0f / 100e3f); }
// --- VCA gate divider R41/R46, output coupling C21 into R67 + R68
constexpr float R41 = 470e3f, R46 = 1e6f, C21 = 1e-6f, RAMP_IN = 120e3f;

// ---------------------------------------------------------------- WDF helper
// A resistive voltage source (E, Rs) in series with a capacitor, terminated in
// a short (or a virtual ground). Series adaptor of two one-ports, root = short.
//   cap wave b is the state; loop current i = (E - b) / (Rs + Rc);
//   new b = b + 2 Rc i;  cap voltage = b + Rc i;  resistor voltage = Rs i.
struct SeriesRC
{
	int32_t b = 0;      // capacitor reflected wave (Q24 V)
	int32_t kCap = 0;   // 2 Rc / (Rs + Rc)   (Q28)
	int32_t kRes = 0;   //  Rs  / (Rs + Rc)   (Q28)

	void Set(float rs, float c)
	{
		float rc = RCap(c);
		kCap = Q28(2.0f * rc / (rs + rc));
		kRes = Q28(rs / (rs + rc));
	}
	// Returns the voltage across the resistor (= Rs * i); updates state.
	inline int32_t TickRes(int32_t e)
	{
		int32_t a = e - b;
		b += mul28(a, kCap);
		return mul28(a, kRes);
	}
	// Returns the voltage across the capacitor; updates state.
	inline int32_t TickCap(int32_t e)
	{
		int32_t a = e - b;
		int32_t db = mul28(a, kCap);
		int32_t vc = b + (db >> 1);
		b += db;
		return vc;
	}
};

// ---------------------------------------------------------------- the model
struct Controls
{
	// all 0..1 unless noted
	float cutoff = 0.65f;   // R28
	float reso = 0.55f;     // R29
	float pitch = 0.5f;     // R1
	float lfoRate = 0.6f;   // R49
	float attack = 0.15f;   // R51
	float release = 0.45f;  // R53
	float vcfMod = 0.3f;    // -1..1: <0 LFO via R38, >0 AREG via R38 (S6), ~0 off
	float vcoMod = 0.0f;    // -1..1: <0 LFO depth (R13), >0 AREG depth (R20 + S1)
	int source = 0;         // 0 ramp, 1 square, 2 ramp+noise, 3 square+noise, 4 noise
	bool repeat = true;     // S8: AREG self-cycles
	bool sync = false;      // S2 (driven from Pulse In 2 here)
	float pitchCV = 0.0f;   // extra volts into the expo converter, 1 V/oct
	float cutoffCV = 0.0f;  // volts, injected into the Q8 gate network
};

struct Inputs
{
	int32_t extAudio = 0;   // Q24 V, summed into the VCF input
	int32_t fmVolts = 0;    // Q24 V, 1 V/oct into the expo converter (audio rate)
	bool gate = false;      // manual gate (S10 / Pulse In 1)
	bool syncPulse = false; // sync drive (high = reset)
};

struct Outputs
{
	int32_t vca = 0;        // Q24 V, AC coupled (C21)
	int32_t vcf = 0;        // Q24 V, VCF output relative to VG, AC coupled
	int32_t vco = 0;        // Q24 V, selected VCO wave relative to VG
	int32_t arg = 0;        // Q24 V, AREG output (absolute)
	int32_t lfo = 0;        // Q24 V, LFO RC wave (absolute)
	bool lfoSquare = false; // LFO square high
	bool aregGate = false;  // AREG gate node high
	bool vcoSquare = false;
};

class NoiseToaster
{
public:
	NoiseToaster()
	{
		legVco.Set(R33, C8);
		legNoise.Set(R34, C12);
		noiseHp.Set(R25, C7);
		avg.Set(R30, C11);
		avg.b = V(VGND + 0.54f);
		sync.Set(RTH, C6);
		lfoTri.Set(R56 * R13 / (R56 + R13), C19);
		dcVca.Set(RAMP_IN, C21);
		dcVcf.Set(RAMP_IN, C21);
		ControlAll(Controls(), 0, 0);
		// start with C21 charged to the VCA's resting operating point (no thump)
		dcVca.b = VcaLookup(mul28(Q9Lookup(0), Q28_VCA_GATE), V(VGND));
	}

	// ------------------------------------------------------------ control rate
	// Called every few samples. lfoV, argV: current LFO / AREG voltages (they
	// drive the VCF's JFET gate network). The cheap per-tick work is the expo
	// converter's static input and the VCF junction coefficients; the pot
	// tapers are recomputed only when they change, one group per tick, so a
	// moving knob never costs more than one group's worth of float maths.
	void WDT_RAMFUNC(Control)(const Controls &c, int32_t lfoV, int32_t argV)
	{
		ctl = c;
		ControlVco();
		switch (slowPhase++ & 3)
		{
		case 0: ControlLfo(); break;
		case 1: ControlAreg(); break;
		case 2: ControlVcfPots(); break;
		default: break;
		}
		UpdateVcf(toVolts(lfoV), toVolts(argV));
	}

	// Same, but updates every group now (start-up and tests).
	void ControlAll(const Controls &c, int32_t lfoV, int32_t argV)
	{
		ctl = c;
		ControlVco();
		ControlLfo();
		ControlAreg();
		ControlVcfPots();
		UpdateVcf(toVolts(lfoV), toVolts(argV));
	}

	// VCF junction coefficients from the current Q8 gate voltage.
	void WDT_RAMFUNC(UpdateVcf)(float lfoV, float argV)
	{
		// Q8 gate network (R44 to B+, R39 from the R38 wiper, R40/R45 to B-),
		// pre-solved in ControlVcfPots: V_gate = g0 + gSrc * V_src + gCv * CV
		float src = vcfSrcIsArg ? argV : lfoV;
		float vgs = gate0 + gateSrc * src + gateCv * ctl.cutoffCV - VGND; // Q8 source on VG
		if (vgs > jfet::VGS_CLAMP) vgs = jfet::VGS_CLAMP;
		float vov = vgs - jfet::VP;
		float gC = g28 + (vov > 0.0f ? 2.0f * jfet::BETA * vov : 0.0f); // R28 || Q8 (1/Rds)

		// X9 junction (see docs/circuit-notes.md):
		//   branch A: R35 + C9 through the op-amp input node N
		//   branch B: R29 + C10 from the op-amp output
		//   branch C: R28 || Q8 to VG
		float inv = 1.0f / (vcfGab + gC);
		float p = 1.0f - VCF_RA * gC * inv;
		vcfGa = (int32_t)(VCF_GA_Q28 * inv);
		vcfGb = (int32_t)(vcfGbQ28 * inv);
		vcfKx = (int32_t)(vcfGab * 268435456.0f * inv);
		vcfInvP = (int32_t)(4194304.0f / p);              // Q22
		vcfQmax = (int32_t)(VCF_Q_LIMIT * p);
		vcfGc = gC;
	}

	// ------------------------------------------------------------ audio rate
	void WDT_RAMFUNC(Process)(const Inputs &in, Outputs &out)
	{
		// ================= LFO (U2C relaxation oscillator) =================
		int32_t lfoDrive = lfoHigh ? V(VOH) : V(VOL);
		int32_t v16 = lfoCap.TickCap(lfoDrive);
		if (lfoHigh && v16 > V(LFO_TH_HI)) lfoHigh = false;
		else if (!lfoHigh && v16 < V(LFO_TH_LO)) lfoHigh = true;
		// R56 into C19, loaded by R13 (Thevenin: x 100k/120k)
		int32_t lfoV = lfoTri.TickCap(mul28(lfoDrive, Q28_LFO_DIV));

		// ================= AREG =================
		int32_t argV = AregTick(in.gate);

		// ================= VCO =================
		int32_t dv = vcoDvStatic + mul28(lfoV, kVcoLfo) + mul28(argV - V(VGND), kVcoArg) +
		             mul28(in.fmVolts, Q28_VT_LN2);
		int32_t inc = ExpoIncrement(dv);
		VcoCore(inc, in.syncPulse);

		int32_t rampOut = rampOutPrev, sqOut = sqOutPrev;
		int32_t vcoSig = 0;
		bool useRamp = (ctl.source == 0 || ctl.source == 2);
		bool useSq = (ctl.source == 1 || ctl.source == 3);
		if (useRamp) vcoSig = rampOut;
		else if (useSq) vcoSig = sqOut;

		// ================= white noise generator =================
		int32_t noise = 0;
		if (ctl.source >= 2)
		{
			// avalanche noise -> Q2 stage; C7/R25 into Q6 (shunt feedback R19)
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			uint32_t r2 = rng * 2654435761u;
			int32_t n = (int32_t)((rng >> 10) + (r2 >> 10)) - (1 << 22); // triangular, +/-0.25 V
			int32_t x4 = -mul28(noiseHp.TickRes(n), Q28_NOISE_GAIN);
			noise = clampV(x4, V(-0.8f), V(3.5f)); // Q6 saturates near its 1 V bias point
		}

		// ================= VCF input legs (current into virtual ground) ====
		// E = R35 * I_in, each leg I = V_R / R  ->  E = V_R * R35 / R
		// The external input gets its own C8/R33 leg in principle; both legs
		// are linear and identical into the same virtual-ground node, so they
		// share one WDF (superposition).
		int32_t e = legVco.TickRes(vcoSig + in.extAudio);
		e += mul28(legNoise.TickRes(noise), Q28_R35_R34);

		// ================= VCF (WDF, root = op-amp) =================
		int32_t vo = VcfTick(e);

		// ================= VCA (Q7) =================
		int32_t vg7 = mul28(argV, Q28_VCA_GATE);
		int32_t vs = VcaLookup(vg7, vo + V(VGND));

		out.vca = dcVca.TickRes(vs);
		out.vcf = dcVcf.TickRes(vo);
		out.vco = vcoSig;
		out.arg = argV;
		out.lfo = lfoV;
		out.lfoSquare = lfoHigh;
		out.aregGate = aregGateHigh;
		out.vcoSquare = sqHigh;
		lastLfo = lfoV;
		lastArg = argV;
	}

	int32_t LastLfo() const { return lastLfo; }
	int32_t LastArg() const { return lastArg; }
	float VcfRg() const { return 1.0f / vcfGc; }

	// Exposed for the host-side VCF test
	int32_t WDT_RAMFUNC(VcfTick)(int32_t e)
	{
		// Branch Thevenin sources (relative to VG), see circuit-notes.md
		int32_t cA = e - b9;   // + Vo
		int32_t cB = -b10;     // + Vo
		int32_t mm = mul28(cA, vcfGa) + mul28(cB, vcfGb);
		int32_t q = e - mul28(cA - mm, Q28_VCF_RA);
		// Vo = -q / p, limited to the LM324 rails (q is limited first so the
		// product cannot overflow when the op-amp is driven hard into a rail)
		q = clampV(q, -vcfQmax, vcfQmax);
		int32_t vo = -mulShift<22>(q, vcfInvP);
		vo = clampV(vo, V(VCF_OUT_LO), V(VCF_OUT_HI));
		int32_t vx9 = mul28(vo, vcfKx) + mm;
		b9 += mul28(vo + cA - vx9, VCF_K9);
		b10 += mul28(vo + cB - vx9, vcfK10);
		return vo;
	}

private:
	// ---- control-rate groups
	void WDT_RAMFUNC(ControlVco)()
	{
		// expo converter: dV = R11 * sum((Vi - VG) / Ri) at Q4's base.
		// R1 wiper = pitch * B+; R3 from B+; R13 wiper = depth * V_lfo (its
		// pin 3 is on B-, so it contributes -VG R11/R14 even at zero depth);
		// R20 wiper = VG + depth (ARG - VG) with S1 closed.
		constexpr float DV_FIXED = R11 * (-VGND / R2 + (VBP - VGND) / R3 - VGND / R14);
		constexpr float DV_PITCH = R11 * VBP / R2;
		float dv = DV_FIXED + DV_PITCH * ctl.pitch + ctl.pitchCV * (VT * 0.693147f);
		vcoDvStatic = (int32_t)(dv * 16777216.0f);
		float lfoDepth = ctl.vcoMod < 0.0f ? -ctl.vcoMod : 0.0f;
		float argDepth = ctl.vcoMod > 0.0f ? ctl.vcoMod : 0.0f;
		kVcoLfo = (int32_t)(lfoDepth * (R11 / R14 * 268435456.0f));
		kVcoArg = (int32_t)(argDepth * (R11 / R21 * 268435456.0f));
	}

	void WDT_RAMFUNC(ControlLfo)()
	{
		if (ctl.lfoRate == lastLfoRate) return;
		lastLfoRate = ctl.lfoRate;
		// R49 1M (reverse-audio taper here: 334^(1-x)), C16 = 1u
		float r49 = 1e6f * (FastExp2(8.3837f * (1.0f - ctl.lfoRate)) - 1.0f) * (1.0f / 333.0f);
		lfoCap.Set(R48 + r49, C16);
	}

	void WDT_RAMFUNC(ControlAreg)()
	{
		if (ctl.attack == lastAttack && ctl.release == lastRelease && ctl.repeat == lastRepeat) return;
		lastAttack = ctl.attack; lastRelease = ctl.release; lastRepeat = ctl.repeat;
		// R51 / R53 1M (audio taper here: 1000^x)
		rAttack = 1e6f * (FastExp2(9.9658f * ctl.attack) - 1.0f) * (1.0f / 999.0f) + 10.0f;
		rRelease = 1e6f * (FastExp2(9.9658f * ctl.release) - 1.0f) * (1.0f / 999.0f) + 10.0f;
		UpdateAregCoeffs();
	}

	void WDT_RAMFUNC(ControlVcfPots)()
	{
		if (ctl.cutoff == lastCutoff && ctl.reso == lastReso && ctl.vcfMod == lastVcfMod) return;
		lastCutoff = ctl.cutoff; lastReso = ctl.reso; lastVcfMod = ctl.vcfMod;

		// R28 cutoff (exponential taper 100k .. 150R), R29 resonance
		g28 = 1.0f / (100e3f * FastExp2(-9.3808f * ctl.cutoff));
		float x = 1.0f - ctl.reso;
		float gB = 1.0f / (100e3f * x * x + 100.0f + RCap(C10));
		vcfGab = VCF_GA + gB;
		vcfGbQ28 = gB * 268435456.0f;
		vcfK10 = (int32_t)(2.0f * RCap(C10) * gB * 268435456.0f);

		// Q8 gate network. S6 picks the mod source for the top of R38 (or
		// leaves it floating); the R38 wiper feeds R39 into node M, which also
		// has R44 to B+ and R40 + R45 to B-. The gate is R45/(R40+R45) of M.
		float m = ctl.vcfMod;
		float ratio = 0.0f, r14;
		vcfSrcIsArg = m > 0.0f;
		if (m > 0.05f || m < -0.05f)
		{
			float p = ((m > 0 ? m : -m) - 0.05f) / 0.95f;  // R38 wiper position
			float rUp = (1.0f - p) * R38, rDn = R43 + p * R38;
			ratio = rDn / (rUp + rDn);
			r14 = rUp * ratio;
		}
		else r14 = R43 + 0.5f * R38;                     // S6 centre (off)
		float gb = 1.0f / (r14 + R39);
		constexpr float GR = 1.0f / (R40 + R45);
		float k = (R45 * GR) / (1.0f / R44 + gb + GR);
		gate0 = k * (VBP / R44);
		gateSrc = k * ratio * gb;
		gateCv = k / R39;  // CV injected as a current through a notional R39
	}

	// ---- constants
	static constexpr int32_t Q28_LFO_DIV = Q28(R13 / (R56 + R13));
	static constexpr int32_t Q28_VT_LN2 = Q28(VT * 0.693147f);
	static constexpr int32_t Q28_NOISE_GAIN = Q28(R19 / R25);
	static constexpr int32_t Q28_R35_R34 = Q28(R35 / R34);
	static constexpr int32_t Q28_VCA_GATE = Q28(R46 / (R41 + R46));
	static constexpr float VCF_GA = 1.0f / (R35 + RCap(C9));
	static constexpr float VCF_RA = R35 * VCF_GA;
	static constexpr float VCF_GA_Q28 = VCF_GA * 268435456.0f;
	static constexpr float VCF_Q_LIMIT = 4.6f * 16777216.0f;
	static constexpr int32_t Q28_VCF_RA = Q28(VCF_RA);
	static constexpr int32_t VCF_K9 = Q28(2.0f * RCap(C9) * VCF_GA);

	// ---- VCO
	inline int32_t ExpoIncrement(int32_t dv)
	{
		// I = IREF exp(dv / VT); ramp slope = I / C2 per second
		// octaves (Q16) = dv / (VT ln2)
		int32_t oct = mul28(dv, Q28_OCT_PER_V_Q16);
		if (oct > (12 << 16)) oct = 12 << 16;
		if (oct < -(24 << 16)) oct = -(24 << 16);
		int32_t ip = oct >> 16;            // floor
		uint32_t fr = (uint32_t)oct & 0xFFFF;
		uint32_t idx = fr >> 8, sub = fr & 0xFF;
		uint32_t t0 = exp2Table[idx], t1 = exp2Table[idx + 1];
		uint32_t frac = t0 + (((t1 - t0) * sub) >> 8); // Q30, 1..2
		int64_t inc = ((int64_t)INC0 * frac) >> 30;
		inc = ip >= 0 ? (inc << ip) : (inc >> -ip);
		if (inc > INC_MAX) inc = INC_MAX;
		if (inc < 1) inc = 1;
		return (int32_t)inc;
	}
	static constexpr int32_t Q28_OCT_PER_V_Q16 = Q28(65536.0f / 16777216.0f / (VT * 0.693147f));
	static constexpr int32_t INC0 = V(IREF * TS / C2);
	static constexpr int32_t INC_MAX = V(0.45f * 1.08f);
	static constexpr int32_t DEAD_Q16 = (int32_t)(RESET_DEAD_US * 1e-6f * FS * 65536.0f);

	// polyBLEP residuals for a step h at fractional time d (Q16) before now
	static inline int32_t BlepPrev(int32_t h, uint32_t d)
	{
		uint32_t d2 = d * d;                       // Q32
		return (int32_t)(((int64_t)h * (d2 >> 1)) >> 32);
	}
	static inline int32_t BlepCur(int32_t h, uint32_t d)
	{
		uint32_t e = 65536u - d;
		uint32_t e2 = e * e;
		if (e >= 65536u) e2 = 0xFFFFFFFFu;
		return -(int32_t)(((int64_t)h * (e2 >> 1)) >> 32);
	}
	static inline uint32_t FracQ16(int32_t num, int32_t inc)
	{
		int64_t d = ((int64_t)num << 16) / inc;
		if (d < 0) d = 0;
		if (d > 65535) d = 65535;
		return (uint32_t)d;
	}

	void WDT_RAMFUNC(VcoCore)(int32_t inc, bool syncPulse)
	{
		int32_t thresh = V(VTH0);
		if (ctl.sync)
		{
			// loop: drive -> C6 -> node -> R(6k) -> 5.4 V
			int32_t drive = syncPulse ? V(VOL) : V(VOH);
			thresh = V(VTH0) + sync.TickRes(drive - V(VTH0));
		}
		// U1B: + input = (ramp R7 + out R12) / (R7 + R12)
		int32_t top = mul28(thresh, Q28_TOP_GAIN) - V(VOL * R12 / R7);
		bool releaseOk = thresh > V(VGND * R7 / (R7 + R12) + VOH * R12 / (R7 + R12));

		int32_t prevCorrRamp = 0, prevCorrSq = 0, curCorrRamp = 0, curCorrSq = 0;
		int32_t avgV = avg.TickCap(ramp);

		if (!resetting)
		{
			ramp += inc;
			if (!sqHigh && ramp > avgV)
			{
				uint32_t d = FracQ16(ramp - avgV, inc);
				sqHigh = true;
				prevCorrSq += BlepPrev(SQ_STEP, d);
				curCorrSq += BlepCur(SQ_STEP, d);
			}
			if (ramp >= top)
			{
				uint32_t d = FracQ16(ramp - top, inc);
				int32_t atEvent = ramp - (int32_t)(((int64_t)inc * d) >> 16);
				int32_t h = V(VGND) - atEvent;
				prevCorrRamp += BlepPrev(h, d);
				curCorrRamp += BlepCur(h, d);
				if (sqHigh)
				{
					sqHigh = false;
					prevCorrSq += BlepPrev(-SQ_STEP, d);
					curCorrSq += BlepCur(-SQ_STEP, d);
				}
				resetting = true;
				hold = DEAD_Q16 - (int32_t)d;
				ramp = V(VGND);
				if (hold <= 0 && releaseOk)
				{
					resetting = false;
					ramp += (int32_t)(((int64_t)inc * (-hold)) >> 16);
				}
			}
		}
		else
		{
			hold -= 65536;
			ramp = V(VGND);
			if (hold <= 0)
			{
				if (releaseOk)
				{
					resetting = false;
					int32_t f = -hold > 65536 ? 65536 : -hold;
					ramp += (int32_t)(((int64_t)inc * f) >> 16);
				}
				else hold = 0;
			}
		}

		// emit previous sample (one sample latency for the BLEP)
		rampOutPrev = rampPrevNaive + rampPrevCorr + prevCorrRamp;
		sqOutPrev = sqPrevNaive + sqPrevCorr + prevCorrSq;
		rampPrevNaive = ramp - V(VGND);
		sqPrevNaive = sqHigh ? SQ_HI : SQ_LO;
		rampPrevCorr = curCorrRamp;
		sqPrevCorr = curCorrSq;
	}
	static constexpr int32_t Q28_TOP_GAIN = Q28((R7 + R12) / R7);
	// SQR node = VG + (U2B out - VG) * R32 / (R31 + R32)
	static constexpr int32_t SQ_HI = V((VOH - VGND) * R32 / (R31 + R32));
	static constexpr int32_t SQ_LO = V((VOL - VGND) * R32 / (R31 + R32));
	static constexpr int32_t SQ_STEP = SQ_HI - SQ_LO;

	// ---- AREG: C18 charged/discharged through the diode network
	void WDT_RAMFUNC(UpdateAregCoeffs)()
	{
		float rsrc;
		if (ctl.repeat) rsrc = 50.0f;            // U2D output
		else rsrc = aregGateHigh ? R57 : R57 + R61; // S10 to B+ via R57, else R61 to B-
		float rp = rsrc + R50 + RCap(C18);
		float rc = RCap(C18);
		aregKa = Q28(2.0f * rc / (rAttack + rp));
		aregKr = Q28(2.0f * rc / (rRelease + rp));
		aregKaH = Q28(rc / (rAttack + rp));
		aregKrH = Q28(rc / (rRelease + rp));
		aregRsrc = rsrc;
	}

	int32_t WDT_RAMFUNC(AregTick)(bool manualGate)
	{
		int32_t vsrc;
		if (ctl.repeat)
		{
			// U2D: + = Schmitt divider R63/R64/R58, - = ARG
			int32_t th = aregGateHigh ? V(AREG_TH(VOH)) : V(AREG_TH(VOL));
			bool hi = th > lastArg;
			aregGateHigh = hi;
			vsrc = hi ? V(VOH) : V(VOL);
		}
		else
		{
			if (manualGate != aregGateHigh)
			{
				aregGateHigh = manualGate;
				UpdateAregCoeffs(); // source resistance changes
			}
			vsrc = manualGate ? V(VBP) : 0;
		}
		// Root: diode pair. Incident wave a = E - b18 (Thevenin of the series
		// adaptor of source+R50 and C18). Forward (D2, R51) if a > Vd,
		// reverse (D3, R53) if a < -Vd, else open.
		int32_t a = vsrc - b18;
		int32_t db = 0, half = 0;
		if (a > V(VDIODE))
		{
			db = mul28(a - V(VDIODE), aregKa);
			half = mul28(a - V(VDIODE), aregKaH);
		}
		else if (a < -V(VDIODE))
		{
			db = mul28(a + V(VDIODE), aregKr);
			half = mul28(a + V(VDIODE), aregKrH);
		}
		int32_t vc = b18 + half;
		b18 += db;
		// Q9 follower (table, Q16 absolute)
		return Q9Lookup(vc);
	}

	static inline int32_t Q9Lookup(int32_t vg)
	{
		// index = vg / (Q9_VMAX / (Q9_N-1)), Q16 fraction
		if (vg < 0) vg = 0;
		int32_t pos = mul28(vg, Q28_Q9_SCALE); // Q16 index
		int32_t i = pos >> 16;
		if (i >= Q9_N - 1) return q9Table[Q9_N - 1] << 8;
		int32_t f = pos & 0xFFFF;
		int32_t y0 = q9Table[i], y1 = q9Table[i + 1];
		return (y0 + (int32_t)(((int64_t)(y1 - y0) * f) >> 16)) << 8;
	}
	static constexpr int32_t Q28_Q9_SCALE = Q28((Q9_N - 1) / Q9_VMAX * 65536.0f / 16777216.0f);

	static inline int32_t VcaLookup(int32_t vg, int32_t vd)
	{
		if (vg < 0) vg = 0;
		if (vd < 0) vd = 0;
		int32_t pg = mul28(vg, Q28_VCA_GS);
		int32_t pd = mul28(vd, Q28_VCA_DS);
		int32_t ig = pg >> 16, id = pd >> 16;
		int32_t fg = (pg & 0xFFFF) >> 4, fd = (pd & 0xFFFF) >> 4; // Q12
		if (ig >= VCA_GN - 1) { ig = VCA_GN - 2; fg = 4096; }
		if (id >= VCA_DN - 1) { id = VCA_DN - 2; fd = 4096; }
		const int32_t *r0 = vcaTable[ig], *r1 = vcaTable[ig + 1];
		int32_t a0 = r0[id] + (((r0[id + 1] - r0[id]) * fd) >> 12);
		int32_t a1 = r1[id] + (((r1[id + 1] - r1[id]) * fd) >> 12);
		return (a0 + (int32_t)(((int64_t)(a1 - a0) * fg) >> 12)) << 8;
	}
	static constexpr int32_t Q28_VCA_GS = Q28((VCA_GN - 1) / VCA_GMAX * 65536.0f / 16777216.0f);
	static constexpr int32_t Q28_VCA_DS = Q28((VCA_DN - 1) / VCA_DMAX * 65536.0f / 16777216.0f);

	// ---- state
	Controls ctl;
	// VCO
	int32_t vcoDvStatic = 0, kVcoLfo = 0, kVcoArg = 0;
	int32_t ramp = V(VGND);
	bool resetting = false, sqHigh = false;
	int32_t hold = 0;
	int32_t rampOutPrev = 0, sqOutPrev = 0, rampPrevNaive = 0, sqPrevNaive = 0;
	int32_t rampPrevCorr = 0, sqPrevCorr = 0;
	SeriesRC avg, sync;
	// LFO
	SeriesRC lfoCap, lfoTri;
	bool lfoHigh = true;
	// AREG
	int32_t b18 = 0;
	bool aregGateHigh = false;
	float rAttack = 1e5f, rRelease = 1e5f, aregRsrc = 0;
	int32_t aregKa = 0, aregKr = 0, aregKaH = 0, aregKrH = 0;
	// noise + legs
	uint32_t rng = 0x12345678u;
	SeriesRC noiseHp, legVco, legNoise;
	// VCF
	float g28 = 1e-4f, vcfGab = 1e-4f, vcfGbQ28 = 0, vcfGc = 1e-4f;
	float gate0 = 0, gateSrc = 0, gateCv = 0;
	bool vcfSrcIsArg = true;
	unsigned slowPhase = 0;
	float lastLfoRate = -1, lastAttack = -1, lastRelease = -1, lastCutoff = -1, lastReso = -1, lastVcfMod = -9;
	bool lastRepeat = false;
	int32_t b9 = 0, b10 = 0;
	int32_t vcfGa = 0, vcfGb = 0, vcfKx = 0, vcfInvP = 0, vcfQmax = 0, vcfK10 = 0;
	// outputs
	SeriesRC dcVca, dcVcf;
	int32_t lastLfo = 0, lastArg = 0;
};

} // namespace wdt
