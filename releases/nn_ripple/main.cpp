// 8mu Ripple
//
// Noise-clocked divider synth, played from the faders of an 8mu.
//
// A square-wave clock steps a shift-register noise source. The rising edges of
// the noise bit drive five dividers (binary /2 to /32 by default, or primes),
// whose outputs and logic combinations are mixed with the raw bit (/1). The mix
// is fed back to the clock pitch and through a resonant filter. Filter cutoff
// is modulated by the clock itself.
//
//   Main knob        - clock pitch (+ CV in 1, ~1V/oct)
//   Knob X           - filter cutoff (+ CV in 2, ~1V/oct)
//   Knob Y           - filter resonance / comb feedback
//   Switch           - up: filter type A, middle: filter type B,
//                      down (momentary): reset
//   Audio in 1       - replaces the noise bit with a comparator on this input
//   Audio in 2       - exponential FM of the clock
//   Pulse in 1       - external clock for the noise register
//   Pulse in 2       - reset: reseed the register and clear the dividers
//   Audio out 1/2    - left/right
//   CV out 1         - random voltage, new value each counter cycle
//   CV out 2         - feedback (noise mod) signal
//   Pulse out 1      - trigger each counter cycle
//   Pulse out 2      - raw noise bit (/1)
//
// 8mu fader pages, selected by tapping buttons 1-3. Tapping a button again
// steps through the pages in its group. Faders on a newly selected page do
// nothing until moved through their stored value.
//
//   Button 1  Play:   /1 /2 /4 /8 /16 /32 levels, noise mod, filter mod
//             Logic:  levels of logic A-D, functions of logic A-D
//   Button 2  Noise:  LFSR length, noise colour, hiss level, hiss colour,
//                     feedback slew
//             Filter: type A, type B, comb damping, filter mod shape,
//                     key tracking, drive, clock tune
//   Button 3  Divide: pan of /1 to /32, logic spread
//             Ratios: ratio of each divider, count length, edge mode
//             Motion: tilt->pitch, roll->cutoff, rotate->noise mod, glide,
//                     volume
//   Button 4  tap to toggle loop lock (freeze the register pattern)


#include "ComputerCard.h"
#include "EightMU.h"
#include "RippleEngine.h"

using ripple::Clip;
using ripple::ListValue;


static constexpr int numFaders = 8;
static constexpr int controlInterval = 8;
static constexpr int32_t pickupWindow = 48;

enum PageId
{
	PagePlay,
	PageLogic,
	PageNoise,
	PageFilter,
	PageDivide,
	PageRatios,
	PageMotion,
	numPages
};

// Pages reached by each of buttons 1-3, in the order repeated taps step
// through them
static constexpr int numGroups = 3;
static constexpr int groupFirst[numGroups] = {PagePlay, PageNoise, PageDivide};
static constexpr int groupLast[numGroups] = {PageLogic, PageFilter, PageMotion};

// Faders that do something, per page (bit i = fader i+1)
static constexpr uint8_t usedFaders[numPages] = {
	0xFF, // Play
	0xFF, // Logic
	0x1F, // Noise
	0x7F, // Filter
	0x7F, // Divide
	0xDF, // Ratios
	0x8F, // Motion
};

// LED page indicator time after a page change, in control steps (6kHz)
static constexpr int pageFlashTime = 3000;


////////////////////////////////////////////////////////////////////////////////
// Fader page with soft takeover
//
// Each page holds its own values. When a page is selected, its faders are not
// live until they reach, or cross, the stored value, so changing page never
// makes a parameter jump.

struct Page
{
	int32_t value[numFaders] = {};
	bool live[numFaders] = {};
	int8_t side[numFaders] = {};

	void Select(const EightMU &mu)
	{
		for (int i = 0; i < numFaders; i++)
		{
			int32_t diff = mu.Fader(i) - value[i];
			live[i] = (diff > -pickupWindow && diff < pickupWindow);
			side[i] = (diff > 0) ? 1 : -1;
		}
	}

	void Update(const EightMU &mu)
	{
		for (int i = 0; i < numFaders; i++)
		{
			int32_t f = mu.Fader(i);
			if (!live[i])
			{
				int32_t diff = f - value[i];
				int8_t s = (diff > 0) ? 1 : -1;
				if ((diff > -pickupWindow && diff < pickupWindow) || s != side[i])
				{
					live[i] = true;
				}
			}
			if (live[i])
			{
				// 8mu faders read 0 to 4064
				value[i] = Clip(f + (f >> 7), 0, 4095);
			}
		}
	}
};


class EightMURipple : public ComputerCard
{
public:
	EightMURipple()
	{
		// Default fader values, from the engine's defaults
		ripple::Params d;
		int32_t *v;

		v = pages[PagePlay].value;
		for (int i = 0; i < ripple::numTaps; i++)
		{
			v[i] = d.level[i];
		}
		v[6] = d.noiseMod;
		v[7] = d.filterMod;

		v = pages[PageLogic].value;
		for (int i = 0; i < ripple::numLogic; i++)
		{
			v[i] = d.logicLevel[i];
			v[4 + i] = d.logicFunc[i];
		}

		v = pages[PageNoise].value;
		v[0] = d.length;
		v[1] = d.colour;
		v[2] = d.hiss;
		v[3] = d.hissColour;
		v[4] = d.fbSlew;

		v = pages[PageFilter].value;
		v[0] = ListValue(ripple::FilterLP, ripple::numFilterTypes);
		v[1] = ListValue(ripple::FilterBP, ripple::numFilterTypes);
		v[2] = d.combDamp;
		v[3] = d.modShape;
		v[4] = d.keyTrack;
		v[5] = d.drive;
		v[6] = d.clockTune;

		v = pages[PageDivide].value;
		for (int i = 0; i < ripple::numTaps; i++)
		{
			v[i] = d.pan[i];
		}
		v[6] = d.logicSpread;

		v = pages[PageRatios].value;
		for (int i = 0; i < ripple::numDividers; i++)
		{
			v[i] = d.ratio[i];
		}
		v[6] = d.countLength;
		v[7] = d.edges;

		v = pages[PageMotion].value;
		v[3] = d.glide;
		v[7] = d.volume;

		EnableNormalisationProbe();

		// Give the USB power circuitry time to settle, then only start the
		// USB host stack if this really is a host port.
		sleep_us(150000);
		if (USBPowerState() == DFP)
		{
			mu.Start(); // claims core1
		}
	}

	virtual void __not_in_flash_func(ProcessSample)()
	{
		if (++controlCounter >= controlInterval)
		{
			controlCounter = 0;
			Control();
		}

		ripple::Inputs in;
		in.useExtClock = Connected(Input::Pulse1);
		in.extClock = PulseIn1RisingEdge();
		in.reset = PulseIn2RisingEdge() || resetPending;
		resetPending = false;
		in.useAudioSource = Connected(Input::Audio1);
		in.audioSource = AudioIn1();
		in.fm = Connected(Input::Audio2) ? AudioIn2() * 4 : 0; // +-2 octaves
		in.cutoffMod = cutoffMod;

		ripple::Outputs out = engine.Process(in);

		AudioOut1(out.left);
		AudioOut2(out.right);
		CVOut1(out.randomCV);
		CVOut2(out.modCV);
		PulseOut1(out.cyclePulse);
		PulseOut2(out.tap[0]);

		lastOut = out;
	}

private:
	// Control-rate work, at 6kHz
	void __not_in_flash_func(Control)()
	{
		bool have8mu = mu.Connected();

		// Buttons 1-3: a tap selects that button's group of pages, and further
		// taps step through the group
		int page = currentPage;
		for (int b = 0; b < numGroups; b++)
		{
			bool down = have8mu && mu.Button(b);
			if (down && !lastButton[b])
			{
				if (page >= groupFirst[b] && page < groupLast[b])
				{
					page++;
				}
				else
				{
					page = groupFirst[b];
				}
			}
			lastButton[b] = down;
		}
		if (have8mu && (!had8mu || page != currentPage))
		{
			pages[page].Select(mu);
			pageFlash = pageFlashTime;
		}
		had8mu = have8mu;
		currentPage = page;
		if (have8mu)
		{
			pages[page].Update(mu);
		}

		// Button 4 toggles loop lock
		bool b4 = have8mu && mu.Button(3);
		if (b4 && !lastButton[3])
		{
			freeze = !freeze;
		}
		lastButton[3] = b4;

		const int32_t *play = pages[PagePlay].value;
		const int32_t *logic = pages[PageLogic].value;
		const int32_t *noise = pages[PageNoise].value;
		const int32_t *filter = pages[PageFilter].value;
		const int32_t *divide = pages[PageDivide].value;
		const int32_t *ratios = pages[PageRatios].value;
		const int32_t *motion = pages[PageMotion].value;

		// Switch: up filter type A, middle filter type B, down (momentary) reset
		Switch sw = SwitchVal();
		if (sw == Up)
		{
			typeB = false;
		}
		else if (sw == Middle)
		{
			typeB = true;
		}
		else if (SwitchChanged())
		{
			resetPending = true;
		}

		ripple::Params p;
		for (int i = 0; i < ripple::numTaps; i++)
		{
			p.level[i] = play[i];
			p.pan[i] = divide[i];
		}
		p.noiseMod = play[6];
		p.filterMod = play[7];

		for (int i = 0; i < ripple::numLogic; i++)
		{
			p.logicLevel[i] = logic[i];
			p.logicFunc[i] = logic[4 + i];
		}

		p.length = noise[0];
		p.colour = noise[1];
		p.hiss = noise[2];
		p.hissColour = noise[3];
		p.fbSlew = noise[4];

		int32_t typeValue = typeB ? filter[1] : filter[0];
		p.filterType = Clip((typeValue * ripple::numFilterTypes) >> 12, 0, ripple::numFilterTypes - 1);
		p.combDamp = filter[2];
		p.modShape = filter[3];
		p.keyTrack = filter[4];
		p.drive = filter[5];
		p.clockTune = filter[6];

		p.logicSpread = divide[6];

		for (int i = 0; i < ripple::numDividers; i++)
		{
			p.ratio[i] = ratios[i];
		}
		p.countLength = ratios[6];
		p.edges = ratios[7];

		p.glide = motion[3];
		p.volume = motion[7];

		// 8mu motion: tilt --> pitch (+-1.5 oct), roll --> cutoff (+-2 oct),
		// rotation --> extra noise mod
		int32_t tilt = 0, roll = 0, yaw = 0;
		if (have8mu)
		{
			tilt = (mu.Pitch() * motion[0]) >> 12;
			roll = (mu.Roll() * motion[1]) >> 12;
			yaw = mu.Yaw();
			yaw = ((yaw < 0 ? -yaw : yaw) * motion[2]) >> 11;
		}
		p.noiseMod = Clip(p.noiseMod + yaw, 0, 4095);

		// Main knob: ~0.5Hz to ~32kHz, CV in 1 at ~1V/oct (341 counts per volt)
		p.pitch = KnobVal(Main) * 16 - 4096 + CVIn1() * 12 + tilt * 3;

		// Knob X: 20Hz to ~16kHz, or with clock tune, -4 to +4 octaves from
		// the clock. CV in 2 at ~1V/oct.
		int32_t kx = KnobVal(X);
		p.cutoff = (kx * ripple::maxCutoff) >> 12;
		p.cutoffOffset = (kx - 2048) * 8;
		cutoffMod = CVIn2() * 12 + roll * 4;

		p.resonance = KnobVal(Y);
		p.freeze = freeze;

		engine.Update(p);

		Leds(have8mu);
	}

	void __not_in_flash_func(Leds)(bool have8mu)
	{
		// Computer LEDs: /1 to the fifth divider
		for (int i = 0; i < ripple::numTaps; i++)
		{
			LedOn(i, lastOut.tap[i]);
		}

		if (!have8mu)
		{
			return;
		}

		if (++blinkCounter >= 3000)
		{
			blinkCounter = 0;
		}
		bool blink = blinkCounter < 1500;

		if (pageFlash > 0)
		{
			// Just changed page: light the LED numbered by the page
			pageFlash--;
			for (int i = 0; i < numFaders; i++)
			{
				mu.SetLed(i, (i == currentPage) ? 4095 : 0);
			}
		}
		else if (currentPage == PagePlay)
		{
			// Tap states, brighter for louder taps, then clock and loop lock
			for (int i = 0; i < ripple::numTaps; i++)
			{
				int32_t lvl = pages[PagePlay].value[i];
				mu.SetLed(i, lastOut.tap[i] ? 512 + ((lvl * 7) >> 3) : 0);
			}
			mu.SetLed(6, lastOut.clockHigh ? 4095 : 0);
			mu.SetLed(7, freeze ? 4095 : 0);
		}
		else
		{
			// Other pages: lit faders are live, blinking ones wait for pickup
			const Page &pg = pages[currentPage];
			for (int i = 0; i < numFaders; i++)
			{
				int32_t v = 0;
				if (usedFaders[currentPage] & (1 << i))
				{
					v = pg.live[i] ? 4095 : (blink ? 800 : 0);
				}
				mu.SetLed(i, v);
			}
		}
	}

	EightMU mu;
	ripple::Engine engine;
	ripple::Outputs lastOut;
	Page pages[numPages];
	int currentPage = PagePlay;
	int pageFlash = 0;
	bool had8mu = false;
	bool lastButton[4] = {};
	bool freeze = false;
	bool typeB = false;
	bool resetPending = false;
	int32_t cutoffMod = 0;
	int controlCounter = 0;
	int blinkCounter = 0;
};


int main()
{
	set_sys_clock_khz(240000, true);

	static EightMURipple card;
	card.Run();
}
