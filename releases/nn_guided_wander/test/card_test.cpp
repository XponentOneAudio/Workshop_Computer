// Host-side simulation of the whole Guided Wander card: main.cpp, run sample
// by sample against stand-ins for the Computer and the 8mu (test/stubs).
//
//   ./run_tests.sh   (stages main.cpp beside the stubs, so they stand in for the hardware)

#define main card_main
#include "main.cpp"   // staged next to the stubs by run_tests.sh
#undef main

#include <stdio.h>
#include <vector>

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
	printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static constexpr int kCtl = 32;              // samples a control tick
static constexpr int kSecond = 48000;

// A raw knob reading that MapKnob turns into the setting s (0-4095)
static int32_t RawFor(int32_t s) { return (s * (4095 - 2 * 128)) / 4095 + 128 + 1; }
static int32_t RawForZone(int zone, int zones) { return RawFor(((zone * 4096) + 2048) / zones); }

struct CardTest
{
	GuidedWander c;
	EightMU &mu;       // the 8mu stand-in, for the tests to drive
	int t = 0;

	CardTest() : mu(c.mu)
	{
		// Knobs at noon: clock x1, and so on; switch up
		c.knobRaw[0] = RawFor(0); c.knobRaw[1] = RawFor(2048); c.knobRaw[2] = RawFor(0);
		c.sw = ComputerCard::Up;
	}

	void Run(int samples, int clockPeriod = 0)
	{
		for (int i = 0; i < samples; i++, t++)
		{
			bool clk = clockPeriod > 0 && (t % clockPeriod) < 48;
			c.Sample(clk);
		}
	}
	void Seconds(double s, int clockPeriod = 0) { Run(int(s * kSecond), clockPeriod); }

	// Past the 8mu's connection hold-off and the 3 s of showing probabilities
	void Settle() { Seconds(1.2); }

	void Press(int b)
	{
		mu.buttons[b] = true; Run(kCtl * 4);
		mu.buttons[b] = false; Run(kCtl * 4);
	}

	// Move fader i from where it is to v, slowly, so it picks up on the way
	void Fader(int i, int32_t v)
	{
		int32_t f = mu.faders[i];
		int dir = v > f ? 32 : -32;
		while (f != v)
		{
			f += dir;
			if ((dir > 0 && f > v) || (dir < 0 && f < v)) f = v;
			mu.faders[i] = f;
			Run(kCtl * 2);
		}
	}

	int Page() const { return c.page; }
	int32_t Param(int p, int i) const { return c.params[p][i]; }
	const wander::Voice &V(int k) const { return c.voice[k]; }
	const wander::Weights &W(int k) const { return c.weights[k]; }
	int Scale() const { return c.scaleIndex; }
	int ClkNum() const { return c.clkNum; }
	int ClkDen() const { return c.clkDen; }
	int Touch() const { return c.touchTicks; }
	int32_t Led(int i) const { return mu.leds[i]; }
	bool Own2(int pair) const { return c.own2[pair]; }
	const wander::ScaleTable &Table() const { return c.table; }
	int Settings(int s) const { return c.settings[s]; }
	int Decay() const { return c.settings[GuidedWander::SetDecay]; }
	bool KnobLatched(int i) const { return c.knobLatched[i]; }
};

// ---------------------------------------------------------------------------
static void TestPages()
{
	CardTest k; k.Settle();
	CHECK(k.Page() == 0);
	k.Press(0); CHECK(k.Page() == 4);          // A again: second page
	k.Press(0); CHECK(k.Page() == 0);          // and back
	k.Press(1); CHECK(k.Page() == 1);          // B: its first page
	k.Press(1); CHECK(k.Page() == 5);
	k.Press(2); CHECK(k.Page() == 2);          // from B's second page, C starts on its first
	k.Press(2); CHECK(k.Page() == 6);
	k.Press(3); CHECK(k.Page() == 3);
	k.Press(3); CHECK(k.Page() == 7);
	k.Press(3); CHECK(k.Page() == 3);
	k.Press(0); CHECK(k.Page() == 0);
	// The page changes on release, not press
	k.mu.buttons[1] = true; k.Run(kCtl * 4); CHECK(k.Page() == 0);
	k.mu.buttons[1] = false; k.Run(kCtl * 4); CHECK(k.Page() == 1);
}

static void TestPickup()
{
	CardTest k; k.Settle();
	// Note 1 is stored at 110 (3520); the fader at 0 does nothing until it gets there
	int32_t stored = k.Param(0, 0);
	CHECK(stored == 110 << 5);
	k.mu.faders[0] = 0; k.Run(kCtl * 4);
	CHECK(k.Param(0, 0) == stored);
	k.Fader(0, 1600);
	CHECK(k.Param(0, 0) == stored);            // short of it: nothing yet
	k.Fader(0, 4064);                          // crossing it picks up
	CHECK(k.Param(0, 0) == 4064);
	k.Fader(0, 0);
	CHECK(k.Param(0, 0) == 0);                 // then follows
	// A page change lets go: fader 4 on page 1 doesn't touch its octave (stored
	// at the top) until it gets there
	k.Press(1);
	int32_t oct = k.Param(1, 3);
	CHECK(oct == 127 << 5);
	k.mu.faders[3] = 2000; k.Run(kCtl * 4);
	CHECK(k.Param(1, 3) == oct);
	k.Fader(3, 4064);
	CHECK(k.Param(1, 3) == 4064);
	k.Fader(3, 1000);
	CHECK(k.Param(1, 3) == 1000);
}

// ---------------------------------------------------------------------------
// Voice 2 shares the first pages while its own are all zero, and is
// independent once any slider on one of them is up, pair by pair
static void TestSharing()
{
	CardTest k; k.Settle();
	for (int p = 0; p < 4; p++) CHECK(!k.Own2(p));
	CHECK(memcmp(&k.W(0), &k.W(1), sizeof(wander::Weights)) == 0);

	// Two different lines from the same probabilities
	int same = 0, steps = 0;
	int lastHits = k.V(0).Hits();
	for (int s = 0; s < 200; s++)
	{
		k.Seconds(0.5);
		if (k.V(0).Note() == k.V(1).Note() && k.V(0).Octave() == k.V(1).Octave()) same++;
		steps++;
	}
	(void)lastHits;
	CHECK(same < steps / 2);

	// Voice 2's octaves page: one slider up, all else zero.  Only voice 2's
	// octaves change; it plays only that octave from then on.
	k.Press(1); k.Press(1); CHECK(k.Page() == 5);
	k.mu.faders[6] = 0; k.Run(kCtl * 4);
	k.Fader(6, 3000);
	CHECK(k.Param(5, 6) == 3000);
	CHECK(k.Own2(1) && !k.Own2(0) && !k.Own2(2) && !k.Own2(3));
	CHECK(memcmp(k.W(1).note, k.W(0).note, sizeof k.W(0).note) == 0);
	CHECK(memcmp(k.W(1).gate, k.W(0).gate, sizeof k.W(0).gate) == 0);
	CHECK(k.W(1).octave[6] == 3000 && k.W(1).octave[2] == 0);
	int v1Oct6 = 0, v2Oct6 = 0;
	for (int s = 0; s < 100; s++)
	{
		k.Seconds(0.5);
		if (k.V(0).Octave() == 6) v1Oct6++;
		if (k.V(1).Octave() == 6) v2Oct6++;
	}
	CHECK(v2Oct6 == 100);
	CHECK(v1Oct6 == 0);                        // voice 1's default has octave 7 at zero... and 6 too
	// Back to zero: shared again
	k.Fader(6, 0);
	CHECK(!k.Own2(1));
	CHECK(memcmp(&k.W(0), &k.W(1), sizeof(wander::Weights)) == 0);
}

// ---------------------------------------------------------------------------
// Scales of fewer than eight notes ignore the extra sliders
static void TestScaleSliders()
{
	CardTest k;
	// Major pentatonic is scale 9: 5 notes
	k.c.knobRaw[ComputerCard::Main] = RawForZone(9, wander::kNumScales);
	k.Settle();
	CHECK(k.Scale() == 9 && k.Table().count == 5);
	CHECK(k.W(0).note[5] == 0 && k.W(0).note[6] == 0 && k.W(0).note[7] == 0);
	for (int s = 0; s < 300; s++) { k.Seconds(0.5); CHECK(k.V(0).Note() < 5 && k.V(1).Note() < 5); }
	// The LEDs past the end of the scale stay dark while showing probabilities
	k.Press(0); k.Press(0);                    // back on notes 1, which counts as a touch
	CHECK(k.Touch() > 0);
	k.Run(kCtl * 4);
	CHECK(k.Led(5) == 0 && k.Led(6) == 0 && k.Led(7) == 0);
	CHECK(k.Led(0) > 0 && k.Led(4) > 0);
}

// ---------------------------------------------------------------------------
static void TestLeds()
{
	CardTest k; k.Settle();
	// Untouched for 3 s: the notes page shows the note playing
	k.Seconds(3.2);
	CHECK(k.Touch() == 0);
	k.Seconds(0.3);
	CHECK(k.Led(k.V(0).Note()) == 4095);
	int lit = 0; for (int i = 0; i < 8; i++) if (k.Led(i) > 0) lit++;
	CHECK(lit <= 2);                           // voice 1, and voice 2 dimmer (it shares the page)

	// A slider touched: the probabilities, the largest fully lit, for 3 s
	k.Fader(3, 64);
	CHECK(k.Touch() > 0);
	k.Run(kCtl * 2);
	int32_t maxw = 0, imax = 0;
	for (int i = 0; i < 7; i++) if (k.Param(0, i) > maxw) { maxw = k.Param(0, i); imax = i; }
	CHECK(k.Led(imax) == 4095);
	for (int i = 0; i < 7; i++)
	{
		if (k.Param(0, i) == 0) CHECK(k.Led(i) == 0);
		else CHECK(k.Led(i) > 0 && k.Led(i) <= 4095);
	}
	CHECK(k.Led(7) == 0);                      // Major has 7 notes: slider 8 is ignored
	k.Seconds(2.5);
	CHECK(k.Touch() > 0);
	k.Seconds(0.6);
	CHECK(k.Touch() == 0);

	// Gates: a bar up to the gate playing; 0% the first LED alone, dim
	k.Press(2); k.Seconds(3.2);
	for (int s = 0; s < 40; s++)
	{
		k.Seconds(0.25);
		int g = k.V(0).GateIndex();
		for (int i = 0; i < 8; i++)
		{
			if (i <= g) CHECK(k.Led(i) == (g == 0 ? 700 : 4095));
			else CHECK(k.Led(i) == 0);
		}
	}

	// Ratchets, after a touch: the sliders flash, on and off, in time
	k.Press(3);
	CHECK(k.Touch() > 0);
	std::vector<int> onCount(8, 0);
	for (int n = 0; n < 200; n++)
	{
		k.Run(kCtl * 8);
		for (int i = 0; i < 8; i++) if (k.Led(i) > 0) onCount[i]++;
	}
	for (int i = 0; i < 8; i++)
	{
		if (k.Param(3, i) == 0) CHECK(onCount[i] == 0);
		else CHECK(onCount[i] > 20 && onCount[i] < 180);   // flashing, not steady
	}
	// Then the ratchet in use flashes at each hit and fades
	k.Seconds(3.2);
	CHECK(k.Touch() == 0);
	int sawFlash = 0;
	for (int n = 0; n < 400; n++)
	{
		k.Run(kCtl * 4);
		int r = k.V(0).Ratchet();
		if (k.V(0).SinceHit() < 960 && k.Led(r) > 3000) sawFlash++;
		for (int i = 0; i < 8; i++) if (i != r && i != k.V(1).Ratchet()) CHECK(k.Led(i) == 0);
	}
	CHECK(sawFlash > 0);
}

// ---------------------------------------------------------------------------
static void TestPanel()
{
	CardTest k;
	// Switch up: X picks the clock ratio, eleven zones
	k.c.knobRaw[ComputerCard::X] = RawForZone(8, 11);
	k.Settle();
	CHECK(k.ClkNum() == 4 && k.ClkDen() == 1);
	k.c.knobRaw[ComputerCard::X] = RawForZone(0, 11); k.Run(kCtl * 4);
	CHECK(k.ClkNum() == 1 && k.ClkDen() == 8);
	k.c.knobRaw[ComputerCard::X] = RawForZone(5, 11); k.Run(kCtl * 4);
	CHECK(k.ClkNum() == 1 && k.ClkDen() == 1);
	// Main: the scale
	for (int z = 0; z < wander::kNumScales; z++)
	{
		k.c.knobRaw[ComputerCard::Main] = RawForZone(z, wander::kNumScales); k.Run(kCtl * 2);
		CHECK(k.Scale() == z);
	}
	// Switch to the middle: the knobs wait to pick up their own settings
	int scaleWas = k.Scale();
	k.c.sw = ComputerCard::Middle; k.Run(kCtl * 4);
	CHECK(!k.KnobLatched(2));
	CHECK(k.Scale() == scaleWas);
	// Main is near the top (the last scale); decay is stored at 1600
	int32_t decay = k.Decay();
	CHECK(decay == 1600);
	k.c.knobRaw[ComputerCard::Main] = RawFor(3000); k.Run(kCtl * 4);
	CHECK(k.Decay() == decay);     // not reached yet
	k.c.knobRaw[ComputerCard::Main] = RawFor(2000); k.Run(kCtl * 4);
	CHECK(k.Decay() == decay);
	k.c.knobRaw[ComputerCard::Main] = RawFor(1000); k.Run(kCtl * 4);   // across it
	CHECK(k.KnobLatched(2) && k.Decay() >= 998 && k.Decay() <= 1002);
	k.c.knobRaw[ComputerCard::Main] = RawFor(4095); k.Run(kCtl * 4);
	CHECK(k.Decay() == 4095);
	CHECK(k.Scale() == scaleWas);                            // the scale untouched
	// Switch down is momentary and leaves the middle's settings in place
	k.c.sw = ComputerCard::Down; k.Run(kCtl * 4);
	CHECK(k.Decay() == 4095);
}

// ---------------------------------------------------------------------------
static void TestOutputs()
{
	CardTest k;
	k.c.knobRaw[ComputerCard::Y] = RawFor(0);                // no slew
	k.Settle();
	k.Seconds(1);
	// Pitch outs follow each voice; pulse outs are its gate; audio its envelope
	int gateHigh[2] = {}, envSeen[2] = {};
	for (int n = 0; n < 48000 * 6; n++)
	{
		k.Run(1);
		for (int v = 0; v < 2; v++)
		{
			CHECK(k.c.cv[v] == k.V(v).PitchMv());
			CHECK(k.c.pulseOut[v] == k.V(v).Gate());
			if (k.c.pulseOut[v]) gateHigh[v]++;
			if (k.c.audio[v] > 0) envSeen[v]++;
			CHECK(k.c.audio[v] >= 0 && k.c.audio[v] <= 1705);
		}
	}
	CHECK(gateHigh[0] > 0 && gateHigh[1] > 0);
	CHECK(envSeen[0] > 0 && envSeen[1] > 0);

	// With slew, the pitch glides rather than jumps
	CardTest s;
	s.c.knobRaw[ComputerCard::Y] = RawFor(2800);
	s.Settle();
	int32_t maxJump = 0, prev = s.c.cv[0];
	for (int n = 0; n < 48000 * 4; n++)
	{
		s.Run(1);
		int32_t d = s.c.cv[0] - prev; if (d < 0) d = -d;
		if (d > maxJump) maxJump = d;
		prev = s.c.cv[0];
	}
	CHECK(maxJump < 20);
}

// ---------------------------------------------------------------------------
static void TestClockIn()
{
	CardTest k;
	k.c.knobRaw[ComputerCard::X] = RawForZone(5, 11);        // x1
	k.Settle();
	// A clock every 10000 samples: one step each
	const int P = 10000;
	k.Run(P * 3, P);
	int starts = 0;
	int32_t last = k.V(0).SinceStep();
	for (int n = 0; n < P * 20; n++)
	{
		k.Run(1, P);
		if (k.V(0).SinceStep() < last) starts++;
		last = k.V(0).SinceStep();
	}
	CHECK(starts >= 19 && starts <= 21);
	CHECK(k.V(0).StepLen() == P);
	// x2: two steps a clock
	k.c.knobRaw[ComputerCard::X] = RawForZone(6, 11);
	k.Run(P * 3, P);
	starts = 0; last = k.V(0).SinceStep();
	for (int n = 0; n < P * 20; n++)
	{
		k.Run(1, P);
		if (k.V(0).SinceStep() < last) starts++;
		last = k.V(0).SinceStep();
	}
	CHECK(starts >= 39 && starts <= 41);
}

// ---------------------------------------------------------------------------
static void TestSysEx()
{
	CardTest k; k.Settle();
	uint8_t msg[sysex::kStatusLen + 8];
	int n = k.c.EncodeStatus(msg);
	CHECK(n == sysex::kStatusLen);
	CHECK(msg[0] == 0xF0 && msg[n - 1] == 0xF7);
	for (int i = 1; i < n - 1; i++) CHECK(msg[i] < 0x80);

	uint8_t st[sysex::kStateLen + 8];
	n = k.c.EncodeState(st);
	CHECK(n == sysex::kStateLen);
	for (int i = 1; i < n - 1; i++) CHECK(st[i] < 0x80);

	// SET and SET_ALL from the editor
	uint8_t set[3] = {6, 2, 99};
	k.c.OnSysEx(sysex::Set, set, 3);
	CHECK(k.Param(6, 2) == 99 << 5);
	k.Run(kCtl * 2);
	CHECK(k.Own2(2));
	uint8_t all[1 + sysex::kNumValues];
	all[0] = sysex::kVersion;
	for (int i = 0; i < sysex::kNumValues; i++) all[1 + i] = uint8_t(i);
	k.c.OnSysEx(sysex::SetAll, all, sizeof all);
	for (int i = 0; i < sysex::kNumValues; i++) CHECK(k.Param(i / 8, i % 8) == (i << 5));
	// A different version is refused
	all[0] = sysex::kVersion + 1; all[1] = 77;
	k.c.OnSysEx(sysex::SetAll, all, sizeof all);
	CHECK(k.Param(0, 0) == 0);
	// The parser takes a whole message out of a byte stream
	sysex::Parser p;
	const uint8_t stream[] = {0x90, 0x01, 0xF0, 0x7D, 0x47, 0x03, 0x01, 0x02, 0x03, 0xF7};
	int got = 0;
	for (uint8_t b : stream) if (p.Feed(b)) { got++; CHECK(p.cmd == 0x03 && p.length == 3 && p.payload[2] == 3); }
	CHECK(got == 1);
	// RESET: defaults back, voice 2 shares again
	k.c.OnSysEx(sysex::Reset, nullptr, 0);
	k.Run(kCtl * 2);
	for (int pr = 0; pr < 4; pr++) CHECK(!k.Own2(pr));
}

int main()
{
	TestPages();
	TestPickup();
	TestSharing();
	TestScaleSliders();
	TestLeds();
	TestPanel();
	TestOutputs();
	TestClockIn();
	TestSysEx();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
