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
//
// USB, chosen once at power-up
//   Port supplying power (an 8mu, or nothing yet): USB host, reading the 8mu.
//   Computer plugged in: USB MIDI device called "Ripple", for the web editor
//   (protocol in sysex.h), which passes on any 8mus plugged into the
//   computer. Either way the card runs on its own.


// First, so TinyUSB is configured for host and device modes before
// EightMU.h supplies its host-only defaults
#include "tusb_config.h"
#include "ComputerCard.h"
#include "EightMU.h"
#include "sysex.h"
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
static_assert(numPages == sysex::kPages, "pages match the protocol");

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

// The web editor's STATUS is built every this many control steps (~30Hz)
static constexpr int statusInterval = 200;
// and the sources are sampled for it every this many samples (1ms)
static constexpr int bitSampleInterval = 48;


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


class EightMURipple;
static EightMURipple *gCard = nullptr;


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

		// Give the USB power circuitry time to settle, then pick the USB
		// mode once: host if the port is supplying power (an 8mu, or nothing
		// yet), device if a computer is (the web editor).  Boards older than
		// Rev 1.1 can't tell, and are always a device.
		sleep_us(150000);
		gCard = this;
		EnableNormalisationProbe();
		hostMode = USBPowerState() == DFP;
		multicore_launch_core1(hostMode ? Core1Host : Core1Device);
	}

	virtual void __not_in_flash_func(ProcessSample)()
	{
		uint32_t startUs = time_us_32();

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

		if (out.cyclePulse && !lastOut.cyclePulse && cycles < 127)
		{
			cycles++;
		}
		lastOut = out;

		if (!hostMode)
		{
			Observe(out);
		}

		uint32_t us = time_us_32() - startUs;
		if (us > busiestUs) busiestUs = us;
		if (++busySamples >= 48000)
		{
			busySamples = 0;
			shownBusiestUs = busiestUs;
			busiestUs = 0;
		}
	}

private:
	// Control-rate work, at 6kHz
	void __not_in_flash_func(Control)()
	{
		bool have8mu = hostMode && mu.Connected();

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
		filterType = p.filterType;
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

		// 8mu motion, from the 8mu on the card or one passed on by the web
		// editor: tilt --> pitch (+-1.5 oct), roll --> cutoff (+-2 oct),
		// rotation --> extra noise mod
		int32_t mPitch = 0, mRoll = 0, mYaw = 0;
		if (have8mu)
		{
			mPitch = mu.Pitch();
			mRoll = mu.Roll();
			mYaw = mu.Yaw();
		}
		else if (!hostMode)
		{
			mPitch = webPitch;
			mRoll = webRoll;
			mYaw = webYaw;
		}
		int32_t tilt = (mPitch * motion[0]) >> 12;
		int32_t roll = (mRoll * motion[1]) >> 12;
		int32_t yaw = ((mYaw < 0 ? -mYaw : mYaw) * motion[2]) >> 11;
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

		if (WebLinked())
		{
			TrackStatus();
		}
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

	//------------------------------------------------------------------------
	// Watching the engine for the web editor (device mode only)
	//------------------------------------------------------------------------

	// Per sample: source bits once a millisecond, a scope capture of Audio
	// Out 1, and output peaks
	void __not_in_flash_func(Observe)(const ripple::Outputs &out)
	{
		if (++bitSampleCounter >= bitSampleInterval)
		{
			bitSampleCounter = 0;
			if (bitCount < sysex::kMaxBits)
			{
				bitAcc[bitCount++] = out.bits;
			}
		}

		int32_t aL = out.left < 0 ? -out.left : out.left;
		int32_t aR = out.right < 0 ? -out.right : out.right;
		if (aL > peakL) peakL = aL;
		if (aR > peakR) peakR = aR;

		// Scope: wait (up to ~40ms) for a rising zero crossing, then take
		// every second sample
		if (scopeState == ScopeArmed)
		{
			bool rising = lastScopeIn < 0 && out.left >= 0;
			lastScopeIn = out.left;
			if (rising || ++scopeWait > 2000)
			{
				scopeState = ScopeCapturing;
				scopeN = 0;
				scopeWait = 0;
				scopeSkip = 0;
			}
		}
		if (scopeState == ScopeCapturing && (scopeSkip++ & 1) == 0)
		{
			scopeBuf[scopeN++] = uint8_t(Clip((out.left + 2048) >> 5, 0, 127));
			if (scopeN >= sysex::kScopeLen)
			{
				scopeState = ScopeFull;
			}
		}
	}

	// Per control step: the range of pitch and cutoff, and every ~33ms a new
	// STATUS
	void __not_in_flash_func(TrackStatus)()
	{
		int32_t pe = (lastOut.pitch + 32768) >> 3;
		int32_t ce = lastOut.cutoff >> 2;
		if (pe < pitchMin) pitchMin = pe;
		if (pe > pitchMax) pitchMax = pe;
		if (ce < cutoffMin) cutoffMin = ce;
		if (ce > cutoffMax) cutoffMax = ce;

		if (++statusCounter < statusInterval)
		{
			return;
		}
		statusCounter = 0;

		// Build into the buffer core1 isn't reading, then hand it over
		int b = statusPublished ^ 1;
		uint8_t *s = statusBuf[b];
		int n = 0;
		s[n++] = uint8_t((freeze ? 1 : 0) | (typeB ? 2 : 0) | (Connected(Input::Pulse1) ? 4 : 0)
			| (Connected(Input::Audio1) ? 8 : 0) | (Connected(Input::Audio2) ? 16 : 0));
		s[n++] = uint8_t(filterType);
		n += sysex::Put14(s + n, pe);
		n += sysex::Put14(s + n, pitchMin);
		n += sysex::Put14(s + n, pitchMax);
		n += sysex::Put14(s + n, ce);
		n += sysex::Put14(s + n, cutoffMin);
		n += sysex::Put14(s + n, cutoffMax);
		s[n++] = uint8_t(KnobVal(Main) >> 5);
		s[n++] = uint8_t(KnobVal(X) >> 5);
		s[n++] = uint8_t(KnobVal(Y) >> 5);
		s[n++] = uint8_t(engine.Length());
		uint32_t reg = engine.Register();
		for (int i = 4; i >= 0; i--)
		{
			s[n++] = uint8_t((reg >> (7 * i)) & 0x7F);
		}
		s[n++] = uint8_t(cycles);
		s[n++] = uint8_t(Clip(peakL >> 4, 0, 127));
		s[n++] = uint8_t(Clip(peakR >> 4, 0, 127));
		s[n++] = uint8_t(shownBusiestUs > 127 ? 127 : shownBusiestUs);
		if (scopeState == ScopeFull)
		{
			s[n++] = sysex::kScopeLen;
			for (int i = 0; i < sysex::kScopeLen; i++)
			{
				s[n++] = scopeBuf[i];
			}
			scopeState = ScopeArmed;
		}
		else
		{
			s[n++] = 0;
		}
		s[n++] = uint8_t(bitCount);
		for (int i = 0; i < bitCount; i++)
		{
			s[n++] = uint8_t(bitAcc[i] >> 7);
			s[n++] = uint8_t(bitAcc[i] & 0x7F);
		}
		statusLen[b] = n;
		statusPublished = b;
		statusSeq = statusSeq + 1;

		bitCount = 0;
		cycles = 0;
		peakL = peakR = 0;
		pitchMin = cutoffMin = 16383;
		pitchMax = cutoffMax = 0;
	}

	//------------------------------------------------------------------------
	// USB, on core1
	//------------------------------------------------------------------------

	// The web editor counts as linked while its pings keep arriving
	bool WebLinked() const
	{
		return !hostMode && pinged && (time_us_32() - lastPingUs) < 3000000;
	}

	// Host mode: read an 8mu plugged straight into the Computer
	static void Core1Host()
	{
		board_init();
		tuh_init(0);
		while (true)
		{
			gCard->mu.Poll();
		}
	}

	// Device mode: talk to the web editor
	static void Core1Device()
	{
		board_init();
		tud_init(0);
		sysex::Parser parser;
		uint32_t sentSeq = 0;
		while (true)
		{
			tud_task();
			uint8_t buf[64];
			while (tud_midi_available())
			{
				uint32_t n = tud_midi_stream_read(buf, sizeof(buf));
				if (n == 0) break;
				for (uint32_t i = 0; i < n; i++)
				{
					if (parser.Feed(buf[i]))
					{
						gCard->OnSysEx(parser.cmd, parser.payload, parser.length);
					}
				}
			}

			if (gCard->WebLinked() && gCard->statusSeq != sentSeq)
			{
				sentSeq = gCard->statusSeq;
				int b = gCard->statusPublished;
				uint8_t msg[sysex::kStatusMaxLen];
				int n = sysex::Header(msg, sysex::Status);
				int len = gCard->statusLen[b];
				for (int i = 0; i < len; i++)
				{
					msg[n++] = gCard->statusBuf[b][i] & 0x7F;
				}
				msg[n++] = 0xF7;
				Write(msg, n);
			}
		}
	}

	// Send a whole message, waiting briefly for room if need be.  If the
	// computer isn't reading, the rest is dropped; the editor's parser
	// resynchronises on the next F0.
	static void Write(const uint8_t *msg, int len)
	{
		uint32_t start = time_us_32();
		int sent = 0;
		while (sent < len && tud_mounted())
		{
			sent += int(tud_midi_stream_write(0, msg + sent, uint32_t(len - sent)));
			if (sent < len)
			{
				if (time_us_32() - start > 50000) return;
				tud_task();
			}
		}
	}

	// Handle one message from the web editor
	void OnSysEx(uint8_t cmd, const uint8_t *p, int len)
	{
		switch (cmd)
		{
		case sysex::Hello:
			SendState();
			break;
		case sysex::Set:
			if (len >= 3 && p[0] < numPages && p[1] < numFaders)
			{
				pages[p[0]].value[p[1]] = sysex::FromFader(p[2] & 0x7F);
			}
			break;
		case sysex::SetAll:
			if (len >= 2 + sysex::kNumValues && p[0] == sysex::kVersion)
			{
				freeze = p[1] != 0;
				for (int i = 0; i < sysex::kNumValues; i++)
				{
					pages[i / numFaders].value[i % numFaders] = sysex::FromFader(p[2 + i] & 0x7F);
				}
			}
			break;
		case sysex::Reseed:
			resetPending = true;
			break;
		case sysex::Ping:
			lastPingUs = time_us_32();
			pinged = true;
			break;
		case sysex::Freeze:
			if (len >= 1) freeze = p[0] != 0;
			break;
		case sysex::Motion:
			if (len >= 6)
			{
				webPitch = Clip(sysex::Get14(p) - 8192, -2032, 2032);
				webRoll = Clip(sysex::Get14(p + 2) - 8192, -2032, 2032);
				webYaw = Clip(sysex::Get14(p + 4) - 8192, -2032, 2032);
			}
			break;
		default:
			break;
		}
	}

	void SendState()
	{
		uint8_t msg[sysex::kStateLen];
		int n = sysex::Header(msg, sysex::State);
		msg[n++] = sysex::kVersion;
		msg[n++] = freeze ? 1 : 0;
		for (int i = 0; i < sysex::kNumValues; i++)
		{
			msg[n++] = uint8_t(sysex::ToFader(pages[i / numFaders].value[i % numFaders]) & 0x7F);
		}
		msg[n++] = 0xF7;
		Write(msg, n);
	}

	EightMU mu;
	ripple::Engine engine;
	ripple::Outputs lastOut;
	Page pages[numPages];
	int currentPage = PagePlay;
	int pageFlash = 0;
	bool had8mu = false;
	bool lastButton[4] = {};
	volatile bool freeze = false;
	bool typeB = false;
	int filterType = 0;
	volatile bool resetPending = false;
	int32_t cutoffMod = 0;
	int controlCounter = 0;
	int blinkCounter = 0;

	// USB mode, fixed at power-up
	bool hostMode = true;

	// Device mode: written on core1, read on core0
	volatile uint32_t lastPingUs = 0;
	volatile bool pinged = false;
	volatile int32_t webPitch = 0, webRoll = 0, webYaw = 0;

	// Observation for the web editor, on core0
	int bitSampleCounter = 0;
	int bitCount = 0;
	uint16_t bitAcc[sysex::kMaxBits] = {};
	int32_t peakL = 0, peakR = 0;
	int32_t cycles = 0;
	int32_t pitchMin = 16383, pitchMax = 0, cutoffMin = 16383, cutoffMax = 0;
	int statusCounter = 0;
	enum ScopeState {ScopeArmed, ScopeCapturing, ScopeFull};
	ScopeState scopeState = ScopeArmed;
	int scopeN = 0, scopeWait = 0, scopeSkip = 0;
	int32_t lastScopeIn = 0;
	uint8_t scopeBuf[sysex::kScopeLen] = {};
	uint32_t busiestUs = 0, shownBusiestUs = 0;
	int32_t busySamples = 0;

	// STATUS, double-buffered: built on core0, sent from core1
	uint8_t statusBuf[2][sysex::kStatusMaxPayload] = {};
	volatile int statusLen[2] = {};
	volatile int statusPublished = 0;
	volatile uint32_t statusSeq = 0;
};


int main()
{
	set_sys_clock_khz(240000, true);

	static EightMURipple card;
	card.Run();
}
