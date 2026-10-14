// MatrixVerb
//
// A reverb laboratory for the Music Thing Workshop Computer: a feedback delay
// network (FDN) of eight delay lines, whose 8x8 feedback matrix is laid out
// on a Music Thing 8mu, one row per page, for direct control.  It's for
// exploring how the matrix, the delay lengths and the usual reverb controls
// shape the network's sound, rather than an everyday reverb, but it's made
// to be safe whatever the settings: see fdn.h for the layers of protection.
// Edited from an 8mu, over USB MIDI host, or from the web editor in
// web/index.html.
//
// The 8mu's eight faders are the eight lines (or, on the matrix pages, the
// eight lines a row takes from).  Its four buttons, acted on when released:
//
//   Short press  the page, below.  Pressing a button again moves on to its
//                next page, and round to its first.  A steps through the
//                eight matrix rows; coming back to A returns to the row last
//                shown.  The Computer's top-left LED flashes the row's number:
//                once for row 1, six times for row 6, then a pause.
//   Long press   (half a second or more) A: the previous row.  C: clear the
//                network.  D: freeze on or off.
//   Hold A + move a fader   load card preset 1-8 (the fader's number)
//   Hold B + move a fader   load matrix template 1-8 (design.h)
//   Hold C + move a fader   a click into that line, to hear how the matrix
//                           spreads it
//   Hold D + move a fader   save the settings as card preset 1-8
//
//             First page                Second page        Third page
//   Button A  ROW 1 ... ROW 8 of the matrix, one page each
//   Button B  LENGTH   each line's delay
//   Button C  REVERB   (below)          LAB (below)
//   Button D  INPUT    each line's      OUTPUT  each       PAN  each line's
//                      share of the in  line's share out   place
//
//   REVERB  1 predelay  2 decay  3 damping  4 low cut  5 tone  6 mod depth
//           7 width  8 mix
//   LAB     1 normalise  2 blend  3 drive  4 diffuse  5 mod rate
//           6 input level  7 output level  8 ceiling
//
// Delay lengths are always pairwise coprime: each line is snapped to the
// nearest length that shares no factor with the lines before it.
//
// After a page change the faders 'pick up': a fader only takes over once it
// has been moved to (or across) the value already stored.  The 8mu's LEDs
// show the stored values on the current page (on the two-sided pages, how
// far from the centre).
//
// Panel
//   Main        decay multiplier, x1/4 to x4 (x1 at the centre)
//   X           size: every delay length x1/4 to x4 (x1 at the centre),
//               snapped coprime again, the lines gliding to their new
//               lengths
//   Y           damping offset: darker (down) or brighter (up)
//   Switch up   freeze: the input is cut, and the network holds what's in it
//   Switch mid  run
//   Switch down momentary: clear.  The wet signal is muted and the network
//               emptied while it's held, then fades back in
//
// Inputs
//   Audio In 1, 2  left and right; with only Audio In 1 patched, it feeds
//               both sides
//   CV In 1     decay, 1V/oct (added to Main)
//   CV In 2     size, 1V/oct (added to X)
//   Pulse In 1  a click into the network, through the input vector
//   Pulse In 2  freeze, while high
//
// Outputs
//   Audio Out 1, 2  left and right, dry and wet as the Mix fader sets
//   CV Out 1    envelope of the wet signal, 0 to 5V
//   CV Out 2    safety: how much the limiter and guard are turning the wet
//               signal down, 0V (not at all) to 5V
//   Pulse Out 1 high while the limiter or guard is acting
//   Pulse Out 2 high while frozen
//
// 8mu motion
//   Pitch (tilt front/back)  decay multiplier, x1/4 to x4, on top of Main
//   Roll (tilt left/right)   damping offset, on top of Y
//   Both ignore about 15 degrees either side of flat
//
// Presets live in the last 4kB sector of the program card's flash, which
// every card has, so it needs no more than a 2MB card.
//
// USB, chosen once at power-up
//   Port supplying power (an 8mu, or nothing yet): USB host, reading the 8mu.
//   Computer plugged in: USB MIDI device called "MatrixVerb", for the web
//   editor (protocol in sysex.h).  An 8mu plugged into the computer is passed
//   on by the editor.  Either way the card runs on its own.

// First, so TinyUSB is configured for host and device modes before
// EightMU.h supplies its host-only defaults
#include "tusb_config.h"
#include "ComputerCard.h"
#include "EightMU.h"
#include "sysex.h"
#include "design.h"
#include "fdn.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/systick.h"
#include <string.h>

class MatrixVerb;
static MatrixVerb *gCard = nullptr;

static_assert(sysex::kNumValues == mv::kNumValues, "protocol carries every page");

// Presets, kept in the last sector of flash.  The same layout is kept in
// RAM, and written back whenever it changes.
struct PresetStore
{
	uint32_t magic;
	uint8_t version;
	uint8_t lastSlot;          // sysex::kNoSlot for none
	uint8_t pad[2];
	uint8_t valid[sysex::kSlots];
	uint8_t values[sysex::kSlots][sysex::kNumValues];
};
static constexpr uint32_t kPresetMagic = 0x4252564D; // "MVRB"
static constexpr uint8_t kPresetVersion = 1;
static constexpr uint32_t kPresetOffset = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
static constexpr int kPresetBytes = (sizeof(PresetStore) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE;
static_assert(sizeof(PresetStore) <= FLASH_SECTOR_SIZE, "presets fit a sector");


class MatrixVerb : public ComputerCard
{
public:
	static constexpr int kLines = mv::kLines;
	static constexpr int kPages = mv::kPages;

	MatrixVerb()
	{
		for (int i = 0; i <= 256; i++)
		{
			exp2Tab[i] = uint32_t(1073741824.0f * exp2f(float(i) / 256.0f));
		}

		SetDefaults();
		LoadPresetStore();
		if (store.lastSlot < sysex::kSlots && store.valid[store.lastSlot])
		{
			ApplySlot(store.lastSlot);
		}

		// The first design, worked out here (before core1 starts using the
		// designer) so the audio starts with one
		eng.Init();
		static mv::Controls c;
		GetControls(c);
		designer1.Compute(c, design);
		eng.SetDesign(design);

		EnableNormalisationProbe();

		// SysTick counting CPU cycles, for the load meter
		systick_hw->rvr = 0x00FFFFFF;
		systick_hw->cvr = 0;
		systick_hw->csr = 0x5;

		// Give the USB power circuitry time to settle, then pick the USB
		// mode once: host if the port is supplying power (an 8mu, or nothing
		// yet), device if a computer is (the web editor).  Boards older than
		// Rev 1.1 can't tell, and are always a device.
		sleep_us(150000);
		gCard = this;
		hostMode = USBPowerState() == DFP;
		multicore_launch_core1(hostMode ? Core1Host : Core1Device);
	}

	// Default settings, so the card does something with no 8mu.  Values are
	// in 8mu fader units (0-127), stored shifted up to 0-4064.
	void SetDefaults()
	{
		uint8_t v[mv::kNumValues];
		mv::Defaults(v);
		for (int i = 0; i < mv::kNumValues; i++) params[i] = int32_t(v[i]) << 5;
	}

	virtual void ProcessSample()
	{
		uint32_t t0 = systick_hw->cvr;
		int32_t inL = int32_t(AudioIn1()) << 4;
		// Mono in: with only Audio In 1 patched, it feeds both sides
		int32_t inR = (Connected(Audio1) && Disconnected(Audio2)) ? inL : int32_t(AudioIn2()) << 4;

		// A click on Pulse In 1 (edges are only seen sample by sample)
		if (PulseIn1RisingEdge()) eng.Impulse(-1);

		int32_t outL, outR;
		eng.Process(inL, inR, outL, outR);
		AudioOut1(int16_t(Clip((outL + 8) >> 4, 2047)));
		AudioOut2(int16_t(Clip((outR + 8) >> 4, 2047)));

		// The card's own control work, on samples the network leaves free
		switch (eng.Slot())
		{
		case 16: ControlUI(); break;
		case 20: Control(); break;
		case 24:
			if (hostMode) HandleEightMU();
			else SetTilt(webPitch, webRoll);
			break;
		case 28: TakeDesign(); break;
		default: break;
		}

		// How long this sample took, in CPU cycles (SysTick counts down)
		uint32_t took = (t0 - systick_hw->cvr) & 0xFFFFFF;
		if (took > loadPeak) loadPeak = took;
	}

private:
	EightMU mu;
	mv::Engine eng;

	// Settings, stored as raw fader values 0-4064, page by page.  Written by
	// core0 from the 8mu, or by core1 from the web editor.
	volatile int32_t params[mv::kNumValues];

	// Knobs, CV and tilt, for the designer on core1
	volatile int32_t decayOct = 0, sizeOct = 0, dampOffset = 0;

	// The designer's latest design, handed from core1 to core0: core1 fills
	// it only while designReady is false, core0 takes it only while true
	mv::Design design;
	volatile bool designReady = false;

	// 8mu paging and fader pickup
	volatile int page = mv::PageRow1;
	int lastRow = 0;               // the matrix row A comes back to
	int pageBlink = 0;
	bool latched[kLines] = {};
	int32_t lastFader[kLines] = {};
	bool lastFaderValid = false;
	bool prevButton[EightMU::numButtons] = {};
	int pressTicks[EightMU::numButtons] = {};
	bool pressUsed[EightMU::numButtons] = {};
	int32_t pressFaders[EightMU::numButtons][kLines] = {};
	static constexpr int kLongPress = 750;   // UI ticks: 0.5s
	// How far a fader must move while a button is held to count, about a
	// sixth of its travel, so a held button and a nudged fader do nothing
	static constexpr int32_t kHoldMove = 640;
	enum Flash {FlashNone, FlashLoad, FlashSave, FlashTemplate, FlashImpulse};
	int ledFlash = 0;          // UI ticks left of an 8mu LED flash
	Flash ledFlashKind = FlashNone;
	int ledFlashValue = 0;
	bool wasConnected = false;
	int connectHoldoff = 0;
	bool muFreeze = false;
	int32_t tiltDecay = 0, tiltDamp = 0;

	// Load meter: the longest sample, in cycles, since the editor last read
	// it, and the time a sample has (200MHz / 48kHz)
	uint32_t loadPeak = 0;
	static constexpr uint32_t kSampleCycles = 200000000 / 48000;
	uint32_t loadHold = 0;
	int loadTicks = 0;

	// Presets
	PresetStore store;
	volatile bool flashDirty = false;
	int presetFlash = 0;

	uint32_t exp2Tab[257];        // 2^(i/256) in Q30

	// USB mode, fixed at power-up
	bool hostMode = true;

	// Device mode: written on core1, read on core0
	volatile bool webFreeze = false;
	volatile bool webClear = false;
	volatile int webImpulse = -2;
	volatile int32_t webPitch = 0, webRoll = 0;
	volatile uint32_t lastPingUs = 0;
	volatile bool pinged = false;

	// Snapshot for the web editor: written on core0, read on core1
	volatile uint8_t stFlags = 0, stLoad = 0, stDamp = 64, stGuard = 127, stLimit = 127;
	volatile uint8_t stIn = 0, stOut = 0, stWet = 0, stTank = 0;
	volatile int32_t stLen[kLines] = {};
	volatile int32_t stDecay = 0, stSigma = 0, stScale = 0, stDecayOct = 8192, stSizeOct = 8192;

	static inline int32_t Clip(int32_t v, int32_t m)
	{
		return v > m ? m : (v < -m - 1 ? -m - 1 : v);
	}

	//------------------------------------------------------------------------
	// Pages
	//------------------------------------------------------------------------

	static bool IsRow(int pg) {return pg >= mv::PageRow1 && pg < mv::PageRow1 + 8;}
	// The button (0-3) a page is on, and its place in that button's list
	static int ButtonOf(int pg, int *layer = nullptr)
	{
		int b, l;
		if (IsRow(pg)) {b = 0; l = pg - mv::PageRow1;}
		else if (pg == mv::PageLength) {b = 1; l = 0;}
		else if (pg == mv::PageReverb || pg == mv::PageLab) {b = 2; l = pg - mv::PageReverb;}
		else {b = 3; l = pg - mv::PageInput;}
		if (layer) *layer = l;
		return b;
	}
	// A short press of button b: its next page if one of its pages is
	// showing, otherwise its first (for A, the row last shown)
	int NextPage(int pg, int b) const
	{
		switch (b)
		{
		case 0: return IsRow(pg) ? (pg + 1 - mv::PageRow1) % 8 + mv::PageRow1 : mv::PageRow1 + lastRow;
		case 1: return mv::PageLength;
		case 2: return pg == mv::PageReverb ? mv::PageLab : mv::PageReverb;
		default: return pg == mv::PageInput ? mv::PageOutput : (pg == mv::PageOutput ? mv::PagePan : mv::PageInput);
		}
	}
	void SetPage(int pg)
	{
		page = pg;
		if (IsRow(pg)) lastRow = pg - mv::PageRow1;
	}
	static bool TwoSided(int pg)
	{
		return IsRow(pg) || pg == mv::PageInput || pg == mv::PageOutput || pg == mv::PagePan;
	}

	//------------------------------------------------------------------------
	// Control
	//------------------------------------------------------------------------

	// base * 2^(oct/4096)
	uint32_t ExpScale(uint32_t base, int32_t oct) const
	{
		int32_t whole = oct >> 12;
		int32_t frac = oct & 4095;
		int i = frac >> 4, r = frac & 15;
		uint32_t m = exp2Tab[i] + (((exp2Tab[i + 1] - exp2Tab[i]) * uint32_t(r)) >> 4);
		uint64_t v = (uint64_t(base) * m) >> 30;
		if (whole >= 0)
		{
			if (whole > 31) return 0xFFFFFFFF;
			v <<= whole;
		}
		else
		{
			if (whole < -31) return 0;
			v >>= -whole;
		}
		return v > 0xFFFFFFFFull ? 0xFFFFFFFF : uint32_t(v);
	}

	// Knobs, CV, switch and the freeze and clear requests
	void Control()
	{
		Switch sw = SwitchVal();

		// Main and CV In 1: decay; X and CV In 2: size; both +-2 octaves
		// from the knob and 1V/oct from the CV.  Y: damping offset, +-half
		// the fader's range.
		int32_t d = (MapKnob(KnobVal(Main), true) - 2048) * 4 + CVIn1() * 12 + tiltDecay;
		int32_t s = (MapKnob(KnobVal(X), true) - 2048) * 4 + CVIn2() * 12;
		int32_t dm = -(MapKnob(KnobVal(Y), true) - 2048) + tiltDamp;
		decayOct = d < -16384 ? -16384 : (d > 16384 ? 16384 : d);
		sizeOct = s < -16384 ? -16384 : (s > 16384 ? 16384 : s);
		dampOffset = dm < -4096 ? -4096 : (dm > 4096 ? 4096 : dm);

		// Freeze: switch up, Pulse In 2 high, the web editor or the 8mu
		eng.SetFreeze(sw == Up || (Connected(Pulse2) && PulseIn2()) || webFreeze || muFreeze);
		// Clear: switch held down, or the web editor
		eng.SetClearHeld(sw == Down);
		if (webClear)
		{
			webClear = false;
			eng.Clear();
		}
		int imp = webImpulse;
		if (imp > -2)
		{
			webImpulse = -2;
			eng.Impulse(imp);
		}
	}

	// A new design from core1, if there is one
	void TakeDesign()
	{
		if (!designReady) return;
		__dmb();
		eng.SetDesign(design);
		stDecay = design.decayCs < 0 ? 16383 : design.decayCs;
		stSigma = design.sigmaMilli;
		stScale = design.scaleMilli;
		__dmb();
		designReady = false;
	}

	void GetControls(mv::Controls &c) const
	{
		for (int i = 0; i < mv::kNumValues; i++) c.v[i] = uint8_t((params[i] >> 5) & 127);
		c.decayOct = decayOct;
		c.sizeOct = sizeOct;
		c.dampOffset = dampOffset;
	}

	// Panel LEDs, CV and pulse outputs, and the web editor's snapshot
	void ControlUI()
	{
		bool safety = eng.Guarding() || eng.limGain < 29205;   // more than 1dB
		CVOut1Millivolts((eng.lvWet * 5000) >> 15);
		int32_t gr = eng.guard < eng.limGain ? eng.guard : eng.limGain;
		CVOut2Millivolts(((32768 - gr) * 5000) >> 15);
		PulseOut1(safety);
		PulseOut2(eng.Frozen());

		// Snapshot for the web editor
		stFlags = uint8_t((eng.Frozen() ? 1 : 0) | (eng.Clearing() ? 2 : 0) | (eng.Guarding() ? 4 : 0)
			| (eng.limGain < 29205 ? 8 : 0) | (mu.Connected() ? 16 : 0) | (SwitchVal() == Down ? 32 : 0));
		for (int i = 0; i < kLines; i++) stLen[i] = eng.LineDelay(i);
		stDecayOct = (decayOct >> 4) + 8192;
		stSizeOct = (sizeOct >> 4) + 8192;
		int32_t dm = 64 + (dampOffset >> 6);
		stDamp = uint8_t(dm < 0 ? 0 : (dm > 127 ? 127 : dm));
		stGuard = uint8_t(eng.guard >= 32768 ? 127 : eng.guard >> 8);
		stLimit = uint8_t(eng.limGain >= 32768 ? 127 : eng.limGain >> 8);
		stIn = Level7(eng.lvIn);
		stOut = Level7(eng.lvOut);
		stWet = Level7(eng.lvWet);
		stTank = Level7(eng.lvTank);
		// Peak load over about a third of a second, as a percentage of a
		// sample's time
		if (loadPeak > loadHold) loadHold = loadPeak;
		loadPeak = 0;
		if (++loadTicks >= 500)
		{
			uint32_t pc = (loadHold * 100) / kSampleCycles;
			stLoad = uint8_t(pc > 127 ? 127 : pc);
			loadHold = 0;
			loadTicks = 0;
		}

		// Computer LEDs: the page (or levels, with no 8mu or editor), the
		// input, and the connection
		bool conn = mu.Connected() || WebLinked();
		pageBlink = (pageBlink + 1) % 900;
		if (presetFlash > 0) presetFlash--;
		int layer;
		int b = ButtonOf(page, &layer);
		for (int i = 0; i < 4; i++)
		{
			if (conn)
			{
				bool lit;
				if (b == 0)
				{
					// The row's number in flashes: n flashes of 0.24s, then
					// a pause of 0.6s
					int n = layer + 1;
					int t = rowBlink % (n * 360 + 900);
					lit = t < n * 360 && (t % 360) < 180;
				}
				else lit = layer == 0 || (layer == 1 ? pageBlink < 450 : pageBlink % 300 < 150);
				LedOn(i, b == i && lit);
			}
			else if (i == 0) LedBrightness(0, uint16_t(eng.lvWet >> 3));
			else if (i == 1) LedBrightness(1, uint16_t(eng.lvTank >> 3));
			else if (i == 2) LedOn(2, eng.Frozen());
			else LedOn(3, safety);
		}
		rowBlink++;
		LedBrightness(4, eng.Frozen() ? 4095 : uint16_t(eng.lvIn >> 3));
		static int flicker = 0;
		flicker = (flicker + 1) % 100;
		if (presetFlash > 0) LedOn(5, (presetFlash / 100) & 1);
		else if (safety) LedOn(5, flicker < 50);
		else LedOn(5, conn);
	}
	int rowBlink = 0;

	static uint8_t Level7(int32_t v) {return uint8_t(v >= 32767 ? 127 : v >> 8);}

	// A knob reading (0-4095) with dead zones, as the knobs don't always
	// reach the very ends of their range: the outer kKnobEndZone at each end
	// reads as fully 0 or 4095, and the travel between is stretched to fill
	// the range.  Two-sided controls also get kKnobCentreZone either side of
	// the middle, which reads as exactly 2048 (x1).
	static constexpr int32_t kKnobEndZone = 128;
	static constexpr int32_t kKnobCentreZone = 64;
	static int32_t MapKnob(int32_t raw, bool twoSided)
	{
		int32_t v = ((raw - kKnobEndZone) * 4095) / (4095 - 2 * kKnobEndZone);
		if (v < 0) v = 0;
		if (v > 4095) v = 4095;
		if (!twoSided) return v;
		int32_t d = v - 2048;
		if (d > -kKnobCentreZone && d < kKnobCentreZone) return 2048;
		if (d > 0) return 2048 + ((d - kKnobCentreZone) * 2047) / (2047 - kKnobCentreZone);
		return 2048 + ((d + kKnobCentreZone) * 2048) / (2048 - kKnobCentreZone);
	}

	// 8mu tilt (-2032 to 2032 for -90 to +90 degrees) to -4096 to 4096,
	// ignoring about 15 degrees either side of flat so that holding the 8mu
	// doesn't change anything; beyond that the tilt is stretched so tipping
	// it right up reaches the end
	static constexpr int32_t kTiltDeadZone = 512;
	static int32_t TiltQ12(int32_t t)
	{
		if (t > -kTiltDeadZone && t < kTiltDeadZone) return 0;
		int32_t d = t > 0 ? t - kTiltDeadZone : t + kTiltDeadZone;
		int32_t m = (d * 4096) / (2032 - kTiltDeadZone);
		return m > 4096 ? 4096 : (m < -4096 ? -4096 : m);
	}

	// Front/back: decay, x1/4 (front lifted) to x4 (back lifted); left/right:
	// damping, darker to the left (Roll is positive with the right lifted)
	void SetTilt(int32_t pitch, int32_t roll)
	{
		tiltDecay = TiltQ12(pitch) * 2;
		tiltDamp = TiltQ12(roll) >> 1;
	}

	void FlashLed(Flash kind, int value)
	{
		ledFlash = 1350;
		ledFlashKind = kind;
		ledFlashValue = value;
	}

	void HandleEightMU()
	{
		bool conn = mu.Connected();
		if (!conn)
		{
			wasConnected = false;
			SetTilt(0, 0);
			return;
		}
		if (!wasConnected)
		{
			// Wait for the 8mu's reply to the fader position query before
			// trusting fader values
			wasConnected = true;
			connectHoldoff = 1500; // ~1s at control rate
			lastFaderValid = false;
			for (int i = 0; i < kLines; i++) latched[i] = false;
		}
		if (connectHoldoff > 0)
		{
			connectHoldoff--;
			return;
		}

		// Buttons, acted on when released: a short press is the page, a long
		// press of A the previous row, of C clear and of D freeze; moving a
		// fader while a button is held loads a preset (A), a template (B),
		// clicks into a line (C) or saves a preset (D).  While a button is
		// held the faders edit nothing.
		bool anyHeld = false;
		for (int b = 0; b < EightMU::numButtons; b++)
		{
			bool down = mu.Button(b);
			if (down && !prevButton[b])
			{
				pressTicks[b] = 0;
				pressUsed[b] = false;
				for (int i = 0; i < kLines; i++) pressFaders[b][i] = mu.Fader(i);
			}
			if (down)
			{
				anyHeld = true;
				if (pressTicks[b] < 0x7FFFFFFF) pressTicks[b]++;
				for (int i = 0; i < kLines; i++)
				{
					int32_t d = mu.Fader(i) - pressFaders[b][i];
					if (d > kHoldMove || d < -kHoldMove)
					{
						pressFaders[b][i] = mu.Fader(i);
						pressUsed[b] = true;
						switch (b)
						{
						case 0: LoadSlot(i); FlashLed(FlashLoad, i); break;
						case 1: ApplyTemplate(i); FlashLed(FlashTemplate, i); break;
						case 2: eng.Impulse(i); FlashLed(FlashImpulse, i); break;
						default: SaveSlot(i); FlashLed(FlashSave, i); break;
						}
					}
				}
			}
			else if (prevButton[b])
			{
				if (!pressUsed[b])
				{
					bool lng = pressTicks[b] >= kLongPress;
					if (lng && b == 0) SetPage(IsRow(page) ? (page - mv::PageRow1 + 7) % 8 + mv::PageRow1 : mv::PageRow1 + lastRow);
					else if (lng && b == 2) eng.Clear();
					else if (lng && b == 3) muFreeze = !muFreeze;
					else SetPage(NextPage(page, b));
				}
				for (int i = 0; i < kLines; i++) latched[i] = false;
			}
			prevButton[b] = down;
		}
		if (ledFlash > 0) ledFlash--;

		// Faders, with pickup
		int pg = page;
		bool two = TwoSided(pg);
		for (int i = 0; i < kLines; i++)
		{
			int32_t f = mu.Fader(i);
			volatile int32_t &p = params[pg * kLines + i];
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

			// 8mu LEDs: a load flashes the preset's fader three times, a save
			// blinks it fast, a template twice and a click once; otherwise
			// the stored values (two-sided: the distance from the centre)
			int32_t led;
			int t = 1350 - ledFlash;
			if (ledFlash > 0 && i != ledFlashValue) led = 0;
			else if (ledFlash > 0 && ledFlashKind == FlashLoad) led = (t / 225) % 2 == 0 ? 4095 : 0;
			else if (ledFlash > 0 && ledFlashKind == FlashSave) led = (t / 75) % 2 == 0 ? 4095 : 0;
			else if (ledFlash > 0 && ledFlashKind == FlashTemplate) led = (t < 900 && (t / 225) % 2 == 0) ? 4095 : 0;
			else if (ledFlash > 0) led = t < 300 ? 4095 : 0;
			else if (two)
			{
				int32_t c = (p >> 5) - 64;
				led = (c < 0 ? -c : c) * 64;
			}
			else led = (p * 9) >> 4;
			mu.SetLed(i, led > 4095 ? 4095 : led);
		}
		lastFaderValid = true;

		SetTilt(mu.Pitch(), mu.Roll());
	}

	void ApplyTemplate(int n)
	{
		if (n < 0 || n >= mv::kNumTemplates) return;
		uint8_t m[64];
		mv::Template(n, m);
		for (int i = 0; i < 64; i++) params[i] = int32_t(m[i]) << 5;
	}

	//------------------------------------------------------------------------
	// Presets
	//------------------------------------------------------------------------

	void LoadPresetStore()
	{
		memcpy(&store, reinterpret_cast<const void *>(XIP_BASE + kPresetOffset), sizeof(store));
		if (store.magic != kPresetMagic || store.version != kPresetVersion)
		{
			memset(&store, 0, sizeof(store));
			store.magic = kPresetMagic;
			store.version = kPresetVersion;
			store.lastSlot = sysex::kNoSlot;
		}
		for (int s = 0; s < sysex::kSlots; s++) if (store.valid[s] != 1) store.valid[s] = 0;
	}

	void ApplySlot(int s)
	{
		for (int i = 0; i < sysex::kNumValues; i++) params[i] = int32_t(store.values[s][i] & 0x7F) << 5;
	}

	// From the 8mu or the web editor; the flash is written on core1.  A
	// preset can sound nothing like the last, so the network is emptied and
	// the new one fades in.
	void LoadSlot(int s)
	{
		if (s < 0 || s >= sysex::kSlots || !store.valid[s]) return;
		ApplySlot(s);
		eng.Clear();
		if (store.lastSlot != s)
		{
			store.lastSlot = uint8_t(s);
			flashDirty = true;
		}
		presetFlash = 600;
	}

	void SaveSlot(int s)
	{
		if (s < 0 || s >= sysex::kSlots) return;
		for (int i = 0; i < sysex::kNumValues; i++) store.values[s][i] = uint8_t((params[i] >> 5) & 0x7F);
		store.valid[s] = 1;
		store.lastSlot = uint8_t(s);
		flashDirty = true;
		presetFlash = 600;
	}

	// On core1, which runs from RAM like everything else (copy_to_ram), so
	// the audio on core0 carries on while the flash is busy
	void ServiceFlash()
	{
		if (!flashDirty) return;
		flashDirty = false;
		static uint8_t buf[kPresetBytes] __attribute__((aligned(4)));
		memset(buf, 0xFF, sizeof(buf));
		memcpy(buf, &store, sizeof(store));
		uint32_t ints = save_and_disable_interrupts();
		flash_range_erase(kPresetOffset, FLASH_SECTOR_SIZE);
		flash_range_program(kPresetOffset, buf, kPresetBytes);
		restore_interrupts(ints);
	}

	//------------------------------------------------------------------------
	// The designer, on core1
	//------------------------------------------------------------------------

	// Every 2ms, if core0 has taken the last design, work out the next.  The
	// matrix's normalisation (a few ms of floating point) is only redone
	// when the matrix changes.
	void ServiceDesigner()
	{
		uint32_t now = time_us_32();
		if (designReady || now - lastDesignUs < 2000) return;
		lastDesignUs = now;
		static mv::Controls c;
		GetControls(c);
		designer1.Compute(c, design);
		__dmb();
		designReady = true;
	}
	mv::Designer designer1;
	uint32_t lastDesignUs = 0;

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
			gCard->ServiceDesigner();
			gCard->ServiceFlash();
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
			gCard->ServiceDesigner();
			gCard->ServiceFlash();
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
			if (len >= 3 && p[0] < kPages && p[1] < kLines)
			{
				params[p[0] * kLines + p[1]] = int32_t(p[2] & 0x7F) << 5;
			}
			break;
		case sysex::SetAll:
			if (len >= 1 + sysex::kNumValues && p[0] == sysex::kVersion)
			{
				for (int i = 0; i < sysex::kNumValues; i++) params[i] = int32_t(p[1 + i] & 0x7F) << 5;
			}
			break;
		case sysex::Page:
			if (len >= 1 && p[0] < kPages) SetPage(p[0]);
			break;
		case sysex::Reset:
			SetDefaults();
			webClear = true;
			SendState();
			break;
		case sysex::Motion:
			if (len >= 4)
			{
				webPitch = sysex::Get14(p) - 2048;
				webRoll = sysex::Get14(p + 2) - 2048;
			}
			break;
		case sysex::Ping:
			lastPingUs = time_us_32();
			pinged = true;
			break;
		case sysex::Freeze:
			if (len >= 1) webFreeze = p[0] != 0;
			break;
		case sysex::Clear:
			webClear = true;
			break;
		case sysex::Impulse:
			if (len >= 1 && p[0] <= 8) webImpulse = p[0] == 8 ? -1 : p[0];
			break;
		case sysex::Save:
			if (len >= 1 && p[0] < sysex::kSlots)
			{
				SaveSlot(p[0]);
				SendSlots();
			}
			break;
		case sysex::Load:
			if (len >= 1 && p[0] < sysex::kSlots)
			{
				if (store.valid[p[0]])
				{
					ApplySlot(p[0]);
					webClear = true;
					store.lastSlot = p[0];
					flashDirty = true;
					presetFlash = 600;
				}
				SendState();
			}
			break;
		case sysex::GetSlots:
			SendSlots();
			break;
		case sysex::Store:
			if (len >= 1 + sysex::kNumValues && p[0] < sysex::kSlots)
			{
				for (int i = 0; i < sysex::kNumValues; i++) store.values[p[0]][i] = p[1 + i] & 0x7F;
				store.valid[p[0]] = 1;
				flashDirty = true;
				SendSlots();
			}
			break;
		case sysex::Erase:
			if (len >= 1 && p[0] < sysex::kSlots)
			{
				store.valid[p[0]] = 0;
				if (store.lastSlot == p[0]) store.lastSlot = sysex::kNoSlot;
				flashDirty = true;
				SendSlots();
			}
			break;
		case sysex::Template:
			if (len >= 1 && p[0] < mv::kNumTemplates)
			{
				ApplyTemplate(p[0]);
				SendState();
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
		out[n++] = uint8_t(page);
		out[n++] = store.lastSlot < sysex::kSlots ? store.lastSlot : sysex::kNoSlot;
		for (int i = 0; i < sysex::kNumValues; i++) out[n++] = uint8_t((params[i] >> 5) & 0x7F);
		out[n++] = 0xF7;
		return n;
	}

	int EncodeStatus(uint8_t *out) const
	{
		int n = sysex::Header(out, sysex::Status);
		out[n++] = stFlags;
		out[n++] = uint8_t(page);
		out[n++] = store.lastSlot < sysex::kSlots ? store.lastSlot : sysex::kNoSlot;
		out[n++] = stLoad;
		for (int i = 0; i < kLines; i++) n += sysex::Put14(out + n, stLen[i]);
		n += sysex::Put14(out + n, stDecay);
		n += sysex::Put14(out + n, stSigma);
		n += sysex::Put14(out + n, stScale);
		n += sysex::Put14(out + n, stDecayOct);
		n += sysex::Put14(out + n, stSizeOct);
		out[n++] = stDamp;
		out[n++] = stGuard;
		out[n++] = stLimit;
		out[n++] = stIn;
		out[n++] = stOut;
		out[n++] = stWet;
		out[n++] = stTank;
		out[n++] = 0xF7;
		return n;
	}

	int EncodeSlots(uint8_t *out) const
	{
		int n = sysex::Header(out, sysex::Slots);
		for (int s = 0; s < sysex::kSlots; s++) out[n++] = store.valid[s] ? 1 : 0;
		for (int s = 0; s < sysex::kSlots; s++)
			for (int i = 0; i < sysex::kNumValues; i++)
				out[n++] = store.values[s][i] & 0x7F;
		out[n++] = 0xF7;
		return n;
	}

private:
	void SendState()
	{
		uint8_t msg[sysex::kStateLen];
		Write(msg, EncodeState(msg));
	}

	void SendSlots()
	{
		static uint8_t msg[sysex::kSlotsLen];
		Write(msg, EncodeSlots(msg));
	}
};


int main()
{
	set_sys_clock_khz(200000, true);

	static MatrixVerb card;
	card.Run();
}
