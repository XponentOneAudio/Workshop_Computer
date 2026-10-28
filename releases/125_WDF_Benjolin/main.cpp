// WDF Benjolin - a wave digital filter model of Rob Hordijk's Benjolin
// for the Music Thing Modular Workshop System Computer.
//
// The DSP lives in benjolin_wdf.h; this file maps it onto the panel.

#include "ComputerCard.h"
#include "benjolin_wdf.h"

#include "hardware/clocks.h"
#if WDFB_PROFILE
#include "hardware/structs/systick.h"
#endif

using namespace wdfb;

// Soft takeover for knobs shared between the normal and shift pages:
// a parameter only follows its knob once the knob has reached (or crossed)
// the parameter's current value, so switching page never makes it jump.
struct Pickup
{
	int32_t value;
	bool live = true;
	int32_t side = 0;

	void Release(int32_t knob)
	{
		live = false;
		side = knob > value ? 1 : -1;
	}
	void Update(int32_t knob)
	{
		if (!live)
		{
			int32_t s = knob > value ? 1 : -1;
			if (s != side || (knob - value < 40 && value - knob < 40)) live = true;
		}
		if (live) value = knob;
	}
};

class WDFBenjolin : public ComputerCard
{
public:
	WDFBenjolin()
	{
		benj.Init();
		benj.reg = (uint8_t)(UniqueCardID() | 1);

		osc1 = {2400};
		osc2 = {1600};
		cutoff = {2600};
		run1 = {1700};
		run2 = {1200};
		res = {2700};
	}

protected:
	void __not_in_flash_func(ProcessSample)() override
	{
#if WDFB_PROFILE
		uint32_t t0 = systick_hw->cvr;
#endif
		int32_t kMain = KnobVal(Knob::Main);
		int32_t kX = KnobVal(Knob::X);
		int32_t kY = KnobVal(Knob::Y);
		Switch sw = SwitchVal();
		bool shift = sw == Switch::Down;

		if (shift != lastShift)
		{
			if (shift)
			{
				run1.Release(kMain);
				run2.Release(kX);
				res.Release(kY);
			}
			else
			{
				osc1.Release(kMain);
				osc2.Release(kX);
				cutoff.Release(kY);
			}
			lastShift = shift;
		}

		if (shift)
		{
			run1.Update(kMain);
			run2.Update(kX);
			res.Update(kY);
		}
		else
		{
			osc1.Update(kMain);
			osc2.Update(kX);
			cutoff.Update(kY);
		}

		Controls c;
		// Oscillators: 13.5 octaves from 0.5 Hz, CV inputs ~1V/oct
		c.osc1Oct = osc1.value * 216;
		c.osc2Oct = osc2.value * 216;
		if (Connected(Input::CV1)) c.osc1Oct += CVIn1() * 192;
		if (Connected(Input::CV2)) c.osc2Oct += CVIn2() * 192;

		// Rungler depths: 0..5 octaves
		c.run1 = run1.value * 80;
		c.run2 = run2.value * 80;

		// Filter: 9 octaves from 30 Hz. Audio In 2 is cutoff CV, normalled
		// to the rungler at 3 octaves depth.
		c.cutoffOct = cutoff.value * 144;
		if (Connected(Input::Audio2))
		{
			c.cutoffOct += AudioIn2() * 192;
			c.runFilt = 0;
		}
		else
		{
			c.runFilt = 3 * OCT;
		}
		c.res = res.value << 18;

		// Switch up = loop; Pulse In 2 high inverts the loop/chaos state
		c.loop = (sw == Switch::Up) != (Connected(Input::Pulse2) && PulseIn2());

		// Pulse In 1 replaces oscillator 2 as the rungler clock
		c.useExtClock = Connected(Input::Pulse1);
		c.extClock = PulseIn1();

		// Audio In 1 replaces the PWM comparator as the filter input
		c.useExtIn = Connected(Input::Audio1);
		c.extIn = AudioIn1() << 13;

		benj.Step(c);

		AudioOut1(Audio(benj.filter.lp));
		AudioOut2(Audio(benj.filter.bp));
		CVOut1(clampi(benj.rungler >> 13, 0, 2047));
		CVOut2(clampi(benj.osc2.v >> 13, -2048, 2047));
		PulseOut1(benj.osc1.Square());
		PulseOut2(benj.osc2.Square());

		if (shift)
		{
			LedBrightness(0, run1.value);
			LedBrightness(1, run2.value);
			LedBrightness(2, res.value);
			LedBrightness(3, res.value);
			LedOff(4);
			LedOff(5);
		}
		else
		{
			LedBrightness(0, (benj.osc1.v + ONE) >> 13);
			LedBrightness(1, (benj.osc2.v + ONE) >> 13);
			LedBrightness(2, benj.rungler >> 12);
			LedOn(3, benj.pwm > 0);
			LedBrightness(4, clampi(abs(benj.filter.lp) >> 12, 0, 4095));
			LedBrightness(5, clampi(abs(benj.filter.bp) >> 12, 0, 4095));
		}

#if WDFB_PROFILE
		// SysTick counts down; light LED 5 permanently if a sample ever
		// takes more than 75% of the 48kHz budget
		uint32_t cycles = (t0 - systick_hw->cvr) & 0xFFFFFF;
		if (cycles > maxCycles) maxCycles = cycles;
		if (maxCycles > (uint32_t)(0.75f * 144000000 / 48000)) LedOn(5);
#endif
	}

private:
	Benjolin benj;
	Pickup osc1, osc2, cutoff, run1, run2, res;
	bool lastShift = false;
#if WDFB_PROFILE
	uint32_t maxCycles = 0;
#endif

	// Q24 filter output -> 12 bit DAC, with ~1.0 mapping to ~4V
	static int16_t Audio(int32_t x)
	{
		return (int16_t)clampi((x >> 12) * 1400 >> 12, -2047, 2047);
	}
};

int main()
{
	set_sys_clock_khz(144000, true);

#if WDFB_PROFILE
	systick_hw->rvr = 0xFFFFFF;
	systick_hw->csr = 0x5; // enable, processor clock
#endif

	static WDFBenjolin card;
	card.EnableNormalisationProbe();
	card.Run();
}
