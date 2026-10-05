// Sample Lab
//
// Four ways of playing back a sample, for the Music Thing Workshop Computer,
// edited from a Music Thing 8mu over USB MIDI host.  The web app in
// web/index.html draws what each is doing and explains why.
//
// The card holds two seconds of sound.  At power-up that's a demo loop (see
// demo.h); with the switch up it records Audio In 1 instead.  The 8mu's
// buttons, or a tap down on the switch, choose how it's played:
//   Button A  VARISPEED    the way tape and early samplers play: reading
//                          faster raises the pitch, slower lowers it.  Time
//                          and pitch are one thing
//   Button B  OVERLAP-ADD  time-stretching by grains: short windowed slices,
//                          taken from where the playhead is and laid down at
//                          a steady rate, overlapping.  Speed and pitch are
//                          separate, but grains that don't line up smear
//                          and flutter
//   Button C  WSOLA        overlap-add, but each grain is nudged (within the
//                          search range) to where it best matches the end of
//                          the one before, so the waveform stays continuous
//   Button D  CLOUD        granular synthesis: grains scattered at random
//                          around the playhead, many at once, for textures
//
// Panel
//   Main knob   Speed, -2 to 2, reverse to the left of centre, stopped in the
//               middle (snapping to 0 and to 1 either way).  Plus Audio In 2
//   X knob      Pitch, -24 to 24 semitones, snapping to 0.  Plus CV In 1 at
//               1V/oct.  For varispeed, it changes the speed too
//   Y knob      Grain size, 10ms to 500ms
//   Switch up   Record Audio In 1, until the switch comes down or the two
//               seconds are full
//   Switch mid  Play
//   Switch down Tap for the next engine
//
// The eight faders:
//   1 Position     where in the sample to play, added to the playhead
//   2 Overlap      grains overlapping, 1 to 8 (B, C); grains per second,
//                  5 to 200 (D)
//   3 Spray        random scatter of where grains start, up to 500ms (B, D)
//   4 Window       grain shape, from square (clicks) to a smooth Hann curve
//   5 Search       how far WSOLA may move a grain to match, up to 15ms (C)
//   6 Pitch spray  random pitch for each grain, up to an octave (D)
//   7 Bits         output bit depth, 12 down to 1
//   8 Rate         output sample rate, 48kHz down to 1kHz, unfiltered
//
// Inputs
//   Audio In 1  Record input
//   Audio In 2  Speed, added to the Main knob
//   CV In 1     Pitch, 1V/oct
//   CV In 2     Position, added to fader 1
//   Pulse In 1  Record while high
//   Pulse In 2  Restart: playhead back to the start
//
// Outputs
//   Audio Out 1 The chosen engine
//   Audio Out 2 Varispeed at the same speed, in step with Out 1: what the
//               sound would be without time-stretching
//   CV Out 1    Playhead, 0 to 5V across the sample
//   CV Out 2    The newest grain's window, 0 to 5V
//   Pulse Out 1 A trigger at the start of each grain
//   Pulse Out 2 A trigger each time the playhead loops
//
// USB, chosen once at power-up
//   Port supplying power (an 8mu, or nothing yet): USB host, reading the 8mu.
//   Computer plugged in: USB MIDI device called "Sample Lab", for the web
//   app (protocol in sysex.h), which can also send the card a sample and
//   read back what it holds.

// First, so TinyUSB is configured for host and device modes before
// EightMU.h supplies its host-only defaults
#include "tusb_config.h"
#include "ComputerCard.h"
#include "EightMU.h"
#include "sysex.h"
#include "demo.h"

#include <math.h>

class SampleLab;
static SampleLab *gCard = nullptr;

static constexpr int kCapacity = 100000; // samples, a little over 2s
static int16_t gBuf[kCapacity];

enum Engine {Varispeed, OverlapAdd, WSOLA, Cloud, kEngines};
static constexpr int kFaders = 8;
static const uint8_t kDefaults[kFaders] = {0, 16, 0, 127, 64, 0, 0, 0};


class SampleLab : public ComputerCard
{
public:
	SampleLab()
	{
		for (int i = 0; i <= kSineSize; i++)
		{
			sineTab[i] = int16_t(32767.0f * sinf(6.2831853f * float(i) / float(kSineSize)));
		}
		for (int i = 0; i <= 256; i++)
		{
			exp2Tab[i] = uint32_t(1073741824.0f * exp2f(float(i) / 256.0f));
		}
		for (int i = 0; i < kFaders; i++) params[i] = int32_t(kDefaults[i]) << 5;
		LoadDemo();

		// Give the USB power circuitry time to settle, then pick the USB
		// mode once: host if the port is supplying power (an 8mu, or nothing
		// yet), device if a computer is (the web app).  Boards older than
		// Rev 1.1 can't tell, and are always a device.
		sleep_us(150000);
		gCard = this;
		hostMode = USBPowerState() == DFP;
		multicore_launch_core1(hostMode ? Core1Host : Core1Device);
	}

	virtual void __not_in_flash_func(ProcessSample)()
	{
		// Recording: switch up, Pulse In 1 high, or the web app
		// Recording starts as one of them goes high, so a take that fills the
		// buffer isn't immediately recorded over
		bool rec = (SwitchVal() == Up) || PulseIn1() || webRecord;
		if (rec && !lastRec && !recording && !uploading) StartRecording();
		lastRec = rec;
		if (recording)
		{
			int32_t in = AudioIn1();
			if (!rec || writePos >= kCapacity)
			{
				recording = false;
				webRecord = false;
				SetLength(writePos);
			}
			else gBuf[writePos++] = int16_t(in * 16);
			AudioOut1(int16_t(in)); // monitor the input while recording
			AudioOut2(int16_t(in));
		}

		// Switch down: next engine.  Pulse In 2: restart
		if (SwitchChanged() && SwitchVal() == Down) SetEngine((engine + 1) % kEngines);
		if (PulseIn2RisingEdge() || restartRequest)
		{
			restartRequest = false;
			Restart();
		}

		if (++controlCount >= 32)
		{
			controlCount = 0;
			Control();
		}
		if (recording) return;
		if (uploading || length < 2)
		{
			AudioOut1(0);
			AudioOut2(0);
			return;
		}

		// The playhead moves at the speed, whatever the engine
		head = Wrap(head + speed);
		if (speed > 0 ? head < lastHead : head > lastHead) loopTrig = 240;
		lastHead = head;

		int32_t out;
		switch (engine)
		{
		case Varispeed:
			// Speed and pitch together, as a tape's speed would
			tape = Wrap(tape + int32_t((int64_t(speed) * pitch) >> 12));
			out = Read(Wrap(tape + offset));
			break;
		case Cloud:
			if (--untilGrain <= 0)
			{
				untilGrain = int32_t((int64_t(cloudInterval) * (2048 + (Random() & 4095))) >> 12);
				if (untilGrain < 1) untilGrain = 1;
				StartGrain(CloudStart(), CloudRate());
			}
			out = int32_t((int64_t(SumGrains()) * gain) >> 12);
			break;
		default:
			if (--untilGrain <= 0)
			{
				untilGrain = hop;
				StartGrain(engine == WSOLA ? searchBest : SprayStart(), pitch);
				if (engine == WSOLA) BeginSearch();
			}
			if (engine == WSOLA) SearchStep();
			out = int32_t((int64_t(SumGrains()) * gain) >> 12);
			break;
		}
		out = out > 32767 ? 32767 : (out < -32767 ? -32767 : out);

		// Bits and rate, as cheap converters would have it: held, then
		// rounded down to fewer levels, with no filtering at all
		holdPhase += holdInc;
		if (holdPhase >= 65536)
		{
			holdPhase -= 65536;
			int32_t v = out >> 4;
			int s = 12 - bits;
			if (s > 0) v = ((v >> s) << s) + (1 << (s - 1));
			held = v;
		}
		AudioOut1(int16_t(held));
		AudioOut2(int16_t(Read(Wrap(head + offset)) >> 4));

		if (grainTrig) grainTrig--;
		if (loopTrig) loopTrig--;
		PulseOut1(grainTrig > 0);
		PulseOut2(loopTrig > 0);
	}

private:
	EightMU mu;

	// Fader values, stored as raw fader values 0-4064.  Written by core1 in
	// device mode, as the web app sends them.
	volatile int32_t params[kFaders];
	volatile int engine = OverlapAdd;
	volatile uint32_t stateChanges = 0; // counts changes the web app should hear of

	// The sample
	volatile int32_t length = 0;        // samples
	int32_t lengthQ = 0;                // length, Q12
	bool recording = false, lastRec = false;
	int32_t writePos = 0;

	// Playback, positions in samples Q12
	int32_t head = 0, lastHead = 0, tape = 0;
	int32_t speed = 4096, pitch = 4096; // Q12
	int32_t offset = 0;                 // Q12
	int32_t grainLen = 4800;            // samples
	int32_t hop = 2400;                 // samples between grain starts (B, C)
	int32_t cloudInterval = 4800;       // samples between grain starts (D), on average
	int32_t spray = 0;                  // Q12
	int32_t alpha = 4096;               // window taper, Q12: 0 square, 1 Hann
	int32_t searchRange = 0;            // samples
	int32_t pitchSpray = 0;             // Q12 octaves
	int32_t gain = 4096;                // Q12
	int bits = 12;
	uint32_t holdInc = 65536, holdPhase = 0;
	int32_t held = 0;
	int32_t untilGrain = 0;
	int grainTrig = 0, loopTrig = 0;
	uint32_t rng = 0x9E3779B9;

	// Grains
	struct Grain
	{
		bool on = false;
		int32_t src = 0;      // read position, Q12
		int32_t rate = 4096;  // read speed, Q12
		int32_t age = 0, len = 0;
		int32_t taper = 0;    // samples of fade at each end
		uint32_t taperInc = 0;
	};
	static constexpr int kGrains = 16;
	Grain grains[kGrains];
	int newest = 0;

	// WSOLA's search for where the next grain should start, done a little
	// at a time between grains: candidate offsets from the nominal start are
	// scored by how little they differ from the natural continuation of the
	// grain just started, first coarsely, then finely around the best
	int32_t searchBest = 0;             // Q12, where the next grain will start
	int32_t searchRef = 0, searchNominal = 0, searchRate = 4096;
	int32_t searchCand = 0, searchStepSize = 1, searchEnd = 0, searchPoint = 0, searchPoints = 0;
	int64_t searchScore = 0, searchBestScore = 0;
	int32_t searchBestOff = 0;
	bool searchFine = false, searching = false;
	static constexpr int kSearchBudget = 96; // compare points per sample

	// 8mu fader pickup
	bool latched[kFaders] = {};
	int32_t lastFader[kFaders] = {};
	bool lastFaderValid = false;
	bool prevButton[EightMU::numButtons] = {};
	bool wasConnected = false;
	int connectHoldoff = 0;
	volatile bool unlatchRequest = false;

	static constexpr int kSineSize = 1024;
	int16_t sineTab[kSineSize + 1];
	uint32_t exp2Tab[257]; // 2^(i/256) in Q30
	int controlCount = 0;
	int flash = 0;

	// USB mode, fixed at power-up
	bool hostMode = true;

	// Device mode: written on core1, read on core0
	volatile bool restartRequest = false, webRecord = false, uploading = false;
	volatile uint32_t lastPingUs = 0;
	volatile bool pinged = false;
	volatile bool dumpRequest = false;

	// Snapshot for the web app: written on core0, read on core1
	volatile int32_t stSpeed = 0, stPitch = 0, stSize = 0, stHead = 0;
	volatile uint8_t stFlags = 0;

	//------------------------------------------------------------------------

	uint32_t __not_in_flash_func(Random)()
	{
		rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
		return rng;
	}

	// One sample of sine, phase a full turn per 2^32, +/-32767
	int32_t __not_in_flash_func(Sine)(uint32_t p) const
	{
		uint32_t i = p >> 22;
		int32_t f = (p >> 7) & 0x7FFF;
		int32_t a = sineTab[i];
		return a + (((sineTab[i + 1] - a) * f) >> 15);
	}

	// base * 2^(oct/4096)
	uint32_t __not_in_flash_func(ExpScale)(uint32_t base, int32_t oct) const
	{
		int32_t whole = oct >> 12;
		int32_t frac = oct & 4095;
		int i = frac >> 4, r = frac & 15;
		uint32_t m = exp2Tab[i] + (((exp2Tab[i + 1] - exp2Tab[i]) * uint32_t(r)) >> 4);
		uint64_t v = (uint64_t(base) * m) >> 30;
		if (whole >= 0) v = whole > 20 ? (1ull << 52) : v << whole;
		else v = whole < -31 ? 0 : v >> -whole;
		return v > 0x7FFFFFFF ? 0x7FFFFFFF : uint32_t(v);
	}

	// A position into the sample, Q12, wrapped round its length
	int32_t __not_in_flash_func(Wrap)(int32_t q) const
	{
		if (lengthQ <= 0) return 0;
		while (q >= lengthQ) q -= lengthQ;
		while (q < 0) q += lengthQ;
		return q;
	}

	// The sample at a position (Q12, already wrapped), between samples by
	// straight-line interpolation
	int32_t __not_in_flash_func(Read)(int32_t q) const
	{
		int32_t i = q >> 12, f = q & 4095;
		int32_t j = i + 1 < length ? i + 1 : 0;
		int32_t a = gBuf[i];
		return a + (((gBuf[j] - a) * f) >> 12);
	}

	void SetLength(int32_t n)
	{
		if (n < 2400) n = n < 2 ? 0 : n; // keep even a short take
		length = n;
		lengthQ = n << 12;
		Restart();
		stateChanges = stateChanges + 1;
	}

	void LoadDemo()
	{
		demo::Build(gBuf);
		SetLength(demo::kLength);
	}

	void StartRecording()
	{
		recording = true;
		writePos = 0;
		for (int i = 0; i < kGrains; i++) grains[i].on = false;
	}

	void Restart()
	{
		head = lastHead = tape = 0;
		untilGrain = 0;
		searching = false;
		searchBest = Wrap(offset);
		for (int i = 0; i < kGrains; i++) grains[i].on = false;
	}

	void SetEngine(int e)
	{
		engine = e;
		flash = 300;
		untilGrain = 0;
		searching = false;
		searchBest = Wrap(head + offset);
		stateChanges = stateChanges + 1;
	}

	//------------------------------------------------------------------------
	// Grains
	//------------------------------------------------------------------------

	void __not_in_flash_func(StartGrain)(int32_t src, int32_t rate)
	{
		// The first free slot, or the oldest if all are busy
		int slot = -1, oldest = 0;
		for (int i = 0; i < kGrains; i++)
		{
			if (!grains[i].on) {slot = i; break;}
			if (grains[i].age > grains[oldest].age) oldest = i;
		}
		if (slot < 0) slot = oldest;
		Grain &g = grains[slot];
		g.on = true;
		g.src = src;
		g.rate = rate;
		g.age = 0;
		g.len = grainLen;
		// Tukey window: a cosine fade over alpha/2 of the grain at each end
		g.taper = int32_t((int64_t(grainLen) * alpha) >> 13);
		g.taperInc = g.taper > 0 ? uint32_t(0x80000000u / uint32_t(g.taper)) : 0;
		newest = slot;
		grainTrig = 96;
	}

	// The window, 0-32767, of a grain 'age' samples in
	int32_t __not_in_flash_func(Window)(const Grain &g) const
	{
		int32_t a = g.age, fromEnd = g.len - 1 - g.age;
		int32_t e = a < fromEnd ? a : fromEnd;
		if (e >= g.taper) return 32767;
		// half a cosine, from 0 up to 1, over the taper
		return (32767 - Sine(uint32_t(e) * g.taperInc + 0x40000000u)) >> 1;
	}

	int32_t __not_in_flash_func(SumGrains)()
	{
		int32_t sum = 0;
		for (int i = 0; i < kGrains; i++)
		{
			Grain &g = grains[i];
			if (!g.on) continue;
			sum += (Read(g.src) * Window(g)) >> 15;
			g.src = Wrap(g.src + g.rate);
			if (++g.age >= g.len) g.on = false;
		}
		return sum;
	}

	// Overlap-add: from the playhead, scattered by the spray
	int32_t __not_in_flash_func(SprayStart)()
	{
		int32_t s = head + offset;
		if (spray > 0) s += int32_t((int64_t(spray) * (int32_t(Random() >> 16) - 32768)) >> 15);
		return Wrap(s);
	}

	int32_t __not_in_flash_func(CloudStart)() {return SprayStart();}

	int32_t __not_in_flash_func(CloudRate)()
	{
		if (pitchSpray == 0) return pitch;
		int32_t oct = int32_t((int64_t(pitchSpray) * (int32_t(Random() >> 16) - 32768)) >> 15);
		return int32_t(ExpScale(uint32_t(pitch), oct));
	}

	//------------------------------------------------------------------------
	// WSOLA
	//------------------------------------------------------------------------

	// Called as a grain starts: find where the next one, a hop from now,
	// should start.  The reference is what this grain will be reading a hop
	// from now; the nominal start is where the playhead will be by then.
	void __not_in_flash_func(BeginSearch)()
	{
		Grain &g = grains[newest];
		searchRate = g.rate;
		searchRef = Wrap(g.src + int32_t((int64_t(g.rate) * hop)));
		searchNominal = Wrap(head + offset + int32_t((int64_t(speed) * hop)));
		searchBest = searchNominal;
		if (searchRange <= 0) {searching = false; return;}
		// Compare up to half a grain, every other sample, within budget
		int32_t cands = 2 * 24 + 1 + 2 * (searchRange / 24 + 1) + 1;
		searchPoints = int32_t((int64_t(hop) * kSearchBudget) / cands);
		int32_t maxPoints = grainLen / 4;
		if (searchPoints > maxPoints) searchPoints = maxPoints;
		if (searchPoints > 256) searchPoints = 256;
		if (searchPoints < 8) searchPoints = 8;
		searchStepSize = searchRange / 24 + 1;
		searchCand = -searchRange;
		searchEnd = searchRange;
		searchFine = false;
		searchPoint = 0;
		searchScore = 0;
		searchBestScore = INT64_MAX;
		searchBestOff = 0;
		searching = true;
	}

	void __not_in_flash_func(SearchStep)()
	{
		if (!searching) return;
		int32_t step2 = searchRate * 2; // compare every other sample
		int32_t candQ = Wrap(searchNominal + (searchCand << 12));
		for (int n = 0; n < kSearchBudget; n++)
		{
			int32_t d = searchPoint * step2;
			int32_t a = gBuf[Wrap(searchRef + d) >> 12];
			int32_t b = gBuf[Wrap(candQ + d) >> 12];
			searchScore += a > b ? a - b : b - a;
			if (++searchPoint < searchPoints) continue;

			// This candidate is done
			if (searchScore < searchBestScore)
			{
				searchBestScore = searchScore;
				searchBestOff = searchCand;
				searchBest = candQ;
			}
			searchPoint = 0;
			searchScore = 0;
			searchCand += searchStepSize;
			if (searchCand > searchEnd)
			{
				if (searchFine || searchStepSize == 1) {searching = false; return;}
				// Now finely, around the best coarse candidate
				searchFine = true;
				searchCand = searchBestOff - searchStepSize + 1;
				searchEnd = searchBestOff + searchStepSize - 1;
				searchStepSize = 1;
			}
			candQ = Wrap(searchNominal + (searchCand << 12));
		}
	}

	//------------------------------------------------------------------------
	// Control, every 32 samples (1.5kHz)
	//------------------------------------------------------------------------

	static int32_t Q12(int32_t v) {return v >= 4064 ? 4096 : (v * 4096) / 4064;}

	void __not_in_flash_func(Control)()
	{
		// Speed: -2 to 2, snapping to 0 and +/-1
		int32_t s = ((KnobVal(Main) - 2048) * 8192) / 2048;
		if (s > -250 && s < 250) s = 0;
		else if (s > 4096 - 250 && s < 4096 + 250) s = 4096;
		else if (s > -4096 - 250 && s < -4096 + 250) s = -4096;
		s += AudioIn2() * 4;
		speed = s < -8192 ? -8192 : (s > 8192 ? 8192 : s);

		// Pitch: -24 to 24 semitones, snapping to 0, plus 1V/oct
		int32_t p = ((KnobVal(X) - 2048) * 24 * 256) / 2048; // Q8 semitones
		if (p > -192 && p < 192) p = 0;
		p += CVIn1() * 9;
		if (p < -36 * 256) p = -36 * 256;
		if (p > 36 * 256) p = 36 * 256;
		stPitch = p;
		pitch = int32_t(ExpScale(4096, (p * 4) / 3)); // Q8 semitones to Q12 octaves

		// Grain size, 10ms to 500ms
		grainLen = int32_t(ExpScale(480, (KnobVal(Y) * 23119) / 4095)); // 480 * 50^(k/4095)
		if (grainLen > 24000) grainLen = 24000;

		if (hostMode) HandleEightMU();
		else if (unlatchRequest) unlatchRequest = false;

		volatile int32_t *f = params;
		int32_t pos = Q12(f[0]) + CVIn2() * 2;
		pos = pos < 0 ? 0 : (pos > 4095 ? 4095 : pos);
		offset = int32_t((int64_t(lengthQ) * pos) >> 12);
		int overlap = 1 + (f[1] >> 5) * 8 / 128;
		hop = grainLen / overlap;
		if (hop < 1) hop = 1;
		// Cloud density, 5 to 200 grains a second
		uint32_t perSec = ExpScale(5 << 8, (Q12(f[1]) * 21800) >> 12); // Q8
		cloudInterval = int32_t((48000ull << 8) / (perSec ? perSec : 1));
		int32_t sp = Q12(f[2]);
		spray = int32_t((int64_t(sp) * sp * 24000) >> 12);   // up to 500ms, squared law, Q12
		alpha = Q12(f[3]);
		int32_t se = Q12(f[4]);
		searchRange = int32_t((int64_t(se) * se * 720) >> 24); // up to 15ms, squared law
		pitchSpray = Q12(f[5]);                                // up to an octave
		bits = 12 - ((f[6] >> 5) * 11 + 63) / 127;
		holdInc = uint32_t(ExpScale(65536, -((Q12(f[7]) * 22877) >> 12))); // 48kHz to 1kHz

		// Overlapping windows add up: scale so the sum stays at full level.
		// Overlap-add's grains line up, so they add as levels; the cloud's
		// are random, so they add as power
		int32_t wmean = 4096 - (alpha >> 1); // Tukey window's average
		if (engine == Cloud)
		{
			float overlapping = float(grainLen) / float(cloudInterval) * float(wmean) / 4096.0f;
			gain = overlapping > 1.0f ? int32_t(4096.0f / sqrtf(overlapping)) : 4096;
		}
		else gain = int32_t((4096ll * 4096) / (int64_t(overlap) * wmean));

		CVOut1Millivolts(lengthQ > 0 ? int32_t((int64_t(head) * 5000) / lengthQ) : 0);
		CVOut2Millivolts(grains[newest].on ? (Window(grains[newest]) * 5000) >> 15 : 0);

		// Snapshot for the web app
		stSpeed = (speed * 1000) / 4096 + 4000;
		stSize = (grainLen * 10) / 48;
		stHead = head >> 12;
		stFlags = uint8_t((recording ? 1 : 0) | (mu.Connected() ? 2 : 0));

		// LEDs: the engine (A-D), all four flashing on a change; LED 5
		// recording, or flickering with each grain; LED 6 an 8mu or web app
		if (flash > 0) flash--;
		for (int i = 0; i < 4; i++) LedOn(i, engine == i || (flash > 0 && (flash / 50) % 2));
		LedBrightness(4, recording ? 4095 : (grainTrig > 0 ? 2048 : 0));
		LedOn(5, mu.Connected() || WebLinked());
	}

	void __not_in_flash_func(HandleEightMU)()
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
			for (int i = 0; i < kFaders; i++) latched[i] = false;
		}
		if (connectHoldoff > 0)
		{
			connectHoldoff--;
			return;
		}

		// Buttons A-D choose the engine
		for (int b = 0; b < EightMU::numButtons; b++)
		{
			bool down = mu.Button(b);
			if (down && !prevButton[b]) SetEngine(b);
			prevButton[b] = down;
		}
		if (unlatchRequest)
		{
			unlatchRequest = false;
			for (int i = 0; i < kFaders; i++) latched[i] = false;
		}

		// Faders, with pickup
		for (int i = 0; i < kFaders; i++)
		{
			int32_t f = mu.Fader(i);
			volatile int32_t &p = params[i];
			if (!latched[i])
			{
				int32_t d = f - p;
				bool near = d > -96 && d < 96;
				bool crossed = lastFaderValid && ((lastFader[i] - p < 0) != (d < 0));
				if (near || crossed) latched[i] = true;
			}
			if (latched[i]) p = f;
			lastFader[i] = f;
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
		int32_t dumpPos = -1;
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

			// A new sample or engine on the card: tell the web app
			if (gCard->stateChanges != lastChanges && !gCard->recording)
			{
				lastChanges = gCard->stateChanges;
				if (gCard->WebLinked()) gCard->SendState();
			}

			// Sending the sample, a chunk at a time between everything else
			if (gCard->dumpRequest)
			{
				gCard->dumpRequest = false;
				dumpPos = 0;
			}
			if (dumpPos >= 0)
			{
				int32_t len = gCard->length;
				if (dumpPos < len)
				{
					uint8_t msg[sysex::kDataLen];
					int n = sysex::Header(msg, sysex::SampleData);
					n += sysex::Put21(msg + n, dumpPos);
					for (int i = 0; i < sysex::kChunk && dumpPos < len; i++, dumpPos++)
					{
						n += sysex::Put14(msg + n, (gBuf[dumpPos] >> 2) + 8192);
					}
					msg[n++] = 0xF7;
					Write(msg, n);
				}
				else
				{
					uint8_t msg[5];
					int n = sysex::Header(msg, sysex::SampleEnd);
					msg[n++] = 0xF7;
					Write(msg, n);
					dumpPos = -1;
				}
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
			if (len >= 2 && p[0] < kFaders) params[p[0]] = int32_t(p[1] & 0x7F) << 5;
			break;
		case sysex::SetAll:
			if (len >= 2 + kFaders && p[0] == sysex::kVersion)
			{
				for (int i = 0; i < kFaders; i++) params[i] = int32_t(p[1 + i] & 0x7F) << 5;
				if (p[1 + kFaders] < kEngines) engine = p[1 + kFaders];
			}
			break;
		case sysex::Engine:
			if (len >= 1 && p[0] < kEngines) engine = p[0];
			break;
		case sysex::Ping:
			lastPingUs = time_us_32();
			pinged = true;
			break;
		case sysex::Restart:
			restartRequest = true;
			break;
		case sysex::SampleBegin:
			if (len >= 3 && !recording)
			{
				int32_t n = sysex::Get21(p);
				uploading = true;
				uploadLength = n > kCapacity ? kCapacity : n;
			}
			break;
		case sysex::SampleData:
			if (len >= 5 && uploading)
			{
				int32_t at = sysex::Get21(p);
				for (int i = 0; 3 + 2 * i + 1 < len; i++, at++)
				{
					if (at >= 0 && at < kCapacity) gBuf[at] = int16_t((sysex::Get14(p + 3 + 2 * i) - 8192) << 2);
				}
			}
			break;
		case sysex::SampleEnd:
			if (uploading)
			{
				SetLength(uploadLength);
				uploading = false;
			}
			break;
		case sysex::GetSample:
			dumpRequest = true;
			break;
		case sysex::Record:
			if (len >= 1) webRecord = p[0] != 0;
			break;
		case sysex::Demo:
			if (!recording)
			{
				uploading = true;
				LoadDemo();
				uploading = false;
			}
			break;
		default:
			break;
		}
	}

	int EncodeState(uint8_t *out) const
	{
		int n = sysex::Header(out, sysex::State);
		out[n++] = sysex::kVersion;
		out[n++] = uint8_t(engine);
		for (int i = 0; i < kFaders; i++) out[n++] = uint8_t((params[i] >> 5) & 0x7F);
		n += sysex::Put21(out + n, length);
		n += sysex::Put21(out + n, kCapacity);
		out[n++] = 0xF7;
		return n;
	}

	int EncodeStatus(uint8_t *out) const
	{
		int n = sysex::Header(out, sysex::Status);
		n += sysex::Put14(out + n, stSpeed);
		n += sysex::Put14(out + n, (stPitch * 100) / 256 + 4800);
		n += sysex::Put14(out + n, stSize);
		n += sysex::Put21(out + n, stHead);
		out[n++] = stFlags;
		out[n++] = 0xF7;
		return n;
	}

private:
	int32_t uploadLength = 0;

	void SendState()
	{
		uint8_t msg[sysex::kStateLen];
		Write(msg, EncodeState(msg));
	}
};


int main()
{
	set_sys_clock_khz(200000, true);

	static SampleLab card;
	card.Run();
}
