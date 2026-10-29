// jfet.h - square-law model of the 2N5457 n-channel JFETs (Q1, Q7, Q8, Q9).
//
// The 2N5457 datasheet spread is wide (Vp -0.5..-6 V, Idss 1..5 mA); these are
// mid-range values. Shared by the table generator (double) and the card's
// control-rate code (float).

#pragma once

namespace jfet
{
	constexpr float VP   = -1.8f;   // pinch-off voltage
	constexpr float IDSS = 3.0e-3f; // drain current at Vgs = 0
	constexpr float BETA = IDSS / (VP * VP);
	constexpr float VGS_CLAMP = 0.5f; // gate-source diode starts conducting

	// Drain -> source current for gate-source vgs and drain-source vds.
	template <typename T>
	T id(T vgs, T vds)
	{
		if (vds < 0) return -id<T>(vgs - vds, -vds); // symmetric device: swap D and S
		if (vgs > T(VGS_CLAMP)) vgs = T(VGS_CLAMP);
		T vov = vgs - T(VP);
		if (vov <= 0) return 0;
		if (vds < vov) return T(BETA) * (T(2) * vov * vds - vds * vds); // triode
		return T(BETA) * vov * vov;                                    // saturation
	}

	// Small-signal channel resistance at vds = 0 (the voltage-controlled
	// resistor in the VCF).
	inline float rds(float vgs)
	{
		if (vgs > VGS_CLAMP) vgs = VGS_CLAMP;
		float vov = vgs - VP;
		if (vov < 1e-6f) return 1e9f;
		float r = 1.0f / (2.0f * BETA * vov);
		return r > 1e9f ? 1e9f : r;
	}
}
