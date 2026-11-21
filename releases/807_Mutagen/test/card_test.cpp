// Whole-card tests of Mutagen: main.cpp, run against stand-ins for the
// Computer, the 8mu and the USB stack (test/stubs), checking the knobs and
// their pickup, the jacks, the LEDs, the 8mu, the load guard and the web
// editor's protocol end to end.  Built by test/run_tests.sh.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

uint32_t gTimeUs = 0;
int gUsbState = 0;
uint8_t gSent[4096];
int gSentLen = 0;

// Let the tests see inside the card
#define private public
#define main card_main
#include "main.cpp"
#undef main
#undef private

static int checks = 0, failures = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t gSamples = 0;

struct Counts {int rises[2]; int audioMax[2], audioMin[2];};
static Counts Run(Mutagen &c, int n)
{
	Counts k = {{0, 0}, {-99999, -99999}, {99999, 99999}};
	bool prev[2] = {c.pulseOut[0], c.pulseOut[1]};
	for (int i = 0; i < n; i++)
	{
		c.Sample();
		gSamples++;
		gTimeUs = uint32_t((gSamples * 125) / 6);
		for (int v = 0; v < 2; v++)
		{
			if (c.pulseOut[v] && !prev[v]) k.rises[v]++;
			prev[v] = c.pulseOut[v];
			if (c.audioOut[v] > k.audioMax[v]) k.audioMax[v] = c.audioOut[v];
			if (c.audioOut[v] < k.audioMin[v]) k.audioMin[v] = c.audioOut[v];
		}
	}
	return k;
}

// The knob value a setting would need (the inverse of MapKnob)
static int32_t KnobFor(int32_t v) {return 40 + (v * (4095 - 80)) / 4095;}

static void TestPowerUpAndKnobs()
{
	Mutagen c;
	c.sw = ComputerCard::Up;
	c.knob[ComputerCard::X] = KnobFor(1000);
	c.knob[ComputerCard::Y] = KnobFor(3000);
	c.knob[ComputerCard::Main] = KnobFor(2048);
	Run(c, 200);
	// Power-up: the knobs take over at once
	CHECK(abs(c.engine.values[PageMut1][MDepth] - 1000) <= 2, "X is mutant 1 depth: %d", c.engine.values[PageMut1][MDepth]);
	CHECK(abs(c.engine.values[PageMut2][MDepth] - 3000) <= 2, "Y is mutant 2 depth: %d", c.engine.values[PageMut2][MDepth]);
	CHECK(abs(c.engine.pitchKnob - 2048) <= 2, "Main is pitch: %d", c.engine.pitchKnob);
	CHECK(!c.waiting, "nothing waiting at power-up");

	// The oscillator plays: Out 1 swings, and Pulse Out 2 (the sync out)
	// rises once a cycle, C4 at the centre: 261.6Hz
	Counts k = Run(c, 48000);
	CHECK(k.audioMax[0] > 1000 && k.audioMin[0] < -1000, "Out 1 swings: %d..%d", k.audioMin[0], k.audioMax[0]);
	CHECK(abs(k.rises[1] - 262) <= 2, "sync out at the pitch: %d a second", k.rises[1]);

	// Middle: X is now mutant 1's ratio (x1 at first), and waits for the
	// knob to reach it.  The depth stays where X left it.
	c.sw = ComputerCard::Middle;
	Run(c, 100);
	int32_t ratio = c.engine.values[PageMut1][MRatio];
	CHECK(c.waiting && !c.knobLatched[0], "X waits in the middle position");
	CHECK(c.knobLatched[2], "Main (pitch in both) doesn't wait");
	c.knob[ComputerCard::X] = KnobFor(1100);    // short of x1 (1365)
	Run(c, 100);
	CHECK(c.engine.values[PageMut1][MRatio] == ratio, "not picked up yet");
	c.knob[ComputerCard::X] = KnobFor(ratio + 200);   // crosses it
	Run(c, 100);
	CHECK(c.knobLatched[0], "picked up after crossing");
	c.knob[ComputerCard::X] = KnobFor(2500);
	Run(c, 100);
	CHECK(abs(c.engine.values[PageMut1][MRatio] - 2500) <= 2, "X sets the ratio now: %d", c.engine.values[PageMut1][MRatio]);
	CHECK(abs(c.engine.values[PageMut1][MDepth] - 1000) <= 2, "the depth kept: %d", c.engine.values[PageMut1][MDepth]);

	// Held down: Main is the wave, X and Y the modes; the LEDs show the
	// modes in binary, mutant 1 down the left
	c.engine.values[PageMut1][MMode] = uint16_t(ValueForZone(Mirror, kModes));
	c.engine.values[PageMut2][MMode] = uint16_t(ValueForZone(Sync, kModes));
	c.sw = ComputerCard::Down;
	Run(c, 400);
	int m1 = Mirror + 1, m2 = Sync + 1;
	bool ok = true;
	for (int b = 0; b < 3; b++)
	{
		ok = ok && ((c.led[2 * b] > 0) == bool((m1 >> b) & 1));
		ok = ok && ((c.led[2 * b + 1] > 0) == bool((m2 >> b) & 1));
	}
	CHECK(ok, "LEDs show the modes (%d %d / %d %d / %d %d)", c.led[0], c.led[1], c.led[2], c.led[3], c.led[4], c.led[5]);
	// A mode knob picks up as soon as it is in the same zone
	c.knob[ComputerCard::X] = KnobFor(ValueForZone(Mirror, kModes) + 100);
	Run(c, 100);
	CHECK(c.knobLatched[0], "mode knob picks up within its zone");
	c.knob[ComputerCard::X] = KnobFor(ValueForZone(Pulse, kModes));
	Run(c, 100);
	CHECK(ZoneOf(c.engine.values[PageMut1][MMode], kModes) == Pulse, "X chooses mutant 1's mode");
	// Main sets the wave (picking it up on the way past)
	c.knob[ComputerCard::Main] = KnobFor(3000);
	Run(c, 100);
	CHECK(abs(c.engine.values[PageOsc][OWave] - 3000) <= 2, "Main sets the wave: %d", c.engine.values[PageOsc][OWave]);
	// Released: Main has to pick up the pitch again
	c.sw = ComputerCard::Up;
	Run(c, 100);
	CHECK(!c.knobLatched[2], "Main waits for the pitch after the switch comes up");

	// Changed elsewhere (8mu, web): the knob lets go
	c.knob[ComputerCard::X] = KnobFor(c.engine.values[PageMut1][MDepth]);
	Run(c, 100);
	CHECK(c.knobLatched[0], "X has the depth");
	c.engine.values[PageMut1][MDepth] = 200;
	Run(c, 100);
	CHECK(c.engine.values[PageMut1][MDepth] == 200 && !c.knobLatched[0], "X lets go after the depth is changed elsewhere");
}

static void TestJacks()
{
	Mutagen c;
	Run(c, 100);
	// Pulse In 1 restarts the wave: the sync out goes high at once
	Run(c, 333);
	c.pulseIn1 = true;
	Run(c, 1);
	c.pulseIn1 = false;
	CHECK(c.pulseOut[1], "Pulse In 1 restarts the wave (sync out high)");
	CHECK(c.engine.resetFlash > 0, "reset shown");
	// Pulse In 2 restarts the LFOs
	Run(c, 20000);
	c.pulseIn2 = true;
	Run(c, 20);
	c.pulseIn2 = false;
	CHECK(c.engine.lfo[0].phase < 100000000u, "Pulse In 2 restarts the LFOs (%u)", c.engine.lfo[0].phase);
	// CV Out 1 follows LFO 1, Pulse Out 1 its square
	c.engine.values[PageLfo1][LRate] = 4095;
	c.engine.values[PageLfo1][LShape] = uint16_t(ValueForZone(LSquare, kShapes));
	Counts k = Run(c, 48000);
	CHECK(k.rises[0] > 35 && k.rises[0] < 45, "Pulse Out 1 at LFO 1's 40Hz: %d", k.rises[0]);
	CHECK(abs(c.cvRaw[0]) > 1600, "CV Out 1 is LFO 1, about 5V: %d", c.cvRaw[0]);
	// Audio In 1 only syncs while patched
	c.audioIn[0] = 1000;
	Run(c, 10);
	CHECK(!c.engine.syncing, "Audio In 1 unpatched: no sync");
	c.jack[0] = true;
	Run(c, 10);
	CHECK(c.engine.syncing, "Audio In 1 patched: sync");
}

static void TestEightMu()
{
	gUsbState = 0;   // host
	Mutagen c;
	c.mu.connected = true;
	for (int i = 0; i < 8; i++) c.mu.fader[i] = 0;
	Run(c, 32 * 1600);   // past the holdoff
	CHECK(c.connectHoldoff == 0, "holdoff over");
	// MUTANT 1's depth fader: picks up from 0 (the depth is 0 by default
	// unless a knob set it; set it here)
	c.engine.values[PageMut1][MDepth] = 0;
	Run(c, 64);
	c.mu.fader[MDepth] = 100 << 5;
	Run(c, 64);
	CHECK(c.engine.values[PageMut1][MDepth] == (100 * 4095) / 127, "8mu fader 3 sets depth: %d", c.engine.values[PageMut1][MDepth]);
	// Button B: MUTANT 2; and its LEDs show its values
	c.mu.button[1] = true;
	Run(c, 64);
	c.mu.button[1] = false;
	Run(c, 64);
	CHECK(c.ui.page == PageMut2, "button B: MUTANT 2 (%d)", c.ui.page);
	CHECK(c.led[1] == 4095 && c.led[0] == 0, "Computer LED 2 shows button B");
	CHECK(c.mu.led[MMix] > 4000, "8mu LED shows MUTANT 2 mix (full): %d", c.mu.led[MMix]);
	// The knob that shares a value with a fader must not fight it.  Switch
	// up, X is mutant 1's depth: a knob left alone still reads a count or
	// two of noise, which mustn't take the depth back from the 8mu.
	c.ui.SetPage(PageMut1);
	c.sw = ComputerCard::Up;
	c.knob[ComputerCard::X] = KnobFor(500);
	Run(c, 48000);   // set, then left alone
	c.mu.fader[MDepth] = (500 * 127 / 4095) << 5;   // where the knob put it
	Run(c, 64);
	{
		bool followed = true;
		for (int f = 16; f <= 127; f += 4)
		{
			c.mu.fader[MDepth] = f << 5;
			for (int j = 0; j < 8; j++)
			{
				c.knob[ComputerCard::X] = KnobFor(500) + ((j & 1) ? 2 : -2);   // noise
				Run(c, 32);
			}
			if (c.engine.values[PageMut1][MDepth] != (f * 4095) / 127) followed = false;
		}
		CHECK(followed, "8mu depth fader works while the X knob sits still (depth %d)", c.engine.values[PageMut1][MDepth]);
		// and turning the knob takes it back, once picked up
		c.knob[ComputerCard::X] = KnobFor(4095);   // reaches it: picked up
		Run(c, 200);
		c.knob[ComputerCard::X] = KnobFor(3000);
		Run(c, 200);
		CHECK(abs(c.engine.values[PageMut1][MDepth] - 3000) <= 2, "the knob takes over when turned: %d", c.engine.values[PageMut1][MDepth]);
	}
	// Tilt passes to the engine
	c.mu.roll = 2032;
	Run(c, 64);
	CHECK(c.engine.tiltRoll == 2032, "tilt");
	c.mu.connected = false;
	Run(c, 64);
	CHECK(c.engine.tiltRoll == 0, "tilt gone with the 8mu");
}

// Collect the SysEx messages the card sent
static int FindMessage(uint8_t cmd, const uint8_t **start)
{
	for (int i = 0; i + 4 < gSentLen; i++)
		if (gSent[i] == 0xF0 && gSent[i + 1] == 0x7D && gSent[i + 2] == 0x4D && gSent[i + 3] == cmd)
		{
			int j = i;
			while (j < gSentLen && gSent[j] != 0xF7) j++;
			*start = gSent + i;
			return j - i + 1;
		}
	return 0;
}

static void TestProtocol()
{
	gUsbState = 1;   // device: a computer
	Mutagen c;
	Run(c, 100);
	CHECK(!c.hostMode, "device mode");
	// HELLO -> STATE
	gSentLen = 0;
	c.OnSysEx(sysex::Hello, nullptr, 0);
	const uint8_t *m = gSent;
	int len = FindMessage(sysex::State, &m);
	CHECK(len == sysex::kStateLen, "STATE length %d, want %d", len, sysex::kStateLen);
	CHECK(m[4] == sysex::kVersion, "STATE version");
	CHECK(sysex::Get14(m + 6 + 2 * (PageMod * 8)) == 4095, "STATE carries the values (MOD 1 = %d)", sysex::Get14(m + 6 + 2 * (PageMod * 8)));
	// SET one value
	uint8_t set[4] = {PageOsc, OLevel, 0, 0};
	sysex::Put14(set + 2, 1234);
	c.OnSysEx(sysex::Set, set, 4);
	CHECK(c.engine.values[PageOsc][OLevel] == 1234, "SET");
	// SET_ALL
	uint8_t all[1 + 2 * sysex::kNumValues];
	all[0] = sysex::kVersion;
	for (int i = 0; i < sysex::kNumValues; i++) sysex::Put14(all + 1 + 2 * i, i * 60);
	c.OnSysEx(sysex::SetAll, all, sizeof(all));
	bool ok = true;
	for (int i = 0; i < sysex::kNumValues; i++) ok = ok && c.engine.values[i / 8][i % 8] == i * 60;
	CHECK(ok, "SET_ALL");
	// Through the parser, as bytes
	sysex::Parser parser;
	uint8_t bytes[] = {0xF0, 0x7D, 0x4D, sysex::Set, PageOsc, OLevel, 0x10, 0x00, 0xF7};
	bool got = false;
	for (uint8_t b : bytes) if (parser.Feed(b)) {got = true; c.OnSysEx(parser.cmd, parser.payload, parser.length);}
	CHECK(got && c.engine.values[PageOsc][OLevel] == (0x10 << 7), "parsed SET");
	// RESET
	c.OnSysEx(sysex::Reset, nullptr, 0);
	Run(c, 64);
	CHECK(c.engine.values[PageMod][0] == 4095 && c.engine.values[PageMut2][MMode] == ValueForZone(Sync, kModes), "RESET to the defaults");
	// PAGE
	uint8_t pg = PageLfo2;
	c.OnSysEx(sysex::Page, &pg, 1);
	Run(c, 64);
	CHECK(c.ui.page == PageLfo2, "PAGE");
	// PING: linked, STATUS built
	c.OnSysEx(sysex::Ping, nullptr, 0);
	Run(c, 64);
	CHECK(c.WebLinked(), "linked after a ping");
	uint8_t st[sysex::kStatusLen];
	CHECK(c.EncodeStatus(st) == sysex::kStatusLen, "STATUS length");
	CHECK(st[sysex::kStatusLen - 1] == 0xF7, "STATUS ends");
	// The knobs' values are in it
	c.engine.values[PageMut1][MDepth] = 3210;
	Run(c, 64);
	c.EncodeStatus(st);
	CHECK(sysex::Get14(st + 4 + 2 + 2 + 2 + 12 + 2) == 3210, "STATUS has mutant 1's depth: %d", sysex::Get14(st + 4 + 2 + 2 + 2 + 12 + 2));
	// MOTION: the tilt of an 8mu on the computer
	uint8_t mo[4];
	sysex::Put14(mo, 2048 + 1500);
	sysex::Put14(mo + 2, 2048 - 1000);
	c.OnSysEx(sysex::Motion, mo, 4);
	Run(c, 64);
	CHECK(c.engine.tiltPitch == 1500 && c.engine.tiltRoll == -1000, "MOTION: %d %d", c.engine.tiltPitch, c.engine.tiltRoll);
	// Pings stop: tilt drops
	gSamples += 48000 * 4;
	Run(c, 64);
	CHECK(!c.WebLinked() && c.engine.tiltPitch == 0, "unlinked after the pings stop");
	gUsbState = 0;
}

static void TestGuard()
{
	Mutagen c;
	Run(c, 10);
	CHECK(!c.engine.lean, "full to begin with");
	c.Guard(16);
	CHECK(c.engine.lean, "lean after a slow sample");
	for (int i = 0; i < 47000; i++) c.Guard(12);
	CHECK(c.engine.lean, "stays lean for a second");
	for (int i = 0; i < 48000; i++) c.Guard(12);
	CHECK(c.engine.lean, "stays lean while the lean patch is still busy");
	for (int i = 0; i < 2 * 48000; i++) c.Guard(6);
	CHECK(!c.engine.lean, "full again once there's room");
	// Lean still plays the right pitch
	c.engine.lean = true;
	Counts k = Run(c, 48000);
	CHECK(abs(k.rises[1] - 262) <= 2, "lean: the same pitch (%d)", k.rises[1]);
	CHECK(k.audioMax[0] > 1000, "lean: still plays");
}

int main()
{
	TestPowerUpAndKnobs();
	TestJacks();
	TestEightMu();
	TestProtocol();
	TestGuard();
	printf("card_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
