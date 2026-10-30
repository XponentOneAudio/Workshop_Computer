// render.cpp — offline audio renderer for the Dual Borg card.
//
// Compiles the REAL src/dual_borg.cpp against a mock hardware layer, drives it
// with generated test signals at 48kHz, and writes stereo WAV files.
//
//   Audio Out 1 (Borg I) -> left channel,  Audio Out 2 (Borg II) -> right.
//
// Build & run:  ./build.sh

#include "computercard_mock.h"   // must come first: fakes the hardware layer
#include "../src/dual_borg.cpp"  // the actual card under test (its main() is skipped)

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>

static constexpr int SR = 48000;
using CC = ComputerCard;

static void writeWav(const std::string &path, const std::vector<int16_t> &samples)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) { printf("  ! could not open %s\n", path.c_str()); return; }
	uint32_t dataBytes = (uint32_t)(samples.size() * 2), riff = 36 + dataBytes;
	uint32_t fmtsz = 16, srate = SR, byteRate = SR * 4;
	uint16_t fmt = 1, ch = 2, blockAlign = 4, bits = 16;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
	fwrite("fmt ", 1, 4, f); fwrite(&fmtsz, 4, 1, f); fwrite(&fmt, 2, 1, f);
	fwrite(&ch, 2, 1, f); fwrite(&srate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
	fwrite(&blockAlign, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
	fwrite(samples.data(), 2, samples.size(), f);
	fclose(f);
}

// Write the card's raw +/-2047 output scaled x16 (no normalisation, so the
// relative level of each scenario is preserved) and report peak / rail use.
static void finalize(const std::string &dir, const char *name, const char *what,
                     const std::vector<int32_t> &raw)
{
	int32_t peak[2] = { 0, 0 };
	long rail = 0;
	std::vector<int16_t> out;
	out.reserve(raw.size());
	for (size_t n = 0; n < raw.size(); n++)
	{
		int32_t a = std::abs(raw[n]);
		if (a > peak[n & 1]) peak[n & 1] = a;
		if (a >= 2047) rail++;
		out.push_back((int16_t)(raw[n] * 16));
	}
	writeWav(dir + "/" + name, out);
	printf("  %-18s %-48s peak I=%3.0f%% II=%3.0f%%  on-rail %.2f%%\n", name, what,
	       100.0 * peak[0] / 2047, 100.0 * peak[1] / 2047, 100.0 * rail / raw.size());
}

static inline bool pulseAt(long n, long period, long width = 48)
{
	return (n % period) < width;
}

struct Saw
{
	double phase = 0;
	int32_t next(double hz, double amp = 1800)
	{
		phase += hz / SR;
		if (phase >= 1) phase -= 1;
		return (int32_t)((phase * 2 - 1) * amp);
	}
};

// Volts -> CV input counts (~341 counts per volt)
static int32_t volts(double v) { return (int32_t)lround(v * 341.33); }

static void run(DualBorg &card, std::vector<int32_t> &raw)
{
	card.simStep();
	raw.push_back(card.simAudioOut[0]);
	raw.push_back(card.simAudioOut[1]);
}

// 1) Same saw into both (Audio In 2 normalled to 1), resonance on, an LFO on
//    both cutoff CVs: hear Borg I's lazy, round sweep vs Borg II's snappy one.
static void renderSweep(const std::string &dir)
{
	DualBorg card;
	card.simSwitch = CC::Up;
	card.simConnected[CC::Audio1] = true;
	card.simKnob[CC::X] = card.simKnob[CC::Y] = 1500;
	card.simKnob[CC::Main] = 3000;
	Saw saw;
	std::vector<int32_t> raw;
	for (long n = 0; n < 8 * SR; n++)
	{
		double lfo = sin(2 * M_PI * 0.5 * n / SR);
		// a square-ish LFO in the second half shows the slew difference clearly
		if (n > 4 * SR) lfo = lfo > 0 ? 1 : -1;
		card.simCVIn[0] = card.simCVIn[1] = volts(2.5 * lfo);
		card.simAudioIn[0] = saw.next(55);
		run(card, raw);
	}
	finalize(dir, "out_sweep.wav", "saw, reso on, LFO then square on cutoff", raw);
}

// 2) Resonance up full, no input: Borg II self-oscillates and tracks CV.
static void renderSelfOsc(const std::string &dir)
{
	DualBorg card;
	card.simSwitch = CC::Up;
	card.simKnob[CC::Main] = 4095;
	card.simKnob[CC::X] = card.simKnob[CC::Y] = 1800;
	const double notes[] = { 0, 1, 2, 3, 2, 1 };
	std::vector<int32_t> raw;
	printf("  self-oscillation tracking (Borg II, CV2 in volts -> Hz):\n");
	for (int s = 0; s < 6; s++)
	{
		card.simCVIn[1] = volts(notes[s]);
		long crossings = 0, measured = 0;
		int32_t prev = 0;
		for (long n = 0; n < SR; n++)
		{
			run(card, raw);
			int32_t y = card.simAudioOut[1];
			if (n > SR / 2) { measured++; if (prev < 0 && y >= 0) crossings++; }
			prev = y;
		}
		printf("    %.0f V -> %7.1f Hz\n", notes[s], crossings * (double)SR / measured);
	}
	finalize(dir, "out_selfosc.wav", "no input, max reso, CV2 steps 0..3V", raw);
}

// 3) VCA mode: saw + pings -> plucks. Borg I rings longer & softer.
static void renderVCA(const std::string &dir)
{
	DualBorg card;
	card.simSwitch = CC::Down;
	card.simConnected[CC::Audio1] = true;
	card.simKnob[CC::X] = card.simKnob[CC::Y] = 0;   // gate closed at rest
	card.simKnob[CC::Main] = 2600;
	Saw saw;
	const double seq[] = { 110, 130.8, 164.8, 98 };
	std::vector<int32_t> raw;
	long step = SR / 4;
	for (long n = 0; n < 8 * SR; n++)
	{
		card.simAudioIn[0] = saw.next(seq[(n / step) % 4]);
		card.simPulse[0] = card.simPulse[1] = pulseAt(n, step);
		run(card, raw);
	}
	finalize(dir, "out_vca.wav", "VCA mode, saw arpeggio + pings", raw);
}

// 4) Filter, resonance off: Main = drive, swept up.
static void renderDrive(const std::string &dir)
{
	DualBorg card;
	card.simSwitch = CC::Middle;
	card.simConnected[CC::Audio1] = true;
	card.simKnob[CC::X] = card.simKnob[CC::Y] = 2400;
	Saw saw;
	std::vector<int32_t> raw;
	for (long n = 0; n < 6 * SR; n++)
	{
		card.simKnob[CC::Main] = (int32_t)(4095.0 * n / (6 * SR));
		card.simAudioIn[0] = saw.next(82.4, 1200);
		run(card, raw);
	}
	finalize(dir, "out_drive.wav", "reso off, drive swept 1x..8x", raw);
}

// 5) Nothing patched, pings only: filter drums (noise-burst excitation).
static void renderDrums(const std::string &dir)
{
	DualBorg card;
	card.simSwitch = CC::Up;
	card.simKnob[CC::X] = 1300;
	card.simKnob[CC::Y] = 2000;
	card.simKnob[CC::Main] = 3700;
	std::vector<int32_t> raw;
	long q = SR / 2;
	for (long n = 0; n < 8 * SR; n++)
	{
		card.simPulse[0] = pulseAt(n, q);
		card.simPulse[1] = pulseAt((n + q / 2) % (8 * SR), q / 2) && (n % q) >= q / 2 - 48;
		run(card, raw);
	}
	finalize(dir, "out_drums.wav", "no input, pings -> resonant filter drums", raw);
}

int main(int argc, char **argv)
{
	std::string dir = argc > 1 ? argv[1] : ".";
	renderSweep(dir);
	renderSelfOsc(dir);
	renderVCA(dir);
	renderDrive(dir);
	renderDrums(dir);
	return 0;
}
