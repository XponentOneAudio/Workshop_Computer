// what.three.wiggles: a Workshop Computer program card.
//
// Three words (a what3words address, or any three words) become three seeds,
// and the seeds set up every oscillator, LFO, random walk and gate pattern.
// The same words always give the same patch. See README.md and TASKS.md.
//
// Core 0 runs the audio callback (ProcessSample, 48 kHz).
// Core 1 runs USB MIDI: the webapp sends words over SysEx, core 1 turns them
// into a Patch, hands it to core 0 and saves the words to flash.
//
// The USB/SysEx plumbing follows Chris Johnson's ComputerCard web_interface
// example (MIT licence).

#include "ComputerCard.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "hardware/structs/systick.h"
#include "tusb.h"
#include <cstring>

#include "seed.h"
#include "dsp.h"
#include "patch.h"
#include "engines.h"
#include "protocol.h"

using namespace w3w;

constexpr uint8_t kFirmwareVersion[3] = {0, 1, 0};

// SysEx: F0 7D <card id> <message> <payload...> F7. See protocol.h.
constexpr uint8_t kManufacturerId = 0x7D; // prototyping, test, private use

constexpr char kDefaultWords[] = "what.three.wiggles";

// The words are saved in the last sector of flash
constexpr uint32_t kFlashOffset = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
constexpr uint32_t kFlashMagic = 0x57335731; // "W3W1"

struct FlashRecord
{
	uint32_t magic;
	uint32_t length;
	uint8_t text[kMaxWordsLen + 1];
	uint32_t check;
};
static_assert(sizeof(FlashRecord) <= FLASH_PAGE_SIZE, "record must fit in one flash page");

inline uint32_t RecordCheck(const uint8_t *text, uint32_t length)
{
	return HashWord(text, length) ^ kFlashMagic;
}

// Knob pages: short press on switch Z (down) flips between them
enum Page { PageA = 0, PageB = 1 };
// Page A: Main = FM depth,  X = quantiser scale,  Y = clock rate
// Page B: Main = CV 1 range, X = trigger density, Y = LFO rate

// Soft takeover: after a page change, a knob does nothing until it reaches
// (or passes) the value it had on that page.
struct SoftKnob
{
	static constexpr int32_t kWindow = 40;
	int32_t value = 2048;
	bool caught = true;
	bool above = false;

	void Enter(int32_t phys)
	{
		int32_t d = phys - value;
		caught = d > -kWindow && d < kWindow;
		above = d > 0;
	}

	void Update(int32_t phys)
	{
		if (!caught)
		{
			int32_t d = phys - value;
			if ((d > -kWindow && d < kWindow) || (d > 0) != above) caught = true;
		}
		if (caught) value = phys;
	}
};

class Wiggles : public ComputerCard
{
public:
	Wiggles()
	{
		gTables.Init();
		LoadWords();
		uint32_t seeds[3];
		SeedsFromWords(words, wordsLen, seeds);
		BuildPatch(seeds, active);
	}

	void StartUSBCore()
	{
		instance = this;
		multicore_launch_core1(Core1Entry);
	}

protected:
	// Times each sample with the SysTick counter, so the webapp can show
	// how much of the per-sample CPU budget the engines use.
	void ProcessSample() override
	{
		if (!systickRunning)
		{
			systick_hw->rvr = 0xFFFFFF;
			systick_hw->cvr = 0;
			systick_hw->csr = 5; // enable, count processor clock cycles
			systickRunning = true;
		}
		uint32_t start = systick_hw->cvr;
		Process();
		uint32_t cycles = (start - systick_hw->cvr) & 0xFFFFFF; // counts down
		if (cycles > peakCycles) peakCycles = cycles;
	}

private:
	void Process()
	{
		////////////////////////////////////////
		// Patch swaps and mode changes fade out, swap, then fade back in

		if (patchPending)
		{
			patchGain -= 16;
			if (patchGain <= 0)
			{
				patchGain = 0;
				__dmb();
				active = pending;
				__dmb();
				patchPending = false;
				ResetAudioEngines();
				ResetControlEngines();
				celebrate = 36000;
			}
		}
		else if (patchGain < 4096)
		{
			patchGain += 16;
		}

		if (ringModActive != ringModWanted)
		{
			modeGain -= 16;
			if (modeGain <= 0)
			{
				modeGain = 0;
				ringModActive = ringModWanted;
			}
		}
		else if (modeGain < 4096)
		{
			modeGain += 16;
		}

		////////////////////////////////////////
		// Knobs, switch and CV-rate things, every 16 samples

		if ((++sampleCount & 15) == 0) UpdateControls();
		if (needsReset) return;

		////////////////////////////////////////
		// Reset (Pulse In 2, or holding the switch down), then the clock

		if (PulseIn2RisingEdge() || resetRequest)
		{
			resetRequest = false;
			ResetControlEngines();
		}

		bool tick = clock.Process(Connected(Input::Pulse1), PulseIn1RisingEdge(), clockInc);
		if (tick)
		{
			const Scale &s = kScales[scaleIndex];
			triggers.Tick(TriggerDensity(active.triggerDensity, knobs[PageB][X].value), clock.period);
			walk.Tick(active, walkMaxDegree, s, clock.period);
			tickLed = 2400;
		}

		////////////////////////////////////////
		// Audio 1: FM loop drone. Depth = Main knob (page A) + Audio In 1

		int32_t depthTarget = Clamp(knobs[PageA][Main].value + ((AudioIn1() * 5) >> 1), 0, 4095);
		depthQ8 += ((depthTarget << 8) - depthQ8) >> 7;
		int32_t fm = fmLoop.Process(active, depthQ8 >> 8, ringModActive);
		int32_t gain1 = (patchGain * modeGain) >> 12;
		AudioOut1(int16_t((((fm * gain1) >> 12) * kAudioLevel) >> 15));

		////////////////////////////////////////
		// Audio 2: additive osc. V/oct on CV In 1, sampled and held on
		// CV In 2 edges; tracks continuously if CV In 2 is unplugged.

		int32_t cv2 = CVIn2();
		bool cv2High = cv2Gate ? (cv2 > kGateLow) : (cv2 > kGateHigh);
		bool sampleEdge = cv2High && !cv2Gate;
		cv2Gate = cv2High;
		if (sampleEdge && Connected(Input::CV2)) SamplePitch();

		int32_t add = additive.Process(active, pitchInc);
		AudioOut2(int16_t((((add * patchGain) >> 12) * kAudioLevel) >> 15));

		////////////////////////////////////////
		// CV 1: random walk (calibrated V/oct).  CV 2: complex LFO, about +/-5 V

		walkMv = walk.Process();
		CVOut1Millivolts(walkMv);
		lfoOut = lfo.Process(active, lfoInc);
		CVOut2Precise((lfoOut * 27307) >> 12);

		////////////////////////////////////////
		// Pulse 1: random triggers.  Pulse 2: flip-flop toggled by them

		bool trig = triggers.Process();
		PulseOut1(trig);
		PulseOut2(triggers.flipFlop);
		if (trig) trigLed = 2400;
		if (trigLed > 0) trigLed--;
		if (tickLed > 0) tickLed--;
		if (celebrate > 0) celebrate--;
	}

	static constexpr int32_t kAudioLevel = 1800; // Q15 full scale -> about +/-5 V
	static constexpr int32_t kGateHigh = 400;    // CV In 2 trigger threshold, about 1.2 V
	static constexpr int32_t kGateLow = 200;

	////////////////////////////////////////
	// Controls (core 0, every 16 samples)

	void UpdateControls()
	{
		int32_t phys[3] = {KnobVal(Main), KnobVal(X), KnobVal(Y)};

		if (needsReset)
		{
			// First pass: page A starts wherever the knobs are
			for (int k = 0; k < 3; k++) knobs[PageA][k].value = phys[k];
			depthQ8 = phys[Main] << 8;
		}

		for (int k = 0; k < 3; k++) knobs[page][k].Update(phys[k]);

		// Switch: middle = FM loop, up = ring mod. Down is momentary:
		// a short press flips the knob page, holding it for a second resets.
		Switch sw = SwitchVal();
		if (sw == Down)
		{
			if (!downHeld)
			{
				downHeld = true;
				downTime = 0;
			}
			else if (downTime < 48000)
			{
				downTime += 16;
				if (downTime >= 48000) resetRequest = true;
			}
		}
		else
		{
			if (downHeld && downTime < 24000)
			{
				page = (page == PageA) ? PageB : PageA;
				for (int k = 0; k < 3; k++) knobs[page][k].Enter(phys[k]);
			}
			downHeld = false;
			ringModWanted = (sw == Up);
		}

		// Quantiser scale, with a little hysteresis between zones
		int32_t scaleKnob = knobs[PageA][X].value;
		int32_t zoneWidth = 4096 / kNumScales;
		int32_t centre = scaleIndex * zoneWidth + zoneWidth / 2;
		int32_t dist = scaleKnob - centre;
		if (dist < 0) dist = -dist;
		if (dist > zoneWidth / 2 + 24) scaleIndex = Clamp(scaleKnob / zoneWidth, 0, kNumScales - 1);
		const Scale &s = kScales[scaleIndex];

		// Rates: clock 0.25-20 Hz, LFO 0.02-20 Hz (both exponential)
		clockInc = IncFromMillivolts(KnobMap(knobs[PageA][Y].value, -10030, -3700));
		lfoInc = IncFromMillivolts(KnobMap(knobs[PageB][Y].value, -13700, -3700));

		// Walk range: 1 to 48 semitones above C4
		walkSpan = KnobMap(knobs[PageB][Main].value, 1, 48);
		walkMaxDegree = WalkMaxDegree(walkSpan, s);

		if (!Connected(Input::CV2)) SamplePitch();

		if (needsReset)
		{
			needsReset = false;
			ResetAudioEngines();
			ResetControlEngines();
			SamplePitch();
		}

		UpdateLeds();
	}

	void SamplePitch()
	{
		int32_t semis = quantiser.Process(CVInMillivolts(0), kScales[scaleIndex]);
		pitchInc = IncFromMillivolts(SemitonesToMillivolts(semis));
	}

	void ResetAudioEngines()
	{
		fmLoop.Reset(active);
		additive.phase = 0;
	}

	void ResetControlEngines()
	{
		clock.Reset();
		triggers.Reset(active);
		walk.Reset(active, walkMaxDegree, kScales[scaleIndex]);
		lfo.Reset(active);
	}

	// LED 0: Pulse 1   LED 1: Pulse 2
	// LED 2: CV 1      LED 3: CV 2
	// LED 4: lit on knob page B
	// LED 5: clock tick, or blinking while a knob waits for soft takeover
	// All six chase round when new words arrive.
	void UpdateLeds()
	{
		if (celebrate > 0)
		{
			int lit = (celebrate >> 11) % 6;
			for (int i = 0; i < 6; i++) LedOn(i, i == lit);
			return;
		}
		LedOn(0, trigLed > 0);
		LedOn(1, triggers.flipFlop);
		int32_t spanMv = SemitonesToMillivolts(walkSpan);
		LedBrightness(2, uint16_t(Clamp(walkMv * 4095 / (spanMv > 0 ? spanMv : 1), 0, 4095)));
		LedBrightness(3, uint16_t((lfoOut < 0 ? -lfoOut : lfoOut) >> 3));
		LedOn(4, page == PageB);
		bool waiting = !(knobs[page][0].caught && knobs[page][1].caught && knobs[page][2].caught);
		LedOn(5, waiting ? ((sampleCount >> 12) & 1) : (tickLed > 0));
	}

	////////////////////////////////////////
	// Words and flash (constructor, then core 1 only)

	void LoadWords()
	{
		const FlashRecord *rec = reinterpret_cast<const FlashRecord *>(XIP_BASE + kFlashOffset);
		uint32_t seeds[3];
		if (rec->magic == kFlashMagic && rec->length <= kMaxWordsLen
			&& rec->check == RecordCheck(rec->text, rec->length)
			&& SeedsFromWords(rec->text, rec->length, seeds))
		{
			memcpy(words, rec->text, rec->length);
			wordsLen = rec->length;
		}
		else
		{
			wordsLen = sizeof(kDefaultWords) - 1;
			memcpy(words, kDefaultWords, wordsLen);
		}
	}

	// The binary runs from RAM (copy_to_ram), so core 0 keeps making sound
	// while flash is erased; only this core's interrupts need to be off.
	void SaveWords()
	{
		static uint8_t buf[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
		memset(buf, 0xFF, sizeof(buf));
		FlashRecord rec;
		memset(&rec, 0, sizeof(rec));
		rec.magic = kFlashMagic;
		rec.length = wordsLen;
		memcpy(rec.text, words, wordsLen);
		rec.check = RecordCheck(rec.text, rec.length);
		memcpy(buf, &rec, sizeof(rec));

		uint32_t ints = save_and_disable_interrupts();
		flash_range_erase(kFlashOffset, FLASH_SECTOR_SIZE);
		flash_range_program(kFlashOffset, buf, FLASH_PAGE_SIZE);
		restore_interrupts(ints);
	}

	////////////////////////////////////////
	// USB MIDI and SysEx (core 1)

	static void Core1Entry() { instance->USBCore(); }

	void USBCore()
	{
		tusb_init();
		while (true)
		{
			tud_task();
			while (tud_midi_available())
			{
				uint8_t rx[64];
				uint32_t n = tud_midi_stream_read(rx, sizeof(rx));
				ParseMIDIBytes(rx, n);
			}
		}
	}

	void ParseMIDIBytes(const uint8_t *rx, uint32_t n)
	{
		for (uint32_t i = 0; i < n; i++)
		{
			uint8_t b = rx[i];
			if (!sysexActive)
			{
				if (b == 0xF0)
				{
					sysexActive = true;
					sysexLen = 0;
				}
				continue;
			}
			if (b == 0xF7)
			{
				if (sysexLen >= 1 && sysexBuf[0] == kManufacturerId)
				{
					ProcessIncomingSysEx(sysexBuf + 1, sysexLen - 1);
				}
				sysexActive = false;
			}
			else if (b & 0x80)
			{
				sysexActive = false; // a status byte interrupted the message
			}
			else if (sysexLen < sizeof(sysexBuf))
			{
				sysexBuf[sysexLen++] = b;
			}
		}
	}

	void ProcessIncomingSysEx(const uint8_t *data, uint32_t size)
	{
		if (size < 2 || data[0] != kCardId) return;

		if (data[1] == kMsgHello)
		{
			SendState();
		}
		else if (data[1] == kMsgGetStats)
		{
			uint8_t msg[14];
			uint32_t peak = peakCycles;
			peakCycles = 0;
			SendSysEx(msg, EncodeStats(peak, clock_get_hz(clk_sys) / 48000, msg));
		}
		else if (data[1] == kMsgSetWords)
		{
			uint8_t text[kMaxWordsLen];
			uint32_t len = 0;
			uint32_t seeds[3];
			if (!DecodeSetWords(data, size, text, len))
			{
				SendError(kErrBadMessage);
				return;
			}
			if (!SeedsFromWords(text, len, seeds))
			{
				SendError(kErrBadWords);
				return;
			}

			bool changed = (len != wordsLen) || memcmp(text, words, len) != 0;
			memcpy(words, text, len);
			wordsLen = len;

			// Wait for core 0 to take any previous patch (a few ms at most)
			while (patchPending) tight_loop_contents();
			BuildPatch(seeds, pending);
			__dmb();
			patchPending = true;

			if (changed) SaveWords();
			SendState();
		}
		else
		{
			SendError(kErrBadMessage);
		}
	}

	void SendState()
	{
		uint8_t msg[kMaxStateLen];
		SendSysEx(msg, EncodeState(words, wordsLen, kFirmwareVersion, msg));
	}

	void SendError(uint8_t code)
	{
		uint8_t msg[] = {kCardId, kMsgError, code};
		SendSysEx(msg, sizeof(msg));
	}

	// A single tud_midi_stream_write can't take a long message all at once
	void MIDIStreamWriteBlocking(const uint8_t *data, uint32_t size)
	{
		uint32_t sent = 0;
		while (sent < size)
		{
			uint32_t n = tud_midi_stream_write(0, data + sent, size - sent);
			sent += n;
			if (!n) tud_task();
		}
	}

	void SendSysEx(const uint8_t *data, uint32_t size)
	{
		const uint8_t header[] = {0xF0, kManufacturerId};
		const uint8_t footer[] = {0xF7};
		MIDIStreamWriteBlocking(header, sizeof(header));
		MIDIStreamWriteBlocking(data, size);
		MIDIStreamWriteBlocking(footer, sizeof(footer));
	}

	////////////////////////////////////////
	// State

	static Wiggles *instance;

	// Patch handover: core 1 fills `pending`, core 0 copies it into `active`
	Patch active;
	Patch pending;
	volatile bool patchPending = false;

	// Words (core 1 after startup)
	uint8_t words[kMaxWordsLen];
	uint32_t wordsLen = 0;
	uint8_t sysexBuf[512];
	uint32_t sysexLen = 0;
	bool sysexActive = false;

	// Engines
	Clock clock;
	Triggers triggers;
	Walk walk;
	ComplexLfo lfo;
	Additive additive;
	FmLoop fmLoop;
	Quantiser quantiser;

	// Controls
	SoftKnob knobs[2][3];
	Page page = PageA;
	bool downHeld = false;
	int32_t downTime = 0;
	bool resetRequest = false;
	bool needsReset = true;
	bool ringModWanted = false;
	bool ringModActive = false;
	int32_t scaleIndex = 0;
	int32_t walkSpan = 24;
	int32_t walkMaxDegree = 24;
	uint32_t clockInc = 0;
	uint32_t lfoInc = 0;
	uint32_t pitchInc = 0;
	int32_t depthQ8 = 0;
	bool cv2Gate = false;

	// Outputs and LEDs
	int32_t patchGain = 0;
	int32_t modeGain = 4096;
	int32_t walkMv = 0;
	int32_t lfoOut = 0;
	int32_t trigLed = 0;
	int32_t tickLed = 0;
	int32_t celebrate = 0;
	uint32_t sampleCount = 0;

	// CPU use
	bool systickRunning = false;
	volatile uint32_t peakCycles = 0;
};

Wiggles *Wiggles::instance = nullptr;

int main()
{
	set_sys_clock_khz(144000, true);

	static Wiggles card;
	card.EnableNormalisationProbe();
	card.StartUSBCore();
	card.Run();
}
