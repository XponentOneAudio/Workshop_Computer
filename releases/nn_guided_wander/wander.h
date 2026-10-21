// Guided Wander - the engine, free of any hardware so it can be tested on a
// computer (see test/engine_test.cpp).  main.cpp wires it to the Computer.
//
//   scales      the quantiser's scales, up to 8 notes each, in cents
//   PickWeighted  a weighted random choice: the sliders are probabilities
//   StepClock   steps from Pulse In 1 (divided or multiplied) or, with no
//               clock, from an internal 120 BPM beat
//   Voice       one random line: a note, octave, gate length and ratchet drawn
//               at every step, and the hits (ratchets) within the step
//   Envelope    the attack/decay "pluck" envelope, retriggered by every hit

#ifndef WANDER_H
#define WANDER_H

#include <stdint.h>
#include <math.h>

namespace wander
{

static constexpr int kSliders = 8;
static constexpr int kSampleRate = 48000;

//----------------------------------------------------------------------------
// Scales
//----------------------------------------------------------------------------

// The lowest octave the OCTAVES sliders reach: slider 1 is -2V, slider 8 +5V,
// so the highest note of any scale stays inside the Computer's +-6V.
static constexpr int kBaseOctave = -2;
static constexpr int kNumScales = 32;

struct ScaleDef
{
	const char *name;
	int count;           // notes in the scale, 1-8
	float cents[8];      // each above the root, within one octave
};

// Twelve-tone scales first, then the microtonal ones.  A scale never has more
// than eight notes: one slider each.
static constexpr ScaleDef kScales[kNumScales] = {
	{"Major",              7, {0, 200, 400, 500, 700, 900, 1100}},
	{"Natural minor",      7, {0, 200, 300, 500, 700, 800, 1000}},
	{"Harmonic minor",     7, {0, 200, 300, 500, 700, 800, 1100}},
	{"Melodic minor",      7, {0, 200, 300, 500, 700, 900, 1100}},
	{"Dorian",             7, {0, 200, 300, 500, 700, 900, 1000}},
	{"Phrygian",           7, {0, 100, 300, 500, 700, 800, 1000}},
	{"Lydian",             7, {0, 200, 400, 600, 700, 900, 1100}},
	{"Mixolydian",         7, {0, 200, 400, 500, 700, 900, 1000}},
	{"Locrian",            7, {0, 100, 300, 500, 600, 800, 1000}},
	{"Major pentatonic",   5, {0, 200, 400, 700, 900}},
	{"Minor pentatonic",   5, {0, 300, 500, 700, 1000}},
	{"Blues",              6, {0, 300, 500, 600, 700, 1000}},
	{"Whole tone",         6, {0, 200, 400, 600, 800, 1000}},
	{"Octatonic",          8, {0, 200, 300, 500, 600, 800, 900, 1100}},
	{"Augmented",          6, {0, 300, 400, 700, 800, 1100}},
	{"Hirajoshi",          5, {0, 200, 300, 700, 800}},
	{"In-sen",             5, {0, 100, 500, 700, 1000}},
	{"Hungarian minor",    7, {0, 200, 300, 600, 700, 800, 1100}},
	{"Phrygian dominant",  7, {0, 100, 400, 500, 700, 800, 1000}},
	{"Double harmonic",    7, {0, 100, 400, 500, 700, 800, 1100}},
	{"Root and fifth",     2, {0, 700}},
	{"5-TET",              5, {0, 240, 480, 720, 960}},
	{"7-TET",              7, {0, 171.43f, 342.86f, 514.29f, 685.71f, 857.14f, 1028.57f}},
	// 13 equal steps to the octave: the eight-note 5L 3s scale, steps
	// 0 2 4 5 7 9 10 12
	{"13-TET (8 of 13)",   8, {0, 184.62f, 369.23f, 461.54f, 646.15f, 830.77f, 923.08f, 1107.69f}},
	// 19-TET diatonic, steps 0 3 6 8 11 14 17
	{"19-TET diatonic",    7, {0, 189.47f, 378.95f, 505.26f, 694.74f, 884.21f, 1073.68f}},
	// 24-TET (quarter tones) maqams: Rast 0 4 7 10 14 18 21, Bayati 0 3 6 10 14 16 20
	{"Maqam Rast (24-TET)",   7, {0, 200, 350, 500, 700, 900, 1050}},
	{"Maqam Bayati (24-TET)", 7, {0, 150, 300, 500, 700, 800, 1000}},
	// 31-TET diatonic, steps 0 5 10 13 18 23 28
	{"31-TET diatonic",    7, {0, 193.55f, 387.10f, 503.23f, 696.77f, 890.32f, 1083.87f}},
	{"Just major",         7, {0, 203.91f, 386.31f, 498.04f, 701.96f, 884.36f, 1088.27f}},
	// harmonics 8 to 15
	{"Overtones 8-15",     8, {0, 203.91f, 386.31f, 551.32f, 701.96f, 840.53f, 968.83f, 1088.27f}},
	// typical values: tunings vary from one gamelan to the next
	{"Slendro (typical)",  5, {0, 231, 474, 717, 955}},
	{"Pelog (typical)",    7, {0, 120, 258, 539, 675, 785, 943}},
};

// A scale as millivolts above its root (1V/octave), made when the scale changes
struct ScaleTable
{
	int count = 7;
	int32_t mv[kSliders] = {0, 167, 333, 417, 583, 750, 917, 0};
};

inline void BuildScaleTable(int index, ScaleTable &t)
{
	if (index < 0) index = 0;
	if (index >= kNumScales) index = kNumScales - 1;
	const ScaleDef &s = kScales[index];
	t.count = s.count;
	for (int i = 0; i < kSliders; i++)
	{
		t.mv[i] = i < s.count ? int32_t(lroundf(s.cents[i] * (1000.0f / 1200.0f))) : 0;
	}
}

// The pitch of a note (a degree of the scale) in an octave slider's octave
inline int32_t PitchMv(const ScaleTable &t, int note, int octave)
{
	if (note < 0) note = 0;
	if (note >= t.count) note = t.count - 1;
	if (octave < 0) octave = 0;
	if (octave > 7) octave = 7;
	return (octave + kBaseOctave) * 1000 + t.mv[note];
}

//----------------------------------------------------------------------------
// Random choice
//----------------------------------------------------------------------------

inline uint32_t NextRandom(uint32_t &s)
{
	s ^= s << 13;
	s ^= s >> 17;
	s ^= s << 5;
	return s;
}

// A choice among the first n of the weights, each as likely as its share of
// the total.  All zero means no preference: every one equally.
inline int PickWeighted(const int32_t *w, int n, uint32_t &rng)
{
	int64_t total = 0;
	for (int i = 0; i < n; i++) total += w[i] > 0 ? w[i] : 0;
	uint32_t r = NextRandom(rng) >> 4;
	if (total <= 0) return int(r % uint32_t(n));
	int64_t x = int64_t(r) % total;
	for (int i = 0; i < n; i++)
	{
		int64_t wi = w[i] > 0 ? w[i] : 0;
		if (x < wi) return i;
		x -= wi;
	}
	return n - 1;
}

inline bool AnyNonZero(const int32_t *w, int n)
{
	for (int i = 0; i < n; i++) if (w[i] > 0) return true;
	return false;
}

// The probabilities one voice draws from: four pages of eight.  The first
// page of each pair serves voice 1; voice 2 uses the second page of a pair if
// any of its sliders is above zero, otherwise the first as well.
struct Weights
{
	int32_t note[kSliders];
	int32_t octave[kSliders];
	int32_t gate[kSliders];
	int32_t ratchet[kSliders];
};

// params: eight pages of eight slider values (0-4064).  Pages 0-3 are voice 1's
// notes, octaves, gates, ratchets; 4-7 are voice 2's.
template <typename P>
inline void BuildWeights(const P &params, int voice, int noteCount, Weights &w)
{
	int32_t *dst[4] = {w.note, w.octave, w.gate, w.ratchet};
	for (int pair = 0; pair < 4; pair++)
	{
		int page = pair;
		if (voice == 1)
		{
			bool own = false;
			for (int i = 0; i < kSliders; i++) if (params[4 + pair][i] > 0) own = true;
			if (own) page = 4 + pair;
		}
		for (int i = 0; i < kSliders; i++) dst[pair][i] = params[page][i];
	}
	// Notes past the end of the scale are ignored
	for (int i = noteCount; i < kSliders; i++) w.note[i] = 0;
}

//----------------------------------------------------------------------------
// Gates and ratchets
//----------------------------------------------------------------------------

// Gate length sliders, evenly from 0% to 100%: the first is a rest (no gate,
// no trigger), the last is tied (the gate stays up into the next hit, which
// does not retrigger the envelope).  Slider g is g/7 of the hit.
static constexpr int kGateRest = 0;
static constexpr int kGateTied = 7;

// Ratchet sliders: the first four hold the step's note, octave and gate
// for every hit, the last four draw a new note, octave and gate at every hit.
// Each at 1x, 1.5x, 2x and 3x the step rate, in half-hits: 2, 3, 4, 6.
static constexpr int kRate2[4] = {2, 3, 4, 6};
inline bool RatchetHeld(int r) {return r < 4;}

// Hits in a step: those at k * step / rate that fall before its end
inline int HitCount(int rate2) {return (rate2 + 1) / 2;}

// Where hit k of a step of len samples falls
inline int32_t HitTime(int k, int32_t len, int rate2)
{
	return int32_t((int64_t(k) * len * 2) / rate2);
}

//----------------------------------------------------------------------------
// Steps
//----------------------------------------------------------------------------

// Clock ratios (the X knob, switch up): steps a clock, num / den
struct Ratio {int8_t num, den;};
static constexpr int kNumRatios = 11;
static constexpr Ratio kRatios[kNumRatios] = {
	{1, 8}, {1, 6}, {1, 4}, {1, 3}, {1, 2}, {1, 1},
	{2, 1}, {3, 1}, {4, 1}, {6, 1}, {8, 1}};

class StepClock
{
public:
	static constexpr int32_t kMaxClockPeriod = 60 * kSampleRate; // a minute
	static constexpr int32_t kFreeBeat = kSampleRate / 2;        // 120 BPM

	// Call every sample.  Returns true when a step begins.  A step is one beat
	// of the clock divided or multiplied by num/den, or with no clock one
	// beat of the internal tempo.  The clock is followed from its second pulse
	// and given up after four of its periods (at least 2 s) without one.
	bool Tick(bool clockEdge, int num, int den)
	{
		if (samplesSinceClock < 0x7FFFFFFF) samplesSinceClock++;
		bool was = clocked;
		if (clockEdge)
		{
			if (haveClock && samplesSinceClock <= kMaxClockPeriod)
			{
				clockPeriod = samplesSinceClock < 48 ? 48 : samplesSinceClock;
				periodKnown = true;
			}
			haveClock = true;
			samplesSinceClock = 0;
		}
		int32_t timeout = clockPeriod > 24000 ? clockPeriod * 4 : 2 * kSampleRate;
		if (periodKnown && samplesSinceClock >= timeout) periodKnown = false;
		clocked = periodKnown;

		bool step = false;
		if (clocked)
		{
			// Beats: each clock, and when multiplying num - 1 more, evenly
			// spaced and started afresh by every clock so they never drift
			bool beat = false;
			if (clockEdge)
			{
				subPhase = 0;
				subCount = 0;
				beat = true;
				if (!was) beatCount = den - 1; // the first clock starts a step
			}
			else if (subCount < num - 1)
			{
				subPhase += num;
				if (subPhase >= clockPeriod)
				{
					subPhase -= clockPeriod;
					subCount++;
					beat = true;
				}
			}
			if (beat && ++beatCount >= den)
			{
				beatCount = 0;
				step = true;
			}
		}
		else
		{
			if (was) freePhase = 0;
			// (the division is done only when the ratio changes)
			if (num != cachedNum || den != cachedDen || freeLen < 1)
			{
				cachedNum = num;
				cachedDen = den;
				freeLen = int32_t(int64_t(kFreeBeat) * den / num);
				if (freeLen < 1) freeLen = 1;
			}
			if (++freePhase >= freeLen)
			{
				freePhase = 0;
				step = true;
			}
		}
		return step;
	}

	// A step's length in samples, now
	int32_t StepLength(int num, int den) const
	{
		int64_t beat = clocked ? clockPeriod : kFreeBeat;
		int64_t len = beat * den / num;
		return len < 1 ? 1 : int32_t(len);
	}

	bool Clocked() const {return clocked;}

private:
	bool haveClock = false, clocked = false, periodKnown = false;
	int32_t samplesSinceClock = 0x7FFFFFFF;
	int32_t clockPeriod = kFreeBeat;
	int32_t subPhase = 0, subCount = 0, beatCount = 0;
	int32_t freePhase = 0x40000000; // the first sample begins the first step
	int cachedNum = 0, cachedDen = 0;
	int32_t freeLen = 0;
};

//----------------------------------------------------------------------------
// A voice: one random line
//----------------------------------------------------------------------------

class Voice
{
public:
	explicit Voice(uint32_t seed = 1) : rng(seed ? seed : 1) {}

	void Seed(uint32_t s) {rng = s ? s : 1;}

	// A step begins: draw its ratchet, and its note, octave and gate, and play
	// the first hit
	void BeginStep(const Weights &w, const ScaleTable &t, int32_t stepLen)
	{
		len = stepLen < 1 ? 1 : stepLen;
		sinceStep = 0;
		ratchet = PickWeighted(w.ratchet, kSliders, rng);
		rate2 = kRate2[ratchet & 3];
		hits = HitCount(rate2);
		hitIndex = 0;
		Draw(w, t);
		Fire(t);
	}

	// Every sample, after BeginStep has been called for the step: later hits
	// of the step, and the end of the gate
	void Tick(const Weights &w, const ScaleTable &t)
	{
		if (sinceStep < 0x7FFFFFFF) sinceStep++;
		if (sinceHit < 0x7FFFFFFF) sinceHit++;
		if (gate_ && !tiedOut && --gateLeft <= 0) gate_ = false;
		if (sinceStep >= nextHitAt)
		{
			if (!RatchetHeld(ratchet)) Draw(w, t);
			Fire(t);
		}
	}

	// True once for each hit that should retrigger the envelope
	bool TakeTrigger()
	{
		bool t = trigger;
		trigger = false;
		return t;
	}

	bool Gate() const {return gate_;}
	int32_t PitchMv() const {return pitchMv;}
	int Note() const {return note;}
	int Octave() const {return octave;}
	int GateIndex() const {return gateIdx;}
	int Ratchet() const {return ratchet;}
	bool Tied() const {return tiedOut;}
	uint8_t Hits() const {return hitCounter;}
	int32_t SinceHit() const {return sinceHit;}
	int32_t SinceStep() const {return sinceStep;}
	int32_t StepLen() const {return len;}

private:
	void Draw(const Weights &w, const ScaleTable &t)
	{
		note = PickWeighted(w.note, t.count < kSliders ? t.count : kSliders, rng);
		octave = PickWeighted(w.octave, kSliders, rng);
		gateIdx = PickWeighted(w.gate, kSliders, rng);
	}

	void Fire(const ScaleTable &t)
	{
		int k = hitIndex++;
		int32_t start = HitTime(k, len, rate2);
		int32_t next = hitIndex < hits ? HitTime(hitIndex, len, rate2) : len;
		nextHitAt = hitIndex < hits ? next : 0x7FFFFFFF;
		int32_t interval = next - start;
		if (interval < 1) interval = 1;

		pitchMv = wander::PitchMv(t, note, octave);
		hitCounter = uint8_t((hitCounter + 1) & 127);
		sinceHit = 0;

		bool rest = gateIdx == kGateRest;
		bool tied = gateIdx == kGateTied;
		bool legato = prevTied && !rest;

		if (rest)
		{
			gate_ = false;
			tiedOut = false;
		}
		else
		{
			// A gate has to fall before the next hit, to give the envelope an
			// edge: at least 2 ms long, and a millisecond short of the hit
			int32_t hi = interval - 48;
			if (hi < 1) hi = 1;
			int32_t high = int32_t((int64_t(interval) * gateIdx) / 7);
			if (high < 96) high = 96;
			if (high > hi) high = hi;
			gate_ = true;
			tiedOut = tied;
			gateLeft = high;
			if (!legato) trigger = true;
		}
		prevTied = tied && !rest;
	}

	uint32_t rng;
	int note = 0, octave = 2, gateIdx = 4, ratchet = 0;
	int rate2 = 2, hits = 1, hitIndex = 1;
	int32_t len = kSampleRate / 2;
	int32_t sinceStep = 0, sinceHit = 0x7FFFFFFF;
	int32_t nextHitAt = 0x7FFFFFFF;   // sinceStep of the step's next hit
	int32_t gateLeft = 0;
	int32_t pitchMv = 0;
	bool gate_ = false, tiedOut = false, prevTied = false, trigger = false;
	uint8_t hitCounter = 0;
};

//----------------------------------------------------------------------------
// The envelope: attack, then a decay that is linear (no pluck) through to
// exponential (full pluck: a fast fall to a long tail), retriggered from
// wherever it is, so a ratchet never clicks.
//----------------------------------------------------------------------------

class Envelope
{
public:
	static constexpr int32_t kFull = 1 << 24;

	// attack and decay in samples, pluck 0-4096
	void Set(int32_t attack, int32_t decay, int32_t pluckQ12)
	{
		if (attack < 1) attack = 1;
		if (decay < 1) decay = 1;
		atkInc = kFull / attack;
		if (atkInc < 1) atkInc = 1;
		decInc = kFull / decay;
		if (decInc < 1) decInc = 1;
		// The exponential falls 52 dB (e^-6) over the decay time
		exCoef = int32_t(2147483647.0f * expf(-6.0f / float(decay)));
		pluck = pluckQ12 < 0 ? 0 : (pluckQ12 > 4096 ? 4096 : pluckQ12);
	}

	// The same attack, decay and pluck as another envelope, without
	// working them out again
	void CopySettings(const Envelope &o)
	{
		atkInc = o.atkInc;
		decInc = o.decInc;
		exCoef = o.exCoef;
		pluck = o.pluck;
	}

	void Trigger() {stage = Attack;}

	// One sample: 0 to kFull
	int32_t Process()
	{
		switch (stage)
		{
		case Attack:
			cur += atkInc;
			if (cur >= kFull)
			{
				cur = kFull;
				lin = ex = kFull;
				stage = Decay;
			}
			break;
		case Decay:
			lin -= decInc;
			if (lin <= 0)
			{
				lin = ex = cur = 0;
				stage = Idle;
			}
			else
			{
				ex = int32_t((int64_t(ex) * exCoef) >> 31);
				cur = int32_t((int64_t(lin) * (4096 - pluck) + int64_t(ex) * pluck) >> 12);
			}
			break;
		default:
			break;
		}
		return cur;
	}

	bool Active() const {return stage != Idle;}

private:
	enum Stage {Idle, Attack, Decay};
	Stage stage = Idle;
	int32_t cur = 0, lin = 0, ex = 0;
	int32_t atkInc = kFull / 48, decInc = kFull / 4800, exCoef = 2147000000;
	int32_t pluck = 2048;
};

//----------------------------------------------------------------------------
// Knob settings (0-4095) to times
//----------------------------------------------------------------------------

// Attack: 0.5 ms to about 1 s
inline int32_t AttackSamples(int32_t setting)
{
	float t = 0.0005f * exp2f(11.0f * float(setting) / 4095.0f);
	int32_t n = int32_t(t * float(kSampleRate));
	return n < 24 ? 24 : n;
}

// Decay: 5 ms to about 5 s
inline int32_t DecaySamples(int32_t setting)
{
	float t = 0.005f * exp2f(10.0f * float(setting) / 4095.0f);
	return int32_t(t * float(kSampleRate));
}

// Slew on the pitch: off across the bottom of the knob, then a time constant
// of 1 ms to about 2 s.  Returns the one-pole coefficient in Q24; 1<<24 is off.
static constexpr int32_t kSlewDeadZone = 140;
inline int32_t SlewAlpha(int32_t setting)
{
	int32_t s = setting - kSlewDeadZone;
	if (s < 0) return 1 << 24;
	float tau = 0.001f * exp2f(float(s) * (11.0f / float(4095 - kSlewDeadZone)));
	int32_t a = int32_t(16777216.0f * (1.0f - expf(-1.0f / (tau * float(kSampleRate)))));
	return a < 1 ? 1 : a;
}

} // namespace wander

#endif
