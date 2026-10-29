// WDF Benjolin 2 - component-level model of the Benjolin Gorga
// for the Music Thing Modular Workshop System Computer.
//
// The circuit model lives in benjolin_circuit.h; this file maps it onto the
// panel. Core 0 runs the circuit at 48kHz in integer arithmetic; core 1
// turns pot positions into circuit coefficients (floating point) whenever
// a pot moves.

#include "ComputerCard.h"
#include "benjolin_circuit.h"

#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#if WDFB_PROFILE
#include "hardware/structs/systick.h"
#endif

using namespace benji;

// Soft takeover: a parameter only follows its knob once the knob has reached
// (or crossed) the parameter's current value, so changing page never jumps.
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

// Knob pages, stepped through by tapping Z down. Pots are the Gorga's.
//            Main               X                    Y
//   page 1   Osc 1 rate (R54)   Rungler > Osc 1 (R55)  Osc 1 CV / cross mod (R51)
//   page 2   Osc 2 rate (R56)   Rungler > Osc 2 (R57)  Osc 2 CV / cross mod (R52)
//   page 3   Cutoff (R58)       Rungler > filter (R61) Resonance (R59)
//   page 4   Chaos (R60)        Filter CV (R53)        Output level
static const int pagePot[4][3] = {
	{POT_RATE1, POT_RUN1, POT_CV1},
	{POT_RATE2, POT_RUN2, POT_CV2},
	{POT_CUTOFF, POT_RUNF, POT_RES},
	{POT_CHAOS, POT_FCV, -1},
};
static constexpr int NUM_PAGES = 4;

// Workshop System jacks: +/-2048 is about +/-6V
static inline int32_t JackToVolts(int32_t x) { return x * (V1 / 2048 * 6); }
static inline int16_t VoltsToJack(int32_t v)
{
	return (int16_t)Clamp(((v >> 12) * 341) >> 12, -2047, 2047);
}

class WDFBenjolin2 : public ComputerCard
{
public:
	WDFBenjolin2()
	{
		benj.Init();
		benj.rungler.reg = (uint8_t)(UniqueCardID() | 1);

		// Default pot positions (knob units, 0..4095)
		const int32_t init[NUM_POTS] = {
			2600, 1400, 0,	  // osc 1: rate, rungler, CV
			2000, 1000, 0,	  // osc 2
			2400, 1600, 3500, // filter: cutoff, rungler, resonance
			2048, 0,		  // chaos, filter CV
		};
		for (int p = 0; p < NUM_POTS; p++)
		{
			pot[p] = {init[p]};
			target[p] = init[p];
			benj.SetPot(p, init[p] / 4095.0f);
			applied[p] = init[p];
		}
		level = {3300};
	}

	void StartSecondCore()
	{
		instance = this;
		multicore_launch_core1(Core1);
	}

protected:
	void __not_in_flash_func(ProcessSample)() override
	{
#if WDFB_PROFILE
		uint32_t t0 = systick_hw->cvr;
#endif
		int32_t knob[3] = {KnobVal(Knob::Main), KnobVal(Knob::X), KnobVal(Knob::Y)};
		Switch sw = SwitchVal();

		// A tap down on Z steps to the next page (20ms debounce)
		if (sw == Switch::Down)
		{
			if (notDownCount >= 960)
			{
				page = (page + 1) % NUM_PAGES;
				for (int k = 0; k < 3; k++) Param(page, k).Release(knob[k]);
			}
			notDownCount = 0;
		}
		else if (notDownCount < 960)
		{
			notDownCount++;
		}

		for (int k = 0; k < 3; k++) Param(page, k).Update(knob[k]);
		for (int k = 0; k < 3; k++)
			if (pagePot[page][k] >= 0) target[pagePot[page][k]] = Param(page, k).value;

		// SW1 (chaos pot connected): switch up; Pulse In 2 high inverts it
		sw1Want = (sw == Switch::Up) != (Connected(Input::Pulse2) && PulseIn2());

		// Jacks, normalled as on the Benjolin: osc CV inputs to the other
		// triangle, filter CV to triangle 2
		int32_t t1 = benj.osc1.T, t2 = benj.osc2.T;
		int32_t cv1 = Connected(Input::CV1) ? JackToVolts(CVIn1()) : t2;
		int32_t cv2 = Connected(Input::CV2) ? JackToVolts(CVIn2()) : t1;
		int32_t fcv = Connected(Input::Audio2) ? JackToVolts(AudioIn2()) : t2;
		int32_t ext = Connected(Input::Audio1) ? JackToVolts(AudioIn1()) : 0;
		int clk = Connected(Input::Pulse1) ? (PulseIn1RisingEdge() ? 1 : 0) : -1;

		benj.Step(cv1, cv2, fcv, ext, clk);

		int32_t lvl = level.value;
		AudioOut1(VoltsToJack(Mul(benj.filter.outAmp, lvl << 18, 30)));
		AudioOut2(VoltsToJack(Mul(benj.pwm, lvl << 17, 30)));
		CVOut1(VoltsToJack(benj.rungler.cv));
		CVOut2(VoltsToJack(benj.osc2.T));
		PulseOut1(benj.osc1.high);
		PulseOut2(benj.osc2.high);

		// Left column: page (page 4 lights all three). Right column:
		// rungler CV, PWM comparator, filter output level
		LedOn(0, page == 0 || page == 3);
		LedOn(2, page == 1 || page == 3);
		LedOn(4, page == 2 || page == 3);
		LedBrightness(1, Clamp((benj.rungler.cv + Volts(5.25f)) >> 13, 0, 4095));
		LedOn(3, benj.pwm > 0);
		LedBrightness(5, Clamp(abs(benj.filter.outAmp) >> 11, 0, 4095));

#if WDFB_PROFILE
		// SysTick counts down; light LED 5 permanently if a sample ever
		// takes more than 75% of the 48kHz budget
		uint32_t cycles = (t0 - systick_hw->cvr) & 0xFFFFFF;
		if (cycles > maxCycles) maxCycles = cycles;
		if (maxCycles > (uint32_t)(0.75f * 192000000 / 48000)) LedOn(5);
#endif
	}

private:
	Benjolin benj;
	Pickup pot[NUM_POTS];
	Pickup level;
	int page = 0;
	int32_t notDownCount = 0;
#if WDFB_PROFILE
	uint32_t maxCycles = 0;
#endif

	// core 0 -> core 1
	volatile int32_t target[NUM_POTS];
	volatile bool sw1Want = false;
	// core 1 only
	int32_t applied[NUM_POTS];

	static inline WDFBenjolin2 *instance = nullptr;

	Pickup &Param(int pg, int k)
	{
		int p = pagePot[pg][k];
		return p >= 0 ? pot[p] : level;
	}

	static void Core1() { instance->ControlLoop(); }

	// Recompute circuit coefficients when a pot moves. Each coefficient is a
	// single aligned 32-bit store, so core 0 always sees a valid value.
	void ControlLoop()
	{
		bool sw1 = false;
		while (true)
		{
			for (int p = 0; p < NUM_POTS; p++)
			{
				int32_t t = target[p];
				if (t - applied[p] > 1 || applied[p] - t > 1)
				{
					benj.SetPot(p, t / 4095.0f);
					applied[p] = t;
				}
			}
			bool want = sw1Want;
			if (want != sw1)
			{
				benj.SetSW1(want);
				sw1 = want;
			}
			sleep_us(500);
		}
	}
};

int main()
{
	// 192MHz: a multiple of 48kHz (no ADC aliasing), officially supported at 1.15V
	vreg_set_voltage(VREG_VOLTAGE_1_15);
	set_sys_clock_khz(192000, true);

#if WDFB_PROFILE
	systick_hw->rvr = 0xFFFFFF;
	systick_hw->csr = 0x5; // enable, processor clock
#endif

	static WDFBenjolin2 card;
	card.EnableNormalisationProbe();
	card.StartSecondCore();
	card.Run();
}
