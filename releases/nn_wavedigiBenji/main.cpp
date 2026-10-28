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

// Soft takeover for knobs shared between pages: a parameter only follows its
// knob once the knob has reached (or crossed) the parameter's current value,
// so changing page never makes it jump.
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

// Knob pages, stepped through by tapping Z down:
//            Main         X                   Y
//   page 1   Osc 1 rate   Rungler > Osc 1     FM Osc 2 > Osc 1
//   page 2   Osc 2 rate   Rungler > Osc 2     FM Osc 1 > Osc 2
//   page 3   Cutoff       Rungler > Cutoff    Resonance
enum Page
{
	PAGE_OSC1,
	PAGE_OSC2,
	PAGE_FILTER,
	NUM_PAGES
};

class WDFBenjolin : public ComputerCard
{
public:
	WDFBenjolin()
	{
		benj.Init();
		benj.reg = (uint8_t)(UniqueCardID() | 1);

		param[PAGE_OSC1][0] = {2400}; // osc 1 rate
		param[PAGE_OSC1][1] = {1700}; // rungler > osc 1
		param[PAGE_OSC1][2] = {0};	  // FM osc 2 > osc 1
		param[PAGE_OSC2][0] = {1600}; // osc 2 rate
		param[PAGE_OSC2][1] = {1200}; // rungler > osc 2
		param[PAGE_OSC2][2] = {0};	  // FM osc 1 > osc 2
		param[PAGE_FILTER][0] = {2600}; // cutoff
		param[PAGE_FILTER][1] = {2458}; // rungler > cutoff (3 octaves)
		param[PAGE_FILTER][2] = {2700}; // resonance
	}

protected:
	void __not_in_flash_func(ProcessSample)() override
	{
#if WDFB_PROFILE
		uint32_t t0 = systick_hw->cvr;
#endif
		int32_t knob[3] = {KnobVal(Knob::Main), KnobVal(Knob::X), KnobVal(Knob::Y)};
		Switch sw = SwitchVal();

		// A tap down on Z steps to the next page. Require the switch to have
		// been out of the down position for 20ms, to ignore contact bounce.
		if (sw == Switch::Down)
		{
			if (notDownCount >= 960)
			{
				page = (page + 1) % NUM_PAGES;
				for (int k = 0; k < 3; k++) param[page][k].Release(knob[k]);
			}
			notDownCount = 0;
		}
		else if (notDownCount < 960)
		{
			notDownCount++;
		}

		for (int k = 0; k < 3; k++) param[page][k].Update(knob[k]);

		Controls c;
		// Oscillators: 13.5 octaves from 0.5 Hz, CV inputs ~1V/oct
		c.osc1Oct = param[PAGE_OSC1][0].value * 216;
		c.osc2Oct = param[PAGE_OSC2][0].value * 216;
		if (Connected(Input::CV1)) c.osc1Oct += CVIn1() * 192;
		if (Connected(Input::CV2)) c.osc2Oct += CVIn2() * 192;

		// Rungler depths: 0..5 octaves
		c.run1 = param[PAGE_OSC1][1].value * 80;
		c.run2 = param[PAGE_OSC2][1].value * 80;
		c.runFilt = param[PAGE_FILTER][1].value * 80;

		// Cross FM depths: 0..4 octaves either side
		c.fm1 = param[PAGE_OSC1][2].value * 64;
		c.fm2 = param[PAGE_OSC2][2].value * 64;

		// Filter: 9 octaves from 30 Hz, Audio In 2 is cutoff CV
		c.cutoffOct = param[PAGE_FILTER][0].value * 144;
		if (Connected(Input::Audio2)) c.cutoffOct += AudioIn2() * 192;
		c.res = param[PAGE_FILTER][2].value << 18;

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

		// Left column: current page. Right column: rungler, comparator, LP level
		LedOn(0, page == PAGE_OSC1);
		LedOn(2, page == PAGE_OSC2);
		LedOn(4, page == PAGE_FILTER);
		LedBrightness(1, benj.rungler >> 12);
		LedOn(3, benj.pwm > 0);
		LedBrightness(5, clampi(abs(benj.filter.lp) >> 12, 0, 4095));

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
	Pickup param[NUM_PAGES][3];
	int page = PAGE_OSC1;
	int32_t notDownCount = 0;
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
