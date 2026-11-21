// Mutagen
//
// An oscillator for the Music Thing Workshop Computer that plays with pulse
// width and sync, after the mutants of a certain digital synth: its wave goes
// through two "mutants" in series that bend its timing - pulse width for any
// wave, squeezing, mirroring, a warp drawn with eight faders, a phase
// difference that makes pulses from saws, and hard sync - each with a ratio
// (how many times a cycle it happens), depth, window, feedback and mix.
// Two LFOs, CV In 2, Audio In 2 and an 8mu's tilt modulate them.
//
// The sound is in mutagen.h and the 8mu's pages in ui.h; this file connects
// them to the Computer's knobs, jacks and USB.
//
// The 8mu (buttons A-D choose the page; press a button again for its second)
//
//             First page                Second page
//   Button A  MUTANT 1                  WARP 1 (mutant 1's curve)
//   Button B  MUTANT 2                  WARP 2
//   Button C  OSC                       MOD (CV In 2, Audio In 2, tilt)
//   Button D  LFO 1                     LFO 2
//
//   MUTANT  mode, ratio, depth, window, feedback, mix, LFO 1 > depth, LFO 2 > depth
//   OSC     wave, semitones, fine, bit crush, drive, Out 2, glide, level
//   LFO     rate (first third: synced to Pulse In 1), shape, > ratio 1, > ratio 2, > wave, > pitch, > mix 1, > mix 2
//   MOD     CV 2 > depth 1, > depth 2, > ratio 1, > ratio 2,
//           Audio 2 > depth 1, > depth 2, tilt > depth 1, > depth 2
//   WARP    eight points of a curve that pushes the phase
//
// After a page change the faders 'pick up': one only takes over once it has
// been moved to (or across) the value already stored; the same when a knob or
// the web editor has changed it.  The 8mu's LEDs show the values.
//
// Panel
//   Switch up    Main = pitch, X = mutant 1 depth, Y = mutant 2 depth
//   Switch mid   Main = pitch, X = mutant 1 ratio, Y = mutant 2 ratio
//   Switch down  (held) Main = wave, X = mutant 1 mode, Y = mutant 2 mode
// The X and Y settings are the same values as the 8mu's faders.  After the
// switch moves (or a fader changes the value), a knob does nothing until it
// reaches the value.
//
// Inputs
//   CV In 1      pitch, 1V/oct
//   CV In 2      modulation (MOD page; at first, mutant 1's depth)
//   Audio In 1   hard sync from another oscillator (while patched)
//   Audio In 2   audio-rate modulation of the depths (MOD page)
//   Pulse In 1   clock, for the LFOs' tempo sync (the first third of their
//                RATE faders: 16 beats a cycle down to an eighth of a beat)
//   Pulse In 2   restart the LFOs
// Outputs
//   Audio Out 1  the oscillator
//   Audio Out 2  the same, or dry, or after mutant 1 only, or a sub
//                oscillator an octave or two down (OSC page)
//   CV Out 1, 2  LFO 1, 2 (+/- 5V)
//   Pulse Out 1  LFO 1's square
//   Pulse Out 2  the oscillator's square: a sync out
//
// USB, chosen once at power-up
//   Port supplying power (an 8mu, or nothing yet): USB host, reading the 8mu.
//   Computer plugged in: USB MIDI device called "Mutagen", for the web editor
//   (protocol in sysex.h).  An 8mu plugged into the computer is passed on by
//   the editor.  Either way the card runs on its own.

// First, so TinyUSB is configured for host and device modes before
// EightMU.h supplies its host-only defaults
#include "tusb_config.h"
#include "ComputerCard.h"
#include "EightMU.h"
#include "sysex.h"
#include "mutagen.h"
#include "ui.h"

using namespace mutagen;

class Mutagen;
static Mutagen *gCard = nullptr;

// The wave tables: 55kB, built at power-up
static Tables gTables;


class Mutagen : public ComputerCard
{
public:
	Mutagen()
	{
		// Give the USB power circuitry time to settle, then pick the USB
		// mode once: host if the port is supplying power (an 8mu, or nothing
		// yet), device if a computer is (the web editor).  Boards older than
		// Rev 1.1 can't tell, and are always a device.
		sleep_us(150000);
		gCard = this;
		// Lets Connected() tell whether a jack is in Audio In 1 and 2
		EnableNormalisationProbe();
		gTables.Build();
		engine.Begin(&gTables);
		hostMode = USBPowerState() == DFP;
		multicore_launch_core1(hostMode ? Core1Host : Core1Device);
	}

	// Runs 48000 times a second, in the audio interrupt.  It must finish
	// well inside the ~20us between samples.  The sound is made every
	// sample; the slower control work (knobs, 8mu, LEDs, the web editor's
	// status) is split into four parts, each run every 32 samples on a
	// different sample, and only on the samples the engine leaves free for
	// it (see Engine::Tick: 2, 6, 10 and 14 of every 16, counting from the
	// first).
	virtual void ProcessSample()
	{
		uint32_t startUs = time_us_32();

		Inputs in;
		in.cv1 = CVIn1();
		in.cv2 = CVIn2();
		in.syncPatched = Connected(Audio1);
		in.audio1 = in.syncPatched ? AudioIn1() : 0;
		in.audio2 = Connected(Audio2) ? AudioIn2() : 0;
		in.clock = PulseIn1();
		in.lfoReset = PulseIn2RisingEdge();
		engine.Tick(in);

		AudioOut1(engine.out1);
		AudioOut2(engine.out2);
		CVOut1(engine.cv1);
		CVOut2(engine.cv2);
		PulseOut1(engine.pulse1);
		PulseOut2(engine.pulse2);

		int part = 0;
		switch (controlCount)
		{
		case 2: ControlKnobs(); part = 1; break;
		case 10: if (hostMode) EightMuInput(); part = 2; break;
		case 18: ControlLeds(); part = 3; break;
		case 26: if (WebLinked()) BuildStatus(); part = 4; break;
		default: break;
		}
		controlCount = (controlCount + 1) & 31;

		// The longest this has taken over the last second, for the web
		// editor: overall, and for each part of the work
		uint32_t us = time_us_32() - startUs;
		if (us > busiestUs) busiestUs = us;
		// (the LEDs and the status share a part)
		if (part == 4) part = 3;
		if (us > busiestPart[part]) busiestPart[part] = us;
		Guard(us);
		if (++busySamples >= 48000)
		{
			busySamples = 0;
			shownBusiestUs = busiestUs;
			busiestUs = 0;
			for (int i = 0; i < 4; i++) {shownPart[i] = busiestPart[i]; busiestPart[i] = 0;}
		}
	}

private:
	Engine engine;

	// The load guard.  If a patch takes too long to make - every sample
	// must be done in well under 20us - the engine goes lean (Out 1 made
	// once a sample instead of twice, about two thirds of the work) and
	// stays so for at least a second; it goes back when the lean patch
	// takes little enough time that the full one would fit again.
	static constexpr uint32_t kTooBusyUs = 15, kRoomUs = 9;
	uint32_t guardMax = 0;
	int32_t leanHold = 0;
	void Guard(uint32_t us)
	{
		if (us > guardMax) guardMax = us;
		if (!engine.lean)
		{
			if (us >= kTooBusyUs)
			{
				engine.lean = true;
				leanHold = 48000;
				guardMax = 0;
			}
			return;
		}
		if (--leanHold > 0) return;
		if (guardMax <= kRoomUs) engine.lean = false;
		else leanHold = 48000;
		guardMax = 0;
	}
	EightMuUi ui;
	EightMU mu;

	int controlCount = 0;
	bool waiting = false;     // a knob is waiting to pick up its setting
	uint32_t busiestUs = 0, shownBusiestUs = 0;
	uint32_t busiestPart[4] = {}, shownPart[4] = {};
	int32_t busySamples = 0;

	//------------------------------------------------------------------------
	// Knobs
	//------------------------------------------------------------------------

	// What each knob sets in each switch position: a fader value (page and
	// fader), or the pitch (page -1)
	struct Target {int page, fader;};
	static constexpr int kPitch = -1;
	static constexpr Target kTargets[3][3] = {
		// X                    Y                    Main
		{{PageMut1, MDepth}, {PageMut2, MDepth}, {kPitch, 0}},      // up
		{{PageMut1, MRatio}, {PageMut2, MRatio}, {kPitch, 0}},      // middle
		{{PageMut1, MMode},  {PageMut2, MMode},  {PageOsc, OWave}}, // down
	};
	int position = -1;        // 0 up, 1 middle, 2 down
	bool knobLatched[3] = {};
	int32_t lastKnob[3] = {};
	int32_t knobWrote[3] = {};
	// Counts more than noise, and how long after the last such move a knob
	// still writes (control ticks at 1.5kHz: half a second)
	static constexpr int32_t kKnobMoved = 24, kTurningTicks = 750;
	int32_t knobTurning[3] = {};
	int32_t knobFrom[3] = {};    // where the knob was when it last wrote, or picked up

	// A dead zone at each end, so the whole range is reached
	static int32_t MapKnob(int32_t k)
	{
		int32_t v = ((k - 40) * 4095) / (4095 - 80);
		return v < 0 ? 0 : (v > 4095 ? 4095 : v);
	}

	int32_t Get(const Target &t) const
	{
		return t.page == kPitch ? engine.pitchKnob : int32_t(engine.values[t.page][t.fader]);
	}
	void Put(const Target &t, int32_t v)
	{
		if (t.page == kPitch) engine.pitchKnob = v;
		else engine.values[t.page][t.fader] = uint16_t(v);
	}

	void ControlKnobs()
	{
		if (resetRequest)
		{
			resetRequest = false;
			engine.DefaultValues();
		}
		if (webPage >= 0)
		{
			ui.SetPage(webPage);
			webPage = -1;
		}
		if (!hostMode)
		{
			// The tilt of an 8mu on the computer, while the editor is there
			bool linked = WebLinked();
			engine.tiltPitch = linked ? webPitch : 0;
			engine.tiltRoll = linked ? webRoll : 0;
		}

		Switch sw = SwitchVal();
		int pos = sw == Up ? 0 : (sw == Middle ? 1 : 2);
		const Knob knobs[3] = {X, Y, Main};
		int32_t k[3];
		for (int i = 0; i < 3; i++) k[i] = MapKnob(KnobVal(knobs[i]));

		if (position < 0)
		{
			// Power-up: the knobs take over straight away
			position = pos;
			for (int i = 0; i < 3; i++)
			{
				Put(kTargets[pos][i], k[i]);
				knobLatched[i] = true;
				knobWrote[i] = knobFrom[i] = k[i];
				lastKnob[i] = k[i];
			}
			return;
		}
		if (pos != position)
		{
			for (int i = 0; i < 3; i++)
			{
				const Target &a = kTargets[position][i], &b = kTargets[pos][i];
				if (a.page != b.page || a.fader != b.fader) knobLatched[i] = false;
			}
			position = pos;
		}
		for (int i = 0; i < 3; i++)
		{
			const Target &t = kTargets[pos][i];
			int32_t v = Get(t);
			// Changed by an 8mu fader or the web editor: let go
			if (knobLatched[i] && v != knobWrote[i]) knobLatched[i] = false;
			if (!knobLatched[i])
			{
				int32_t d = k[i] - v;
				bool near = d > -48 && d < 48;
				bool crossed = (lastKnob[i] - v < 0) != (d < 0);
				int n = t.page == kPitch ? 0 : ZonesOf(t.page, t.fader);
				if (n && ZoneOf(k[i], n) == ZoneOf(v, n)) near = true;
				if (near || crossed)
				{
					// Picked up, but it writes only once turned on from
					// here: a fader sweeping past an idle knob mustn't wake it
					knobLatched[i] = true;
					knobWrote[i] = v;
					knobFrom[i] = k[i];
				}
			}
			if (knobLatched[i])
			{
				// A knob that shares its value with an 8mu fader (or the
				// web editor) only writes while it is being turned: one left
				// alone still reads a count or two of noise, and writing
				// that would take the value back from the fader each time
				// the fader moved, the two taking turns.  The pitch shares
				// with nothing, so Main follows every count.
				bool shared = t.page != kPitch;
				int32_t moved = k[i] - knobFrom[i];
				if (!shared || moved >= kKnobMoved || moved <= -kKnobMoved) knobTurning[i] = kTurningTicks;
				if (knobTurning[i] > 0)
				{
					knobTurning[i]--;
					if (v != k[i]) Put(t, k[i]);
					knobWrote[i] = knobFrom[i] = k[i];
				}
			}
			lastKnob[i] = k[i];
		}
		waiting = !knobLatched[0] || !knobLatched[1] || !knobLatched[2];
	}

	//------------------------------------------------------------------------
	// LEDs: the 8mu's (when it's on the card) and the Computer's
	//------------------------------------------------------------------------
	int blink = 0;

	void ControlLeds()
	{
		if (hostMode && mu.Connected() && connectHoldoff == 0)
		{
			int32_t led[kFaders];
			ui.Leds(engine, led);
			for (int i = 0; i < kFaders; i++) mu.SetLed(i, led[i]);
		}

		// The Computer's LEDs, top to bottom in pairs: 1 2 / 3 4 / 5 6.
		// Switch held down: the two modes in binary, mutant 1 down the left
		// (LEDs 1, 3, 5, the lowest bit at the top) and mutant 2 down the
		// right.  Otherwise LEDs 1-4 show the 8mu page's button (A B / C D,
		// lit for its first page, blinking for its second), or with no 8mu
		// or editor, the two depths (1, 2), sync at Audio In 1 (3) and a
		// clock at Pulse In 1 (4).  LEDs 5 and 6 follow the two LFOs;
		// LED 6 blinks fast while a knob is waiting to pick up.
		blink = (blink + 1) % 600;
		if (position == 2)
		{
			int m1 = engine.stage[0].mode + 1, m2 = engine.stage[1].mode + 1;
			for (int b = 0; b < 3; b++)
			{
				LedOn(2 * b, (m1 >> b) & 1);
				LedOn(2 * b + 1, (m2 >> b) & 1);
			}
			return;
		}
		bool conn = mu.Connected() || WebLinked();
		if (conn)
		{
			int button = ButtonOf(ui.page), layer = LayerOf(ui.page);
			for (int i = 0; i < 4; i++) LedOn(i, button == i && (layer == 0 || blink % 300 < 150));
		}
		else
		{
			LedBrightness(0, uint16_t(engine.stage[0].ShownDepth() >> 3));
			LedBrightness(1, uint16_t(engine.stage[1].ShownDepth() >> 3));
			LedOn(2, engine.syncing);
			LedOn(3, engine.clockFlash > 0);
		}
		LedBrightness(4, uint16_t((engine.lfo[0].out + 32768) >> 4));
		if (waiting) LedOn(5, blink % 150 < 75);
		else LedBrightness(5, uint16_t((engine.lfo[1].out + 32768) >> 4));
	}

	//------------------------------------------------------------------------
	// The 8mu on the card: its buttons, faders and tilt
	//------------------------------------------------------------------------
	bool wasConnected = false;
	int connectHoldoff = 0;

	void EightMuInput()
	{
		if (!mu.Connected())
		{
			wasConnected = false;
			engine.tiltPitch = engine.tiltRoll = 0;
			return;
		}
		if (!wasConnected)
		{
			// Wait for the 8mu's reply to the fader position query before
			// trusting fader values
			wasConnected = true;
			connectHoldoff = 1500; // ~1s at this 1.5kHz rate
			ui.Reset();
		}
		if (connectHoldoff > 0)
		{
			connectHoldoff--;
			return;
		}

		int32_t f[kFaders];
		bool b[kButtons];
		for (int i = 0; i < kFaders; i++) f[i] = mu.Fader(i) >> 5;   // 0-127
		for (int i = 0; i < kButtons; i++) b[i] = mu.Button(i);
		ui.Update(engine, f, b);
		engine.tiltPitch = mu.Pitch();
		engine.tiltRoll = mu.Roll();
	}

	//------------------------------------------------------------------------
	// STATUS for the web editor (only built while one is listening)
	//------------------------------------------------------------------------
	volatile uint8_t status[sysex::kStatusPayload] = {};

	static uint8_t Bip7(int32_t q)   // -32767 ... 32767 as 0 ... 127
	{
		int32_t v = (q + 32768) >> 9;
		return uint8_t(v < 0 ? 0 : (v > 127 ? 127 : v));
	}

	void Put14At(int &n, int32_t v)
	{
		if (v < 0) v = 0;
		if (v > 16383) v = 16383;
		status[n++] = uint8_t(v >> 7);
		status[n++] = uint8_t(v & 127);
	}

	void BuildStatus()
	{
		int n = 0;
		status[n++] = uint8_t((mu.Connected() ? 1 : 0) | (position == 0 ? 2 : 0) | (position == 2 ? 4 : 0)
			| (waiting ? 8 : 0) | (engine.syncing ? 16 : 0) | (Connected(Audio2) ? 32 : 0) | (engine.lean ? 64 : 0));
		status[n++] = uint8_t((knobLatched[0] ? 0 : 1) | (knobLatched[1] ? 0 : 2) | (knobLatched[2] ? 0 : 4)
			| (engine.clocked ? 8 : 0));
		Put14At(n, engine.pitch >> 6);
		Put14At(n, engine.wavePos);
		for (int s = 0; s < 2; s++)
		{
			const Stage &st = engine.stage[s];
			status[n++] = uint8_t(st.mode);
			Put14At(n, st.ShownDepth() >> 2);
			Put14At(n, st.ShownRatioLog() >> 5);
			int32_t m = st.ShownMix() >> 8;
			status[n++] = uint8_t(m > 127 ? 127 : m);
		}
		status[n++] = Bip7(engine.lfo[0].out);
		status[n++] = Bip7(engine.lfo[1].out);
		for (int s = 0; s < 2; s++) Put14At(n, engine.values[s][MDepth]);
		for (int s = 0; s < 2; s++) Put14At(n, engine.values[s][MRatio]);
		for (int s = 0; s < 2; s++) Put14At(n, engine.values[s][MMode]);
		Put14At(n, engine.values[PageOsc][OWave]);
		status[n++] = Bip7(engine.tiltQ[0]);
		status[n++] = Bip7(engine.tiltQ[1]);
		status[n++] = uint8_t(shownBusiestUs > 127 ? 127 : shownBusiestUs);
		for (int i = 0; i < 4; i++) status[n++] = uint8_t(shownPart[i] > 127 ? 127 : shownPart[i]);
		Put14At(n, int32_t(engine.clockPeriod / 48));
	}

	//------------------------------------------------------------------------
	// USB, on core1
	//------------------------------------------------------------------------

	// USB mode, fixed at power-up
	bool hostMode = true;

	// Device mode: written on core1, read on core0
	volatile int webPage = -1;
	volatile bool resetRequest = false;
	volatile uint32_t lastPingUs = 0;
	volatile bool pinged = false;
	volatile int32_t webPitch = 0, webRoll = 0;

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
		uint32_t lastStatusUs = 0;
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

public:
	// Handle one message from the web editor.  Public so it can be tested.
	void OnSysEx(uint8_t cmd, const uint8_t *p, int len)
	{
		switch (cmd)
		{
		case sysex::Hello:
			SendState();
			break;
		case sysex::Set:
			if (len >= 4 && p[0] < kPages && p[1] < kFaders)
			{
				int32_t v = sysex::Get14(p + 2);
				engine.values[p[0]][p[1]] = uint16_t(v > 4095 ? 4095 : v);
			}
			break;
		case sysex::SetAll:
			if (len >= 1 + 2 * sysex::kNumValues && p[0] == sysex::kVersion)
			{
				for (int i = 0; i < sysex::kNumValues; i++)
				{
					int32_t v = sysex::Get14(p + 1 + 2 * i);
					engine.values[i / kFaders][i % kFaders] = uint16_t(v > 4095 ? 4095 : v);
				}
			}
			break;
		case sysex::Page:
			if (len >= 1 && p[0] < kPages) webPage = p[0];
			break;
		case sysex::Reset:
			engine.DefaultValues();
			resetRequest = true; // and again on core0, in case of a race
			SendState();
			break;
		case sysex::Ping:
			lastPingUs = time_us_32();
			pinged = true;
			break;
		case sysex::Motion:
			if (len >= 4)
			{
				webPitch = sysex::Get14(p) - 2048;
				webRoll = sysex::Get14(p + 2) - 2048;
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
		out[n++] = uint8_t(ui.page);
		for (int i = 0; i < sysex::kNumValues; i++)
			n += sysex::Put14(out + n, engine.values[i / kFaders][i % kFaders]);
		out[n++] = 0xF7;
		return n;
	}

	int EncodeStatus(uint8_t *out) const
	{
		int n = sysex::Header(out, sysex::Status);
		for (int i = 0; i < sysex::kStatusPayload; i++) out[n++] = status[i] & 0x7F;
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

	static Mutagen card;
	card.Run();
	return 0;
}
