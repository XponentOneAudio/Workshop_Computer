#include "ComputerCard.h"
#include "pico/multicore.h"
#include "hardware/sync.h"
#include <cmath>

/*
Sprout Probe — plant conductance test card
==========================================

A test bench for reading a plant's electrical resistance with the Workshop
Computer, as a first step towards a MIDI Sprout style biodata card.

The plant is measured with an audio tone, not DC:

  Audio Out 1 ──► electrode A ── [PLANT] ── electrode B ──► amplifier in
  Audio In 1  ◄────────────────────────────────────────── amplifier out

The plant and the amplifier's input resistance form a voltage divider, so
the level of the tone that comes back changes with the plant's resistance.

The card measures that level with lock-in (synchronous) detection: the
input is multiplied by the reference sine and by a 90° copy (cosine), and
both products are summed over a 20 ms window. The window always holds a
whole number of cycles of the tone, so DC offsets, slow drift and anything
not at the tone frequency (such as mains hum) average out. Averaging 960
samples also gives much finer resolution than a single ADC reading.

Core 0 (ProcessSample, 48 kHz): makes the tone, accumulates the sums,
                                 drives the jacks.
Core 1 (SlowCore, every 20 ms): turns the sums into level and phase,
                                 tracks the baseline, runs a MIDI Sprout
                                 style change detector, drives the LEDs.

Controls
--------
Main knob : tone output level (start low, around 9 o'clock)
X knob    : tone frequency, five steps: 250 Hz, 500 Hz, 1 kHz, 2 kHz, 4 kHz
Y knob    : zoom (gain, 1x to 128x) for the change signal on CV Out 2
Switch Up / Middle : normal
Switch Down (hold) : re-zero, snaps the baseline to the current level

Outputs
-------
Audio Out 1 : excitation tone, to the plant
Audio Out 2 : phase of the returned tone (-pi..+pi -> about -6V..+6V)
CV Out 1    : returned tone level (0 .. about +6V at ADC full scale)
CV Out 2    : change signal, (level - slow baseline) x zoom, centred on 0V
Pulse Out 1 : 10 ms trigger when the MIDI Sprout style detector fires
Pulse Out 2 : high while the input is clipping

LEDs (layout  0 1 / 2 3 / 4 5)
----
Left column (4 bottom, 2, 0 top): level meter, aim for 2 lit
LED 1 : input clipping, turn the tone level or amplifier gain down
LED 3 : size of the change signal
LED 5 : flashes when the detector fires (a note in the full card)
*/

class SproutProbe : public ComputerCard
{
	// ---- constants ----------------------------------------------------
	static constexpr int kTableSize = 192;        // one sine cycle
	static constexpr int kQuarter = kTableSize / 4;  // 90 degrees
	static constexpr int kWindow = 960;           // 20 ms at 48 kHz
	static constexpr int16_t kRefAmp = 2047;      // sine table amplitude
	static constexpr int kTrigSamples = 480;      // 10 ms trigger
	// Table steps per sample: 192/step samples per cycle.
	// 1, 2, 4, 8, 16 -> 250, 500, 1000, 2000, 4000 Hz.
	// 960 is a multiple of every cycle length, so each window is whole cycles.
	static constexpr int kSteps[5] = {1, 2, 4, 8, 16};

	// Detector settings (as in MIDI Sprout: range > stdev x threshold)
	static constexpr int kHistory = 10;           // readings per window
	static constexpr float kThreshold = 2.6f;     // range/stdev ratio to fire
	static constexpr int kRefractory = 5;         // readings (100 ms) between triggers
	static constexpr float kMinLevel = 4.0f;      // ignore if signal below this (ADC counts)
	static constexpr float kClipLevel = 1950.0f;  // |input| above this counts as clipping

	int16_t sine[kTableSize];

	// ---- core 0 state (only touched in ProcessSample) ------------------
	int phase = 0;
	int step = 4;
	int count = 0;
	int64_t accI = 0, accQ = 0;
	int32_t peak = 0;
	int32_t trigTimer = 0;
	uint32_t lastTrigSeen = 0;

	// ---- core 0 -> core 1 ---------------------------------------------
	volatile int64_t winI = 0, winQ = 0;
	volatile int32_t winPeak = 0;
	volatile uint32_t winSeq = 0;

	// ---- core 1 -> core 0 ---------------------------------------------
	volatile int16_t outLevel = 0;   // CV Out 1
	volatile int16_t outChange = 0;  // CV Out 2
	volatile int16_t outPhase = 0;   // Audio Out 2
	volatile bool outClip = false;   // Pulse Out 2
	volatile uint32_t trigSeq = 0;   // incremented on each detector event

	static SproutProbe *instance;

public:
	SproutProbe()
	{
		for (int i = 0; i < kTableSize; i++)
		{
			sine[i] = int16_t(lroundf(kRefAmp * sinf(2.0f * float(M_PI) * float(i) / kTableSize)));
		}
	}

	void StartSecondCore()
	{
		instance = this;
		multicore_launch_core1(core1);
	}

	static void core1() { instance->SlowCore(); }

	// ---- 48 kHz -------------------------------------------------------
	virtual void ProcessSample()
	{
		int32_t ref = sine[phase];
		int32_t refQ = sine[(phase + kQuarter) % kTableSize];

		// Excitation tone, level from Main knob (0..2047 peak)
		int32_t level = KnobVal(Knob::Main) >> 1;
		AudioOut1(int16_t((ref * level) >> 11));

		// Lock-in accumulation (in x ref fits 23 bits; sum of 960 needs 64 bits)
		int32_t in = AudioIn1();
		accI += in * ref;
		accQ += in * refQ;
		int32_t mag = in < 0 ? -in : in;
		if (mag > peak) peak = mag;

		phase += step;
		if (phase >= kTableSize) phase -= kTableSize;

		if (++count == kWindow)
		{
			// Hand the finished window to core 1
			winI = accI;
			winQ = accQ;
			winPeak = peak;
			__dmb();
			winSeq = winSeq + 1;

			accI = accQ = 0;
			peak = 0;
			count = 0;

			// Change frequency only at a window boundary, so each window
			// always holds whole cycles
			int zone = (KnobVal(Knob::X) * 5) >> 12;  // 0..4
			if (zone > 4) zone = 4;
			step = kSteps[zone];
		}

		// Detector trigger
		uint32_t t = trigSeq;
		if (t != lastTrigSeen)
		{
			lastTrigSeen = t;
			trigTimer = kTrigSamples;
		}
		if (trigTimer > 0) trigTimer--;

		PulseOut1(trigTimer > 0);
		PulseOut2(outClip);
		CVOut1(outLevel);
		CVOut2(outChange);
		AudioOut2(outPhase);
	}

	// ---- every 20 ms, on core 1 ----------------------------------------
	void SlowCore()
	{
		uint32_t seenSeq = winSeq;
		float baseline = 0.0f;
		bool haveBaseline = false;
		float history[kHistory] = {0};
		int histCount = 0, histPos = 0;
		int refractory = 0;
		int clipHold = 0, trigHold = 0;

		while (true)
		{
			while (winSeq == seenSeq) tight_loop_contents();
			seenSeq = winSeq;
			__dmb();
			float fI = float(winI);
			float fQ = float(winQ);
			int32_t pk = winPeak;

			// sum(in*ref) over whole cycles = A * kRefAmp * N / 2, so
			// A = 2 * |I,Q| / (kRefAmp * N), in ADC counts (0..2047)
			float level = 2.0f * sqrtf(fI * fI + fQ * fQ) / (float(kRefAmp) * float(kWindow));
			float ph = atan2f(fQ, fI);  // -pi..pi

			// Baseline: slow average (~1.3 s), or snap with switch held down
			if (!haveBaseline || SwitchVal() == Switch::Down)
			{
				baseline = level;
				haveBaseline = true;
			}
			else
			{
				baseline += (level - baseline) * (1.0f / 64.0f);
			}

			// Zoom 1x..128x, exponential across the Y knob
			float zoom = exp2f(float(KnobVal(Knob::Y)) * (7.0f / 4096.0f));
			float change = (level - baseline) * zoom;

			// MIDI Sprout style detector over the last kHistory readings
			history[histPos] = level;
			histPos = (histPos + 1) % kHistory;
			if (histCount < kHistory) histCount++;
			if (refractory > 0) refractory--;

			bool fire = false;
			if (histCount == kHistory && refractory == 0 && level > kMinLevel)
			{
				float mn = history[0], mx = history[0], sum = 0.0f, sumSq = 0.0f;
				for (int i = 0; i < kHistory; i++)
				{
					float v = history[i];
					if (v < mn) mn = v;
					if (v > mx) mx = v;
					sum += v;
					sumSq += v * v;
				}
				float mean = sum / kHistory;
				float var = sumSq / kHistory - mean * mean;
				// stdev floor: ignore sub-0.05-count wobble (pure arithmetic noise)
				float sd = sqrtf(var > 0.0025f ? var : 0.0025f);
				if ((mx - mn) > sd * kThreshold && (mx - mn) > 0.25f)
				{
					fire = true;
					refractory = kRefractory;
				}
			}

			// Publish to core 0
			outLevel = clamp12(level);
			outChange = clamp12(change);
			outPhase = clamp12(ph * (2047.0f / float(M_PI)));
			outClip = float(pk) > kClipLevel;
			if (fire) trigSeq = trigSeq + 1;

			// LEDs: left column level meter, 3 segments over 0..1536 counts
			LedBrightness(4, segment(level, 0.0f));
			LedBrightness(2, segment(level, 512.0f));
			LedBrightness(0, segment(level, 1024.0f));

			if (outClip) clipHold = 10;  // 200 ms
			LedOn(1, clipHold > 0);
			if (clipHold > 0) clipHold--;

			float ac = change < 0 ? -change : change;
			LedBrightness(3, uint16_t(ac * 2.0f > 4095.0f ? 4095.0f : ac * 2.0f));

			if (fire) trigHold = 3;  // 60 ms
			LedOn(5, trigHold > 0);
			if (trigHold > 0) trigHold--;
		}
	}

private:
	static int16_t clamp12(float v)
	{
		if (v > 2047.0f) return 2047;
		if (v < -2048.0f) return -2048;
		return int16_t(v);
	}

	// LED brightness 0..4095 for the part of 'level' in [start, start+512)
	static uint16_t segment(float level, float start)
	{
		float x = (level - start) * (4095.0f / 512.0f);
		if (x < 0.0f) return 0;
		if (x > 4095.0f) return 4095;
		return uint16_t(x);
	}
};

SproutProbe *SproutProbe::instance = nullptr;
constexpr int SproutProbe::kSteps[5];

int main()
{
	set_sys_clock_khz(144000, true);

	static SproutProbe card;
	card.StartSecondCore();
	card.Run();
}
