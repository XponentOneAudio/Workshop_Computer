// render_wav.cpp
//
// Runs the shaker model on an ordinary computer and writes the result to a
// WAV file, so the model can be heard and experimented with without any
// hardware.  This works because Shaker.h has no hardware code in it at all.
//
// Build and run, from this card's folder:
//
//     g++ -O2 -I. -o render_wav test/render_wav.cpp
//     ./render_wav shaker.wav
//
// It plays six short scenes, each a different setting of the model, with a
// pretend hand doing the shaking.  Try changing the numbers and listening.

#include "Shaker.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static Shaker shaker;
static FILE *wav;
static long samplesWritten = 0;

// Write a 16-bit number to the file, low byte first, as WAV requires.
static void Write16(int v)
{
	fputc(v & 0xFF, wav);
	fputc((v >> 8) & 0xFF, wav);
}

static void Write32(long v)
{
	Write16(int(v & 0xFFFF));
	Write16(int((v >> 16) & 0xFFFF));
}

// A WAV file starts with a 44-byte header describing the audio that follows.
static void WriteHeader(long numSamples)
{
	const long dataBytes = numSamples * 4;   // 2 channels x 2 bytes each
	fputs("RIFF", wav); Write32(36 + dataBytes);
	fputs("WAVE", wav);
	fputs("fmt ", wav); Write32(16);
	Write16(1);                              // plain uncompressed audio
	Write16(2);                              // stereo
	Write32(Shaker::sampleRate);
	Write32(Shaker::sampleRate * 4);         // bytes per second
	Write16(4);                              // bytes per stereo sample
	Write16(16);                             // bits per sample
	fputs("data", wav); Write32(dataBytes);
}

// The same output stage as main.cpp, with the volume knob at half way,
// then scaled from the hardware's 12 bits up to the WAV file's 16.
static int Output(int32_t v)
{
	v = ((v >> 8) * 2048) >> 11;
	const int32_t knee = 1024, room = 2047 - knee;
	const bool negative = v < 0;
	if (negative) v = -v;
	if (v > knee)
	{
		int32_t over = v - knee;
		if (over > 1000000) over = 1000000;
		v = knee + (room * over) / (over + room);
	}
	return (negative ? -v : v) * 16;
}

// The kinds of movement the pretend hand can make.
enum Gesture { ShakeIt, TiltSlowly, Triggers };

// Run the model for a number of seconds with one kind of movement.
static void Play(Gesture gesture, float seconds)
{
	const int n = int(seconds * Shaker::sampleRate);
	for (int i = 0; i < n; i++)
	{
		const float t = float(i) / Shaker::sampleRate;
		int32_t side = 0;

		if (gesture == ShakeIt)
		{
			// Shaking back and forth four times a second.  A real shake is
			// several g, but the 8mu's sensor stops at 1 g, so clip it.
			float g = 3.0f * sinf(2.0f * 3.14159265f * 4.0f * t);
			if (g > 1.0f) g = 1.0f;
			if (g < -1.0f) g = -1.0f;
			// The 8mu only reports steps of 16.
			side = (int32_t(g * 2032.0f) / 16) * 16;
		}
		else if (gesture == TiltSlowly)
		{
			// Tipped about 20 degrees one way, then the other.
			side = (t < seconds / 2) ? 700 : -700;
		}
		else if (gesture == Triggers && i % 12000 == 0)
		{
			// No movement: a trigger four times a second, every other
			// one an accent, as if from a sequencer.
			shaker.Trigger((i % 24000 == 0) ? 70000 : 20000);
		}

		shaker.SetMotion(side, 0, 0);

		int32_t left, right;
		shaker.Process(0, left, right);
		Write16(Output(left));
		Write16(Output(right));
		samplesWritten++;
	}

	// Half a second of stillness, to hear the tail die away.
	if (gesture != Triggers) shaker.SetMotion(0, 0, 0);
	for (int i = 0; i < Shaker::sampleRate / 2; i++)
	{
		int32_t left, right;
		shaker.Process(0, left, right);
		Write16(Output(left));
		Write16(Output(right));
		samplesWritten++;
	}
}

int main(int argc, char **argv)
{
	const char *filename = (argc > 1) ? argv[1] : "shaker.wav";
	wav = fopen(filename, "wb");
	if (!wav)
	{
		printf("Could not open %s\n", filename);
		return 1;
	}
	WriteHeader(0);   // placeholder; rewritten with the true length at the end

	shaker.Init();

	// 1. The default settings: a maraca.
	shaker.params = ShakerParams();
	Play(ShakeIt, 2.0f);

	// 2. Sleigh bells: small, metal, few hard grains, scattered tuning.
	shaker.params = ShakerParams();
	shaker.params.size = 20;
	shaker.params.material = 115;
	shaker.params.grains = 40;
	shaker.params.hardness = 120;
	shaker.params.decay = 60;
	shaker.params.scatter = 60;
	Play(ShakeIt, 2.0f);

	// 3. A big box of sand: large, dead, many soft grains.
	shaker.params = ShakerParams();
	shaker.params.size = 110;
	shaker.params.material = 5;
	shaker.params.grains = 127;
	shaker.params.hardness = 10;
	Play(ShakeIt, 2.0f);

	// 4. Rainstick: long travel and a long decay, played by tilting.
	shaker.params = ShakerParams();
	shaker.params.size = 50;
	shaker.params.material = 55;
	shaker.params.grains = 60;
	shaker.params.decay = 85;
	shaker.params.travel = 105;
	shaker.params.scatter = 25;
	Play(TiltSlowly, 6.0f);

	// 5. A single pea in a tin: one grain, bright metal.
	shaker.params = ShakerParams();
	shaker.params.size = 45;
	shaker.params.material = 100;
	shaker.params.grains = 12;
	shaker.params.hardness = 127;
	shaker.params.decay = 70;
	Play(ShakeIt, 2.0f);

	// 6. The maraca again, played by triggers instead of motion.
	shaker.params = ShakerParams();
	Play(Triggers, 2.0f);

	// Go back and write the header again, now the length is known.
	fseek(wav, 0, SEEK_SET);
	WriteHeader(samplesWritten);
	fclose(wav);

	printf("Wrote %ld samples (%.1f seconds) to %s\n",
	       samplesWritten, double(samplesWritten) / Shaker::sampleRate, filename);
	return 0;
}
