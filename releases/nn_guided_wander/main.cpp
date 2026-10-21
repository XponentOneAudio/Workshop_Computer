// Guided Wander
//
// A random pitch CV generator for the Music Thing Workshop Computer, in which
// the randomness is guided: the faders of a Music Thing 8mu are probabilities.
// Two voices wander at once, each at every step drawing a note of the chosen
// scale, an octave, a gate length and a ratchet.  Edited from the 8mu over USB
// MIDI host, or from the web editor in web/index.html.  Takes its 8mu handling
// from WaveSeq and CVSeq.
//
// The 8mu's four buttons choose the page, acted on when released; press a
// button again for its second page.
//
//             First page (both voices)     Second page (voice 2)
//   Button A  NOTES     the 8 notes of the scale, relative probabilities
//   Button B  OCTAVES   8 octaves, -2V to +5V
//   Button C  GATES     0% (a rest), then evenly up to 100%, which is tied
//   Button D  RATCHETS  1x 1.5x 2x 3x holding the note, then the same four
//                       drawing a new note, octave and gate at every hit
//
// Voice 2 uses the second page of a pair if any slider on it is above zero;
// with all of them at zero it draws from the first page too, as a second line
// with the same probabilities.  Each pair is decided on its own.
//
// Sliders are relative probabilities: a slider at zero is never chosen, one at
// twice the height of another twice as often.  Notes past the end of the
// scale (many have fewer than eight) are ignored.
//
// The 8mu's LEDs show the probabilities for 3 seconds after a slider (or a
// page) is touched, then what is playing:
//   NOTES, OCTAVES  the slider just chosen
//   GATES           a bar up to the gate length, a dim first LED for a rest
//   RATCHETS        the sliders flash their speeds in time with the clock,
//                   then the ratchet in use flashes at every hit
// After a page change the faders 'pick up': a fader only takes over once it has
// been moved to (or across) the value already stored.
//
// Panel
//   Switch up    Main = scale (32, with microtonal ones), X = clock divide or
//                multiply, Y = slew on the pitch
//   Switch mid   Main = envelope decay, X = attack, Y = pluckiness
//   Switch down  momentary; does nothing
// The six knob settings each keep their value; after the switch moves a knob
// does nothing until it reaches its new setting's value, so nothing jumps.
// LED 5 blinks fast while one is waiting.
//
// Jacks
//   Pulse In 1   Clock.  Without one the card keeps its own time, 120 BPM,
//                still divided or multiplied by X
//   CV Out 1, 2  Pitch of voice 1, 2, 1V/octave, C at 0V
//   Audio Out 1, 2  Envelope of voice 1, 2, 0 to 5V, triggered by every hit
//   Pulse Out 1, 2  Gates of voice 1, 2
//
// USB, chosen once at power-up
//   Port supplying power (an 8mu, or nothing yet): USB host, reading the 8mu.
//   Computer plugged in: USB MIDI device called "Guided Wander", for the web
//   editor (protocol in sysex.h).  An 8mu plugged into the computer is passed
//   on by the editor.  Either way the card runs on its own.

// First, so TinyUSB is configured for host and device modes before
// EightMU.h supplies its host-only defaults
#include "tusb_config.h"
#include "ComputerCard.h"
#include "EightMU.h"
#include "sysex.h"
#include "wander.h"

class GuidedWander;
static GuidedWander *gCard = nullptr;


class GuidedWander : public ComputerCard
{
	friend struct CardTest; // test/card_test.cpp
public:
	// A page is one set of eight probabilities.  The numbers are the
	// protocol's (and the web editor's presets').
	enum Page {PageNotes1, PageOctaves1, PageGates1, PageRatchets1,
		PageNotes2, PageOctaves2, PageGates2, PageRatchets2, kPages};
	static_assert(sysex::kPages == kPages, "protocol carries every page");
	static constexpr int kVoices = 2;
	static constexpr int kSliders = wander::kSliders;
	static constexpr int kControlRate = 1500;             // Control() runs 1.5kHz
	static constexpr int kTouchTicks = 3 * kControlRate;  // 3 s

	GuidedWander()
	{
		SetDefaults();

		// Give the USB power circuitry time to settle, then pick the USB
		// mode once: host if the port is supplying power (an 8mu, or nothing
		// yet), device if a computer is (the web editor).  Boards older than
		// Rev 1.1 can't tell, and are always a device.
		sleep_us(150000);
		gCard = this;
		hostMode = USBPowerState() == DFP;

		// Each power-up wanders differently
		uint32_t t = time_us_32();
		voice[0].Seed(t * 2654435761u + 12345u);
		voice[1].Seed((t ^ 0xA5A5A5A5u) * 40503u + 777u);

		wander::BuildScaleTable(scaleIndex, table);
		UpdateWeights();
		for (int v = 0; v < kVoices; v++) env[v].Set(wander::AttackSamples(settings[SetAttack]),
			wander::DecaySamples(settings[SetDecay]), PluckQ12());

		multicore_launch_core1(hostMode ? Core1Host : Core1Device);
	}

	// Default probabilities, so the card does something with no 8mu: a
	// pentatonic-friendly spread of notes, the middle octaves, gates mostly
	// short of full, mostly single hits.  Voice 2's pages are zero, so it
	// shares them.  Values are in 8mu fader units (0-127), stored shifted up
	// to 0-4064.
	void SetDefaults()
	{
		static const uint8_t def[4][kSliders] = {
			{110, 50, 80, 40, 100, 45, 55, 30},      // notes: the root and fifth
			{0, 10, 70, 127, 90, 30, 0, 0},          // octaves -2V to +5V
			{15, 20, 45, 80, 100, 60, 30, 12},       // rest, 14% ... 86%, tied
			{110, 25, 35, 15, 0, 12, 20, 8},         // ratchets
		};
		for (int p = 0; p < kPages; p++)
			for (int i = 0; i < kSliders; i++)
				params[p][i] = p < 4 ? int32_t(def[p][i]) << 5 : 0;
	}

	virtual void ProcessSample()
	{
		bool clockEdge = PulseIn1RisingEdge();

		if (++controlCount >= 32)
		{
			controlCount = 0;
			Control();
		}

		if (stepClock.Tick(clockEdge, clkNum, clkDen))
		{
			int32_t len = stepClock.StepLength(clkNum, clkDen);
			for (int v = 0; v < kVoices; v++) voice[v].BeginStep(weights[v], table, len);
			stepFlash = 2400;
		}
		else
		{
			for (int v = 0; v < kVoices; v++) voice[v].Tick(weights[v], table);
		}
		if (stepFlash > 0) stepFlash--;

		for (int v = 0; v < kVoices; v++)
		{
			if (voice[v].TakeTrigger()) env[v].Trigger();

			// Slew on the pitch
			int32_t target = voice[v].PitchMv();
			if (slewAlpha >= (1 << 24))
			{
				smoothed[v] = int64_t(target) * 65536;
			}
			else
			{
				smoothed[v] += ((int64_t(target) * 65536 - smoothed[v]) * slewAlpha) >> 24;
			}
			int32_t out = int32_t(smoothed[v] >> 16);
			if (out > 6000) out = 6000;
			if (out < -6000) out = -6000;
			pitchOut[v] = out;

			// The envelope, 0 to 5V (5V is 1704 on the audio outputs, which
			// are 349 a volt)
			int32_t e = env[v].Process();
			envLevel[v] = e;
			int16_t a = int16_t((int64_t(e) * 1704) >> 24);
			bool gate = voice[v].Gate();

			if (v == 0)
			{
				CVOut1Millivolts(out);
				AudioOut1(a);
				PulseOut1(gate);
			}
			else
			{
				CVOut2Millivolts(out);
				AudioOut2(a);
				PulseOut2(gate);
			}
		}
	}

private:
	EightMU mu;

	// Probabilities, stored as raw fader values 0-4064.  Written by core1 in
	// device mode, as the web editor sends them.
	volatile int32_t params[kPages][kSliders];
	volatile int page = PageNotes1;

	wander::StepClock stepClock;
	wander::Voice voice[kVoices];
	wander::Envelope env[kVoices];
	wander::Weights weights[kVoices];
	wander::ScaleTable table;
	int scaleIndex = 0;
	int scaleShow = 0;

	int controlCount = 0;
	int clkNum = 1, clkDen = 1;
	int stepFlash = 0;
	int32_t slewAlpha = 1 << 24;
	int64_t smoothed[kVoices] = {};
	int32_t pitchOut[kVoices] = {};
	volatile int32_t envLevel[kVoices] = {};
	bool own2[4] = {};        // voice 2 has pages of its own, by pair

	// Remembered, so the exponentials are done only when a knob moves
	int32_t lastAttack = -1, lastDecay = -1, lastPluck = -1, lastSlew = -1;
	int ctlPhase = 0;

	// 8mu paging and fader pickup
	bool latched[kSliders] = {};
	int32_t lastFader[kSliders] = {};
	bool lastFaderValid = false;
	bool prevButton[EightMU::numButtons] = {};
	bool wasConnected = false;
	int connectHoldoff = 0;
	int touchTicks = 0;       // control ticks left of showing the probabilities
	int pageBlink = 0;

	// Knob settings, with soft takeover when the switch moves.
	// Up: clock ratio (X), slew (Y), scale (Main).
	// Middle: attack (X), pluckiness (Y), decay (Main).
	enum Setting {SetClock, SetSlew, SetScale, SetAttack, SetPluck, SetDecay, kSettings};
	// Clock x1 is the middle of its zones
	int32_t settings[kSettings] = {2048, 0, 0, 300, 2048, 1600};
	static constexpr int kKnobSetting[2][3] = {
		{SetClock, SetSlew, SetScale}, {SetAttack, SetPluck, SetDecay}};
	int knobBank = -1;        // 0 = up, 1 = middle
	bool knobLatched[3] = {};
	int32_t lastKnob[3] = {};

	// USB mode, fixed at power-up
	bool hostMode = true;

	// Device mode: written on core1, read on core0
	volatile uint32_t lastPingUs = 0;
	volatile bool pinged = false;

	// Snapshot for the web editor: written on core0, read on core1
	struct VoiceSnap
	{
		volatile uint8_t noteOct = 0, gateRatchet = 0, hits = 0, flags = 0, env = 0;
		volatile int32_t pitch = 0;
	};
	VoiceSnap stVoice[kVoices];
	volatile uint8_t stFlags = 0, stFlags2 = 0, stProgress = 0;
	volatile int32_t stStepMs = 0;

	int32_t PluckQ12() const {return (settings[SetPluck] * 4096) / 4095;}

	// A knob reading (0-4095) with dead zones, as the knobs don't always
	// reach the very ends of their range: the outer kKnobEndZone at each end
	// reads as fully 0 or 4095, and the travel between is stretched to fill
	// the range.
	static constexpr int32_t kKnobEndZone = 128;
	static int32_t MapKnob(int32_t raw)
	{
		int32_t v = ((raw - kKnobEndZone) * 4095) / (4095 - 2 * kKnobEndZone);
		if (v < 0) v = 0;
		if (v > 4095) v = 4095;
		return v;
	}

	// The page a button starts on, and the button a page is on
	static int ButtonOf(int pg) {return pg & 3;}
	// A short press of button b: its second page if its first is showing (or
	// the first if the second is), otherwise its first
	static int NextPage(int pg, int b)
	{
		if (ButtonOf(pg) != b) return b;
		return pg ^ 4;
	}

	bool OwnPage(int pair) const
	{
		for (int i = 0; i < kSliders; i++) if (params[4 + pair][i] > 0) return true;
		return false;
	}

	void UpdateWeights()
	{
		for (int v = 0; v < kVoices; v++) wander::BuildWeights(params, v, table.count, weights[v]);
		for (int p = 0; p < 4; p++) own2[p] = OwnPage(p);
	}

	// Runs every 32 samples (1.5kHz)
	void Control()
	{
		HandleKnobs();

		// X: the clock ratio, in eleven equal zones
		int ri = (settings[SetClock] * wander::kNumRatios) >> 12;
		clkNum = wander::kRatios[ri].num;
		clkDen = wander::kRatios[ri].den;

		// Main (up): the scale, in 32 equal zones
		int sc = (settings[SetScale] * wander::kNumScales) >> 12;
		if (sc != scaleIndex)
		{
			scaleIndex = sc;
			wander::BuildScaleTable(scaleIndex, table);
			scaleShow = kControlRate; // show it on the LEDs for ~1s
		}

		// Slew (Y, up) and the envelope take exponentials to work out, so
		// they're done only when their knobs move, and on alternate ticks,
		// to keep the work done in any one sample down
		ctlPhase ^= 1;
		if (ctlPhase == 0 && settings[SetSlew] != lastSlew)
		{
			lastSlew = settings[SetSlew];
			slewAlpha = wander::SlewAlpha(lastSlew);
		}
		if (ctlPhase == 1 && (settings[SetAttack] != lastAttack || settings[SetDecay] != lastDecay || settings[SetPluck] != lastPluck))
		{
			lastAttack = settings[SetAttack];
			lastDecay = settings[SetDecay];
			lastPluck = settings[SetPluck];
			env[0].Set(wander::AttackSamples(lastAttack), wander::DecaySamples(lastDecay), PluckQ12());
			env[1].CopySettings(env[0]);
		}

		if (hostMode) HandleEightMU();
		UpdateWeights();

		Snapshot();
		DriveLeds();
	}

	// X and Y and Main, with soft takeover when the switch changes which
	// settings they control.  Down is momentary and keeps the settings of the
	// position it was pressed from.
	void HandleKnobs()
	{
		Switch sw = SwitchVal();
		int bank = sw == Up ? 0 : (sw == Middle ? 1 : knobBank);
		if (bank < 0) bank = 1;
		const Knob knobs[3] = {X, Y, Main};
		int32_t k[3];
		for (int i = 0; i < 3; i++) k[i] = MapKnob(KnobVal(knobs[i]));
		if (knobBank < 0)
		{
			// Power-up: the current position's settings take the knobs as
			// they are
			knobBank = bank;
			for (int i = 0; i < 3; i++)
			{
				settings[kKnobSetting[bank][i]] = k[i];
				knobLatched[i] = true;
				lastKnob[i] = k[i];
			}
			return;
		}
		if (bank != knobBank)
		{
			knobBank = bank;
			knobLatched[0] = knobLatched[1] = knobLatched[2] = false;
		}
		for (int i = 0; i < 3; i++)
		{
			int32_t &v = settings[kKnobSetting[bank][i]];
			if (!knobLatched[i])
			{
				int32_t d = k[i] - v;
				bool near = d > -48 && d < 48;
				bool crossed = (lastKnob[i] - v < 0) != (d < 0);
				if (near || crossed) knobLatched[i] = true;
			}
			if (knobLatched[i]) v = k[i];
			lastKnob[i] = k[i];
		}
	}

	//------------------------------------------------------------------------
	// The 8mu
	//------------------------------------------------------------------------

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
			for (int i = 0; i < kSliders; i++) latched[i] = false;
			touchTicks = kTouchTicks;
		}
		if (connectHoldoff > 0)
		{
			connectHoldoff--;
			return;
		}

		// Buttons, acted on when released: A-D choose the page, and again the
		// second page.  While a button is held the faders edit nothing.
		bool anyHeld = false;
		for (int b = 0; b < EightMU::numButtons; b++)
		{
			bool down = mu.Button(b);
			if (down) anyHeld = true;
			else if (prevButton[b])
			{
				page = NextPage(page, b);
				touchTicks = kTouchTicks;
				for (int i = 0; i < kSliders; i++) latched[i] = false;
			}
			prevButton[b] = down;
		}

		// Faders, with pickup, editing the current page
		for (int i = 0; i < kSliders; i++)
		{
			int32_t f = mu.Fader(i);
			volatile int32_t &p = params[page][i];
			// Any movement (an 8mu fader moves in steps of 32) is a touch
			if (lastFaderValid && !anyHeld && (f - lastFader[i] >= 32 || lastFader[i] - f >= 32))
				touchTicks = kTouchTicks;
			if (anyHeld) latched[i] = false;
			else if (!latched[i])
			{
				int32_t d = f - p;
				bool near = d > -96 && d < 96;
				bool crossed = lastFaderValid && ((lastFader[i] - p < 0) != (d < 0));
				if (near || crossed) latched[i] = true;
			}
			if (latched[i]) p = f;
			lastFader[i] = f;
		}
		lastFaderValid = true;
		if (touchTicks > 0) touchTicks--;

		DriveEightMuLeds();
	}

	// A probability as an LED, relative to the largest on the page: nothing at
	// zero, otherwise never quite dark
	static int32_t Relative(int32_t w, int32_t maxw)
	{
		if (w <= 0 || maxw <= 0) return 0;
		return 150 + ((4095 - 150) * w) / maxw;   // w <= 4064: no overflow
	}

	// A flash that fades over 100 ms after a hit
	static int32_t HitPulse(int32_t sinceHit)
	{
		const int32_t kLen = 4800;
		return sinceHit >= kLen ? 0 : (4095 * (kLen - sinceHit)) / kLen;
	}

	// How far through the step voice 1 is, 0-4095
	int32_t StepPosition() const
	{
		const wander::Voice &v = voice[0];
		int32_t len = v.StepLen();
		return int32_t((int64_t(v.SinceStep() < len ? v.SinceStep() : len - 1) * 4096) / len);
	}

	// A page's ratchet sliders flashing their speeds in time with the clock:
	// slider r flashes at each of its hits through the step (u, 0-4095), as
	// many as it plays, brighter the likelier it is
	static int32_t RatchetFlash(int r, int32_t w, int32_t maxw, int32_t u)
	{
		if (w <= 0) return 0;
		int rate2 = wander::kRate2[r & 3], hits = wander::HitCount(rate2);
		int k = (u * rate2) / 8192;
		if (k > hits - 1) k = hits - 1;
		int32_t start = (k * 2 * 4096) / rate2;
		int32_t end = k + 1 < hits ? ((k + 1) * 2 * 4096) / rate2 : 4096;
		bool on = (u - start) * 100 < (end - start) * 40;
		return on ? 600 + ((4095 - 600) * w) / maxw : 0;
	}

	void DriveEightMuLeds()
	{
		int kind = page & 3;
		int vi = page >> 2;                       // the voice whose page this is
		int32_t led[kSliders] = {};
		int32_t maxw = 0;
		for (int i = 0; i < kSliders; i++)
		{
			int32_t w = params[page][i];
			if (kind == 0 && i >= table.count) w = 0;   // past the end of the scale
			if (w > maxw) maxw = w;
		}

		if (touchTicks > 0)
		{
			// The probabilities, or for the ratchets their speeds
			int32_t u = kind == 3 ? StepPosition() : 0;
			for (int i = 0; i < kSliders; i++)
			{
				int32_t w = params[page][i];
				if (kind == 0 && i >= table.count) w = 0;
				led[i] = kind == 3 ? RatchetFlash(i, w, maxw, u) : Relative(w, maxw);
			}
		}
		else
		{
			// What is playing.  On a first page where voice 2 shares the
			// probabilities it shows too, dimmer.
			const wander::Voice &a = voice[vi];
			const wander::Voice &b = voice[1];
			bool both = vi == 0 && !own2[kind];
			switch (kind)
			{
			case 0:
				led[a.Note()] = 4095;
				if (both && led[b.Note()] < 1300) led[b.Note()] = 1300;
				break;
			case 1:
				led[a.Octave()] = 4095;
				if (both && led[b.Octave()] < 1300) led[b.Octave()] = 1300;
				break;
			case 2:
			{
				// A bar up to the gate length: the first LED dim for 0%
				int g = a.GateIndex();
				for (int i = 0; i <= g; i++) led[i] = g == 0 ? 700 : 4095;
				break;
			}
			default:
				led[a.Ratchet()] = HitPulse(a.SinceHit());
				if (both)
				{
					int32_t d = HitPulse(b.SinceHit()) / 3;
					if (led[b.Ratchet()] < d) led[b.Ratchet()] = d;
				}
				break;
			}
		}
		for (int i = 0; i < kSliders; i++) mu.SetLed(i, led[i]);
	}

	//------------------------------------------------------------------------
	// The Computer's own LEDs and the snapshot for the web editor
	//------------------------------------------------------------------------

	bool WebLinked() const
	{
		return !hostMode && pinged && (time_us_32() - lastPingUs) < 3000000;
	}

	void DriveLeds()
	{
		bool waiting = !knobLatched[0] || !knobLatched[1] || !knobLatched[2];
		bool conn = mu.Connected() || WebLinked();
		pageBlink = (pageBlink + 1) % 900;
		if (scaleShow > 0) scaleShow--;
		for (int i = 0; i < 4; i++)
		{
			if (scaleShow > 0)
			{
				// Scale number in binary, LED 0 the lowest bit
				LedOn(i, (scaleIndex >> i) & 1);
			}
			else if (conn)
			{
				// The page: steady for a first page, blinking for a second
				bool lit = page < 4 || pageBlink < 450;
				LedOn(i, ButtonOf(page) == i && lit);
			}
			else if (i < 2)
			{
				LedOn(i, voice[i].Gate());                 // the gates
			}
			else
			{
				int32_t e = envLevel[i - 2] >> 12;         // the envelopes
				LedBrightness(i, uint16_t(e > 4095 ? 4095 : e));
			}
		}
		// LED 4: each step (or the top bit of the scale number)
		LedOn(4, scaleShow > 0 ? ((scaleIndex >> 4) & 1) : stepFlash > 0);
		static int blink = 0;
		blink = (blink + 1) % 300;
		LedBrightness(5, waiting ? (blink < 150 ? 4095 : 0) : (conn ? 4095 : 0));
	}

	void Snapshot()
	{
		bool waiting = !knobLatched[0] || !knobLatched[1] || !knobLatched[2];
		stFlags = uint8_t((stepClock.Clocked() ? 1 : 0) | (mu.Connected() ? 2 : 0)
			| (knobBank == 0 ? 4 : 0) | (waiting ? 8 : 0));
		stFlags2 = uint8_t((knobLatched[0] ? 0 : 1) | (knobLatched[1] ? 0 : 2) | (knobLatched[2] ? 0 : 4));
		int32_t len = voice[0].StepLen();
		int64_t pos = voice[0].SinceStep() < len ? voice[0].SinceStep() : len - 1;
		stProgress = uint8_t((pos * 127) / len);
		stStepMs = len / (wander::kSampleRate / 1000);
		for (int v = 0; v < kVoices; v++)
		{
			const wander::Voice &o = voice[v];
			stVoice[v].noteOct = uint8_t(o.Note() | (o.Octave() << 3));
			stVoice[v].gateRatchet = uint8_t(o.GateIndex() | (o.Ratchet() << 3));
			stVoice[v].hits = o.Hits();
			stVoice[v].flags = uint8_t((o.Gate() ? 1 : 0) | (o.Tied() ? 2 : 0));
			stVoice[v].env = uint8_t((envLevel[v] >> 17) & 127);
			stVoice[v].pitch = pitchOut[v] + 8192;
		}
	}

	//------------------------------------------------------------------------
	// USB, on core1
	//------------------------------------------------------------------------

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
			if (len >= 3 && p[0] < kPages && p[1] < kSliders)
			{
				params[p[0]][p[1]] = int32_t(p[2] & 0x7F) << 5;
			}
			break;
		case sysex::SetAll:
			if (len >= 1 + sysex::kNumValues && p[0] == sysex::kVersion)
			{
				for (int i = 0; i < sysex::kNumValues; i++)
				{
					params[i / kSliders][i % kSliders] = int32_t(p[1 + i] & 0x7F) << 5;
				}
			}
			break;
		case sysex::Page:
			if (len >= 1 && p[0] < kPages) page = p[0];
			break;
		case sysex::Reset:
			SetDefaults();
			SendState();
			break;
		case sysex::Ping:
			lastPingUs = time_us_32();
			pinged = true;
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
			out[n++] = uint8_t((params[i / kSliders][i % kSliders] >> 5) & 0x7F);
		}
		out[n++] = 0xF7;
		return n;
	}

	int EncodeStatus(uint8_t *out) const
	{
		int n = sysex::Header(out, sysex::Status);
		out[n++] = stFlags;
		out[n++] = stFlags2;
		out[n++] = uint8_t(page);
		out[n++] = uint8_t(scaleIndex);
		int ri = (settings[SetClock] * wander::kNumRatios) >> 12;
		out[n++] = uint8_t(ri);
		out[n++] = uint8_t((settings[SetSlew] >> 5) & 127);
		out[n++] = uint8_t((settings[SetAttack] >> 5) & 127);
		out[n++] = uint8_t((settings[SetDecay] >> 5) & 127);
		out[n++] = uint8_t((settings[SetPluck] >> 5) & 127);
		out[n++] = stProgress;
		n += sysex::Put21(out + n, stStepMs);
		for (int v = 0; v < kVoices; v++)
		{
			out[n++] = stVoice[v].noteOct;
			out[n++] = stVoice[v].gateRatchet;
			out[n++] = stVoice[v].hits;
			out[n++] = stVoice[v].flags;
			out[n++] = stVoice[v].env;
			n += sysex::Put14(out + n, stVoice[v].pitch);
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
};


int main()
{
	set_sys_clock_khz(200000, true);

	static GuidedWander card;
	card.Run();
	return 0;
}
