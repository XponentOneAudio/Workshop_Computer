// hypster_circuit.h - component values and the static (memoryless) parts of
// Ian Fritz's Hypster, each modelled as a wave digital filter.
//
// Every block here is a resistor/diode/zener network with no capacitor in it,
// so its WDF solution is a pure function of its input. These functions are
// evaluated in float off the audio path and written into lookup tables that
// the fixed-point core (hypster_core.h) interpolates at 48kHz:
//
//   ZenerPairShaper   ZD1/ZD2 + A2  (NL in the x -> u path)
//                     ZD3/ZD4 + A8  (NL in the u -> z path)
//   BreakpointShaperY D6 + ZD5 + A11 (NL in the z -> y path)
//   BreakpointShaperX D7 + ZD6 + A14 (NL in the y -> x path)
//   ShapeNetwork      R94-R98, D8, D9, S1, S2  (cross coupling, both options)
//   Limiter           D2-D4 between the Gain and Damping control currents
//
// Part numbers / designators follow Fritz's schematics (Jan 2018).

#pragma once
#include "wdf.h"

namespace hyp
{

// ------------------------------------------------------------ device models
// 1N4148: Is = 2.52nA, n = 1.752
constexpr float kDiodeIs = 2.52e-9f;
constexpr float kDiodeVt = 1.752f * 0.025852f;

// 4.7V zener in breakdown, modelled as an exponential with a soft knee:
// 4.7V at 5mA and an effective thermal voltage of 0.35V. Low voltage zeners
// (< 5.6V) break down by tunnelling and have a famously soft knee - they
// leak noticeably from about 3.5V - and that softness shapes how the NL
// stages bend, so it is kept.
constexpr float kZenerVz = 4.7f, kZenerIz = 5.0e-3f, kZenerVt = 0.35f;

// A zener in breakdown in series with a forward junction is still a single
// exponential law (the log terms add), so the pair becomes one equivalent
// junction with Vt = Vt_diode + Vt_zener.
constexpr float kStringVt = kDiodeVt + kZenerVt;
inline float StringIs()
{
	// voltage of the string at the zener's test current
	const float v = kZenerVz + kDiodeVt * logf(kZenerIz / kDiodeIs);
	return kZenerIz * expf(-v / kStringVt);
}

// TL07x on +/-12V: output saturates near +/-10.8V. Gentle knee from 10V.
constexpr float kRail = 10.8f;
inline float OpAmpSat(float v)
{
	constexpr float knee = 10.0f, span = kRail - knee;
	const float m = fabsf(v);
	if (m <= knee) return v;
	const float y = knee + span * tanhf((m - knee) / span);
	return v < 0.0f ? -y : y;
}

// Output impedance given to ideal (op-amp driven) sources so that they can
// sit in an adapted WDF port.
constexpr float kRout = 10.0f;

// ------------------------------------------------------------ OTA integrators
// LM13700: gm = Iabc / (2 VT). Input attenuators are 220R to ground.
constexpr float kVT2 = 2.0f * 0.025852f;
constexpr float kCint = 1.0e-9f;   // C1..C4
constexpr float kRdiv = 220.0f;    // R12, R13, R56, R57, ...

inline constexpr float Div(float rTop, float rBottom) { return rBottom / (rTop + rBottom); }
// Input / damping attenuations seen by the OTA differential inputs
constexpr float kAinU = Div(82e3f, kRdiv);    // R11 / R12
constexpr float kAinZ = Div(82e3f, kRdiv);    // R55 / R56
constexpr float kAdZ = Div(82e3f, kRdiv);     // R59 / R57
constexpr float kAinY = Div(47e3f, kRdiv);    // R70 / R71
constexpr float kAdY = Div(220e3f, kRdiv);    // R74 / R72
constexpr float kAinX = Div(47e3f, kRdiv);    // R85 / R86
constexpr float kAdX = Div(330e3f, kRdiv);    // R89 / R87
constexpr float kAinG = Div(82e3f, kRdiv);    // R1 / R2 (gain OTA U1)
constexpr float kAdU = Div(180e3f, kRdiv);    // R19 / R20 (damping OTA U3)
constexpr float kR5 = 220e3f;                 // A1 transimpedance

// ------------------------------------------------------------ control ranges
// Gain (c) and Damping (b) are linear voltage-to-current converters (A5/Q1,
// A6/Q2, 18k emitter resistors). Rate (a) is the A7/Q3/Q4 exponential
// converter, whose current is shared by the four integrator OTAs through
// R14, R58, R73, R88.
constexpr float kRemit = 18e3f;          // R32, R41
constexpr float kGainPerV = 100.0f / 220.0f;  // R30 / R24
constexpr float kDampPerV = 100.0f / 270.0f;  // R39 / R35
// R38 560k into A6's summing node sets a minimum damping current: with no
// damping at all the u integrator is a pure integrator and any DC in the ring
// runs it into a rail. 12V * R39/R38:
constexpr float kDampOffsetV = 12.0f * 100.0f / 560.0f;
constexpr float kPotV = 12.0f;           // panel pots run 0..12V
constexpr float kTrimSpan = 1.2f;        // R28/R29 1M: +/-1.2V at A5

// Per-OTA rate current range. 2^16 = 16 octaves.
constexpr float kIaMin = 4.0e-9f;
constexpr float kRateOctaves = 16.0f;

// ------------------------------------------------------------ ZD pair + amp
// Vin -> [two 4.7V zeners back to back] -> node N (Rload to ground, and Rm to
// the amp's inverting input, which sits at kPlus*Vin) ; amp output
//     Vo = kPlus*Vin*(1 + Rf/Rm) - (Rf/Rm)*V_N
// A2: R6 2k2, R7 33k, R8 100k, R9 220k / R10 100k
// A8: R50 2k2, R51 33k, R54 100k, R52 330k / R53 100k
class ZenerPairShaper
{
public:
	ZenerPairShaper(float rLoad, float rM, float rF, float rPlusTop, float rPlusBottom)
	    : src(kRout), rload(rLoad), rm(rM), par(rload, rm), inv(par), ser(src, inv),
	      root(ser, StringIs(), kStringVt), Rf(rF), kPlus(Div(rPlusTop, rPlusBottom))
	{
	}

	// returns the amp output for input Vin (already a real op-amp voltage)
	float operator()(float vin)
	{
		src.setVoltage(vin);
		rm.setVoltage(kPlus * vin);
		root.process();
		const float vN = rload.voltage();
		const float vo = kPlus * vin * (1.0f + Rf / rm.R) - (Rf / rm.R) * vN;
		return OpAmpSat(vo);
	}

private:
	wdf::ResistiveVoltageSource src;
	wdf::Resistor rload;
	wdf::ResistiveVoltageSource rm;
	wdf::Parallel par;
	wdf::PolarityInverter inv;
	wdf::Series ser;
	wdf::DiodePairRoot root;
	float Rf, kPlus;
};

// ------------------------------------------------------------ z -> y (A11)
// Z -> R62/R63 1k:1k divider (M) -> D6 + ZD5 -> node N, R66 2k2 to ground.
// The string conducts N -> M (only when M is below about -5V).
// A11 non-inverting from N, inverting from M through R67 180k:
//     Vo = V_N (1 + R68/R67 + R68/R69) - (R68/R67) V_M
class BreakpointShaperY
{
public:
	BreakpointShaperY()
	    : src(500.0f), r66(2.2e3f), inv(r66), ser(src, inv),
	      root(ser, StringIs(), kStringVt)
	{
	}
	float operator()(float z)
	{
		src.setVoltage(0.5f * z);
		root.process();                 // root voltage = V_N - V_M
		const float vN = r66.voltage();
		const float vM = src.voltage();
		constexpr float R67 = 180e3f, R68 = 100e3f, R69 = 39e3f;
		const float vo = vN * (1.0f + R68 / R67 + R68 / R69) - (R68 / R67) * vM;
		return OpAmpSat(vo);
	}

private:
	wdf::ResistiveVoltageSource src;
	wdf::Resistor r66;
	wdf::PolarityInverter inv;
	wdf::Series ser;
	wdf::DiodeRoot root;
};

// ------------------------------------------------------------ y -> x (A14)
// Y -> R77/R78 1k:1k divider (M, also loaded by R83 220k to A14's virtual
// ground) -> D7 + ZD6 -> node N (R81 2k2 to ground, R82 100k to virtual
// ground). The string conducts M -> N (only when M is above about +5V).
// A14 inverting summer:  Vo = -(R84/R82 V_N + R84/R83 V_M)
class BreakpointShaperX
{
public:
	BreakpointShaperX()
	    : src(Par3(1e3f, 1e3f, 220e3f)), rN(Par2(2.2e3f, 100e3f)), invSrc(src),
	      ser(invSrc, rN), root(ser, StringIs(), kStringVt)
	{
	}
	float operator()(float y)
	{
		// Thevenin of the divider node with R83 to virtual ground
		const float rb = Par2(1e3f, 220e3f);
		src.setVoltage(y * rb / (1e3f + rb));
		root.process();                 // root voltage = V_M - V_N
		const float vN = rN.voltage();
		const float vM = src.voltage();
		constexpr float R82 = 100e3f, R83 = 220e3f, R84 = 100e3f;
		return OpAmpSat(-(R84 / R82 * vN + R84 / R83 * vM));
	}

private:
	static float Par2(float a, float b) { return a * b / (a + b); }
	static float Par3(float a, float b, float c) { return 1.0f / (1.0f / a + 1.0f / b + 1.0f / c); }
	wdf::ResistiveVoltageSource src;
	wdf::Resistor rN;
	wdf::PolarityInverter invSrc;
	wdf::Series ser;
	wdf::DiodeRoot root;
};

// ------------------------------------------------------------ limiter D2-D4
// Optional limiter: three 1N4148 in series from the Gain current node (c) to
// the Damping current node (b). Both nodes feed an OTA bias pin (about 2Vbe
// above -12V) through 10k (R4, R22) and are driven by transistor current
// sources (Q1, Q2). Seen from the diodes that is a 10k(Ic - Ib) source behind
// 20k. Returns the current the diodes steer from Gain into Damping.
class Limiter
{
public:
	Limiter() : src(10e3f), r(10e3f), ser(src, r), root(ser, kDiodeIs, 3.0f * kDiodeVt) {}
	float operator()(float dI)
	{
		src.setVoltage(-10e3f * dI); // sign: the root sees -(v_src + v_r)
		root.process();
		return root.current();
	}

private:
	wdf::ResistiveVoltageSource src;
	wdf::Resistor r;
	wdf::Series ser;
	wdf::DiodeRoot root;
};

// ------------------------------------------------------------ Shape network
// Cross coupling from -X' (A16 output) into the damping nodes A (U6 +in) and
// B (U5 +in), each 220R to ground. Fritz gives two options:
//
// Option 2: -X' - R97 22k - D8 (|| D9 when S1 'symm' is closed) - wiper of
//           R95 100k ; pot ends -> R94 100k -> A and R96 100k -> B.
//           'fat' = wiper at the A end, 'slim' = wiper at the B end.
// Option 1: -X' - R98 82k - J ; J - R97 470k - P ; S2 'wild' adds D8/D9
//           antiparallel across R97 ; S1 picks P = 'squirrelly' (R94 220k
//           end) or 'curly' (R96 82k end), R95 100k between the two.
enum class ShapeMode : int
{
	Opt2Asym = 0,      // option 2, S1 open (one diode)
	Opt2Symm,          // option 2, S1 closed (both diodes)
	Opt1SquirrelyMild, // option 1, S1 squirrelly, S2 mild
	Opt1SquirrelyWild, // option 1, S1 squirrelly, S2 wild
	Opt1CurlyMild,     // option 1, S1 curly, S2 mild
	Opt1CurlyWild,     // option 1, S1 curly, S2 wild
	Count
};

struct ShapeOut
{
	float vA, vB; // voltages injected at the U6 (A) and U5 (B) damping nodes
};

class ShapeNetwork
{
public:
	ShapeNetwork()
	    : src2(22e3f + kRout), rPar2(50e3f), inv2(rPar2), ser2(src2, inv2),
	      d2(ser2, kDiodeIs, kDiodeVt), dd2(ser2, kDiodeIs, kDiodeVt),
	      src1(82e3f + kRout), rNet1(50e3f), inv1(rNet1), ser1(src1, inv1),
	      r97(470e3f), par1(r97, ser1), dd1(par1, kDiodeIs, kDiodeVt)
	{
	}

	// mode and pot position (0 = fat, 1 = slim) for option 2
	void Configure(ShapeMode m, float pot)
	{
		mode = m;
		const float rA = 100e3f + pot * 100e3f + kRdiv; // R94 + upper pot + 220R
		const float rB = 100e3f + (1.0f - pot) * 100e3f + kRdiv;
		if (m == ShapeMode::Opt2Asym || m == ShapeMode::Opt2Symm)
		{
			toA = rA;
			toB = rB;
		}
		else
		{
			const bool squirrely = (m == ShapeMode::Opt1SquirrelyMild || m == ShapeMode::Opt1SquirrelyWild);
			if (squirrely)
			{
				toA = 220e3f + kRdiv;          // R94
				toB = 100e3f + 82e3f + kRdiv;  // R95 + R96
			}
			else
			{
				toA = 100e3f + 220e3f + kRdiv; // R95 + R94
				toB = 82e3f + kRdiv;           // R96
			}
		}
		const float rNet = toA * toB / (toA + toB);
		rPar2.setResistance(rNet);
		rNet1.setResistance(rNet);
	}

	ShapeOut operator()(float x)
	{
		const float vSrc = -x; // A16 output is -X'
		float vNode;           // voltage of the node feeding toA / toB
		switch (mode)
		{
		case ShapeMode::Opt2Asym:
			src2.setVoltage(vSrc);
			d2.process();      // root voltage = V_W - V_J: D8 anode at the wiper
			vNode = rPar2.voltage();
			break;
		case ShapeMode::Opt2Symm:
			src2.setVoltage(vSrc);
			dd2.process();
			vNode = rPar2.voltage();
			break;
		case ShapeMode::Opt1SquirrelyWild:
		case ShapeMode::Opt1CurlyWild:
			src1.setVoltage(vSrc);
			dd1.process();
			vNode = rNet1.voltage();
			break;
		default: // option 1 'mild': resistive only
			vNode = vSrc * rNet1.R / (src1.R + 470e3f + rNet1.R);
			break;
		}
		return {vNode * kRdiv / toA, vNode * kRdiv / toB};
	}

private:
	ShapeMode mode = ShapeMode::Opt2Asym;
	float toA = 150e3f, toB = 150e3f;
	// option 2 tree
	wdf::ResistiveVoltageSource src2;
	wdf::Resistor rPar2;
	wdf::PolarityInverter inv2;
	wdf::Series ser2;
	wdf::DiodeRoot d2;
	wdf::DiodePairRoot dd2;
	// option 1 tree
	wdf::ResistiveVoltageSource src1;
	wdf::Resistor rNet1;
	wdf::PolarityInverter inv1;
	wdf::Series ser1;
	wdf::Resistor r97;
	wdf::Parallel par1;
	wdf::DiodePairRoot dd1;
};

} // namespace hyp
