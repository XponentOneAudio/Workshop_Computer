/*
NAM Amp - Neural Amp Modeler captures on the Workshop Computer

Plays Neural Amp Modeler (.nam) amp and pedal captures, such as those shared
on tone3000.com, converted to fixed point with tools/nam2header.py.

The model is split between the RP2040's two cores as a two-stage pipeline:
core 1 runs the first part of the model on sample n while core 0 (in
ProcessSample) runs the rest of the model on sample n-1. This adds one sample
of latency.

Controls
--------
Main knob + CV 1:  Input gain (drive), -24dB to +24dB. Noon is the model's
                   calibrated input level.
Knob X:            Output level
Knob Y + CV 2:     Cabinet / tone: low-pass filter from 1kHz to 12kHz, off
                   fully clockwise
Switch Up:         Model 1
Switch Middle:     Model 2 (or model 1, if only one is built in)
Switch Down:       Bypass while held
Pulse In 1:        Rising edge toggles bypass

Audio In 1:        Input
Audio Out 1:       Model, through the cabinet filter
Audio Out 2:       Model, without the cabinet filter
CV Out 1:          Envelope of Audio Out 1
Pulse Out 1:       High when bypassed

LEDs: top row input level and output level; middle row model 1 / model 2;
bottom left bypass; bottom right lights if the model is too slow for the
processor (CPU overload).
*/

#include "ComputerCard.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include <math.h>

#include "model_data.h"

// The RP2040's clock must be a multiple of 48MHz for ComputerCard
static_assert(NAM_MODEL_CLOCK_MHZ % 48 == 0, "clock must be a multiple of 48MHz");

static int16_t ringMemory[NAM_RING_WORDS];

class NAMAmp : public ComputerCard
{
	nam_fx::Engine engines[kNumModels];

	// Frames passed between the cores, alternately
	nam_fx::Frame frames[2];
	uint32_t nextSlot = 0;
	bool pending = false;

	int32_t lastY = 0;
	int modelIndex = 0;
	int32_t fade = 0; // 0 to 4096, for switching models without clicks
	bool bypass = false;
	int32_t overloadTimer = 0, clipTimer = 0;

	// Input and output gains
	int32_t inGainQ12 = 0;     // Computer input -> model input (Q12)
	int32_t outGainQ16 = 0;    // model output (Q16) -> Computer output, Q16
	int32_t driveTable[256];   // Q16, -24dB to +24dB
	int32_t levelTable[256];   // Q16, -inf to +12dB

	// Cabinet filter: biquad coefficients (Q14) for 128 cutoff frequencies
	int16_t cabCoeffs[128][5];
	int cabIndex = 0;
	int32_t cx1 = 0, cx2 = 0, cy1 = 0, cy2 = 0;

	// DC blocker state (Q16)
	int32_t dcx1 = 0, dcy1 = 0;

	int32_t inEnv = 0, outEnv = 0;
	uint32_t controlCounter = 0;

public:
	NAMAmp()
	{
		int16_t *ring = ringMemory;
		for (int i = 0; i < kNumModels; i++)
		{
			engines[i].Init(kModels[i], ring);
			ring += kModels[i]->ring_words;
		}

		for (int i = 0; i < 256; i++)
		{
			float db = -24.0f + 48.0f * i / 255.0f;
			driveTable[i] = int32_t(65536.0f * powf(10.0f, db / 20.0f));
			float l = i / 255.0f;
			levelTable[i] = int32_t(65536.0f * 4.0f * l * l); // noon = 0dB
		}

		for (int i = 0; i < 128; i++)
		{
			// RBJ low-pass, Q = 0.8: a mild speaker-like resonance
			float fc = 1000.0f * powf(12.0f, i / 127.0f);
			float w0 = 2.0f * float(M_PI) * fc / 48000.0f;
			float alpha = sinf(w0) / (2.0f * 0.8f);
			float cw = cosf(w0), a0 = 1.0f + alpha;
			float b0 = (1.0f - cw) * 0.5f / a0;
			cabCoeffs[i][0] = int16_t(b0 * 16384.0f + 0.5f);
			cabCoeffs[i][1] = int16_t(2.0f * b0 * 16384.0f + 0.5f);
			cabCoeffs[i][2] = cabCoeffs[i][0];
			cabCoeffs[i][3] = int16_t(-2.0f * cw / a0 * 16384.0f - 0.5f);
			cabCoeffs[i][4] = int16_t((1.0f - alpha) / a0 * 16384.0f + 0.5f);
		}
		UpdateGains();
	}

	// Start the second core. Call before Run().
	void StartSecondCore()
	{
		instance = this;
		multicore_launch_core1(core1);
	}

	static void core1()
	{
		instance->Core1Loop();
	}

	// Core 1: run the first part of the model on each sample sent by core 0
	void Core1Loop()
	{
		while (1)
		{
			uint32_t token = multicore_fifo_pop_blocking();
			uint32_t slot = (token >> 16) & 1, model = (token >> 17) & 1;
			nam_fx::Frame &f = frames[slot];
			f.x = int16_t(token & 0xFFFF);
			engines[model].Run(f, 0, kModels[model]->split);
			__dmb();
			multicore_fifo_push_blocking(slot | (model << 1));
		}
	}

	void UpdateGains()
	{
		const nam_fx::Model *m = kModels[modelIndex];
		int drive = (KnobVal(Knob::Main) + CVIn1()) >> 4;
		drive = drive < 0 ? 0 : (drive > 255 ? 255 : drive);
		inGainQ12 = int32_t((int64_t(m->input_gain_q16) * driveTable[drive]) >> 20);

		int level = KnobVal(Knob::X) >> 4;
		outGainQ16 = int32_t((int64_t(m->output_gain_q16) * levelTable[level]) >> 16);

		int tone = KnobVal(Knob::Y) + CVIn2();
		cabIndex = tone >= 4000 ? -1 : (tone < 0 ? 0 : tone >> 5);
	}

	int32_t CabFilter(int32_t x)
	{
		if (cabIndex < 0)
		{
			cx2 = cx1; cx1 = x; cy2 = cy1; cy1 = x;
			return x;
		}
		const int16_t *c = cabCoeffs[cabIndex];
		int32_t y = (c[0] * x + c[1] * cx1 + c[2] * cx2 - c[3] * cy1 - c[4] * cy2 + 8192) >> 14;
		cx2 = cx1; cx1 = x;
		cy2 = cy1; cy1 = y;
		return y;
	}

	virtual void ProcessSample()
	{
		int32_t in = AudioIn1();

		if ((++controlCounter & 15) == 0) UpdateGains();

		// Model selection, with a short fade out and in
		int wanted = (SwitchVal() == Switch::Middle && kNumModels > 1) ? 1 : 0;
		if (wanted != modelIndex)
		{
			fade -= 32;
			if (fade <= 0)
			{
				fade = 0;
				modelIndex = wanted;
			}
		}
		else if (fade < 4096)
		{
			fade += 4;
		}

		if (PulseIn1RisingEdge()) bypass = !bypass;
		bool bypassed = bypass || SwitchVal() == Switch::Down;

		// Model input, Q12, limited to +-2.0
		int32_t x = (in * inGainQ12) >> 12;
		if (x > 8192 || x < -8192) clipTimer = 4800;
		x = nam_fx::Clamp(x, 8192);

		// Collect the result for the previous sample from core 1, send it
		// this sample, then run the rest of the model on the previous sample
		nam_fx::Frame *done = nullptr;
		int doneModel = 0;
		if (pending)
		{
			if (multicore_fifo_rvalid())
			{
				uint32_t token = sio_hw->fifo_rd;
				done = &frames[token & 1];
				doneModel = (token >> 1) & 1;
				pending = false;
			}
			else
			{
				overloadTimer = 12000;
			}
		}
		if (!pending)
		{
			sio_hw->fifo_wr = uint32_t(x & 0xFFFF) | (nextSlot << 16) | (uint32_t(modelIndex) << 17);
			__sev();
			nextSlot ^= 1;
			pending = true;
		}
		if (done)
		{
			engines[doneModel].Run(*done, kModels[doneModel]->split, kModels[doneModel]->num_units);
			lastY = done->y;
		}

		// DC blocker (~30Hz), then gain to the Computer's output level
		int32_t dc = lastY - dcx1 + dcy1 - (dcy1 >> 8);
		dcx1 = lastY;
		dcy1 = dc;
		int32_t y = int32_t((int64_t(dc) * outGainQ16) >> 16);
		y = (nam_fx::Clamp(y, 4095) * fade) >> 12;

		int32_t out1 = CabFilter(y);
		int32_t out2 = y;
		if (bypassed) out1 = out2 = in;
		AudioOut1(int16_t(out1));
		AudioOut2(int16_t(out2));

		// Envelopes and LEDs
		int32_t ain = in < 0 ? -in : in, aout = out1 < 0 ? -out1 : out1;
		inEnv += (ain - inEnv) >> 7;
		outEnv += (aout - outEnv) >> 7;
		CVOut1(int16_t(outEnv > 2047 ? 2047 : outEnv));
		PulseOut1(bypassed);

		if (overloadTimer > 0) overloadTimer--;
		if (clipTimer > 0) clipTimer--;
		if ((controlCounter & 63) == 0)
		{
			LedBrightness(0, clipTimer > 0 ? 4095 : uint16_t(inEnv * 2 > 4095 ? 4095 : inEnv * 2));
			LedBrightness(1, uint16_t(outEnv * 2 > 4095 ? 4095 : outEnv * 2));
			LedOn(2, modelIndex == 0);
			LedOn(3, modelIndex == 1);
			LedOn(4, bypassed);
			LedOn(5, overloadTimer > 0);
		}
	}

private:
	static NAMAmp *instance;
};

NAMAmp *NAMAmp::instance = nullptr;

int main()
{
	// Overclocking: raise the core voltage first. 240MHz at 1.2V matches
	// other Workshop Computer cards.
	vreg_set_voltage(NAM_MODEL_CLOCK_MHZ > 192 ? VREG_VOLTAGE_1_20 : VREG_VOLTAGE_1_15);
	sleep_ms(10);
	set_sys_clock_khz(NAM_MODEL_CLOCK_MHZ * 1000, true);

	static NAMAmp amp;
	amp.StartSecondCore();
	amp.Run();
}
