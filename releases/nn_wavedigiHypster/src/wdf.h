// wdf.h - a small wave digital filter (WDF) toolkit.
//
// Voltage waves, as in Fettweis / Werner / chowdsp_wdf:
//     a = v + R i      (incident on an element)
//     b = v - R i      (reflected from an element)
// with i the current flowing INTO the element and R the port resistance.
//
// The elements are linked into a binary connection tree. Leaves and adaptors
// are "adapted" (their reflected wave does not depend on the incident wave
// on the same port), so the whole subtree collapses onto one port that is
// closed by a single nonlinear root element (a diode, a diode pair, ...).
// The diode roots use the closed-form Wright-omega solution of Werner et al.
// (DAFx 2015) followed by a Newton polish, so they are exact to float
// precision.
//
// This file is float based and is used OFF the audio interrupt: at boot
// (memoryless shaper tables) and on core 1 (Shape network tables). The
// audio-rate integrators live in hypster_core.h, in fixed point.

#pragma once
#include <math.h>

namespace wdf
{

// ---------------------------------------------------------------- Wright omega
// omega(x) solves  w + ln(w) = x.
inline float omega(float x)
{
	float w;
	if (x < -3.341459552768620f)
	{
		// w ~ e^x for very negative x; one Newton step below tidies it up
		w = expf(x);
	}
	else if (x < 8.0f)
	{
		// cubic fit (D'Angelo et al.)
		w = 6.313183464296682e-1f +
		    x * (3.631952663804445e-1f +
		    x * (4.775931364975583e-2f +
		    x * -1.314293149877800e-3f));
	}
	else
	{
		w = x - logf(x);
	}
	// Newton on  f(w) = w + ln w - x,  f' = 1 + 1/w
	for (int k = 0; k < 3; k++)
	{
		if (w <= 0.0f) { w = expf(x); break; }
		w = w - (w + logf(w) - x) * w / (1.0f + w);
	}
	return w;
}

// ---------------------------------------------------------------- base node
struct Node
{
	float R = 1.0f, G = 1.0f; // port resistance / conductance
	float a = 0.0f, b = 0.0f; // incident / reflected wave
	Node *parent = nullptr;

	virtual void calcImpedance() {}
	virtual void incident(float x) = 0; // wave arriving at this element
	virtual float reflected() = 0;      // compute and return b

	void setPortResistance(float r)
	{
		R = r;
		G = 1.0f / r;
	}
	void propagateImpedance()
	{
		calcImpedance();
		if (parent) parent->propagateImpedance();
	}
	float voltage() const { return 0.5f * (a + b); }
	float current() const { return 0.5f * (a - b) * G; } // into the element
};

// ---------------------------------------------------------------- leaves
struct Resistor : Node
{
	explicit Resistor(float r) { setPortResistance(r); }
	void setResistance(float r)
	{
		setPortResistance(r);
		if (parent) parent->propagateImpedance();
	}
	void incident(float x) override { a = x; }
	float reflected() override
	{
		b = 0.0f;
		return b;
	}
};

// Voltage source Vs in series with resistance R.
struct ResistiveVoltageSource : Node
{
	float Vs = 0.0f;
	explicit ResistiveVoltageSource(float r) { setPortResistance(r); }
	void setResistance(float r)
	{
		setPortResistance(r);
		if (parent) parent->propagateImpedance();
	}
	void setVoltage(float v) { Vs = v; }
	void incident(float x) override { a = x; }
	float reflected() override
	{
		b = Vs;
		return b;
	}
};

// Flips the polarity of the port below it.
struct PolarityInverter : Node
{
	Node &p;
	explicit PolarityInverter(Node &port) : p(port)
	{
		p.parent = this;
		calcImpedance();
	}
	void calcImpedance() override { setPortResistance(p.R); }
	void incident(float x) override
	{
		a = x;
		p.incident(-x);
	}
	float reflected() override
	{
		b = -p.reflected();
		return b;
	}
};

// ---------------------------------------------------------------- adaptors
// Three-port series adaptor, adapted at the parent port: R = R1 + R2.
struct Series : Node
{
	Node &p1, &p2;
	Series(Node &a1, Node &a2) : p1(a1), p2(a2)
	{
		p1.parent = this;
		p2.parent = this;
		calcImpedance();
	}
	void calcImpedance() override { setPortResistance(p1.R + p2.R); }
	void incident(float x) override
	{
		a = x;
		const float s = x + p1.b + p2.b;
		p1.incident(p1.b - p1.R * G * s);
		p2.incident(p2.b - p2.R * G * s);
	}
	float reflected() override
	{
		b = -(p1.reflected() + p2.reflected());
		return b;
	}
};

// Three-port parallel adaptor, adapted at the parent port: G = G1 + G2.
struct Parallel : Node
{
	Node &p1, &p2;
	Parallel(Node &a1, Node &a2) : p1(a1), p2(a2)
	{
		p1.parent = this;
		p2.parent = this;
		calcImpedance();
	}
	void calcImpedance() override { setPortResistance(1.0f / (p1.G + p2.G)); }
	void incident(float x) override
	{
		a = x;
		// node voltage v = (a + b) / 2 ; each child receives 2v - (its b)
		const float twoV = x + b;
		p1.incident(twoV - p1.b);
		p2.incident(twoV - p2.b);
	}
	float reflected() override
	{
		b = (p1.G * p1.reflected() + p2.G * p2.reflected()) * R;
		return b;
	}
};

// ---------------------------------------------------------------- roots
// Exponential diode (or any one-directional exponential junction string):
//     i = Is (exp(v / Vt) - 1)
// Root of the tree: closes the port presented by 'child'.
struct DiodeRoot
{
	Node &child;
	float Is, Vt;
	float a = 0.0f, b = 0.0f;
	DiodeRoot(Node &c, float is, float vt) : child(c), Is(is), Vt(vt) {}

	// solve for the wave reflected by an exponential junction at port
	// resistance R, given incident wave a. Exact to float precision.
	static float solve(float a, float R, float Is, float Vt)
	{
		const float RIs = R * Is;
		float b = a + 2.0f * RIs -
		          2.0f * Vt * omega(logf(RIs / Vt) + (a + RIs) / Vt);
		// Newton polish on v:  g(v) = v - a + R Is (e^{v/Vt} - 1) = 0
		float v = 0.5f * (a + b);
		for (int k = 0; k < 2; k++)
		{
			const float e = expf(v / Vt);
			const float g = v - a + RIs * (e - 1.0f);
			const float gp = 1.0f + RIs * e / Vt;
			v -= g / gp;
		}
		return 2.0f * v - a;
	}

	void process()
	{
		a = child.reflected();
		b = solve(a, child.R, Is, Vt);
		child.incident(b);
	}
	float voltage() const { return 0.5f * (a + b); }
	float current() const { return 0.5f * (a - b) / child.R; }
};

// Antiparallel pair of identical exponential junctions:
//     i = Is (exp(v / Vt) - exp(-v / Vt))
struct DiodePairRoot
{
	Node &child;
	float Is, Vt;
	float a = 0.0f, b = 0.0f;
	DiodePairRoot(Node &c, float is, float vt) : child(c), Is(is), Vt(vt) {}

	static float solve(float a, float R, float Is, float Vt)
	{
		// The reverse junction is negligible wherever the forward one
		// conducts, so start from the single-junction closed form for the
		// conducting direction, then polish with the full pair law.
		const float lambda = (a >= 0.0f) ? 1.0f : -1.0f;
		float b = lambda * DiodeRoot::solve(lambda * a, R, Is, Vt);
		float v = 0.5f * (a + b);
		const float RIs = R * Is;
		for (int k = 0; k < 2; k++)
		{
			const float ep = expf(v / Vt), em = expf(-v / Vt);
			const float g = v - a + RIs * (ep - em);
			const float gp = 1.0f + RIs * (ep + em) / Vt;
			v -= g / gp;
		}
		return 2.0f * v - a;
	}

	void process()
	{
		a = child.reflected();
		b = solve(a, child.R, Is, Vt);
		child.incident(b);
	}
	float voltage() const { return 0.5f * (a + b); }
	float current() const { return 0.5f * (a - b) / child.R; }
};

} // namespace wdf
