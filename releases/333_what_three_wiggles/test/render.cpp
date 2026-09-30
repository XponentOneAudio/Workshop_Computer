// Render what a set of words sounds like, using the card's own engine code.
//
//   g++ -std=c++17 -O2 test/render.cpp -o build/render
//   build/render index.home.raft 20 [depth 0-4095] [ring]
//
// Writes <words>_audio.wav (stereo: Audio 1, Audio 2) and <words>_cv.wav
// (4 channels: CV 1, CV 2, Pulse 1, Pulse 2, full scale = 6 V), which opens
// in Audacity or any audio editor to see the shapes.
//
// Knobs are fixed at noon apart from FM depth. Audio 2 is played as if CV 1
// were patched into CV In 1, sampled on each clock tick (pentatonic scale).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../seed.h"
#include "../dsp.h"
#include "../patch.h"
#include "../engines.h"

using namespace w3w;

static void WriteWav(const std::string &name, int channels, const std::vector<int16_t> &data)
{
	FILE *f = std::fopen(name.c_str(), "wb");
	if (!f) { std::perror(name.c_str()); std::exit(1); }
	auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
	auto u16 = [f](uint16_t v) { std::fwrite(&v, 2, 1, f); };
	uint32_t bytes = uint32_t(data.size() * 2);
	std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
	u32(16); u16(1); u16(uint16_t(channels)); u32(48000); u32(48000u * 2u * uint32_t(channels));
	u16(uint16_t(2 * channels)); u16(16);
	std::fwrite("data", 1, 4, f); u32(bytes);
	std::fwrite(data.data(), 2, data.size(), f);
	std::fclose(f);
	std::printf("wrote %s\n", name.c_str());
}

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: %s word.word.word seconds [depth 0-4095] [ring]\n", argv[0]);
		return 1;
	}
	gTables.Init();

	uint32_t seeds[3];
	if (!SeedsFromWords(reinterpret_cast<const uint8_t *>(argv[1]), std::strlen(argv[1]), seeds))
	{
		std::fprintf(stderr, "need exactly three words separated by dots\n");
		return 1;
	}
	int samples = int(48000 * std::atof(argv[2]));
	int32_t depth = argc > 3 ? Clamp(std::atoi(argv[3]), 0, 4095) : 2048;
	bool ring = argc > 4 && std::strcmp(argv[4], "ring") == 0;

	Patch p;
	BuildPatch(seeds, p);

	const Scale &s = kScales[5];
	int32_t maxDeg = WalkMaxDegree(24, s);
	uint32_t clockInc = IncFromMillivolts(KnobMap(2048, -10030, -3700));
	uint32_t lfoInc = IncFromMillivolts(KnobMap(2048, -13700, -3700));

	Clock clock;
	Triggers triggers;
	Walk walk;
	ComplexLfo lfo;
	Additive additive;
	FmLoop fm;
	Quantiser quantiser;
	clock.Reset();
	triggers.Reset(p);
	walk.Reset(p, maxDeg, s);
	lfo.Reset(p);
	fm.Reset(p);

	std::vector<int16_t> audio, cv;
	uint32_t pitchInc = IncFromMillivolts(0);
	int32_t walkMv = 0;
	for (int i = 0; i < samples; i++)
	{
		if (clock.Process(false, false, clockInc))
		{
			triggers.Tick(TriggerDensity(p.triggerDensity, 2048), clock.period);
			walk.Tick(p, maxDeg, s, clock.period);
			pitchInc = IncFromMillivolts(SemitonesToMillivolts(quantiser.Process(walkMv, s)));
		}
		// Same output scaling as main.cpp: audio about +/-5 V of +/-6 V
		audio.push_back(int16_t((fm.Process(p, depth, ring) * 1800) >> 11));
		audio.push_back(int16_t((additive.Process(p, pitchInc) * 1800) >> 11));
		walkMv = walk.Process();
		cv.push_back(int16_t(walkMv * 32767 / 6000));
		cv.push_back(int16_t((lfo.Process(p, lfoInc) * 5) / 6));
		cv.push_back(triggers.Process() ? 27306 : 0);
		cv.push_back(triggers.flipFlop ? 27306 : 0);
	}

	std::printf("///%s  seeds %08x %08x %08x\n", argv[1], seeds[0], seeds[1], seeds[2]);
	std::printf("drone operators (Hz): %.2f %.2f %.2f\n",
		p.fmInc[0] * 48000.0 / 4294967296.0, p.fmInc[1] * 48000.0 / 4294967296.0, p.fmInc[2] * 48000.0 / 4294967296.0);
	std::printf("partials:");
	for (int k = 0; k < kNumPartials; k++) std::printf(" %d", p.partialLevel[k]);
	std::printf("\nLFO ratios: %.2f %.2f %.2f, fold %.2fx; walk leap %d%%, glide %d%%, hold up to %d; trigger density %d%%\n",
		p.lfoRatio[0] / 256.0, p.lfoRatio[1] / 256.0, p.lfoRatio[2] / 256.0, p.lfoFold / 256.0,
		p.walkLeap * 100 / 65536, p.walkGlide * 100 / 65536, p.walkMaxHold, p.triggerDensity * 100 / 65536);

	std::string base = argv[1];
	WriteWav(base + "_audio.wav", 2, audio);
	WriteWav(base + "_cv.wav", 4, cv);
	return 0;
}
