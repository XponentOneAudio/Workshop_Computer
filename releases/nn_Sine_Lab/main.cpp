// Sine Lab
//
// Two ways of making a complex wave from sine waves, for the Music Thing
// Workshop Computer, edited from a Music Thing 8mu over USB MIDI host.  Hold
// the switch down for a second (or hold the 8mu's button D) to swap between
// them.  The web app in web/index.html draws and explains both.
//
// ADDITIVE
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
//   Switch down Tap to load the next shape
//   CV In 2     Partials, added to X
//   Audio In 1  Stretch, added to Y
//   Pulse In 1  Sync: every circle back to its start phase
//   Pulse In 2  Next shape
//   Audio Out 1 The sum: the height of the outermost circle (sines)
//   Audio Out 2 Its shadow on the other axis (cosines).  Out 1 and Out 2
//               into a scope's X-Y mode draw the nested circles' path
//
// FM
// Two operators: a modulator sine wave bends a carrier sine wave, in one of
// four ways, chosen by the 8mu's buttons (or a tap down on the switch):
//   Button A  LINEAR FM        the modulator is added to the carrier's
//                              frequency, which stops at 0Hz, as an
//                              ordinary oscillator's linear FM input does
//   Button B  EXPONENTIAL FM   the modulator is added to the carrier's pitch,
//                              in octaves, as a 1V/oct input does
//   Button C  THROUGH-ZERO FM  linear, but the frequency can go below 0Hz,
//                              so the carrier runs backwards
//   Button D  PHASE MOD        the modulator is added to the carrier's phase,
//                              as in Yamaha's 'FM' synths
// For the three kinds of FM, the depth sets the deviation as a multiple of
// the carrier frequency (depth 1 swings it from 0 to twice its frequency); for
// phase modulation it sets the index directly, in radians.  With the
// modulator at the carrier's frequency the two agree.
//
// The eight faders:
//   1 Carrier ratio   0.5, 1, 2 ... 8 times the pitch
//   2 Modulator ratio 2Hz, 6Hz (fixed, for vibrato), then 0.25 to 16 times
//   3 Modulator fine  centre exact, up to 1 semitone either way
//   4 Depth           0 to 10, squared law
//   5 Feedback        the modulator modulating its own phase, 0 to 2
//   6 Decay           of the envelope started at Pulse In 1, 20ms to 8s
//   7 Env > depth     how much the envelope adds to the depth, 0 to 10
//   8 Env > level     0: the level stays put; 127: it follows the envelope
//
//   Main knob   Pitch, C1 to C7, plus CV In 1 at 1V/oct
//   X knob      Depth, added to fader 4
//   Y knob      Modulator ratio, up to 12 steps up from fader 2
//   Switch up   Drone: the envelope held at its peak
//   Switch mid  Envelope: Pulse In 1 starts it, then it decays
//   Switch down Tap for the next FM type
//   CV In 2     Depth, added to X
//   Audio In 1  External modulator, added to the modulator
//   Pulse In 1  Trigger: starts the envelope, and both operators from 0
//   Pulse In 2  Next example
//   Audio Out 1 The carrier, modulated
//   Audio Out 2 The modulator
//
// Both
//   CV Out 1    Pitch, 1V/oct, 0V at middle C
//   Pulse Out 1 Square wave at the pitch
//   After a page, mode or shape change the faders 'pick up': a fader only
//   takes over once moved to (or across) the value already stored, so
//   nothing jumps.  The 8mu's LEDs show the stored values.
//
// USB, chosen once at power-up
//   Port supplying power (an 8mu, or nothing yet): USB host, reading the 8mu.
//   Computer plugged in: USB MIDI device called "Sine Lab", for the
//   web app in web/index.html (protocol in sysex.h).  An 8mu plugged into
//   the computer is passed on by the web app.

// First, so TinyUSB is configured for host and device modes before
// EightMU.h supplies its host-only defaults
#include "tusb_config.h"
#include "ComputerCard.h"
#include "EightMU.h"
#include "sysex.h"

#include <math.h>

class SineLab;
static SineLab *gCard = nullptr;

// Built-in additive shapes, in 8mu fader units (0-127): level, phase,
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

// FM ratios.  A fader picks an entry with value * count / 128.  Modulator
// entries below zero are fixed frequencies, in -Hz.
static constexpr int kNumCarRatios = 9;
static const float kCarRatios[kNumCarRatios] = {0.5f, 1, 2, 3, 4, 5, 6, 7, 8};
static constexpr int kNumModRatios = 22;
static const float kModRatios[kNumModRatios] = {
	-2, -6, 0.25f, 0.5f, 0.75f, 1, 1.5f, 2, 2.5f, 3, 3.5f, 4,
	5, 6, 7, 8, 9, 10, 11, 12, 14, 16};

// FM examples: the eight faders (as above), then the FM type.
// The same table is in web/index.html.
enum FMType {FMLinear, FMExp, FMThroughZero, FMPhase, kFMTypes};
static constexpr int kNumExamples = 6;
static const uint8_t kExamples[kNumExamples][9] = {
	// Vibrato: a 6Hz modulator, exponential, as an LFO into 1V/oct
	{21, 9, 64, 20, 0, 64, 0, 0, FMExp},
	// Clarinet: modulator at twice the carrier, so odd harmonics only
	{21, 44, 64, 51, 0, 64, 0, 0, FMPhase},
	// Electric piano: 1:1, a bright attack fading to near sine
	{21, 32, 64, 31, 0, 91, 70, 127, FMPhase},
	// Bell: modulator at sqrt(2), an irrational ratio, so inharmonic
	{21, 38, 1, 0, 0, 112, 90, 127, FMPhase},
	// Feedback: the modulator modulating itself, towards a saw
	{21, 32, 64, 40, 76, 64, 0, 0, FMPhase},
	// Deep 1:1: try each type at a depth beyond 1
	{21, 32, 64, 64, 0, 64, 0, 0, FMLinear},
};


class SineLab : public ComputerCard
{
public:
	static constexpr int kPartials = 8;
	// Pages 0-2 are the additive pages; page 3 is the FM faders
	enum Page {PageLevel, PagePhase, PageFreq, PageFM, kPages};
	enum Mode {ModeAdditive, ModeFM, kModes};

	SineLab()
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
		LoadExample(1);
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
		bool fm = mode == ModeFM;

		// Pulse In 1: sync (additive), trigger (FM)
		bool pulse1 = PulseIn1RisingEdge();
		if (syncRequest)
		{
			syncRequest = false;
			pulse1 = true;
		}

		// Switch down: a tap is the next shape or FM type, a hold swaps mode
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
				if (!downSwapped)
				{
					if (fm) SetFMType((fmType + 1) % kFMTypes);
					else LoadShape((shape + 1) % kNumShapes);
				}
			}
		}
		if (downHeld && !downSwapped && ++downSamples >= 48000)
		{
			downSwapped = true;
			ToggleMode();
		}

		// Pulse In 2: next shape, or next example
		if (PulseIn2RisingEdge())
		{
			if (fm) LoadExample((example + 1) % kNumExamples);
			else LoadShape((shape + 1) % kNumShapes);
		}

		if (++controlCount >= 32)
		{
			controlCount = 0;
			Control();
		}

		if (mode == ModeFM) FMSample(pulse1);
		else AdditiveSample(pulse1);
	}

private:
	EightMU mu;

	// Fader values, stored as raw fader values 0-4064: pages 0-2 the
	// additive partials, page 3 the FM faders.  Written by core1 in device
	// mode, as the web app sends them.
	volatile int32_t params[kPages][kPartials];
	volatile int mode = ModeAdditive;
	volatile int shape = 0;
	volatile int example = 0;
	volatile int fmType = FMPhase;
	volatile uint32_t stateChanges = 0; // counts changes the web app should hear of
	int flash = 0;                      // LED flash on a mode change
	int shapeFlash = 0;

	// Switch down: tap or hold
	bool downHeld = false, downSwapped = false;
	int32_t downSamples = 0;

	// 8mu paging and fader pickup
	volatile int page = PageLevel; // additive page; FM always edits PageFM
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
	uint32_t exp2Tab[257];   // 2^(i/256) in Q30
	int32_t log2N[kPartials]; // log2(n), Q12 octaves

	// Additive
	uint32_t master = 0, masterInc = 0;
	uint32_t ph[kPartials] = {};
	uint32_t inc[kPartials] = {};
	uint32_t phaseOff[kPartials] = {};
	bool harmonic[kPartials] = {};
	int32_t gain[kPartials] = {};       // Q12, as heard
	int32_t gainTarget[kPartials] = {}; // Q12, including normalisation

	// FM, set at control rate
	uint32_t phC = 0, phM = 0, phP = 0; // carrier, modulator, pitch square
	uint32_t incC = 0, incM = 0;
	int32_t depthBase = 0;   // Q12, 0-16
	int32_t depthEnv = 0;    // Q12, added at the envelope's peak
	int32_t feedback = 0;    // Q12 radians
	int32_t levelEnv = 0;    // Q12
	int32_t envCoef = 0;     // per-sample decay multiplier, Q31
	int32_t lastDecay = -1;
	int32_t env = 0;         // Q31
	int32_t mod1 = 0, mod2 = 0; // last two modulator samples, for feedback
	int32_t depthKnob = 0;   // Q12, X + CV In 2, for the web app
	int32_t ratioSteps = 0;  // from Y

	int controlCount = 0;
	int32_t baseNote = 48 << 8;   // Q8 semitones
	int32_t partialsQ8 = 8 << 8;  // 1 to 8, Q8
	int32_t stretch = 0;          // 0 to 2048, for 0 to 0.5 (Q12)
	bool switchUp = false;        // harmonic lock (additive), drone (FM)

	// USB mode, fixed at power-up
	bool hostMode = true;

	// Device mode: written on core1, read on core0
	volatile bool syncRequest = false;
	volatile int shapeRequest = -1, exampleRequest = -1, modeRequest = -1;
	volatile uint32_t lastPingUs = 0;
	volatile bool pinged = false;

	// Snapshot for the web app: written on core0, read on core1
	volatile int32_t stNote = 0, stDepth = 0;
	volatile uint8_t stPartials = 0, stStretch = 0, stFlags = 0, stSteps = 0, stEnv = 0;

	static constexpr uint32_t kIncNote0 = 731558; // MIDI note 0, 8.18Hz
	static constexpr uint32_t kIncPerHz = 89478;  // 2^32 / 48000
	// Partials fade out between 16kHz and 20kHz, to stay clear of Nyquist
	static constexpr uint64_t kIncFadeLo = 1431655765ull; // 16kHz
	static constexpr uint64_t kIncFadeHi = 1789569707ull; // 20kHz
	static constexpr int32_t kMaxInc = 0x7FFFFFFF;        // 24kHz

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

	static uint32_t ClampInc(int64_t v)
	{
		return v < 0 ? 0 : (v > kMaxInc ? uint32_t(kMaxInc) : uint32_t(v));
	}

	// Stored fader value (0-4064) to 0-4096, so a fader at the top is 1
	static int32_t Q12(int32_t v)
	{
		return v >= 4064 ? 4096 : (v * 4096) / 4064;
	}

	// 0-4064 to 0-10 on a squared law, Q12
	static int32_t Depth(int32_t v)
	{
		int32_t q = Q12(v);
		return int32_t((int64_t(q) * q * 10) >> 12);
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

	void LoadExample(int e)
	{
		for (int n = 0; n < kPartials; n++)
		{
			params[PageFM][n] = int32_t(kExamples[e][n]) << 5;
		}
		fmType = kExamples[e][8];
		example = e;
		shapeFlash = 300;
		unlatchRequest = true;
		stateChanges = stateChanges + 1;
	}

	void SetFMType(int t)
	{
		fmType = t;
		stateChanges = stateChanges + 1;
	}

	void ToggleMode()
	{
		mode = mode == ModeFM ? ModeAdditive : ModeFM;
		flash = 450; // ~0.3s at control rate
		unlatchRequest = true;
		stateChanges = stateChanges + 1;
	}

	//------------------------------------------------------------------------
	// Additive
	//------------------------------------------------------------------------

	void AdditiveSample(bool sync)
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

	void AdditiveControl()
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
	}

	//------------------------------------------------------------------------
	// FM
	//------------------------------------------------------------------------

	void FMSample(bool trigger)
	{
		if (trigger)
		{
			env = 0x7FFFFFFF;
			phC = phM = 0;
			mod1 = mod2 = 0;
		}
		if (switchUp) env = 0x7FFFFFFF; // drone: held at the peak
		else env = int32_t((int64_t(env) * envCoef) >> 31);

		int32_t depth = depthBase + int32_t((int64_t(depthEnv) * env) >> 31);
		if (depth > (16 << 12)) depth = 16 << 12;

		// Modulator, with feedback from its own last two samples (averaged,
		// which keeps high feedback from buzzing at Nyquist)
		phM += incM;
		// (radians to a phase offset: 2^32 / 2pi is 5215/1024 in Q27)
		uint32_t fbOff = uint32_t((int64_t(feedback) * ((mod1 + mod2) >> 1) * 5215) >> 10);
		int32_t m = Sine(phM + fbOff);
		mod2 = mod1;
		mod1 = m;

		// Plus anything at Audio In 1
		m += AudioIn1() * 16;
		m = m < -32767 ? -32767 : (m > 32767 ? 32767 : m);

		uint32_t p;
		switch (fmType)
		{
		case FMLinear:
		case FMThroughZero:
		{
			// Frequency = carrier + depth * carrier * modulator
			int64_t dInc = ((int64_t(incC) * m) >> 15) * depth >> 12;
			int64_t i = int64_t(incC) + dInc;
			if (fmType == FMLinear) phC += ClampInc(i); // stops at 0Hz
			else
			{
				// Below 0Hz the phase runs backwards
				if (i > kMaxInc) i = kMaxInc;
				if (i < -kMaxInc) i = -kMaxInc;
				phC += uint32_t(int32_t(i));
			}
			p = phC;
			break;
		}
		case FMExp:
		{
			// Pitch = carrier + 0.4 * depth octaves * modulator
			int32_t oct = int32_t(((int64_t(depth) * m) >> 15) * 1638 >> 12);
			uint64_t i = ExpScale(incC, oct);
			phC += i > uint64_t(kMaxInc) ? uint32_t(kMaxInc) : uint32_t(i);
			p = phC;
			break;
		}
		default:
		{
			// Phase = carrier + depth radians * modulator
			phC += incC;
			// (can be several turns, so this wraps, as a phase should)
			p = phC + uint32_t((int64_t(depth) * m * 5215) >> 10);
			break;
		}
		}

		int32_t level = 4096 - levelEnv + int32_t((int64_t(levelEnv) * env) >> 31);
		AudioOut1(int16_t((Sine(p) * level) >> 16));
		AudioOut2(int16_t(mod1 >> 4));
		phP += masterInc;
		PulseOut1(phP < 0x80000000u);
	}

	void FMControl()
	{
		volatile int32_t *f = params[PageFM];

		// Depth: fader, X and CV In 2 (1 per volt)
		int32_t xk = Q12(KnobVal(X));
		depthKnob = int32_t((int64_t(xk) * xk * 10) >> 12) + CVIn2() * 12;
		int32_t d = Depth(f[3]) + depthKnob;
		depthBase = d < 0 ? 0 : (d > (16 << 12) ? 16 << 12 : d);
		depthEnv = Depth(f[6]);
		feedback = Q12(f[4]) * 2;
		levelEnv = Q12(f[7]);

		// Decay, 20ms to 8s
		if (f[5] != lastDecay)
		{
			lastDecay = f[5];
			float t = 0.02f * powf(400.0f, float(f[5]) / 4064.0f);
			envCoef = int32_t(2147483647.0f * expf(-1.0f / (t * 48000.0f)));
		}

		// Carrier and modulator speeds
		int ci = int((f[0] >> 5) * kNumCarRatios / 128);
		incC = ClampInc(int64_t(float(masterInc) * kCarRatios[ci]));
		ratioSteps = (KnobVal(Y) * 12 + 2047) / 4095;
		int mi = int((f[1] >> 5) * kNumModRatios / 128) + ratioSteps;
		if (mi >= kNumModRatios) mi = kNumModRatios - 1;
		float r = kModRatios[mi];
		uint32_t base = r < 0 ? uint32_t(-r * float(kIncPerHz)) : ClampInc(int64_t(float(masterInc) * r));
		incM = ClampInc(int64_t(ExpScale(base, Detune(f[2], 4096 / 12))));
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
		req = exampleRequest;
		if (req >= 0) {exampleRequest = -1; LoadExample(req);}
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

		masterInc = uint32_t(ExpScale(kIncNote0, (baseNote * 4) / 3));
		bool fm = mode == ModeFM;
		if (fm) FMControl();
		else AdditiveControl();

		CVOut1Millivolts(((baseNote - (60 << 8)) * 1000) / (12 * 256));

		// Snapshot for the web app
		stNote = baseNote >> 5;
		stPartials = uint8_t(((partialsQ8 - 256) * 127) / (7 * 256));
		stStretch = uint8_t(stretch >> 4);
		stFlags = uint8_t((switchUp ? 1 : 0) | (mu.Connected() ? 2 : 0));
		stDepth = depthKnob < 0 ? 0 : (depthKnob * 1000) >> 12;
		stSteps = uint8_t(ratioSteps);
		stEnv = uint8_t(env >> 24);

		// Computer LEDs, all lit for a moment after a mode change.
		// Additive, with an 8mu or the web app: the page (A-C), with D
		// flashing on a shape change; without, the levels of partials 1-4.
		// FM: the FM type (A-D), and LED 5 the envelope.
		bool conn = mu.Connected() || WebLinked();
		if (shapeFlash > 0) shapeFlash--;
		if (flash > 0) flash--;
		for (int i = 0; i < 4; i++)
		{
			if (flash > 0) LedOn(i, true);
			else if (fm) LedOn(i, fmType == i);
			else if (conn) LedOn(i, i == 3 ? shapeFlash > 0 : page == i);
			else LedBrightness(i, uint16_t(gain[i] > 4095 ? 4095 : gain[i]));
		}
		if (flash > 0) LedOn(4, true);
		else if (fm) LedBrightness(4, uint16_t(env >> 19));
		else LedOn(4, master < 0x80000000u);
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

		// Additive: A-C choose the page, a tap on D the next shape.
		// FM: A-D choose the FM type.  Either: holding D swaps mode.
		bool fm = mode == ModeFM;
		for (int b = 0; b < EightMU::numButtons; b++)
		{
			bool down = mu.Button(b);
			if (down && !prevButton[b])
			{
				if (fm) SetFMType(b);
				else if (b < 3) {page = b; unlatchRequest = true;}
				if (b == 3) {buttonHeld = 0; buttonSwapped = false;}
			}
			if (b == 3 && down && !buttonSwapped && ++buttonHeld >= 1500)
			{
				buttonSwapped = true;
				ToggleMode();
			}
			if (b == 3 && !down && prevButton[b] && !buttonSwapped && !fm)
			{
				LoadShape((shape + 1) % kNumShapes);
			}
			prevButton[b] = down;
		}
		if (unlatchRequest)
		{
			unlatchRequest = false;
			for (int i = 0; i < kPartials; i++) latched[i] = false;
		}

		// Faders, with pickup
		int pg = mode == ModeFM ? int(PageFM) : page;
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

			// A shape, example, type or mode changed on the card: tell the
			// web app, once any request from it has been carried out
			if (gCard->stateChanges != lastChanges && gCard->shapeRequest < 0
				&& gCard->exampleRequest < 0 && gCard->modeRequest < 0)
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
			break;
		case sysex::Set:
			if (len >= 3 && p[0] < kPages && p[1] < kPartials)
			{
				params[p[0]][p[1]] = int32_t(p[2] & 0x7F) << 5;
			}
			break;
		case sysex::SetAll:
			if (len >= 2 + sysex::kNumValues && p[0] == sysex::kVersion)
			{
				for (int i = 0; i < sysex::kNumValues; i++)
				{
					params[i / kPartials][i % kPartials] = int32_t(p[1 + i] & 0x7F) << 5;
				}
				if (p[1 + sysex::kNumValues] < kFMTypes) fmType = p[1 + sysex::kNumValues];
			}
			break;
		case sysex::Page:
			if (len >= 1 && p[0] < 3) page = p[0];
			break;
		case sysex::Shape:
			// Core0 loads it; the card then replies with STATE
			if (len >= 1 && p[0] < kNumShapes) shapeRequest = p[0];
			break;
		case sysex::Example:
			if (len >= 1 && p[0] < kNumExamples) exampleRequest = p[0];
			break;
		case sysex::Mode:
			if (len >= 1 && p[0] < kModes) modeRequest = p[0];
			break;
		case sysex::FMType:
			if (len >= 1 && p[0] < kFMTypes) fmType = p[0];
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
		out[n++] = uint8_t(fmType);
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
		out[n++] = uint8_t(fmType);
		n += sysex::Put14(out + n, stDepth);
		out[n++] = stSteps & 0x7F;
		out[n++] = stEnv & 0x7F;
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

	static SineLab card;
	card.Run();
}
