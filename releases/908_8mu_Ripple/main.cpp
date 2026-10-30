// 8mu Ripple
//
// Noise-clocked ripple divider synth, played from the faders of an 8mu.
//
// A square-wave clock steps a shift-register noise source. The rising edges of
// the noise bit drive a binary counter, whose outputs (/2 to /32) are mixed
// with the raw bit (/1), and the mix is fed back to the clock pitch and
// through a resonant filter. Filter cutoff is modulated by the clock itself.
//
//   Main knob        - clock pitch (+ CV in 1, ~1V/oct)
//   Knob X           - filter cutoff (+ CV in 2, ~1V/oct)
//   Knob Y           - filter resonance
//   Switch           - up: lowpass, middle: bandpass, down (momentary): reset
//   Audio in 1       - replaces the noise bit with a comparator on this input
//   Audio in 2       - exponential FM of the clock
//   Pulse in 1       - external clock for the noise register
//   Pulse in 2       - reset: reseed the register and clear the counter
//   Audio out 1/2    - left/right
//   CV out 1         - random voltage, new value each counter cycle
//   CV out 2         - feedback (noise mod) signal
//   Pulse out 1      - trigger each counter cycle
//   Pulse out 2      - raw noise bit (/1)
//
// 8mu faders have four pages. Hold an 8mu button to reach a page; faders on a
// newly selected page do nothing until moved through their stored value.
//
//   No button  Play:   /1 /2 /4 /8 /16 /32 levels, noise mod, filter mod
//   Button 1   Noise:  LFSR length, noise colour, hiss level, hiss colour,
//                      drive, filter mod shape, feedback slew, key tracking
//   Button 2   Divide: pan of /1 to /32, count length, edge mode
//   Button 3   Motion: tilt->pitch, roll->cutoff, rotate->noise mod, glide,
//                      -, -, -, volume
//   Button 4   tap to toggle loop lock (freeze the register pattern)


#include "ComputerCard.h"
#include "EightMU.h"
#include "RippleEngine.h"

using ripple::Clip;


static constexpr int numPages = 4;
static constexpr int numFaders = 8;
static constexpr int controlInterval = 8;
static constexpr int32_t pickupWindow = 48;


////////////////////////////////////////////////////////////////////////////////
// Fader page with soft takeover
//
// Each page holds its own values. When a page is selected, its faders are not
// live until they reach, or cross, the stored value, so changing page never
// makes a parameter jump.

struct Page
{
	int32_t value[numFaders];
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
		for (int i = 0; i < ripple::numTaps; i++)
		{
			pages[0].value[i] = d.level[i];
			pages[2].value[i] = d.pan[i];
		}
		pages[0].value[6] = d.noiseMod;
		pages[0].value[7] = d.filterMod;

		int32_t noise[numFaders] = {d.length, d.colour, d.hiss, d.hissColour,
		                            d.drive, d.modShape, d.fbSlew, d.keyTrack};
		for (int i = 0; i < numFaders; i++)
		{
			pages[1].value[i] = noise[i];
			pages[3].value[i] = 0;
		}
		pages[2].value[6] = d.countLength;
		pages[2].value[7] = d.edges;
		pages[3].value[3] = d.glide;
		pages[3].value[7] = d.volume;

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

		// Page selection: lowest-numbered held button wins
		int page = 0;
		if (have8mu)
		{
			for (int b = 0; b < 3; b++)
			{
				if (mu.Button(b))
				{
					page = b + 1;
					break;
				}
			}
		}
		if (have8mu && (!had8mu || page != currentPage))
		{
			pages[page].Select(mu);
		}
		had8mu = have8mu;
		currentPage = page;
		if (have8mu)
		{
			pages[page].Update(mu);
		}

		// Button 4 toggles loop lock
		bool b4 = have8mu && mu.Button(3);
		if (b4 && !lastButton4)
		{
			freeze = !freeze;
		}
		lastButton4 = b4;

		// Switch: up lowpass, middle bandpass, down (momentary) reset
		Switch sw = SwitchVal();
		if (sw == Up)
		{
			bandpass = false;
		}
		else if (sw == Middle)
		{
			bandpass = true;
		}
		else if (SwitchChanged())
		{
			resetPending = true;
		}

		const int32_t *play = pages[0].value;
		const int32_t *noise = pages[1].value;
		const int32_t *divide = pages[2].value;
		const int32_t *motion = pages[3].value;

		ripple::Params p;
		for (int i = 0; i < ripple::numTaps; i++)
		{
			p.level[i] = play[i];
			p.pan[i] = divide[i];
		}
		p.noiseMod = play[6];
		p.filterMod = play[7];

		p.length = noise[0];
		p.colour = noise[1];
		p.hiss = noise[2];
		p.hissColour = noise[3];
		p.drive = noise[4];
		p.modShape = noise[5];
		p.fbSlew = noise[6];
		p.keyTrack = noise[7];

		p.countLength = divide[6];
		p.edges = divide[7];

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

		// Knob X: 20Hz to ~16kHz, CV in 2 at ~1V/oct
		p.cutoff = (KnobVal(X) * ripple::maxCutoff) >> 12;
		cutoffMod = CVIn2() * 12 + roll * 4;

		p.resonance = KnobVal(Y);
		p.bandpass = bandpass;
		p.freeze = freeze;

		engine.Update(p);

		Leds(have8mu);
	}

	void __not_in_flash_func(Leds)(bool have8mu)
	{
		// Computer LEDs: /1 to /32
		for (int i = 0; i < ripple::numTaps; i++)
		{
			LedOn(i, lastOut.tap[i]);
		}

		if (!have8mu)
		{
			return;
		}

		if (currentPage == 0)
		{
			// Tap states, brighter for louder taps, then clock and loop lock
			for (int i = 0; i < ripple::numTaps; i++)
			{
				int32_t lvl = pages[0].value[i];
				mu.SetLed(i, lastOut.tap[i] ? 512 + ((lvl * 7) >> 3) : 0);
			}
			mu.SetLed(6, lastOut.clockHigh ? 4095 : 0);
			mu.SetLed(7, freeze ? 4095 : 0);
		}
		else
		{
			// Held page: lit faders are live, blinking ones wait for pickup
			if (++blinkCounter >= 3000)
			{
				blinkCounter = 0;
			}
			bool blink = blinkCounter < 1500;
			for (int i = 0; i < numFaders; i++)
			{
				bool used = (currentPage != 3) || (i < 4) || (i == 7);
				int32_t v = 0;
				if (used)
				{
					v = pages[currentPage].live[i] ? 4095 : (blink ? 800 : 0);
				}
				mu.SetLed(i, v);
			}
		}
	}

	EightMU mu;
	ripple::Engine engine;
	ripple::Outputs lastOut;
	Page pages[numPages];
	int currentPage = 0;
	bool had8mu = false;
	bool lastButton4 = false;
	bool freeze = false;
	bool bandpass = false;
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
