// Host-side tests for the Guided Wander engine (wander.h).
//
//   g++ -std=c++17 -O1 -Wall -Wextra -I.. engine_test.cpp -o engine_test && ./engine_test

#include "wander.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace wander;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
	printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, tol) do { checks++; double _a = (a), _b = (b); \
	if (!(fabs(_a - _b) <= (tol))) { failures++; \
	printf("FAIL line %d: %s = %g, expected %g +- %g\n", __LINE__, #a, _a, _b, double(tol)); } } while (0)

// ---------------------------------------------------------------------------
static void TestScales()
{
	for (int i = 0; i < kNumScales; i++)
	{
		const ScaleDef &s = kScales[i];
		CHECK(s.name && s.name[0]);
		CHECK(s.count >= 1 && s.count <= 8);
		CHECK(s.cents[0] == 0);
		for (int k = 1; k < s.count; k++) CHECK(s.cents[k] > s.cents[k - 1]);
		CHECK(s.cents[s.count - 1] < 1200);
		// Names are unique, so the editor can tell them apart
		for (int j = 0; j < i; j++) CHECK(strcmp(s.name, kScales[j].name) != 0);
	}
	CHECK(strcmp(kScales[0].name, "Major") == 0);

	ScaleTable t;
	BuildScaleTable(0, t);
	CHECK(t.count == 7);
	CHECK(t.mv[0] == 0 && t.mv[1] == 167 && t.mv[2] == 333 && t.mv[6] == 917);   // 200 cents = 166.7 mV
	// 5-TET: 240 cents = 200 mV
	BuildScaleTable(21, t);
	CHECK(strcmp(kScales[21].name, "5-TET") == 0 && t.count == 5);
	CHECK(t.mv[1] == 200 && t.mv[4] == 800);
	// Out of range indices are clamped
	BuildScaleTable(-3, t); CHECK(t.count == 7);
	BuildScaleTable(99, t); CHECK(strcmp(kScales[kNumScales - 1].name, "Pelog (typical)") == 0 && t.count == 7);

	// Pitch: octave slider 0 is -2V, 7 is +5V; the highest note stays inside +-6V
	BuildScaleTable(0, t);
	CHECK(PitchMv(t, 0, 0) == -2000);
	CHECK(PitchMv(t, 0, 2) == 0);
	CHECK(PitchMv(t, 2, 3) == 1333);
	for (int i = 0; i < kNumScales; i++)
	{
		BuildScaleTable(i, t);
		CHECK(PitchMv(t, t.count - 1, 7) < 6000);
		CHECK(PitchMv(t, 0, 0) >= -6000);
	}
	// A note past the end of the scale is clamped
	BuildScaleTable(21, t);
	CHECK(PitchMv(t, 7, 2) == PitchMv(t, 4, 2));
}

// ---------------------------------------------------------------------------
static void TestPick()
{
	uint32_t rng = 12345;
	int32_t w[8] = {100, 0, 300, 0, 0, 600, 0, 0};
	int count[8] = {};
	const int N = 200000;
	for (int i = 0; i < N; i++) count[PickWeighted(w, 8, rng)]++;
	CHECK(count[1] == 0 && count[3] == 0 && count[4] == 0 && count[6] == 0 && count[7] == 0);
	CHECK_NEAR(count[0] / double(N), 0.1, 0.01);
	CHECK_NEAR(count[2] / double(N), 0.3, 0.01);
	CHECK_NEAR(count[5] / double(N), 0.6, 0.01);

	// Only the first n are considered: notes past the end of a scale
	memset(count, 0, sizeof count);
	int32_t u[8] = {10, 10, 10, 10, 10, 10, 10, 10};
	for (int i = 0; i < N; i++) count[PickWeighted(u, 5, rng)]++;
	CHECK(count[5] == 0 && count[6] == 0 && count[7] == 0);
	for (int i = 0; i < 5; i++) CHECK_NEAR(count[i] / double(N), 0.2, 0.01);

	// All zero: no preference, every one equally
	memset(count, 0, sizeof count);
	int32_t z[8] = {};
	for (int i = 0; i < N; i++) count[PickWeighted(z, 8, rng)]++;
	for (int i = 0; i < 8; i++) CHECK_NEAR(count[i] / double(N), 0.125, 0.01);

	// Relative, not absolute: a slider twice as high is twice as likely
	memset(count, 0, sizeof count);
	int32_t r[8] = {1000, 2000, 0, 0, 0, 0, 0, 0};
	for (int i = 0; i < N; i++) count[PickWeighted(r, 8, rng)]++;
	CHECK_NEAR(double(count[1]) / count[0], 2.0, 0.1);
}

// ---------------------------------------------------------------------------
static void TestWeights()
{
	int32_t params[8][8] = {};
	for (int p = 0; p < 4; p++) for (int i = 0; i < 8; i++) params[p][i] = (p + 1) * 100 + i;

	Weights w0, w1;
	// Voice 2's pages all zero: it shares voice 1's, pair by pair
	BuildWeights(params, 0, 8, w0);
	BuildWeights(params, 1, 8, w1);
	CHECK(memcmp(&w0, &w1, sizeof w0) == 0);

	// One of voice 2's pages set: only that pair is independent
	params[6][3] = 50;            // gates 2
	BuildWeights(params, 1, 8, w1);
	CHECK(memcmp(w1.note, w0.note, sizeof w0.note) == 0);
	CHECK(memcmp(w1.octave, w0.octave, sizeof w0.octave) == 0);
	CHECK(memcmp(w1.ratchet, w0.ratchet, sizeof w0.ratchet) == 0);
	CHECK(w1.gate[3] == 50 && w1.gate[0] == 0 && w1.gate[7] == 0);
	CHECK(w0.gate[3] == 303);     // voice 1 unaffected
	BuildWeights(params, 0, 8, w0);
	CHECK(w0.gate[3] == 303);

	// A single non-zero slider is enough, however low
	params[4][0] = 1;
	BuildWeights(params, 1, 8, w1);
	CHECK(w1.note[0] == 1 && w1.note[1] == 0);

	// Sliders past the end of the scale are ignored
	BuildWeights(params, 0, 5, w0);
	CHECK(w0.note[4] == 104 && w0.note[5] == 0 && w0.note[7] == 0);
	CHECK(w0.octave[7] == 207);   // only notes are limited by the scale
}

// ---------------------------------------------------------------------------
static void TestRatchetTimes()
{
	// 1x, 1.5x, 2x, 3x
	CHECK(HitCount(2) == 1 && HitCount(3) == 2 && HitCount(4) == 2 && HitCount(6) == 3);
	const int32_t L = 24000;
	for (int r = 0; r < 4; r++)
	{
		int n = HitCount(kRate2[r]);
		CHECK(HitTime(0, L, kRate2[r]) == 0);
		for (int k = 1; k < n; k++) CHECK(HitTime(k, L, kRate2[r]) > HitTime(k - 1, L, kRate2[r]));
		CHECK(HitTime(n - 1, L, kRate2[r]) < L);
		CHECK(HitTime(n, L, kRate2[r]) >= L);   // the next would fall in the next step
	}
	CHECK(HitTime(1, L, 4) == 12000);                          // 2x: halfway
	CHECK(HitTime(1, L, 6) == 8000 && HitTime(2, L, 6) == 16000); // 3x: thirds
	CHECK(HitTime(1, L, 3) == 16000);                          // 1.5x: two thirds
	CHECK(RatchetHeld(0) && RatchetHeld(3) && !RatchetHeld(4) && !RatchetHeld(7));
}

// ---------------------------------------------------------------------------
// Steps from a clock, a count of them in a number of samples
static int RunClock(StepClock &c, int num, int den, int samples, int period, int *firstStep = nullptr,
	int clocks = -1, int *stepsAt = nullptr, int maxSteps = 0)
{
	int steps = 0;
	int sent = 0;
	for (int i = 0; i < samples; i++)
	{
		bool edge = period > 0 && i % period == 0 && (clocks < 0 || sent < clocks);
		if (edge) sent++;
		if (c.Tick(edge, num, den))
		{
			if (steps == 0 && firstStep) *firstStep = i;
			if (stepsAt && steps < maxSteps) stepsAt[steps] = i;
			steps++;
		}
	}
	return steps;
}

static void TestStepClock()
{
	// No clock: 120 BPM, a step every 24000 samples, the first at once
	{
		StepClock c; int first = -1;
		int n = RunClock(c, 1, 1, 24000 * 10, 0, &first);
		CHECK(first == 0);
		CHECK(n == 10);
		CHECK(!c.Clocked());
		CHECK(c.StepLength(1, 1) == 24000);
	}
	// Free running with ratios
	{
		StepClock a; CHECK(RunClock(a, 1, 2, 48000 * 8, 0) == 8);      // /2: every second
		StepClock b; CHECK(RunClock(b, 3, 1, 24000 * 4, 0) == 12);     // x3
		StepClock d; CHECK(RunClock(d, 1, 8, 24000 * 16, 0) == 2);     // /8: every 4 s
		CHECK(a.StepLength(1, 2) == 48000 && b.StepLength(3, 1) == 8000);
	}
	// Clocked, x1: a step on every clock from the second
	{
		StepClock c; int stepsAt[64]; const int P = 12000;
		int n = RunClock(c, 1, 1, P * 20 + 1, P, nullptr, -1, stepsAt, 64);
		CHECK(c.Clocked());
		CHECK(n >= 19 && n <= 21);
		// steps land on the clocks, once it is following them
		for (int k = 2; k < n; k++) CHECK(stepsAt[k] % P == 0);
		CHECK(c.StepLength(1, 1) == P);
	}
	// Clocked, /4: every fourth clock
	{
		StepClock c; int stepsAt[64]; const int P = 12000;
		int n = RunClock(c, 1, 4, P * 40 + 1, P, nullptr, -1, stepsAt, 64);
		CHECK(n >= 9 && n <= 11);
		for (int k = 2; k < n; k++) CHECK(stepsAt[k] % P == 0 && (stepsAt[k] - stepsAt[k - 1]) == 4 * P);
		CHECK(c.StepLength(1, 4) == 4 * P);
	}
	// Clocked, x4: four evenly spaced steps a clock, locked to it
	{
		StepClock c; int stepsAt[256]; const int P = 12000;
		int n = RunClock(c, 4, 1, P * 20 + 1, P, nullptr, -1, stepsAt, 256);
		CHECK(n >= 4 * 19 && n <= 4 * 21);
		for (int k = 12; k < n; k++)
		{
			int gap = stepsAt[k] - stepsAt[k - 1];
			CHECK(gap == P / 4 || gap == P / 4 + 1 || gap == P / 4 - 1);
		}
		CHECK(c.StepLength(4, 1) == P / 4);
	}
	// x3: thirds, even when the period doesn't divide
	{
		StepClock c; int stepsAt[256]; const int P = 10000;
		int n = RunClock(c, 3, 1, P * 20 + 1, P, nullptr, -1, stepsAt, 256);
		CHECK(n >= 3 * 19);
		for (int k = 12; k < n; k++)
		{
			int gap = stepsAt[k] - stepsAt[k - 1];
			CHECK(gap >= P / 3 - 2 && gap <= P / 3 + 2);
		}
	}
	// The clock stops: back to the internal tempo after four periods (2 s at least)
	{
		StepClock c; const int P = 12000;
		RunClock(c, 1, 1, P * 6, P, nullptr, 6);
		CHECK(c.Clocked());
		// clocks stop; at least 2 s (96000 samples) later it gives up
		int n = 0;
		for (int i = 0; i < 48000 * 4; i++) n += c.Tick(false, 1, 1);
		CHECK(!c.Clocked());
		CHECK(n >= 3);
	}
	// A very fast clock isn't followed any faster than 1 ms
	{
		StepClock c; const int P = 10;
		RunClock(c, 1, 1, 2000, P);
		CHECK(c.StepLength(1, 1) == 48);
	}
}

// ---------------------------------------------------------------------------
// A voice running over a number of steps, counting triggers and watching gates
struct Run
{
	int hits = 0, triggers = 0;
	int gateSamples = 0;
	int firstGateFall = -1;
};

static Weights Only(int note, int octave, int gate, int ratchet)
{
	Weights w{};
	w.note[note] = 100;
	w.octave[octave] = 100;
	w.gate[gate] = 100;
	w.ratchet[ratchet] = 100;
	return w;
}

static void TestVoice()
{
	ScaleTable t; BuildScaleTable(0, t);
	const int32_t L = 24000;

	// 1x, a 3/7 gate: one hit, a gate 3/7 of the step long, one trigger
	{
		Voice v(7); Weights w = Only(2, 3, 3, 0);
		v.BeginStep(w, t, L);
		CHECK(v.Hits() == 1 && v.Gate() && v.TakeTrigger());
		CHECK(v.Note() == 2 && v.Octave() == 3 && v.GateIndex() == 3 && v.Ratchet() == 0);
		CHECK(v.PitchMv() == PitchMv(t, 2, 3));
		int on = 1, hits = 0;
		for (int i = 1; i < L; i++) { v.Tick(w, t); if (v.Gate()) on++; if (v.TakeTrigger()) hits++; }
		CHECK(hits == 0);
		CHECK_NEAR(on, L * 3 / 7, 2);
		CHECK(v.Hits() == 1);
	}
	// A rest (0%): no gate, no trigger, but the pitch still moves
	{
		Voice v(7); Weights w = Only(4, 5, 0, 0);
		v.BeginStep(w, t, L);
		CHECK(!v.Gate() && !v.TakeTrigger() && v.PitchMv() == PitchMv(t, 4, 5));
		for (int i = 1; i < L; i++) { v.Tick(w, t); CHECK(!v.Gate()); }
	}
	// 3x held: three hits a third of a step apart, the same note each time
	{
		Voice v(7); Weights w = Only(1, 2, 2, 3);
		int trig = 0; int hitAt[4] = {}; int nh = 0; uint8_t last = v.Hits();
		v.BeginStep(w, t, L);
		if (v.TakeTrigger()) trig++;
		hitAt[nh++] = 0; last = v.Hits();
		for (int i = 1; i < L; i++)
		{
			v.Tick(w, t);
			if (v.Hits() != last) { last = v.Hits(); if (nh < 4) hitAt[nh] = i; nh++; }
			if (v.TakeTrigger()) trig++;
		}
		CHECK(nh == 3 && trig == 3);
		CHECK(hitAt[1] == 8000 && hitAt[2] == 16000);
		CHECK(v.Note() == 1 && v.Octave() == 2);
	}
	// 1.5x: two hits, the second two thirds through
	{
		Voice v(7); Weights w = Only(0, 2, 4, 1);
		int nh = 1, at1 = -1; uint8_t last;
		v.BeginStep(w, t, L); last = v.Hits();
		for (int i = 1; i < L; i++) { v.Tick(w, t); if (v.Hits() != last) { last = v.Hits(); if (nh == 1) at1 = i; nh++; } }
		CHECK(nh == 2 && at1 == 16000);
	}
	// 2x new: a new note, octave and gate drawn at every hit
	{
		Voice v(99); Weights w{};
		for (int i = 0; i < 8; i++) { w.note[i] = 100; w.octave[i] = 100; w.gate[i] = 100; }
		w.ratchet[6] = 100;                                     // 2x, new
		int differing = 0, hits = 0;
		for (int s = 0; s < 200; s++)
		{
			v.BeginStep(w, t, L);
			int n0 = v.Note(), o0 = v.Octave();
			for (int i = 1; i < L; i++)
			{
				uint8_t h = v.Hits();
				v.Tick(w, t);
				if (v.Hits() != h) { hits++; if (v.Note() != n0 || v.Octave() != o0) differing++; }
			}
		}
		CHECK(hits == 200);                      // one more hit in each step
		CHECK(differing > 100);                  // and usually a different note or octave
	}
	// 2x held: the second hit repeats the first
	{
		Voice v(99); Weights w{};
		for (int i = 0; i < 8; i++) { w.note[i] = 100; w.octave[i] = 100; w.gate[i] = 100; }
		w.ratchet[2] = 100;                                     // 2x, held
		int differing = 0;
		for (int s = 0; s < 200; s++)
		{
			v.BeginStep(w, t, L);
			int n0 = v.Note(), o0 = v.Octave(), g0 = v.GateIndex();
			for (int i = 1; i < L; i++) { v.Tick(w, t); if (v.Note() != n0 || v.Octave() != o0 || v.GateIndex() != g0) differing++; }
		}
		CHECK(differing == 0);
	}
	// Gates always fall before the next hit, so the envelope has an edge:
	// a 6/7 gate at 3x still drops for at least a millisecond between hits
	{
		Voice v(7); Weights w = Only(0, 2, 6, 3);
		v.BeginStep(w, t, L);
		int lowRun = 0, minLow = 1 << 30, highRun = 0;
		for (int i = 1; i < L * 3; i++)
		{
			if (i % L == 0) v.BeginStep(w, t, L); else v.Tick(w, t);
			if (!v.Gate()) lowRun++;
			else { if (lowRun > 0 && lowRun < minLow) minLow = lowRun; lowRun = 0; highRun++; }
		}
		CHECK(minLow >= 48);
	}
	// Tied: the gate stays up into the next hit, which doesn't retrigger
	{
		Voice v(7); Weights wTied = Only(1, 2, 7, 0), wShort = Only(3, 2, 2, 0);
		v.BeginStep(wTied, t, L);
		CHECK(v.Gate() && v.Tied() && v.TakeTrigger());       // the first note triggers
		for (int i = 1; i < L; i++) { v.Tick(wTied, t); CHECK(v.Gate()); }   // up all the way
		v.BeginStep(wShort, t, L);                            // tied into this one
		CHECK(v.Gate() && !v.TakeTrigger());                  // no new trigger: legato
		CHECK(v.PitchMv() == PitchMv(t, 3, 2));               // but the pitch moves
		int on = 1;
		for (int i = 1; i < L; i++) { v.Tick(wShort, t); if (v.Gate()) on++; CHECK(!v.TakeTrigger()); }
		CHECK_NEAR(on, L * 2 / 7, 2);                         // then it ends as usual
		v.BeginStep(wShort, t, L);                            // and the next is a fresh trigger
		CHECK(v.TakeTrigger());
	}
	// Tied into a rest: the gate falls at the rest
	{
		Voice v(7); Weights wTied = Only(1, 2, 7, 0), wRest = Only(3, 2, 0, 0);
		v.BeginStep(wTied, t, L); v.TakeTrigger();
		for (int i = 1; i < L; i++) v.Tick(wTied, t);
		v.BeginStep(wRest, t, L);
		CHECK(!v.Gate() && !v.TakeTrigger());
	}
	// A tie never carries on past a plain gate: tied, short, then fresh
	{
		Voice v(7); Weights wTied = Only(1, 2, 7, 0), wShort = Only(3, 2, 2, 0);
		v.BeginStep(wTied, t, L); v.TakeTrigger();
		for (int i = 1; i < L; i++) v.Tick(wTied, t);
		v.BeginStep(wShort, t, L);                           // legato
		for (int i = 1; i < L; i++) v.Tick(wShort, t);
		v.BeginStep(wShort, t, L);
		CHECK(v.Gate() && v.TakeTrigger());
	}
	// Very short steps: still a gate of at least 2 ms or half the hit, and a fall
	{
		Voice v(7); Weights w = Only(0, 2, 1, 0);
		v.BeginStep(w, t, 200);
		CHECK(v.Gate());
		int on = 1;
		for (int i = 1; i < 200; i++) { v.Tick(w, t); if (v.Gate()) on++; }
		CHECK(on >= 96 - 1 && on <= 152);
	}
	// Independent lines from the same weights
	{
		Voice a(1), b(2); Weights w{};
		for (int i = 0; i < 8; i++) { w.note[i] = 100; w.octave[i] = 100; w.gate[i] = 100; w.ratchet[i] = (i == 0) ? 100 : 0; }
		int same = 0;
		for (int s = 0; s < 400; s++)
		{
			a.BeginStep(w, t, L); b.BeginStep(w, t, L);
			if (a.Note() == b.Note() && a.Octave() == b.Octave()) same++;
		}
		CHECK(same < 400 / 8 * 2);   // chance is 1 in 64: well short of this
	}
	// Over many steps the notes follow their probabilities, and scales of
	// fewer than eight notes never play the ignored sliders
	{
		BuildScaleTable(9, t);                       // major pentatonic: 5 notes
		Voice v(3); int32_t params[8][8] = {};
		for (int i = 0; i < 8; i++) { params[0][i] = 100 * (i + 1); params[1][i] = 100; params[2][i] = 100; params[3][i] = (i == 0) ? 100 : 0; }
		Weights w; BuildWeights(params, 0, t.count, w);
		int count[8] = {}; const int N = 100000;
		for (int s = 0; s < N; s++) { v.BeginStep(w, t, 100); count[v.Note()]++; }
		CHECK(count[5] == 0 && count[6] == 0 && count[7] == 0);
		double total = 100 + 200 + 300 + 400 + 500;
		for (int i = 0; i < 5; i++) CHECK_NEAR(count[i] / double(N), 100.0 * (i + 1) / total, 0.01);
	}
}

// ---------------------------------------------------------------------------
static void TestEnvelope()
{
	// Attack then decay: the times are as set
	{
		Envelope e; e.Set(480, 4800, 0);                  // 10 ms, 100 ms, linear
		CHECK(e.Process() == 0 && !e.Active());
		e.Trigger();
		int peak = -1, n = 0, prev = 0; bool mono = true;
		for (int i = 0; i < 20000; i++)
		{
			int32_t v = e.Process();
			if (peak < 0 && v >= Envelope::kFull) peak = i;
			if (peak >= 0 && i > peak && v > prev) mono = false;
			prev = v;
			if (peak >= 0 && v == 0 && n == 0) n = i;
		}
		CHECK_NEAR(peak, 480, 3);
		CHECK_NEAR(n - peak, 4800, 5);
		CHECK(mono);
		CHECK(!e.Active());
	}
	// Linear at pluck 0: halfway through the decay it is half
	{
		Envelope e; e.Set(24, 4800, 0); e.Trigger();
		int32_t v = 0;
		for (int i = 0; i < 24 + 2400; i++) v = e.Process();
		CHECK_NEAR(v / double(Envelope::kFull), 0.5, 0.01);
	}
	// Full pluck: exponential, much lower halfway, and it still ends
	{
		Envelope e; e.Set(24, 4800, 4096); e.Trigger();
		int32_t v = 0, end = 0;
		for (int i = 0; i < 24 + 2400; i++) v = e.Process();
		CHECK_NEAR(v / double(Envelope::kFull), exp(-3.0), 0.01);   // e^-3 = 5%
		for (int i = 0; i < 8000; i++) { e.Process(); if (!e.Active() && !end) end = i; }
		CHECK(end > 0);
	}
	// Pluck between: between the two at the midpoint
	{
		double mid[3];
		int pl[3] = {0, 2048, 4096};
		for (int k = 0; k < 3; k++)
		{
			Envelope e; e.Set(24, 4800, pl[k]); e.Trigger();
			int32_t v = 0;
			for (int i = 0; i < 24 + 2400; i++) v = e.Process();
			mid[k] = v / double(Envelope::kFull);
		}
		CHECK(mid[0] > mid[1] && mid[1] > mid[2]);
	}
	// Retriggered mid-decay: it rises from where it was, never jumping
	{
		Envelope e; e.Set(480, 9600, 2048); e.Trigger();
		int32_t v = 0, prev = 0, maxJump = 0;
		for (int i = 0; i < 6000; i++) v = e.Process();
		CHECK(v > 0 && v < Envelope::kFull);
		e.Trigger();
		prev = v;
		for (int i = 0; i < 3000; i++)
		{
			v = e.Process();
			int32_t d = v - prev; if (d < 0) d = -d;
			if (d > maxJump) maxJump = d;
			prev = v;
		}
		CHECK(maxJump <= Envelope::kFull / 400);   // no step bigger than a fraction of a percent
	}
	// A ratchet train: ten hits a few ms apart never drop to zero between
	{
		Envelope e; e.Set(48, 4800, 2048);
		int32_t minAfterFirst = Envelope::kFull;
		for (int hit = 0; hit < 10; hit++)
		{
			e.Trigger();
			for (int i = 0; i < 1600; i++) { int32_t v = e.Process(); if (hit > 0 && v < minAfterFirst) minAfterFirst = v; }
		}
		CHECK(minAfterFirst > 0);
	}
}

// ---------------------------------------------------------------------------
static void TestKnobs()
{
	// Attack 0.5 ms to about 1 s, decay 5 ms to about 5 s
	CHECK(AttackSamples(0) == 24);
	CHECK_NEAR(AttackSamples(4095), 49152, 200);   // 0.5 ms x 2^11, just over a second
	CHECK_NEAR(DecaySamples(0), 240, 2);
	CHECK_NEAR(DecaySamples(4095), 245760, 800);   // 5 ms x 2^10, just over five seconds
	for (int s = 1; s < 4096; s++) CHECK(AttackSamples(s) >= AttackSamples(s - 1) && DecaySamples(s) >= DecaySamples(s - 1));
	// Slew: off in the dead zone, then slower and slower
	CHECK(SlewAlpha(0) == (1 << 24) && SlewAlpha(kSlewDeadZone - 1) == (1 << 24));
	CHECK(SlewAlpha(kSlewDeadZone + 1) < (1 << 24));
	for (int s = kSlewDeadZone + 1; s < 4096; s++) CHECK(SlewAlpha(s) <= SlewAlpha(s - 1));
	CHECK(SlewAlpha(4095) >= 1);
	// The one-pole's time constant at the top is about 2 s
	double a = SlewAlpha(4095) / 16777216.0;
	CHECK_NEAR(-1.0 / log(1.0 - a) / 48000.0, 2.05, 0.1);
}

// ---------------------------------------------------------------------------
// The whole engine over a few minutes of a clock, a check on what comes out
static void TestEndToEnd()
{
	StepClock clock; Voice v[2] = {Voice(11), Voice(22)};
	Envelope env[2]; ScaleTable t; BuildScaleTable(10, t);
	int32_t params[8][8] = {};
	for (int i = 0; i < 8; i++) { params[0][i] = 4000; params[1][i] = i == 3 ? 4000 : 0; params[2][i] = 1000 + 400 * i; params[3][i] = (i == 3 || i == 7) ? 4000 : 500; }
	params[5][4] = 4000;                              // voice 2 has its own octave: slider 5 only
	Weights w[2];
	for (int k = 0; k < 2; k++) BuildWeights(params, k, t.count, w[k]);
	for (int k = 0; k < 2; k++) env[k].Set(48, 9600, 2048);

	int octaves[2][8] = {}; int rests[2] = {}, steps = 0; int trig[2] = {};
	int32_t pitchMin = 1 << 30, pitchMax = -(1 << 30);
	const int P = 12000;
	for (int i = 0; i < P * 4000; i++)
	{
		bool edge = i % P == 0;
		if (clock.Tick(edge, 1, 1))
		{
			steps++;
			int32_t len = clock.StepLength(1, 1);
			for (int k = 0; k < 2; k++)
			{
				v[k].BeginStep(w[k], t, len);
				octaves[k][v[k].Octave()]++;
				if (v[k].GateIndex() == 0) rests[k]++;
			}
		}
		else for (int k = 0; k < 2; k++) v[k].Tick(w[k], t);
		for (int k = 0; k < 2; k++)
		{
			if (v[k].TakeTrigger()) { env[k].Trigger(); trig[k]++; }
			env[k].Process();
			int32_t p = v[k].PitchMv();
			if (p < pitchMin) pitchMin = p;
			if (p > pitchMax) pitchMax = p;
		}
	}
	CHECK(steps > 3900);
	// Voice 1 only plays octave slider 3, voice 2 only slider 4
	CHECK(octaves[0][3] == steps);
	CHECK(octaves[1][4] == steps);
	// Pitches stay inside the Computer's output range
	CHECK(pitchMin >= -2000 && pitchMax <= 6000);
	// Rests happen in about the proportion of their weight: 1000 of 1000+1400+...
	double total = 0; for (int i = 0; i < 8; i++) total += 1000 + 400 * i;
	CHECK_NEAR(rests[0] / double(steps), 1000.0 / total, 0.03);
	// Each hit but a rest triggers the envelope, except the ones tied into
	CHECK(trig[0] > steps / 2 && trig[1] > steps / 2);
}

int main()
{
	TestScales();
	TestPick();
	TestWeights();
	TestRatchetTimes();
	TestStepClock();
	TestVoice();
	TestEnvelope();
	TestKnobs();
	TestEndToEnd();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
