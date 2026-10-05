// The demo loop the card starts with: one bar at 120bpm, two seconds, of a
// kick, snare and hats with a plucked melody over them.  It's made at
// power-up rather than stored, and web/index.html makes the same loop the
// same way.  Drums have sharp attacks and the plucks have clear pitch, which
// between them show off what each playback engine does well and badly.
//
// The plucks are Karplus-Strong synthesis: a burst of noise circulating in a
// delay line one period long, softened a little on every pass.

#ifndef SAMPLELAB_DEMO_H
#define SAMPLELAB_DEMO_H

#include <stdint.h>
#include <math.h>

namespace demo
{

static constexpr int kRate = 48000;
static constexpr int kLength = 96000;

struct Rng
{
	uint32_t s = 0x12345678;
	float Next() // -1 to 1
	{
		s ^= s << 13; s ^= s >> 17; s ^= s << 5;
		return float(int32_t(s)) * (1.0f / 2147483648.0f);
	}
};

// Mix v (-1 to 1) into the buffer, clipping
inline void Add(int16_t *buf, int i, float v)
{
	int32_t x = buf[i] + int32_t(v * 32767.0f);
	buf[i] = int16_t(x > 32767 ? 32767 : (x < -32767 ? -32767 : x));
}

inline void Build(int16_t *buf)
{
	const float sr = float(kRate), tau = 6.2831853f;
	for (int i = 0; i < kLength; i++) buf[i] = 0;
	Rng rng;

	// Kicks on beats 1 and 3: a sine sweeping down from 150Hz to 50Hz
	for (int k = 0; k < 2; k++)
	{
		int at = k * 48000;
		float ph = 0;
		for (int i = 0; i < 19200; i++)
		{
			float t = float(i) / sr;
			ph += tau * (50.0f + 100.0f * expf(-t / 0.04f)) / sr;
			Add(buf, at + i, 0.55f * sinf(ph) * expf(-t / 0.12f));
		}
	}
	// Snares on beats 2 and 4: noise and a 180Hz tone
	for (int k = 0; k < 2; k++)
	{
		int at = 24000 + k * 48000;
		for (int i = 0; i < 12000; i++)
		{
			float t = float(i) / sr;
			Add(buf, at + i, 0.3f * rng.Next() * expf(-t / 0.06f)
				+ 0.25f * sinf(tau * 180.0f * t) * expf(-t / 0.04f));
		}
	}
	// Hats on the off-beats: brightened noise
	for (int k = 0; k < 4; k++)
	{
		int at = 12000 + k * 24000;
		float prev = 0;
		for (int i = 0; i < 3840; i++)
		{
			float t = float(i) / sr, x = rng.Next();
			Add(buf, at + i, 0.06f * (x - prev) * expf(-t / 0.02f));
			prev = x;
		}
	}
	// Plucked melody, one note per eighth, each ringing for half a second
	static const int notes[8] = {57, 60, 64, 67, 69, 67, 64, 60};
	float line[256];
	for (int k = 0; k < 8; k++)
	{
		float f = 440.0f * powf(2.0f, float(notes[k] - 69) / 12.0f);
		int d = int(sr / f + 0.5f);
		for (int i = 0; i < d; i++) line[i] = 0.38f * rng.Next();
		int at = k * 12000, j = 0;
		for (int i = 0; i < 24000 && at + i < kLength; i++)
		{
			float out = line[j];
			int j1 = j + 1 < d ? j + 1 : 0;
			line[j] = 0.498f * (line[j] + line[j1]);
			j = j1;
			Add(buf, at + i, out);
		}
	}
}

} // namespace demo

#endif
