// nn_wavedigiHypster - a wave digital filter model of Ian Fritz's Hypster
// fourth-order hyperchaos generator, for the Music Thing Modular Workshop
// System Computer.
//
// Core 0 (48kHz audio interrupt): panel, paging, and the fixed-point WDF
// core (hypster_core.h). Core 1: re-solves the Shape network WDF into a
// lookup table whenever the Shape controls move.
//
// Panel (see README.md for the full story):
//   Z down, tap       next page (1 -> 2 -> 3 -> 4 -> 1)
//   Z down, hold 1s   back to page 1
//   Z up              limiter diodes D2-D4 fitted ; Z middle: not fitted
//   page 1  Main Rate        X Gain           Y Damping
//   page 2  Main Fat..Slim   X Shape mode     Y Gain trim
//   page 3  Main Rate CV amt X Gain CV amt    Y Damping CV amt
//   page 4  Main Out level   X Output map     Y Rate fine
//   CV In 1 Rate (1V/oct at full amount)   CV In 2 Gain mod
//   Audio In 1 Damping mod                 Audio In 2 Shape (fat..slim) CV
//   Pulse In 1 Kick (re-seed)              Pulse In 2 Hold (freeze while high)
//   Audio Out 1/2, CV Out 1/2: x y u z (by output map) ; Pulse Outs: sign of Audio Outs

#include "ComputerCard.h"
#include "hypster_core.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"

using hyp::ShapeMode;

// ------------------------------------------------------------ shared state
static hyp::ShaperTables gTabs;
static hyp::ShapeTables gShape[2];
static volatile int32_t gShapeFront = 0;          // table the audio core reads
static volatile int32_t gShapeRequest = 0;        // (mode << 8) | pot (0..127), from core 0
static volatile int32_t gShapeBuilt = -1;         // request the front table holds

static void BuildShape(hyp::ShapeTables &t, int32_t request)
{
	hyp::BuildShapeTables(t, (ShapeMode)(request >> 8), (float)(request & 0xFF) / 127.0f);
}

// Core 1: keep the Shape table in step with the Shape controls.
static void Core1Main()
{
	while (true)
	{
		const int32_t req = gShapeRequest;
		if (req != gShapeBuilt)
		{
			const int32_t back = 1 - gShapeFront;
			BuildShape(gShape[back], req);
			gShapeBuilt = req;
			__dmb();
			gShapeFront = back;
		}
		else
		{
			sleep_us(500);
		}
	}
}

// ------------------------------------------------------------ card state
// Kept outside the ComputerCard object (so the cycle bench can drive
// ProcessSample with a stand-in card).
static constexpr int kPages = 4;
struct CardState
{
	hyp::Core core;
	int32_t param[kPages][3];
	bool caught[3];
	int32_t entry[3]; // sign of (knob - param) on entering the page
	int page;
	int32_t downCount;
	bool homed;
	int32_t pageFlash;
	uint32_t tick;
	uint32_t seed;
	bool limiter;
	int32_t lastShapeMode, lastShapePot;
	bool pulseState[2];

	void Init()
	{
		core.Init(&gTabs, &gShape[0]);
		// page defaults (0..4095); page 1 follows the knobs from power on
		const int32_t defaults[kPages][3] = {
			{2048, 2048, 2048}, // Rate, Gain, Damping
			{2048, 0, 2048},    // Fat..Slim, Shape mode, Gain trim (centre = 0V)
			{4095, 2048, 2048}, // Rate CV 1V/oct, Gain CV, Damping CV (centre = off)
			{2048, 0, 2048},    // Out level 1x, Output map, Rate fine 0
		};
		for (int p = 0; p < kPages; p++)
			for (int k = 0; k < 3; k++) param[p][k] = defaults[p][k];
		for (int k = 0; k < 3; k++)
		{
			caught[k] = true;
			entry[k] = 1;
		}
		page = 0;
		downCount = 0;
		homed = false;
		pageFlash = 0;
		tick = 0;
		seed = 0x2545F491u;
		limiter = false;
		lastShapeMode = 0;
		lastShapePot = 0;
		pulseState[0] = pulseState[1] = false;
	}
};
static CardState gS;

// ------------------------------------------------------------ the card
class Hypster : public ComputerCard
{
public:
	Hypster()
	{
		EnableNormalisationProbe();
		gS.Init();
	}

protected:
	void __not_in_flash_func(ProcessSample)() override
	{
		Ui();
		Controls();
		gS.core.Process();
		Outputs();
		gS.tick++;
	}

private:
	static constexpr int32_t kTapSamples = 48000 * 6 / 10;  // < 0.6s is a tap
	static constexpr int32_t kHomeSamples = 48000;          // 1s hold -> page 1

	// ---------------------------------------------------------------- panel
	void Ui()
	{
		// Z switch: Down is momentary (tap = next page, hold = home), Up = limiter
		const Switch sw = SwitchVal();
		if (sw == Switch::Down)
		{
			gS.downCount++;
			if (gS.downCount == kHomeSamples && gS.page != 0)
			{
				SetPage(0);
				gS.homed = true;
			}
		}
		else
		{
			if (gS.downCount > 0 && gS.downCount < kTapSamples && !gS.homed) SetPage((gS.page + 1) % kPages);
			gS.downCount = 0;
			gS.homed = false;
		}
		gS.limiter = (sw == Switch::Up);

		// knobs with soft takeover
		static const Knob knobs[3] = {Knob::Main, Knob::X, Knob::Y};
		for (int k = 0; k < 3; k++)
		{
			const int32_t v = KnobVal(knobs[k]);
			int32_t &p = gS.param[gS.page][k];
			if (!gS.caught[k])
			{
				const int32_t d = v - p;
				const int32_t s = d > 0 ? 1 : -1;
				if (d < 48 && d > -48) gS.caught[k] = true;
				else if (s != gS.entry[k]) gS.caught[k] = true; // passed through it
			}
			if (gS.caught[k]) p = v;
		}

		// Pulse In 1: kick ; Pulse In 2: hold (read in Controls)
		if (PulseIn1RisingEdge()) gS.core.Kick(NextRand());
	}

	void SetPage(int p)
	{
		gS.page = p;
		for (int k = 0; k < 3; k++)
		{
			gS.caught[k] = false;
			static const Knob knobs[3] = {Knob::Main, Knob::X, Knob::Y};
			gS.entry[k] = KnobVal(knobs[k]) - gS.param[gS.page][k] > 0 ? 1 : -1;
		}
		gS.pageFlash = 48000;
	}

	uint32_t NextRand()
	{
		gS.seed ^= gS.seed << 13;
		gS.seed ^= gS.seed >> 17;
		gS.seed ^= gS.seed << 5;
		return gS.seed;
	}

	// ---------------------------------------------------------------- controls
	// CV inputs read about 341 counts per volt (+/-6V full scale)
	int32_t In(Input i, int32_t value) { return Connected(i) ? value : 0; }

	// counts (341/V) x amount (0..4095, centre 2048 = 0) -> Q16 volts or octaves
	static int32_t Amount(int32_t counts, int32_t amt)
	{
		// 65536 / (341.33 * 2048) = 3/32
		return (counts * (amt - 2048) * 3) >> 5;
	}

	void Controls()
	{
		// rate: every sample, so CV In 1 can modulate at audio rate
		const int32_t *p1 = gS.param[0], *p2 = gS.param[1], *p3 = gS.param[2], *p4 = gS.param[3];
		int32_t rate = (p1[0] << 8)                                   // 16 octaves over the knob
		               + ((p4[2] - 2048) << 5)                        // +/-1 octave fine
		               + Amount(In(Input::CV1, CVIn1()), p3[0]);      // 1V/oct at full CW
		gS.core.UpdateRate(rate);

		if ((gS.tick & 7) != 0) return;

		// gain / damping / trim: A5 and A6 converter inputs, Q16 volts
		hyp::Controls c;
		c.rateOct = rate;
		c.gainV = p1[1] * 192 + Amount(In(Input::CV2, CVIn2()), p3[1]);   // 0..12V
		c.dampV = p1[2] * 192 + Amount(In(Input::Audio1, AudioIn1()), p3[2]);
		c.trimV = ((p2[2] - 2048) * 2458) >> 6;                          // +/-1.2V
		c.limiter = gS.limiter;
		c.hold = PulseIn2();
		gS.core.UpdateCurrents(c);

		// Shape: pot position with Audio In 2 (+/-5V sweeps the whole pot)
		int32_t pot = p2[0] + ((In(Input::Audio2, AudioIn2()) * 12) >> 2);
		if (pot < 0) pot = 0;
		if (pot > 4095) pot = 4095;
		const int32_t mode = (p2[1] * (int32_t)ShapeMode::Count) >> 12;
		// a little hysteresis so a still knob does not keep core 1 busy
		if (mode != gS.lastShapeMode || pot > gS.lastShapePot + 40 || pot < gS.lastShapePot - 40)
		{
			gS.lastShapeMode = mode;
			gS.lastShapePot = pot;
			gShapeRequest = (mode << 8) | (pot >> 5); // pot 0..127
		}
		gS.core.SetShape(&gShape[gShapeFront]);
	}

	// ---------------------------------------------------------------- outputs
	void Outputs()
	{
		const int32_t u = gS.core.U(), z = gS.core.Z(), y = gS.core.Y(), x = gS.core.X();
		int32_t o[4];
		switch ((gS.param[3][1] * 4) >> 12)
		{
		case 0: o[0] = x; o[1] = y; o[2] = u; o[3] = z; break; // the x-y attractor on the audio outs
		case 1: o[0] = x; o[1] = z; o[2] = y; o[3] = u; break; // asymmetric pairs
		case 2: o[0] = u; o[1] = z; o[2] = x; o[3] = y; break;
		default: o[0] = y; o[1] = u; o[2] = x; o[3] = z; break;
		}
		// level 0..2x (param / 2048) ; 1x maps the op-amp swing (+/-10.8V)
		// onto the full output range (+/-6V):
		//   counts = v / 2^24 * param / 2048 * 2047 / 10.8 ~ v * param * 379 / 2^36
		// (in 32-bit steps: v to Q12 first)
		const int32_t level = gS.param[3][0];
		int32_t c[4];
		for (int i = 0; i < 4; i++)
		{
			int32_t v = ((((o[i] >> 12) * level) >> 8) * 379) >> 16;
			if (v > 2047) v = 2047;
			if (v < -2048) v = -2048;
			c[i] = v;
		}
		AudioOut1(c[0]);
		AudioOut2(c[1]);
		CVOut1(c[2]);
		CVOut2(c[3]);

		// comparators with a little hysteresis
		for (int i = 0; i < 2; i++)
		{
			if (o[i] > (1 << 22)) gS.pulseState[i] = true;
			else if (o[i] < -(1 << 22)) gS.pulseState[i] = false;
		}
		PulseOut1(gS.pulseState[0]);
		PulseOut2(gS.pulseState[1]);

		if ((gS.tick & 31) == 4) Leds(c); // off the control-update samples
	}

	void Leds(const int32_t *c)
	{
		if (gS.pageFlash > 0) gS.pageFlash -= 32;
		// on pages 2-4 the page number blinks briefly every 2 seconds
		const bool flash = gS.pageFlash > 0 || (gS.page != 0 && (gS.tick % 96000) < 9600);
		if (flash)
		{
			for (int i = 0; i < 6; i++) LedBrightness(i, i <= gS.page ? 4095 : 0);
			return;
		}
		for (int i = 0; i < 4; i++)
		{
			const int32_t m = c[i] < 0 ? -c[i] : c[i];
			LedBrightness(i, (uint16_t)(m * 2 > 4095 ? 4095 : m * 2));
		}
		LedOn(4, gS.pulseState[0]);
		LedOn(5, gS.pulseState[1]);
	}
};

#ifdef HYPSTER_BENCH
// Cycle bench (tools/bench): the emulator has no boot ROM (where the float
// routines live), so the bench writes host-built tables into gTabs/gShape and
// then calls this.
extern "C" __attribute__((used)) void bench_init()
{
	gS.Init();
}
extern "C"
{
__attribute__((used)) void *bench_tables[] = {&gTabs, &gShape[0], &gShape[1]};
}
#endif

int main()
{
	// 192MHz (a multiple of 48MHz, as ComputerCard recommends) with the core
	// voltage raised a step; the card runs from RAM (copy_to_ram).
	vreg_set_voltage(VREG_VOLTAGE_1_15);
	busy_wait_us_32(1000);
	set_sys_clock_khz(192000, true);

	// Solve the memoryless WDF networks into tables before any sound.
	hyp::BuildShaperTables(gTabs);
	BuildShape(gShape[0], 0);
	gShapeBuilt = 0;

	// On the stack, not a static local (that would pull in guard code)
	Hypster card;
	multicore_launch_core1(Core1Main);
	card.Run();
}
