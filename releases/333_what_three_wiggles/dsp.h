// what.three.wiggles: fixed-point DSP helpers.
//
// Plain C++ with no Pico SDK dependencies, so it also compiles on a desktop
// for the host tests. Floats are only used by Init(), never per sample.

#ifndef W3W_DSP_H
#define W3W_DSP_H

#include <cstdint>
#include <cmath>

namespace w3w
{

constexpr float kSampleRate = 48000.0f;
constexpr float kC4Hz = 261.625565f; // 0 V (and 0 mV) is C4 throughout

// Phase increment (2^32 per cycle) for a frequency in Hz. Init-time only.
inline uint32_t IncFromHz(float hz)
{
	float inc = hz * (4294967296.0f / kSampleRate);
	if (inc < 0.0f) inc = 0.0f;
	if (inc > 4294967040.0f) inc = 4294967040.0f;
	return uint32_t(inc);
}

struct Tables
{
	static constexpr int kSineBits = 10;
	static constexpr int kSineSize = 1 << kSineBits;

	int16_t sine[kSineSize + 1]; // Q15 sine, with a guard point for interpolation
	uint32_t octave[1000];       // phase increment for C4 * 2^(i/1000), i in mV

	void Init()
	{
		for (int i = 0; i <= kSineSize; i++)
		{
			sine[i] = int16_t(lrintf(32767.0f * sinf(6.28318531f * float(i) / float(kSineSize))));
		}
		for (int i = 0; i < 1000; i++)
		{
			octave[i] = IncFromHz(kC4Hz * exp2f(float(i) / 1000.0f));
		}
	}
};

inline Tables gTables;

// Interpolated sine of a 32-bit phase, Q15 (-32767..32767).
inline int32_t Sine(uint32_t phase)
{
	uint32_t i = phase >> (32 - Tables::kSineBits);
	int32_t f = int32_t((phase >> (16 - Tables::kSineBits)) & 0xFFFF);
	int32_t a = gTables.sine[i];
	int32_t b = gTables.sine[i + 1];
	return a + (((b - a) * f) >> 16);
}

// Phase increment for C4 * 2^(mv/1000): 1 V/oct with 0 V = C4.
// Range is about 16 octaves below C4 (LFO rates) to 5 octaves above.
inline uint32_t IncFromMillivolts(int32_t mv)
{
	int32_t oct = mv / 1000;
	int32_t frac = mv % 1000;
	if (frac < 0) { frac += 1000; oct -= 1; }
	if (oct < -16) { oct = -16; frac = 0; }
	if (oct > 4) { oct = 4; frac = 999; }
	uint32_t inc = gTables.octave[frac];
	return oct >= 0 ? inc << oct : inc >> (-oct);
}

constexpr int32_t Clamp(int32_t x, int32_t lo, int32_t hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

// Floor division for possibly negative numerators.
constexpr int32_t FloorDiv(int32_t a, int32_t b)
{
	return (a >= 0) ? a / b : -((-a + b - 1) / b);
}

// Semitones (relative to C4) to millivolts at 1 V/oct, rounded.
constexpr int32_t SemitonesToMillivolts(int32_t semis)
{
	return FloorDiv(semis * 1000 + 6, 12);
}

// Linear map of a 0..4095 knob to lo..hi.
constexpr int32_t KnobMap(int32_t knob, int32_t lo, int32_t hi)
{
	return lo + ((hi - lo) * knob) / 4095;
}

////////////////////////////////////////////////////////////////////////////////
// Scales and quantiser

struct Scale
{
	const char *name;
	uint8_t count;
	uint8_t notes[12];
};

inline constexpr Scale kScales[] = {
	{"chromatic",       12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
	{"major",            7, {0, 2, 4, 5, 7, 9, 11}},
	{"minor",            7, {0, 2, 3, 5, 7, 8, 10}},
	{"dorian",           7, {0, 2, 3, 5, 7, 9, 10}},
	{"mixolydian",       7, {0, 2, 4, 5, 7, 9, 10}},
	{"major pentatonic", 5, {0, 2, 4, 7, 9}},
	{"minor pentatonic", 5, {0, 3, 5, 7, 10}},
	{"whole tone",       6, {0, 2, 4, 6, 8, 10}},
};
constexpr int kNumScales = int(sizeof(kScales) / sizeof(kScales[0]));

// Scale degree (any integer, 0 = C4) to semitones relative to C4.
inline int32_t DegreeToSemitone(int32_t degree, const Scale &s)
{
	int32_t oct = FloorDiv(degree, s.count);
	int32_t idx = degree - oct * s.count;
	return oct * 12 + s.notes[idx];
}

inline bool InScale(int32_t semis, const Scale &s)
{
	int32_t pc = semis - FloorDiv(semis, 12) * 12;
	for (int i = 0; i < s.count; i++)
	{
		if (s.notes[i] == pc) return true;
	}
	return false;
}

// Nearest scale note (semitones relative to C4) to a pitch in millisemitones.
inline int32_t NearestInScale(int32_t msemis, const Scale &s)
{
	int32_t oct = FloorDiv(msemis, 12000);
	int32_t within = msemis - oct * 12000;
	int32_t best = 0, bestDist = INT32_MAX;
	// Candidates: this octave's notes, plus the neighbours across octave edges
	for (int i = -1; i <= s.count; i++)
	{
		int32_t note = (i < 0) ? int32_t(s.notes[s.count - 1]) - 12
		             : (i == s.count) ? 12 : int32_t(s.notes[i]);
		int32_t d = within - note * 1000;
		if (d < 0) d = -d;
		if (d < bestDist) { bestDist = d; best = note; }
	}
	return oct * 12 + best;
}

// Quantiser with hysteresis, so a noisy CV sitting near the boundary between
// two notes doesn't chatter between them.
struct Quantiser
{
	static constexpr int32_t kHysteresis = 200; // millisemitones (20 cents)
	int32_t current = 0;
	bool valid = false;

	// mv: 1 V/oct pitch; returns semitones relative to C4.
	int32_t Process(int32_t mv, const Scale &s)
	{
		int32_t msemis = mv * 12;
		int32_t nearest = NearestInScale(msemis, s);
		if (valid && nearest != current && InScale(current, s))
		{
			int32_t dCur = msemis - current * 1000;
			int32_t dNew = msemis - nearest * 1000;
			if (dCur < 0) dCur = -dCur;
			if (dNew < 0) dNew = -dNew;
			if (dCur < dNew + kHysteresis) return current;
		}
		current = nearest;
		valid = true;
		return current;
	}
};

} // namespace w3w

#endif
