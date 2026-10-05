// Nested Circles
//
// An additive oscillator for the Music Thing Workshop Computer, edited from
// a Music Thing 8mu over USB MIDI host.
//
// Eight sine waves - partials - are added together.  Each is a circle
// turning at its own speed, and the circles are nested: each one rides on
// the rim of the one before.  The height of the outermost point is the
// output, so the sum of simple circular motions draws a more complicated
// waveform.  This is a Fourier series, and the web app in web/index.html
// draws it.
//
// The 8mu's eight faders are the eight partials; its buttons choose what the
// faders edit:
//   Button A  LEVEL      size of each circle, the partial's amplitude
//   Button B  PHASE      where each circle starts, 0 to 360 degrees
//   Button C  FREQUENCY  speed of each circle.  The centre is exactly the
//                        partial's harmonic (partial n turns n times as fast
//                        as the first); either side detunes it up to 12
//                        semitones, making the sound inharmonic
//   Button D  SHAPE      loads the next built-in recipe: saw, square,
//                        triangle, sine, pulse, bell
//
// After a page or shape change the faders 'pick up': a fader only takes over
// its partial once moved to (or across) the value already stored, so nothing
// jumps.  The 8mu's LEDs show the stored values on the current page.
//
// Panel
//   Main knob   Pitch, C1 to C7, plus CV In 1 at 1V/oct
//   X knob      Partials: how many sound, 1 to 8, fading each one in
//   Y knob      Stretch: partial n moves from n times the fundamental to
//               n^(1 + stretch) times, 0 to 0.5, as in stiff piano strings
//   Switch up   Harmonic lock: every partial at its exact harmonic, ignoring
//               the FREQUENCY page and stretch
//   Switch mid  Free: FREQUENCY page and stretch apply
//   Switch down Tap to load the next shape
//
// Inputs
//   CV In 1     Pitch, 1V/oct
//   CV In 2     Partials, added to X
//   Audio In 1  Stretch, added to Y
//   Pulse In 1  Sync: every circle back to its start phase
//   Pulse In 2  Next shape
//
// Outputs
//   Audio Out 1 The sum: the height of the outermost circle (sines)
//   Audio Out 2 Its shadow on the other axis (cosines).  Out 1 and Out 2
//               into a scope's X-Y mode draw the nested circles' path
//   CV Out 1    Pitch, 1V/oct, 0V at middle C
//   Pulse Out 1 Square wave at the fundamental, high for the first half of
//               each turn of the first circle
//
// USB, chosen once at power-up
//   Port supplying power (an 8mu, or nothing yet): USB host, reading the 8mu.
//   Computer plugged in: USB MIDI device called "Nested Circles", for the
//   web app in web/index.html (protocol in sysex.h).  An 8mu plugged into
//   the computer is passed on by the web app.

// First, so TinyUSB is configured for host and device modes before
// EightMU.h supplies its host-only defaults
#include "tusb_config.h"
#include "ComputerCard.h"
#include "EightMU.h"
#include "sysex.h"

#include <math.h>

class NestedCircles;
static NestedCircles *gCard = nullptr;

// Built-in shapes, in 8mu fader units (0-127): level, phase, frequency.
// Levels are amplitudes, 127 = 1.  Phase 64 = 180 degrees.  Frequency 64 is
// the exact harmonic.  The same table is in web/index.html.
static constexpr int kNumShapes = 6;
static const uint8_t kShapes[kNumShapes][3][8] = {
	// Saw: every harmonic, amplitude 1/n
	{{127, 64, 42, 32, 25, 21, 18, 16}, {0, 0, 0, 0, 0, 0, 0, 0},
	 {64, 64, 64, 64, 64, 64, 64, 64}},
	// Square: odd harmonics, amplitude 1/n
	{{127, 0, 42, 0, 25, 0, 18, 0}, {0, 0, 0, 0, 0, 0, 0, 0},
	 {64, 64, 64, 64, 64, 64, 64, 64}},
	// Triangle: odd harmonics, amplitude 1/n^2, every other one inverted
	{{127, 0, 14, 0, 5, 0, 3, 0}, {0, 0, 64, 0, 0, 0, 64, 0},
	 {64, 64, 64, 64, 64, 64, 64, 64}},
	// Sine: the first circle alone
	{{127, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0},
	 {64, 64, 64, 64, 64, 64, 64, 64}},
	// Pulse: every harmonic at the same level, all cosines (90 degrees)
	{{64, 64, 64, 64, 64, 64, 64, 64}, {32, 32, 32, 32, 32, 32, 32, 32},
	 {64, 64, 64, 64, 64, 64, 64, 64}},
	// Bell: inharmonic partials, as a struck metal object has
	{{127, 100, 80, 70, 55, 45, 35, 30}, {0, 0, 0, 0, 0, 0, 0, 0},
	 {64, 78, 96, 101, 99, 99, 101, 108}},
};


class NestedCircles : public ComputerCard
{
public:
	static constexpr int kPartials = 8;
	enum Page {PageLevel, PagePhase, PageFreq, kPages};

	NestedCircles()
	{
		for (int i = 0; i <= kSineSize; i++)
		{
			sineTab[i] = int16_t(32767.0f * sinf(6.2831853f * float(i) / float(kSineSize)));
		}
		for (int i = 0; i <= 256; i++)
		{
			exp2Tab[i] = uint32_t(1073741824.0f * exp2f(float(i) / 256.0f));
		}
		for (int n = 0; n < kPartials; n++)
		{
			log2N[n] = int32_t(4096.0f * log2f(float(n + 1)) + 0.5f);
		}

		LoadShape(0);
		for (int n = 0; n < kPartials; n++) phaseOff[n] = TargetPhase(n);

		// Give the USB power circuitry time to settle, then pick the USB
		// mode once: host if the port is supplying power (an 8mu, or nothing
		// yet), device if a computer is (the web app).  Boards older than
		// Rev 1.1 can't tell, and are always a device.
		sleep_us(150000);
		gCard = this;
		hostMode = USBPowerState() == DFP;
		multicore_launch_core1(hostMode ? Core1Host : Core1Device);
	}

	virtual void ProcessSample()
	{
		// Sync: every circle back to its start
		bool sync = PulseIn1RisingEdge();
		if (syncRequest)
		{
			syncRequest = false;
			sync = true;
		}
		if (sync)
		{
			master = 0;
			for (int n = 0; n < kPartials; n++) ph[n] = 0;
		}

		// Next shape, from a tap down on the switch or Pulse In 2
		if ((SwitchChanged() && SwitchVal() == Down) || PulseIn2RisingEdge())
		{
			LoadShape((shape + 1) % kNumShapes);
		}

		if (++controlCount >= 32)
		{
			controlCount = 0;
			Control();
		}

		// The first circle's turning: partials at exact harmonics follow
		// it, so their phases stay locked to it however the pitch moves
		master += masterInc;
		int32_t y = 0, x = 0;
		for (int n = 0; n < kPartials; n++)
		{
			if (harmonic[n]) ph[n] = master * uint32_t(n + 1);
			else ph[n] += inc[n];

			// Glide each gain to its target, so nothing clicks
			int32_t dg = gainTarget[n] - gain[n];
			gain[n] += (dg >> 6) != 0 ? (dg >> 6) : dg;
			if (gain[n] == 0) continue;

			uint32_t p = ph[n] + phaseOff[n];
			y += (Sine(p) * gain[n]) >> 12;
			x += (Sine(p + 0x40000000u) * gain[n]) >> 12;
		}

		// The gains sum to at most 1 (Q12), so x and y stay within +/-32767
		AudioOut1(int16_t(y >> 4));
		AudioOut2(int16_t(x >> 4));
		PulseOut1(master < 0x80000000u);
	}

private:
	EightMU mu;

	// Partial values, stored as raw fader values 0-4064.  Written by core1
	// in device mode, as the web app sends them.
	volatile int32_t params[kPages][kPartials];
	volatile int shape = 0;
	volatile uint32_t shapeLoads = 0; // counts every shape loaded
	int shapeFlash = 0;

	// 8mu paging and fader pickup
	volatile int page = PageLevel;
	bool latched[kPartials] = {};
	int32_t lastFader[kPartials] = {};
	bool lastFaderValid = false;
	bool prevButton[EightMU::numButtons] = {};
	bool wasConnected = false;
	int connectHoldoff = 0;
	volatile bool unlatchRequest = false;

	// Oscillator
	static constexpr int kSineSize = 1024;
	int16_t sineTab[kSineSize + 1];
	uint32_t exp2Tab[257];   // 2^(i/256) in Q30
	int32_t log2N[kPartials]; // log2(n), Q12 octaves

	uint32_t master = 0, masterInc = 0;
	uint32_t ph[kPartials] = {};
	uint32_t inc[kPartials] = {};
	uint32_t phaseOff[kPartials] = {};
	bool harmonic[kPartials] = {};
	int32_t gain[kPartials] = {};       // Q12, as heard
	int32_t gainTarget[kPartials] = {}; // Q12, including normalisation

	int controlCount = 0;
	int32_t baseNote = 48 << 8;   // Q8 semitones
	int32_t partialsQ8 = 8 << 8;  // 1 to 8, Q8
	int32_t stretch = 0;          // 0 to 2048, for 0 to 0.5 (Q12)
	bool lock = false;

	// USB mode, fixed at power-up
	bool hostMode = true;

	// Device mode: written on core1, read on core0
	volatile bool syncRequest = false;
	volatile int shapeRequest = -1;
	volatile uint32_t lastPingUs = 0;
	volatile bool pinged = false;

	// Snapshot for the web app: written on core0, read on core1
	volatile int32_t stNote = 0;
	volatile uint8_t stPartials = 0, stStretch = 0, stFlags = 0;

	static constexpr uint32_t kIncNote0 = 731558; // MIDI note 0, 8.18Hz
	// Partials fade out between 16kHz and 20kHz, to stay clear of Nyquist
	static constexpr uint64_t kIncFadeLo = 1431655765ull; // 16kHz
	static constexpr uint64_t kIncFadeHi = 1789569707ull; // 20kHz

	// One sample of sine, phase a full turn per 2^32, +/-32767
	int32_t Sine(uint32_t p) const
	{
		uint32_t i = p >> 22;
		int32_t f = (p >> 7) & 0x7FFF;
		int32_t a = sineTab[i];
		return a + (((sineTab[i + 1] - a) * f) >> 15);
	}

	// base * 2^(oct/4096), in 64 bits so high partials can't overflow
	uint64_t ExpScale(uint32_t base, int32_t oct) const
	{
		int32_t whole = oct >> 12;
		int32_t frac = oct & 4095;
		int i = frac >> 4, r = frac & 15;
		uint32_t m = exp2Tab[i] + (((exp2Tab[i + 1] - exp2Tab[i]) * uint32_t(r)) >> 4);
		uint64_t v = (uint64_t(base) * m) >> 30;
		if (whole >= 0) return whole > 20 ? (1ull << 52) : v << whole;
		return whole < -31 ? 0 : v >> -whole;
	}

	// Stored fader value (0-4064) to 0-4096, so a fader at the top is 1
	static int32_t Q12(int32_t v)
	{
		return v >= 4064 ? 4096 : (v * 4096) / 4064;
	}

	// PHASE fader to a phase offset: the full fader range is one turn
	uint32_t TargetPhase(int n) const
	{
		return uint32_t(params[PagePhase][n] >> 5) << 25;
	}

	// FREQUENCY fader to an offset from the harmonic, Q12 octaves.  The
	// middle three fader steps are exactly harmonic; outside them it's
	// up to 12 semitones either way.
	int32_t FreqOffset(int n) const
	{
		int32_t d = (params[PageFreq][n] >> 5) - 64;
		if (d >= -1 && d <= 1) return 0;
		d += d > 0 ? -1 : 1;
		if (d < -62) d = -62;
		return (d * 4096) / 62;
	}

	void LoadShape(int s)
	{
		for (int p = 0; p < kPages; p++)
		{
			for (int n = 0; n < kPartials; n++)
			{
				params[p][n] = int32_t(kShapes[s][p][n]) << 5;
			}
		}
		shape = s;
		shapeLoads = shapeLoads + 1;
		shapeFlash = 300; // ~0.2s at control rate
		unlatchRequest = true;
	}

	// Runs every 32 samples (1.5kHz)
	void Control()
	{
		// A shape asked for by the web app
		int req = shapeRequest;
		if (req >= 0)
		{
			shapeRequest = -1;
			LoadShape(req);
		}

		// Panel
		baseNote = (24 << 8) + (KnobVal(Main) * 72 * 256) / 4095 + CVIn1() * 9;
		if (baseNote < 0) baseNote = 0;
		if (baseNote > (127 << 8)) baseNote = 127 << 8;
		int32_t px = KnobVal(X) + CVIn2();
		px = px < 0 ? 0 : (px > 4095 ? 4095 : px);
		partialsQ8 = 256 + (px * 7 * 256) / 4095;
		int32_t sy = KnobVal(Y) + AudioIn1();
		sy = sy < 0 ? 0 : (sy > 4095 ? 4095 : sy);
		stretch = sy >> 1;
		Switch sw = SwitchVal();
		if (sw == Up) lock = true;
		else if (sw == Middle) lock = false;

		if (hostMode) HandleEightMU();
		else if (unlatchRequest) unlatchRequest = false;

		masterInc = uint32_t(ExpScale(kIncNote0, (baseNote * 4) / 3));

		// Each partial's speed, and its share of the output
		int32_t level[kPartials];
		int32_t total = 0;
		for (int n = 0; n < kPartials; n++)
		{
			int32_t off = lock ? 0 : FreqOffset(n);
			bool harm = off == 0 && (lock || stretch == 0);
			uint64_t i64;
			if (harm) i64 = uint64_t(masterInc) * uint64_t(n + 1);
			else
			{
				// n^(1 + stretch) times the fundamental, then the detune
				int32_t oct = log2N[n] + ((log2N[n] * stretch) >> 12) + off;
				i64 = ExpScale(masterInc, oct);
			}

			// Leaving harmonic lock, carry on from where the partial was
			if (harmonic[n] && !harm) ph[n] = master * uint32_t(n + 1);
			harmonic[n] = harm;
			inc[n] = i64 >= 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(i64);

			// Fade out near Nyquist, and fade partials in with X
			int32_t g = Q12(params[PageLevel][n]);
			if (i64 >= kIncFadeHi) g = 0;
			else if (i64 > kIncFadeLo)
			{
				g = int32_t((int64_t(g) * int64_t(kIncFadeHi - i64)) / int64_t(kIncFadeHi - kIncFadeLo));
			}
			int32_t fade = partialsQ8 - (n << 8);
			fade = fade < 0 ? 0 : (fade > 256 ? 256 : fade);
			g = (g * fade) >> 8;
			level[n] = g;
			total += g;

			// Glide the phase offset towards its fader, the short way round
			uint32_t target = TargetPhase(n);
			int32_t diff = int32_t(target - phaseOff[n]);
			int32_t step = diff >> 4;
			phaseOff[n] += uint32_t(step != 0 ? step : diff);
		}

		// Normalise, so that circles that would reach further than the
		// output can go are scaled down together
		for (int n = 0; n < kPartials; n++)
		{
			gainTarget[n] = total > 4096 ? (level[n] * 4096) / total : level[n];
		}

		CVOut1Millivolts(((baseNote - (60 << 8)) * 1000) / (12 * 256));

		// Snapshot for the web app
		stNote = baseNote >> 5;
		stPartials = uint8_t(((partialsQ8 - 256) * 127) / (7 * 256));
		stStretch = uint8_t(stretch >> 4);
		stFlags = uint8_t((lock ? 1 : 0) | (mu.Connected() ? 2 : 0));

		// Computer LEDs.  With an 8mu or the web app: the page (A-C), with D
		// flashing on a shape change.  Without: the levels of partials 1-4.
		bool conn = mu.Connected() || WebLinked();
		if (shapeFlash > 0) shapeFlash--;
		for (int i = 0; i < 4; i++)
		{
			if (conn) LedOn(i, i == 3 ? shapeFlash > 0 : page == i);
			else LedBrightness(i, uint16_t(gain[i] > 4095 ? 4095 : gain[i]));
		}
		LedOn(4, master < 0x80000000u);
		LedOn(5, conn);
	}

	void HandleEightMU()
	{
		bool conn = mu.Connected();
		if (!conn)
		{
			wasConnected = false;
			return;
		}
		if (!wasConnected)
		{
			// Wait for the 8mu's reply to the fader position query before
			// trusting fader values
			wasConnected = true;
			connectHoldoff = 1500; // ~1s at control rate
			lastFaderValid = false;
			for (int i = 0; i < kPartials; i++) latched[i] = false;
		}
		if (connectHoldoff > 0)
		{
			connectHoldoff--;
			return;
		}

		// Buttons A-C choose the page, D loads the next shape
		for (int b = 0; b < EightMU::numButtons; b++)
		{
			bool down = mu.Button(b);
			if (down && !prevButton[b])
			{
				if (b < kPages) page = b;
				else LoadShape((shape + 1) % kNumShapes);
				unlatchRequest = true;
			}
			prevButton[b] = down;
		}
		if (unlatchRequest)
		{
			unlatchRequest = false;
			for (int i = 0; i < kPartials; i++) latched[i] = false;
		}

		// Faders, with pickup
		for (int i = 0; i < kPartials; i++)
		{
			int32_t f = mu.Fader(i);
			volatile int32_t &p = params[page][i];
			if (!latched[i])
			{
				int32_t d = f - p;
				bool near = d > -96 && d < 96;
				bool crossed = lastFaderValid && ((lastFader[i] - p < 0) != (d < 0));
				if (near || crossed) latched[i] = true;
			}
			if (latched[i]) p = f;
			lastFader[i] = f;

			// 8mu LEDs: the stored value on this page
			mu.SetLed(i, (p * 9) >> 4);
		}
		lastFaderValid = true;
	}

	//------------------------------------------------------------------------
	// USB, on core1
	//------------------------------------------------------------------------

	// The web app counts as linked while its pings keep arriving
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

	// Device mode: talk to the web app
	static void Core1Device()
	{
		board_init();
		tud_init(0);
		sysex::Parser parser;
		uint32_t lastStatusUs = 0;
		uint32_t lastLoads = gCard->shapeLoads;
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

			// A shape loaded from the panel: tell the web app
			if (gCard->shapeLoads != lastLoads && gCard->shapeRequest < 0)
			{
				lastLoads = gCard->shapeLoads;
				if (gCard->WebLinked()) gCard->SendState();
			}

			uint32_t now = time_us_32();
			if (gCard->WebLinked() && now - lastStatusUs >= 33000)
			{
				lastStatusUs = now;
				uint8_t msg[sysex::kStatusLen];
				int len = gCard->EncodeStatus(msg);
				Write(msg, len);
			}
		}
	}

	// Send a whole message, waiting briefly for room if need be.  If the
	// computer isn't reading, the rest is dropped; the web app's parser
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

public:
	// Handle one message from the web app.  Public so it can be tested.
	void OnSysEx(uint8_t cmd, const uint8_t *p, int len)
	{
		switch (cmd)
		{
		case sysex::Hello:
			SendState();
			break;
		case sysex::Set:
			if (len >= 3 && p[0] < kPages && p[1] < kPartials)
			{
				params[p[0]][p[1]] = int32_t(p[2] & 0x7F) << 5;
			}
			break;
		case sysex::SetAll:
			if (len >= 1 + sysex::kNumValues && p[0] == sysex::kVersion)
			{
				for (int i = 0; i < sysex::kNumValues; i++)
				{
					params[i / kPartials][i % kPartials] = int32_t(p[1 + i] & 0x7F) << 5;
				}
			}
			break;
		case sysex::Page:
			if (len >= 1 && p[0] < kPages) page = p[0];
			break;
		case sysex::Shape:
			if (len >= 1 && p[0] < kNumShapes)
			{
				// Load it now, so the reply carries it; core0 only flashes
				// the LED and resets pickup
				for (int pg = 0; pg < kPages; pg++)
				{
					for (int n = 0; n < kPartials; n++)
					{
						params[pg][n] = int32_t(kShapes[p[0]][pg][n]) << 5;
					}
				}
				shapeRequest = p[0];
				SendState();
			}
			break;
		case sysex::Ping:
			lastPingUs = time_us_32();
			pinged = true;
			break;
		case sysex::Sync:
			syncRequest = true;
			break;
		default:
			break;
		}
	}

	int EncodeState(uint8_t *out) const
	{
		int n = sysex::Header(out, sysex::State);
		out[n++] = sysex::kVersion;
		out[n++] = uint8_t(page);
		for (int i = 0; i < sysex::kNumValues; i++)
		{
			out[n++] = uint8_t((params[i / kPartials][i % kPartials] >> 5) & 0x7F);
		}
		out[n++] = 0xF7;
		return n;
	}

	int EncodeStatus(uint8_t *out) const
	{
		int n = sysex::Header(out, sysex::Status);
		n += sysex::Put14(out + n, stNote);
		out[n++] = stPartials & 0x7F;
		out[n++] = stStretch & 0x7F;
		out[n++] = stFlags;
		out[n++] = 0xF7;
		return n;
	}

private:
	void SendState()
	{
		uint8_t msg[sysex::kStateLen];
		Write(msg, EncodeState(msg));
	}
};


int main()
{
	set_sys_clock_khz(200000, true);

	static NestedCircles card;
	card.Run();
}
