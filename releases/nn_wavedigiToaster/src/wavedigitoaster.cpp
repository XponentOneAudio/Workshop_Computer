// wavedigitoaster.cpp - MFOS Noise Toaster as a wave digital model, for the
// Music Thing Modular Workshop System Computer.
//
// The circuit model lives in toaster_model.h; this file maps the Noise
// Toaster's front panel onto the Computer's three knobs and switch, and wires
// up the jacks. See ../README.md for the panel layout.

#include "ComputerCard.h"
#include "toaster_model.h"

using namespace wdt;

class WaveDigiToaster : public ComputerCard
{
public:
	WaveDigiToaster()
	{
		// Page A (switch middle) starts at the physical knob positions;
		// pages B and C start at sensible defaults and are picked up later.
		for (int k = 0; k < 3; k++) page[0][k] = KnobVal((Knob)k);
		page[1][0] = 2600; page[1][1] = 700; page[1][2] = 1900; // LFO rate, attack, release
		page[2][0] = 2900; page[2][1] = 2048; page[2][2] = 0;    // VCF mod, VCO mod, source
		for (int p = 0; p < 3; p++)
			for (int k = 0; k < 3; k++) caught[p][k] = (p == 0);
	}

#ifdef COMPUTERCARD_HOST_SIM
	// lets the offline renderer preset pages B/C without simulating pickup
	void SimSetPage(int p, int k, int32_t v) { page[p][k] = v; caught[p][k] = (p == 0); }
#endif

protected:
	void __not_in_flash_func(ProcessSample)() override
	{
		// ---- control rate: knobs, switch, coefficients
		if (++ctlCount >= CONTROL_DIV)
		{
			ctlCount = 0;
			ControlUpdate();
		}

		// ---- audio rate
		Inputs in;
		in.extAudio = Connected(Audio1) ? AudioIn1() * EXT_AUDIO_SCALE : 0;
		in.fmVolts = Connected(Audio2) ? AudioIn2() * VOLTS_PER_COUNT : 0;
		bool gatedMode = Connected(Pulse1);
		in.gate = gatedMode && (PulseIn1() || SwitchVal() == Down);
		in.syncPulse = PulseIn2();

		Outputs out;
		toaster.Process(in, out);

		AudioOut1(ClampOut(out.vca));
		AudioOut2(ClampOut(out.vcf));

		// AREG envelope 0..~6 V, LFO "triangle" +/-5 V
		int32_t env = ToCounts(mul28(out.arg - V(1.26f), Q28(0.75f)));
		CVOut1(ClampCV(env));
		int32_t lfo = ToCounts(mul28(out.lfo - V(3.125f), Q28(1.6f)));
		CVOut2(ClampCV(lfo));
		PulseOut1(out.lfoSquare);
		PulseOut2(out.aregGate);

		// LEDs: LFO rate indicator (lit when the LFO square is low, as on the
		// toaster), envelope, VCO square, VCF level, page indicators.
		LedOn(0, !out.lfoSquare);
		LedBrightness(1, (uint16_t)ClampU12(env * 2));
		LedOn(2, out.vcoSquare);
		int32_t lvl = out.vcf < 0 ? -out.vcf : out.vcf;
		vcfLevel += (lvl - vcfLevel) >> 10;
		LedBrightness(3, (uint16_t)ClampU12(vcfLevel >> 13)); // ~2 V -> full
		uint16_t pageLed = anyUncaught ? 1200 : 4095;
		LedBrightness(4, currentPage == 1 ? pageLed : 0);
		LedBrightness(5, currentPage == 2 ? pageLed : 0);
	}

private:
	static constexpr int CONTROL_DIV = 8;                 // 6 kHz control rate
	static constexpr int32_t COUNTS_PER_VOLT = 341;       // jacks: ~6 V full scale
	static constexpr int32_t AUDIO_GAIN = 2;              // audio outs: toaster line level x2
	static constexpr int32_t VOLTS_PER_COUNT = V(1.0f) / COUNTS_PER_VOLT;
	static constexpr int32_t EXT_AUDIO_SCALE = V(1.0f) / (5 * COUNTS_PER_VOLT); // +/-5 V in -> +/-1 V at the VCF
	static constexpr int PICKUP_WINDOW = 40;

	// Q24 volts -> DAC counts, times gain (no division on the M0+)
	static int32_t ToCounts(int32_t v, int32_t gain = 1)
	{
		return ((v >> 8) * (COUNTS_PER_VOLT * gain)) >> 16;
	}
	// soft knee over the top quarter of the DAC range instead of a hard clip
	static int16_t ClampOut(int32_t v)
	{
		constexpr int32_t KNEE = 1536, ROOM = 2047 - KNEE;
		int32_t c = ToCounts(v, AUDIO_GAIN);
		int32_t a = c < 0 ? -c : c;
		if (a > KNEE)
		{
			int32_t over = a - KNEE;
			a = KNEE + over * ROOM / (ROOM + over);
			c = c < 0 ? -a : a;
		}
		return (int16_t)c;
	}
	static int16_t ClampCV(int32_t c)
	{
		if (c > 2047) c = 2047;
		if (c < -2048) c = -2048;
		return (int16_t)c;
	}
	static int32_t ClampU12(int32_t c) { return c < 0 ? 0 : (c > 4095 ? 4095 : c); }

	void __not_in_flash_func(ControlUpdate)()
	{
		Switch sw = SwitchVal();
		currentPage = sw == Middle ? 0 : (sw == Up ? 1 : 2);
		if (currentPage != lastPage)
		{
			// soft takeover: a knob only takes effect once it reaches the stored value
			for (int k = 0; k < 3; k++) caught[currentPage][k] = false;
			lastPage = currentPage;
		}
		anyUncaught = false;
		for (int k = 0; k < 3; k++)
		{
			int32_t v = KnobVal((Knob)k);
			int32_t &stored = page[currentPage][k];
			if (!caught[currentPage][k])
			{
				int32_t d = v - stored;
				if (d < PICKUP_WINDOW && d > -PICKUP_WINDOW) caught[currentPage][k] = true;
				else anyUncaught = true;
			}
			// small deadband: ADC jitter would otherwise re-trigger the
			// control-rate coefficient maths on every tick
			if (caught[currentPage][k] && (v - stored > 2 || stored - v > 2)) stored = v;
		}

		Controls c;
		c.cutoff = page[0][0] * (1.0f / 4095.0f);
		c.pitch = page[0][1] * (1.0f / 4095.0f);
		c.reso = page[0][2] * (1.0f / 4095.0f);
		c.lfoRate = page[1][0] * (1.0f / 4095.0f);
		c.attack = page[1][1] * (1.0f / 4095.0f);
		c.release = page[1][2] * (1.0f / 4095.0f);
		c.vcfMod = page[2][0] * (1.0f / 2047.5f) - 1.0f;
		c.vcoMod = page[2][1] * (1.0f / 2047.5f) - 1.0f;
		if (c.vcoMod > -0.04f && c.vcoMod < 0.04f) c.vcoMod = 0.0f;
		c.source = page[2][2] * 5 / 4096;
		c.repeat = !Connected(Pulse1);
		c.sync = Connected(Pulse2);
		c.pitchCV = Connected(CV1) ? CVIn1() * (1.0f / 341.0f) : 0.0f;
		c.cutoffCV = Connected(CV2) ? CVIn2() * (1.0f / 341.0f) : 0.0f;
		toaster.Control(c, toaster.LastLfo(), toaster.LastArg());
	}

	NoiseToaster toaster;
	int32_t page[3][3];
	bool caught[3][3];
	bool anyUncaught = false;
	int currentPage = 0, lastPage = 0;
	int ctlCount = 0;
	int32_t vcfLevel = 0;
};

#ifndef COMPUTERCARD_HOST_SIM
int main()
{
	set_sys_clock_khz(192000, true);
	static WaveDigiToaster card;
	card.EnableNormalisationProbe();
	card.Run();
}
#endif
