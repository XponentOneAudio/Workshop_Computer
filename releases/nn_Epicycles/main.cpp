// Epicycles
//
// Circles turning on circles, for the Music Thing Workshop Computer, edited
// from a Music Thing 8mu over USB MIDI host.  Two modes; hold the switch down
// for a second (or hold the 8mu's button D) to swap between them.  The web
// app in web/index.html draws and explains both, and turns drawings and
// pictures into circles for the card to play.
//
// PARTIALS
// Eight sine waves - partials - are added together.  Each is a circle
// turning at its own speed, and the circles are nested: each one rides on
// the rim of the one before.  The height of the outermost point is the
// output, so the sum of simple circular motions draws a more complicated
// waveform.  This is a Fourier series.
//
// The 8mu's eight faders are the eight partials; its buttons choose what the
// faders edit:
//   Button A  LEVEL      size of each circle, the partial's amplitude
//   Button B  PHASE      where each circle starts, 0 to 360 degrees
//   Button C  FREQUENCY  speed of each circle.  The centre is exactly the
//                        partial's harmonic (partial n turns n times as fast
//                        as the first); either side detunes it up to 12
//                        semitones, making the sound inharmonic
//   Button D  SHAPE      (tap) loads the next built-in recipe: saw, square,
//                        triangle, sine, pulse, bell
//
//   Main knob   Pitch, C1 to C7, plus CV In 1 at 1V/oct
//   X knob      Partials: how many sound, 1 to 8, fading each one in
//   Y knob      Stretch: partial n moves from n times the fundamental to
//               n^(1 + stretch) times, 0 to 0.5, as in stiff piano strings
//   Switch up   Harmonic lock: every partial at its exact harmonic, ignoring
//               the FREQUENCY page and stretch
//   Switch mid  Free: FREQUENCY page and stretch apply
//   CV In 2     Partials, added to X
//   Audio In 1  Stretch, added to Y
//   Pulse In 1  Sync: every circle back to its start phase
//   Pulse In 2  Next shape
//   Audio Out 1 The sum: the height of the outermost circle (sines)
//   Audio Out 2 Its shadow on the other axis (cosines).  Out 1 and Out 2
//               into a scope's X-Y mode draw the nested circles' path
//
// DRAWING
// Up to 64 circles, each turning a whole number of times for every time
// round the drawing, some anticlockwise and some clockwise.  Their outermost
// point traces a closed shape: Audio Out 2 is its position across, Audio
// Out 1 its height, so the two into a scope's X-Y mode draw it.  The circles
// are the drawing's (complex) Fourier series, biggest first.  The web app
// finds them for a drawing or picture and sends them; eight built-in
// drawings are in drawings.h.
//
//   Main knob   Pitch: how many times a second the drawing is traced, C1 to
//               C7, plus CV In 1 at 1V/oct
//   X knob      Circles: how many turn, 1 to 64, biggest first, each fading in
//   Y knob      Rotate the drawing, a full turn
//   Switch up   Slow: seven octaves down, about 0.25Hz to 16Hz, so the
//               outputs draw it as CV
//   Switch mid  Audio
//   CV In 2     Circles, added to X
//   Audio In 1  Rotate, added to Y, at audio rate
//   Pulse In 1  Sync: back to the start of the drawing
//   Pulse In 2  Next drawing
//   8mu faders  The circles in eight bands by speed: 1, 2, 3-4, 5-8, 9-16,
//               17-32, 33-64 and 65-127 turns per drawing
//   Audio Out 1 The height of the outermost point
//   Audio Out 2 Its position across
//
// Both
//   Switch down Tap for the next shape or drawing; hold to swap mode
//   CV Out 1    Pitch, 1V/oct, 0V at middle C
//   Pulse Out 1 Square wave at the pitch
//   After a page, mode or shape change the faders 'pick up': a fader only
//   takes over once moved to (or across) the value already stored, so
//   nothing jumps.  The 8mu's LEDs show the stored values.
//
// USB, chosen once at power-up
//   Port supplying power (an 8mu, or nothing yet): USB host, reading the 8mu.
//   Computer plugged in: USB MIDI device called "Epicycles", for the
//   web app in web/index.html (protocol in sysex.h).  An 8mu plugged into
//   the computer is passed on by the web app.

// First, so TinyUSB is configured for host and device modes before
// EightMU.h supplies its host-only defaults
#include "tusb_config.h"
#include "ComputerCard.h"
#include "EightMU.h"
#include "sysex.h"
#include "drawings.h"

#include <math.h>

class Epicycles;
static Epicycles *gCard = nullptr;

// Built-in partials shapes, in 8mu fader units (0-127): level, phase,
// frequency.  Levels are amplitudes, 127 = 1.  Phase 64 = 180 degrees.
// Frequency 64 is the exact harmonic.  The same table is in web/index.html.
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


class Epicycles : public ComputerCard
{
public:
	static constexpr int kPartials = 8;
	static constexpr int kCircles = drawings::kMaxCircles;
	// Pages 0-2 are the partials pages; page 3 is the drawing's bands
	enum Page {PageLevel, PagePhase, PageFreq, PageBands, kPages};
	enum Mode {ModePartials, ModeDrawing, kModes};

	Epicycles()
	{
		for (int i = 0; i <= kSineSize; i++)
		{
			sineTab[i] = int16_t(32767.0f * sinf(6.2831853f * float(i) / float(kSineSize)));
		}
		for (int i = 0; i < kQuadSize; i++)
		{
			float a = 6.2831853f * float(i) / float(kQuadSize);
			int32_t s = int32_t(32767.0f * sinf(a)), c = int32_t(32767.0f * cosf(a));
			quadTab[i] = int32_t(uint32_t(s) << 16) | (c & 0xFFFF);
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
		for (int b = 0; b < kPartials; b++) params[PageBands][b] = 4064;
		LoadDrawing(kStartDrawing);
		mode = ModeDrawing;

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
		bool drawing = mode == ModeDrawing;

		// Pulse In 1: sync
		bool pulse1 = PulseIn1RisingEdge();
		if (syncRequest)
		{
			syncRequest = false;
			pulse1 = true;
		}

		// Switch down: a tap is the next shape or drawing, a hold swaps mode
		if (SwitchChanged())
		{
			if (SwitchVal() == Down)
			{
				downHeld = true;
				downSamples = 0;
				downSwapped = false;
			}
			else if (downHeld)
			{
				downHeld = false;
				if (!downSwapped) Next();
			}
		}
		if (downHeld && !downSwapped && ++downSamples >= 48000)
		{
			downSwapped = true;
			ToggleMode();
		}

		// Pulse In 2: next shape, or next drawing
		if (PulseIn2RisingEdge()) Next();

		if (++controlCount >= 32)
		{
			controlCount = 0;
			Control();
		}

		if (drawing) DrawingSample(pulse1);
		else PartialsSample(pulse1);
	}

private:
	EightMU mu;

	// Fader values, stored as raw fader values 0-4064: pages 0-2 the
	// partials, page 3 the drawing's bands.  Written by core1 in device
	// mode, as the web app sends them.
	volatile int32_t params[kPages][kPartials];
	volatile int mode = ModePartials;
	volatile int shape = 0;
	volatile int drawingIndex = 0;      // 0-7 built in, or sysex::kCustomDrawing
	volatile uint32_t stateChanges = 0; // counts changes the web app should hear of
	int flash = 0;                      // LED flash on a mode change
	int shapeFlash = 0;

	// Switch down: tap or hold
	bool downHeld = false, downSwapped = false;
	int32_t downSamples = 0;

	// 8mu paging and fader pickup
	volatile int page = PageLevel; // partials page; drawing always edits PageBands
	bool latched[kPartials] = {};
	int32_t lastFader[kPartials] = {};
	bool lastFaderValid = false;
	bool prevButton[EightMU::numButtons] = {};
	int buttonHeld = 0;            // control ticks button D has been held
	bool buttonSwapped = false;
	bool wasConnected = false;
	int connectHoldoff = 0;
	volatile bool unlatchRequest = false;

	// Oscillators
	static constexpr int kSineSize = 1024;
	int16_t sineTab[kSineSize + 1];
	// For the drawing's 64 circles: sin in the top 16 bits, cos in the
	// bottom, so one load gives both.  No interpolation.
	static constexpr int kQuadSize = 4096;
	int32_t quadTab[kQuadSize];
	uint32_t exp2Tab[257];          // 2^(i/256) in Q30
	int32_t log2N[kPartials];       // log2(n), Q12 octaves

	// Partials
	uint32_t master = 0, masterInc = 0;
	uint32_t ph[kPartials] = {};
	uint32_t inc[kPartials] = {};
	uint32_t phaseOff[kPartials] = {};
	bool harmonic[kPartials] = {};
	int32_t gain[kPartials] = {};       // Q12, as heard
	int32_t gainTarget[kPartials] = {}; // Q12, including normalisation

	// Drawing: the circles in use
	static constexpr int kStartDrawing = 5; // heart
	struct Wheel
	{
		uint32_t k;   // turns per turn of the drawing, two's complement
		uint32_t off; // start phase
		int32_t g;    // Q12, as heard: radius, band, X and Nyquist
	};
	Wheel wheels[kCircles] = {};
	uint16_t amps[kCircles] = {};     // radius, 16384 = 1
	int drawCount = 0;
	uint32_t rotation = 0;            // from Y
	int32_t circlesQ8 = 64 << 8;      // 1 to 64, Q8
	int32_t circlesCtl = 4095, rotateCtl = 0; // for the web app
	int32_t outX = 0, outY = 0;       // last outputs, for the LEDs
	int32_t ledLevel[6] = {};

	// A drawing arriving from the web app: written by core1 into staged,
	// then carried over on core0 at the next control tick
	drawings::Circle staged[kCircles];
	volatile int commitRequest = -1;

	int controlCount = 0;
	int32_t baseNote = 48 << 8;   // Q8 semitones
	int32_t partialsQ8 = 8 << 8;  // 1 to 8, Q8
	int32_t stretch = 0;          // 0 to 2048, for 0 to 0.5 (Q12)
	bool switchUp = false;        // harmonic lock (partials), slow (drawing)

	// USB mode, fixed at power-up
	bool hostMode = true;

	// Device mode: written on core1, read on core0
	volatile bool syncRequest = false;
	volatile int shapeRequest = -1, drawingRequest = -1, modeRequest = -1;
	volatile uint32_t lastPingUs = 0;
	volatile bool pinged = false;

	// Snapshot for the web app: written on core0, read on core1
	volatile int32_t stNote = 0, stCircles = 0, stRotate = 0;
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

	// Gain (Q12) faded towards Nyquist, for a circle turning inc per sample
	static int32_t NyquistFade(int32_t g, uint64_t inc)
	{
		if (inc >= kIncFadeHi) return 0;
		if (inc <= kIncFadeLo) return g;
		return int32_t((int64_t(g) * int64_t(kIncFadeHi - inc)) / int64_t(kIncFadeHi - kIncFadeLo));
	}

	// PHASE fader to a phase offset: the full fader range is one turn
	uint32_t TargetPhase(int n) const
	{
		return uint32_t(params[PagePhase][n] >> 5) << 25;
	}

	// A detune fader to Q12 octaves.  The middle three fader steps are
	// exact; outside them it's up to 'range' octaves either way.
	static int32_t Detune(int32_t v, int32_t range)
	{
		int32_t d = (v >> 5) - 64;
		if (d >= -1 && d <= 1) return 0;
		d += d > 0 ? -1 : 1;
		if (d < -62) d = -62;
		return (d * range) / 62;
	}

	// The band (8mu fader) a circle turning m times per drawing is in:
	// 1, 2, 3-4, 5-8, 9-16, 17-32, 33-64, 65-127
	static int Band(uint32_t m)
	{
		return m <= 1 ? 0 : 32 - __builtin_clz(m - 1);
	}

	void LoadShape(int s)
	{
		for (int p = 0; p < 3; p++)
		{
			for (int n = 0; n < kPartials; n++)
			{
				params[p][n] = int32_t(kShapes[s][p][n]) << 5;
			}
		}
		shape = s;
		shapeFlash = 300; // ~0.2s at control rate
		unlatchRequest = true;
		stateChanges = stateChanges + 1;
	}

	// Start drawing new circles, faded in from silence
	void UseCircles(const drawings::Circle *c, int count)
	{
		if (count > kCircles) count = kCircles;
		for (int i = 0; i < count; i++)
		{
			wheels[i].k = uint32_t(int32_t(c[i].k));
			wheels[i].off = uint32_t(c[i].phase & 16383) << 18;
			wheels[i].g = 0;
			amps[i] = c[i].amp;
		}
		drawCount = count;
		shapeFlash = 300;
	}

	void LoadDrawing(int d)
	{
		UseCircles(drawings::kDrawings[d].circles, drawings::kDrawings[d].count);
		drawingIndex = d;
		stateChanges = stateChanges + 1;
	}

	void Next()
	{
		if (mode == ModeDrawing)
		{
			int d = drawingIndex == sysex::kCustomDrawing ? 0 : drawingIndex + 1;
			LoadDrawing(d % drawings::kNumDrawings);
		}
		else LoadShape((shape + 1) % kNumShapes);
	}

	void ToggleMode()
	{
		mode = mode == ModeDrawing ? ModePartials : ModeDrawing;
		flash = 450; // ~0.3s at control rate
		unlatchRequest = true;
		stateChanges = stateChanges + 1;
	}

	//------------------------------------------------------------------------
	// Partials
	//------------------------------------------------------------------------

	void PartialsSample(bool sync)
	{
		if (sync)
		{
			master = 0;
			for (int n = 0; n < kPartials; n++) ph[n] = 0;
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

	void PartialsControl()
	{
		int32_t px = KnobVal(X) + CVIn2();
		px = px < 0 ? 0 : (px > 4095 ? 4095 : px);
		partialsQ8 = 256 + (px * 7 * 256) / 4095;
		int32_t sy = KnobVal(Y) + AudioIn1();
		sy = sy < 0 ? 0 : (sy > 4095 ? 4095 : sy);
		stretch = sy >> 1;
		bool lock = switchUp;

		// Each partial's speed, and its share of the output
		int32_t level[kPartials];
		int32_t total = 0;
		for (int n = 0; n < kPartials; n++)
		{
			int32_t off = lock ? 0 : Detune(params[PageFreq][n], 4096);
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
			int32_t g = NyquistFade(Q12(params[PageLevel][n]), i64);
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
	}

	//------------------------------------------------------------------------
	// Drawing
	//------------------------------------------------------------------------

	void DrawingSample(bool sync)
	{
		if (sync) master = 0;
		master += masterInc;

		// Each circle: k turns per turn of the drawing.  sin for the height,
		// cos for the position across.  Kept lean: this runs 64 times a
		// sample.
		const uint32_t m = master;
		const int32_t *tab = quadTab;
		int32_t y = 0, x = 0;
		for (const Wheel *w = wheels, *end = wheels + drawCount; w < end; w++)
		{
			int32_t v = tab[(m * w->k + w->off) >> 20];
			y += (v >> 16) * w->g;
			x += int16_t(v) * w->g;
		}

		// Turning every circle by the same angle turns the whole drawing,
		// as does turning their sum, which is cheaper.  Y, plus Audio In 1 as
		// +/- half a turn.
		int32_t r = tab[(rotation + (uint32_t(int32_t(AudioIn1())) << 20)) >> 20];
		int32_t rs = r >> 16, rc = int16_t(r);
		x >>= 15;
		y >>= 15;
		int32_t xr = (x * rc - y * rs) >> 16;
		int32_t yr = (x * rs + y * rc) >> 16;

		// The drawing reaches 0.95 from the centre at most, but with circles
		// left out it can reach further, so clip
		y = yr < -2048 ? -2048 : (yr > 2047 ? 2047 : yr);
		x = xr < -2048 ? -2048 : (xr > 2047 ? 2047 : xr);
		y = y < -2048 ? -2048 : (y > 2047 ? 2047 : y);
		x = x < -2048 ? -2048 : (x > 2047 ? 2047 : x);
		AudioOut1(int16_t(y));
		AudioOut2(int16_t(x));
		PulseOut1(master < 0x80000000u);
		outX = x;
		outY = y;
	}

	void DrawingControl()
	{
		// A drawing from the web app, complete
		int c = commitRequest;
		if (c >= 0)
		{
			UseCircles(staged, c);
			drawingIndex = sysex::kCustomDrawing;
			stateChanges = stateChanges + 1;
			commitRequest = -1;
		}

		// Circles, on a squared law so the first few have room
		int32_t px = KnobVal(X) + CVIn2();
		px = px < 0 ? 0 : (px > 4095 ? 4095 : px);
		circlesCtl = px;
		circlesQ8 = 256 + int32_t((int64_t(63 * 256) * px * px) / (4095 * 4095));
		rotateCtl = KnobVal(Y);
		rotation = uint32_t(rotateCtl) << 20;

		for (int i = 0; i < drawCount; i++)
		{
			int32_t k = int32_t(wheels[i].k);
			uint32_t m = uint32_t(k < 0 ? -k : k);

			// Radius, scaled by its band's fader, faded near Nyquist, and
			// faded in with X, biggest circle first
			int32_t g = (int32_t(amps[i]) * Q12(params[PageBands][Band(m)])) >> 14;
			g = NyquistFade(g, uint64_t(masterInc) * m);
			int32_t fade = circlesQ8 - (i << 8);
			fade = fade < 0 ? 0 : (fade > 256 ? 256 : fade);
			g = (g * fade) >> 8;

			// Glide, so nothing clicks
			int32_t dg = g - wheels[i].g;
			wheels[i].g += (dg >> 3) != 0 ? (dg >> 3) : dg;
		}
	}

	// The six LEDs as a tiny scope: each lit by how near the drawing's
	// point passes it.  Slow, the point can be watched going round.
	void DrawingLeds()
	{
		static constexpr int32_t kLedX[6] = {-1024, 1024, -1024, 1024, -1024, 1024};
		static constexpr int32_t kLedY[6] = {1365, 1365, 0, 0, -1365, -1365};
		for (int i = 0; i < 6; i++)
		{
			int32_t dx = outX - kLedX[i], dy = outY - kLedY[i];
			dx = dx < 0 ? -dx : dx;
			dy = dy < 0 ? -dy : dy;
			int32_t d = dx > dy ? dx + (dy >> 1) : dy + (dx >> 1);
			int32_t b = d >= 1200 ? 0 : 4095 - (d * 4095) / 1200;
			ledLevel[i] += (b - ledLevel[i]) >> 3;
			LedBrightness(i, uint16_t(flash > 0 ? 4095 : ledLevel[i]));
		}
	}

	//------------------------------------------------------------------------
	// Both
	//------------------------------------------------------------------------

	// Runs every 32 samples (1.5kHz)
	void Control()
	{
		// Requests from the web app
		int req = shapeRequest;
		if (req >= 0) {shapeRequest = -1; LoadShape(req);}
		req = drawingRequest;
		if (req >= 0) {drawingRequest = -1; LoadDrawing(req);}
		req = modeRequest;
		if (req >= 0)
		{
			modeRequest = -1;
			if (req != mode) ToggleMode();
		}

		// Panel
		baseNote = (24 << 8) + (KnobVal(Main) * 72 * 256) / 4095 + CVIn1() * 9;
		if (baseNote < 0) baseNote = 0;
		if (baseNote > (127 << 8)) baseNote = 127 << 8;
		Switch sw = SwitchVal();
		if (sw == Up) switchUp = true;
		else if (sw == Middle) switchUp = false;

		if (hostMode) HandleEightMU();
		else if (unlatchRequest) unlatchRequest = false;

		bool drawing = mode == ModeDrawing;
		int32_t oct = (baseNote * 4) / 3;
		if (drawing && switchUp) oct -= 7 << 12; // slow
		masterInc = uint32_t(ExpScale(kIncNote0, oct));
		if (drawing) DrawingControl();
		else PartialsControl();

		CVOut1Millivolts(((baseNote - (60 << 8)) * 1000) / (12 * 256));

		// Snapshot for the web app
		stNote = baseNote >> 5;
		stPartials = uint8_t(((partialsQ8 - 256) * 127) / (7 * 256));
		stStretch = uint8_t(stretch >> 4);
		stFlags = uint8_t((switchUp ? 1 : 0) | (mu.Connected() ? 2 : 0));
		stCircles = circlesCtl;
		stRotate = rotateCtl;

		// Computer LEDs, all lit for a moment after a mode change.
		// Partials, with an 8mu or the web app: the page (A-C), with D
		// flashing on a shape change; without, the levels of partials 1-4.
		// LED 5 the square, LED 6 an 8mu or the web app.
		// Drawing: a tiny scope.
		if (shapeFlash > 0) shapeFlash--;
		if (flash > 0) flash--;
		if (drawing)
		{
			DrawingLeds();
			return;
		}
		bool conn = mu.Connected() || WebLinked();
		for (int i = 0; i < 4; i++)
		{
			if (flash > 0) LedOn(i, true);
			else if (conn) LedOn(i, i == 3 ? shapeFlash > 0 : page == i);
			else LedBrightness(i, uint16_t(gain[i] > 4095 ? 4095 : gain[i]));
		}
		LedOn(4, flash > 0 || master < 0x80000000u);
		LedOn(5, conn || flash > 0);
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

		// Partials: A-C choose the page.  Either mode: a tap on D is the
		// next shape or drawing, holding it swaps mode.
		bool drawing = mode == ModeDrawing;
		for (int b = 0; b < EightMU::numButtons; b++)
		{
			bool down = mu.Button(b);
			if (down && !prevButton[b])
			{
				if (!drawing && b < 3) {page = b; unlatchRequest = true;}
				if (b == 3) {buttonHeld = 0; buttonSwapped = false;}
			}
			if (b == 3 && down && !buttonSwapped && ++buttonHeld >= 1500)
			{
				buttonSwapped = true;
				ToggleMode();
			}
			if (b == 3 && !down && prevButton[b] && !buttonSwapped) Next();
			prevButton[b] = down;
		}
		if (unlatchRequest)
		{
			unlatchRequest = false;
			for (int i = 0; i < kPartials; i++) latched[i] = false;
		}

		// Faders, with pickup
		int pg = drawing ? int(PageBands) : page;
		for (int i = 0; i < kPartials; i++)
		{
			int32_t f = mu.Fader(i);
			volatile int32_t &p = params[pg][i];
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
		uint32_t lastChanges = gCard->stateChanges;
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

			// A shape, drawing or mode changed on the card: tell the web
			// app, once any request from it has been carried out
			if (gCard->stateChanges != lastChanges && gCard->shapeRequest < 0
				&& gCard->drawingRequest < 0 && gCard->modeRequest < 0
				&& gCard->commitRequest < 0)
			{
				lastChanges = gCard->stateChanges;
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
			if (drawingIndex == sysex::kCustomDrawing) SendDrawing();
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
			if (len >= 1 && p[0] < 3) page = p[0];
			break;
		case sysex::Shape:
			// Core0 loads it; the card then replies with STATE
			if (len >= 1 && p[0] < kNumShapes) shapeRequest = p[0];
			break;
		case sysex::Drawing:
			if (len >= 1 && p[0] < drawings::kNumDrawings) drawingRequest = p[0];
			break;
		case sysex::Circles:
		{
			// Ignored while the last drawing is still being taken up
			if (len < 2 || commitRequest >= 0) break;
			int first = p[0], count = p[1];
			if (count > sysex::kCirclesPerMessage || len < 2 + 6 * count) break;
			for (int i = 0; i < count && first + i < kCircles; i++)
			{
				const uint8_t *c = p + 2 + 6 * i;
				int32_t k = sysex::Get14(c) - 128;
				staged[first + i].k = int8_t(k < -127 ? -127 : (k > 127 ? 127 : k));
				staged[first + i].amp = uint16_t(sysex::Get14(c + 2));
				staged[first + i].phase = uint16_t(sysex::Get14(c + 4));
			}
			break;
		}
		case sysex::Commit:
			if (len >= 1 && p[0] <= kCircles && commitRequest < 0) commitRequest = p[0];
			break;
		case sysex::Mode:
			if (len >= 1 && p[0] < kModes) modeRequest = p[0];
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
		out[n++] = uint8_t(mode);
		out[n++] = uint8_t(page);
		for (int i = 0; i < sysex::kNumValues; i++)
		{
			out[n++] = uint8_t((params[i / kPartials][i % kPartials] >> 5) & 0x7F);
		}
		out[n++] = uint8_t(shape);
		out[n++] = uint8_t(drawingIndex);
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
		out[n++] = uint8_t(mode);
		n += sysex::Put14(out + n, stCircles);
		n += sysex::Put14(out + n, stRotate);
		out[n++] = 0xF7;
		return n;
	}

	// Circles 'first' onwards, up to eight of them, of the drawing in use
	int EncodeCircles(uint8_t *out, int first) const
	{
		int n = sysex::Header(out, sysex::Circles);
		int count = drawCount - first;
		if (count > sysex::kCirclesPerMessage) count = sysex::kCirclesPerMessage;
		out[n++] = uint8_t(first);
		out[n++] = uint8_t(count);
		for (int i = first; i < first + count; i++)
		{
			n += sysex::Put14(out + n, int32_t(wheels[i].k) + 128);
			n += sysex::Put14(out + n, amps[i]);
			n += sysex::Put14(out + n, int32_t(wheels[i].off >> 18));
		}
		out[n++] = 0xF7;
		return n;
	}

private:
	void SendState()
	{
		uint8_t msg[sysex::kStateLen];
		Write(msg, EncodeState(msg));
	}

	void SendDrawing()
	{
		uint8_t msg[sysex::kCirclesLen];
		int count = drawCount;
		for (int first = 0; first < count; first += sysex::kCirclesPerMessage)
		{
			Write(msg, EncodeCircles(msg, first));
		}
		int n = sysex::Header(msg, sysex::Commit);
		msg[n++] = uint8_t(count);
		msg[n++] = 0xF7;
		Write(msg, n);
	}
};


int main()
{
	set_sys_clock_khz(200000, true);

	static Epicycles card;
	card.Run();
}
