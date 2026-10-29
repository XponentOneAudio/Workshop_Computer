// render.cpp - renders demo WAVs from the real card code (via the mock
// hardware layer). Audio Out 1 (VCA) -> left, Audio Out 2 (VCF) -> right.
//
// Build & run: ./build.sh

#include "computercard_mock.h"
#include "../src/wavedigitoaster.cpp"

#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <chrono>
#include <functional>

static const int SR = 48000;

static void WriteWav(const std::string &path, const std::vector<int16_t> &lr)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	uint32_t data = (uint32_t)lr.size() * 2, riff = 36 + data, fmtLen = 16, sr = SR, br = SR * 4;
	uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmtLen, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
	fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
	fwrite(lr.data(), 2, lr.size(), f);
	fclose(f);
}

struct Scene
{
	const char *name;
	double seconds;
	std::function<void(WaveDigiToaster &)> setup;
	std::function<void(WaveDigiToaster &, double t)> perSample;
};

int main(int argc, char **argv)
{
	std::string dir = argc > 1 ? argv[1] : ".";
	std::vector<Scene> scenes = {
		{"01_repeat_drone", 8.0,
		 [](WaveDigiToaster &c) { c.knob[0] = 2600; c.knob[1] = 2200; c.knob[2] = 2600; },
		 nullptr},
		{"02_filter_sweep_ramp", 8.0,
		 [](WaveDigiToaster &c) { c.knob[1] = 1700; c.knob[2] = 3300; c.SimSetPage(2, 0, 2048); c.SimSetPage(1, 2, 3000); },
		 [](WaveDigiToaster &c, double t) { c.knob[0] = (int32_t)(4095 * (0.5 - 0.5 * cos(t * 2 * M_PI / 8.0))); }},
		{"03_gated_notes_pitch_cv", 8.0,
		 [](WaveDigiToaster &c) {
			 c.connected[ComputerCard::Pulse1] = true; c.connected[ComputerCard::CV1] = true;
			 c.knob[0] = 2300; c.knob[1] = 1500; c.knob[2] = 3000;
			 c.SimSetPage(1, 1, 300); c.SimSetPage(1, 2, 1600); c.SimSetPage(2, 0, 3300); },
		 [](WaveDigiToaster &c, double t) {
			 static const int notes[8] = {0, 7, 12, 3, 0, 10, 5, 15};
			 int step = (int)(t * 4.0) % 8;
			 c.pulseIn[0] = fmod(t * 4.0, 1.0) < 0.35;
			 c.cvIn[0] = (int16_t)(notes[step] / 12.0 * 341); }},
		{"04_noise_wind", 8.0,
		 [](WaveDigiToaster &c) {
			 c.knob[0] = 2400; c.knob[2] = 3900;
			 c.SimSetPage(2, 2, 4095); c.SimSetPage(2, 0, 600); c.SimSetPage(1, 0, 1200); },
		 nullptr},
		{"05_lfo_trill_square", 8.0,
		 [](WaveDigiToaster &c) {
			 c.knob[0] = 3000; c.knob[1] = 2500; c.knob[2] = 2000;
			 c.SimSetPage(2, 1, 600); c.SimSetPage(2, 2, 1000); c.SimSetPage(1, 0, 3000);
			 c.SimSetPage(2, 0, 2048); },
		 nullptr},
		{"06_sync_sweep", 8.0,
		 [](WaveDigiToaster &c) {
			 c.connected[ComputerCard::Pulse2] = true;
			 c.knob[0] = 3200; c.knob[2] = 2000; c.SimSetPage(2, 0, 2048); },
		 [](WaveDigiToaster &c, double t) {
			 c.pulseIn[1] = fmod(t * 110.0, 1.0) < 0.5;
			 c.knob[1] = (int32_t)(2000 + 1500 * (0.5 - 0.5 * cos(t * 2 * M_PI / 8.0))); }},
		{"07_areg_pitch_zaps", 8.0,
		 [](WaveDigiToaster &c) {
			 c.knob[0] = 3000; c.knob[1] = 1800; c.knob[2] = 3400;
			 c.SimSetPage(2, 1, 3600); c.SimSetPage(1, 1, 0); c.SimSetPage(1, 2, 1400); },
		 nullptr},
	};

	double worstLoad = 0;
	for (auto &s : scenes)
	{
		WaveDigiToaster card;
		if (s.setup) s.setup(card);
		int n = (int)(s.seconds * SR);
		std::vector<int16_t> lr;
		lr.reserve(n * 2);
		int clip = 0, peak = 0;
		auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < n; i++)
		{
			if (s.perSample) s.perSample(card, (double)i / SR);
			card.Step();
			for (int ch = 0; ch < 2; ch++)
			{
				int v = card.audioOut[ch];
				if (v >= 2047 || v <= -2048) clip++;
				if (std::abs(v) > peak) peak = std::abs(v);
				lr.push_back((int16_t)(v * 16));
			}
		}
		double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		worstLoad = std::max(worstLoad, sec / s.seconds);
		WriteWav(dir + "/" + s.name + ".wav", lr);
		printf("%-26s peak %4d/2047  clipped samples %6d\n", s.name, peak, clip);
	}
	printf("host speed: %.1fx realtime (worst scene)\n", 1.0 / worstLoad);
	return 0;
}
